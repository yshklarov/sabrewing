#ifdef _WIN32
#include <process.h>  // _beginthreadex, _endthreadex
#else
#include <pthread.h>  // pthread_create(), etc.
#endif

#ifdef _WIN32
#define PROCESS HANDLE
#define THREAD HANDLE
#define THREAD_EVENT HANDLE
#define THREAD_MUTEX HANDLE
#define THREAD_ENTRYPOINT unsigned __stdcall
#else
#define THREAD pthread_t
// Struct to simulate Win32 event behavior.
typedef struct {
    pthread_cond_t cond;
    pthread_mutex_t mtx;
    bool flag;
} pthread_event_t;
#define THREAD_EVENT pthread_event_t*
#define THREAD_MUTEX pthread_mutex_t*
#define THREAD_ENTRYPOINT void*
#endif

void mutex_initialize(THREAD_MUTEX mtx)
{
    #ifdef _WIN32
    (void)mtx;
    // Unimplemented
    #else
    pthread_mutex_init(mtx, NULL);
    #endif
}

void mutex_acquire(THREAD_MUTEX mtx)
{
    #ifdef _WIN32
    WaitForSingleObject(mtx, INFINITE);
    #else
    pthread_mutex_lock(mtx);
    #endif
}

void mutex_release(THREAD_MUTEX mtx)
{
    #ifdef _WIN32
    ReleaseMutex(mtx);
    #else
    pthread_mutex_unlock(mtx);
    #endif
}

void mutex_destroy(THREAD_MUTEX mtx)
{
    #ifdef _WIN32
    CloseHandle(mtx);
    #else
    pthread_mutex_destroy(mtx);
    #endif
}

void event_initialize(THREAD_EVENT evt)
{
    #ifdef _WIN32
    (void)evt;
    // Unimplemented
    #else
    pthread_cond_init(&evt->cond, NULL);
    pthread_mutex_init(&evt->mtx, NULL);
    evt->flag = false;
    #endif
}

void event_destroy(THREAD_EVENT evt)
{
    #ifdef _WIN32
    CloseHandle(evt);
    #else
    pthread_cond_destroy(&evt->cond);
    pthread_mutex_destroy(&evt->mtx);
    evt->flag = false;
    #endif
}

void event_signal(THREAD_EVENT evt)
{
    #ifdef _WIN32
    SetEvent(evt);
    #else
    pthread_mutex_lock(&evt->mtx);
    evt->flag = true;
    pthread_mutex_unlock(&evt->mtx);
    pthread_cond_signal(&evt->cond);
    #endif
}

// Non-blocking test for whether evt has been set.
bool event_check(THREAD_EVENT evt)
{
    #ifdef _WIN32
    return WaitForSingleObject(evt, 0) == WAIT_OBJECT_0;
    #else
    pthread_mutex_lock(&evt->mtx);
    bool result = evt->flag;
    pthread_mutex_unlock(&evt->mtx);
    return result;
    #endif
}

// Block until evt has been signaled by event_signal().
void event_wait(THREAD_EVENT evt)
{
    #ifdef _WIN32
    WaitForSingleObject(evt, INFINITE);
    #else
    pthread_mutex_lock(&evt->mtx);
    while (!evt->flag) {
        pthread_cond_wait(&evt->cond, &evt->mtx);
    }
    #endif
}

bool thread_has_joined(THREAD t)
{
    #ifdef _WIN32
    return WaitForSingleObject(t, 0) == WAIT_OBJECT_0;
    #else
    struct timespec timeout = {0, 0};
    return pthread_timedjoin_np(t, NULL, &timeout) == 0;
    #endif
}
