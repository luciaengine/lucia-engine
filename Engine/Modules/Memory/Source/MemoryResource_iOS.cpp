#include <memory_resource>

// iOS 16 has no libc++ memory_resource key function. Arena and ScratchScope
// supply their own allocation operations, so only this empty base destructor
// and its vtable/typeinfo need to live in the application for those resources.
_LIBCPP_BEGIN_NAMESPACE_STD
namespace pmr
{
memory_resource::~memory_resource() = default;
}
_LIBCPP_END_NAMESPACE_STD
