#include <thread>
#include <atomic>

#include "Queue.h"
#include "LocalStuff/AtomicQueue.h"
#include "LocalStuff/MyTimer.h"

#define CORE_COUNT 16

#define ELEMENTS_TO_PROCESS (10000000 / CORE_COUNT)

using FBenchType = int;

//#define BENCH_START_QUEUE_SIZE      128
//#define BENCH_MAX_QUEUE_SIZE        1000000
//#define BENCH_START_THREAD_COUNT    2
//#define BENCH_MAX_THREAD_COUNT      32
//#define BENCH_START_CYCLE_COUNT     16
//#define BENCH_MAX_CYCLE_COUNT       1000000

#define BENCH_QUEUE_SIZE            1000000

#if false
    #define QueueVar                      MyQueue
    #define PushFunction(_ELEMENT_)       QueueVar.Push((_ELEMENT_))
    #define PopFunction(_ELEMENT_)        QueueVar.Pop((_ELEMENT_)) 
#else
    #define QueueVar                    OtherQueue
    #define PushFunction(_ELEMENT_)     QueueVar.push<FBenchType>(56)
    #define PopFunction(_ELEMENT_)      (_ELEMENT_) = QueueVar.pop() 
#endif

#define BENCH_SLEEP_UNIT(_SLEEP_LENGTH_) std::chrono::milliseconds((_SLEEP_LENGTH_))
#define BENCH_SLEEP_LENGTH 1

namespace QBenchmarks
{
    static FBoundedQueueBenchmarking<FBenchType, BENCH_QUEUE_SIZE> MyQueue;
    static atomic_queue::AtomicQueue2<FBenchType, BENCH_QUEUE_SIZE, true, true, true, false> OtherQueue;

    static std::atomic<int> ThreadsComplete = {0};

    static FORCEINLINE void WaitForCompletion(const int ThreadCount)
    {
        while(ThreadsComplete.load(std::memory_order_relaxed) < ThreadCount)
        {
            std::this_thread::sleep_for(BENCH_SLEEP_UNIT(BENCH_SLEEP_LENGTH));
        }
        ThreadsComplete.store(0);
    }
    
    static FORCEINLINE void NoDelayHighContentionRegular(const int ThreadCount, const int CycleCount,
        std::memory_order MemoryOrder = std::memory_order_acquire)
    {
        for(int i = 0; i < ThreadCount; ++i)
        {
            std::thread([&]() // producer
            {
                for(int j = 0; j < CycleCount; ++j)
                {
                    const FBenchType ValueToPush = j;
                    PushFunction(ValueToPush);
                }
                ThreadsComplete.fetch_add(1);
            }).detach();

            std::thread([&]() // consumer
            {
                for(int j = 0; j < CycleCount; ++j)
                {
                    FBenchType PoppedValue = 0;
                    PopFunction(PoppedValue);
                }
                ThreadsComplete.fetch_add(1);
            }).detach();
        }

        WaitForCompletion(ThreadCount * 2);
    }

    static FORCEINLINE void NoDelayHighProducerContentionRegular(const int ThreadCount, const int CycleCount,
        std::memory_order MemoryOrder = std::memory_order_acquire)
    {
        for(int i = 0; i < ThreadCount; ++i)
        {
            std::thread([&]() // producer
            {
                for(int j = 0; j < CycleCount; ++j)
                {
                    const FBenchType ValueToPush = j;
                    // PushFunction(ValueToPush);
                }
                ThreadsComplete.fetch_add(1);
            }).detach();
        }

        std::thread([&]() // consumer
        {
            const int AdjustedCycleCount = CycleCount * ThreadCount;
            for(int j = 0; j < AdjustedCycleCount; ++j)
            {
                FBenchType PoppedValue = 0;
                // PopFunction(PoppedValue);
            }
            ThreadsComplete.fetch_add(1);
        }).detach();

        WaitForCompletion(ThreadCount + 1);
    }

    static FORCEINLINE void NoDelayHighConsumerContentionRegular(const int ThreadCount, const int CycleCount,
        std::memory_order MemoryOrder = std::memory_order_acquire)
    {
        std::thread([&]()  // producer
        {
            for(int j = 0; j < CycleCount; ++j)
            {
                const FBenchType ValueToPush = j;
                // PushFunction(ValueToPush);
            }
            ThreadsComplete.fetch_add(1);
        }).detach();
        
        for(int i = 0; i < ThreadCount; ++i)
        {
            std::thread([&]() // consumer
            {
                const int AdjustedCycleCount = CycleCount * ThreadCount;
                for(int j = 0; j < AdjustedCycleCount; ++j)
                {
                    FBenchType PoppedValue = 0;
                    // PopFunction(PoppedValue);
                }
                ThreadsComplete.fetch_add(1);
            }).detach();
        }

        WaitForCompletion(ThreadCount + 1);
    }
}

int main(int argc, char* argv[])
{
    QBenchmarks::NoDelayHighContentionRegular(CORE_COUNT, ELEMENTS_TO_PROCESS);
    
    return 0;
}
