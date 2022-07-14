#include <thread>
#include <atomic>

#include "Queue.h"

#define CORE_COUNT 8
#define ELEMENTS_TO_PROCESS         (6000000 / CORE_COUNT)
#define BENCH_QUEUE_SIZE            1000000

#define QueueVar                      MyQueue
#define PushFunction(_ELEMENT_)       QueueVar.Push((_ELEMENT_))
#define PopFunction(_ELEMENT_)        (_ELEMENT_) = QueueVar.Pop() 

#define BENCH_SLEEP_UNIT(_SLEEP_LENGTH_) std::chrono::milliseconds((_SLEEP_LENGTH_))
#define BENCH_SLEEP_LENGTH 1

using FBenchType = int;

namespace QBenchmarks
{
    static TBoundedCircularQueue<FBenchType, BENCH_QUEUE_SIZE, true, true, false> MyQueue;

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
                    FBenchType ValueToPush = j;
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
}

int main(int argc, char* argv[])
{
    QBenchmarks::NoDelayHighContentionRegular(CORE_COUNT, ELEMENTS_TO_PROCESS);
    
    return 0;
}
