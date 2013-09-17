#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <numa.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>
#include <limits.h>

#include "util.h"
#include "test.h"
#include "cpuinfo.h"

pthread_mutex_t experiment_lock = PTHREAD_MUTEX_INITIALIZER;
volatile struct start_struct *start;

int
increment_ready_counter(volatile uint64_t *counter, 
                        volatile uint64_t *flag, 
                        int num_threads) 
{
    uint64_t counter_value = fetch_and_increment(counter); 
    // If true, all threads are ready to go. Signal them by writing a 1 
    // to the start_flag. 
    if (counter_value == num_threads) {
        xchgq(counter, 0);
        xchgq(flag, 1);
        return 1;
    }
    else 
        return 0;

}

// Wait for someone to signal the start flag. Avoid using pthread mechanisms
// to signal to avoid any library/kernel overhead. 
void
wait_for_flag(volatile uint64_t *flag)
{
    while (*flag == 0) 
        ;
}

void
arg_error() 
{
    fprintf(stdout,
            "Expected arguments:\n"
            "-a 0: alloc per socket, 1: alloc per cpu\n"
            "-c critical section length\n"
            "-o time spent outside the critical section\n"
            "-n number of threads\n"
            "-i number of iterations in the program\n"
            "-e type of lock to use\n"
            "-r number of times to run the experiment\n");

    exit(-1);
}

// Warm up, just because. 
inline void
warmup_counter()
{
    int i;
    for (i = 0; i < 1000; ++i) {
        rdtsc();
    }
}

// Assumes that all the values in the array are non-zero. 
int
find_min(int size,
         uint64_t *values)
{
    uint64_t min_value = ULONG_MAX;
    uint64_t min_index = -1;
    int i;
    for (i = 0; i < size; ++i) 
        if (values[i] < min_value) {
            min_value = values[i];
            min_index = i;
        }
    return min_index;
}

// Assumes that all the values in the array are non-zero. 
int
find_max(int size,
         uint64_t *values)
{
    uint64_t max_value = 0;
    int max_index = -1;
    int i;
    for (i = 0; i < size; ++i) 
        if (values[i] >  max_value) {
            max_value = values[i];
            max_index = i;
        }
    return max_index;
}

void
do_output(int num_threads,
          int num_runs,
          uint64_t **start_times,
          uint64_t **end_times) 
{    
    uint64_t times[num_runs];
    int i;
    fprintf(stdout, "thread times:\n");
    for (i = 0; i < num_runs; ++i) {
        int start_index = find_min(num_threads, start_times[i]);
        int end_index = find_max(num_threads, end_times[i]);
        times[i] = end_times[i][end_index] - start_times[i][start_index];
        fprintf(stdout, "run %d: %lu\n", i, times[i]);
    }
}

void
do_experiment(int alloc_policy, 
              int cs_len, 
              int outside_len, 
              int num_threads,
              int iterations,
              int experiment,
              int num_runs)
{
    cpu_set_t cpu_alloc[num_threads];

    // Assign each thread a cpu to bind to.
    int i;
    for (i = 0; i < num_threads; ++i) {
        int cpu_number = get_cpu(i, alloc_policy);

        // Allocate a new bitmask, zero out its bits and write a single bit 
        // corresponding to the cpu we want to run on.
        CPU_ZERO(&cpu_alloc[i]);
        CPU_SET(cpu_number, &cpu_alloc[i]);
    }

    // Initialize the set of start_structs to coordinate each run of 
    // the experiment.
    start = 
        (struct start_struct *)malloc(sizeof(struct start_struct) * num_runs);
    memset((void *)start, 0, sizeof(struct start_struct) * num_runs);

    // Output arrays for start and end times of each thread. 
    uint64_t **start_times = 
        (uint64_t **)malloc(sizeof(uint64_t*) * num_runs);
    uint64_t **end_times = 
        (uint64_t **)malloc(sizeof(uint64_t*) * num_runs);

    for (i = 0; i < num_runs; ++i) {
        start_times[i] = (uint64_t *)malloc(sizeof(uint64_t) * num_threads);
        end_times[i] = (uint64_t *)malloc(sizeof(uint64_t) * num_threads);
        
        int j;
        for (j = 0; j < num_threads; ++j) {
            start_times[i][j] = 0;
            end_times[i][j] = 0;
        }
    }
    
    // Allocate an array of thread arguments, one element for each thread, and
    // initialize each argument.
    struct thread_args *args = 
        (struct thread_args *)malloc(sizeof(struct thread_args) * num_threads);
    for (i = 0; i < num_threads; ++i) {
        args[i].index = i;
        args[i].cpu_mask = &cpu_alloc[i];
        args[i].iterations = iterations;
        args[i].critical_section = cs_len;
        args[i].outside_section = outside_len;
        args[i].lock = &experiment_lock;
        args[i].start_times = start_times;
        args[i].end_times = end_times;
        args[i].num_runs = num_runs;
        args[i].num_threads = num_threads;
    }
    
    void *(*thread_function)(void *);
    if (experiment == 0) 
        thread_function = per_thread_function;
    else 
        thread_function = per_replicated_thread_function;

    // Allocate and run worker threads.
    pthread_t worker_threads[num_threads];
    for (i = 0; i < num_threads; ++i) 
        pthread_create(&worker_threads[i], 
                       NULL, 
                       thread_function, 
                       (void *)&args[i]);
    
    // Wait for the threads to finish.
    for (i = 0; i < num_threads; ++i) 
        pthread_join(worker_threads[i], NULL);
    
    do_output(num_threads, num_runs, start_times, end_times);
}

void
pin_thread(cpu_set_t *cpu) 
{
    // Kill the program if we can't bind. 
    pthread_t self = pthread_self();
    if (pthread_setaffinity_np(self, sizeof(cpu_set_t), cpu) < 0) {
        fprintf(stderr, "Couldn't bind to my cpu!\n");
        exit(-1);
    }
}

void*
per_thread_function(void *thread_args)
{
    struct thread_args *args = (struct thread_args *)thread_args;
    pin_thread(args->cpu_mask);
    int iterations = args->iterations;
    int cs_len = args->critical_section;
    pthread_mutex_t *mutex = args->lock;
    int num_runs = args->num_runs;

    int i;
    for (i = 0; i < num_runs; ++i) {    
    
        // Signal that this thread is ready to go, then wait for other threads
        // to get ready. 
        if (increment_ready_counter(&(start[i]).num_ready, 
                                    &(start[i]).start_flag, 
                                    args->num_threads) == 0) 
            wait_for_flag(&(start[i]).start_flag);

        // Do the actual experiment. 
        uint64_t start_time = rdtsc();
        do_pthread(args->lock, 
                   iterations, 
                   args->critical_section,
                   args->outside_section);
        uint64_t end_time = rdtsc();
    
        // Wait for everyone to finish. 
        if (increment_ready_counter(&(start[i]).num_done, 
                                    &(start[i]).done_flag, 
                                    args->num_threads) == 0) 
            wait_for_flag(&(start[i]).done_flag);

    
        // We don't care if threads step on each other here (performance wise).
        (args->start_times)[i][args->index] = start_time;
        (args->end_times)[i][args->index] = end_time;
    }
    return args;
}

void*
per_replicated_thread_function(void *thread_args)
{

    struct thread_args *args = (struct thread_args *)thread_args;
    pin_thread(args->cpu_mask);
    int iterations = args->iterations;
    int cs_len = args->critical_section;
    int out_len = args->outside_section;
    int num_runs = args->num_runs;
    
    // Allocate and initialize an array of queue_structs. 
    int i, j;  
    struct queue_struct *cs_args = 
        (struct queue_struct *)malloc(sizeof(struct queue_struct) * iterations);

    for (j = 0; j < num_runs; ++j) {

        for (i = 0; i < iterations; ++i) {
            cs_args[i].prev = NULL;
        }        

        // Signal that this thread is ready to go, then wait for other threads
        // to get ready. 
        if (increment_ready_counter(&(start[j]).num_ready, 
                                    &(start[j]).start_flag, 
                                    args->num_threads) == 0) {
            wait_for_flag(&(start[j]).start_flag);
        }

        uint64_t start_time = rdtsc();
        do_replicated(cs_args, iterations, cs_len, out_len);
        uint64_t end_time = rdtsc();
    
        // Wait for everyone to finish.
        if (increment_ready_counter(&(start[j]).num_done,
                                    &(start[j]).done_flag,
                                    args->num_threads) == 0) {
            wait_for_flag(&(start[j]).done_flag);
        }    
     
        // We don't care if threads step on each other here (performance wise).
        (args->start_times)[j][args->index] = start_time;
        (args->end_times)[j][args->index] = end_time;
    }
    return args;    
}

// Do all pending work before processing cur. 
inline void
process_recursive(volatile struct queue_struct *last, 
                  volatile struct queue_struct *cur,
                  int cs_time)
{
    if (last == cur) {
        return;
    }
    else {
        process_recursive(last, cur->prev, cs_time);
        do_work(cs_time);
    }
}

inline void
do_replicated(struct queue_struct *cs_args, 
              int iterations, 
              int cs_time, 
              int out_time)
{
    volatile struct queue_struct *last = NULL;
    int i;
    for (i = 0; i < iterations; ++i) {

        // Create a new argument struct and enqueue it.
        volatile struct queue_struct *cur = &cs_args[i];
        enqueue(cur);
        cur = &cs_args[i];

        // Recursively perform all pending work, including the just enqueued
        // critical section.
        process_recursive(last, cur, cs_time);
        last = cur;
        do_work(out_time);
    }
}

void
do_pthread(pthread_mutex_t *lock, int iterations, int cs_time, int out_time)
{
    int i;
    for (i = 0; i < iterations; ++i) {
        pthread_mutex_lock(lock);
        do_work(cs_time);		// The "critical section"
        pthread_mutex_unlock(lock);
        do_work(out_time);		// Time "outside the critical section"
    }
}

// Make sure to benchmark this function before you actually use it
// in a benchmark. Measurements on morz and smorz showed that a single
// nop costs 2 cycles. Use nop as the "smallest unit of work". 
void
do_work(int amount)
{
    int i;
    for (i = 0; i < amount; ++i) 
        single_work();
}


int
main(int argc, char **argv) 
{
    init_cpuinfo();
    char *argstring = "a:c:o:n:i:e:r:";        

    int alloc_policy = -1;
    int cs_len = -1;
    int outside_len = -1;
    int num_threads = -1;
    int iterations = -1;    
    int experiment = -1;
    int runs = -1;

    int c;
    while ((c = getopt(argc, argv, argstring)) != -1) {
        switch (c) {
        case 'r':
            if (runs != -1)
                arg_error();
            runs = atoi(optarg);
            break;
        case 'a':
            if (alloc_policy != -1)
                arg_error();
            alloc_policy = atoi(optarg);
            break;
        case 'c':
            if (cs_len != -1)
                arg_error();
            cs_len = atoi(optarg);
            break;
        case 'o':
            if (outside_len != -1)
                arg_error();
            outside_len = atoi(optarg);
            break;           
        case 'n':
            if (num_threads != -1)
                arg_error();
            num_threads = atoi(optarg);
            break;
        case 'i':
            if (iterations != -1)
                arg_error();
            iterations = atoi(optarg);
            break;
        case 'e':
            if (experiment != -1)
                arg_error();
            experiment = atoi(optarg);
            break;
        default:
            arg_error();
        }
    }

    if ((alloc_policy == -1) || 
        (cs_len == -1) || 
        (outside_len == -1) ||
        (num_threads == -1) ||
        (iterations == -1) ||
        (experiment == -1)) {
        arg_error();
    }

    do_experiment(alloc_policy, 
                  cs_len, 
                  outside_len, 
                  num_threads, 
                  iterations,
                  experiment,
                  runs);
    return 0;
}
