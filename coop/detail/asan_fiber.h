#pragma once

#include <cstddef>

// COOP_HAVE_ASAN — 1 when this translation unit is compiled with AddressSanitizer.
//
// clang answers through __has_feature; GCC predefines __SANITIZE_ADDRESS__, as does clang from 15
// onward for compatibility. Both are consulted so the annotations below are active under either
// toolchain, and — the property that matters more — absent under neither. Everything this header
// offers collapses to nothing when the macro is 0, so an ordinary build emits byte-for-byte the
// code it emitted before the header existed.
//
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define COOP_HAVE_ASAN 1
#  endif
#endif
#if !defined(COOP_HAVE_ASAN) && defined(__SANITIZE_ADDRESS__)
#  define COOP_HAVE_ASAN 1
#endif
#if !defined(COOP_HAVE_ASAN)
#  define COOP_HAVE_ASAN 0
#endif

#if COOP_HAVE_ASAN
#include <pthread.h>
#endif

namespace coop
{
namespace detail
{

// StackRegion — a stack's lowest address and its extent, which is the shape AddressSanitizer wants
// when it is told which stack is about to become the running one. A Context carries its region in
// its Segment; a cooperator's scheduler loop runs on an ordinary OS thread stack and has to ask the
// platform for its.
//
struct StackRegion
{
    void const* bottom = nullptr;
    size_t      size   = 0;
};

// StackOf — the region a context's code runs on.
//
// A Segment is one contiguous span shared by the context's stack, growing down from the top, and
// its bump heap, growing up from the bottom. The whole span is handed to the sanitizer: it is
// exactly the memory that becomes live when the context is scheduled, and describing less of it
// would leave the heap end outside the bounds the sanitizer believes the running stack occupies.
//
// Templated purely to keep this header free of a dependency on the full Context definition. The
// switch path this header exists to annotate sits below Context in the include order.
//
template<typename ContextT>
inline StackRegion StackOf(ContextT* ctx)
{
    return StackRegion{ctx->m_segment.Bottom(), ctx->m_segment.Size()};
}

#if COOP_HAVE_ASAN

} // end namespace detail
} // end namespace coop

// The sanitizer's own entry points, declared rather than included: the sanitizer/ headers are a
// toolchain packaging detail that several distributions omit, while these four symbols are a
// stable part of the runtime's ABI and are what the headers would have declared.
//
extern "C"
{
    void __sanitizer_start_switch_fiber(void** fake_stack_save, void const* bottom, size_t size);
    void __sanitizer_finish_switch_fiber(
        void* fake_stack_save, void const** bottom_old, size_t* size_old);
    void __asan_poison_memory_region(void const volatile* addr, size_t size);
    void __asan_unpoison_memory_region(void const volatile* addr, size_t size);
}

namespace coop
{
namespace detail
{

// The bounds of the calling thread's own stack. Resolved once per cooperator, never on the switch
// path, so the /proc/self/maps read glibc performs for the main thread costs nothing that matters.
//
inline StackRegion CurrentThreadStack()
{
    StackRegion region;

    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
    {
        return region;
    }

    void*  addr = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &addr, &size) == 0)
    {
        region.bottom = addr;
        region.size   = size;
    }
    pthread_attr_destroy(&attr);

    return region;
}

inline void AsanStartSwitch(void** fakeStackSave, StackRegion const& to)
{
    __sanitizer_start_switch_fiber(fakeStackSave, to.bottom, to.size);
}

inline void AsanFinishSwitch(void* fakeStackSave)
{
    __sanitizer_finish_switch_fiber(fakeStackSave, nullptr, nullptr);
}

inline void AsanUnpoison(void* addr, size_t size)
{
    __asan_unpoison_memory_region(addr, size);
}

#endif // COOP_HAVE_ASAN

} // end namespace detail
} // end namespace coop

// COOP_ASAN_SWITCH_BEGIN / COOP_ASAN_SWITCH_END bracket a call to ContextSwitch.
//
// AddressSanitizer keeps a per-thread notion of which stack is running: its bounds, and — when
// detect_stack_use_after_return is on — the fake stack holding locals belonging to frames that have
// returned. Nothing about a hand-written stack swap tells it any of that changed, so without these
// calls its shadow keeps describing whichever stack ran last. The visible symptom is not a missed
// bug but a supply of invented ones: poison a departed fiber left behind is read as a live error in
// the fiber that inherits the address, and gets reported against whatever ordinary function
// happened to touch it.
//
// BEGIN names the stack control is moving to and parks the outgoing fiber's fake stack in a local.
// That local lives on the outgoing fiber's own stack, so when the switch call finally returns —
// possibly much later, after other fibers have run — it still holds the value END must hand back.
//
// BEGIN declares a variable, so it belongs in a braced scope and cannot be the unbraced body of an
// `if`. Each BEGIN needs exactly one END on the path that resumes; the two exceptions have their
// own spellings below.
//
#if COOP_HAVE_ASAN

#define COOP_ASAN_SWITCH_BEGIN(region)                                                             \
    void* coopAsanFakeStack = nullptr;                                                             \
    ::coop::detail::AsanStartSwitch(&coopAsanFakeStack, (region))

#define COOP_ASAN_SWITCH_END() ::coop::detail::AsanFinishSwitch(coopAsanFakeStack)

// The outgoing fiber will never be resumed: it has run its entry, destructed itself, and its
// segment is about to go back to the pool. A null save slot is what tells the sanitizer to release
// the fake stack instead of holding it for a return that will not happen — the difference between a
// bounded runtime and one that leaks a fake stack for every context ever spawned.
//
#define COOP_ASAN_SWITCH_BEGIN_FINAL(region) ::coop::detail::AsanStartSwitch(nullptr, (region))

// Control arrived on a stack that has never run. There is no saved fake stack to restore, and the
// arrival is not the return of some earlier BEGIN — ContextInit built this frame by hand.
//
#define COOP_ASAN_SWITCH_FIRST_ENTRY() ::coop::detail::AsanFinishSwitch(nullptr)

// Hand a recycled segment back to the sanitizer clean.
//
// Stack memory is pooled, and the shadow bytes describing it are not reset when it changes hands.
// An instrumented function unpoisons its own redzones as it returns, so a fiber that runs to
// completion leaves nothing behind — but one torn down while frames are still live does, and that
// poison then sits underneath the next fiber's locals. This is the same stale-poison failure the
// switch annotations address, arriving by a different route, and it produces the same kind of
// report: an error against an innocent function, in a test that only fails sometimes.
//
#define COOP_ASAN_UNPOISON(addr, size) ::coop::detail::AsanUnpoison((addr), (size))

#else

#define COOP_ASAN_SWITCH_BEGIN(region)       ((void)0)
#define COOP_ASAN_SWITCH_END()               ((void)0)
#define COOP_ASAN_SWITCH_BEGIN_FINAL(region) ((void)0)
#define COOP_ASAN_SWITCH_FIRST_ENTRY()       ((void)0)
#define COOP_ASAN_UNPOISON(addr, size)       ((void)0)

#endif
