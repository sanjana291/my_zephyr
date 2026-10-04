/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * src/osal/osal_zephyr.c — Zephyr RTOS OSAL implementation.
 *
 */

#include <iol-master/osal.h>

#include <stdlib.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(iol_osal, CONFIG_IOL_MASTER_LOG_LEVEL);

/* ================================================================== *
 *  Resource-pool sizing                                               *
 * ================================================================== */
#define TIMERS_PER_PORT   3
#define MUTEX_PER_PORT    6
#define EVT_PER_PORT      2
#define THREADS_PER_PORT  1
#define MBOX_PER_PORT     2

/* Add a small overhead for the master-level thread and misc objects. */
#define TOTAL_TIMERS   (TIMERS_PER_PORT  * CONFIG_IOL_MASTER_NUM_CHANNELS + 2)
#define TOTAL_MUTEX    (MUTEX_PER_PORT   * CONFIG_IOL_MASTER_NUM_CHANNELS + 4)
#define TOTAL_EVT      (EVT_PER_PORT     * CONFIG_IOL_MASTER_NUM_CHANNELS + 4)
#define TOTAL_THREADS  (THREADS_PER_PORT * CONFIG_IOL_MASTER_NUM_CHANNELS + 1)
#define TOTAL_MBOX     (MBOX_PER_PORT    * CONFIG_IOL_MASTER_NUM_CHANNELS + 2)

/* ================================================================== *
 *  Static pools                                                       *
 * ================================================================== */
static os_timer_t     timer_pool  [TOTAL_TIMERS];
static struct k_mutex mutex_pool  [TOTAL_MUTEX];
static struct k_event evt_pool    [TOTAL_EVT];
static os_mbox_t      mbox_pool   [TOTAL_MBOX];
static struct k_thread thread_pool[TOTAL_THREADS];

/*
 * Thread stacks MUST be declared with Zephyr's K_THREAD_STACK_ARRAY_DEFINE.
 */
K_THREAD_STACK_ARRAY_DEFINE(thread_stacks, TOTAL_THREADS,
                             CONFIG_IOL_MASTER_STACK_SIZE);

/* ================================================================== *
 *  Pool slot tracking — free-slot bitmasks                           *
 *                                                                     *
 *  Each bit represents one pool slot.  1 = in use, 0 = free.        *
 *  Using uint32_t supports up to 32 slots per pool; increase to      *
 *  uint64_t if TOTAL_* ever exceeds 32.                              *
 *                                                                     *
 *  This replaces the old monotonic counters (timer_cnt, mutex_cnt,   *
 *  etc.) that could never reclaim destroyed slots.                   *
 * ================================================================== */
static uint32_t timer_mask;
static uint32_t mutex_mask;
static uint32_t evt_mask;
static uint32_t thread_mask;
static uint32_t mbox_mask;

/* Return index of the first 0-bit (free slot), or -1 if all full. */
static int pool_alloc(uint32_t *mask, int total)
{
    for (int i = 0; i < total; i++) {
        if (!(*mask & (1u << i))) {
            *mask |= (1u << i);
            return i;
        }
    }
    return -1; /* pool exhausted */
}

/* Clear the bit for slot idx, marking it free for reuse. */
static void pool_free(uint32_t *mask, int idx)
{
    if (idx >= 0) {
        *mask &= ~(1u << idx);
    }
}

/* ================================================================== *
 *  Internal helpers                                                   *
 * ================================================================== */
static k_timeout_t osal_to_zephyr_tmo(uint32_t osal_ms)
{
    return (osal_ms == OS_WAIT_FOREVER) ? K_FOREVER : K_MSEC(osal_ms);
}

/* ================================================================== *
 *  Memory                                                             *
 * ================================================================== */
void *os_malloc(size_t size)
{
    return k_malloc(size);
}

void os_free(void *ptr)
{
    k_free(ptr);
}

/* ================================================================== *
 *  Time                                                               *
 * ================================================================== */
void os_usleep(uint32_t us)
{
    k_sleep(K_USEC(us));
}

uint32_t os_get_current_time_us(void)
{
    /* k_uptime_get() returns ms; convert to µs (may overflow after ~71 min) */
    return (uint32_t)(k_uptime_get() * 1000U);
}

os_tick_t os_tick_current(void)
{
    return k_uptime_ticks();
}

os_tick_t os_tick_from_us(uint32_t us)
{
    return k_us_to_ticks_ceil32(us);
}

void os_tick_sleep(os_tick_t tick)
{
    k_sleep(K_TICKS(tick));
}

/* ================================================================== *
 *  Thread                                                             *
 * ================================================================== */
static void os_thread_entry_wrapper(void *p1, void *p2, void *p3)
{
    void (*fn)(void *) = (void (*)(void *))p1;
    fn(p2);
    ARG_UNUSED(p3);
}

os_thread_t *os_thread_create(
    const char   *name,
    uint32_t      priority,
    void         *const stkSto,     /* ignored — stacks come from pool */
    const uint16_t stkSize,         /* ignored — see K_THREAD_STACK_ARRAY_DEFINE */
    void        (*entry)(void *arg),
    void         *arg)
{
    ARG_UNUSED(stkSto);
    ARG_UNUSED(stkSize);

    int idx = pool_alloc(&thread_mask, TOTAL_THREADS);
    if (idx < 0) {
        LOG_ERR("os_thread_create: pool exhausted (%d slots)", TOTAL_THREADS);
        return NULL;
    }

    struct k_thread  *t     = &thread_pool[idx];
    k_thread_stack_t *stack = thread_stacks[idx];

    k_tid_t tid = k_thread_create(
        t, stack, CONFIG_IOL_MASTER_STACK_SIZE,
        os_thread_entry_wrapper,
        (void *)entry, arg, NULL,
        (int)priority,
        0,           /* options */
        K_NO_WAIT);

    if (tid == NULL) {
        LOG_ERR("os_thread_create: k_thread_create returned NULL");
        pool_free(&thread_mask, idx);   /* give the slot back immediately */
        return NULL;
    }

    if (name != NULL) {
        k_thread_name_set(t, name);
    }

    LOG_DBG("Thread '%s' created at slot %d (prio=%u)", name ? name : "?",
            idx, priority);
    return (os_thread_t *)t;
}

/*
 * os_thread_destroy()
 *
 * Aborts the thread and releases its pool slot and stack back for
 * reuse by the next os_thread_create() call.
 */
void os_thread_destroy(os_thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    struct k_thread *t = (struct k_thread *)thread;

    /* Locate the pool index so we can free the slot. */
    int idx = -1;
    for (int i = 0; i < TOTAL_THREADS; i++) {
        if (&thread_pool[i] == t) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        LOG_ERR("os_thread_destroy: thread %p not from pool", (void *)t);
        return;
    }

    /* Abort first — Zephyr must not schedule onto a pool slot we're
     * about to mark free.                                             */
    k_thread_abort(t);

    /* Release the slot and stack back to the pool. */
    pool_free(&thread_mask, idx);

    LOG_DBG("Thread slot %d released (mask=0x%08x)", idx, thread_mask);
}

/* ================================================================== *
 *  Mutex                                                              *
 * ================================================================== */
os_mutex_t *os_mutex_create(void)
{
    int idx = pool_alloc(&mutex_mask, TOTAL_MUTEX);
    if (idx < 0) {
        LOG_ERR("os_mutex_create: pool exhausted");
        return NULL;
    }
    struct k_mutex *m = &mutex_pool[idx];
    k_mutex_init(m);
    return (os_mutex_t *)m;
}

void os_mutex_lock(os_mutex_t *mutex)
{
    k_mutex_lock((struct k_mutex *)mutex, K_FOREVER);
}

void os_mutex_unlock(os_mutex_t *mutex)
{
    k_mutex_unlock((struct k_mutex *)mutex);
}

void os_mutex_destroy(os_mutex_t *mutex)
{
    struct k_mutex *m = (struct k_mutex *)mutex;
    int idx = -1;
    for (int i = 0; i < TOTAL_MUTEX; i++) {
        if (&mutex_pool[i] == m) {
            idx = i;
            break;
        }
    }
    pool_free(&mutex_mask, idx);
}

/* ================================================================== *
 *  Semaphore                                                          *
 *  Allocated from heap (not pool) because they are used sparsely.    *
 * ================================================================== */
os_sem_t *os_sem_create(size_t count)
{
    struct k_sem *s = k_malloc(sizeof(struct k_sem));
    if (s == NULL) {
        LOG_ERR("os_sem_create: k_malloc failed");
        return NULL;
    }
    k_sem_init(s, (unsigned int)count, K_SEM_MAX_LIMIT);
    return (os_sem_t *)s;
}

bool os_sem_wait(os_sem_t *sem, uint32_t time)
{
    int rc = k_sem_take((struct k_sem *)sem, osal_to_zephyr_tmo(time));
    return (rc != 0); /* true = timed out */
}

void os_sem_signal(os_sem_t *sem)
{
    k_sem_give((struct k_sem *)sem);
}

void os_sem_destroy(os_sem_t *sem)
{
    k_free(sem);
}

/* ================================================================== *
 *  Event flags — k_event provides a direct 32-bit flag word          *
 * ================================================================== */
os_event_t *os_event_create(void)
{
    int idx = pool_alloc(&evt_mask, TOTAL_EVT);
    if (idx < 0) {
        LOG_ERR("os_event_create: pool exhausted");
        return NULL;
    }
    struct k_event *e = &evt_pool[idx];
    k_event_init(e);
    return (os_event_t *)e;
}

bool os_event_wait(os_event_t *event, uint32_t mask,
                   uint32_t *value, uint32_t time)
{
    uint32_t bits = k_event_wait((struct k_event *)event, mask,
                                 false,  /* do not clear on match */
                                 osal_to_zephyr_tmo(time));
    *value = bits & mask;
    return (*value == 0); /* true → timed out */
}

void os_event_set(os_event_t *event, uint32_t value)
{
    k_event_post((struct k_event *)event, value);
}

void os_event_clr(os_event_t *event, uint32_t value)
{
    k_event_clear((struct k_event *)event, value);
}

void os_event_destroy(os_event_t *event)
{
    struct k_event *e = (struct k_event *)event;
    int idx = -1;
    for (int i = 0; i < TOTAL_EVT; i++) {
        if (&evt_pool[i] == e) {
            idx = i;
            break;
        }
    }
    pool_free(&evt_mask, idx);
}

/* ================================================================== *
 *  Mailbox — pointer ring-buffer                                      *
 *  Signal bits:  0x01 = data available,  0x02 = space available      *
 * ================================================================== */
os_mbox_t *os_mbox_create(size_t size)
{
    int idx = pool_alloc(&mbox_mask, TOTAL_MBOX);
    if (idx < 0) {
        LOG_ERR("os_mbox_create: pool exhausted");
        return NULL;
    }
    if (size > MAX_MAILBOX_MSGS) {
        LOG_ERR("os_mbox_create: requested %zu > MAX_MAILBOX_MSGS %d",
                size, MAX_MAILBOX_MSGS);
        pool_free(&mbox_mask, idx);
        return NULL;
    }
    os_mbox_t *mb = &mbox_pool[idx];
    k_mutex_init(&mb->mutex);
    k_event_init(&mb->evt);
    mb->r     = 0;
    mb->w     = 0;
    mb->count = 0;
    mb->size  = (uint32_t)size;
    return mb;
}

bool os_mbox_fetch(os_mbox_t *mbox, void **msg, uint32_t time)
{
    k_timeout_t tmo = osal_to_zephyr_tmo(time);

    /* Fast path — data already available */
    k_mutex_lock(&mbox->mutex, K_FOREVER);
    if (mbox->count == 0) {
        k_mutex_unlock(&mbox->mutex);
        /* Block until data arrives or timeout */
        uint32_t bits = k_event_wait(&mbox->evt, 0x01U, true, tmo);
        if (bits == 0) {
            return true; /* timeout */
        }
        k_mutex_lock(&mbox->mutex, K_FOREVER);
    }

    *msg        = mbox->msg[mbox->r];
    mbox->r     = (mbox->r + 1) % mbox->size;
    mbox->count--;
    k_event_post(&mbox->evt, 0x02U); /* signal space available */
    k_mutex_unlock(&mbox->mutex);
    return false;
}

bool os_mbox_post(os_mbox_t *mbox, void *msg, uint32_t time)
{
    k_timeout_t tmo = osal_to_zephyr_tmo(time);

    /* Fast path — space already available */
    k_mutex_lock(&mbox->mutex, K_FOREVER);
    if (mbox->count == mbox->size) {
        k_mutex_unlock(&mbox->mutex);
        /* Block until space opens or timeout */
        uint32_t bits = k_event_wait(&mbox->evt, 0x02U, true, tmo);
        if (bits == 0) {
            return true; /* timeout */
        }
        k_mutex_lock(&mbox->mutex, K_FOREVER);
    }

    mbox->msg[mbox->w] = msg;
    mbox->w     = (mbox->w + 1) % mbox->size;
    mbox->count++;
    k_event_post(&mbox->evt, 0x01U); /* signal data available */
    k_mutex_unlock(&mbox->mutex);
    return false;
}

void os_mbox_destroy(os_mbox_t *mbox)
{
    os_mbox_t *mb = (os_mbox_t *)mbox;
    int idx = -1;
    for (int i = 0; i < TOTAL_MBOX; i++) {
        if (&mbox_pool[i] == mb) {
            idx = i;
            break;
        }
    }
    pool_free(&mbox_mask, idx);
}

/* ================================================================== *
 *  Timer                                                              *
 * ================================================================== */
static void timer_zephyr_cb(struct k_timer *ztimer)
{
    os_timer_t *t = (os_timer_t *)ztimer->user_data;
    if (t->fn != NULL) {
        t->fn(t, t->arg);
    }
}

os_timer_t *os_timer_create(
    uint32_t us,
    void (*fn)(os_timer_t *, void *arg),
    void *arg,
    bool oneshot)
{
    int idx = pool_alloc(&timer_mask, TOTAL_TIMERS);
    if (idx < 0) {
        LOG_ERR("os_timer_create: pool exhausted");
        return NULL;
    }
    os_timer_t *t = &timer_pool[idx];
    t->fn         = fn;
    t->arg        = arg;
    t->us         = us;
    t->one_shot   = oneshot;

    k_timer_init(&t->handle, timer_zephyr_cb, NULL);
    t->handle.user_data = t;
    return t;
}

void os_timer_set(os_timer_t *timer, uint32_t us)
{
    timer->us = us;
}

void os_timer_start(os_timer_t *timer)
{
    k_timeout_t dur    = K_USEC(timer->us);
    /* For one-shot: period = K_NO_WAIT stops the timer after first expiry. */
    k_timeout_t period = timer->one_shot ? K_NO_WAIT : K_USEC(timer->us);
    k_timer_start(&timer->handle, dur, period);
}

void os_timer_stop(os_timer_t *timer)
{
    k_timer_stop(&timer->handle);
}

void os_timer_destroy(os_timer_t *timer)
{
    k_timer_stop(&timer->handle);
    os_timer_t *t = (os_timer_t *)timer;
    int idx = -1;
    for (int i = 0; i < TOTAL_TIMERS; i++) {
        if (&timer_pool[i] == t) {
            idx = i;
            break;
        }
    }
    pool_free(&timer_mask, idx);
}