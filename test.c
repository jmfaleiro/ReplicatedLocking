#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <numa.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>

#include "util.h"
#include "test.h"
#include "cpuinfo.h"

pthread_mutex_t experiment_lock = PTHREAD_MUTEX_INITIALIZER;
volatile struct start_struct start;
volatile struct queue_struct *tail = NULL;

int
increment_ready_counter(volatile uint64_t *counter, 
                        volatile uint64_t *flag, 
                        int num_threads) 
{
    uint64_t counter_value = fetch_and_increment(counter); 
    // If true, all threads are ready to go. Signal them by writing a 1 
    // to the start_flag. 
    if (counter_value == num_threads) {
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
            "-e type of lock to use\n");

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

void
do_output(int num_threads,
          uint64_t *start_times,
          uint64_t *end_times) 
{    
    int i;
    fprintf(stdout, "thread times:\n");
    for (i = 0; i < num_threads; ++i) {
        fprintf(stdout, "Thread %d start: %lu\n", i, start_times[i]);
        fprintf(stdout, "Thread %d end: %lu\n", i, end_times[i]);
    }
}

void
do_experiment(int alloc_policy, 
              int cs_len, 
              int outside_len, 
              int num_threads,
              int iterations,
              int experiment)
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

    // Output arrays for start and end times of each thread. 
    uint64_t *start_times = (uint64_t *)malloc(sizeof(uint64_t) * num_threads);
    uint64_t *end_times = (uint64_t *)malloc(sizeof(uint64_t) * num_threads);
    
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
        args[i].num_threads = num_threads;
    }
    
    void *(*thread_function)(void *);
    if (experiment == 0) {
        thread_function = per_thread_function;
    }
    else {
        thread_function = per_replicated_thread_function;
    }

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
    
    do_output(num_threads, start_times, end_times);
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
    
    // Signal that this thread is ready to go, then wait for other threads
    // to get ready. 
    if (increment_ready_counter(&start.num_ready, 
                                &start.start_flag, 
                                args->num_threads) == 0) 
        wait_for_flag(&start.start_flag);

    // Do the actual experiment. 
    uint64_t start_time = rdtsc();
    do_pthread(args->lock, 
               iterations, 
               args->critical_section,
               args->outside_section);
    uint64_t end_time = rdtsc();
    
    // Wait for everyone to finish. 
    if (increment_ready_counter(&start.num_done, 
                                &start.done_flag, 
                                args->num_threads) == 0) 
        wait_for_flag(&start.done_flag);
    
    // We don't care if threads step on each other here (performance wise).
    (args->start_times)[args->index] = start_time;
    (args->end_times)[args->index] = end_time;
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
    
    // Allocate and initialize an array of queue_structs. 
    int i;    
    struct queue_struct *cs_args = 
        (struct queue_struct *)malloc(sizeof(struct queue_struct) * iterations);
    for (i = 0; i < iterations; ++i) {
        cs_args[i].next = NULL;
    }        
    
    // Initialize the pointer from which we're going to start. 
    volatile struct queue_struct **start_struct = &tail;

    // Signal that this thread is ready to go, then wait for other threads
    // to get ready. 
    if (increment_ready_counter(&start.num_ready, 
                                &start.start_flag, 
                                args->num_threads) == 0) 
        wait_for_flag(&start.start_flag);


    uint64_t start_time = rdtsc();
    do_replicated(start_struct, cs_args, iterations, cs_len, out_len);
    uint64_t end_time = rdtsc();
    
    // Wait for everyone to finish.
    if (increment_ready_counter(&start.num_done,
                                &start.done_flag,
                                args->num_threads) == 0)
        wait_for_flag(&start.done_flag);
    
    // We don't care if threads step on each other here (performance wise).
    (args->start_times)[args->index] = start_time;
    (args->end_times)[args->index] = end_time;
    return args;    
}

void
do_replicated(volatile struct queue_struct **next,
              struct queue_struct *cs_args, 
              int iterations, 
              int cs_time, 
              int out_time)
{
    int i;
    for (i = 0; i < iterations; ++i) {

        // Create a new argument struct and enqueue it.
        struct queue_struct *cur = &cs_args[i];


        // XXX: We need the following two operations to be atomic, otherwise 
        // we could get pre-empted in between. 
        struct queue_struct *prev = 
            (struct queue_struct *)xchgq((uint64_t *)&tail,                 
                                         (uint64_t)cur);
        if (prev != NULL)
            prev->next = cur;

        while (*next == NULL)
            ;
        
        // First perform any pending work. 
        while (*next != cur) {
            do_work(cs_time);
            next = (volatile struct queue_struct **)&((*next)->next);
            while (*next == NULL)
                ;
        }
        
        // Then execute the current request.
        do_work(cs_time);
        next = (volatile struct queue_struct **)&((*next)->next);
        
        // Spin outside the critical section. 
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
    char *argstring = "a:c:o:n:i:e:";        

    int alloc_policy = -1;
    int cs_len = -1;
    int outside_len = -1;
    int num_threads = -1;
    int iterations = -1;    
    int experiment = -1;

    int c;
    while ((c = getopt(argc, argv, argstring)) != -1) {
        switch (c) {
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
                  experiment);
    return 0;
}
