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

/* 登记「当前线程」的 ThreadType（主线程用；第 54 轮加）。
 * SDK 从 `tpidrro_el0 + 0x1F8` 取当前线程、再读它的 +0x1B0（线程 ID）——
 * 真机上这由内核/rtld 填，宿主必须自己填，否则 SDK 一进临界区就崩在 0x1B0。
 * **必须在 dm_plat_tls_install() 之前调用**（装槽时会把这个指针写进 TLR）。 */
void dm_plat_set_current_thread(void *thread_type);

/* 装过几次（诊断：正常进客体前应为 1） */
int  dm_plat_tls_install_count(void);

/* 崩溃 handler 里调（第 55 轮）：把 4 个线程槽的「应填值 vs 裸读实值」写进日志。
 * 只做读 + 手写格式化 + write(2)，信号处理安全。 */
void dm_plat_dump_slots_crash(void);

/* 第 59 轮：运行期槽核对 + 自愈（每次门调用时调一次）。发现 4 槽里任何一个是
 * 「非应填值」（第 59 轮真机：K18 槽在运行中变成 0，而启动时核对是好的）就立刻
 * 恢复成应填值，并把「第一次发生」连触发门一起记进日志。
 * 成本：每门 4 次槽读 + 比较（L1 级，可忽略）。 */
void dm_plat_slots_guard(uint64_t ret);

/* 第 59 轮：当前线程的 ThreadLocalRegion 地址（= KTLR 槽里的值）。没装槽时 0。
 * 用途：svc 0x21 SendSyncRequest 的请求消息就放在「本线程 TLR 的头 0x100 字节」
 * （Horizon 的线程本地 IPC 命令缓冲）——诊断里抄出来，下一轮日志就能看到
 * sm: 会话到底在请求什么（命令号是 sm: 协议的核心线索）。 */
uint64_t dm_plat_tlr_addr(void);

/* ---- 客体线程与堆（第 57 轮加，svc 1/8/9/0A 用；照 D2R hle_svc.c / hle_thread.c 语义）---- */

/* SetHeapSize：预留/扩出客体堆。size = 期望字节数；成功 *base_out = 堆基址。
 * 返回 0 = 成功；1 = 超出应用内存池（ResultLimitReached）；-1 = 分配失败。 */
int  dm_plat_heap_set_size(uint64_t size, uint64_t *base_out);

/* CreateThread：建宿主 pthread（跑 trampoline：装线程槽 → 等 StartThread → 切栈进客体）。
 * 成功返回 0 并写 *handle_out（0x1001 起）。prio/core 目前只记账。 */
int  dm_plat_thread_create(uint64_t entry, uint64_t arg, uint64_t sp,
                           uint64_t prio, uint64_t core, uint64_t *handle_out);
/* StartThread：放行等待中的线程入口。 */
void dm_plat_thread_start(uint64_t handle);
/* ExitThread：终止当前宿主 pthread（不返回）。 */
void dm_plat_thread_exit(void);
/* 堆现状（svc 0x29 GetInfo 用；未初始化时全 0）。 */
void dm_plat_heap_peek(uint64_t *base, uint64_t *cap, uint64_t *cur);

#endif /* aarch64 */

/* 非 aarch64 目标给空/假实现，调用点不用包 #if。
 * MSVC 的 C 模式对 inline 支持不稳，直接用 static（未用会有警告但不报错）。 */
#if !defined(__aarch64__) && !defined(_M_ARM64)
static void dm_plat_dump_slots_crash(void) { }
static void dm_plat_slots_guard(uint64_t ret) { (void)ret; }
static uint64_t dm_plat_tlr_addr(void) { return 0; }
static int  dm_plat_heap_set_size(uint64_t size, uint64_t *base_out) {
    (void)size; if (base_out) *base_out = 0x10000000ull; return 0;   /* 自测用假堆 */
}
static int  dm_plat_thread_create(uint64_t e, uint64_t a, uint64_t s,
                                  uint64_t p, uint64_t c, uint64_t *h) {
    (void)e; (void)a; (void)s; (void)p; (void)c; if (h) *h = 0; return -1;   /* Windows 不建线程 */
}
static void dm_plat_thread_start(uint64_t handle) { (void)handle; }
static void dm_plat_thread_exit(void) { }
static void dm_plat_heap_peek(uint64_t *base, uint64_t *cap, uint64_t *cur) {
    if (base) *base = 0x10000000ull; if (cap) *cap = 3285ull << 20; if (cur) *cur = 0;
}
#endif

#endif /* DM_HLE_PLAT_H */
