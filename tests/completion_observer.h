#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace coop::test
{

// Test-artifact-only observation of real CQEs at the ordinary dispatch boundary.
// The linker wrapper always forwards the original CQE, once, without modification.
struct CompletionRecord
{
    uintptr_t data;
    int result;
    unsigned flags;
};

struct CompletionObserver
{
    CompletionObserver();
    ~CompletionObserver();
    CompletionObserver(const CompletionObserver&) = delete;
    CompletionObserver& operator=(const CompletionObserver&) = delete;

    size_t Count(uintptr_t data, unsigned mask = 0, unsigned value = 0) const;
    std::array<CompletionRecord, 256> records{};
    size_t size{0};
    bool overflow{false};
};

} // namespace coop::test
