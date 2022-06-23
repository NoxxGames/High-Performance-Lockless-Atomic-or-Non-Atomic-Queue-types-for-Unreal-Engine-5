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

typedef uint32 uint;

#define PLATFORM_CACHE_LINE_SIZE 64

#if defined(_MSC_VER)
    #define HARDWARE_PAUSE()                _mm_pause();
    #define FORCEINLINE                     __forceinline
#else
    #if defined(__clang__) || defined(__GNUC__)
        #define HARDWARE_PAUSE()            __builtin_ia32_pause();
    #else
        #define HARDWARE_PAUSE()            std::this_thread::yield()
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

template<size_t elements_per_cache_line> struct GetCacheLineIndexBits { static int constexpr value = 0; };
template<> struct GetCacheLineIndexBits<256> { static int constexpr value = 8; };
template<> struct GetCacheLineIndexBits<128> { static int constexpr value = 7; };
template<> struct GetCacheLineIndexBits< 64> { static int constexpr value = 6; };
template<> struct GetCacheLineIndexBits< 32> { static int constexpr value = 5; };
template<> struct GetCacheLineIndexBits< 16> { static int constexpr value = 4; };
template<> struct GetCacheLineIndexBits<  8> { static int constexpr value = 3; };
template<> struct GetCacheLineIndexBits<  4> { static int constexpr value = 2; };
template<> struct GetCacheLineIndexBits<  2> { static int constexpr value = 1; };

template<bool TBool, uint array_size, size_t elements_per_cache_line>
struct GetIndexShuffleBits {
    static int constexpr bits = GetCacheLineIndexBits<elements_per_cache_line>::value;
    static unsigned constexpr min_size = 1U << (bits * 2);
    static int constexpr value = array_size < min_size ? 0 : bits;
};

template<uint array_size, size_t elements_per_cache_line>
struct GetIndexShuffleBits<false, array_size, elements_per_cache_line> {
    static int constexpr value = 0;
};

template<uint TBits>
constexpr uint64 RemapCursorWithMix(const uint64 Index, const uint64 Mix)
{
    return Index ^ Mix ^ (Mix << TBits);
}

/**
 * @cite https://graphics.stanford.edu/~seander/bithacks.html#SwappingBitsXOR
 */
template<uint TBits>
constexpr uint64 RemapCursor(const uint64 Index) noexcept
{
    return RemapCursorWithMix<TBits>(Index, ((Index ^ (Index >> TBits)) & ((1U << TBits) - 1)));
}

template<>
constexpr uint64 RemapCursor<0>(const uint64 Index) noexcept
{
    return Index;
}

constexpr uint64 RoundQueueSizeUpToNearestPowerOfTwo(const uint64 QueueSize)
{
    uint64 N = QueueSize;

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

template<typename T, uint64 TQueueSize, bool TSPSC = false, typename TIntegerType = uint64, bool TTotalOrder = true>
class FBoundedQueueBenchmarking
{
    /*
     * TODO: static_asserts
    */
    static_assert(TQueueSize > 0, "");
    
    enum class EBufferNodeState : uint8
    {
        EMPTY,
        STORING,
        FULL,
        LOADING
    };
    
    using FElementType = T;
    using FIntegerType = uint64;
    
    static constexpr TIntegerType RoundedSize = RoundQueueSizeUpToNearestPowerOfTwo(TQueueSize);
    static constexpr int ShuffleBits = GetIndexShuffleBits<
        false, RoundedSize, PLATFORM_CACHE_LINE_SIZE / sizeof(EBufferNodeState)>::value;
    static constexpr TIntegerType IndexMask = RoundedSize - 1;
    
public:
    FBoundedQueueBenchmarking()
        : ProducerCursor{0},
        ConsumerCursor{0}
    {
    }
    
    ~FBoundedQueueBenchmarking() = default;

    FBoundedQueueBenchmarking(const FBoundedQueueBenchmarking& other)                 = delete;
    FBoundedQueueBenchmarking(FBoundedQueueBenchmarking&& other) noexcept             = delete;
    FBoundedQueueBenchmarking& operator=(const FBoundedQueueBenchmarking& other)      = delete;
    FBoundedQueueBenchmarking& operator=(FBoundedQueueBenchmarking&& other) noexcept  = delete;

protected:
    class FBufferData
    {
    public:
        FElementType CircularBuffer[RoundedSize];
        CACHE_ALIGN EBufferNodeState CircularBufferStates[RoundedSize];
        
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
    FORCEINLINE bool Push(const FElementType& NewElement) noexcept(Q_NOEXCEPT_ENABLED)
    {
        const FIntegerType CurrentProducerCursor = ProducerCursor.load(std::memory_order_acquire);
        const FIntegerType CurrentConsumerCursor = ConsumerCursor.load(std::memory_order_acquire);

        if(((CurrentProducerCursor) + 1)
            == (CurrentConsumerCursor))
        {
            return false;
        }

        const FIntegerType ThisIndex = ProducerCursor.fetch_add(1, std::memory_order_acquire);

        const uint64 Index = RemapCursor<ShuffleBits>(ThisIndex & BufferData.IndexMask);
        BufferData.CircularBuffer[Index] = std::move(NewElement);
        
        return true;
    }
    
    FORCEINLINE bool Pop(FElementType& OutElement) noexcept(Q_NOEXCEPT_ENABLED)
    {
        const FIntegerType CurrentProducerCursor = ProducerCursor.load(std::memory_order_acquire);
        const FIntegerType CurrentConsumerCursor = ConsumerCursor.load(std::memory_order_acquire);

        if((CurrentProducerCursor)
            == (CurrentConsumerCursor))
        {
            return false;
        }

        const FIntegerType ThisIndex = ConsumerCursor.fetch_add(1, std::memory_order_acquire);
        
        const uint64 Index = RemapCursor<ShuffleBits>(ThisIndex & BufferData.IndexMask);
        OutElement = std::forward<FElementType>(BufferData.CircularBuffer[Index]);
        
        return true;
    }
    
    FORCEINLINE uint64 Size() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        return RoundedSize;
    }

    FORCEINLINE bool Full() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        const FIntegerType CurrentProducerCursor = ProducerCursor.load(std::memory_order_acquire);
        const FIntegerType CurrentConsumerCursor = ConsumerCursor.load(std::memory_order_acquire);

        if(((CurrentProducerCursor) + 1)
            == (CurrentConsumerCursor))
        {
            return true;
        }

        return false;
    }
    
    FORCEINLINE bool Empty() const noexcept(Q_NOEXCEPT_ENABLED)
    {
        const FIntegerType CurrentProducerCursor = ProducerCursor.load(std::memory_order_acquire);
        const FIntegerType CurrentConsumerCursor = ConsumerCursor.load(std::memory_order_acquire);

        if((CurrentProducerCursor)
            == (CurrentConsumerCursor))
        {
            return true;
        }

        return false;
    } 

protected:
    CACHE_ALIGN FBufferData BufferData;
    
    CACHE_ALIGN std::atomic<FIntegerType>   ProducerCursor;
    CACHE_ALIGN std::atomic<FIntegerType>   ConsumerCursor;
};
