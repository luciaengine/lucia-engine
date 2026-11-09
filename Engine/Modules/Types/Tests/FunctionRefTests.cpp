#include "Types/FunctionRef.h"

#include "Memory/AllocationCountScope.h"

#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <gtest/gtest.h>

using GameEngine::FunctionRef;
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::CountWindow;

namespace
{

static_assert(std::is_trivially_copyable_v<FunctionRef<int(int)>>);
static_assert(sizeof(FunctionRef<int(int)>) == 2 * sizeof(void*));
static_assert(!std::is_copy_assignable_v<FunctionRef<int(int)>>, "an assignment can make a FunctionRef refer to a temporary");

int Twice(int value)
{
    return value * 2;
}

int CallWithThree(FunctionRef<int(int)> function)
{
    return function(3);
}

// The non-const call operator returns 1 and the const one returns 2.
struct ConstnessProbe
{
    int operator()(int) { return 1; }
    int operator()(int) const { return 2; }
};

} // namespace

TEST(FunctionRefTests, CallsALambda)
{
    int calls = 0;
    EXPECT_EQ(CallWithThree([&calls](int value) { ++calls; return value + 1; }), 4);
    EXPECT_EQ(calls, 1);
}

TEST(FunctionRefTests, CallsAFunctionByNameAndThroughAPointer)
{
    EXPECT_EQ(CallWithThree(Twice), 6);
    EXPECT_EQ(CallWithThree(&Twice), 6);
}

TEST(FunctionRefTests, CallsAConstCallableThroughItsConstOperator)
{
    const ConstnessProbe constProbe;
    ConstnessProbe mutableProbe;
    EXPECT_EQ(CallWithThree(constProbe), 2);
    EXPECT_EQ(CallWithThree(mutableProbe), 1);
}

TEST(FunctionRefTests, ForwardsArgumentsAndTheResult)
{
    int target = 0;
    auto store = [](std::unique_ptr<int> value, int& destination) -> int& { destination = *value; return destination; };
    FunctionRef<int&(std::unique_ptr<int>, int&)> function = store;
    int& result = function(std::make_unique<int>(7), target);
    EXPECT_EQ(target, 7);
    EXPECT_EQ(&result, &target);
}

TEST(FunctionRefTests, AVoidSignatureDiscardsTheResult)
{
    int calls = 0;
    auto countCall = [&calls](int) { return ++calls; };
    FunctionRef<void(int)> function = countCall;
    function(1);
    function(2);
    EXPECT_EQ(calls, 2);
}

TEST(FunctionRefTests, BindingAndCallingDoNotAllocate)
{
    // The lambda captures 64 bytes: more than any standard library's std::function keeps inline.
    const std::array<std::uint64_t, 8> values{1, 2, 3, 4, 5, 6, 7, 8};
    AllocationCountScope scope(CountWindow::ThisThread);
    const int result = CallWithThree([values](int value) { return value + static_cast<int>(values.back()); });
    EXPECT_EQ(scope.Count(), 0u);
    EXPECT_EQ(result, 11);
}
