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

    virtual FORCEINLINE bool Push(const FElement& NewElement) = 0;
    virtual FORCEINLINE bool Pop(const FElement& OutElement) = 0;

protected:
    class CACHE_ALIGN FBufferNodeBase
    {
    protected:
        FElement Data;
        uint8 PaddingBytes0[QUEUE_PADDING_BYTES(sizeof(T))] = {};
        FSpinLock SpinLock;
        uint8 PaddingBytes1[QUEUE_PADDING_BYTES(sizeof(FSpinLock))] = {};

    public:
        FBufferNodeBase()
            : SpinLock()
        {
        }

        FORCEINLINE void GetData(const FElement& Out)
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

    template<class TBufferNodeType = FBufferNodeBase>
    class CACHE_ALIGN FBufferData
    {
    protected:
        const uint64 IndexMask;
        TBufferNodeType* CircularBuffer;
        uint8 PaddingBytes0[QUEUE_PADDING_BYTES(sizeof(uint64) - sizeof(TBufferNodeType*))] = {};
        
    public:
        FBufferData()
            : IndexMask(RoundQueueSizeUpToNearestPowerOfTwo() - 1)
        {
            /** Contigiously allocate the buffer.
              * The theory behind using calloc and not aligned_alloc
              * or equivelant, is that the memory should still be aligned,
              * since calloc will align by the type size, which in this case
              * is a multiple of the cache line size.
             */
            CircularBuffer = (TBufferNodeType*)calloc(IndexMask + 1, sizeof(TBufferNodeType));
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

private:
    FORCEINLINE uint64 RoundQueueSizeUpToNearestPowerOfTwo()
    {
        uint64 N = TQueueSize;

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
};

template<typename T>
class FBoundedQueueMpmc final : public FBoundedQueueBase<T>
{
    using FElement = T;
    
public:
    FBoundedQueueMpmc()
        : FBoundedQueueBase<T>()
    {
    }

    ~FBoundedQueueMpmc()
    {
    }

    FBoundedQueueMpmc(const FBoundedQueueMpmc& other)                   = delete;
    FBoundedQueueMpmc(FBoundedQueueMpmc&& other) noexcept               = delete;
    FBoundedQueueMpmc& operator=(const FBoundedQueueMpmc& other)        = delete;
    FBoundedQueueMpmc& operator=(FBoundedQueueMpmc&& other) noexcept    = delete;

    FORCEINLINE bool Push(const FElement& NewElement) override
    {
        return false;
    }
    FORCEINLINE bool Pop(const FElement& OutElement) override
    {
        
        return false;
    }

protected:
    struct FBufferNode : public FBoundedQueueBase<T>::FBufferNodeBase
    {
        FBufferNode()
            : FBoundedQueueBase<T>::FBufferNodeBase()
        {
        }
    };
};

/* sub-type inherritance example
class Other : FQueueBase<int>
{
    struct Testing2 : Testing
    {
        bool Tesssst() <--- virtual function
        {
            return false;
        }
    };
};*/
