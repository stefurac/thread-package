// #define _GNU_SOURCE
#include "csc369_thread.h"
#include <ucontext.h>
#include <assert.h>
#include <sys/time.h>
// TODO: You may find this useful, otherwise remove it
//#define DEBUG_USE_VALGRIND // uncomment to debug with valgrind
#ifdef DEBUG_USE_VALGRIND
#include <valgrind/valgrind.h>
#endif
#include "csc369_interrupts.h"
#include <stdio.h>
#include <stdlib.h>

//****************************************************************************
// Private Definitions
//****************************************************************************
// TODO: You may find this useful, otherwise remove it
typedef enum
{
    CSC369_THREAD_FREE = 0,
    CSC369_THREAD_READY = 1,
    CSC369_THREAD_RUNNING = 2,
    CSC369_THREAD_ZOMBIE = 3,
    CSC369_THREAD_BLOCKED = 4
} CSC369_ThreadState;
/**
 * The Thread Control Block.
 */
typedef struct
{
    /**
     * The id of the thread.
    */
    Tid thread_id;
    /**
     * The thread that the current thread yielded to.
    */
    Tid yielded_to;
    /**
     * What code the thread exited with.
     */
    int exit_code;
    /**
   * Whether or not the thread should exit;
   */
    int should_exit;
    /**
     * The state of the thread.
    */
    CSC369_ThreadState state;
    /**
     * The thread context.
     */
    ucontext_t context;
    /**
     * The stack pointer of the thread.
    */
    void* stack_pointer;

    /**
     * The next TCB that the TCB should execute if it is in a queue
    */
    int next;

    int threads_to_collect_exit_code;

    /**
     * The queue of threads that are waiting on this thread to finish.
     */
    CSC369_WaitQueue* join_threads; 
} TCB;

/**
 * A wait queue.
 */
typedef struct csc369_wait_queue_t
{
    TCB *head;
} CSC369_WaitQueue;
//**************************************************************************************************
// Private Global Variables (Library State)
//**************************************************************************************************

/**
 * All possible threads have their control blocks stored contiguously in memory.
 */
TCB threads[CSC369_MAX_THREADS];
/**
 * Threads that are ready to run in FIFO order.
 */
CSC369_WaitQueue ready_threads;
/**
 * Threads that need to be cleaned up.
 */
CSC369_WaitQueue zombie_threads;
/**
 * Thread that is currently running code.
*/
TCB *running_thread;
//**************************************************************************************************
// Helper Functions
//**************************************************************************************************

void
GetContextHelper(TCB *thread) {
  CSC369_InterruptsDisable();
  getcontext(&thread->context);
}

int
SetContextHelper(TCB *thread) {
  CSC369_InterruptsDisable();
  int return_value = setcontext(&thread->context);
  return return_value;
}

int
Queue_IsEmpty(CSC369_WaitQueue* queue)
{
    if (queue->head == NULL) {
        return 1;
    }
    return 0;
}

void
Queue_Enqueue(CSC369_WaitQueue* queue, TCB* tcb)
{
    if (Queue_IsEmpty(queue)) {
        queue->head = &threads[tcb->thread_id];
        tcb->next = -1;
    }
    else {
        TCB *pointer = queue->head;
        while (pointer->next != -1) {
            pointer = &threads[pointer->next];
        }
        pointer->next = tcb->thread_id;
        tcb->next = -1;
    }
}

TCB*
Queue_Dequeue(CSC369_WaitQueue* queue)
{
    TCB *tcb_to_return = queue->head;
    // return nothing if the queue is empty
    if (!tcb_to_return) {
        return NULL;
    }

    // set the head of the queue to the next thread in the queue
    if (queue->head->next >= 0) {
        queue->head = &threads[queue->head->next];
    }
    else {
        queue->head = NULL;
    }
    return tcb_to_return;
}

void
Queue_Remove(CSC369_WaitQueue* queue, TCB* tcb)
{

    TCB *thread_to_remove = &threads[(int) tcb->thread_id];
    TCB *pointer = queue->head;
    if (pointer == NULL) {
        return;
    }
    if (pointer->thread_id == tcb->thread_id) {
        Queue_Dequeue(queue);

    }
    else {
        while (pointer->next != -1 && pointer->next != tcb->thread_id) {
            pointer = &threads[pointer->next];
        }
        if (pointer->next == -1) {
            return;
        }
        pointer->next = thread_to_remove->next;
    }
}

void
TCB_Init(Tid thread_id)
{
    threads[thread_id].thread_id = thread_id;
    threads[thread_id].yielded_to = (Tid) -1;
    threads[thread_id].should_exit = 0;
    threads[thread_id].exit_code = 0;
    threads[thread_id].exit_code = CSC369_EXIT_CODE_NORMAL;
    GetContextHelper(&threads[thread_id]);
    threads[thread_id].state = CSC369_THREAD_FREE;
    // threads[thread_id].stack_pointer = malloc(CSC369_THREAD_STACK_SIZE);
    threads[thread_id].next = -1;
}

int 
FindThreadInQueue(CSC369_WaitQueue *queue, Tid thread_id) 
{
    // if queue doesnt exist, not in queue
    if (queue == NULL) {
        return 0;
    }

    TCB *pointer = queue->head;

    // if queue is empty, not in queue
    if (pointer == NULL) {
        return 0;
    }

    // iterate to either last thread in queue or thread we are looking for
    while (pointer->next != -1) {
        if (pointer->thread_id == thread_id) {
            break;
        }
        pointer = &threads[pointer->next];
    }

    // if pointer is not thread we are looking for, its just the last thread in the queue
    if (pointer->thread_id != thread_id) {
        return 0;
    }
    
    // we found the node we are looking for
    return 1;
}

void FreeZombiesWithoutWaitingThreads() {
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    for (int i = 0; i < CSC369_MAX_THREADS; i++) {
        if (threads[i].state == CSC369_THREAD_ZOMBIE && !FindThreadInQueue(&zombie_threads, threads[i].thread_id) && threads[i].threads_to_collect_exit_code == 0)
            {
                TCB_Init(threads[i].thread_id);
                if (&threads[i].stack_pointer) {
                    free(*&threads[i].stack_pointer);
                }
            }
    }
    CSC369_InterruptsSet(prev_state);

}

void stub_function(void (*f)(void *), void *arg) {
    CSC369_InterruptsEnable();
    // FreeZombiesWithoutWaitingThreads();
    f(arg);
    CSC369_ThreadExit(0);
}

void Context_Init(Tid thread_id, void (*f)(void*), void* arg) {
    greg_t stack_size = CSC369_THREAD_STACK_SIZE - 8; // 8bytes allocated for the pointer
    threads[thread_id].context.uc_stack.ss_size = stack_size;
    threads[thread_id].context.uc_stack.ss_sp = threads[thread_id].stack_pointer;
    threads[thread_id].context.uc_mcontext.gregs[REG_RSP] = (greg_t)threads[thread_id].stack_pointer + stack_size;
    threads[thread_id].context.uc_mcontext.gregs[REG_RIP] = (greg_t)&stub_function;
    threads[thread_id].context.uc_mcontext.gregs[REG_RSI] = (greg_t)arg;
    threads[thread_id].context.uc_mcontext.gregs[REG_RDI] = (greg_t)f;
}

//**************************************************************************************************
// thread.h Functions
//**************************************************************************************************

int
CSC369_ThreadInit(void)
{
    // printf("create main thread");
    // set the first thread in the threads array to be the kernel thread
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();
    Tid thread_id = 0;
    TCB_Init(thread_id);
    threads[thread_id].stack_pointer = malloc(CSC369_THREAD_STACK_SIZE);

    running_thread = &threads[thread_id];
    running_thread->state = CSC369_THREAD_RUNNING;

    if (running_thread->stack_pointer == NULL ) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_OTHER;
    }
    CSC369_InterruptsSet(prev_state);
    return 0;
}

Tid
CSC369_ThreadId(void)
{
    return running_thread->thread_id;
}

Tid
CSC369_ThreadCreate(void (*f)(void*), void* arg)
{
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    int i;
    for (i = 0; i < CSC369_MAX_THREADS; i++) {
        if (threads[i].state == CSC369_THREAD_FREE || (threads[i].state == CSC369_THREAD_ZOMBIE && !FindThreadInQueue(&zombie_threads, threads[i].thread_id))) {
            break;
        }
    }

    if (i == CSC369_MAX_THREADS) {
        return CSC369_ERROR_SYS_THREAD;
    }
    
    if (threads[i].state == CSC369_THREAD_ZOMBIE) {
        free(*&threads[i].stack_pointer);
    } 

    TCB_Init((Tid) i);
    threads[i].stack_pointer = malloc(CSC369_THREAD_STACK_SIZE);

    if (threads[i].stack_pointer == NULL) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_SYS_MEM;
    }
    threads[i].state = CSC369_THREAD_READY;
    Context_Init((Tid) i, f, arg);

    Queue_Enqueue(&ready_threads, &threads[i]);
    // printf("create thread %d\n", threads[i].thread_id);

    CSC369_InterruptsSet(prev_state);
    return i;
}

void
CSC369_ThreadExit(int exit_code)
{
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    // FreeZombiesWithoutWaitingThreads();

    running_thread->state = CSC369_THREAD_ZOMBIE;
    running_thread->exit_code = exit_code;

    if (running_thread->join_threads != NULL) {
        Queue_Enqueue(&zombie_threads, running_thread);
        running_thread->threads_to_collect_exit_code = CSC369_ThreadWakeAll(running_thread->join_threads);
    }

    TCB *tcb = Queue_Dequeue(&ready_threads);
 
    if (!tcb) {
        CSC369_InterruptsSet(prev_state);
        exit(exit_code);
    }
    // printf("switch out of thread %d from exit to %d\n", running_thread->thread_id, tcb->thread_id);
    running_thread = tcb;
    running_thread->state = CSC369_THREAD_RUNNING;
    int ret = SetContextHelper(running_thread);
    if (ret == -1) {
        CSC369_InterruptsSet(prev_state);
        exit(-1);
    }

}

Tid
CSC369_ThreadKill(Tid tid)
{
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    // FreeZombiesWithoutWaitingThreads();

    if (tid == running_thread->thread_id) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_THREAD_BAD;
    }

    if (tid >= CSC369_MAX_THREADS || tid < 0) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_TID_INVALID;
    }

    int thread_id = (int) tid;
    if (threads[thread_id].state == CSC369_THREAD_FREE) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_SYS_THREAD;
    }

    threads[thread_id].exit_code = CSC369_EXIT_CODE_KILL;
    threads[thread_id].state = CSC369_THREAD_ZOMBIE;
    Queue_Remove(&ready_threads, &threads[thread_id]);
    if (threads[thread_id].join_threads != NULL) {
        Queue_Enqueue(&zombie_threads, &threads[thread_id]);
        threads[thread_id].threads_to_collect_exit_code = CSC369_ThreadWakeAll(threads[thread_id].join_threads);
    }
    CSC369_InterruptsSet(prev_state);
    return tid;
}

int
CSC369_ThreadYield()
{
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    GetContextHelper(running_thread);

    if (!running_thread->should_exit) {
        if (Queue_IsEmpty(&ready_threads)) {
            running_thread->yielded_to = running_thread->thread_id;
            running_thread->should_exit = 1;
            // printf("yield to self, thread %d on yield \n", running_thread->thread_id);
            SetContextHelper(running_thread);
        } else {
            TCB *tcb = Queue_Dequeue(&ready_threads);
            running_thread->yielded_to = tcb->thread_id;
            running_thread->should_exit = 1;
            running_thread->state = CSC369_THREAD_READY;
            Queue_Enqueue(&ready_threads, running_thread);
            // printf("yield to thread %d from %d on yield \n", running_thread->thread_id, tcb->thread_id);
            running_thread = tcb;
            running_thread->state = CSC369_THREAD_RUNNING;
            SetContextHelper(running_thread);
        }
    }
    // FreeZombiesWithoutWaitingThreads();

    running_thread->should_exit = 0;
    int yielded_to = running_thread->yielded_to;
    CSC369_InterruptsSet(prev_state);
    // printf("exiting yield with thread %d \n", running_thread->thread_id);
    return yielded_to;
}

int
CSC369_ThreadYieldTo(Tid tid)
{

    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    if (tid >= CSC369_MAX_THREADS || tid < 0) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_TID_INVALID;
    }

    if (threads[(int) tid].state != CSC369_THREAD_READY && running_thread->thread_id != tid) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_THREAD_BAD;
    }

    GetContextHelper(running_thread);

    if (!running_thread->should_exit) {
        if (running_thread->thread_id == tid) {
            running_thread->yielded_to = running_thread->thread_id;
            running_thread->should_exit = 1;
            // printf("yield to self, thread %d on yield to \n", running_thread->thread_id);
            SetContextHelper(running_thread);
        } else {
            TCB *tcb = &threads[(int) tid];
            Queue_Remove(&ready_threads, tcb);
            running_thread->yielded_to = tid;
            running_thread->should_exit = 1;
            running_thread->state = CSC369_THREAD_READY;
            Queue_Enqueue(&ready_threads, running_thread);
            // printf("yield to thread %d from %d on yield to \n", running_thread->thread_id, tcb->thread_id);
            running_thread = tcb;
            running_thread->state = CSC369_THREAD_RUNNING;
            SetContextHelper(running_thread);
        }
    }
    // FreeZombiesWithoutWaitingThreads();

    running_thread->should_exit = 0;
    int yielded_to = running_thread->yielded_to;
    CSC369_InterruptsSet(prev_state);
    // printf("exiting yield to with thread %d \n", running_thread->thread_id);
    return yielded_to;
}
//****************************************************************************
// New Assignment 2 Definitions - Task 2
//****************************************************************************
CSC369_WaitQueue*
CSC369_WaitQueueCreate(void)
{
    // allocate memory for a list of threads that are waiting on running thread
    CSC369_WaitQueue* queue = (CSC369_WaitQueue *) malloc(sizeof(CSC369_WaitQueue));
    if (!queue) {
        return NULL;
    }
    return queue;
}
int
CSC369_WaitQueueDestroy(CSC369_WaitQueue* queue)
{
    if (!queue || !Queue_IsEmpty(queue)) {
        return CSC369_ERROR_OTHER;
    }

    // free memory of an empty wait queue
    free(&running_thread->join_threads->head);
    return 0;
}

void
CSC369_ThreadSpin(int duration)
{
    struct timeval start, end, diff;
    int ret = gettimeofday(&start, NULL);
    assert(!ret);
    while (1) {
        ret = gettimeofday(&end, NULL);
        assert(!ret);
        timersub(&end, &start, &diff);
        if ((diff.tv_sec * 1000000 + diff.tv_usec) >= duration) {
            return;
        }
    }
}
int
CSC369_ThreadSleep(CSC369_WaitQueue* queue)
{    
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    if (Queue_IsEmpty(&ready_threads)) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_SYS_THREAD;
    }

    GetContextHelper(running_thread);

    if (!running_thread->should_exit) {
        TCB *tcb = Queue_Dequeue(&ready_threads);
        running_thread->yielded_to = tcb->thread_id;
        running_thread->should_exit = 1;
        running_thread->state = CSC369_THREAD_BLOCKED;
        Queue_Enqueue(queue, running_thread);
        // printf("thread %d is going to switch to %d in thread sleep \n", running_thread->thread_id, tcb->thread_id);
        running_thread = tcb;
        running_thread->state = CSC369_THREAD_RUNNING;
        SetContextHelper(running_thread);
    }

    running_thread->should_exit = 0;
    int yielded_to = running_thread->yielded_to;
    // printf("thread %d is going to exit from thread sleep \n", running_thread->thread_id);
    CSC369_InterruptsSet(prev_state);
    return yielded_to;
    
}
int
CSC369_ThreadWakeNext(CSC369_WaitQueue* queue)
{
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    if (queue == NULL || queue->head == NULL) {
        CSC369_InterruptsSet(prev_state);
        return 0;
    }

    TCB *tcb = Queue_Dequeue(queue);
    tcb->state = CSC369_THREAD_READY;
    Queue_Enqueue(&ready_threads, tcb);
    CSC369_InterruptsSet(prev_state);
    return 1;
}
int
CSC369_ThreadWakeAll(CSC369_WaitQueue* queue)
{

    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    if (queue == NULL || queue->head == NULL) {
        CSC369_InterruptsSet(prev_state);
        return 0;
    }

    int counter = 1;
    int next_thread_to_wake = queue->head->next;
    
    TCB *tcb = queue->head;
    tcb->state = CSC369_THREAD_READY;
    Queue_Remove(queue, tcb);
    Queue_Enqueue(&ready_threads, tcb);

    if (next_thread_to_wake == -1) {
        CSC369_InterruptsSet(prev_state);
        return 1;
    }
    // printf("wake thread %d \n", tcb->thread_id);
    while (next_thread_to_wake != -1) {
        tcb = &threads[next_thread_to_wake];
        Queue_Remove(queue, tcb);
        next_thread_to_wake = tcb->next;
        counter++;

        tcb->state = CSC369_THREAD_READY;
        Queue_Enqueue(&ready_threads, tcb);
        // printf("wake thread %d \n", tcb->thread_id);
        
    }

    CSC369_WaitQueueDestroy(queue);
    CSC369_InterruptsSet(prev_state);
    return counter;
}
//****************************************************************************
// New Assignment 2 Definitions - Task 3
//****************************************************************************
int
CSC369_ThreadJoin(Tid tid, int* exit_code)
{    
    CSC369_InterruptsState const prev_state = CSC369_InterruptsDisable();

    // printf("adding thread %d to thread %d's join threads wait queue \n", running_thread->thread_id, tid);

    if (tid >= CSC369_MAX_THREADS || tid < 0) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_TID_INVALID;
    }
    if (tid == running_thread->thread_id) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_THREAD_BAD;
    }

    TCB *thread_to_join = &threads[(int) tid];
    *exit_code = thread_to_join->exit_code;

    // thread is either READY, BLOCKED
    if (thread_to_join->state == CSC369_THREAD_FREE || thread_to_join->state == CSC369_THREAD_ZOMBIE || thread_to_join->state == CSC369_THREAD_RUNNING) {
        CSC369_InterruptsSet(prev_state);
        return CSC369_ERROR_SYS_THREAD;
    }

    // allocate memory for the wait queue of the thread this thread is waiting on if none exists
    if (!thread_to_join->join_threads) {
        CSC369_WaitQueue *queue = CSC369_WaitQueueCreate();
        thread_to_join->join_threads = queue;
    }

    // printf("thread %d is going to sleep \n", running_thread->thread_id);
    CSC369_ThreadSleep(thread_to_join->join_threads);
    *exit_code = thread_to_join->exit_code;


    thread_to_join->threads_to_collect_exit_code--;
    if (thread_to_join->threads_to_collect_exit_code == 0 && FindThreadInQueue(&zombie_threads, thread_to_join->thread_id)) {
        Queue_Remove(&zombie_threads, thread_to_join);
        // free(*thread_to_joinstack_pointer);
    }
    
    CSC369_InterruptsSet(prev_state);
    return tid;
}

// int
// yield_till_main_thread(void)
// {
//   int num_yields = 0;

//   // Yield until we are back at the main thread
//   int result;
//   do {
//     result = CSC369_ThreadYield();
//     // ck_assert_int_ge(result, 0);
//     // ck_assert_int_lt(result, CSC369_MAX_THREADS);

//     num_yields++;
//   } while (result != 0);

//   return num_yields;
// }

// void
// f_no_exit(void)
// {
//   while (1) {
//     printf("%d \n", CSC369_InterruptsAreEnabled());
//   }
// }

// void
// f_no_exit(void)
// {
//   while (1) {
//     printf("%d\n", CSC369_InterruptsAreEnabled());
//   }
// }


// int
// main()
// {
//   CSC369_ThreadInit();
//   CSC369_InterruptsInit();
//   CSC369_InterruptsSetLogLevel(CSC369_INTERRUPTS_VERBOSE);

//     Tid const tid = CSC369_ThreadCreate((void (*)(void*))f_no_exit, NULL);
// //   ck_assert_int_gt(tid, 0);
// //   ck_assert_int_lt(tid, CSC369_MAX_THREADS);

//   // Turn the newly created thread into a zombie
//   int ret2 = CSC369_ThreadKill(tid);
// //   , tid);

//   CSC369_ThreadSpin(CSC369_INTERRUPTS_SIGNAL_INTERVAL * 2);

//   int exit_value;
//   int ret = CSC369_ThreadJoin(tid, &exit_value);
//   return 0;
// //   , CSC369_ERROR_SYS_THREAD);
// //   ck_assert_int_eq(exit_value, CSC369_EXIT_CODE_KILL);
// }
