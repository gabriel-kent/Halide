#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <math.h>
#include <pthread.h>
#include <assert.h>
#include <cfloat>

#include "buffer.h"


extern "C" {

#define PTR_OFFSET(ptr,offset)	((void*)((char*)(ptr) + (offset)))

// The functions below are all weak so that you can link multiple
// halide modules without name conflicts
#define WEAK __attribute__((weak))

// This only gets defined if it's not already defined by an including module, e.g. PTX.
// This also serves to forcibly include the buffer_t struct definition
#ifndef _COPY_TO_HOST
#define _COPY_TO_HOST
WEAK void __copy_to_host(buffer_t* buf) { /* NOP */ }
#endif //_COPY_TO_HOST

WEAK void *fast_malloc(size_t x) {
    void *orig = malloc(x+16);
    // Walk either 8 or 16 bytes forward
    void *ptr = (void *)((((size_t)orig + 16) >> 4) << 4);
    ((void **)ptr)[-1] = orig;
    return ptr;
}

WEAK void fast_free(void *ptr) {
    free(((void**)ptr)[-1]);
}

WEAK void *safe_malloc(size_t x) {
    void *mem;
    x = ((x + 4095)/4096) * 4096;
    posix_memalign(&mem, 4096, x + 4096 * 3);
    printf("Allocated %lu bytes at %p with an electric fence\n", x, mem);

    // write the end address to unprotect in the initial fence
    ((void **)mem)[0] = PTR_OFFSET(mem, x + 4096);
    
    mprotect(mem, 4096, PROT_NONE);
    mprotect(PTR_OFFSET(mem, x + 4096), 4096, PROT_NONE);
    
    return PTR_OFFSET(mem, 4096);
}

WEAK void safe_free(void *ptr) {
    void *start = PTR_OFFSET(ptr, -4096);
    mprotect(start, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    void *end = ((void **)start)[0];
    mprotect(end, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    free(start);
}

static void (*halide_error_handler)(char *) = NULL;

WEAK void halide_error(char *msg) {
    if (halide_error_handler) (*halide_error_handler)(msg);
    else {
        fprintf(stderr, "Error: %s\n", msg);
        exit(1);
    }
}

WEAK void set_error_handler(void (*handler)(char *)) {
    halide_error_handler = handler;
}

struct work {
    void (*f)(int, uint8_t *);
    int next, max;
    uint8_t *closure;
    int id;
    int active_workers;
};

// The work queue and thread count are static, which means each halide
// function gets a unique one. Is this a good idea?
#define MAX_JOBS 65536
#define MAX_THREADS 64
static struct {
    work jobs[MAX_JOBS];
    int head;
    int tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_t threads[MAX_THREADS];
    int ids;
} work_queue;

struct worker_arg {
    int id;
    work *job;
};

static int threads;

WEAK void *worker(void *void_arg) {
    worker_arg *arg = (worker_arg *)void_arg;
    while (1) {
        //fprintf(stderr, "About to lock mutex\n");
        pthread_mutex_lock(&work_queue.mutex);
        //fprintf(stderr, "Mutex locked, checking for work\n");

        // we're master, and there's no more work
        if (arg && arg->job->id != arg->id) {
            // wait until other workers are done
            if (arg->job->active_workers) {
                pthread_mutex_unlock(&work_queue.mutex);
                while (true) {
                    //fprintf(stderr, "Master waiting for workers to finish\n");
                    pthread_mutex_lock(&work_queue.mutex);
                    if (!arg->job->active_workers)
                        break;
                    pthread_mutex_unlock(&work_queue.mutex);
                }
            }
            // job is actually done
            pthread_mutex_unlock(&work_queue.mutex);
            //fprintf(stderr, "My work here is done. I am needed ... elsewhere!\n");
            return NULL;
        }
            
        if (work_queue.head == work_queue.tail) {
            assert(!arg); // the master should never get here
            //fprintf(stderr, "No work left. Going to sleep.\n");

            pthread_cond_wait(&work_queue.not_empty, &work_queue.mutex);
            pthread_mutex_unlock(&work_queue.mutex);
            continue;
        }

        //fprintf(stderr, "There is work\n");
        work *job = work_queue.jobs + work_queue.head;
        if (job->next == job->max) {
            //fprintf(stderr, "Found a finished job. Removing it\n");
            work_queue.head = (work_queue.head + 1) % MAX_JOBS;            
            job->id = 0; // mark the job done
            pthread_mutex_unlock(&work_queue.mutex);
        } else {
            int remaining = job->max - job->next;
            // Claim some tasks
            //int claimed = (remaining + threads - 1)/threads;
            int claimed = 1;
            //printf("Claiming %d tasks\n", claimed);
            work myjob = *job;
            job->next += claimed;            
            myjob.max = job->next;
            job->active_workers++;
            pthread_mutex_unlock(&work_queue.mutex);
            //fprintf(stderr, "Doing job %d\n", myjob.next);
            for (; myjob.next < myjob.max; myjob.next++)
                myjob.f(myjob.next, myjob.closure);
            //fprintf(stderr, "Done with job %d\n", myjob.next);
            pthread_mutex_lock(&work_queue.mutex);
            job->active_workers--;
            pthread_mutex_unlock(&work_queue.mutex);
        }        
    }
}

WEAK void do_par_for(void (*f)(int, uint8_t *), int min, int size, uint8_t *closure) {
    static bool thread_pool_initialized = false;
    if (!thread_pool_initialized) {
        pthread_mutex_init(&work_queue.mutex, NULL);
        pthread_cond_init(&work_queue.not_empty, NULL);
        work_queue.head = work_queue.tail = 0;
        work_queue.ids = 1;
        char *threadStr = getenv("HL_NUMTHREADS");
        threads = 8;
        if (threadStr) {
            threads = atoi(threadStr);
        } else {
            printf("HL_NUMTHREADS not defined. Defaulting to 8 threads.\n");
        }
        if (threads > MAX_THREADS) threads = MAX_THREADS;
        for (int i = 0; i < threads-1; i++) {
            pthread_create(work_queue.threads + i, NULL, worker, NULL);
        }

        thread_pool_initialized = true;
    }

    // Enqueue the job
    pthread_mutex_lock(&work_queue.mutex);
    //fprintf(stderr, "Enqueuing some work\n");
    work job = {f, min, min + size, closure, work_queue.ids++, 0};
    if (job.id == 0) job.id = work_queue.ids++; // disallow zero, as it flags a completed job
    work_queue.jobs[work_queue.tail] = job;
    work *jobPtr = work_queue.jobs + work_queue.tail;
    worker_arg arg = {job.id, jobPtr};
    int new_tail = (work_queue.tail + 1) % MAX_JOBS;
    assert(new_tail != work_queue.head); 
    work_queue.tail = new_tail;

    // TODO: check to make sure the work queue doesn't overflow
    pthread_mutex_unlock(&work_queue.mutex);
    
    // Wake up everyone
    pthread_cond_broadcast(&work_queue.not_empty);

    // Do some work myself
    worker((void *)(&arg));
}

WEAK float sqrt_f32(float x) {
    return sqrtf(x);
}

WEAK float sin_f32(float x) {
    return sinf(x);
}

WEAK float cos_f32(float x) {
    return cosf(x);
}

WEAK float exp_f32(float x) {
    return expf(x);
}

WEAK float log_f32(float x) {
    return logf(x);
}

WEAK float pow_f32(float x, float y) {
    return powf(x, y);
}

WEAK float floor_f32(float x) {
    return floorf(x);
}

WEAK float ceil_f32(float x) {
    return ceilf(x);
}

WEAK float round_f32(float x) {
    return roundf(x);
}

WEAK float maxval_f32() {return FLT_MAX;}
WEAK float minval_f32() {return -FLT_MAX;}
WEAK double maxval_f64() {return DBL_MAX;}
WEAK double minval_f64() {return -DBL_MAX;}
WEAK uint8_t maxval_u8() {return 0xff;}
WEAK uint8_t minval_u8() {return 0;}
WEAK uint16_t maxval_u16() {return 0xffff;}
WEAK uint16_t minval_u16() {return 0;}
WEAK uint32_t maxval_u32() {return 0xffffffff;}
WEAK uint32_t minval_u32() {return 0;}
WEAK uint64_t maxval_u64() {return 0xffffffffffffffff;}
WEAK uint64_t minval_u64() {return 0;}
WEAK int8_t maxval_s8() {return 0x7f;}
WEAK int8_t minval_s8() {return 0x80;}
WEAK int16_t maxval_s16() {return 0x7fff;}
WEAK int16_t minval_s16() {return 0x8000;}
WEAK int32_t maxval_s32() {return 0x7fffffff;}
WEAK int32_t minval_s32() {return 0x80000000;}
WEAK int64_t maxval_s64() {return 0x7fffffffffffffff;}
WEAK int64_t minval_s64() {return 0x8000000000000000;}

#include <sys/time.h>

#ifndef current_time_defined
#define current_time_defined
WEAK int currentTime() {
    static bool initialized = false;
    static timeval start;
    if (!initialized) {
        gettimeofday(&start, NULL);
        initialized = true;
        return 0;
    } else {
        timeval now;
        gettimeofday(&now, NULL);
        return
            (now.tv_sec - start.tv_sec)*1000 + 
            (now.tv_usec - start.tv_usec)/1000;
    }
}
#endif

}
