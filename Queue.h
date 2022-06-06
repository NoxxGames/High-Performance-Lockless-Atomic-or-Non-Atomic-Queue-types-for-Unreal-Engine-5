﻿#pragma once

#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

//------------------------------------------------------------//
//                                                            //
//                    START UE5 INTERFACE                     //
//                                                            //
//------------------------------------------------------------//

typedef int8_t      int8;
typedef int16_t     int16;
typedef int32_t     int32;
typedef int64_t     int64;

typedef uint8_t     uint8;
typedef uint16_t    uint16;
typedef uint32_t    uint32;
typedef uint64_t    uint64;

#define PLATFORM_CACHE_LINE_SIZE 64

#if defined(_MSC_VER)
    #define HARDWARE_PAUSE()                _mm_pause();
    #define FORCEINLINE                     __forceinline
#else
    #if defined(__clang__) || defined(__GNUC__)
        #define HARDWARE_PAUSE()            __builtin_ia32_pause();
    #else
        #define HARDWARE_PAUSE()            std::this_thread::yield()
    #endif
    #define FORCEINLINE                     inline
#endif

//------------------------------------------------------------//
//                                                            //
//                     END UE5 INTERFACE                      //
//                                                            //
//------------------------------------------------------------//

#define QUEUE_PADDING_BYTES(_TYPE_SIZES_) (PLATFORM_CACHE_LINE_SIZE - (_TYPE_SIZES_) % PLATFORM_CACHE_LINE_SIZE)
#define CACHE_ALIGN alignas(PLATFORM_CACHE_LINE_SIZE)

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
class FBoundedQueueBenchmarking
{
    using FElement = T;
    using FCursor = uint64;

    /*
     * TODO: static_asserts
    */
    static_assert(TQueueSize > 0, "");
    
public:
    FBoundedQueueBenchmarking()             = default;
    virtual ~FBoundedQueueBenchmarking()    = default;

    FBoundedQueueBenchmarking(const FBoundedQueueBenchmarking& other)                         = delete;
    FBoundedQueueBenchmarking(FBoundedQueueBenchmarking&& other) noexcept                     = delete;
    virtual FBoundedQueueBenchmarking& operator=(const FBoundedQueueBenchmarking& other)      = delete;
    virtual FBoundedQueueBenchmarking& operator=(FBoundedQueueBenchmarking&& other) noexcept  = delete;

protected:
    class FBufferNode
    {
    public:
        std::atomic<FElement> Data;
        uint8 PaddingBytes0[QUEUE_PADDING_BYTES(sizeof(std::atomic<FElement>))] = {};

    public:
        FBufferNode()
        {
        }

        FORCEINLINE void GetData(FElement& Out)
        {
            Out = Data.load(std::memory_order_acquire);
        }
        
        FORCEINLINE void SetData(const FElement& NewData)
        {
            Data.store(NewData, std::memory_order_release);
        }
    };
    
    class FBufferData
    {
    public:
        /** Both IndexMask & CircularBuffer data are only accessed at the same time.
          * This results in true-sharing.
         */
        const volatile uint64 IndexMask;
        FBufferNode CircularBuffer[RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize)];
        uint8 PaddingBytes0[QUEUE_PADDING_BYTES((sizeof(uint64) * 2) - sizeof(void*))] = {};
        /** A Secondary index mask used for all cases except when accessing the CircularBuffer.
          * This extra index mask helps to avoid false-sharing.
         */
        const volatile uint64 IndexMaskUtility;
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
            // CircularBuffer = (FBufferNode*)calloc(IndexMask + 1, sizeof(FBufferNode));
        }

        virtual ~FBufferData()
        {
            //if(CircularBuffer)
            //{
            //    free(CircularBuffer);
            //}
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

    virtual FORCEINLINE bool Push_MultiCursor(const FElement& NewElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
        return true;
    }
    
    virtual FORCEINLINE bool Pop_MultiCursor(FElement& OutElement,
        const std::memory_order CursorDataLoadOrder = std::memory_order_acquire)
    {
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

    /* cite: https://travisdowns.github.io/blog/2020/07/06/concurrency-costs.html */
    
    class FCASMultiCursor
    {
        static constexpr uint64 NUM_CURSORS     = 64U;
        static constexpr uint64 INDEX_MASK      = NUM_CURSORS - 1;

        std::atomic<uint8> Index{0};
        std::atomic<uint64> Cursors[NUM_CURSORS];
        
    public:
        uint64 operator++(int)
        {
            for(;;)
            {
                const uint8 i = Index.load(std::memory_order_relaxed);
                std::atomic<uint64>& Cursor = Cursors[i];

                uint64 CurrentValue = Cursor.load();
                if(Cursor.compare_exchange_strong(CurrentValue, CurrentValue + 1))
                {
                    return CurrentValue;
                }
                
                Index.store(((i + 1) & INDEX_MASK));
            }
        }

        uint64 Read()
        {
            uint64 Sum = 0;
            for(auto& Cursor : Cursors)
            {
                Sum += Cursor.load();
            }
            return Sum;
        }
    };

    CACHE_ALIGN FCASMultiCursor ProducerMultiCursor;
    CACHE_ALIGN FCASMultiCursor ConsumerMultiCursor;
    
   /* TODO, proper TLS implementation, without using thread_local
    class FTLSCursor
    {
    public:
        FTLSCursor()
            : Cursor{0}
        {
            TLSCursorData.AddNewTLSCursor(this);
        }

        ~FTLSCursor()
        {
            TLSCursorData.RemoveTLSCursor(this);
        }
        
        uint64 GetCursorValue() const
        {
            return Cursor.load(std::memory_order_relaxed);
        }

        void Increment()
        {
            const uint64 Current = Cursor.load(std::memory_order_relaxed);
            Cursor.store(Current + 1, std::memory_order_relaxed);
        }

    private:
        std::atomic<uint64> Cursor;
    };

private:
    class FTLSCursorData
    {
        std::mutex Lock;
        std::vector<FTLSCursor*> AllCursors;
        uint64 Accumulated;
        
    public:
        FTLSCursorData()
        {
            std::lock_guard<std::mutex> g(Lock);
            Accumulated = 0;
        }
        
        void AddNewTLSCursor(const FTLSCursor* NewCursor)
        {
            std::lock_guard<std::mutex> g(Lock);
            AllCursors.push_back(NewCursor);
        }
        
        void RemoveTLSCursor(const FTLSCursor* CursorToRemove)
        {
            std::lock_guard<std::mutex> g(Lock);
            Accumulated += CursorToRemove->GetCursorValue();
            AllCursors.erase(std::remove(AllCursors.begin(), AllCursors.end(), CursorToRemove), AllCursors.end());
        }

        uint64 Read()
        {
            std::lock_guard<std::mutex> g(Lock);
            uint64 Sum = 0;
            for(auto Local : AllCursors)
            {
                Sum += Local->GetCursorValue();
            }
            return Sum + Accumulated;
        }

        void Increment()
        {
            for(auto Local : AllCursors)
            {
                Local->Increment();
            }
        }
    };
    
    const FTLSCursorData TLSCursorData;
*/
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
