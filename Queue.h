#pragma once

#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <functional>

#include "UEInterface.h"

#define FASTCALL __fastcall // pointles on x64
#define HARDWARE_PAUSE() std::this_thread::yield(); // TODO: other platforms

#define QUEUE_PADDING_BYTES(_TYPE_SIZES_) (PLATFORM_CACHE_LINE_SIZE - (_TYPE_SIZES_) % PLATFORM_CACHE_LINE_SIZE)
#define CACHE_ALIGN alignas(PLATFORM_CACHE_LINE_SIZE)

namespace QueueTypes
{
    /**
     * Simple, efficient spin-lock implementation.
     * A function that takes a void lambda function can be used to
     * conveiniently do something which will be protected by the lock.
     * @cite Credit to Erik Rigtorp https://rigtorp.se/spinlock/
    */
    class FSpinLock
    {
        std::atomic<bool> LockFlag;

    public:
        FSpinLock()
            : LockFlag{false}
        {
        }

        void UseLockLambda(const std::function<void()>& Functor)
        {
            for(;;)
            {
                if(!LockFlag.exchange(true, std::memory_order_acquire))
                {
                    break;
                }

                while(LockFlag.load(std::memory_order_relaxed))
                {
                    HARDWARE_PAUSE();
                }
            }

            // Do the work
            Functor();

            LockFlag.store(false, std::memory_order_release);
        } 
    };
}

constexpr uint64 RoundQueueSizeUpToNearestPowerOfTwo(const uint64 QueueSize)
{
    uint64 N = QueueSize;

    N--;
    N |= N >> 1;
    N |= N >> 2;
    N |= N >> 4;
    N |= N >> 8;
    N |= N >> 16;
    N |= N >> 32;
    N++;
            
    return N;
}

template<typename T, uint64 TQueueSize = 0>
class FBoundedQueueBase
{
    using FElement = T;
    using FSpinLock = QueueTypes::FSpinLock;
    using FCursor = uint64;

    /* TODO: static_asserts */
    static_assert(TQueueSize > 0, "");
    
public:
    FBoundedQueueBase()             = default;
    virtual ~FBoundedQueueBase()    = default;

    FBoundedQueueBase(const FBoundedQueueBase& other)                         = delete;
    FBoundedQueueBase(FBoundedQueueBase&& other) noexcept                     = delete;
    virtual FBoundedQueueBase& operator=(const FBoundedQueueBase& other)      = delete;
    virtual FBoundedQueueBase& operator=(FBoundedQueueBase&& other) noexcept  = delete;

protected:
    class FBufferNode
    {
    public:
        FElement Data;
        uint8 PaddingBytes0[QUEUE_PADDING_BYTES(sizeof(T))] = {};
        FSpinLock SpinLock;
        uint8 PaddingBytes1[QUEUE_PADDING_BYTES(sizeof(FSpinLock))] = {};

    public:
        FBufferNode()
            : SpinLock()
        {
        }

        FORCEINLINE void GetData(FElement& Out)
        {
            this->SpinLock.UseLockLambda([&]()
            {
                Out = this->Data;
            });
        }
        
        FORCEINLINE void SetData(const FElement& NewData)
        {
            this->SpinLock.UseLockLambda([&]()
            {
                this->Data = NewData;
            });
        }
    };
    
    class FBufferData
    {
    public:
        /** Both IndexMask & CircularBuffer data are only accessed at the same time.
          * This results in true-sharing.
         */
        const uint64 IndexMask;
        FBufferNode* CircularBuffer;
        uint8 PaddingBytes0[QUEUE_PADDING_BYTES((sizeof(uint64) * 2) - sizeof(void*))] = {};
        /** A Secondary index mask used for all cases except when accessing the CircularBuffer.
          * This extra index mask helps to avoid false-sharing.
         */
        const uint64 IndexMaskUtility;
        uint8 PaddingBytes1[QUEUE_PADDING_BYTES(sizeof(uint64))] = {};
        
    public:
        FBufferData()
            : IndexMask(RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize) - 1),
            IndexMaskUtility(IndexMask)
        {
            /** Contigiously allocate the buffer.
              * The theory behind using calloc and not aligned_alloc
              * or equivelant, is that the memory should still be aligned,
              * since calloc will align by the type size, which in this caseS
              * is a multiple of the cache line size.
             */
            CircularBuffer = (FBufferNode*)calloc(IndexMask + 1, sizeof(FBufferNode));
        }

        virtual ~FBufferData()
        {
            if(CircularBuffer)
            {
                free(CircularBuffer);
            }
        }

        FBufferData(const FBufferData& other)                           = delete;
        FBufferData(FBufferData&& other) noexcept                       = delete;
        virtual FBufferData& operator=(const FBufferData& other)        = delete;
        virtual FBufferData& operator=(FBufferData&& other) noexcept    = delete;
    };

public:
    virtual FORCEINLINE bool Push(const FElement& NewElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursor CurrentProducerCursor;
        
        for(;;)
        {
            CurrentProducerCursor = ProducerCursor.load(CursorDataLoadOrder);
            const FCursor CurrentConsumerCursor = ConsumerCursor.load(CursorDataLoadOrder);

            if(((CurrentProducerCursor & BufferData.IndexMaskUtility) + 1)
                == (CurrentConsumerCursor & BufferData.IndexMaskUtility))
            {
                return false;
            }

            if(ProducerCursor.compare_exchange_weak(CurrentProducerCursor,
                CurrentProducerCursor + 1,
                std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }

            HARDWARE_PAUSE();
        }

        BufferData.CircularBuffer[
            CurrentProducerCursor & BufferData.IndexMask]
            .SetData(NewElement);
        
        return true;
    }
    
    virtual FORCEINLINE bool Pop(FElement& OutElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursor CurrentConsumerCursor;
        
        for(;;)
        {
            const FCursor CurrentProducerCursor = ProducerCursor.load(CursorDataLoadOrder);
            CurrentConsumerCursor = ConsumerCursor.load(CursorDataLoadOrder);

            if((CurrentProducerCursor & BufferData.IndexMaskUtility)
                == (CurrentConsumerCursor & BufferData.IndexMaskUtility))
            {
                return false;
            }

            if(ConsumerCursor.compare_exchange_weak(CurrentConsumerCursor,
                CurrentConsumerCursor + 1,
                std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }

            HARDWARE_PAUSE();
        }

        BufferData.CircularBuffer[
            CurrentConsumerCursor & BufferData.IndexMask]
            .GetData(OutElement);
        
        return true;
    }

    virtual FORCEINLINE bool Push_Cached(const FElement& NewElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursorDataCache CurrentCursorDataCache;
        
        for(;;)
        {
            CurrentCursorDataCache = CursorDataCache.Load(CursorDataLoadOrder);

            if((CurrentCursorDataCache.ProducerCursor & BufferData.IndexMaskUtility) + 1
                == (CurrentCursorDataCache.ConsumerCursor & BufferData.IndexMaskUtility))
            {
                return false;
            }

            const uint64 DesiredProducerCursorValue = CurrentCursorDataCache.ProducerCursor + 1;
            
            if(ProducerCursor.compare_exchange_weak(CurrentCursorDataCache.ProducerCursor,
                DesiredProducerCursorValue,
                std::memory_order_release, std::memory_order_relaxed))
            {
                CursorDataCache.SetProducerCursor(DesiredProducerCursorValue);
                break;
            }

            HARDWARE_PAUSE();
        }

        BufferData.CircularBuffer[
            CurrentCursorDataCache.ProducerCursor & BufferData.IndexMask]
            .SetData(NewElement);
        
        return true;
    }
    
    virtual FORCEINLINE bool Pop_Cached(FElement& OutElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursorDataCache CurrentCursorDataCache;
        
        for(;;)
        {
            CurrentCursorDataCache = CursorDataCache.Load(CursorDataLoadOrder);

            if((CurrentCursorDataCache.ProducerCursor & BufferData.IndexMaskUtility)
                == (CurrentCursorDataCache.ConsumerCursor & BufferData.IndexMaskUtility))
            {
                return false;
            }

            const uint64 DesiredConsumerCursorValue = CurrentCursorDataCache.ConsumerCursor + 1;
            
            if(ConsumerCursor.compare_exchange_weak(CurrentCursorDataCache.ConsumerCursor,
                DesiredConsumerCursorValue,
                std::memory_order_release, std::memory_order_relaxed))
            {
                CursorDataCache.SetConsumerCursor(DesiredConsumerCursorValue);
                break;
            }

            HARDWARE_PAUSE();
        }

        BufferData.CircularBuffer[
            CurrentCursorDataCache.ConsumerCursor & BufferData.IndexMask]
            .GetData(OutElement);
        
        return true;
    }

    virtual FORCEINLINE bool Push_Cached_Individual(const FElement& NewElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursor CurrentProducerCursorCached;
        
        for(;;)
        {
            CurrentProducerCursorCached = ProducerCursorCache.load(CursorDataLoadOrder);
            const FCursor CurrentConsumerCursorCached = ConsumerCursorCache.load(CursorDataLoadOrder);

            if(((CurrentProducerCursorCached & BufferData.IndexMaskUtility) + 1)
                == (CurrentConsumerCursorCached & BufferData.IndexMaskUtility))
            {
                return false;
            }

            const uint64 DesiredProducerCursorValue = CurrentProducerCursorCached + 1;
            
            if(ProducerCursor.compare_exchange_weak(CurrentProducerCursorCached,
                DesiredProducerCursorValue,
                std::memory_order_release, std::memory_order_relaxed))
            {
                ProducerCursorCache.store(std::memory_order_release);
                break;
            }

            HARDWARE_PAUSE();
        }

        BufferData.CircularBuffer[
            CurrentProducerCursorCached & BufferData.IndexMask]
            .SetData(NewElement);
        
        return true;
    }
    
    virtual FORCEINLINE bool Pop_Cached_Individual(FElement& OutElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursor CurrentConsumerCursorCached;
        
        for(;;)
        {
            const FCursor CurrentProducerCursorCached = ProducerCursorCache.load(CursorDataLoadOrder);
            CurrentConsumerCursorCached = ConsumerCursorCache.load(CursorDataLoadOrder);

            if((CurrentProducerCursorCached & BufferData.IndexMaskUtility)
                == (CurrentConsumerCursorCached & BufferData.IndexMaskUtility))
            {
                return false;
            }

            const uint64 DesiredConsumerCursorValue = CurrentConsumerCursorCached + 1;
            
            if(ProducerCursor.compare_exchange_weak(CurrentConsumerCursorCached,
                DesiredConsumerCursorValue,
                std::memory_order_release, std::memory_order_relaxed))
            {
                ConsumerCursorCache.store(std::memory_order_release);
                break;
            }

            HARDWARE_PAUSE();
        }

        BufferData.CircularBuffer[
            CurrentConsumerCursorCached & BufferData.IndexMask]
            .GetData(OutElement);
        
        return true;
    }

    virtual FORCEINLINE uint64 Size() const
    {
        return BufferData.IndexMaskUtility + 1;
    }
    
    virtual FORCEINLINE bool Empty() const
    {
        return false;
    }
    
    virtual FORCEINLINE bool Full() const
    {
        return false;
    }

protected:
    struct FCursorDataCache
    {
        FCursor ProducerCursor;
        FCursor ConsumerCursor;
    };
    
    struct FCursorData
    {
        std::atomic<FCursor> ProducerCursor;
        std::atomic<FCursor> ConsumerCursor;

        FCursorData(const FCursor InProducerCursor = 0, const FCursor InConsumerCursor = 0)
        {
            ProducerCursor.store(InProducerCursor, std::memory_order_release);
            ConsumerCursor.store(InConsumerCursor, std::memory_order_release);
        }

        void SetProducerCursor(const FCursor InProducerCursor)
        {
            ProducerCursor.store(InProducerCursor, std::memory_order_release);
        }

        void SetConsumerCursor(const FCursor InConsumerCursor)
        {
            ConsumerCursor.store(InConsumerCursor, std::memory_order_release);
        }

        FCursorDataCache Load(const std::memory_order MemoryOrder) const
        {
            return {ProducerCursor.load(MemoryOrder), ConsumerCursor.load(MemoryOrder)};
        }
    };

protected:
    CACHE_ALIGN FBufferData BufferData;
    
    /* Used in all versions of push/pop */
    CACHE_ALIGN std::atomic<FCursor> ProducerCursor;
    CACHE_ALIGN std::atomic<FCursor> ConsumerCursor;

    /* For the cached functions */
    CACHE_ALIGN FCursorData CursorDataCache;

    /* For the individually cached functions */
    CACHE_ALIGN std::atomic<FCursor> ProducerCursorCache;
    CACHE_ALIGN std::atomic<FCursor> ConsumerCursorCache;
};
