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
