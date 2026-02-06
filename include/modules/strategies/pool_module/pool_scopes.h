#pragma once

#include <modules/allocator_registry.h>
#include <type_traits>

namespace Allocator {

template <size_t TSize> struct BucketScope
{
    static constexpr size_t g_BucketSize = TSize;
    static constexpr size_t g_Alignment = TSize;

    static constexpr bool SupportsHandles = true;
    static constexpr bool IsRewindable = false;
};

template <size_t TSize> struct PoolMap
{
    using Type = std::conditional_t<
        TSize <= 16, BucketScope<16>,
        std::conditional_t<TSize <= 32, BucketScope<32>,
                           std::conditional_t<TSize <= 64, BucketScope<64>,
                                              std::conditional_t<TSize <= 128, BucketScope<128>,
                                                                 BucketScope<256>>>>>;
};

template <typename TContext> struct PoolScope : public PoolMap<sizeof(TContext)>::Type
{
    using ObjectType = TContext;
    static constexpr bool g_IsTrivial = std::is_trivial_v<TContext>;

    static constexpr bool SupportsHandles = PoolMap<sizeof(TContext)>::Type::SupportsHandles;
    static constexpr bool IsRewindable = PoolMap<sizeof(TContext)>::Type::IsRewindable;
    static constexpr size_t g_BucketSize = PoolMap<sizeof(TContext)>::Type::g_BucketSize;
};

} // namespace Allocator
