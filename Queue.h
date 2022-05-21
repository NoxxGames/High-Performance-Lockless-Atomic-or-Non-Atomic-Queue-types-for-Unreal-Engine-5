#pragma once

#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <functional>

#include "UEInterface.h"

#define FASTCALL __fastcall // pointles on x64
#define HARDWARE_PAUSE() _mm_pause(); // TODO: other platforms

#define QUEUE_PADDING_BYTES(_TYPE_SIZES_) (PLATFORM_CACHE_LINE_SIZE - (_TYPE_SIZES_) % PLATFORM_CACHE_LINE_SIZE)

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

template<typename T>
class FBoundedQueueBase
{
    using FElement = T;
    using FSpinLock = QueueTypes::FSpinLock;

    /* TODO: static_asserts */
    
public:
    FBoundedQueueBase()             = default;
    virtual ~FBoundedQueueBase()    = default;

    FBoundedQueueBase(const FBoundedQueueBase& other)                         = delete;
    FBoundedQueueBase(FBoundedQueueBase&& other) noexcept                     = delete;
    virtual FBoundedQueueBase& operator=(const FBoundedQueueBase& other)      = delete;
    virtual FBoundedQueueBase& operator=(FBoundedQueueBase&& other) noexcept  = delete;

    virtual FORCEINLINE bool FASTCALL Push(const FElement& NewElement) = 0;
    virtual FORCEINLINE bool FASTCALL Pop(const FElement& OutElement) = 0;

protected:
    class FBufferNodeBase
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

        void GetData(const FElement& Out)
        {
            this->SpinLock.UseLockLambda([&]()
            {
                Out = this->Data;
            });
        }
        
        void SetData(const FElement& NewData)
        {
            this->SpinLock.UseLockLambda([&]()
            {
                this->Data = NewData;
            });
        }
    };
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

    FORCEINLINE bool FASTCALL Push(const FElement& NewElement) override
    {
        return false;
    }
    FORCEINLINE bool FASTCALL Pop(const FElement& OutElement) override
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
