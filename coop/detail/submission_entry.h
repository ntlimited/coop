#pragma once

#include <semaphore>

#include "coop/spawn_configuration.h"

namespace coop
{

struct Context;

// Type-erased node on a cooperator's inbound submission list. Heap TypedSubmission
// owns a lambda; CooperateHandle embeds one for the reverse spawn-result notify
// so the target never allocates on the way back.
//
struct SubmissionEntry
{
    SubmissionEntry* m_next{nullptr};
    void (*m_invoke)(SubmissionEntry*, Context*){nullptr};
    void (*m_destroy)(SubmissionEntry*){nullptr};
    SpawnConfiguration m_config{};
    std::binary_semaphore* m_completion{nullptr};
    bool* m_completionOk{nullptr};

    // When true, DrainSubmissions runs m_invoke on this thread instead of Spawn.
    // Cooperate's reverse notify is a wakeup, not work.
    //
    bool m_inline{false};
};

} // end namespace coop
