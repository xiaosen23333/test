/* aarch64 平台侧补件的接口（实现见 hle_plat_aarch64.c）。
 *
 * 为什么要单独一个头：这个文件里的东西**只有 aarch64 目标才产生代码**
 * （pthread 键、线程槽、切栈入口、门分发适配）。以前是「用到的人在 .c 里自己
 * extern 声明」，第 44 轮就因为两处声明返回类型不一致（void* vs dm_host*）
 * 在 Mac 上报 conflicting types。现在统一放这里，一处定义、处处包含。 */
#ifndef DM_HLE_PLAT_H
#define DM_HLE_PLAT_H

#if defined(__aarch64__) || defined(_M_ARM64)

/* 给**当前线程**装好客体要用的 4 个线程槽（K18/KTLS/KTLR/KGATE）。幂等。
 *
 * !! 必须在客体代码用到它们之前调用 !! —— 也就是 dm_guest_enter 之前。
 * 不调的后果（第 46 轮实测）：客体第一次用 x18/tpidr_el0 读到 NULL →
 * `ldr w0, [NULL, #8]` → SIGSEGV → 闪退（卡在启动序列第 1 步）。
 * 宿主自己再建客体线程时（nn::os::CreateThread 那条路）也要在该线程里调一次。 */
void dm_plat_tls_install(void);

/* 装过几次（诊断：正常进客体前应为 1） */
int  dm_plat_tls_install_count(void);

#endif /* aarch64 */

#endif /* DM_HLE_PLAT_H */
