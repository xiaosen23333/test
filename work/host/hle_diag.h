/* 宿主侧诊断通道 —— 把「走到哪一步」变成能看见的文字。
 *
 * ============================================================================
 * 为什么需要它（第 45 轮加）
 * ============================================================================
 * 真机上排查「黑屏」目前只有一条路：看屏幕上的状态文字。但这条路的**上限很低**：
 *
 *   ① 客体（游戏）是**跑在主线程上**的 —— dm_host_frame → dm_host_enter_guest →
 *      nnMain。客体一旦不返回（游戏自己的循环就是这样的），主线程就被它占住了，
 *      **屏幕上的字再也不会更新**，触摸也不响应。也就是说：卡在哪一步，屏幕上
 *      永远显示的是「进客体之前」那一句。
 *   ② 如果客体崩了（SIGSEGV 等），整个 App 直接消失，那就什么也看不到了。
 *
 * 所以真正可靠的通道是**文件**：
 *   · 每一条阶段标记都会**立刻写进日志文件并 fflush**（不依赖缓冲区被冲出去，
 *     因为真机上往往是崩溃之后才去看它）
 *   · 下一次启动时，iOS 外壳把日志的尾部**直接显示在屏幕上** ——
 *     于是「上次跑到哪一步崩的」不用连电脑、不用拷文件，开机就能看到
 *
 * 分工：
 *   hle_diag.c  本文件 —— 存文本 + 写文件 + 拼一行总览（平台无关，Windows 上也编）
 *   front_ios.m iOS 外壳 —— 指定日志路径、把日志尾部显示到屏幕、每 30 帧刷新总览
 *   hle_nvn_boot.c / hle_nvn_guest.c —— 在关键位置打标记（装载、进客体五步）
 * ============================================================================
 */
#ifndef DM_HLE_DIAG_H
#define DM_HLE_DIAG_H

/* ---- 日志文件 ---- */

/* 指定日志文件（iOS 外壳启动时调一次，指向 <Documents>/dm-log.txt）。
 * 传 NULL 关闭。文件超过 256 KB 时会**只保留尾部 64 KB**（避免无限增长）。
 * 之所以要分大小：真机上一跑就是几个小时，日志会越写越大。 */
void dm_diag_set_log(const char *path);

/* ---- 打标记 ---- */

/* 追加一条事件（写日志 + 记为「最后事件」）。适合一次性的事情：
 * 装载开始/结束、进客体、门表登记完成… */
void dm_diag_mark(const char *fmt, ...);

/* 覆盖式：客体启动序列走到第几步（1..5；0 = 还没进客体）。
 * **每次调用都会立刻写日志** —— 见文件头的说明：客体占着主线程时，
 * 日志是唯一还能更新的通道。 */
void dm_diag_stage(int stage, const char *fmt, ...);

/* 装载完成后的摘要（一行，长期保留在屏幕/日志上） */
void dm_diag_boot(const char *fmt, ...);

/* ---- 读取（给屏幕用）---- */

int         dm_diag_stage_no(void);
const char *dm_diag_stage_text(void);   /* 形如 "3/5 调用 nninitStartup" */
const char *dm_diag_boot_text(void);
const char *dm_diag_last_event(void);

/* 一行总览：客体走到哪 + 命令录音/回放 + 本帧 pass/draw/present。
 * 写进 buf（不超过 n，含结尾 0），返回写入的字符数。
 * 这几个数字正好回答「帧循环有没有跑」「客体有没有到渲染」「画面为什么是黑的」。*/
int dm_host_diag(char *buf, int n);

/* ---- 崩溃时要用到的（信号处理里只有 `write(2)` 是安全的）---- */

/* 登记一个模块的基址（装载时调）。这样崩溃时能把 PC 翻成「模块 + 偏移」，
 * 直接拿去反汇编就知道是哪条指令炸的 —— 不用再猜。 */
void dm_diag_set_module(const char *name, const void *base, unsigned long long size);

/* 日志文件的 fd（-1 = 没有）。崩溃时直接往它写，不经 stdio。 */
int  dm_diag_log_fd(void);

/* 把地址翻成「模块名 + 偏移」。写入 out 并返回 1；不在任何模块内返回 0。
 * **不做任何内存分配、不调 stdio** —— 信号处理里也能用。 */
int  dm_diag_addr(unsigned long long addr, char *out, int n);

/* 崩了：把信号号 / 故障地址 / PC / LR / SP / x0-x3 写进日志。
 * 本函数只做「拼字符串 + write()」，别的什么都不做（信号安全）。
 * x 可为 NULL。 */
void dm_diag_crash_line(int sig, unsigned long long fault_addr,
                        unsigned long long pc, unsigned long long lr,
                        unsigned long long sp, const unsigned long long *x, int nx);

/* ---- 门调用记录（第 47 轮加）----
 * 客体每次经门（svc / 读 cntpct）进宿主时记下返回点。用处：
 * 崩溃/卡死时能看到最后几个门调用是**哪些客体站点** —— 用来区分
 *   「在客体代码里自旋」 与  「死在某个宿主 HLE 里（比如等一个永远不来的锁）」。
 * 只做一次数组写入，很便宜；可以每帧都调。 */
void dm_diag_gate(unsigned long long ret);

/* ---- 看门狗（第 47 轮加）----
 * 主线程**卡住**和**崩溃**是两种完全不同的情况：
 *   · 崩溃 → 信号处理写 [崩溃] 行（dm_diag_crash_line）
 *   · 卡住 → 主线程不再前进，信号不会来，屏幕也刷不动
 * 所以由 iOS 外壳起一个看门狗线程，发现「主线程 N 秒没有进展」时写一行
 * [看门狗]，把当前阶段与最近门调用一起记下来。
 * 本函数在**普通线程**里被调（不是信号上下文），可以用 stdio。 */
void dm_diag_hang_line(unsigned long long seconds);

#endif /* DM_HLE_DIAG_H */
