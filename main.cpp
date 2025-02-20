#include <vector>

#include "coordinator.h"
#include "iomgr.h"
#include "manager_thread.h"

void SpawningTask(ExecutionContext* ctx, void* arg)
{
	auto* mgr = reinterpret_cast<Manager*>(arg);
	printf("In spawning task %p with manager %p\n", ctx, ctx->m_manager);

    std::vector<ExecutionHandle*> subtasks;

    Coordinator coord;

    coord.Acquire(ctx);

    int i = 0;
    while (i++ < 10)
    {
        ExecutionHandle* h = new ExecutionHandle;
        bool success = mgr->Spawn([&](ExecutionContext* ctx)
        {
            printf("Entering %p\n", ctx);
            coord.Acquire(ctx);
            printf("Exiting %p with i=%d\n", ctx, ++i);
            coord.Release(ctx);
        }, h);
        if (success)
        {
            subtasks.push_back(h);
        }
        else
        {
            delete h;
        }
    }

    printf("Spawning completed\n");
    coord.Release(ctx);
    printf("Resumed after release\n");

    for (auto* handle : subtasks)
    {
        while (*handle)
        {
            printf("Waiting on handle %p with context %p to finish\n", handle, handle->m_executionContext);
            ctx->Yield(true);
        }
    }

    printf("Done waiting for tasks to halt\n");
    mgr->Shutdown();
}

int main()
{
	Manager manager;
    ManagerThread mt(&manager);

	manager.Submit(&SpawningTask, reinterpret_cast<void*>(&manager));
	return 0;
}
