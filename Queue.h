#pragma once

#include <cstdio>
#include <cstdlib>

#include <atomic>
// #include <functional>
// #include <mutex>
// #include <vector>

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

template<uint TArraySize, size_t TElementsPerCacheLine, bool TUnused = false>
struct GetIndexShuffleBits
{
    static int constexpr Bits = GetCacheLineIndexBits<TElementsPerCacheLine>::Value;
    static unsigned constexpr MinSize = 1U << (Bits * 2);
    static int constexpr Value = TArraySize < MinSize ? 0 : Bits;
};

template<uint TArraySize, size_t TElementsPerCacheLine>
struct GetIndexShuffleBits<TArraySize, TElementsPerCacheLine>
{
    static int constexpr Value = 0;
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

static constexpr std::memory_order ACQUIRE     = std::memory_order_acquire;
static constexpr std::memory_order RELEASE     = std::memory_order_release;
static constexpr std::memory_order RELAXED     = std::memory_order_relaxed;
static constexpr std::memory_order SEQ_CONST   = std::memory_order_seq_cst;

/**
 * @biref Common base type for creating bounded queues.
 */
template<unsigned int TQueueSize, bool TTotalOrder = true>
class FBoundedQueueCommon
{
    static constexpr std::memory_order FetchAddMemoryOrder = TTotalOrder ? SEQ_CONST : ACQUIRE;

protected:
    static constexpr uint   RoundedSize = RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize);
    static constexpr uint   IndexMask = RoundedSize - 1;
    
public:
    FBoundedQueueCommon() noexcept = default;
    
    virtual ~FBoundedQueueCommon() = default;

    FBoundedQueueCommon(const FBoundedQueueCommon& other)                 = delete;
    FBoundedQueueCommon(FBoundedQueueCommon&& other) noexcept             = delete;
    FBoundedQueueCommon& operator=(const FBoundedQueueCommon& other)      = delete;
    FBoundedQueueCommon& operator=(FBoundedQueueCommon&& other) noexcept  = delete;
    
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
    CACHE_ALIGN std::atomic<uint>   ProducerCursor = {};
    CACHE_ALIGN std::atomic<uint>   ConsumerCursor = {};
};

/**
 * @brief Base type for creating bounded circular queues.
 */
template<typename T, unsigned int TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class FBoundedCircularQueueBase : public FBoundedQueueCommon<TQueueSize, TTotalOrder>
{
protected:
    enum class EBufferNodeState : uint8
    {
        EMPTY, STORING, FULL, LOADING
    };
    
    using FQueueBaseType = FBoundedQueueCommon<TQueueSize, TTotalOrder>;
    using FElementType = T;
    
    static constexpr uint               RoundedSize = FQueueBaseType::RoundedSize;
    static constexpr int                ShuffleBits = GetIndexShuffleBits<RoundedSize,
                                            PLATFORM_CACHE_LINE_SIZE / sizeof(EBufferNodeState)>::Value;
    static constexpr uint               IndexMask = FQueueBaseType::IndexMask;
    
    static constexpr std::atomic<uint>  ProducerCursor = FQueueBaseType::ProducerCursor;
    static constexpr std::atomic<uint>  ConsumerCursor = FQueueBaseType::ConsumerCursor;
    
public:
    FBoundedCircularQueueBase()
        : FQueueBaseType()
    {
    }

protected:
    static FORCEINLINE void PushBase(const FElementType& NewElement,
        std::atomic<EBufferNodeState>& State, FElementType& QueueIndex) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            return;
        }
        
        /* Likely to succeed on first iteration. */
        for(;;)
        {
            EBufferNodeState Expected = EBufferNodeState::EMPTY;
            if(State.compare_exchange_strong(
                Expected, EBufferNodeState::LOADING,
                ACQUIRE, RELAXED))
            {
                QueueIndex = NewElement;
                State.store(EBufferNodeState::EMPTY, RELEASE);
                return;
            }
        
            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while(TMaxThroughput && State.load(RELAXED) != EBufferNodeState::EMPTY);
        }
    }

    static FORCEINLINE FElementType PopBase() noexcept(Q_NOEXCEPT_ENABLED)
    {
        return {};
    }
};

/**
 * Bounded circular queue for non-atomic elements.
 */
template<typename T, unsigned int TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
CACHE_ALIGN class FBoundedCircularQueue final : public FBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    /*
     * TODO: static_asserts
    */
    static_assert(TQueueSize > 0, "");

    using FQueueBaseType = FBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>;
    using FElementType = T;
    using EBufferNodeState = typename FQueueBaseType::EBufferNodeState;
    
    static constexpr uint                       RoundedSize = FQueueBaseType::RoundedSize;
    static constexpr int                        ShuffleBits = FQueueBaseType::ShuffleBits;
    static constexpr uint                       IndexMask = FQueueBaseType::IndexMask;
    
    static constexpr std::atomic<uint>          ProducerCursor = FQueueBaseType::ProducerCursor;
    static constexpr std::atomic<uint>          ConsumerCursor = FQueueBaseType::ConsumerCursor;

    CACHE_ALIGN FElementType                    CircularBuffer[RoundedSize];
    CACHE_ALIGN std::atomic<EBufferNodeState>   CircularBufferStates[RoundedSize];
    
public:
    FBoundedCircularQueue()
        : FQueueBaseType(),
        CircularBuffer{},
        CircularBufferStates{EBufferNodeState::EMPTY}
    {
    }
    
    ~FBoundedCircularQueue() override = default;

    FBoundedCircularQueue(const FBoundedCircularQueue& other)                 = delete;
    FBoundedCircularQueue(FBoundedCircularQueue&& other) noexcept             = delete;
    FBoundedCircularQueue& operator=(const FBoundedCircularQueue& other)      = delete;
    FBoundedCircularQueue& operator=(FBoundedCircularQueue&& other) noexcept  = delete;

public:
    FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            return;
        }

        const uint ThisIndex = FQueueBaseType::IncrementProducerCursor();
        const uint Index = RemapCursor<ShuffleBits>(ThisIndex & IndexMask);

        FQueueBaseType::PushBase(NewElement,
            CircularBufferStates[Index], CircularBuffer[Index]);
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
            if(CircularBufferStates[Index].compare_exchange_strong(
                Expected, EBufferNodeState::LOADING,
                ACQUIRE, RELAXED))
            {
                FElementType Element{(std::move(CircularBuffer[Index]))}; 
                CircularBufferStates[Index].store(EBufferNodeState::EMPTY, RELEASE);
                return Element;
            }

            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while(TMaxThroughput && CircularBufferStates[Index].load(RELAXED) != EBufferNodeState::FULL);
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
};

template<typename T, unsigned int TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class FBoundedCircularAtomicQueueBase : public FBoundedQueueCommon<TQueueSize, TTotalOrder>
{
    
};

template<typename T, unsigned int TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class FBoundedCircularAtomicQueue final : public FBoundedCircularAtomicQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    
};

#undef CACHE_ALIGN
#undef QUEUE_PADDING_BYTES
#undef SPIN_LOOP_PAUSE
