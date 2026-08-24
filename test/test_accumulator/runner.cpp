#include <unity.h>

#include <cstdlib>
#include <new>

// ONLY the array forms are overridden — the ones AccumulatorV2's window uses.
//
// Overriding scalar new/delete as well crashed the binary before Unity printed
// a line: those are used by libstdc++ and Unity themselves, including during
// static initialisation, and replacing them from a test translation unit is a
// wider blast radius than this measurement needs.
//
// This exists because CLAUDE.md carried "AccumulatorV2 allocates on every
// sample" as an open contradiction for as long as nothing measured it. A note
// cannot fail; this can.
unsigned long g_allocCount = 0;
unsigned long g_freeCount = 0;

void*
operator new[](std::size_t size)
{
    ++g_allocCount;
    void* p = std::malloc(size ? size : 1);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void*
operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    ++g_allocCount;
    return std::malloc(size ? size : 1);
}

void
operator delete[](void* p) noexcept
{
    if (p != nullptr) {
        ++g_freeCount;
    }
    std::free(p);
}

void
operator delete[](void* p, std::size_t) noexcept
{
    ::operator delete[](p);
}

void
operator delete[](void* p, const std::nothrow_t&) noexcept
{
    ::operator delete[](p);
}

void run_accumulator_tests(void);

void
setUp(void)
{
}

void
tearDown(void)
{
}

int
main(int, char**)
{
    UNITY_BEGIN();
    run_accumulator_tests();
    return UNITY_END();
}
