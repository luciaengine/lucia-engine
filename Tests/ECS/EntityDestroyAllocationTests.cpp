#include <gtest/gtest.h>

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Types/GeometricReserve.h"

#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

// This source belongs only to the dedicated destruction executable. Injection
// is thread-local and armed solely around the operation under test.
namespace
{
thread_local bool failAllocations = false;
thread_local std::size_t failAllocationSize = 0;
thread_local unsigned deniedAllocations = 0;
} // namespace

void* operator new(std::size_t size)
{
    if (failAllocations || (failAllocationSize != 0 && size == failAllocationSize))
    {
        ++deniedAllocations;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void* memory) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory) noexcept
{
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
using namespace GameEngine::ECS;
struct AllocationResource
{
    int Value = 73;
};
struct OtherAllocationResource
{
    int Value = 73;
};
int cleanupCalls = 0;
bool failAfterCleanup = false;
bool throwAfterCleanup = false;

void Cleanup(AllocationResource& value)
{
    ++cleanupCalls;
    value.Value = 0;
    failAllocations = failAfterCleanup;
    if (throwAfterCleanup)
        throw 7;
}

void CleanupOther(OtherAllocationResource& value)
{
    ++cleanupCalls;
    value.Value = 0;
    failAllocations = failAfterCleanup;
    if (throwAfterCleanup)
        throw 7;
}

class EntityDestroyAllocation : public testing::Test
{
  protected:
    void SetUp() override
    {
        failAllocations = failAfterCleanup = throwAfterCleanup = false;
        failAllocationSize = deniedAllocations = 0;
        cleanupCalls = 0;
    }
    void TearDown() override
    {
        failAllocations = failAfterCleanup = throwAfterCleanup = false;
        failAllocationSize = 0;
    }
};

TEST_F(EntityDestroyAllocation, FreeSlotPreparationFailureLeavesOwnershipIntact)
{
    World world;
    world.RegisterOnRemove<AllocationResource>(Cleanup);
    const auto entity = world.CreateHandle(AllocationResource{});
    bool rejected = false;
    failAllocations = true;
    try
    {
        world.DestroyEntityImmediate(entity);
    }
    catch (const std::bad_alloc&)
    {
        rejected = true;
    }
    failAllocations = false;
    EXPECT_TRUE(rejected);
    EXPECT_EQ(cleanupCalls, 0);
    ASSERT_TRUE(world.IsValid(entity));
    EXPECT_EQ(world.GetComponent<AllocationResource>(entity)->Value, 73);
    world.DestroyEntityImmediate(entity);
    EXPECT_FALSE(world.IsValid(entity));
    EXPECT_EQ(cleanupCalls, 1);
}

TEST_F(EntityDestroyAllocation, RequestedSetFinishesWithoutPreparationAfterFirstCleanup)
{
    // One entity more than the minimum reservation: a loop of singular
    // destroys would have to grow its retirement storage again after the
    // earlier entities released their resources.
    World world;
    world.RegisterOnRemove<AllocationResource>(Cleanup);
    world.EnableLifecycleEvents<AllocationResource>();
    std::vector<EntityHandle> requested;
    while (requested.size() < GameEngine::kMinimumGeometricCapacity + 1)
        requested.push_back(world.CreateHandle(AllocationResource{}));
    failAfterCleanup = true;
    bool threw = false;
    try
    {
        world.DestroyEntitiesImmediate(requested);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocations = failAfterCleanup = false;
    EXPECT_FALSE(threw);
    EXPECT_EQ(static_cast<std::size_t>(cleanupCalls), requested.size());
    for (const auto entity : requested)
        EXPECT_FALSE(world.IsValid(entity));
    EXPECT_EQ(world.GetEntityCount(), 0u);
    world.SwapLifecycleEvents();
    EXPECT_EQ(world.GetRemoved<AllocationResource>().size(), requested.size());
}

TEST_F(EntityDestroyAllocation, BatchSnapshotFailureLeavesAllOwnershipIntact)
{
    World world;
    world.RegisterOnRemove<AllocationResource>(Cleanup);
    world.EnableLifecycleEvents<AllocationResource>();
    const std::array requested{
        world.CreateHandle(AllocationResource{}), world.CreateHandle(AllocationResource{})};
    const auto version = world.GetStructuralChangeVersion();
    bool rejected = false;
    failAllocations = true;
    try
    {
        world.DestroyEntitiesImmediate(requested);
    }
    catch (const std::bad_alloc&)
    {
        rejected = true;
    }
    failAllocations = false;
    EXPECT_TRUE(rejected);
    EXPECT_EQ(cleanupCalls, 0);
    EXPECT_EQ(world.GetStructuralChangeVersion(), version);
    EXPECT_EQ(world.GetEntityCount(), requested.size());
    for (const auto entity : requested)
    {
        ASSERT_TRUE(world.IsValid(entity));
        EXPECT_EQ(world.GetComponent<AllocationResource>(entity)->Value, 73);
    }
    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetRemoved<AllocationResource>().empty());
    EXPECT_EQ(world.GetAdded<AllocationResource>().size(), requested.size());
}

TEST_F(EntityDestroyAllocation, LaterLifecycleTypePreparationFailureLeavesEntireBatchIntact)
{
    // Signatures enumerate types in ID order. Reservation grows from the
    // minimum geometric capacity, so the later type gets two entities more
    // than that minimum and its event allocation is the only one of its size:
    // the snapshot and the free indices hold every entity, the first type's
    // events reserve the minimum.
    constexpr std::size_t kLateSlots = GameEngine::kMinimumGeometricCapacity + 2;
    constexpr std::size_t kEarlySlots = 2;
    using Early = std::conditional_t<(GetComponentTypeId<AllocationResource>() <
                                      GetComponentTypeId<OtherAllocationResource>()),
                                     AllocationResource, OtherAllocationResource>;
    using Late = std::conditional_t<std::is_same_v<Early, AllocationResource>,
                                    OtherAllocationResource, AllocationResource>;
    World world;
    world.RegisterOnRemove<AllocationResource>(Cleanup);
    world.RegisterOnRemove<OtherAllocationResource>(CleanupOther);
    world.EnableLifecycleEvents<Early>();
    world.EnableLifecycleEvents<Late>();
    std::vector<EntityHandle> requested{world.CreateHandle(Early{}, Late{}), world.CreateHandle(Early{})};
    while (requested.size() < kEarlySlots + kLateSlots - 1)
        requested.push_back(world.CreateHandle(Late{}));
    const auto version = world.GetStructuralChangeVersion();
    bool rejected = false;
    failAllocationSize = kLateSlots * sizeof(EntityHandle);
    try
    {
        world.DestroyEntitiesImmediate(requested);
    }
    catch (const std::bad_alloc&)
    {
        rejected = true;
    }
    failAllocationSize = 0;
    EXPECT_TRUE(rejected);
    EXPECT_EQ(deniedAllocations, 1u);
    EXPECT_EQ(cleanupCalls, 0);
    EXPECT_EQ(world.GetStructuralChangeVersion(), version);
    EXPECT_EQ(world.GetEntityCount(), requested.size());
    for (const auto entity : requested)
    {
        ASSERT_TRUE(world.IsValid(entity));
        if (const auto* value = world.GetComponent<Early>(entity))
            EXPECT_EQ(value->Value, 73);
        if (const auto* value = world.GetComponent<Late>(entity))
            EXPECT_EQ(value->Value, 73);
    }
    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetRemoved<Early>().empty());
    EXPECT_TRUE(world.GetRemoved<Late>().empty());
    EXPECT_EQ(world.GetAdded<Early>().size(), kEarlySlots);
    EXPECT_EQ(world.GetAdded<Late>().size(), kLateSlots);
    world.DestroyEntitiesImmediate(requested);
    EXPECT_EQ(world.GetEntityCount(), 0u);
    EXPECT_EQ(cleanupCalls, static_cast<int>(kEarlySlots + kLateSlots));
}

TEST_F(EntityDestroyAllocation, EmptyInvalidAndStaleBatchesNeedNoAllocation)
{
    World world;
    const auto stale = world.CreateEntity();
    world.DestroyEntityImmediate(stale);
    const auto live = world.CreateEntity();
    const std::array invalid{EntityHandle::Invalid(), stale, stale};
    bool threw = false;
    failAllocations = true;
    try
    {
        world.DestroyEntitiesImmediate({});
        world.DestroyEntitiesImmediate(invalid);
    }
    catch (...)
    {
        threw = true;
    }
    failAllocations = false;
    EXPECT_FALSE(threw);
    EXPECT_TRUE(world.IsValid(live));
    EXPECT_EQ(world.GetEntityCount(), 1u);
}

TEST_F(EntityDestroyAllocation, RemovedReservationOverflowPreservesPendingEvents)
{
    LifecycleEventBuffers events;
    const auto type = GetComponentTypeId<AllocationResource>();
    events.Register(type);
    const EntityHandle entity{17, 1};
    events.AppendRemoved(type, entity);
    EXPECT_THROW(events.PrepareRemoved(type, std::numeric_limits<std::size_t>::max()), std::length_error);
    events.Swap();
    ASSERT_EQ(events.CurrentRemoved(type).size(), 1u);
    EXPECT_EQ(events.CurrentRemoved(type)[0], entity);
}

TEST_F(EntityDestroyAllocation, LifecyclePreparationFailureLeavesPreservedEntityIntact)
{
    World world;
    world.RegisterOnRemove<AllocationResource>(Cleanup);
    world.EnableLifecycleEvents<AllocationResource>();
    const auto entity = world.CreateHandle(AllocationResource{});
    bool rejected = false;
    failAllocations = true;
    try
    {
        world.DestroyEntityImmediatePreserveHandle(entity);
    }
    catch (const std::bad_alloc&)
    {
        rejected = true;
    }
    failAllocations = false;
    EXPECT_TRUE(rejected);
    EXPECT_EQ(cleanupCalls, 0);
    ASSERT_TRUE(world.IsValid(entity));
    EXPECT_EQ(world.GetComponent<AllocationResource>(entity)->Value, 73);
    world.DestroyEntityImmediatePreserveHandle(entity);
    EXPECT_FALSE(world.IsValid(entity));
    EXPECT_TRUE(world.ReviveEntityImmediatePreserveHandle(entity));
}

TEST_F(EntityDestroyAllocation, FinalizationDoesNotAllocateAfterOwnershipWasReleased)
{
    for (const bool preserve : {false, true})
    {
        World world;
        world.RegisterOnRemove<AllocationResource>(Cleanup);
        world.EnableLifecycleEvents<AllocationResource>();
        const auto entity = world.CreateHandle(AllocationResource{});
        failAfterCleanup = true;
        bool threw = false;
        try
        {
            if (preserve)
                world.DestroyEntityImmediatePreserveHandle(entity);
            else
                world.DestroyEntityImmediate(entity);
        }
        catch (...)
        {
            threw = true;
        }
        failAllocations = failAfterCleanup = false;
        EXPECT_FALSE(threw);
        EXPECT_FALSE(world.IsValid(entity));
        EXPECT_EQ(world.GetEntityCount(), 0u);
        world.SwapLifecycleEvents();
        ASSERT_EQ(world.GetRemoved<AllocationResource>().size(), 1u);
        EXPECT_EQ(world.GetRemoved<AllocationResource>()[0], entity);
    }
}

TEST_F(EntityDestroyAllocation, FailureReportingCannotVetoFinalizationDuringAllocationFailure)
{
    World world;
    world.RegisterOnRemove<AllocationResource>(Cleanup);
    const auto entity = world.CreateHandle(AllocationResource{});
    failAfterCleanup = throwAfterCleanup = true;
    bool threw = false;
    try
    {
        world.DestroyEntityImmediate(entity);
    }
    catch (...)
    {
        threw = true;
    }
    failAllocations = failAfterCleanup = throwAfterCleanup = false;
    EXPECT_FALSE(threw);
    EXPECT_FALSE(world.IsValid(entity));
    EXPECT_EQ(cleanupCalls, 1);
}

TEST_F(EntityDestroyAllocation, WarmUnsubscribedCreateDestroyNeedsNoAllocation)
{
    World world;
    world.DestroyEntityImmediate(world.CreateHandle(AllocationResource{}));
    unsigned completed = 0;
    failAllocations = true;
    try
    {
        for (; completed != 1024; ++completed)
            world.DestroyEntityImmediate(world.CreateHandle(AllocationResource{}));
    }
    catch (...)
    {
    }
    failAllocations = false;
    EXPECT_EQ(completed, 1024u);
    EXPECT_EQ(world.GetEntityCount(), 0u);
}
} // namespace
