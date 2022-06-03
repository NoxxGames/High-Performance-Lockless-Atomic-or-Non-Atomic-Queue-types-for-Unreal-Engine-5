#include <thread>
#include <atomic>

#include "Queue.h"
#include "LocalStuff/AtomicQueue.h"
#include "LocalStuff/MyTimer.h"
#include "LocalStuff/My_MpmcQueue.h"

#define CORE_COUNT 16

#define ELEMENTS_TO_PROCESS (1000000 / CORE_COUNT)

std::atomic<int> ConsumersComplete = {0};

using FBenchType = int;

#define BENCH_START_QUEUE_SIZE      128
#define BENCH_MAX_QUEUE_SIZE        1000000

#define BENCH_START_THREAD_COUNT    2
#define BENCH_MAX_THREAD_COUNT      32

#define BENCH_START_CYCLE_COUNT     16
#define BENCH_MAX_CYCLE_COUNT       1000000

#define QueueVar                    MyQueue
#define PushFunction(_ELEMENT_)     Push((_ELEMENT_))
#define PopFunction(_ELEMENT_)      Pop((_ELEMENT_)) 

#define BENCH_SLEEP_UNIT(_SLEEP_LENGTH_) std::chrono::milliseconds((_SLEEP_LENGTH_))
#define BENCH_SLEEP_LENGTH 1

namespace QBenchmarks
{
    static FBoundedQueueBenchmarking<int, 100000> MyQueue;

    static std::atomic<int> ThreadsComplete = {0};

    static FORCEINLINE void WaitForCompletion(const int ThreadCount)
    {
        while(ThreadsComplete.load(std::memory_order_relaxed) < ThreadCount)
        {
            std::this_thread::sleep_for(BENCH_SLEEP_UNIT(BENCH_SLEEP_LENGTH));
        }
        ThreadsComplete.store(0);
    }

    static FORCEINLINE void CreateThreadLambda(const std::function<void()>& Functor)
    {
        std::thread([&]() // Producer
        {
            Functor();
            ThreadsComplete.fetch_add(1);
        }).detach();
    }
    
    static FORCEINLINE void NoDelayHighContentionRegular(const int ThreadCount, const int CycleCount,
        std::memory_order MemoryOrder = std::memory_order_acquire)
    {
        for(int i = 0; i < ThreadCount; ++i)
        {
            CreateThreadLambda([&]() // producer
            {
                for(int j = 0; j < CycleCount; ++j)
                {
                    const FBenchType ValueToPush = j;
                    QueueVar.PushFunction(ValueToPush);
                }
            });

            CreateThreadLambda([&]() // consumer
            {
                for(int j = 0; j < CycleCount; ++j)
                {
                    FBenchType PoppedValue = 0;
                    QueueVar.PopFunction(PoppedValue);
                }
            });
        }

        WaitForCompletion(ThreadCount);
    }

    static FORCEINLINE void NoDelayHighProducerContentionRegular(const int ThreadCount, const int CycleCount,
        std::memory_order MemoryOrder = std::memory_order_acquire)
    {
        for(int i = 0; i < ThreadCount; ++i)
        {
            CreateThreadLambda([&]() // producer
            {
                for(int j = 0; j < CycleCount; ++j)
                {
                    const FBenchType ValueToPush = j;
                    QueueVar.PushFunction(ValueToPush);
                }
            });
        }

        CreateThreadLambda([&]() // consumer
        {
            for(int j = 0; j < CycleCount; ++j)
            {
                FBenchType PoppedValue = 0;
                QueueVar.PopFunction(PoppedValue);
            }
        });

        WaitForCompletion(ThreadCount);
    }

    static FORCEINLINE void NoDelayHighConsumerContentionRegular(const int ThreadCount, const int CycleCount,
        std::memory_order MemoryOrder = std::memory_order_acquire)
    {
        CreateThreadLambda([&]()  // producer
        {
            for(int j = 0; j < CycleCount; ++j)
            {
                const FBenchType ValueToPush = j;
                QueueVar.PushFunction(ValueToPush);
            }
        });
        
        for(int i = 0; i < ThreadCount; ++i)
        {
            CreateThreadLambda([&]() // consumer
            {
                for(int j = 0; j < CycleCount; ++j)
                {
                    FBenchType PoppedValue = 0;
                    QueueVar.PopFunction(PoppedValue);
                }
            });
        }

        WaitForCompletion(ThreadCount);
    }
}

int main(int argc, char* argv[])
{
    
    OpenNanoTimer MyTimer;
    FBoundedQueueBenchmarking<int, 100000> MyQueue;
    //atomic_queue::AtomicQueueB2<int, std::allocator<int>,true, true, true> MyOtherQueue(1000000);
    

    // QBenchmarks::NoDelayHighContentionBenchmarkRegular(2, 1000000);
    
    for(int i = 0; i < CORE_COUNT / 2; ++i)
    {
        std::thread([&]()
        {
            const int ValueToAdd = 256;
            for(int j = 0; j < ELEMENTS_TO_PROCESS / 2; ++j)
            {
                MyQueue.Push_Cached(ValueToAdd);
                //MyOtherQueue.try_push(ValueToAdd);
            }
        }).detach();
    }

    for(int i = 0; i < CORE_COUNT / 2; ++i)
    {
        std::thread([&]()
        {
            int ValueToAdd = 0;
            for(int j = 0; j < ELEMENTS_TO_PROCESS / 2; ++j)
            {
                MyQueue.Pop_Cached(ValueToAdd);
                //MyOtherQueue.try_pop(ValueToAdd);
            }

            ConsumersComplete.fetch_add(1, std::memory_order_seq_cst);
        }).detach();
    }
    
    MyTimer.StartTimer();
    
    while(ConsumersComplete.load(std::memory_order_relaxed) < (CORE_COUNT / 2))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    MyTimer.EndTimer();
    
    const long double Ms = MyTimer.GetNanoseconds();
    printf("Time Taken: %Lf\n", Ms);
    printf("Per Element avg: %Lf\n", (Ms / 1000000));
    
    system("pause");
    return 0;
}
