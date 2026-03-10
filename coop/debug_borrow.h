#pragma once

#include <cassert>
#include <cstdint>

#include "cooperator.h"
#include "context.h"
#include "context_var.h"
#include "detail/embedded_list.h"

namespace coop::debug
{

#ifndef NDEBUG

struct BorrowRecord : EmbeddedListHookups<BorrowRecord>
{
    Context*      owner{nullptr};
    const void*   ptr{nullptr};
    const char*   label{nullptr};
};

struct BorrowState
{
    EmbeddedList<BorrowRecord> borrows{};
    uint32_t                   count{0};
};

inline ContextVar<BorrowState> s_borrowState;

inline bool HasScheduledContext()
{
    return Cooperator::thread_cooperator != nullptr
           && Cooperator::thread_cooperator->Scheduled() != nullptr;
}

inline BorrowState* GetBorrowState(Context* ctx)
{
    return s_borrowState.Get(ctx);
}

inline void TrackBorrow(BorrowRecord* record, const void* ptr, const char* label)
{
    if (!record || !ptr || !HasScheduledContext())
        return;

    Context* ctx = Self();
    assert(ctx != nullptr && "TrackBorrow requires scheduled context");
    assert(record->owner == nullptr && "borrow record already tracked");
    assert(record->Disconnected() && "borrow record already linked");

    BorrowState* state = GetBorrowState(ctx);
    record->owner = ctx;
    record->ptr   = ptr;
    record->label = label;
    state->borrows.Push(record);
    state->count++;
}

inline void UntrackBorrow(BorrowRecord* record)
{
    if (!record || !record->owner)
        return;

    Context* owner = record->owner;
    BorrowState* state = GetBorrowState(owner);
    assert(!record->Disconnected() && "borrow record not linked");
    assert(state->count > 0
           && "borrow state corrupted on untrack");
    state->borrows.Remove(record);

    state->count--;
    record->owner = nullptr;
    record->ptr   = nullptr;
    record->label = nullptr;
}

inline void AssertNoOutstandingBorrows(Context* ctx)
{
    BorrowState* state = GetBorrowState(ctx);
    assert(state->borrows.IsEmpty()
           && "cannot Yield while debug-tracked skiplist node borrows are outstanding");
    assert(state->count == 0
           && "borrow count mismatch: expected no tracked borrows at Yield");
}

#else

struct BorrowRecord {};

inline void TrackBorrow(BorrowRecord*, const void*, const char*) {}
inline void UntrackBorrow(BorrowRecord*) {}
inline void AssertNoOutstandingBorrows(Context*) {}

#endif

} // namespace coop::debug
