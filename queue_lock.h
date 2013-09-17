#ifndef QUEUE_LOCK_H
#define QUEUE_LOCK_H

// Make sure that a queue struct can own an entire cache line. 
struct queue_struct {
    volatile struct queue_struct *prev;
} __attribute__((aligned(128)));

static volatile struct queue_struct *tail = NULL;

static inline uint64_t
cmp_and_swap(volatile struct queue_struct *n, 
             volatile struct queue_struct *cmp)
{
    struct queue_struct *out;
    asm volatile("lock; cmpxchgq %2, %1;"
                 : "=a" (out), "+m" (tail)
                 : "q" (n), "0"(cmp)
                 : "cc");
    return out == cmp;
}


void
enqueue(volatile struct queue_struct* next)
{
    while (1) {
        next->prev = tail;
        if (cmp_and_swap(next, next->prev)) 
            break;
    }
}

#endif
