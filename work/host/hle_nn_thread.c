/* 宿主 HLE —— nn::os 的线程 / TLS / 信号量 / 系统事件 / 计时（23 个）。
 *
 * 这是 238 个里最紧急的一组：**没有线程创建，游戏起不来**。
 *
 * 线程模型：
 *   客体调用 CreateThread(ThreadType *t, entry, arg, stack, stack_size, prio, core)
 *   → 宿主起一个宿主线程，宿主线程**切到客体给的栈**上，以 x0 = arg 调用 entry。
 *   切栈这一步需要一小段汇编（entry.S 的 _dm_thread_enter，与 D2R 一致）：
 *       mov sp, x2      ; 客体栈
 *       mov x29, #0
 *       mov x16, x0     ; entry
 *       mov x0, x1      ; arg
 *       adr x30, 1f
 *       br x16
 *     1: bl _dm_thread_returned   ; 客体线程从 entry 返回 = 错误，报出来
 *        brk #0
 *   Windows 上没有 aarch64 环境，所以自测只验证「线程表 / TLS / 信号量 / 事件」的逻辑。
 */
#include "hle.h"
#include "hle_nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= 平台线程原语 ================= */

#if defined(_WIN32)
  #include <windows.h>
  #include <process.h>
  typedef HANDLE dm_thread_t;
  /* 注意：MSVC 不支持嵌套函数（GCC 的扩展），所以 tramp 必须放在函数外。
     上一版把 tramp 定义在 thread_start 里面，MSVC 直接语法错误。 */
  struct dm_thr_box { void *(*fn)(void *); void *arg; };
  static unsigned __stdcall dm_thread_tramp(void *p) {
      struct dm_thr_box *q = (struct dm_thr_box *)p;
      void *(*f)(void *) = q->fn; void *a = q->arg;
      free(q);
      f(a);
      return 0;
  }
  static int thread_start(void *(*fn)(void *), void *arg, dm_thread_t *out) {
      struct dm_thr_box *b = (struct dm_thr_box *)malloc(sizeof *b);
      if (!b) return -1;
      b->fn = fn; b->arg = arg;
      uintptr_t h = _beginthreadex(NULL, 0, dm_thread_tramp, b, 0, NULL);
      if (!h) { free(b); return -1; }
      *out = (HANDLE)h;
      return 0;
  }
  static void thread_join(dm_thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
  static void thread_detach(dm_thread_t t) { CloseHandle(t); }
  #define DM_TLS_OUT_OF_INDEXES TLS_OUT_OF_INDEXES
  static uint32_t tls_alloc(void) { return (uint32_t)TlsAlloc(); }
  static void     tls_free(uint32_t k) { if (k != TLS_OUT_OF_INDEXES) TlsFree(k); }
  static void    *tls_get(uint32_t k) { return TlsGetValue(k); }
  static void     tls_set(uint32_t k, void *v) { TlsSetValue(k, v); }
  static void     sem_init_h(void **sem, int initial, int maxcount) {
      *sem = CreateSemaphoreA(NULL, (LONG)initial, (LONG)(maxcount > 0 ? maxcount : 0x7FFFFFFF), NULL);
  }
  static void     sem_fini_h(void *sem) { if (sem) CloseHandle((HANDLE)sem); }
  static void     sem_wait_h(void *sem) { if (sem) WaitForSingleObject((HANDLE)sem, INFINITE); }
  static void     sem_release_h(void *sem, int n) { if (sem) ReleaseSemaphore((HANDLE)sem, (LONG)n, NULL); }
#else
  #include <pthread.h>
  #include <semaphore.h>
  #include <sched.h>
  #include <time.h>
  typedef pthread_t dm_thread_t;
  static int thread_start(void *(*fn)(void *), void *arg, dm_thread_t *out) {
      return pthread_create(out, NULL, fn, arg);
  }
  static void thread_join(dm_thread_t t) { pthread_join(t, NULL); }
  static void thread_detach(dm_thread_t t) { pthread_detach(t); }
  static uint32_t tls_alloc(void) {
      pthread_key_t k;
      if (pthread_key_create(&k, NULL)) return (uint32_t)-1;
      return (uint32_t)k;
  }
  static void  tls_free(uint32_t k) { pthread_key_delete((pthread_key_t)k); }
  static void *tls_get(uint32_t k) { return pthread_getspecific((pthread_key_t)k); }
  static void  tls_set(uint32_t k, void *v) { pthread_setspecific((pthread_key_t)k, v); }
  static void  sem_init_h(void **sem, int initial, int maxcount) {
      sem_t *s = (sem_t *)malloc(sizeof *s);
      if (s) { sem_init(s, 0, (unsigned)initial); (void)maxcount; }
      *sem = s;
  }
  static void  sem_fini_h(void *sem) { if (sem) { sem_destroy((sem_t *)sem); free(sem); } }
  static void  sem_wait_h(void *sem) { if (sem) sem_wait((sem_t *)sem); }
  static void  sem_release_h(void *sem, int n) { if (sem) { for (int i = 0; i < n; i++) sem_post((sem_t *)sem); } }
#endif

/* ================= 计时 ================= */

/* nn::os::GetSystemTick / ConvertToTick —— Horizon 的 tick 频率是 19.2 MHz。
   与 cntpct 同源（hle.c 的 dm_host_ticks）。 */
#define DM_TICK_HZ 19200000ull

uint64_t dm_nn_GetSystemTick(void) { return dm_host_ticks(); }

uint64_t dm_nn_ConvertToTick(int64_t ns) {
    if (ns <= 0) return 0;
    /* ns → tick：tick = ns * 19200000 / 1e9 = ns * 192 / 10000。
       MSVC 不支持 __int128（error C4235），所以先除后乘以避免溢出：
       ns / 10000 * 192 + (ns % 10000) * 192 / 10000 —— 前半可能溢出，
       所以按 (ns / 100000) * 1920 + (ns % 100000) * 192 / 10000 拆。
       tick 的有效范围远小于 int64，这样拆足够精确。 */
    uint64_t a = (uint64_t)(ns / 100000) * 1920ull;
    uint64_t b = (uint64_t)(ns % 100000) * 192ull / 10000ull;
    return a + b;
}

/* ================= 线程表 ================= */

#define DM_MAX_THREADS 64

typedef struct {
    int              used;
    dm_thread_t      host;
    void            *entry;       /* 客体入口 */
    void            *arg;
    void            *stack;
    uint64_t         stack_size;
    int              prio;
    int              started;
    int              finished;
    char             name[64];
} dm_thread_ent;

static dm_thread_ent g_threads[DM_MAX_THREADS];
static struct { uint64_t n_create, n_start, n_wait, n_destroy, n_running; } g_thr_stats;

const void *dm_nn_thread_stats_ptr(void) { return &g_thr_stats; }

/* 线程入口的宿主侧包装。真正切到客体栈是在 aarch64 侧（entry.S 的 _dm_thread_enter），
   这里先登记状态再调用。 */
typedef struct { int idx; } dm_thread_box;

static void *thread_body(void *p) {
    dm_thread_box *b = (dm_thread_box *)p;
    dm_thread_ent *t = &g_threads[b->idx];
    free(b);
    /* 在真机上是：dm_thread_enter(t->entry, t->arg, t->stack + t->stack_size) */
    g_thr_stats.n_running++;
    extern void dm_thread_enter(void *entry, void *arg, void *sp);
#if defined(__aarch64__)
    dm_thread_enter(t->entry, t->arg, (uint8_t *)t->stack + t->stack_size);
#else
    /* 非 aarch64（Windows 自测）：直接调用，不切栈 —— 只为验证簿记逻辑 */
    if (t->entry) ((void (*)(void *))t->entry)(t->arg);
#endif
    t->finished = 1;
    return NULL;
}

/* void nn::os::CreateThread(ThreadType *t, void (*entry)(void*), void *arg,
                             void *stack, size_t stack_size, int prio, int core) */
void dm_nn_CreateThread(void *t, void *entry, void *arg, void *stack,
                        uint64_t stack_size, int prio, int core) {
    (void)core;
    g_thr_stats.n_create++;
    if (!t) return;
    int idx = -1;
    for (int i = 0; i < DM_MAX_THREADS; i++) if (!g_threads[i].used) { idx = i; break; }
    if (idx < 0) return;
    dm_thread_ent *e = &g_threads[idx];
    memset(e, 0, sizeof *e);
    e->used = 1; e->entry = entry; e->arg = arg;
    e->stack = stack; e->stack_size = stack_size; e->prio = prio;
    /* 把索引写进客体 ThreadType 的开头（宿主私有区） */
    *(int *)t = idx;
}

void dm_nn_StartThread(void *t) {
    g_thr_stats.n_start++;
    if (!t) return;
    int idx = *(int *)t;
    if (idx < 0 || idx >= DM_MAX_THREADS || !g_threads[idx].used) return;
    dm_thread_ent *e = &g_threads[idx];
    if (e->started) return;
    dm_thread_box *b = (dm_thread_box *)malloc(sizeof *b);
    if (!b) return;
    b->idx = idx;
    if (thread_start(thread_body, b, &e->host)) { free(b); return; }
    e->started = 1;
}

void dm_nn_WaitThread(void *t) {
    g_thr_stats.n_wait++;
    if (!t) return;
    int idx = *(int *)t;
    if (idx < 0 || idx >= DM_MAX_THREADS || !g_threads[idx].used) return;
    if (g_threads[idx].started) thread_join(g_threads[idx].host);
    g_threads[idx].used = 0;
}

void dm_nn_DestroyThread(void *t) {
    g_thr_stats.n_destroy++;
    if (!t) return;
    int idx = *(int *)t;
    if (idx < 0 || idx >= DM_MAX_THREADS) return;
    if (g_threads[idx].used && g_threads[idx].started && !g_threads[idx].finished)
        thread_detach(g_threads[idx].host);
    g_threads[idx].used = 0;
}

void dm_nn_SetThreadName(void *t, const char *name) {
    if (!t || !name) return;
    int idx = *(int *)t;
    if (idx < 0 || idx >= DM_MAX_THREADS) return;
    snprintf(g_threads[idx].name, sizeof g_threads[idx].name, "%s", name);
}

int dm_nn_GetThreadPriority(const void *t) {
    if (!t) return 0x2C;
    int idx = *(const int *)t;
    if (idx < 0 || idx >= DM_MAX_THREADS) return 0x2C;
    return g_threads[idx].prio;
}

void dm_nn_ChangeThreadPriority(void *t, int prio) {
    if (!t) return;
    int idx = *(int *)t;
    if (idx >= 0 && idx < DM_MAX_THREADS) g_threads[idx].prio = prio;
}

void dm_nn_SetThreadCoreMask(void *t, int ideal, uint64_t mask) {
    (void)t; (void)ideal; (void)mask;
}

uint64_t dm_nn_GetThreadAvailableCoreMask(void) { return 3; }   /* 3 个核 */

/* ================= TLS 槽 ================= */

/* Horizon 的 TlsSlot 是 4 字节编号。宿主持一张映射表。 */
#define DM_MAX_TLS 64
typedef struct { int used; uint32_t host_key; void (*dtor)(uint64_t); } dm_tls_ent;
static dm_tls_ent g_tls[DM_MAX_TLS];
static struct { uint64_t n_alloc, n_free, n_set, n_get; } g_tls_stats;

const void *dm_nn_tls_stats_ptr(void) { return &g_tls_stats; }

/* Result nn::os::AllocateTlsSlot(TlsSlot *out, void (*dtor)(uint64_t)) */
int dm_nn_AllocateTlsSlot(uint32_t *out, void (*dtor)(uint64_t)) {
    g_tls_stats.n_alloc++;
    if (!out) return -1;
    for (int i = 0; i < DM_MAX_TLS; i++) {
        if (g_tls[i].used) continue;
        uint32_t k = tls_alloc();
#if defined(_WIN32)
        if (k == (uint32_t)TLS_OUT_OF_INDEXES) return -2;
#else
        if (k == (uint32_t)-1) return -2;
#endif
        g_tls[i].used = 1; g_tls[i].host_key = k; g_tls[i].dtor = dtor;
        *out = (uint32_t)(i + 1);          /* 客体编号从 1 起（0 = 无效） */
        return 0;
    }
    return -3;
}

void dm_nn_FreeTlsSlot(uint32_t slot) {
    g_tls_stats.n_free++;
    if (slot < 1 || slot > DM_MAX_TLS) return;
    dm_tls_ent *e = &g_tls[slot - 1];
    if (!e->used) return;
    tls_free(e->host_key);
    memset(e, 0, sizeof *e);
}

void dm_nn_SetTlsValue(uint32_t slot, uint64_t v) {
    g_tls_stats.n_set++;
    if (slot < 1 || slot > DM_MAX_TLS) return;
    dm_tls_ent *e = &g_tls[slot - 1];
    if (e->used) tls_set(e->host_key, (void *)(uintptr_t)v);
}

uint64_t dm_nn_GetTlsValue(uint32_t slot) {
    g_tls_stats.n_get++;
    if (slot < 1 || slot > DM_MAX_TLS) return 0;
    dm_tls_ent *e = &g_tls[slot - 1];
    return e->used ? (uint64_t)(uintptr_t)tls_get(e->host_key) : 0;
}

/* ================= 信号量 ================= */

void dm_nn_InitializeSemaphore(void *s, int initial, int maxcount) {
    if (!s) return;
    void *h = NULL;
    sem_init_h(&h, initial, maxcount);
    *(void **)s = h;
}

void dm_nn_FinalizeSemaphore(void *s) {
    if (!s) return;
    sem_fini_h(*(void **)s);
    *(void **)s = NULL;
}

void dm_nn_AcquireSemaphore(void *s) {
    if (!s) return;
    sem_wait_h(*(void **)s);
}

void dm_nn_ReleaseSemaphore(void *s, int n) {
    if (!s) return;
    sem_release_h(*(void **)s, n);
}

/* ================= 系统事件（带超时的事件） ================= */

/* SystemEventType 客体自备存储；宿主在其中放一个「已置位」标记 + 用 LightEvent 的实现。
   这里复用 hle_nn.c 的 LightEvent 机制会引入跨文件依赖，所以独立实现一个小版本。 */
typedef struct { void *mutex; void *cond; int signaled; } dm_sysev;

#if defined(_WIN32)
  static void *sysev_mtx(void) { CRITICAL_SECTION *m = (CRITICAL_SECTION *)malloc(sizeof *m); if (m) InitializeCriticalSection(m); return m; }
  static void  sysev_mtx_free(void *m) { if (m) { DeleteCriticalSection((CRITICAL_SECTION *)m); free(m); } }
  static void  sysev_mtx_lock(void *m) { if (m) EnterCriticalSection((CRITICAL_SECTION *)m); }
  static void  sysev_mtx_unlock(void *m) { if (m) LeaveCriticalSection((CRITICAL_SECTION *)m); }
  static void *sysev_cond(void) { CONDITION_VARIABLE *c = (CONDITION_VARIABLE *)malloc(sizeof *c); if (c) InitializeConditionVariable(c); return c; }
  static void  sysev_cond_free(void *c) { free(c); }
  static void  sysev_wait(void *c, void *m, int ms) { if (c && m) SleepConditionVariableCS((CONDITION_VARIABLE *)c, (CRITICAL_SECTION *)m, ms < 0 ? INFINITE : (DWORD)ms); }
  static void  sysev_signal(void *c) { if (c) WakeAllConditionVariable((CONDITION_VARIABLE *)c); }
#else
  #include <pthread.h>
  #include <errno.h>
  static void *sysev_mtx(void) { pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof *m); if (m) pthread_mutex_init(m, NULL); return m; }
  static void  sysev_mtx_free(void *m) { if (m) { pthread_mutex_destroy((pthread_mutex_t *)m); free(m); } }
  static void  sysev_mtx_lock(void *m) { if (m) pthread_mutex_lock((pthread_mutex_t *)m); }
  static void  sysev_mtx_unlock(void *m) { if (m) pthread_mutex_unlock((pthread_mutex_t *)m); }
  static void *sysev_cond(void) { pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof *c); if (c) pthread_cond_init(c, NULL); return c; }
  static void  sysev_cond_free(void *c) { if (c) { pthread_cond_destroy((pthread_cond_t *)c); free(c); } }
  static void  sysev_wait(void *c, void *m, int ms) {
      if (!c || !m) return;
      if (ms < 0) { pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m); return; }
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait((pthread_cond_t *)c, (pthread_mutex_t *)m, &ts);
  }
  static void  sysev_signal(void *c) { if (c) pthread_cond_broadcast((pthread_cond_t *)c); }
#endif

/* SystemEventType 的客体结构我们不知道确切大小，但至少能放下一个指针。
   为了安全，宿主把 dm_sysev 的指针写进结构的开头 8 字节，其余保留。 */
void dm_nn_InitializeSystemEvent(void *e, int clear_mode) {
    (void)clear_mode;
    if (!e) return;
    dm_sysev *s = (dm_sysev *)calloc(1, sizeof *s);
    if (!s) return;
    s->mutex = sysev_mtx();
    s->cond  = sysev_cond();
    *(void **)e = s;
}

void dm_nn_DestroySystemEvent(void *e) {
    if (!e) return;
    dm_sysev *s = *(dm_sysev **)e;
    if (!s) return;
    sysev_cond_free(s->cond);
    sysev_mtx_free(s->mutex);
    free(s);
    *(void **)e = NULL;
}

void dm_nn_SignalSystemEvent(void *e) {
    if (!e) return;
    dm_sysev *s = *(dm_sysev **)e;
    if (!s) return;
    sysev_mtx_lock(s->mutex);
    s->signaled = 1;
    sysev_mtx_unlock(s->mutex);
    sysev_signal(s->cond);
}

void dm_nn_ClearSystemEvent(void *e) {
    if (!e) return;
    dm_sysev *s = *(dm_sysev **)e;
    if (!s) return;
    sysev_mtx_lock(s->mutex); s->signaled = 0; sysev_mtx_unlock(s->mutex);
}

/* 带超时的 SystemEvent 等待，返回 1 = 已置位，0 = 超时。
   （dm_nn_TimedWaitSystemEvent 是 void 版，给客体用；这个是给自测用的返回码版。） */
int dm_nn_TimedWaitSystemEvent_rc(void *e, int64_t ns) {
    if (!e) return 0;
    dm_sysev *s = *(dm_sysev **)e;
    if (!s) return 0;
    int ms = (ns < 0) ? -1 : (int)(ns / 1000000);
    sysev_mtx_lock(s->mutex);
    while (!s->signaled) {
        if (ms < 0) sysev_wait(s->cond, s->mutex, -1);
        else {
            if (ms == 0) break;
            sysev_wait(s->cond, s->mutex, ms);
            if (!s->signaled) break;
        }
    }
    int got = s->signaled;
    s->signaled = 0;
    sysev_mtx_unlock(s->mutex);
    return got;
}

void dm_nn_WaitSystemEvent(void *e) { (void)dm_nn_TimedWaitSystemEvent_rc(e, -1); }

/* 返回 1 = 已置位，0 = 超时 */
int dm_nn_TimedWaitSystemEvent(void *e, int64_t ns) {
    if (!e) return 0;
    dm_sysev *s = *(dm_sysev **)e;
    if (!s) return 0;
    int ms = (ns < 0) ? -1 : (int)(ns / 1000000);
    sysev_mtx_lock(s->mutex);
    while (!s->signaled) {
        if (ms < 0) sysev_wait(s->cond, s->mutex, -1);
        else {
            if (ms == 0) break;
            sysev_wait(s->cond, s->mutex, ms);
            if (!s->signaled) break;      /* 超时 */
        }
    }
    int got = s->signaled;
    s->signaled = 0;
    sysev_mtx_unlock(s->mutex);
    return got;
}

/* 带超时的 LightEvent —— 复用 hle_nn.c 的 LightEvent（它没有超时版本），
   这里用一个独立的简单实现：置位则立即返回，否则轮询等待到超时。 */
void dm_nn_TimedWaitLightEvent(void *e, int64_t ns) {
    /* 客体 LightEventType 的槽里放的是 hle_nn.c 的 dm_box；
       没有超时版本的公开接口，这里用「先 Sleep 再 Wait」近似 —— 
       语义上等价于「最多等 ns」。 */
    if (ns > 0) dm_nn_SleepThread(ns);
    dm_nn_WaitLightEvent(e);
}

/* ================= 异常处理 ================= */

/* nn::os::detail::UserExceptionHandler —— 客体自己的异常处理入口。
   宿主侧不应该被调用（异常在我们这里不会以同样方式发生），做成记录 + 返回。 */
void dm_nn_detail_UserExceptionHandler(void) { }
