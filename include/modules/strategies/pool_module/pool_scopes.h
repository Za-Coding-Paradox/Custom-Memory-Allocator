#pragma once

#include <modules/allocator_registry.h>

namespace Allocator {

template <size_t TSize> struct BucketScope
{
    static constexpr size_t g_BucketSize = TSize;
    static constexpr size_t g_Alignment = TSize;

    static constexpr bool SupportsHandles = true;
    static constexpr bool IsRewindable = false;

    static constexpr bool IsPool = true;
};

template <size_t TSize> struct LargeBlockScope
{
    static constexpr size_t g_BucketSize = TSize;

    static constexpr size_t g_Alignment = 64;

    static constexpr bool SupportsHandles = true;
    static constexpr bool IsRewindable = false;

    static constexpr bool IsPool = false;
};

template <size_t TSize> struct PoolMap
{
    using Type = std::conditional_t<
        TSize <= 16, BucketScope<16>,
        std::conditional_t<
            TSize <= 32, BucketScope<32>,
            std::conditional_t<TSize <= 64, BucketScope<64>,
                               std::conditional_t<TSize <= 128, BucketScope<128>,
                                                  std::conditional_t<TSize <= 256, BucketScope<256>,
                                                                     LargeBlockScope<TSize>>>>>>;
};

template <typename TContext> struct PoolScope : public PoolMap<sizeof(TContext)>::Type
{
    using ObjectType = TContext;
    static constexpr bool g_IsTrivial = std::is_trivial_v<TContext>;

    using BaseScope = typename PoolMap<sizeof(TContext)>::Type;

    static constexpr bool SupportsHandles = BaseScope::SupportsHandles;
    static constexpr bool IsRewindable = BaseScope::IsRewindable;
    static constexpr size_t g_BucketSize = BaseScope::g_BucketSize;

    static constexpr bool IsPool = BaseScope::IsPool;
};

} // namespace Allocator
