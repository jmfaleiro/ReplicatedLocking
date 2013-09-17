#ifndef QUEUE_LOCK_H
#define QUEUE_LOCK_H

struct queue_struct {
    struct queue_struct *prev;
} __attribute__((aligned(128)));

static volatile struct queue_struct *tail;

static inline uint64_t
cmp_and_swap(volatile struct queue_struct *other, 
             volatile struct queue_struct *n, 
             uint64_t cmp)
{
    struct queue_struct *out;
    asm volatile(
                 "lock; cmpxchgq %2, %1"
                 : "=a" (out), "+m" (other)
                 : "q" (cmp), "0"(n)
                 : "cc");
    return out == n;
}


void
enqueue(volatile struct queue_struct* next)
{
    while (1) {
        next->prev = (struct queue_struct*)tail;
        if (cmp_and_swap(tail, next, (uint64_t)next->prev)) 
            break;
    }
}

#endif
