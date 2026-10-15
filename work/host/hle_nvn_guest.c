/* 宿主入口 —— 进客体（aarch64 专用）。
 *
 * ============================================================================
 * 这是「Windows 侧能写、但只有 aarch64 上能跑」的那一步。
 * 本文件的所有函数在 Windows 上**不编译**（由 hle_nvn_boot.c 里的
 * `#if defined(__aarch64__) || defined(_M_ARM64)` 隔开）。
 * ============================================================================
 *
 * 进客体要做的四件事（顺序不能错）：
 *   ① 建客体主线程的**栈**（Horizon 主线程栈大小通常 1 MB，取值见下）
 *   ② 给主线程**填键页与线程槽**（改写后的客体读 tpidrro_el0 + 8*键号 取槽；
 *      四个槽 K18/KTLS/KTLR/KGATE 必须都填，否则客体一读 x18 就崩）
 *   ③ 按启动计划依次调：_init_libc1 → （跑 __init_array）→ nnosInitialize
 *      → nninitStartup → nnMain
 *   ④ **不能在宿主线程上直接跳** —— 客体以为自己在它自己的栈上，
 *      且 x30 要求是「返回即错误」的哨兵。所以切栈到 ①，用 dm_guest_enter。
 *
 * 关于启动顺序的诚实说明：
 *   启动计划里五个符号的**地址**都是实测确定的（见 hle_nvn_boot.c 的注释），
 *   但它们的**调用顺序与参数**在 Windows 上无法验证 ——
 *   我核对过：main 里**没有** bl 到 nninitStartup/nnMain 的调用点（0 处），
 *   说明 SDK 是通过**函数指针**调它们的（很可能是 nn::init::Start 里）。
 *   所以下面的顺序是「照 D2R 的 main.c 描述 + 符号语义」排的，
 *   **真机上可能需要按实测调整**。调整点已用 TODO 标出。
 */
#include "hle.h"
#include "loader.h"
#include "hle_nn.h"
#include "hle_diag.h"      /* 诊断通道：五步各打一个标记（真机上这是唯一能看到的东西） */
#include <stdint.h>     /* uintptr_t —— nnosInitialize 传主线程块地址 */
#include "hle_plat.h"      /* dm_plat_tls_install：进客体前必须先把 4 个线程槽装好 */
/* 注意：没有 hle_nvn_boot.h —— 启动逻辑在 hle_nvn_boot.c 里，
   它的接口只有三个函数，直接在这里声明（我第一版 #include 了一个不存在的头，
   编译报 C1083）。理想做法是把它们提到一个头文件共用。 */
extern void  *dm_host_get_guest_entry(void);
extern const void *dm_host_boot_plan(void);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(__aarch64__) || defined(_M_ARM64)

/* 声明 _dm_guest_enter（汇编，见 dm_guest_entry.S）：
   x0 = 入口地址, x1 = 参数, x2 = 客体栈顶 */
extern void dm_guest_enter(void *entry, void *arg, void *stack_top);

/* 客体入口返回了（不该发生 —— Horizon 线程必须走 ExitThread）。
   与 D2R 的 _dm_thread_returned 同语义：报告后终止该线程。 */
extern void dm_thread_returned(void);

/* 客体主线程栈。Horizon 的主线程栈按 1 MB 准备（SDK 默认值；真机上若
   客体自己再建线程，那些线程的栈由 nn::os::CreateThread 的参数给出）。 */
#define DM_GUEST_STACK_SIZE (1u << 20)
static unsigned char g_guest_stack[DM_GUEST_STACK_SIZE] __attribute__((aligned(16)));

/* 启动计划的结构（与 hle_nvn_boot.c 里的 dm_guest_boot_plan 一致）。
   注意：这里**必须**与那边的字段顺序完全相同 —— 两处独立定义是隐患，
   理想做法是提到头文件共用（我在第 29 轮的错里记过这条）。 */
typedef struct {
    void *init_libc0;      /* sdk::_init_libc0 */
    void *os_initialize;   /* nnosInitialize（x0 = 主线程句柄，x1 = 0） */
    void *init_libc1;      /* sdk::_init_libc1 */
    void *sdk_module;      /* nninitInitializeSdkModule */
    void *init_startup;    /* nninitStartup */
    void *init_libc2;      /* sdk::_init_libc2 */
    void *init_array;      /* 各模块 DT_INIT（_init）—— 全局构造函数 */
    void *nn_main;         /* nnMain */
    int   n_found;
} dm_guest_boot_plan_t;
/* !! 上面这 9 个字段的顺序必须与 hle_nvn_boot.c 的 dm_guest_boot_plan 完全相同 !!
   （两处独立定义是隐患，理想做法是提到一个头文件里共用。） */

/* nnosInitialize 的 x0 是「主线程句柄」（第 55 轮改，.ips 实证的崩溃链）：
 * D2R 的 main.c 传的是 svc_thread_main_init() 返回的句柄（其句柄表 0x1000+i
 * 的第一个 = 0x1001）。SDK 把它存进全局，ThreadManagerImplByHorizon 的构造函数
 * （sdk+0x147f4c，本次 .ips 栈帧 #7 所在函数）拿它去调 GetThreadPriority /
 * GetProcessId。第 54 轮及之前我们传的是一块零填充内存的**地址** —— SDK 把
 * 地址低 32 位当句柄用，语义完全不对。宿主侧 svc 实现目前不校验句柄，
 * 值自洽即可；取与 D2R 相同的 0x1001。 */
#define DM_MAIN_THREAD_HANDLE 0x1001ull

/* SDK 自己的 ThreadType 是 0x470+ 字节（ThreadManager 构造里 memset 0x470、
 * 字段写到 +0x418）。这块缓冲只是「万一有路径对它写」的保险，给足 0x800。 */
static unsigned char g_main_thread_type[0x800] __attribute__((aligned(16)));

/* 主体：真正的“进客体”逻辑。放在单独函数里，是为了让 dm_host_enter_guest
   只承担「调一次」的职责，便于以后加诊断。 */
static void guest_main(void) {
    const dm_guest_boot_plan_t *P = (const dm_guest_boot_plan_t *)dm_host_boot_plan();
    if (!P) { fprintf(stderr, "[dm] 没有启动计划\n"); return; }

    /* 注意：**键页与线程槽不用在这里填** ——
       dm_loader_map（hle_nvn_boot.c 的装载循环里）已经把「键号×8」写进键页了
       （loader.c:154-158）。我第一版在这里又写了一个 dm_load_install_key_page 调用，
       而那个函数根本不存在 —— 链接会失败。写入口代码时容易重复做已经做过的事，
       所以这里明确写一行「不做什么」。 */

    /* 依启动计划调用。
     *
     * !! 顺序说明（真机上可能要调）!!
     *   D2R 的 main.c 第 60 行：「逐条照抄 Start：_init_libc1 = __nnmusl_init_locks
     *   （把 stdio 等锁初始化成递归锁，略过会在 putc 嵌套加锁时自锁死）」——
     *   所以 _init_libc1 **必须最先**，且在客体有机会 printf 之前。
     *   之后是 SDK 运行时 nnosInitialize，再 nninitStartup，最后 nnMain。
     *
     * 【第 39 轮的反汇编核对结果 —— 把原先的两个 TODO 收窄了】
     *   · 五个符号的地址**全部落在各自模块的 text 段**（核过）：
     *       _init_libc1    -> sdk  + 0x56c210  (text)
     *       nnosInitialize -> sdk  + 0x1493e0  (text)
     *       nninitStartup  -> main + 0x9ac350  (text)
     *       nnMain         -> main + 0x9aec30  (text, 72 条指令, **不用 x0 → 无参**)
     *     所以「跳到垃圾地址」这个致命风险已排除。
     *   · `nninitStartup` 的**参数不重要**：它开头把 x0 存到全局 0xba56a8，
     *     而那个全局在 main/sdk 里**只被写、从不被读**（各扫了一遍：引用数 1 / 0）。
     *     所以传什么都不会被用到 —— 原先我担心的「回调参数」不存在。
     *     （教训：我第 37 轮从一行 `str x0, [x8,#0x6a8]` 就推断「那是回调」，
     *       是过度解读；**「谁读它」才是判据**。）
     *
     * TODO(真机): 剩下唯一不确定的是**这五步之间的先后**。
     *   若卡在某一步不再前进，把这五步各自的耗时打出来就能定位。
     */
    typedef void (*fn0)(void);
    typedef void (*fn2)(uint64_t, uint64_t);   /* nnosInitialize(主线程句柄, 0) */
    /* !! 五步都打诊断标记（第 45 轮加）!!
     * 为什么重要：客体**跑在主线程上**，它一旦不返回，屏幕上的字就再也不会更新
     * （CADisplayLink 的回调卡在这里面）。所以这五条标记写的是**日志文件**，
     * 而且每写一条就 fflush —— 真机上崩溃/卡死之后，日志里最后一条就是答案。
     * 下一次启动时，iOS 外壳会把日志尾部直接显示在屏幕上。
     *
     * !! 第 47 轮改的顺序（有真机证据）!!
     *   真机日志停在「第 1 步：调用 _init（C++ 静态构造）」—— 后面**没有**「_init 已返回」，
     *   说明卡在 _init 里面。而按 D2R 的记录，`_init_libc1` 必须**最先**跑：
     *     「略过它会在 putc 嵌套加锁时自锁死」（本文件开头引的就是这句）。
     *   上一版恰好把 _init 放在 _init_libc1 前面 —— 于是 _init 里的任何输出
     *   （静态构造很常见）都会去拿还没初始化成递归锁的 libc 锁 → 自锁死 → 卡住。
     *   现在改成：**_init_libc1 最先**，再 _init（C++ 静态构造）。 */
    /* !! 顺序照 D2R 的 dm/host/main.c（真机验证过的脚本）!!
     *   _init_libc0 → nnosInitialize(主线程句柄, 0) → _init_libc1
     *   → nninitInitializeSdkModule → nninitStartup → _init_libc2
     *   → 各模块 DT_INIT（全局构造）→ nnMain
     * 第 52 轮我们踩过的坑：把 `_init`（DT_INIT）放在最前面调 → 它去调
     * nn::ro::ProtectRelro，而那个函数要读的表是 **rtld 装载时**填的（我们没 rtld）
     * → 拿偏移当指针 → 段错误。现在 nn::ro 一律走宿主实现（loader.c 的 dm_loader_sym），
     * 而且顺序也照 D2R 排。 */
    if (P->init_libc0) {
        dm_diag_stage(1, "调用 _init_libc0");
        ((fn0)P->init_libc0)();
        dm_diag_stage(1, "_init_libc0 已返回");
    } else {
        dm_diag_stage(1, "没有 _init_libc0 符号（跳过）");
    }

    if (P->os_initialize) {
        dm_diag_stage(2, "调用 nnosInitialize（SDK 运行时；x0=主线程句柄）");
        ((fn2)P->os_initialize)(DM_MAIN_THREAD_HANDLE, 0);
        dm_diag_stage(2, "nnosInitialize 已返回");
    } else {
        dm_diag_stage(2, "**没有 nnosInitialize 符号**");
    }

    if (P->init_libc1) {
        dm_diag_stage(3, "调用 _init_libc1（libc 锁；略过会在 putc 嵌套加锁时自锁死）");
        ((fn0)P->init_libc1)();
        dm_diag_stage(3, "_init_libc1 已返回");
    } else {
        dm_diag_stage(3, "没有 _init_libc1 符号（跳过）");
    }

    /* !! 第 62 轮：全局构造**不在这里跑**，挪到 _init_libc2 之后（见下面第 7 步）!!
     * 第 61 轮曾把它提前到这里，理由是「第 4 步要用服务描述表」。但真机 .ips
     * （DmGame-2026-10-15-222913）证明：在这里跑会直接跳进无效页
     * —— PC = 0x10ac39d40（KERN_PROTECTION_FAILURE / Invalid Page），
     *   不在 Guest 映像范围（Guest 在 0x10d10c000）。
     * D2R Mobile 真机源码 dm/host/main.c:64-78 的顺序是逐字照抄 Start 的：
     *   _init_libc0 → nnosInitialize → _init_libc1 → nninitInitializeSdkModule
     *   → nninitStartup → _init_libc2 → 【各模块 DT_INIT + init_array】→ nnMain
     * 也就是构造在 **SDK 初始化全部完成之后**。第 4 步要用服务描述表这件事，
     * D2R 不是靠提前跑构造解决的，而是把 nninitInitializeSdkModule 调的 7 个
     * 「连 Horizon 服务进程」的初始化**强制接管成空操作**（本轮已补齐，
     * 见 hle_nvn_more.c 的 dm_nn_fs_InitializeForApplication 等）。 */

    if (P->sdk_module) {
        dm_diag_stage(4, "调用 nninitInitializeSdkModule");
        ((fn0)P->sdk_module)();
        dm_diag_stage(4, "nninitInitializeSdkModule 已返回");
    } else {
        dm_diag_stage(4, "没有 nninitInitializeSdkModule 符号（跳过）");
    }

    if (P->init_startup) {
        dm_diag_stage(5, "调用 nninitStartup");
        ((fn0)P->init_startup)();
        dm_diag_stage(5, "nninitStartup 已返回");
    } else {
        dm_diag_stage(5, "**没有 nninitStartup 符号**");
    }

    if (P->init_libc2) {
        dm_diag_stage(6, "调用 _init_libc2");
        ((fn0)P->init_libc2)();
        dm_diag_stage(6, "_init_libc2 已返回");
    } else {
        dm_diag_stage(6, "没有 _init_libc2 符号（跳过）");
    }

    /* !! 第 7 步：各模块 DT_INIT + .init_array（全局构造）—— 照 D2R 放在 _init_libc2 之后 !!
     * 这是 rtld 的「回调 B」。第 62 轮 .ips 证明提前到 SDK 初始化之前会跳进无效页。 */
    {
        extern void dm_boot_run_module_inits(void);   /* hle_nvn_boot.c */
        dm_diag_stage(7, "跑各模块全局构造（DT_INIT + .init_array）");
        dm_boot_run_module_inits();
        dm_diag_stage(7, "全局构造已完成");
    }
    /* 计划里独立的 _init 兜底：ProtectRelro 的宿主实现幂等，重复调无害。 */
    if (P->init_array) {
        dm_diag_stage(7, "补跑计划里的 _init（兜底）");
        ((fn0)P->init_array)();
        dm_diag_stage(7, "兜底 _init 已返回");
    } else {
        dm_diag_stage(7, "没有 _init 符号（跳过）");
    }

    if (P->nn_main) {
        dm_diag_stage(8, "调用 nnMain（客体入口，之后游戏自己跑）");
        ((fn0)P->nn_main)();
        dm_diag_stage(9, "nnMain 已返回（游戏不该从 nnMain 返回）");
    } else {
        dm_diag_stage(8, "**没有 nnMain 符号**");
    }

    /* 客体从 nnMain 返回 = 游戏退出。真机侧这里应当正常退出 App。 */
    fprintf(stderr, "[dm] nnMain 返回了（游戏应当是自己退出的）\n");
}

/* 客体主线程栈区间（svc 6 QueryMemory 用，第 57 轮加）：
 * SDK 初始化主线程记录时拿当时的 sp 查 QueryMemory，把回报的整段当主线程栈
 * （D2R hle_svc.c 的同款语义）。 */
void dm_guest_stack_range(void **base, size_t *size) {
    if (base) *base = g_guest_stack;
    if (size) *size = DM_GUEST_STACK_SIZE;
}

/* 由 hle_nvn_boot.c 的 dm_host_frame 调用（aarch64 分支） */
void dm_host_enter_guest(void) {
    static int entered = 0;
    if (entered) return;      /* 客体自己会一直跑，不该被重复进入 */
    entered = 1;
    dm_diag_mark("dm_host_enter_guest：开始进客体");

    void *entry = dm_host_get_guest_entry();
    if (!entry) {
        fprintf(stderr, "[dm] 还没有客体入口点（nnMain 未解析）\n");
        dm_diag_mark("**没有客体入口点** —— 回到宿主");
        return;
    }

    /* 客体栈顶：栈向下生长，所以给的是**高地址端**，并保持 16 字节对齐
       （AAPCS64 要求 SP 在函数入口 16 字节对齐） */
    void *stack_top = (void *)(g_guest_stack + DM_GUEST_STACK_SIZE);

    /* !! 第 46 轮：进客体之前**必须**先把线程槽装好 !!
     * 客体的 x18 / tpidr_el0 / tpidrro_el0 已被改写器换成「读宿主线程槽」，
     * 门调用（svc、读 cntpct）也走槽取门地址。槽里本来是空的 → 客体一用就拿到
     * NULL → 第一次访存即 SIGSEGV（实测闪退在启动序列第 1 步）。
     * 这一步在所有真机路径上都是**第一个**要做的。 */
    /* 先登记「当前线程」，再装槽（装槽时会把它写进 TLR+0x1F8 —— SDK 从那里取当前线程） */
    dm_plat_set_current_thread(g_main_thread_type);
    dm_plat_tls_install();
    dm_diag_mark("线程槽已装好（装过 %d 次）", dm_plat_tls_install_count());

    dm_diag_mark("切栈进客体（栈 %u KB）", DM_GUEST_STACK_SIZE / 1024);
    dm_guest_enter((void *)guest_main, NULL, stack_top);

    /* 能回到这里说明 dm_guest_enter 返回了（即客体那一路走完了） */
    dm_diag_mark("客体那一路已返回宿主");
}

#else  /* 非 aarch64：本文件不产出任何代码 */

/* 空实现，避免链接期找不到符号（Windows 自测里 hle_nvn_boot.c 不会调它） */
void dm_host_enter_guest(void) { }
void dm_guest_stack_range(void **base, size_t *size) { if (base) *base = 0; if (size) *size = 0; }

#endif
