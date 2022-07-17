#pragma once

// #include <cstdio>
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

#define CACHE_ALIGN alignas(PLATFORM_CACHE_LINE_SIZE)
#define Q_NOEXCEPT_ENABLED true

namespace AtomicQueue
{
    namespace Utils
    {
        template<uint TElementsPerCacheLine> struct GetCacheLineIndexBits { static int constexpr value = 0; };
        template<> struct GetCacheLineIndexBits<256> { static int constexpr Value = 8; };
        template<> struct GetCacheLineIndexBits<128> { static int constexpr Value = 7; };
        template<> struct GetCacheLineIndexBits< 64> { static int constexpr Value = 6; };
        template<> struct GetCacheLineIndexBits< 32> { static int constexpr Value = 5; };
        template<> struct GetCacheLineIndexBits< 16> { static int constexpr Value = 4; };
        template<> struct GetCacheLineIndexBits<  8> { static int constexpr Value = 3; };
        template<> struct GetCacheLineIndexBits<  4> { static int constexpr Value = 2; };
        template<> struct GetCacheLineIndexBits<  2> { static int constexpr Value = 1; };

        template<uint TArraySize, uint TElementsPerCacheLine, bool TUnused = false>
        struct GetIndexShuffleBits
        {
            static constexpr int Bits = GetCacheLineIndexBits<TElementsPerCacheLine>::Value;
            static constexpr uint MinSize = 1U << (Bits * 2);
            static constexpr int Value = TArraySize < MinSize ? 0 : Bits;
        };

        template<uint TArraySize, uint TElementsPerCacheLine>
        struct GetIndexShuffleBits<TArraySize, TElementsPerCacheLine>
        {
            static constexpr int Value = 0;
        };

        template<uint TBits>
        constexpr uint RemapCursorWithMix(const uint CursorIndex, const uint Mix) noexcept
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
    } // namespace Utils

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
    
    static constexpr std::memory_order FetchAddMemoryOrder = TTotalOrder ? Utils::SEQ_CONST : Utils::ACQUIRE;

    using FElementType      = T;

    
protected:
    static constexpr uint   RoundedSize = Utils::RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize);
    static constexpr uint   IndexMask = RoundedSize - 1;
    
public:
    TBoundedQueueCommon(const uint InProducerCursor = 0, const uint InConsumerCursor = 0) noexcept(Q_NOEXCEPT_ENABLED)
        : ProducerCursor{InProducerCursor},
        ConsumerCursor{InConsumerCursor}
    {
    }
    
    virtual ~TBoundedQueueCommon() noexcept(Q_NOEXCEPT_ENABLED) = default;

    TBoundedQueueCommon(const TBoundedQueueCommon& Other) noexcept(Q_NOEXCEPT_ENABLED)
        : ProducerCursor(Other.ProducerCursor.load(Utils::RELAXED)),
        ConsumerCursor(Other.ConsumerCursor.load(Utils::RELAXED))
    {
    }
    
    TBoundedQueueCommon& operator=(const TBoundedQueueCommon& Other) noexcept(Q_NOEXCEPT_ENABLED)
    {
        ProducerCursor.store(Other.ProducerCursor.load(Utils::RELAXED), Utils::RELAXED);
        ConsumerCursor.store(Other.ConsumerCursor.load(Utils::RELAXED), Utils::RELAXED);
        return *this;
    }

    void Swap(const TBoundedQueueCommon& Other) noexcept(Q_NOEXCEPT_ENABLED)
    {
        const uint ThisProducerCursor = ProducerCursor.load(Utils::RELAXED);
        const uint ThisConsumerCursor = ConsumerCursor.load(Utils::RELAXED);
        ProducerCursor.store(Other.ProducerCursor.load(Utils::RELAXED), Utils::RELAXED);
        ConsumerCursor.store(Other.ConsumerCursor.load(Utils::RELAXED), Utils::RELAXED);
        Other.ProducerCursor.store(ThisProducerCursor, Utils::RELAXED);
        Other.ConsumerCursor.store(ThisConsumerCursor, Utils::RELAXED);
    }
    
protected:
    template<bool TSPSC>
    FORCEINLINE uint IncrementProducerCursor() noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            const uint Cursor = ProducerCursor.load(Utils::RELAXED);
            ProducerCursor.store(Cursor + 1, Utils::RELAXED);
            return Cursor;
        }
        
        return ProducerCursor.fetch_add(1, FetchAddMemoryOrder);
    }

    template<bool TSPSC>
    FORCEINLINE uint IncrementConsumerCursor() noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            const uint Cursor = ConsumerCursor.load(Utils::RELAXED);
            ConsumerCursor.store(Cursor + 1, Utils::RELAXED);
            return Cursor;
        }
        
        return ConsumerCursor.fetch_add(1, FetchAddMemoryOrder);
    }

    FORCEINLINE bool TryPushBase(const std::function<void()>& DerivedPushFunction) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(this->Full())
        {
            return false;
        }
        DerivedPushFunction();
        return true;
    }

    FORCEINLINE bool TryPopBase(const std::function<void()>& DerivedPopFunction) noexcept(Q_NOEXCEPT_ENABLED)
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
        const uint CurrentProducerCursor = ProducerCursor.load(Utils::RELAXED);
        const uint CurrentConsumerCursor = ConsumerCursor.load(Utils::RELAXED);

        return (CurrentProducerCursor + 1) == CurrentConsumerCursor;
    }
    
    FORCEINLINE bool Empty() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        const uint CurrentProducerCursor = ProducerCursor.load(Utils::RELAXED);
        const uint CurrentConsumerCursor = ConsumerCursor.load(Utils::RELAXED);

        return CurrentProducerCursor == CurrentConsumerCursor;
    }

    FORCEINLINE uint Num() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        // tail_ can be greater than head_ because of consumers doing pop, rather that try_pop, when the queue is empty.
        const int64 Difference = ProducerCursor.load(Utils::RELAXED) - ConsumerCursor.load(Utils::RELAXED);
        
        return Difference > 0 ? Difference : 0;
    }

protected:
    CACHE_ALIGN std::atomic<uint>   ProducerCursor;
    CACHE_ALIGN std::atomic<uint>   ConsumerCursor;
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

    static constexpr uint               TypeSize = sizeof(FElementType);
    static constexpr uint               StateSize = sizeof(std::atomic<EBufferNodeState>);
    static constexpr uint               RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr uint               IndexMask = TQueueBaseType::IndexMask;
    
public:
    TBoundedCircularQueueBase() noexcept
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
    

    virtual FORCEINLINE void Push(const FElementType&NewElement) noexcept(Q_NOEXCEPT_ENABLED) override        = 0;
    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED) override                              = 0;
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override    = 0;
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED) override           = 0;

protected:
    static FORCEINLINE void PushBase(const FElementType& NewElement,
        std::atomic<EBufferNodeState>& State, FElementType& QueueIndex) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            while(State.load(Utils::ACQUIRE) != EBufferNodeState::EMPTY)
            {
                if(TMaxThroughput)
                {
                    SPIN_LOOP_PAUSE();
                }
            }
            QueueIndex = NewElement;
            State.store(EBufferNodeState::FULL, Utils::RELEASE);
        }
        
        /* Likely to succeed on first iteration. */
        for(;;)
        {
            EBufferNodeState Expected = EBufferNodeState::EMPTY;
            if(State.compare_exchange_strong(
                Expected, EBufferNodeState::STORING,
                Utils::ACQUIRE, Utils::RELAXED))
            {
                QueueIndex = NewElement;
                State.store(EBufferNodeState::FULL, Utils::RELEASE);
                return;
            }
        
            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while(TMaxThroughput && State.load(Utils::RELAXED) != EBufferNodeState::EMPTY);
        }
    }

    static FORCEINLINE FElementType PopBase(
        std::atomic<EBufferNodeState>& State, FElementType& QueueIndex) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            while(State.load(Utils::ACQUIRE) != EBufferNodeState::FULL)
            {
                if(TMaxThroughput)
                {
                    SPIN_LOOP_PAUSE();
                }
            }
            const FElementType Element = QueueIndex;
            State.store(EBufferNodeState::EMPTY, Utils::RELEASE);
            return Element;
        }
        
        /* Likely to succeed on first iteration. */
        for(;;)
        {
            EBufferNodeState Expected = EBufferNodeState::FULL;
            if(State.compare_exchange_strong(
                Expected, EBufferNodeState::LOADING,
                Utils::ACQUIRE, Utils::RELAXED))
            {
                State.store(EBufferNodeState::EMPTY, Utils::RELEASE);
                return QueueIndex;
            }

            // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
            do
            {
                SPIN_LOOP_PAUSE();
            }
            while(TMaxThroughput && State.load(Utils::RELAXED) != EBufferNodeState::FULL);
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

    static constexpr uint                       TypeSize = TQueueBaseType::TypeSize;
    static constexpr uint                       StateSize = TQueueBaseType::StateSize;
    static constexpr uint                       RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                        ShuffleBits = Utils::GetIndexShuffleBits<TQueueSize,
                                                    PLATFORM_CACHE_LINE_SIZE / StateSize>::Value;
    static constexpr uint                       IndexMask = TQueueBaseType::IndexMask;

    CACHE_ALIGN FElementType                    CircularBuffer[RoundedSize];
    CACHE_ALIGN std::atomic<EBufferNodeState>   CircularBufferStates[RoundedSize];

public:
    TBoundedCircularQueue() noexcept
        : TQueueBaseType(),
        CircularBuffer{},
        CircularBufferStates{EBufferNodeState::EMPTY}
    {
    }
    
    virtual ~TBoundedCircularQueue() noexcept override = default; 

    TBoundedCircularQueue(const TBoundedCircularQueue&) noexcept(Q_NOEXCEPT_ENABLED)                  = delete;
    TBoundedCircularQueue& operator=(const TBoundedCircularQueue&) noexcept(Q_NOEXCEPT_ENABLED)       = delete;
    
    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementProducerCursor<TSPSC>();
        const uint Index = Utils::RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        TQueueBaseType::PushBase(NewElement, CircularBufferStates[Index], CircularBuffer[Index]);
    }
    
    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementConsumerCursor<TSPSC>();
        const uint Index = Utils::RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        return TQueueBaseType::PopBase(CircularBufferStates[Index], CircularBuffer[Index]);
    }
    
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPushBase([this, &NewElement](){ Push(NewElement); });
    }
    
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPopBase([this, &OutElement](){ OutElement = Pop(); });
    }
};

template<typename T, uint TQueueSize, T TNil = T{}, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN TBoundedCircularQueueHeap : public TBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    using TQueueBaseTypeCommon  = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using TQueueBaseType        = TBoundedCircularQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>;
    using FElementType          = T;
    using EBufferNodeState      = typename TQueueBaseType::EBufferNodeState;

    static constexpr uint                       TypeSize = TQueueBaseType::TypeSize;
    static constexpr uint                       StateSize = TQueueBaseType::StateSize;
    static constexpr uint                       RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                        ShuffleBits = Utils::GetCacheLineIndexBits<
                                                    PLATFORM_CACHE_LINE_SIZE / StateSize>::Value;
    static constexpr uint                       IndexMask = TQueueBaseType::IndexMask;
    
    CACHE_ALIGN FElementType                    *CircularBuffer;
    CACHE_ALIGN std::atomic<EBufferNodeState>   *CircularBufferStates;
    
public:
    TBoundedCircularQueueHeap() noexcept
        : TQueueBaseType(),
        CircularBuffer(static_cast<FElementType*>(
            calloc(RoundedSize, sizeof(FElementType)))),
        CircularBufferStates(static_cast<std::atomic<EBufferNodeState>*>(
            calloc(RoundedSize, sizeof(std::atomic<EBufferNodeState>))))
    {
        for(uint i = 0; i < RoundedSize; ++i)
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
    TBoundedCircularQueueHeap& operator=(const TBoundedCircularQueueHeap& other)        = delete;

    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementProducerCursor<TSPSC>();
        const uint Index = Utils::RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        TQueueBaseType::PushBase(NewElement, CircularBufferStates[Index], CircularBuffer[Index]);
    }

    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementConsumerCursor<TSPSC>();
        const uint Index = Utils::RemapCursor<ShuffleBits>(ThisIndex & IndexMask);
        return TQueueBaseType::PopBase(CircularBufferStates[Index], CircularBuffer[Index]);
    }
    
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPushBase([this, &NewElement](){ Push(NewElement); });
    }
    
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPopBase([this, &OutElement](){ OutElement = Pop(); });
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
protected:
    using TQueueBaseType                = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using FElementType                  = T;

    static constexpr uint               TypeSize = sizeof(std::atomic<FElementType>);
    static constexpr uint               RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr uint               IndexMask = TQueueBaseType::IndexMask;
    
public:
    using TElementType = FElementType;
    
    TBoundedCircularAtomicQueueBase() noexcept
        : TQueueBaseType()
    {
        assert(std::atomic<FElementType>{TNil}.is_lock_free());
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
            while(QueueIndex.load(Utils::RELAXED) != TNil)
            {
                if(TMaxThroughput)
                {
                    SPIN_LOOP_PAUSE();
                }
            }
            QueueIndex.store(NewElement, Utils::RELEASE);
        }
        else
        {
            for(;;)
            {
                FElementType Expected = TNil;
                if(QueueIndex.compare_exchange_strong(Expected, NewElement,
                    Utils::RELEASE, Utils::RELAXED))
                {
                    return;
                }

                // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
                do
                {
                    SPIN_LOOP_PAUSE();
                }
                while (TMaxThroughput && QueueIndex.load(Utils::RELAXED) != TNil);
            }
        }
    }

    static FORCEINLINE FElementType PopBase(std::atomic<FElementType>& QueueIndex) noexcept(Q_NOEXCEPT_ENABLED)
    {
        if(TSPSC)
        {
            for(;;)
            {
                FElementType Element = QueueIndex.load(Utils::RELAXED);
                if(Element != TNil)
                {
                    QueueIndex.store(TNil, Utils::RELEASE);
                    return Element;
                }
                if(TMaxThroughput)
                {
                    SPIN_LOOP_PAUSE();
                }
            }
        }
        else
        {
            for(;;)
            {
                FElementType Element = QueueIndex.exchange(TNil, Utils::RELEASE);
                if(Element != TNil)
                {
                    return Element;
                }

                // Do speculative loads while busy-waiting to avoid broadcasting RFO messages.
                do
                {
                    SPIN_LOOP_PAUSE();
                }
                while (TMaxThroughput && QueueIndex.load(Utils::RELAXED) == TNil);
            }
        }
    }
};

template<typename T, uint TQueueSize, T TNil = T{}, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN TBoundedCircularAtomicQueue : public TBoundedCircularAtomicQueueBase<T, TNil, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    using TQueueBaseTypeCommon  = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using TQueueBaseType        = TBoundedCircularAtomicQueueBase<T, TQueueSize, TNil, TTotalOrder, TMaxThroughput, TSPSC>;
    using FElementType          = T;

    static constexpr uint                       TypeSize = TQueueBaseType::TypeSize;
    static constexpr uint                       RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                        ShuffleBits = Utils::GetIndexShuffleBits<RoundedSize,
                                                    PLATFORM_CACHE_LINE_SIZE / TypeSize>::Value;
    static constexpr uint                       IndexMask = TQueueBaseType::IndexMask;

    CACHE_ALIGN std::atomic<FElementType>       CircularBuffer[RoundedSize];

public:
    TBoundedCircularAtomicQueue() noexcept
        : TQueueBaseType()
    {
        if(FElementType{} != TNil)
        {
            for(uint i = 0; i < RoundedSize; ++i)
            {
               CircularBuffer[i].store(TNil, Utils::RELAXED);
            }
        }
    }

    virtual ~TBoundedCircularAtomicQueue() noexcept override = default;

    TBoundedCircularAtomicQueue(const TBoundedCircularAtomicQueue& other)                   = delete;
    TBoundedCircularAtomicQueue& operator=(const TBoundedCircularAtomicQueue& other)        = delete;

    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementProducerCursor<TSPSC>();
        std::atomic<FElementType>& Element = Utils::MapElement<FElementType, ShuffleBits>(CircularBuffer, ThisIndex & IndexMask);
        TQueueBaseType::PushBase(NewElement, Element);
    }
    
    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementConsumerCursor<TSPSC>();
        std::atomic<FElementType>& Element = Utils::MapElement<FElementType, ShuffleBits>(CircularBuffer, ThisIndex & IndexMask);
        return TQueueBaseType::PopBase(Element);
    }
    
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPush([this, &NewElement](){ Push(NewElement); });
    }
    
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPop([this, &OutElement](){ OutElement = Pop(); });
    }
};


template<typename T, uint TQueueSize, T TNil = T{}, bool TTotalOrder = true, bool TMaxThroughput = true, bool TSPSC = false>
class CACHE_ALIGN TBoundedCircularAtomicQueueHeap : public TBoundedCircularAtomicQueueBase<T, TQueueSize, TTotalOrder, TMaxThroughput, TSPSC>
{
    using TQueueBaseTypeCommon  = TBoundedQueueCommon<T, TQueueSize, TTotalOrder>;
    using TQueueBaseType        = TBoundedCircularAtomicQueueBase<T, TQueueSize, TNil, TTotalOrder, TMaxThroughput, TSPSC>;
    using FElementType          = T;

    static constexpr uint                       TypeSize = TQueueBaseType::TypeSize;
    static constexpr uint                       RoundedSize = TQueueBaseType::RoundedSize;
    static constexpr int                        ShuffleBits = Utils::GetIndexShuffleBits<RoundedSize,
                                                    PLATFORM_CACHE_LINE_SIZE / TypeSize>::Value;
    static constexpr uint                       IndexMask = TQueueBaseType::IndexMask;

    CACHE_ALIGN std::atomic<FElementType>       *CircularBuffer;

public:
    TBoundedCircularAtomicQueueHeap() noexcept
        : TQueueBaseType(),
        CircularBuffer(static_cast<std::atomic<FElementType>*>(
            calloc(RoundedSize, TypeSize)))
    {
        for(uint i = 0; i < RoundedSize; ++i)
        {
            CircularBuffer[i].store(TNil, Utils::RELAXED);
        }
    }
    
    virtual ~TBoundedCircularAtomicQueueHeap() noexcept override
    {
        if(CircularBuffer)
        {
            free(CircularBuffer);
        }
    }

    TBoundedCircularAtomicQueueHeap(const TBoundedCircularAtomicQueueHeap& other)                   = delete;
    TBoundedCircularAtomicQueueHeap& operator=(const TBoundedCircularAtomicQueueHeap& other)        = delete;
    
    virtual FORCEINLINE void Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementProducerCursor<TSPSC>();
        std::atomic<FElementType>& Element = Utils::MapElement<FElementType, ShuffleBits>(CircularBuffer, ThisIndex & IndexMask);
        TQueueBaseType::PushBase(NewElement, Element);
    }
    
    virtual FORCEINLINE FElementType Pop() noexcept(Q_NOEXCEPT_ENABLED) override
    {
        const uint ThisIndex = TQueueBaseType::template IncrementConsumerCursor<TSPSC>();
        std::atomic<FElementType>& Element = Utils::MapElement<FElementType, ShuffleBits>(CircularBuffer, ThisIndex & IndexMask);
        return TQueueBaseType::PopBase(Element);
    }
    
    virtual FORCEINLINE bool TryPush(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPush([this, &NewElement](){ Push(NewElement); });
    }
    
    virtual FORCEINLINE bool TryPop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED) override
    {
        return TQueueBaseTypeCommon::TryPop([this, &OutElement](){ OutElement = Pop(); });
    }
};
    
} // AtomicQueue namespace

//////////////////////// END ATOMIC QUEUE VERSIONS //////////////////////////


#undef CACHE_ALIGN
#undef QUEUE_PADDING_BYTES
#undef SPIN_LOOP_PAUSE
#undef Q_NOEXCEPT_ENABLED
