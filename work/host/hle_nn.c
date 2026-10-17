/* 宿主 HLE —— nn:: 系列函数的实现。
 *
 * 依据：main 从 nn:: 导入的**完整清单只有 38 个函数**（见 work/porting/main_nn_api.md）：
 *   nn::os  23 个   内存 / 互斥 / 轻量事件 / 线程 / 命令行
 *   nn::fs   8 个   挂载 / 打开 / 读 / 关 / 大小 / 卸载 / 缓存大小 / 分配器
 *   nn::ro   5 个   模块加载（LoadModule / RegisterModuleInfo / ProtectRelro / GetBufferSize / Initialize）
 *   nn::oe   2 个   语言 / 焦点
 *
 * 注意「main 不做任何 svc」—— 所有平台交互都走这 38 个导入。
 * 所以本文件 + hle.c 的 svc 实现，就是宿主的全部平台面。
 *
 * 对象模型（Horizon → 宿主）：
 *   nn::os::MutexType / BusyMutexType / LightEventType 都是**客体自备存储**的结构体，
 *   宿主只把宿主对象（CRITICAL_SECTION / CONDITION_VARIABLE / HANDLE）的指针写进它们的
 *   一块保留区。这样客体的结构体大小不变，而宿主拿到自己的对象。
 *
 * 平台：Windows 上用 CRITICAL_SECTION / CONDITION_VARIABLE / Sleep；
 *       macOS/iOS 上换成 pthread_mutex / pthread_cond（DM_HOST_POSIX 分支）。
 */
#include "hle.h"
#include "hle_nn.h"
#include "hle_nn_more.h"
#include "hle_diag.h"   /* 第 82 轮：大块内存分配结果写进设备日志 */
#include "hle_plat.h"   /* 第 83 轮：mmap 失败时从客体池顶划大块 */
#if !defined(_WIN32)
#include <sys/mman.h>
#include <errno.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= 平台原语 ================= */

#if defined(_WIN32)
  #include <windows.h>
  typedef CRITICAL_SECTION dm_mutex_t;
  typedef CONDITION_VARIABLE dm_cond_t;
  static int  mtx_init(dm_mutex_t *m) { InitializeCriticalSection(m); return 0; }
  static void mtx_fini(dm_mutex_t *m) { DeleteCriticalSection(m); }
  static void mtx_lock(dm_mutex_t *m) { EnterCriticalSection(m); }
  static int  mtx_trylock(dm_mutex_t *m) { return TryEnterCriticalSection(m) ? 1 : 0; }
  static void mtx_unlock(dm_mutex_t *m) { LeaveCriticalSection(m); }
  static int  cond_init(dm_cond_t *c) { InitializeConditionVariable(c); return 0; }
  static void cond_fini(dm_cond_t *c) { (void)c; }
  static void cond_wait(dm_cond_t *c, dm_mutex_t *m) { SleepConditionVariableCS(c, m, INFINITE); }
  static void cond_signal(dm_cond_t *c) { WakeConditionVariable(c); }
  static void cond_bcast(dm_cond_t *c) { WakeAllConditionVariable(c); }
  static void thread_sleep_ns(int64_t ns) {
      if (ns < 0) { Sleep(0); return; }              /* 负值 = yield */
      DWORD ms = (DWORD)(ns / 1000000);
      if (ms == 0 && ns > 0) ms = 1;
      Sleep(ms);
  }
  static void thread_yield(void) { Sleep(0); }
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_mutex_t dm_mutex_t;
  typedef pthread_cond_t  dm_cond_t;
  static int  mtx_init(dm_mutex_t *m) { return pthread_mutex_init(m, NULL); }
  static void mtx_fini(dm_mutex_t *m) { pthread_mutex_destroy(m); }
  static void mtx_lock(dm_mutex_t *m) { pthread_mutex_lock(m); }
  static int  mtx_trylock(dm_mutex_t *m) { return pthread_mutex_trylock(m) == 0; }
  static void mtx_unlock(dm_mutex_t *m) { pthread_mutex_unlock(m); }
  static int  cond_init(dm_cond_t *c) { return pthread_cond_init(c, NULL); }
  static void cond_fini(dm_cond_t *c) { pthread_cond_destroy(c); }
  static void cond_wait(dm_cond_t *c, dm_mutex_t *m) { pthread_cond_wait(c, m); }
  static void cond_signal(dm_cond_t *c) { pthread_cond_signal(c); }
  static void cond_bcast(dm_cond_t *c) { pthread_cond_broadcast(c); }
  static void thread_sleep_ns(int64_t ns) {
      if (ns < 0) { sched_yield(); return; }
      struct timespec ts = { ns / 1000000000, ns % 1000000000 };
      nanosleep(&ts, NULL);
  }
  static void thread_yield(void) { sched_yield(); }
#endif

/* ================= 客体结构的「宿主指针槽」 =================
 *
 * 客体的 MutexType / LightEventType 等结构我们不知道确切布局，
 * 但它们的**大小**可以从 nn::os 的头文件推断（Horizon SDK 里这些是固定大小的 POD）。
 * 做法：把宿主对象指针写进结构的**开头**（8 字节），其余保留。
 * 这与 Horizon 的实现一致（它们内部也是一个 InternalCriticalSection + 状态字）。
 */
#define DM_SLOT0(p)  (*(void **)(p))

/* nn::os 的宿主对象箱（每个客体结构配一个） */
typedef struct {
    dm_mutex_t mtx;
    dm_cond_t  cond;
    int        initialized;
    int        is_busy;     /* BusyMutex 用：非 0 表示已被占用 */
    int        signaled;    /* LightEvent 用 */
    int        auto_clear;  /* LightEvent 清模式 */
} dm_box;

/* ================= 统计与自检钩子 ================= */

static struct {
    uint64_t n_mem_alloc, n_mem_free;
    uint64_t n_mtx_init, n_mtx_lock, n_mtx_trylock_ok, n_mtx_trylock_busy;
    uint64_t n_ev_init, n_ev_wait, n_ev_signal, n_ev_clear;
    uint64_t n_sleep, n_yield;
    uint64_t n_fs_mount, n_fs_open, n_fs_read, n_fs_close;
    uint64_t bytes_alloc, bytes_read;
} g_nn_stats;

const void *dm_nn_stats_ptr(void) { return &g_nn_stats; }

/* ================= nn::os ================= */

/* Round 84: keep our own allocation table and free only blocks listed in it.
 * Evidence (DmGame-2026-10-17-175631.ips): round 83 routed every AllocateMemoryBlock
 * through mmap/pool, but FreeMemoryBlock only skipped frees for size >= 16 MB. An 8 MB
 * ROM-cache block from mmap was then passed to Darwin free(), which raised
 *   ___BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED_WAS_NOT_ALLOCATED
 * and called abort() (SIGABRT, backtrace __pthread_kill<-pthread_kill<-abort<-malloc_report).
 * Now: kind 1 -> munmap/VirtualFree, kind 2 -> free (legacy posix_memalign path),
 * kind 0 -> pool carve (cannot be released), not in table -> ignore. */
#if defined(_MSC_VER)
#include <intrin.h>
#define DM_RET_ADDR() ((void *)_ReturnAddress())
#else
#define DM_RET_ADDR() __builtin_return_address(0)
#endif
#define DM_BLK_MAX 1024
static struct { void *p; uint64_t len; int kind; } g_blk[DM_BLK_MAX];
static int g_blk_n;
static uint64_t g_last_fail_size;   /* Round 85: last failed AllocateMemoryBlock request */
static int      g_last_fail_err;
static uint64_t g_last_big_size;    /* Round 86: last >=256MB request (size/result) */
static uint64_t g_last_big_ptr;
static int      g_last_big_ok;

static void blk_add(void *p, uint64_t len, int kind) {
    if (!p) return;
    for (int i = 0; i < g_blk_n; i++)
        if (!g_blk[i].p) { g_blk[i].p = p; g_blk[i].len = len; g_blk[i].kind = kind; return; }
    if (g_blk_n < DM_BLK_MAX) { g_blk[g_blk_n].p = p; g_blk[g_blk_n].len = len; g_blk[g_blk_n].kind = kind; g_blk_n++; }
}
static int blk_take(void *p, uint64_t *len) {
    for (int i = 0; i < g_blk_n; i++)
        if (g_blk[i].p == p) { *len = g_blk[i].len; int k = g_blk[i].kind; g_blk[i].p = NULL; return k; }
    return -1;
}

/* void* nn::os::AllocateMemoryBlock(size_t *out_size, size_t size)
   —— 旧的自测用接口（返回指针 + 回填大小），真机 ABI 见 dm_nn_AllocateMemoryBlock_out。 */
void *dm_nn_AllocateMemoryBlock(uint64_t *out_size, uint64_t size) {
    g_nn_stats.n_mem_alloc++;
    if (size == 0) size = 0x1000;
    if (out_size) *out_size = size;
#if defined(_WIN32)
    void *p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p) blk_add(p, size, 1);
#else
    void *p = NULL;
    if (posix_memalign(&p, 0x1000, size)) p = NULL;
    if (p) blk_add(p, size, 2);
#endif
    g_nn_stats.bytes_alloc += size;
    return p;
}

/* uint64_t nn::os::AllocateMemoryBlock(uint64_t *out_addr, uint64_t size)
 *
 * !! 第 82 轮：这是**真机 ABI**，与上面那个「返回指针」的旧实现不同 !!
 * 证据（sdk.patched.bin）：
 *   nn::os::AllocateMemoryBlock(sdk+0x143f10) → MemoryHeapManager::AllocateFromHeap
 *   (sdk+0x145480)；成功时 `str x8, [x21]`（把**地址**写进 out），w0 = 0；
 *   失败时 w0 = 0x1003 且**不写 out**。
 * 游戏侧（main+0x9ac510）就是 `bl AllocateMemoryBlock(&out, size)` 之后拿 [out] 当地址用，
 * 而且**不检查 Result** —— 所以 out 一旦没被写，就会把栈上的垃圾当堆地址。
 * 本机真机实证（DmGame-2026-10-17-171543.ips）：引擎的 MemoryInterface 把这个「地址」
 * 交给 CLinkBlockHeap::init，块头变成 {base, 0}，块尾 = 0x30000000，
 * 之后 CGraTexMgr 一 alloc 就下溢崩在 alloc+0xb4。
 *
 * 而且 SDK 那份实现依赖 MemoryHeapManager 的空闲链 —— 这份移植里它从来没被装过内存
 * （svc SetHeapSize 的池在宿主侧，sdk 的空闲链是空的），所以它必然返回 0x1003。
 * 因此这里由宿主直接分配（4KB 对齐）并**按真机语义把地址写回 out**，返回 Result。 */
uint64_t dm_nn_AllocateMemoryBlock_out(uint64_t *out_addr, uint64_t size) {
    g_nn_stats.n_mem_alloc++;
    if (size == 0) size = 0x1000;
    uint64_t want = (size + 0xFFFFull) & ~0xFFFFull;   /* 64KB 对齐，兼容引擎的 64KB 取整 */
    void *p = NULL;
    int err = 0, kind = 1;             /* 1 = mmap/VirtualAlloc(freeable); 0 = pool carve(never freed) */
    /* Round 85: for big blocks try the POOL FIRST. mmap eats extra process address space,
     * and on device the 3.2 GB pool + 1.25 GB mmap + 768 MB mmap blew the VA limit:
     * CGraMemMgr's 768 MB request failed, its heap base became 0, and CLinkBlockHeap::alloc
     * then faulted at 0x2feddfb8 (DmGame-2026-10-17-180546.ips). The pool IS the app memory,
     * so carving from it is also the faithful behaviour. */
    if (size >= 0x1000000ull) {
        p = dm_plat_heap_alloc_big(want);
        if (p) { kind = 0; err = 0; }
    }
    if (!p) {
#if defined(_WIN32)
        p = VirtualAlloc(NULL, want, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) err = (int)GetLastError();
#else
        p = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) { err = errno; p = NULL; }
#endif
    }
    if (p) blk_add(p, want, kind);
    if (!p) { g_last_fail_size = size; g_last_fail_err = err; }
    if (size >= 0x10000000ull) {
        g_last_big_size = size; g_last_big_ptr = (uint64_t)(uintptr_t)p; g_last_big_ok = p ? 1 : 0;
    }
    g_nn_stats.bytes_alloc += size;
    if (out_addr) *out_addr = (uint64_t)(uintptr_t)p;
    /* For requests >= 256 MB also record the GUEST caller. __builtin_return_address(0)
       is the instruction right after the guest's blr (our thunk uses `br`, so x30 is
       preserved), so subtracting the module base names the requester. */
    if (size >= 0x10000000ull)
        dm_diag_mark("[os] AllocateMemoryBlock(%llu MB) caller=%p %s",
                     (unsigned long long)(size >> 20), DM_RET_ADDR(),
                     p ? "ok" : "fail");
    else if (!p) dm_diag_mark("[os] AllocateMemoryBlock(%llu MB) fail errno=%d",
                              (unsigned long long)(size >> 20), err);
    else if (size >= 0x1000000ull) dm_diag_mark("[os] AllocateMemoryBlock(%llu MB) -> %p",
                                                (unsigned long long)(size >> 20), p);
    return p ? 0 : 0x1003;      /* Result: 0 = ok / 0x1003 = OutOfMemory */
}

/* Round 85: memory ledger for the crash block (called from the signal path, so the
 * caller formats it with its own async-signal-safe helpers - no snprintf here). */
void dm_mem_stats(uint64_t *cap, uint64_t *cur, uint64_t *carved, int *blocks,
                  uint64_t *last_fail, int *last_err,
                  uint64_t *last_big, uint64_t *last_big_ptr, int *last_big_ok) {
    uint64_t base = 0, hcap = 0, hcur = 0;
    dm_plat_heap_peek(&base, &hcap, &hcur);
    uint64_t c = 0;
    int nb = 0;
    for (int i = 0; i < g_blk_n; i++)
        if (g_blk[i].p) { nb++; if (g_blk[i].kind == 0) c += g_blk[i].len; }
    if (cap) *cap = hcap;
    if (cur) *cur = hcur;
    if (carved) *carved = c;
    if (blocks) *blocks = nb;
    if (last_fail) *last_fail = g_last_fail_size;
    if (last_err) *last_err = g_last_fail_err;
    if (last_big) *last_big = g_last_big_size;
    if (last_big_ptr) *last_big_ptr = g_last_big_ptr;
    if (last_big_ok) *last_big_ok = g_last_big_ok;
}

/* void nn::os::FreeMemoryBlock(void *p, size_t size)
 *
 * Round 84: only free blocks that we registered ourselves.
 * Evidence (DmGame-2026-10-17-175631.ips): round 83 moved small blocks to mmap too,
 * and the "size >= 16 MB" test missed e.g. the 8 MB ROM cache block, so free() on an
 * mmap pointer raised Darwin's
 *   ___BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED_WAS_NOT_ALLOCATED -> abort().
 * Now: kind 1 -> munmap/VirtualFree, kind 2 -> free, kind 0 (pool carve) -> keep,
 * and anything not in the table is IGNORED (never hand a guest pointer to Darwin free). */
void dm_nn_FreeMemoryBlock(void *p, uint64_t size) {
    (void)size;
    g_nn_stats.n_mem_free++;
    if (!p) return;
    uint64_t len = 0;
    int kind = blk_take(p, &len);
    if (kind < 0) {
        dm_diag_mark("[os] FreeMemoryBlock(%p) not in table - ignored", p);
        return;
    }
    if (kind == 0) return;             /* pool carve: lives until process exit */
    if (kind == 2) { free(p); return; }
#if defined(_WIN32)
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, len ? len : 0x1000);
#endif
}

/* void nn::os::SetMemoryHeapSize(size_t size)
   —— Horizon 里是设进程堆上限。宿主侧语义最接近的是「无操作，只是记录」。 */
void dm_nn_SetMemoryHeapSize(uint64_t size) { (void)size; }

/* void nn::os::QueryMemoryInfo(MemoryInfo *out)
   —— 填 { total, used, free }（Horizon 是 3 个 u64）。 */
void dm_nn_QueryMemoryInfo(uint64_t *out) {
    if (!out) return;
    /* 报告一个稳定的假值：总 4 GB、已用 1 GB、空闲 3 GB。
       游戏一般只拿它做诊断显示，不会据此做分配决策。 */
    out[0] = 4ull << 30;
    out[1] = 1ull << 30;
    out[2] = 3ull << 30;
}

/* nn::os::SetMemoryAllocatorForThreadLocal(alloc, free) —— 记录即可 */
void dm_nn_SetMemoryAllocatorForThreadLocal(void *alloc, void *freefn) {
    (void)alloc; (void)freefn;
}

/* int nn::os::GetHostArgc(); char **nn::os::GetHostArgv() */
static int    g_argc = 1;
static char  *g_argv[2] = { (char *)"dm", NULL };
int    dm_nn_GetHostArgc(void) { return g_argc; }
char **dm_nn_GetHostArgv(void) { return g_argv; }

/* ---- Mutex ---- */

static dm_box *box_of(void *obj, int is_event) {
    if (!obj) return NULL;
    dm_box *b = (dm_box *)DM_SLOT0(obj);
    if (b) return b;
    b = (dm_box *)calloc(1, sizeof *b);
    if (!b) return NULL;
    if (is_event) { cond_init(&b->cond); }
    else          { mtx_init(&b->mtx); }
    b->initialized = 1;
    DM_SLOT0(obj) = b;
    return b;
}

/* void nn::os::InitializeMutex(MutexType *m, bool recursive, int level) */
void dm_nn_InitializeMutex(void *m, int recursive, int level) {
    (void)recursive; (void)level;
    g_nn_stats.n_mtx_init++;
    box_of(m, 0);
}
void dm_nn_FinalizeMutex(void *m) {
    dm_box *b = m ? (dm_box *)DM_SLOT0(m) : NULL;
    if (b) { mtx_fini(&b->mtx); free(b); DM_SLOT0(m) = NULL; }
}
void dm_nn_LockMutex(void *m)   { dm_box *b = box_of(m, 0); if (b) { g_nn_stats.n_mtx_lock++; mtx_lock(&b->mtx); } }
void dm_nn_UnlockMutex(void *m) { dm_box *b = m ? (dm_box *)DM_SLOT0(m) : NULL; if (b) mtx_unlock(&b->mtx); }

/* ---- BusyMutex（自旋式；宿主用普通互斥 + 一个占用标记实现） ---- */
void dm_nn_InitializeBusyMutex(void *m) { box_of(m, 0); }
void dm_nn_FinalizeBusyMutex(void *m)   { dm_nn_FinalizeMutex(m); }
void dm_nn_LockBusyMutex(void *m) {
    dm_box *b = box_of(m, 0);
    if (b) { g_nn_stats.n_mtx_lock++; mtx_lock(&b->mtx); b->is_busy = 1; }
}
int dm_nn_TryLockBusyMutex(void *m) {
    dm_box *b = box_of(m, 0);
    if (!b) return 0;
    if (mtx_trylock(&b->mtx)) { b->is_busy = 1; g_nn_stats.n_mtx_trylock_ok++; return 1; }
    g_nn_stats.n_mtx_trylock_busy++;
    return 0;
}
void dm_nn_UnlockBusyMutex(void *m) {
    dm_box *b = m ? (dm_box *)DM_SLOT0(m) : NULL;
    if (b) { b->is_busy = 0; mtx_unlock(&b->mtx); }
}

/* ---- LightEvent ---- */

/* void nn::os::InitializeLightEvent(LightEventType *e, bool signaled, EventClearMode mode) */
void dm_nn_InitializeLightEvent(void *e, int signaled, int mode) {
    g_nn_stats.n_ev_init++;
    dm_box *b = box_of(e, 1);
    if (!b) return;
    mtx_init(&b->mtx);
    b->signaled = signaled ? 1 : 0;
    /* Horizon 的 EventClearMode: 0 = auto clear, 1 = manual clear */
    b->auto_clear = (mode == 0);
}
void dm_nn_FinalizeLightEvent(void *e) {
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (b) { cond_fini(&b->cond); mtx_fini(&b->mtx); free(b); DM_SLOT0(e) = NULL; }
}
void dm_nn_SignalLightEvent(void *e) {
    g_nn_stats.n_ev_signal++;
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (!b) return;
    mtx_lock(&b->mtx); b->signaled = 1; mtx_unlock(&b->mtx);
    cond_bcast(&b->cond);
}
void dm_nn_ClearLightEvent(void *e) {
    g_nn_stats.n_ev_clear++;
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (!b) return;
    mtx_lock(&b->mtx); b->signaled = 0; mtx_unlock(&b->mtx);
}
void dm_nn_WaitLightEvent(void *e) {
    g_nn_stats.n_ev_wait++;
    dm_box *b = e ? (dm_box *)DM_SLOT0(e) : NULL;
    if (!b) return;
    mtx_lock(&b->mtx);
    while (!b->signaled) cond_wait(&b->cond, &b->mtx);
    if (b->auto_clear) b->signaled = 0;
    mtx_unlock(&b->mtx);
}

/* ---- 线程 ---- */

/* void nn::os::SleepThread(nn::TimeSpan) —— TimeSpan 是 int64 纳秒，负值 = yield */
void dm_nn_SleepThread(int64_t ns) { g_nn_stats.n_sleep++; thread_sleep_ns(ns); }
void dm_nn_YieldThread(void)       { g_nn_stats.n_yield++; thread_yield(); }

/* ================= 分发（按名字） =================
 *
 * 装载器解析重定位时按**符号名**取地址。宿主要做的是：
 * 在解析到 nn:: 的符号时，**优先返回宿主实现**（而不是客体里的实现）。
 * 这与 D2R 的 force_lookup（hle_force_ent 表）是同一机制。
 */
typedef struct { const char *name; void *fn; } dm_nn_ent;

static const dm_nn_ent g_nn_syms[] = {
    /* nn::os —— 内存 */
    { "_ZN2nn2os19AllocateMemoryBlockEPmm",              (void *)dm_nn_AllocateMemoryBlock_out },
    { "_ZN2nn2os15FreeMemoryBlockEmm",                   (void *)dm_nn_FreeMemoryBlock },
    { "_ZN2nn2os17SetMemoryHeapSizeEm",                  (void *)dm_nn_SetMemoryHeapSize },
    { "_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE",   (void *)dm_nn_QueryMemoryInfo },
    { "_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE", (void *)dm_nn_SetMemoryAllocatorForThreadLocal },
    /* nn::os —— 命令行 */
    { "_ZN2nn2os11GetHostArgcEv",                        (void *)dm_nn_GetHostArgc },
    { "_ZN2nn2os11GetHostArgvEv",                        (void *)dm_nn_GetHostArgv },
    /* nn::os —— 互斥 */
    { "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi",   (void *)dm_nn_InitializeMutex },
    { "_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE",       (void *)dm_nn_FinalizeMutex },
    { "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE",            (void *)dm_nn_LockMutex },
    { "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE",         (void *)dm_nn_UnlockMutex },
    { "_ZN2nn2os19InitializeBusyMutexEPNS0_13BusyMutexTypeE", (void *)dm_nn_InitializeBusyMutex },
    { "_ZN2nn2os17FinalizeBusyMutexEPNS0_13BusyMutexTypeE",   (void *)dm_nn_FinalizeBusyMutex },
    { "_ZN2nn2os13LockBusyMutexEPNS0_13BusyMutexTypeE",       (void *)dm_nn_LockBusyMutex },
    { "_ZN2nn2os16TryLockBusyMutexEPNS0_13BusyMutexTypeE",    (void *)dm_nn_TryLockBusyMutex },
    { "_ZN2nn2os15UnlockBusyMutexEPNS0_13BusyMutexTypeE",     (void *)dm_nn_UnlockBusyMutex },
    /* nn::os —— 轻量事件 */
    { "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", (void *)dm_nn_InitializeLightEvent },
    { "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", (void *)dm_nn_FinalizeLightEvent },
    { "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE",   (void *)dm_nn_SignalLightEvent },
    { "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE",    (void *)dm_nn_ClearLightEvent },
    { "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE",     (void *)dm_nn_WaitLightEvent },
    /* nn::os —— 线程 */
    { "_ZN2nn2os11SleepThreadENS_8TimeSpanE",            (void *)dm_nn_SleepThread },
    { "_ZN2nn2os11YieldThreadEv",                        (void *)dm_nn_YieldThread },
    /* nn::oe */
    { "_ZN2nn2oe18GetDesiredLanguageEv",                 (void *)dm_nn_GetDesiredLanguage },
    { "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", (void *)dm_nn_SetFocusHandlingMode },
    /* nn::fs */
    { "_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE",          (void *)dm_nn_fs_SetAllocator },
    { "_ZN2nn2fs22QueryMountRomCacheSizeEPm",            (void *)dm_nn_fs_QueryMountRomCacheSize },
    { "_ZN2nn2fs8MountRomEPKcPvm",                       (void *)dm_nn_fs_MountRom },
    { "_ZN2nn2fs7UnmountEPKc",                           (void *)dm_nn_fs_Unmount },
    { "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",       (void *)dm_nn_fs_OpenFile },
    { "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",      (void *)dm_nn_fs_GetFileSize },
    { "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",      (void *)dm_nn_fs_ReadFile },
    { "_ZN2nn2fs9CloseFileENS0_10FileHandleE",           (void *)dm_nn_fs_CloseFile },
    /* nn::ro */
    { "_ZN2nn2ro10InitializeEv",                         (void *)dm_nn_ro_Initialize },
    { "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi",      (void *)dm_nn_ro_LoadModule },
    { "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", (void *)dm_nn_ro_RegisterModuleInfo },
    { "_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_",         (void *)dm_nn_ro_ProtectRelro },
    { "_ZN2nn2ro13GetBufferSizeEPmPKv",                  (void *)dm_nn_ro_GetBufferSize },
    /* 服务层初始化（强制接管；依据 D2R Mobile dm/host/hle_sdkinit.c）
       —— nninitInitializeSdkModule 调的这批会去连 Horizon 服务进程，宿主没有，一律空操作 */
    { "_ZN2nn2fs24InitializeForApplicationEv",           (void *)dm_nn_fs_InitializeForApplication },
    { "_ZN2nn4diag34InitializeApplicationAbortObserverEv", (void *)dm_nn_diag_AbortObserver },
    { "_ZN2nn3err26InitializeContextWriterApiEv",        (void *)dm_nn_err_InitCtx },
    { "_ZN2nn3aoc10InitializeEv",                        (void *)dm_nn_aoc_Initialize },
    { "_ZN2nn4pctl10InitializeEv",                       (void *)dm_nn_pctl_Initialize },
    { "nndiagStartup",                                   (void *)dm_nn_nndiagStartup },
    /* 第 72 轮：客体 libc 的 DSO 初始化（各模块 _init 里的第一步）。
       返回 1 ⇒ _init 跳过它自己那遍 init_array；构造函数由宿主的循环跑一遍。 */
    { "__nnmusl_init_dso",                               (void *)dm_nn_nnmusl_init_dso },
    /* 第 77 轮：别名区的栈地址分配器（nn::os 线程创建要用）。
       SDK 里那份的区域状态没人初始化（永远返回 0）→ 线程创建即 Abort。 */
    { "_ZN2nn2os6detail25AddressSpaceAllocatorBaseImmE13AllocateSpaceEmmmPFmmE",
                                                         (void *)dm_nn_addr_alloc_space },
    /* nn::time 整族强制接管（第 65 轮；照 D2R dm/host/hle_time.c）：
       SDK 的 time 客户端会去 sm: 要 "time:u"，宿主没有该服务进程 → 空会话 →
       [Break] reason=0 致命中止。接管后它永远走不到那一步。 */
    { "_ZN2nn4time10InitializeEv",                       (void *)dm_nn_time_Initialize },
    { "_ZN2nn4time13IsInitializedEv",                    (void *)dm_nn_time_IsInitialized },
    { "_ZN2nn4time23StandardUserSystemClock3nowEv",      (void *)dm_nn_time_now },
    { "_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
                                                         (void *)dm_nn_time_to_time_t },
};

void *dm_nn_force_lookup(const char *name) {
    if (!name || name[0] != '_') return NULL;
    /* 先问程序化生成的表（覆盖 os/fs/ro/oe/hid/util/time/diag/init/nifm）
       —— 那张表由 gen_impl_table.py 从真实符号表生成，名字逐字节可靠。 */
    extern void *dm_nn_impl_lookup(const char *);
    void *p = dm_nn_impl_lookup(name);
    if (p) return p;
    /* 再问本文件的手写表（保留作为兜底；两张表内容不冲突） */
    for (size_t i = 0; i < sizeof g_nn_syms / sizeof g_nn_syms[0]; i++)
        if (!strcmp(g_nn_syms[i].name, name)) return g_nn_syms[i].fn;
    return NULL;
}

unsigned dm_nn_symbol_count(void) {
    extern unsigned dm_nn_impl_count(void);
    return dm_nn_impl_count();
}
