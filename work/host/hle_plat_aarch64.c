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

    pthread_setspecific((pthread_key_t)k18,   tp_tls);
    pthread_setspecific((pthread_key_t)ktls,  tp_tls);
    pthread_setspecific((pthread_key_t)ktlr,  tp_tlr);
    pthread_setspecific((pthread_key_t)kgate, (void *)dm_gate);
    g_tls_installs++;

    if (!b->installed) {
        b->installed = 1;
        printf("[dm] 线程槽已装好：键号 K18=%u KTLS=%u KTLR=%u KGATE=%u；"
               "TLS 块=%p（TP=%p） TLR=%p（TP=%p） 门=%p\n",
               k18, ktls, ktlr, kgate, (void *)b->tls, tp_tls, (void *)b->tlr, tp_tlr,
               (void *)dm_gate);
    }
}

/* 单调递增的「装过几次」计数（诊断用） */
int dm_plat_tls_install_count(void) { return g_tls_installs; }


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
extern void dm_thread_enter_asm(void *entry, void *arg, void *sp);

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
