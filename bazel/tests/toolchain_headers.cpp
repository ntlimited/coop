#include <features.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

static_assert(__WORDSIZE == sizeof(void*) * 8);
static_assert(sizeof(uintptr_t) == sizeof(void*));

int main()
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_lock(&mutex) != 0)
        return 1;
    if (pthread_mutex_unlock(&mutex) != 0)
        return 1;
    return pthread_mutex_destroy(&mutex) == 0 ? 0 : 1;
}
