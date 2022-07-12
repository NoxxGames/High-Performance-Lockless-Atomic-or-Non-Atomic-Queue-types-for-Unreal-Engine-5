#pragma once

#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <functional>

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
    #define SPIN_LOOP_PAUSE()                _mm_pause()
    #define FORCEINLINE                      __forceinline
#else
    #if defined(__clang__) || defined(__GNUC__)
        #define SPIN_LOOP_PAUSE()            __builtin_ia32_pause()
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
template<> struct GetCacheLineIndexBits<256> { static int constexpr Value = 8; };
template<> struct GetCacheLineIndexBits<128> { static int constexpr Value = 7; };
template<> struct GetCacheLineIndexBits< 64> { static int constexpr Value = 6; };
template<> struct GetCacheLineIndexBits< 32> { static int constexpr Value = 5; };
template<> struct GetCacheLineIndexBits< 16> { static int constexpr Value = 4; };
template<> struct GetCacheLineIndexBits<  8> { static int constexpr Value = 3; };
template<> struct GetCacheLineIndexBits<  4> { static int constexpr Value = 2; };
template<> struct GetCacheLineIndexBits<  2> { static int constexpr Value = 1; };

template<uint TArraySize, size_t TElementsPerCacheLine, bool TUnused = false>
struct GetIndexShuffleBits
{
    static constexpr int Bits = GetCacheLineIndexBits<TElementsPerCacheLine>::Value;
    static constexpr uint MinSize = 1U << (Bits * 2);
    static constexpr int Value = TArraySize < MinSize ? 0 : Bits;
};

template<uint TArraySize, size_t TElementsPerCacheLine>
struct GetIndexShuffleBits<TArraySize, TElementsPerCacheLine>
{
    static constexpr int Value = 0;
};

template<uint TBits>
constexpr uint RemapCursorWithMix(const uint CursorIndex, const uint Mix)
{
    return CursorIndex ^ Mix ^ (Mix << TBits);
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
constexpr uint RemapCursor(const uint CursorIndex) noexcept
{
    return RemapCursorWithMix<TBits>(
        CursorIndex, ((CursorIndex ^ (CursorIndex >> TBits)) & ((1U << TBits) - 1)));
}

constexpr uint RemapCursor(const uint CursorIndex) noexcept
{
    return CursorIndex;
}

template<typename T, uint TBits>
constexpr T& MapElement(T* Elements, const uint CursorIndex) noexcept
{
    return Elements[RemapCursor<TBits>(CursorIndex)];
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

constexpr std::memory_order ACQUIRE     = std::memory_order_acquire;
constexpr std::memory_order RELEASE     = std::memory_order_release;
constexpr std::memory_order RELAXED     = std::memory_order_relaxed;
constexpr std::memory_order SEQ_CONST   = std::memory_order_seq_cst;

/**
 * @biref Common base type for creating bounded queues.
 */
template<typename T, uint TQueueSize, bool TTotalOrder = true>
class CACHE_ALIGN TBoundedQueueCommon
{
    /*
     * TODO: static_asserts
    */
    static_assert(TQueueSize > 0,                                              "Queue too small!");
    static_assert(TQueueSize < (1U << ((sizeof(uint) * 8) - 1)) - 1,           "Queue too large!");
    
    static constexpr std::memory_order FetchAddMemoryOrder = TTotalOrder ? SEQ_CONST : ACQUIRE;

    using FElementType      = T;
    
protected:
    static constexpr uint   RoundedSize = RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize);
    static constexpr uint   IndexMask = RoundedSize - 1;
    
public:
    TBoundedQueueCommon() noexcept = default;
    
    virtual ~TBoundedQueueCommon() noexcept = default;

    TBoundedQueueCommon(const TBoundedQueueCommon& Other) noexcept
        : ProducerCursor(Other.ProducerCursor.load(RELAXED)),
        ConsumerCursor(Other.ConsumerCursor.load(RELAXED))
    {
    }
    
    TBoundedQueueCommon& operator=(const TBoundedQueueCommon& Other) noexcept
    {
        ProducerCursor.store(Other.ProducerCursor.load(RELAXED), RELAXED);
        ConsumerCursor.store(Other.ConsumerCursor.load(RELAXED), RELAXED);
        return *this;
    }

    TBoundedQueueCommon(TBoundedQueueCommon&&) noexcept             = delete;
    TBoundedQueueCommon& operator=(TBoundedQueueCommon&&) noexcept  = delete;

    void Swap(const TBoundedQueueCommon& Other) noexcept
    {
        const uint ThisProducerCursor = ProducerCursor.load(RELAXED);
        const uint ThisConsumerCursor = ConsumerCursor.load(RELAXED);
        ProducerCursor.store(Other.ProducerCursor.load(RELAXED), RELAXED);
        ConsumerCursor.store(Other.ConsumerCursor.load(RELAXED), RELAXED);
        Other.ProducerCursor.store(ThisProducerCursor, RELAXED);
        Other.ConsumerCursor.store(ThisConsumerCursor, RELAXED);
    }
    
protected:

    
    template<bool TSPSC>
    FORCEINLINE uint IncrementProducerCursor() noexcept
    {
        if(TSPSC)
        {
            const uint Cursor = ProducerCursor.load(RELAXED);
            ProducerCursor.store(Cursor + 1, RELAXED);
            return Cursor;
        }
        
        return ProducerCursor.fetch_add(1, FetchAddMemoryOrder);
    }

    template<bool TSPSC>
    FORCEINLINE uint IncrementConsumerCursor() noexcept
    {
        if(TSPSC)
        {
            const uint Cursor = ConsumerCursor.load(RELAXED);
            ConsumerCursor.store(Cursor + 1, RELAXED);
            return Cursor;
        }
        
        return ConsumerCursor.fetch_add(1, FetchAddMemoryOrder);
    }

    FORCEINLINE bool TryPushBase(const std::function<void()>& DerivedPushFunction) noexcept
    {
        if(this->Full())
        {
            return false;
        }
        DerivedPushFunction();
        return true;
    }

    FORCEINLINE bool TryPopBase(const std::function<void()>& DerivedPopFunction) noexcept
    {
        if(this->Empty())
        {
            return false;
        }
        DerivedPopFunction();
        return true;
    }
    
public:
    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED)      = 0;
    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED)                             = 0;
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED)   = 0;
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED)          = 0;
    
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

////////////////////////////////////////////////////////////////////////////
///
///                     REGULAR QUEUE VERSIONS
///
////////////////////////////////////////////////////////////////////////////

/**
 * @brief Base type for creating bounded circular queues.
 */
template<typename T, uint TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN TBoundedCircularQueueBase : public TBoundedQueueCommon<T, TQueueSize, TTotalOrder>
{
protected:
    enum class EBufferNodeState : uint8
    {
        EMPTY, STORING, FULL, LOADING
    };
    
    using TQueueBaseType                = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using FElementType                  = T;

    static constexpr uint               TypeSize =    sizeof(FElementType);
    static constexpr uint               StateSize =   sizeof(std::atomic<EBufferNodeState>);
    static constexpr uint               RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                ShuffleBits = GetCacheLineIndexBits<
                                            PLATFORM_CACHE_LINE_SIZE / StateSize>::Value;
    static constexpr uint               IndexMask = TQueueBaseType::IndexMask;
    
public:
    TBoundedCircularQueueBase()
        : TQueueBaseType()
    {
    }

    virtual ~TBoundedCircularQueueBase() noexcept override = default;

    TBoundedCircularQueueBase(const TBoundedCircularQueueBase& Other) noexcept
    {
        TQueueBaseType::TQueueBaseType(Other);
    }
    
    TBoundedCircularQueueBase& operator=(const TBoundedCircularQueueBase& Other) noexcept
    {
        return TQueueBaseType::operator=(Other);
    }
    
    TBoundedCircularQueueBase(TBoundedCircularQueueBase&&) noexcept               = delete;
    TBoundedCircularQueueBase& operator=(TBoundedCircularQueueBase&&) noexcept    = delete;

    virtual FORCEINLINE void Push(const FElementType&NewElement) noexcept(true) override        = 0;
    virtual FORCEINLINE FElementType Pop() noexcept(true) override                              = 0;
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(true) override    = 0;
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(true) override           = 0;

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

    static FORCEINLINE FElementType PopBase(
        std::atomic<EBufferNodeState>& State, FElementType& QueueIndex) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            return {};
        }
        
        /* Likely to succeed on first iteration. */
        for(;;)
        {
            EBufferNodeState Expected = EBufferNodeState::FULL;
            if(State.compare_exchange_strong(
                Expected, EBufferNodeState::LOADING,
                ACQUIRE, RELAXED))
            {
                State.store(EBufferNodeState::EMPTY, RELEASE);
                return QueueIndex;
            }

            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while(TMaxThroughput && State.load(RELAXED) != EBufferNodeState::FULL);
        }
    }
};

/**
 * Bounded circular queue for non-atomic elements.
 */
template<typename T, uint TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN TBoundedCircularQueue : public TBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    using TQueueBaseTypeCommon  = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using TQueueBaseType        = TBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>;
    using FElementType          = T;
    using EBufferNodeState      = typename TQueueBaseType::EBufferNodeState;
    
    static constexpr uint                       RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                        ShuffleBits = TQueueBaseType::ShuffleBits;
    static constexpr uint                       IndexMask = TQueueBaseType::IndexMask;

    CACHE_ALIGN FElementType                    CircularBuffer[RoundedSize];
    CACHE_ALIGN std::atomic<EBufferNodeState>   CircularBufferStates[RoundedSize];

public:
    TBoundedCircularQueue()
        : TQueueBaseType(),
        CircularBuffer{},
        CircularBufferStates{EBufferNodeState::EMPTY}
    {
    }
    
    virtual ~TBoundedCircularQueue() noexcept override = default; 

    TBoundedCircularQueue(const TBoundedCircularQueue&) noexcept                  = delete;
    TBoundedCircularQueue(TBoundedCircularQueue&&) noexcept                       = delete;
    TBoundedCircularQueue& operator=(const TBoundedCircularQueue&) noexcept       = delete;
    TBoundedCircularQueue& operator=(TBoundedCircularQueue&&) noexcept            = delete;
    
    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementProducerCursor<TSPSC>();
        const uint Index = RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        TQueueBaseType::PushBase(NewElement,
            CircularBufferStates[Index], CircularBuffer[Index]);
    }
    
    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementConsumerCursor<TSPSC>();
        const uint Index = RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        return TQueueBaseType::PopBase(
            CircularBufferStates[Index], CircularBuffer[Index]);
    }
    
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPushBase([&](){ Push(NewElement); });
    }
    
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPopBase([&](){ OutElement = Pop(); });
    }
};

template<typename T, uint TQueueSize, T TNil = T{}, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN TBoundedCircularQueueHeap : public TBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    using TQueueBaseTypeCommon  = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using TQueueBaseType        = TBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>;
    using FElementType          = T;
    using EBufferNodeState      = typename TQueueBaseType::EBufferNodeState;
    
    static constexpr uint                       RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                        ShuffleBits = TQueueBaseType::ShuffleBits;
    static constexpr uint                       IndexMask = TQueueBaseType::IndexMask;
    
    CACHE_ALIGN FElementType                    *CircularBuffer;
    CACHE_ALIGN std::atomic<EBufferNodeState>   *CircularBufferStates;
    
public:
    TBoundedCircularQueueHeap()
        : TQueueBaseType(),
        CircularBuffer(static_cast<FElementType*>(
            calloc(TQueueSize, sizeof(FElementType)))),
        CircularBufferStates(static_cast<std::atomic<EBufferNodeState>*>(
            calloc(TQueueSize, sizeof(std::atomic<EBufferNodeState>))))
    {
        for(uint i = 0; i < TQueueSize; ++i)
        {
            CircularBuffer[i]       = TNil;
            CircularBufferStates[i] = EBufferNodeState::Empty;
        }
    }

    virtual ~TBoundedCircularQueueHeap() noexcept override
    {
        if(CircularBuffer)
        {
            free(CircularBuffer);
        }
        if(CircularBufferStates)
        {
            free(CircularBufferStates);
        }
    }

    TBoundedCircularQueueHeap(const TBoundedCircularQueueHeap& other)                   = delete;
    TBoundedCircularQueueHeap(TBoundedCircularQueueHeap&& other) noexcept               = delete;
    TBoundedCircularQueueHeap& operator=(const TBoundedCircularQueueHeap& other)        = delete;
    TBoundedCircularQueueHeap& operator=(TBoundedCircularQueueHeap&& other) noexcept    = delete;

    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementProducerCursor<TSPSC>();
        const uint Index = RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        TQueueBaseType::PushBase(NewElement,
            CircularBufferStates[Index], CircularBuffer[Index]);
    }

    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementConsumerCursor<TSPSC>();
        const uint Index = RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        return TQueueBaseType::PopBase(
            CircularBufferStates[Index], CircularBuffer[Index]);
    }
    
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseType::TryPushBase([&](){ Push(NewElement); });
    }
    
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPopBase([&](){ OutElement = Pop(); });
    }
};

//////////////////////// END REGULAR QUEUE VERSIONS ////////////////////////

////////////////////////////////////////////////////////////////////////////
///
///                     ATOMIC QUEUE VERSIONS
///
////////////////////////////////////////////////////////////////////////////

template<typename T, uint TQueueSize, T TNil = T{}, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN TBoundedCircularAtomicQueueBase : public TBoundedQueueCommon<T, TQueueSize, TTotalOrder>
{
    using TQueueBaseType                = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using FElementType                  = T;

    static constexpr uint               TypeSize =    sizeof(std::atomic<FElementType>);
    static constexpr uint               RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                ShuffleBits = GetCacheLineIndexBits<
                                            PLATFORM_CACHE_LINE_SIZE / TypeSize>::Value;
    static constexpr uint               IndexMask = TQueueBaseType::IndexMask;

    // CACHE_ALIGN std::atomic<FElementType>            CircularBuffer[RoundedSize];
    
public:
    using TElementType = FElementType;
    
    TBoundedCircularAtomicQueueBase() noexcept
        : TQueueBaseType()
    {
        assert(std::atomic<FElementType>{TNil}.is_lock_free());
        //if(FElementType{} != TNil)
        //{
        //    for(uint i = 0; i < RoundedSize; ++i)
        //    {
         //       CircularBuffer[i].store(TNil, RELAXED);
        //    }
        //}
    }
    
    virtual ~TBoundedCircularAtomicQueueBase() noexcept override = default;
    
    TBoundedCircularAtomicQueueBase(TBoundedCircularAtomicQueueBase&) noexcept               = delete;
    TBoundedCircularAtomicQueueBase& operator=(TBoundedCircularAtomicQueueBase&) noexcept    = delete;

    virtual FORCEINLINE void Push(const FElementType&NewElement) noexcept(true) override        = 0;
    virtual FORCEINLINE FElementType Pop() noexcept(true) override                              = 0;
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(true) override    = 0;
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(true) override           = 0;

protected:
    static FORCEINLINE void PushBase(const FElementType& NewElement,
        std::atomic<FElementType>& QueueIndex) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            return;
        }

        
        for(;;)
        {
            FElementType Expected = TNil;
            if(QueueIndex.compare_exchange_strong(Expected, NewElement,
                RELEASE, RELAXED))
            {
                return;
            }

            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while (TMaxThroughput && QueueIndex.load(RELAXED) != TNil);
        }
    }

    static FORCEINLINE FElementType PopBase(std::atomic<FElementType>& QueueIndex) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            return {};
        }

        for(;;)
        {
            FElementType Element = QueueIndex.exchange(TNil, RELEASE);
            if(Element != TNil)
            {
                return Element;
            }

            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while (TMaxThroughput && QueueIndex.load(RELAXED) == TNil);
        }
    }
};

/*
template<typename T, uint TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN FBoundedCircularAtomicQueue : public FBoundedCircularAtomicQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{

};

template<typename T, uint TQueueSize, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN FBoundedCircularAtomicQueueHeap : public FBoundedCircularAtomicQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{

};

//////////////////////// END ATOMIC QUEUE VERSIONS //////////////////////////

*/

#undef CACHE_ALIGN
#undef QUEUE_PADDING_BYTES
#undef SPIN_LOOP_PAUSE
#undef Q_NOEXCEPT_ENABLED
