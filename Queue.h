#pragma once

#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <functional>

#include "UEInterface.h"

#define FASTCALL __fastcall // pointles on x64
#define HARDWARE_PAUSE() _mm_pause(); // TODO: other platforms

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

    /* TODO: static_asserts */
    static_assert(TQueueSize > 0, ""); // TODO
    
public:
    FBoundedQueueBase()             = default;
    virtual ~FBoundedQueueBase()    = default;

    FBoundedQueueBase(const FBoundedQueueBase& other)                         = delete;
    FBoundedQueueBase(FBoundedQueueBase&& other) noexcept                     = delete;
    virtual FBoundedQueueBase& operator=(const FBoundedQueueBase& other)      = delete;
    virtual FBoundedQueueBase& operator=(FBoundedQueueBase&& other) noexcept  = delete;

    virtual FORCEINLINE bool    Push(const FElement& NewElement)    = 0;
    virtual FORCEINLINE bool    Pop(FElement& OutElement)           = 0;
    virtual FORCEINLINE uint64  Size() const                        = 0;
    virtual FORCEINLINE bool    Empty() const                       = 0;
    virtual FORCEINLINE bool    Full() const                        = 0;

protected:
    class CACHE_ALIGN FBufferNode
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
    
    class CACHE_ALIGN FBufferData
    {
    public:
        const uint64 IndexMask;
        // FBufferNode CircularBuffer[RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize)];
        FBufferNode* CircularBuffer;
        uint8 PaddingBytes0[QUEUE_PADDING_BYTES(sizeof(uint64) - sizeof(void*))] = {};
        
    public:
        FBufferData()
            : IndexMask(RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize) - 1)
        {
            /** Contigiously allocate the buffer.
              * The theory behind using calloc and not aligned_alloc
              * or equivelant, is that the memory should still be aligned,
              * since calloc will align by the type size, which in this caseS
              * is a multiple of the cache line size.
             */
            CircularBuffer = (FBufferNode*)calloc(IndexMask + 1, sizeof(FBufferNode));
        }

        ~FBufferData()
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
    FBufferData BufferData;
};

template<typename T, uint64 TQueueSize = 0>
class FBoundedQueueMpmc final : public FBoundedQueueBase<T, TQueueSize>
{
    using FElement = T;
    using FBase = FBoundedQueueBase<T, TQueueSize>;
    
public:
    FBoundedQueueMpmc()
        : FBase()
    {
    }

    ~FBoundedQueueMpmc()
    {
    }

    FBoundedQueueMpmc(const FBoundedQueueMpmc& other)                   = delete;
    FBoundedQueueMpmc(FBoundedQueueMpmc&& other) noexcept               = delete;
    FBoundedQueueMpmc& operator=(const FBoundedQueueMpmc& other)        = delete;
    FBoundedQueueMpmc& operator=(FBoundedQueueMpmc&& other) noexcept    = delete;

    virtual FORCEINLINE bool Push(const FElement& NewElement) override
    {
        FCursor CurrentProducerCursor;
        FCursor CurrentConsumerCursor;
        
        while(true)
        {
            CurrentProducerCursor = ProducerCursor.load(std::memory_order_acquire);
            CurrentConsumerCursor = ConsumerCursor.load(std::memory_order_acquire);

            if((CurrentProducerCursor.Cursor & FBase::BufferData.IndexMask) + 1
                == (CurrentConsumerCursor.Cursor & FBase::BufferData.IndexMask))
            {
                return false;
            }

            if(ProducerCursor.compare_exchange_weak(CurrentConsumerCursor,
                FCursor(CurrentConsumerCursor.Cursor + 1),
                std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }

            HARDWARE_PAUSE();
        }

        FBase::BufferData.CircularBuffer[
            CurrentProducerCursor.Cursor & FBase::BufferData.IndexMask]
            .SetData(NewElement);
        
        return true;
    }
    
    FORCEINLINE bool Push_Cached(const FElement& NewElement, std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursorDataCache CurrentCursorData;
        
        while(true)
        {
            CurrentCursorData = CursorDataCache.Load(CursorDataLoadOrder);

            if((CurrentCursorData.ProducerCursor & FBase::BufferData.IndexMask) + 1
                == (CurrentCursorData.ConsumerCursor & FBase::BufferData.IndexMask))
            {
                return false;
            }

            const uint64 DesiredProducerCursorValue = CurrentCursorData.ProducerCursor +1;
            const FCursor CurrentProducerCursor = FCursor(CurrentCursorData.ProducerCursor);
            const FCursor DesiredProducerCursor = FCursor(DesiredProducerCursorValue);
            if(ProducerCursor.compare_exchange_weak(CurrentProducerCursor,
                DesiredProducerCursor,
                std::memory_order_release, std::memory_order_relaxed))
            {
                CursorDataCache.SetProducerCursor(DesiredProducerCursorValue);
                break;
            }

            HARDWARE_PAUSE();
        }

        FBase::BufferData.CircularBuffer[
            CurrentCursorData.ProducerCursor & FBase::BufferData.IndexMask]
            .SetData(NewElement);
        
        return true;
    }
    
    virtual FORCEINLINE bool Pop(FElement& OutElement) override // TODO: Cache version of this
    {
        FCursor CurrentProducerCursor;
        FCursor CurrentConsumerCursor;
        
        while(true)
        {
            CurrentProducerCursor = ProducerCursor.load(std::memory_order_acquire);
            CurrentConsumerCursor = ConsumerCursor.load(std::memory_order_acquire);

            if((CurrentProducerCursor.Cursor & FBase::BufferData.IndexMask)
                == (CurrentConsumerCursor.Cursor & FBase::BufferData.IndexMask))
            {
                return false;
            }

            if(ConsumerCursor.compare_exchange_weak(CurrentConsumerCursor,
                FCursor(CurrentConsumerCursor.Cursor + 1),
                std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }

            HARDWARE_PAUSE();
        }

        FBase::BufferData.CircularBuffer[
            CurrentConsumerCursor.Cursor & FBase::BufferData.IndexMask]
            .GetData(OutElement);
        
        return true;
    }

    FORCEINLINE bool Pop_Cached(FElement& OutElement, std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        FCursorDataCache CurrentCursorData;
        
        while(true)
        {
            CurrentCursorData = CursorDataCache.Load(CursorDataLoadOrder);

            if((CurrentCursorData.ProducerCursor & FBase::BufferData.IndexMask)
                == (CurrentCursorData.ConsumerCursor & FBase::BufferData.IndexMask))
            {
                return false;
            }

            const uint64 DesiredConsumerCursorValue = CurrentCursorData.ConsumerCursor + 1;
            const FCursor CurrentConsumerCursor = FCursor(CurrentCursorData.ConsumerCursor);
            const FCursor DesiredConsumerCursor = FCursor(DesiredConsumerCursorValue);
            if(ConsumerCursor.compare_exchange_weak(CurrentConsumerCursor,
                DesiredConsumerCursor,
                std::memory_order_release, std::memory_order_relaxed))
            {
                CursorDataCache.SetConsumerCursor(DesiredConsumerCursorValue);
                break;
            }

            HARDWARE_PAUSE();
        }

        FBase::BufferData.CircularBuffer[
            CurrentCursorData.ConsumerCursor & FBase::BufferData.IndexMask]
            .GetData(OutElement);
        
        return true;
    }

    virtual FORCEINLINE uint64 Size() const override
    {
        return 0;
    }
    
    virtual FORCEINLINE bool Empty() const override
    {
        return false;
    }
    
    virtual FORCEINLINE bool Full() const override
    {
        return false;
    }

private:
    struct FCursorDataCache
    {
        uint64 ProducerCursor;
        uint64 ConsumerCursor;
    };
    
    struct FCursorData
    {
        std::atomic<uint64> ProducerCursor;
        std::atomic<uint64> ConsumerCursor;

        FCursorData(const uint64 InProducerCursor = 0, const uint64 InConsumerCursor = 0)
        {
            ProducerCursor.store(InProducerCursor, std::memory_order_release);
            ConsumerCursor.store(InConsumerCursor, std::memory_order_release);
        }

        void SetProducerCursor(const uint64 InProducerCursor)
        {
            ProducerCursor.store(InProducerCursor, std::memory_order_release);
        }

        void SetConsumerCursor(const uint64 InConsumerCursor)
        {
            ConsumerCursor.store(InConsumerCursor, std::memory_order_release);
        }

        FCursorDataCache Load(const std::memory_order MemoryOrder) const
        {
            return {ProducerCursor.load(MemoryOrder), ConsumerCursor.load(MemoryOrder)};
        }
    };

    struct FCursor
    {
        uint64 Cursor;
        
        FCursor(const uint64 InCursor = 0)
            : Cursor(InCursor)
        {
        }
    };

private:
    CACHE_ALIGN std::atomic<FCursor> ProducerCursor;
    CACHE_ALIGN std::atomic<FCursor> ConsumerCursor;
    CACHE_ALIGN FCursorData CursorDataCache;
};
