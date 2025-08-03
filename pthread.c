#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <uinstd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include <signal.h>

// our own custom pthread types
typedef struct my_thread* my_pthread_t;
typedef struct my_mutex* my_pthread_mutex_t;
typedef void* (*my_thread_func_t)(void*);

struct my_thread {
    pid_t tid; // linux thread ID (from clone())
    void* stack; // thread stack pointer
    size_t stack_size; 
    my_thread_func_t start_func;
    void* arg;
    void* return_value;
    int joinable;
    int joined;
    int detached;
}

static my_pthread_mutex_t thread_list_mutex;

struct test_data {
    int counter;
    my_pthread_mutex_t mutex;
};
// Mutex structure
struct my_mutex {
    atomic_int futex_word;
    pid_t owner;
    int type;
    int count // lock count, for recursive mutexes
};

#define MY_PTHREAD_MUTEX_NORMAL 0

// initialize a mutex
int my_pthread_mutex_init(my_pthread_mutex_t* mutex_ptr, void* attr) {
    struct my_mutex* mutex = malloc(sizeof(struct my_mutex));
    if (!mutex) return ENOMEM;

    atomic_init(&mutex->futex_word, 0); // 0 = unlocked, 1 = locked
    mutex->owner = 0;
    mutex->type = MY_PTHREAD_MUTEX_NORMAL;
    mutex->count = 0;

    *mutex_ptr = mutex;
    return 0;
}

void* worker_thread(void* arg) {
    struct test_data* data = (struct test_data*)arg;

    printf("Worker thread started with PID: %d, TID: %d\n", getpid(), (int)syscall(SYS_gettid));
    for (int i = 0; i < 5; i++) {
        my_pthread_mutex_lock(data->mutex);
        int old_val = data->counter;
        usleep(100000); // simulate work
        data->counter = old_val + 1;
        printf("Worker thread %d incremented counter to %d\n", (int)syscall(SYS_gettid), data->counter);
        my_pthread_mutex_unlock(data->mutex);
        usleep(50000); // simulate some delay
    }
    return (void*)(long)syscall(SYS_gettid);
}

// Create a new thread
int my_pthread_create(my_pthread_t* thread_ptr, void* attr, 
                     my_thread_func_t start_routine, void* arg) {
    
    // Allocate thread control block
    struct my_thread* thread = malloc(sizeof(struct my_thread));
    if (!thread) return ENOMEM;
    
    // Allocate stack
    thread->stack_size = MY_PTHREAD_STACK_SIZE;
    thread->stack = mmap(NULL, thread->stack_size, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    
    if (thread->stack == MAP_FAILED) {
        free(thread);
        return ENOMEM;
    }
    
    // Initialize thread structure
    thread->start_func = start_routine;
    thread->arg = arg;
    thread->return_value = NULL;
    thread->joinable = 1;
    thread->joined = 0;
    thread->detached = 0;
    
    // Calculate stack top (stacks grow down)
    void* stack_top = (char*)thread->stack + thread->stack_size;
    
    // Clone flags for creating a thread (not a process)
    int clone_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                     CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                     CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
    
    // Create the thread using clone system call
    thread->tid = sys_clone(thread_wrapper, stack_top, clone_flags, 
                           thread, &thread->tid, NULL, &thread->tid);
    
    if (thread->tid == -1) {
        munmap(thread->stack, thread->stack_size);
        free(thread);
        return errno;
    }
    
    // Add to thread list (simplified - should use proper synchronization)
    thread_list[thread_count++] = thread;
    
    *thread_ptr = thread;
    return 0;
}

// Unlock mutex
int my_pthread_mutex_unlock(my_pthread_mutex_t mutex) {
    if (!mutex) return EINVAL;
    
    pid_t tid = syscall(SYS_gettid);
    
    // Check if we own the mutex
    if (mutex->owner != tid) {
        return EPERM;
    }
    
    mutex->owner = 0;
    mutex->count = 0;
    
    // Release the lock atomically
    atomic_store(&mutex->futex_word, 0);
    
    // Wake up one waiting thread
    sys_futex(&mutex->futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    
    return 0;
}

// Lock mutex
int my_pthread_mutex_lock(my_pthread_mutex_t mutex) {
    if (!mutex) return EINVAL;
    
    pid_t tid = syscall(SYS_gettid);
    
    // Fast path: try to acquire lock atomically
    int expected = 0;
    if (atomic_compare_exchange_strong(&mutex->futex_word, &expected, 1)) {
        mutex->owner = tid;
        mutex->count = 1;
        return 0; // Got the lock immediately
    }
    
    // Slow path: lock is contended, use futex to wait
    while (1) {
        // Try to acquire lock again
        expected = 0;
        if (atomic_compare_exchange_strong(&mutex->futex_word, &expected, 1)) {
            mutex->owner = tid;
            mutex->count = 1;
            return 0;
        }
        
        // Wait on futex - kernel will block this thread until futex changes
        sys_futex(&mutex->futex_word, FUTEX_WAIT, 1, NULL, NULL, 0);
    }
}

// Destroy mutex
int my_pthread_mutex_destroy(my_pthread_mutex_t mutex) {
    if (!mutex) return EINVAL;
    
    // Check if mutex is still locked
    if (atomic_load(&mutex->futex_word) != 0) {
        return EBUSY;
    }
    
    free(mutex);
    return 0;
}


int main (){
    printf("=== Custom pthread implementation ===\n");
    printf("Main thread PID: %d\n", getpid());
    printf("Main thread TID: %d\n", (int)syscall(SYS_gettid));

    struct test_data data = {0, NULL};
    // initialize mutex
    my_pthread_mutex_init()
    // create threads
    my_pthread_mutex_init(&data.mutex, NULL);
    // create threads
    my_pthread_t threads[3];

    for (int i = 0; i < 3; i++) {
        int result = my_pthread_create(&threads[i], NULL, worker_thread, &data);
        if (result != 0) {
            fprintf(stderr, "Error creating thread %d: %s\n", i, strerror(result));
            return result;
        }
        printf("Created thread %d with TID: %d\n", i, (int)threads[i]->tid);
    }
    // Wait for threads to complete
    for (int i = 0; i < 3; i++) {
        void* retval;
        int result = my_pthread_join(threads[i], &retval);
        if (result == 0) {
            printf("Thread %d joined, returned: %ld\n", i, (long)retval);
        } else {
            fprintf(stderr, "Failed to join thread %d: %s\n", i, strerror(result));
        }
    }
    
    printf("Final counter value: %d\n", data.counter);

    // Clean up
    my_pthread_mutex_destroy(data.mutex);
}