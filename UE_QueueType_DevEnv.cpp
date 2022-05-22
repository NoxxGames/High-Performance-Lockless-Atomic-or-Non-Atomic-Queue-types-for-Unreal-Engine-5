#include <thread>
#include <atomic>

#include "LocalStuff/MyTimer.h"
#include "LocalStuff/My_MpmcQueue.h"

#define CORE_COUNT 6

#define ELEMENTS_TO_PROCESS (1000000 / CORE_COUNT)

std::atomic<int> ConsumersComplete = {0};

int main(int argc, char* argv[])
{
    /* TODO: Understand what is meant by "optimistic" in the AtomicQueueType */
    
    OpenNanoTimer MyTimer;
    bounded_circular_mpmc_queue<int, 1024000> MyQueue;
    
    for(int i = 0; i < CORE_COUNT / 2; ++i)
    {
        std::thread([&]()
        {
            const int ValueToAdd = 256;
            for(int j = 0; j < ELEMENTS_TO_PROCESS / 2; ++j)
            {
                MyQueue.push(ValueToAdd);
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
                MyQueue.pop(ValueToAdd);
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
