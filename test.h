#ifndef _TEST_H
#define _TEST_H

#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <numa.h>

// Align each arg to a cache line
struct thread_args {
    int index;			// "Index" of this node in time arrays (below).
    int iterations;		// Number of iterations in the experiment
    int critical_section;	// Time spent inside critical section
    int outside_section;	// Time spent outside critical section
    pthread_mutex_t *lock;	// Use this lock to notify of start. 
    uint64_t *start_times;	// Array of start times
    uint64_t *end_times;	// Array of end times
    int num_threads;		// The number of threads in experiment. 
    cpu_set_t* cpu_mask;	// 
} __attribute__ ((aligned(128)));

// Layout the fields on separate cache lines. 
struct start_struct {
    volatile uint64_t num_ready;
    volatile uint64_t start_flag;
    volatile uint64_t num_done;
    volatile uint64_t done_flag;
};

int
increment_ready_counter(volatile uint64_t *counter, 
                        volatile uint64_t *flag,
                        int num_threads);

void
wait_for_flag(volatile uint64_t *flag);

void
arg_error();

inline void
warmup_counter();

void
do_experiment(int alloc_policy, 
              int cs_len, 
              int outside_len, 
              int num_threads,
              int iterations);

void
pin_thread(cpu_set_t *cpu);

void*
per_thread_function(void *thread_args);

void
do_pthread(pthread_mutex_t *lock, int iterations, int cs_time, int out_time);

void
do_work(int amount);

#endif
