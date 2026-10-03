#include "completion_observer.h"

#include <cassert>
#include <cstdlib>
#include <liburing.h>

namespace
{
thread_local coop::test::CompletionObserver* observer = nullptr;
}

namespace coop::test
{

CompletionObserver::CompletionObserver()
{
    if (observer != nullptr)
        std::abort();
    observer = this;
}

CompletionObserver::~CompletionObserver()
{
    assert(observer == this);
    observer = nullptr;
}

size_t CompletionObserver::Count(uintptr_t data, unsigned mask, unsigned value) const
{
    size_t count = 0;
    for (size_t i = 0; i < size; ++i)
        if (records[i].data == data && (records[i].flags & mask) == value)
            ++count;
    return count;
}

} // namespace coop::test

extern "C" void __real__ZN4coop2io6Handle8CallbackEP12io_uring_cqe(io_uring_cqe*);

extern "C" void __wrap__ZN4coop2io6Handle8CallbackEP12io_uring_cqe(io_uring_cqe* cqe)
{
    const coop::test::CompletionRecord record{cqe->user_data, cqe->res, cqe->flags};
    if (observer)
    {
        if (observer->size == observer->records.size())
            observer->overflow = true;
        else
            observer->records[observer->size++] = record;
    }
    __real__ZN4coop2io6Handle8CallbackEP12io_uring_cqe(cqe);
    if (observer && observer->afterDispatch)
        observer->afterDispatch(record, observer->afterDispatchData);
}
