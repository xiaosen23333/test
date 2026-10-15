/* 宿主入口 —— aarch64 平台侧的补件（macOS / iOS）。
 *
 * ============================================================================
 * 为什么有这个文件：我在 Windows 上写宿主时，有几个符号是「平台相关、暂时用桩」
 * 或「声明了但没实现」的。它们不会在 Windows 自测里暴露（自测不链接汇编、
 * 也不调线程切栈），但**在 Mac 上链接就会失败**。本文件把它们补齐。
 *
 * 补的是四样（都是 D2R 的 entry.S / hle_thread.c 里做过的事）：
 *   ① dm_thread_key()        —— 宿主侧的 pthread 键创建（改写后的客体按「键号×8」读键页）
 *   ② dm_thread_returned()   —— 客体线程从入口返回 = 错误（Horizon 要求走 ExitThread）
 *   ③ dm_gate_dispatch_c()   —— entry.S 用的名字（C 侧实现叫 dm_gate_dispatch）
 *   ④ dm_thunk_c()           —— NVN 跳板的「计数」不在这里，但未实现入口要这个名字
 * ============================================================================
 *
 * !! 本文件只有 aarch64 目标参与编译 !!
 */
#include "hle.h"
#include "loader.h"
#include "hle_diag.h"   /* dm_diag_mark：把「装槽结果」写进日志（iOS 上 stdout 没有去处） */
/* 同上：没有 hle_nvn_boot.h，直接声明用到的接口 */
extern void  *dm_host_current(void);

#include <stdio.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/* ---------------- ① pthread 键 ---------------- */
/* 改写后的客体不直接用 tpidr_el0/tpidrro_el0，而是读宿主的**线程槽**
 * （槽位置 = tpidrro_el0 + 8×键号；键页里存的就是「键号 × 8」）。
 * 所以宿主必须先建好这四个键。 */
static pthread_key_t g_keys[DM_KEY_COUNT];
static int           g_keys_ready = 0;

unsigned dm_thread_key(int which) {
    if (!g_keys_ready) {
        for (int i = 0; i < DM_KEY_COUNT; i++) {
            if (pthread_key_create(&g_keys[i], NULL) != 0) {
                fprintf(stderr, "[dm] pthread_key_create 失败（第 %d 个）\n", i);
                return 0;
            }
        }
        g_keys_ready = 1;
    }
    if (which < 0 || which >= DM_KEY_COUNT) return 0;
    return (unsigned)g_keys[which];
}

/* ---------------- ①b 往线程槽里填值（第 46 轮补 —— 这是闪退的直接原因） ----------
 *
 * !! 为什么必须有这一段 !!
 * 改写器把客体的三种线程相关访问都改成「读槽」：
 *      mrs x18, tpidrro_el0        →  x18 = *(宿主tpidrro_el0 + 键号K18×8)
 *      mrs/msr tpidr_el0           →  ...      键号KTLS×8
 *      mrs tpidrro_el0             →  ...      键号KTLR×8
 * 也就是说：**槽里必须放指针**，而且宿主必须把它们填上。
 * 而全仓库**从来没有人填过**（只有 hle_nn_thread.c 里给 nn::os 线程簿记用的
 * tls_set，用的是另一批键）。于是客体第一次用 x18/tpidr_el0 就拿到 NULL：
 *      ldr w0, [NULL, #8]  →  SIGSEGV  →  闪退（实测：卡在启动序列第 1 步）
 *
 * 三个槽该放什么（依据见 work/porting/tls_probe.py 在**解压映像**上量出的偏移范围）：
 *   K18 / KTLS → musl 的线程指针（TLS 基址）。**两者必须是同一个值** ——
 *                量出来 main 里有 1259 处 `mrs tpidr_el0`、146 处 x18 相对访存，
 *                偏移范围 x18 −96..+2488 / tpidr_el0 −8..+2352，就是同一块 TLS。
 *                负偏移是 AArch64 的 TCB（在 TP 下方），所以 TP 不能放在块首 ——
 *                这里给块首留 0x100 给 TCB，TP = 块首 + 0x100。
 *   KTLR      → Horizon 的 ThreadLocalRegion（sdk 里 124 处 mrs tpidrro_el0，
 *                相对偏移 +8..+496，量到最大 +4080）。同样给 TP 留 0x100。
 *   KGATE     → **&_dm_gate**（entry.S 的门入口）。不填的话，客体第一次
 *                svc / cntpct 读就是 `blr NULL` —— 第二次闪退点。
 *
 * 大小取值：TLS 块 0x8000、ThreadLocalRegion 0x2000。
 * 依据：NmplDLL 的 62 个 TLSDESC 里，TLS 偏移最大 14296（≈0x37D8），
 * 全模块 x18 相对访存最大 +14336（0x3800）→ 0x8000 足够且留了余量。 */
#define DM_TLS_BLOCK   (0x100 + 0x8000)     /* 前面 0x100 是 TCB 区 */
#define DM_TLR_BLOCK   (0x100 + 0x2000)     /* 前面 0x100 留余量 */
#define DM_TP_BIAS     0x100                /* TP 相对块首的偏移 */

typedef struct {
    unsigned char tls[DM_TLS_BLOCK];        /* musl TLS（K18 / KTLS 指向它） */
    unsigned char tlr[DM_TLR_BLOCK];        /* ThreadLocalRegion（KTLR） */
    int           installed;
} dm_thread_tls;

/* 本线程的块存哪儿：另用一个 pthread 键存指针（与那 4 个键不冲突） */
static pthread_key_t g_block_key;
static int           g_block_key_ready = 0;
static int           g_tls_installs = 0;      /* 装过几次（诊断） */

/* entry.S 的门入口（Mach-O 里叫 _dm_gate，C 侧名字就是 dm_gate） */
extern void dm_gate(void);

static dm_thread_tls *tls_block_this_thread(void) {
    if (!g_block_key_ready) {
        if (pthread_key_create(&g_block_key, NULL) != 0) return NULL;
        g_block_key_ready = 1;
    }
    dm_thread_tls *b = (dm_thread_tls *)pthread_getspecific(g_block_key);
    if (!b) {
        b = (dm_thread_tls *)calloc(1, sizeof *b);   /* 全零 = 「新线程」的状态 */
        if (!b) return NULL;
        pthread_setspecific(g_block_key, b);
    }
    return b;
}

/* ---- 主线程登记（第 54 轮加）---------------------------------------------
 *
 * SDK 的 nn::os::detail 用这条路径取「当前线程」：
 *     mrs  x8, tpidrro_el0        ← 被改写器换成"读 KTLR 槽"（我们给的 TLR 指针）
 *     ldr  x8, [x8, #0x1F8]       ← TLS+0x1F8 = 当前线程的 ThreadType*
 *     ldr  w8, [x8, #0x1B0]       ← ThreadType+0x1B0 = 线程 ID
 * 真机上这两个字段由内核/rtld 在创建线程时填好；宿主不跑它们，**必须自己填**，
 * 否则 SDK 第一次进临界区就会读到 0 → 崩在 0x1B0（第 53 轮实测：
 * `nn::os::detail::InternalCriticalSectionImplByHorizon::Leave` / `IsLockedByCurrentThread`）。
 * 创建的客体线程由 SDK 自己登记；主线程只能由宿主登记。 */
static void *g_current_thread_type = NULL;

void dm_plat_set_current_thread(void *thread_type) {
    g_current_thread_type = thread_type;
    if (thread_type) *(uint32_t *)((char *)thread_type + 0x1B0) = 1;  /* 线程 ID（自洽即可） */
    dm_diag_mark("主线程登记：ThreadType=%p（TLS+0x1F8 已写）", thread_type);
}

/* 把当前线程指针写进 TLR 的 +0x1F8（装槽时调用） */
static void install_current_thread(void *tlr_tp) {
    if (tlr_tp) *(void **)((char *)tlr_tp + 0x1F8) = g_current_thread_type;
}

/* 裸读 tpidrro_el0（与改写后的客体同一条路：它不经过 pthread_getspecific） */
static inline unsigned long long rd_tpidrro_raw(void) {
    unsigned long long v;
    __asm__ __volatile__("mrs %0, tpidrro_el0" : "=r"(v));
    return v;
}

/* 装槽时的 4 个「应填值」与键号（崩溃倾倒 / 读回核对用） */
static unsigned long long g_key_nums[DM_KEY_COUNT];
static unsigned long long g_want_vals[DM_KEY_COUNT];

/* 崩溃 handler 里调（第 55 轮）：把 4 个线程槽的「应填值 vs 裸读实值」写进日志。
 * 只做读 + 手写格式化 + write —— 信号处理里唯一安全的做法（与 hle_diag.c 同规）。
 * 起因：PrepareAbort 崩溃时 KTLR 槽里是野指针 0x14f014200，无法分辨
 * 「槽被改写」还是「Darwin 直通槽假设失效」，下次崩溃这行日志直接给出答案。 */
static void put_hex(char *buf, int *n, int cap, unsigned long long v) {
    char t[17]; int m = 0;
    if (!v) t[m++] = '0';
    while (v) { unsigned d = (unsigned)(v & 0xF); t[m++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    if (*n < cap - 1) buf[(*n)++] = '0';
    if (*n < cap - 1) buf[(*n)++] = 'x';
    while (m && *n < cap - 1) buf[(*n)++] = t[--m];
}

void dm_plat_dump_slots_crash(void) {
    char buf[512];
    int n = 0, cap = (int)sizeof buf;
    unsigned long long tp = rd_tpidrro_raw();
    const char *head = "\n[崩溃] 线程槽 tpidrro=";
    for (const char *p = head; *p && n < cap - 1; p++) buf[n++] = *p;
    put_hex(buf, &n, cap, tp);
    const char *names[DM_KEY_COUNT] = { " K18=", " KTLS=", " KTLR=", " KGATE=" };
    for (int i = 0; i < DM_KEY_COUNT; i++) {
        for (const char *p = names[i]; *p && n < cap - 1; p++) buf[n++] = *p;
        put_hex(buf, &n, cap, *(unsigned long long *)(tp + g_key_nums[i]));
        if (g_want_vals[i] != *(unsigned long long *)(tp + g_key_nums[i])) {
            const char *mm = "(≠应填";
            for (const char *p = mm; *p && n < cap - 1; p++) buf[n++] = *p;
            put_hex(buf, &n, cap, g_want_vals[i]);
            if (n < cap - 1) buf[n++] = ')';
        }
    }
    if (n < cap - 1) buf[n++] = '\n';
    dm_diag_append_crash_text(buf, n);
}

/* 给**当前线程**装好 4 个槽。幂等。必须在客体代码用到它们之前调用 ——
   也就是在 dm_guest_enter 之前（见 hle_nvn_guest.c）。
   宿主自己再建客体线程时（nn::os::CreateThread）也要在该线程里调一次。 */
void dm_plat_tls_install(void) {
    /* 先确保 4 个键已建好（dm_thread_key 会建） */
    unsigned k18 = dm_thread_key(DM_KEY_K18), ktls = dm_thread_key(DM_KEY_KTLS),
             ktlr = dm_thread_key(DM_KEY_KTLR), kgate = dm_thread_key(DM_KEY_KGATE);
    dm_thread_tls *b = tls_block_this_thread();
    if (!b) { fprintf(stderr, "[dm] TLS 块分配失败\n"); return; }

    void *tp_tls = b->tls + DM_TP_BIAS;
    void *tp_tlr = b->tlr + DM_TP_BIAS;

    /* SDK 从这里找「当前线程」（见上面的说明）—— 必须在客体跑起来之前写好 */
    install_current_thread(tp_tlr);

    /* TLR+0x1F0 = 「当前 TP」的 SDK 记账位（第 55 轮，从原 sdk 反汇编实测）：
     *   sdk+0x147fec（ThreadManagerImplByHorizon 构造）:
     *       mrs x8, tpidrro ; ldr x8, [x8, #0x1F0] ; msr tpidr_el0, x8
     *   sdk+0x99b4c（nn::os 内部）: str x22, [x8, #0x1F0] ; msr tpidr_el0, x22
     * 即：SDK 切/还原线程指针都经 TLR+0x1F0。不预填的话第一次切换后 TP=0，
     * main 里 1259 处 mrs tpidr_el0 全部读到空指针。 */
    *(void **)((char *)tp_tlr + 0x1F0) = tp_tls;

    pthread_setspecific((pthread_key_t)k18,   tp_tls);
    pthread_setspecific((pthread_key_t)ktls,  tp_tls);
    pthread_setspecific((pthread_key_t)ktlr,  tp_tlr);
    pthread_setspecific((pthread_key_t)kgate, (void *)dm_gate);
    g_tls_installs++;

    /* 记下键号与应填值（崩溃倾倒 dm_plat_dump_slots_crash 用） */
    g_key_nums[DM_KEY_K18]   = (unsigned long long)k18   * 8;
    g_key_nums[DM_KEY_KTLS]  = (unsigned long long)ktls  * 8;
    g_key_nums[DM_KEY_KTLR]  = (unsigned long long)ktlr  * 8;
    g_key_nums[DM_KEY_KGATE] = (unsigned long long)kgate * 8;
    g_want_vals[DM_KEY_K18]  = (unsigned long long)tp_tls;
    g_want_vals[DM_KEY_KTLS] = (unsigned long long)tp_tls;
    g_want_vals[DM_KEY_KTLR] = (unsigned long long)tp_tlr;
    g_want_vals[DM_KEY_KGATE]= (unsigned long long)dm_gate;

    /* 直通槽读回核对（第 55 轮诊断）：改写后的客体按「tpidrro_el0 + 8×键号」裸读，
     * 不经过 pthread_getspecific。这里用同一条路读回 —— 若不一致，说明 Darwin 的
     * 直通槽假设在这个键号上不成立，必须在进客体前发现（第 55 轮 .ips：
     * 崩在 PrepareAbort 时 KTLR 槽已是野指针 0x14f014200，事后无法分辨是哪一类）。 */
    {
        unsigned long long tp = rd_tpidrro_raw();
        unsigned long long got[DM_KEY_COUNT];
        int bad = 0;
        for (int i = 0; i < DM_KEY_COUNT; i++) {
            got[i] = *(unsigned long long *)(tp + g_key_nums[i]);
            if (got[i] != g_want_vals[i]) bad = 1;
        }
        dm_diag_mark("槽读回核对：%s（tpidrro=%llx K18=%llx KTLS=%llx KTLR=%llx KGATE=%llx）",
                     bad ? "**不一致 —— 直通槽假设失效！**" : "一致",
                     tp, got[0], got[1], got[2], got[3]);
    }

    if (!b->installed) {
        b->installed = 1;
        /* 走诊断通道（写日志 + 下次启动会显示在屏幕上），不用 printf ——
         * iOS 上 stdout 没有去处，屏幕上看到的只有 dm_diag 写进日志的那些行。
         * 这几个键号很关键：方案依赖「值就存在 tpidrro_el0 + 8×键号」，键号必须是
         * 小索引（Darwin 的直通槽），真机日志里能看到实际值。 */
        char line[256];
        snprintf(line, sizeof line,
                 "线程槽已装好：键号 K18=%u KTLS=%u KTLR=%u KGATE=%u；"
                 "TLS 块=%p TP=%p；TLR=%p TP=%p；门=%p",
                 k18, ktls, ktlr, kgate, (void *)b->tls, tp_tls,
                 (void *)b->tlr, tp_tlr, (void *)dm_gate);
        dm_diag_mark("%s", line);
    }
}

/* 单调递增的「装过几次」计数（诊断用） */
int dm_plat_tls_install_count(void) { return g_tls_installs; }


/* ---------------- ⑤ 客体堆（svc 1 SetHeapSize，第 57 轮加；D2R hle_svc.c 0x01 同语义） -----
 *
 * Horizon ABI：x1 = 期望大小；成功 x1 = 堆基址、x0 = 0。
 * 做法与 D2R 一致：预留应用内存池（Switch 4GB 机型给应用 3285MB），SetHeapSize 只
 * 记账返回基址 —— vm_allocate 预留的地址空间未触碰不提交物理页，无需 mprotect 游戏。
 */
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <sched.h>

#define DM_APP_POOL  (3285ull << 20)      /* 应用内存池 3285 MB */
static uint8_t *g_heap;
static uint64_t g_heap_cap;
static uint64_t g_heap_cur;               /* SetHeapSize 记账的大小（GetInfo 用） */

int dm_plat_heap_set_size(uint64_t size, uint64_t *base_out) {
    if (!g_heap) {
        vm_address_t addr = 0;
        kern_return_t kr = vm_allocate(mach_task_self(), &addr, DM_APP_POOL, VM_FLAGS_ANYWHERE);
        if (kr != KERN_SUCCESS) {
            dm_diag_mark("SetHeapSize：预留 %llu MB 失败 kr=%d", DM_APP_POOL >> 20, kr);
            return -1;
        }
        g_heap = (uint8_t *)addr;
        g_heap_cap = DM_APP_POOL;
        dm_diag_mark("客体堆：预留 %llu MB @ %p", DM_APP_POOL >> 20, g_heap);
    }
    if (size > g_heap_cap) return 1;      /* ResultLimitReached —— 游戏会按 2MB 递减重试 */
    g_heap_cur = size;
    if (base_out) *base_out = (uint64_t)g_heap;
    return 0;
}

/* GetInfo 用（第 58 轮）：堆基址 / 池上限 / 已记账大小 */
void dm_plat_heap_peek(uint64_t *base, uint64_t *cap, uint64_t *cur) {
    if (base) *base = g_heap ? (uint64_t)g_heap : 0;
    if (cap)  *cap  = g_heap ? g_heap_cap : 0;
    if (cur)  *cur  = g_heap_cur;
}

/* ---------------- ⑥ 客体线程（svc 8/9/0A，第 57 轮加；D2R hle_thread.c 同语义） -----------
 *
 * Horizon ABI：CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核)
 *              StartThread(w0 句柄) / ExitThread()
 * 每个客体线程 = 一个宿主 pthread：先装本线程的 4 个槽（dm_plat_tls_install 本来就是
 * 每线程一套 calloc 的 TLS/TLR），等 StartThread 放行，再切栈进客体入口。
 */
#define DM_NTHR 64
typedef struct {
    int used, started;
    uint64_t entry, arg, sp;
    pthread_t pt;
} dm_thr;
static dm_thr g_thrs[DM_NTHR];
static pthread_mutex_t g_thr_lock = PTHREAD_MUTEX_INITIALIZER;

extern void dm_thread_enter(void *entry, void *arg, void *sp);   /* entry.S 的 _dm_thread_enter */
extern void dm_thread_returned(void);                                /* 本文件 ② */

static void *dm_thr_trampoline(void *p) {
    dm_thr *t = p;
    dm_plat_tls_install();                     /* 本线程的 TLS/TLR/门槽（+TLR+0x1F0/0x1F8） */
    while (!__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) sched_yield();
    dm_thread_enter((void *)t->entry, (void *)t->arg, (void *)t->sp);
    dm_thread_returned();                      /* 客体入口不许返回 */
    return NULL;
}

int dm_plat_thread_create(uint64_t entry, uint64_t arg, uint64_t sp,
                          uint64_t prio, uint64_t core, uint64_t *handle_out) {
    (void)prio; (void)core;                    /* 宿主只记账，不排核 */
    pthread_mutex_lock(&g_thr_lock);
    int idx = -1;
    for (int i = 0; i < DM_NTHR; i++) if (!g_thrs[i].used) { idx = i; break; }
    if (idx >= 0) {
        dm_thr *t = &g_thrs[idx];
        t->used = 1; t->started = 0;
        t->entry = entry; t->arg = arg; t->sp = sp;
        if (pthread_create(&t->pt, NULL, dm_thr_trampoline, t) != 0) {
            t->used = 0;
            idx = -1;
        }
    }
    pthread_mutex_unlock(&g_thr_lock);
    if (idx < 0) {
        dm_diag_mark("CreateThread：线程表满/创建失败");
        return -1;
    }
    if (handle_out) *handle_out = 0x1001ull + (uint64_t)idx;   /* D2R 句柄 0x1001 起 */
    return 0;
}

void dm_plat_thread_start(uint64_t handle) {
    if (handle < 0x1001ull || handle >= 0x1001ull + DM_NTHR) return;
    __atomic_store_n(&g_thrs[handle - 0x1001ull].started, 1, __ATOMIC_RELEASE);
}

void dm_plat_thread_exit(void) {
    pthread_exit(NULL);                        /* 不返回（D2R thread_exit 同款） */
}

/* ---------------- ② 客体线程从入口返回 ---------------- */
/* Horizon 的线程必须经 nn::os::ExitThread 退出；入口函数返回就是 bug。
 * 这里如实报告（而不是静默把线程收掉）—— 静默会让「客体逻辑错」变成一个查不到的谜。 */
void dm_thread_returned(void) {
    fprintf(stderr, "[dm] 客体线程从入口函数返回了 —— Horizon 要求走 ExitThread，"
                    "这是客体逻辑错误（或启动参数不对）\n");
    pthread_exit(NULL);
}

/* ---------------- ③ entry.S 用的门分发入口 ---------------- */
/* work/host/entry.S 里是 `bl _dm_gate_dispatch_c`（x0 = 返回点，x1 = regs）。
 * 而 C 侧实现叫 dm_gate_dispatch（带 dm_host* 参数、内部用全局 host）。
 * 这里做一层适配。 */
extern void dm_gate_dispatch(dm_host *h, uint64_t ret, uint64_t *regs);
/* 注意：dm_host_current() 已经在本文件开头（第 20 行，#if 之外）声明过了。
 * 这里**不要再声明一次** —— 之前这里写成 `extern dm_host *dm_host_current(void);`，
 * 与上面的 `extern void *dm_host_current(void);` 返回类型冲突，
 * mac 上 clang 直接报：
 *   error: conflicting types for 'dm_host_current'
 * 真实定义在 hle_nvn_boot.c: `void *dm_host_current(void) { return &g_host; }`。 */

void dm_gate_dispatch_c(uint64_t ret, uint64_t *regs) {
    dm_gate_dispatch(dm_host_current(), ret, regs);
}

/* ---------------- ④ 客体线程切栈入口（汇编在 entry.S） ---------------- */
/* dm_thread_enter(entry, arg, sp) 由 entry.S 的 _dm_thread_enter 实现。
   这里只做一次「非空检查」，避免传空指针进去后 brk。 */
extern void dm_thread_enter(void *entry, void *arg, void *sp);   /* entry.S 的 _dm_thread_enter */

/* ---------------- NVN 未实现入口 ---------------- */
/* 跳板表里「没有实现」的槽指向这里。它必须**不崩**并且能报出是哪一个
 * （第 23 轮那套诊断靠它）。x16 里是槽表基址，但这里拿不到 idx ——
 * 所以如实报告「有未实现的 NVN 被调了」，并交给诊断计数。 */
extern uint64_t dm_nvn_unimpl(int idx, uint64_t x0, uint64_t x1);
void dm_nvn_unimpl_entry(void) {
    /* 没有 idx：用 -1 让诊断记成「未知」，而不是假装知道 */
    (void)dm_nvn_unimpl(-1, 0, 0);
}

#else  /* 非 aarch64 */

/* 这些函数在 Windows 自测里由各测试文件自己提供桩，这里不重复定义，
   以免与自测里的桩冲突（LNK2005）；只在真正需要它们的目标上提供实现。 */

#endif
