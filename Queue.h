#pragma once

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

typedef unsigned int uint;

#define PLATFORM_CACHE_LINE_SIZE 64

#if defined(_MSC_VER)
    #define SPIN_LOOP_PAUSE()                _mm_pause();
    #define FORCEINLINE                     __forceinline
#else
    #if defined(__clang__) || defined(__GNUC__)
        #define SPIN_LOOP_PAUSE()            __builtin_ia32_pause();
    #else
        #define SPIN_LOOP_PAUSE()            std::this_thread::yield()
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
#define Q_NOEXCEPT_ENABLED true

template<size_t TElementsPerCacheLine> struct GetCacheLineIndexBits { static int constexpr value = 0; };
template<> struct GetCacheLineIndexBits<256> { static int constexpr value = 8; };
template<> struct GetCacheLineIndexBits<128> { static int constexpr value = 7; };
template<> struct GetCacheLineIndexBits< 64> { static int constexpr value = 6; };
template<> struct GetCacheLineIndexBits< 32> { static int constexpr value = 5; };
template<> struct GetCacheLineIndexBits< 16> { static int constexpr value = 4; };
template<> struct GetCacheLineIndexBits<  8> { static int constexpr value = 3; };
template<> struct GetCacheLineIndexBits<  4> { static int constexpr value = 2; };
template<> struct GetCacheLineIndexBits<  2> { static int constexpr value = 1; };

template<bool TBool, uint TArraySize, size_t TElementsPerCacheLine>
struct GetIndexShuffleBits {
    static int constexpr bits = GetCacheLineIndexBits<TElementsPerCacheLine>::value;
    static unsigned constexpr min_size = 1U << (bits * 2);
    static int constexpr value = TArraySize < min_size ? 0 : bits;
};

template<uint array_size, size_t elements_per_cache_line>
struct GetIndexShuffleBits<false, array_size, elements_per_cache_line> {
    static int constexpr value = 0;
};

template<uint TBits>
constexpr uint RemapCursorWithMix(const uint Index, const uint Mix)
{
    return Index ^ Mix ^ (Mix << TBits);
}

/**
 * Multiple writers/readers contend on the same cache line when storing/loading elements at
 * subsequent indexes, aka false sharing. For power of 2 ring buffer size it is possible to re-map
 * the index in such a way that each subsequent element resides on another cache line, which
 * minimizes contention. This is done by swapping the lowest order N bits (which are the index of
 * the element within the cache line) with the next N bits (which are the index of the cache line)
 * of the element index.
 *
 * @cite https://graphics.stanford.edu/~seander/bithacks.html#SwappingBitsXOR
 * @cite https://stackoverflow.com/questions/12363715/swapping-individual-bits-with-xor
 */
template<uint TBits>
constexpr uint RemapCursor(const uint Index) noexcept
{
    return RemapCursorWithMix<uint, TBits>(Index, ((Index ^ (Index >> TBits)) & ((1U << TBits) - 1)));
}

constexpr uint RemapCursor(const uint Index) noexcept
{
    return Index;
}

template<typename T>
constexpr T& MapElement(T* Elements, uint Index) noexcept
{
    return Elements[Index];
}

constexpr uint32 RoundQueueSizeUpToNearestPowerOfTwo(uint32 A) noexcept
{
    --A;
    A |= A >> 1; A |= A >> 2; A |= A >> 4; A |= A >> 8; A |= A >> 16;
    ++A;
    
    return A;
}

constexpr uint64 RoundQueueSizeUpToNearestPowerOfTwo(uint64 A) noexcept
{
    --A;
    A |= A >> 1; A |= A >> 2; A |= A >> 4; A |= A >> 8; A |= A >> 16; A |= A >> 32;
    ++A;
    
    return A;
}

std::memory_order constexpr ACQUIRE     = std::memory_order_acquire;
std::memory_order constexpr RELEASE     = std::memory_order_release;
std::memory_order constexpr RELAXED     = std::memory_order_relaxed;
std::memory_order constexpr SEQ_CONST   = std::memory_order_seq_cst;

template<typename T, uint TQueueSize,
    bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class FBoundedQueueBase
{
    using FElementType = T;
    
    static constexpr std::memory_order FetchAddMemoryOrder = TTotalOrder ? SEQ_CONST : ACQUIRE;

protected:
    static constexpr uint RoundedSize = RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize);
    static constexpr uint IndexMask = RoundedSize - 1;
    
public:
    FBoundedQueueBase()
        : ProducerCursor{0},
        ConsumerCursor{0}
    {
    }
    
    virtual ~FBoundedQueueBase() = default;

    FBoundedQueueBase(const FBoundedQueueBase& other)                 = delete;
    FBoundedQueueBase(FBoundedQueueBase&& other) noexcept             = delete;
    FBoundedQueueBase& operator=(const FBoundedQueueBase& other)      = delete;
    FBoundedQueueBase& operator=(FBoundedQueueBase&& other) noexcept  = delete;

protected:
    virtual FORCEINLINE void Push(const FElementType& NewElement) = 0;
    
protected:
    uint IncrementProducerCursor() noexcept
    {
        return ProducerCursor.fetch_add(1, FetchAddMemoryOrder);
    }

    uint IncrementConsumerCursor() noexcept
    {
        return ConsumerCursor.fetch_add(1, FetchAddMemoryOrder);
    }

public:
    FORCEINLINE uint Size() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        return RoundedSize;
    }

    FORCEINLINE bool Full() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        const uint CurrentProducerCursor = ProducerCursor.load(RELAXED);
        const uint CurrentConsumerCursor = ConsumerCursor.load(RELAXED);

        return (CurrentProducerCursor + 1) == CurrentConsumerCursor;
    }
    
    FORCEINLINE bool Empty() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        const uint CurrentProducerCursor = ProducerCursor.load(RELAXED);
        const uint CurrentConsumerCursor = ConsumerCursor.load(RELAXED);

        return CurrentProducerCursor == CurrentConsumerCursor;
    }

    FORCEINLINE uint Num() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        // tail_ can be greater than head_ because of consumers doing pop, rather that try_pop, when the queue is empty.
        const int64 Difference = ProducerCursor.load(RELAXED) - ConsumerCursor.load(RELAXED);
        
        return Difference > 0 ? Difference : 0;
    }

protected:
    CACHE_ALIGN std::atomic<uint>   ProducerCursor;
    CACHE_ALIGN std::atomic<uint>   ConsumerCursor;
};

template<typename T, uint64 TQueueSize,
    bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class FBoundedCircularQueue : public FBoundedQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    /*
     * TODO: static_asserts
    */
    static_assert(TQueueSize > 0, "");
    
    enum class EBufferNodeState : uint8
    {
        EMPTY, STORING, FULL, LOADING
    };

    using FQueueBaseType = FBoundedQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>;
    
    using FElementType = T;
    
    static constexpr uint RoundedSize = FQueueBaseType::RoundedSize;
    static constexpr int ShuffleBits = GetIndexShuffleBits<
        false, RoundedSize, PLATFORM_CACHE_LINE_SIZE / sizeof(EBufferNodeState)>::value;
    static constexpr uint IndexMask = FQueueBaseType::IndexMask;
    
    static constexpr std::atomic<uint> ProducerCursor = FQueueBaseType::ProducerCursor;
    static constexpr std::atomic<uint> ConsumerCursor = FQueueBaseType::ConsumerCursor;
    
public:
    FBoundedCircularQueue()
        : FQueueBaseType()
    {
    }
    
    virtual ~FBoundedCircularQueue() = default;

    FBoundedCircularQueue(const FBoundedCircularQueue& other)                 = delete;
    FBoundedCircularQueue(FBoundedCircularQueue&& other) noexcept             = delete;
    FBoundedCircularQueue& operator=(const FBoundedCircularQueue& other)      = delete;
    FBoundedCircularQueue& operator=(FBoundedCircularQueue&& other) noexcept  = delete;

protected:
    class FBufferData
    {
    public:
        FElementType CircularBuffer[RoundedSize];
        CACHE_ALIGN std::atomic<EBufferNodeState> CircularBufferStates[RoundedSize];
        
    public:
        FBufferData()
            : CircularBuffer{},
            CircularBufferStates{EBufferNodeState::EMPTY}
        {
        }

        virtual ~FBufferData() = default;
        
        FBufferData(const FBufferData& other)                   = delete;
        FBufferData(FBufferData&& other) noexcept               = delete;
        FBufferData& operator=(const FBufferData& other)        = delete;
        FBufferData& operator=(FBufferData&& other) noexcept    = delete;
    };

public:
    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        if(TSPSC)
        {
            return;
        }

        const uint ThisIndex = FQueueBaseType::IncrementProducerCursor();
        const uint Index = RemapCursor<ShuffleBits>(ThisIndex & IndexMask);

        /* Likely to succeed on first iteration. */
        for(;;)
        {
            EBufferNodeState Expected = EBufferNodeState::EMPTY;
            if(BufferData.CircularBufferStates[Index].compare_exchange_strong(
                Expected, EBufferNodeState::LOADING,
                ACQUIRE, RELAXED))
            {
                BufferData.CircularBuffer[Index] = NewElement;
                BufferData.CircularBufferStates[Index].store(EBufferNodeState::EMPTY, RELEASE);
                return;
            }
        
            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while(TMaxThroughput && BufferData.CircularBufferStates[Index].load(RELAXED) != EBufferNodeState::EMPTY);
        }
    }
    
    FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            return FElementType{};
        }

        const uint ThisIndex = FQueueBaseType::IncrementConsumerCursor();
        const uint Index = RemapCursor<ShuffleBits>(ThisIndex & IndexMask);

        /* Likely to succeed on first iteration. */
        for(;;)
        {
            EBufferNodeState Expected = EBufferNodeState::FULL;
            if(BufferData.CircularBufferStates[Index].compare_exchange_strong(
                Expected, EBufferNodeState::LOADING,
                ACQUIRE, RELAXED))
            {
                FElementType Element{(std::move(BufferData.CircularBuffer[Index]))}; 
                BufferData.CircularBufferStates[Index].store(EBufferNodeState::EMPTY, RELEASE);
                return Element;
            }

            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while(TMaxThroughput && BufferData.CircularBufferStates[Index].load(RELAXED) != EBufferNodeState::FULL);
        }
    }

    FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED)
    {
        FQueueBaseType::TryPush22(NewElement);
        
        const uint CurrentProducerCursor = ProducerCursor.load(ACQUIRE);
        const uint CurrentConsumerCursor = ConsumerCursor.load(ACQUIRE);

        if(((CurrentProducerCursor) + 1)
            == (CurrentConsumerCursor))
        {
            return false;
        }

        if(TSPSC)
        {
            return false;
        }

        Push(NewElement);
    
        return true;
    }
    
    FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED)
    {
        const uint CurrentProducerCursor = ProducerCursor.load(ACQUIRE);
        const uint CurrentConsumerCursor = ConsumerCursor.load(ACQUIRE);

        if((CurrentProducerCursor)
            == (CurrentConsumerCursor))
        {
            return false;
        }

        if(TSPSC)
        {
            return false;
        }

        OutElement = Pop();
        
        return true;
    }

protected:
    CACHE_ALIGN FBufferData                 BufferData;
};
