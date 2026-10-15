/* 宿主入口 —— dm_host_boot / dm_host_frame / dm_host_shutdown。
 *
 * 这两个函数是 iOS 外壳唯一需要调的东西（见 work/ios/front_ios.m）：
 *     dm_host_boot(romfs_dir, save_dir)  启动时一次
 *     dm_host_frame()                    每帧一次
 *
 * 【能做到什么、做不到什么 —— 必须分清】
 *
 * Windows 上能验证的（本文件的主要部分）：
 *   · 装载 5 个模块（NSO ×3 + NRO ×2）
 *   · 应用全部重定位、解析符号（含 238 个 nn:: 与 538 个 NVN 名）
 *   · 填键页、登记门、挂 RomFS、装载着色器映射表
 *   · 门分发表就绪、NVN 实现登记就绪
 *   → 这些是「宿主起来了」的实质内容，且**第 22~26 轮已逐项验证过**
 *
 * 只有 aarch64 真机/模拟器上才能做的（本文件里用平台条件隔开）：
 *   · 真正**跳进客体的入口点**开始执行客体代码
 *   · aarch64 跳板（客体拿到 NVN 函数表后按槽调用）
 *   → Windows 上把这两步写成「明确报未实现」，而不是假装做了
 *
 * 【关于客体入口点 —— 本轮的一笔账】
 *   我这一轮花了很大力气想静态定位 MOD0 里的入口点，**没有找到，方法也错了**：
 *   · MOD0 里没有内嵌 ELF 头（我一开始以为 header_off 指向 ELF 头，是错的）
 *   · MOD0 的 9 个字是成对的段边界（.eh_frame / .text / .data / .rodata 的起止）
 *   · 我还犯了一个错：把**文件偏移当虚拟地址**用（解压后映像里 seg 的 file≠mem），
 *     于是反汇编出来是数据，得出了错误的结论
 *
 *   正确做法（下一轮做）：Horizon 上模块是由 `nn::ro::LoadModule` 装载的 ——
 *   客体调它时会把入口点作为参数传进来。所以应当在 **nn::ro HLE 里截获入口点**，
 *   而不是去 MOD0 里猜。这个做法在 Windows 上可验证（用真实的调用参数自测）。
 */
#include "hle.h"
#include "loader.h"
#include "hle_nn.h"
#include "hle_nn_fs2.h"      /* nn::fs 的基本部分在 hle_nn_fs.c，没有单独的头（只在下面声明用到的两个） */
#include "hle_nn_thread.h"
#include "hle_nvn.h"
#include "hle_nvn_obj.h"
#include "hle_nvn_cmd.h"
#include "hle_nvn_metal.h"
#include "hle_nvn_glslc.h"
#include "hle_host_dir.h"      /* dm_host_res_dir()：资源目录（iOS 上必须是绝对路径） */
#include "hle_diag.h"          /* 诊断通道：把「走到哪一步」写进日志/屏幕 */

/* hle_nn_fs.c 提供（该文件没有配套头文件，故在此声明） */
extern void dm_nn_fs_set_romfs_root(const char *dir);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 宿主时钟（hle.c 需要；Windows 上用单调计数，真机侧由 hle_nn_thread 提供） */
static uint64_t g_ticks = 0;
uint64_t dm_host_ticks(void) { return ++g_ticks; }

/* 装载的模块清单（顺序与 D2R 一致：先 rtld，再 sdk，再 main；NRO 按需） */
static const char *g_mod_names[] = { "rtld", "sdk", "main", "NmplDLL.nro", "lz4.nro" };
#define DM_NMODS (sizeof g_mod_names / sizeof g_mod_names[0])

static dm_load_ctx g_ctx;
static dm_host     g_host;      /* 门表：dm_host 持有 ents/n（单个有序表） */
static int         g_booted = 0;
static char        g_romfs[1024], g_save[1200];

/* 线程键：真机侧由 hle_plat_aarch64.c 的 dm_thread_key() 真建 pthread 键；
   Windows 上给一组假键号即可（键页只存「键号×8」），避免自测时链接缺符号。 */
#if defined(_WIN32)
static unsigned thread_key_stub(int which) { return 10u + (unsigned)which; }
#define dm_thread_key thread_key_stub
#else
/* 原来这里漏了声明：非 Windows 分支上 clang 直接报
 *   error: call to undeclared function 'dm_thread_key';
 *          ISO C99 and later do not support implicit function declarations
 * 实现见 hle_plat_aarch64.c: `unsigned dm_thread_key(int which)`。 */
extern unsigned dm_thread_key(int which);
#endif

/* 客体入口点（dm_host_boot 里按名字找 nnMain 得到）。
   注意：C 里必须先声明后用 —— 上一版把它放在 dm_host_boot 之后，编译不过。 */
static void *g_guest_entry = NULL;
void dm_host_set_guest_entry(void *entry) { g_guest_entry = entry; }
void *dm_host_get_guest_entry(void) { return g_guest_entry; }

/* ------------------------------------------------------------------ */
/* 客体启动计划                                                        */
/* ------------------------------------------------------------------ */
/* 装载之后不能直接跳 nnMain —— SDK 运行时要先初始化。顺序照 D2R 的 main.c
 * （「照 SDK 里 nn::init::Start 的真实顺序初始化 SDK 运行时，再进 nnMain」），
 * 每一步的符号都由本游戏的真实 dynsym 解析得到（第 29/30 轮实测地址见注释）。
 *
 * !! 一个必须记住的坑 !!（D2R 的 main.c 第 60 行原话）
 *   略过 `_init_libc1` 会在 putc 嵌套加锁时**自锁死**。
 *   本游戏里它叫 `_init_libc1`（sdk + 0x56c210），
 *   而**不存在** `__nnmusl_init_locks` 这个名字。
 *
 * 另外：SDK 的 libc 是**客体自带**的（putchar_unlocked / pthread_mutex_* / memcpy /
 * printf 全都**定义在 sdk 里**），所以锁的初始化是客体自己的事 ——
 * 宿主只需**调** _init_libc1，而不是去提供它的实现。 */
typedef struct {
    void *init_libc1;      /* sdk + 0x56c210   —— 必须调，否则自锁死 */
    void *init_array;      /* ELF `_init`      —— 跑 C++ 静态构造；本游戏 dynsym 里没有 */
    void *os_initialize;   /* sdk + 0x1493e0 */
    void *init_startup;    /* main + 0x9ac350 */
    void *nn_main;         /* main + 0x9aec30  —— 客体入口 */
    int   n_found;
} dm_guest_boot_plan;

static dm_guest_boot_plan g_plan;

const void *dm_host_boot_plan(void) { return &g_plan; }

/* 宿主启动自检结果 */
static struct {
    unsigned nmods, nsyms, nunres, ngates;
    int      guest_entry_found;
    int      plan_found;
    unsigned n_tlsdesc;
    uint64_t n_relocs;
    int      shaders_loaded;
    int      vn_registered;
    int      fs_mounted;
    int      prot_fail;      /* 页保护失败的段数（第 48 轮；>0 说明代码可能不可执行） */
} g_boot_info;

const void *dm_host_boot_info(void) { return &g_boot_info; }

/* 门分发需要拿到「当前宿主」（entry.S 的 _dm_gate 只传返回点与 regs，没有 host 参数）。
   全进程只有一个宿主，所以暴露 g_host 的地址是安全的。
   —— 上一版漏了这个函数，Mac 上链接 entry.S 时会报 _dm_gate_dispatch_c 那一串未定义。 */
void *dm_host_current(void) { return &g_host; }

/* ---------------- 装载 ---------------- */

int dm_host_boot(const char *romfs_dir, const char *save_dir) {
    if (g_booted) return 0;
    if (!romfs_dir) return -1;

    dm_diag_mark("装载开始：romfs=%s", romfs_dir);
    dm_diag_mark("存档目录=%s", (save_dir && save_dir[0]) ? save_dir : "(无)");

    snprintf(g_romfs, sizeof g_romfs, "%s", romfs_dir);
    if (save_dir) snprintf(g_save, sizeof g_save, "%s", save_dir);

    /* 挂上兜底解析器：glslc* 是本游戏唯一的「不在模块里」的符号 */
    dm_loader_set_fallback(dm_glslc_lookup);
    dm_loader_reset(&g_ctx);
    /* 宿主先建好 pthread 键（键页里存的是「键号 × 8」，见第 12 轮） */
    g_ctx.key_values[DM_KEY_K18]   = dm_thread_key(DM_KEY_K18);
    g_ctx.key_values[DM_KEY_KTLS]  = dm_thread_key(DM_KEY_KTLS);
    g_ctx.key_values[DM_KEY_KTLR]  = dm_thread_key(DM_KEY_KTLR);
    g_ctx.key_values[DM_KEY_KGATE] = dm_thread_key(DM_KEY_KGATE);

    /* 模块二进制放在资源目录下（包内布局见 work/tools/dmpack.py）。
     *
     * !! 第 41 轮修正 !! 原来这里是 `getenv("DM_MODULE_DIR")`，否则用相对路径 "dm"。
     *   Windows 上开发时当前目录就是工作目录，所以 17 个自测全过；
     *   但 **iOS 上 App 的当前工作目录不是 bundle 目录** ——
     *   `fopen("dm/main.patched.bin")` 会失败，表现为「装上了但一起就报找不到游戏程序」。
     *   现在统一走 dm_host_res_dir()：Apple 上由 iOS 外壳用 NSBundle 给出绝对路径。 */
    const char *dir = dm_host_res_dir();

    char bin[1400], fix[1400];
    for (unsigned i = 0; i < DM_NMODS; i++) {
        snprintf(bin, sizeof bin, "%s/%s.patched.bin", dir, g_mod_names[i]);
        snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dir, g_mod_names[i]);
        /* 不要预设 m->name —— dm_loader_map 会 memset 结构，名字它自己推导 */
        dm_module *m = &g_ctx.mods[g_ctx.nmods];
        int rc = dm_loader_map(m, bin, fix, g_ctx.key_values);
        if (rc) {
            fprintf(stderr, "[dm] 装载 %s 失败 rc=%d (%s)\n", g_mod_names[i], rc, dm_loader_err(rc));
            dm_diag_mark("装载失败：第 %u 个模块 %s rc=%d", i, g_mod_names[i], rc);
            return -10 - (int)i;
        }
        dm_diag_mark("已装载模块 %s（base=%p size=%llu）", g_mod_names[i],
                     (void *)m->base, (unsigned long long)m->size);
        /* 登记给崩溃诊断：这样崩溃时能把 PC 翻成「模块 + 偏移」，
           直接拿去反汇编就知道是哪条指令炸的（不用再猜） */
        dm_diag_set_module(m->name, m->base, (unsigned long long)m->size);
        g_ctx.nmods++;
    }
    g_boot_info.nmods = g_ctx.nmods;

    /* 重定位（每个模块都要拿到全部模块的符号表） */
    dm_load_stats tot; memset(&tot, 0, sizeof tot);
    for (unsigned i = 0; i < g_ctx.nmods; i++) {
        dm_load_stats st; memset(&st, 0, sizeof st);
        dm_loader_relocate(&g_ctx.mods[i], g_ctx.mods, g_ctx.nmods, &st);
        tot.n_relative += st.n_relative; tot.n_abs += st.n_abs;
        tot.n_jump += st.n_jump; tot.n_unresolved += st.n_unresolved;
        tot.n_tlsdesc += st.n_tlsdesc;
        tot.n_bad_sym += st.n_bad_sym;
    }
    g_boot_info.n_relocs = tot.n_relative + tot.n_abs + tot.n_jump + tot.n_tlsdesc;

    /* !! 重定位做完之后、进客体之前：把各模块的**代码页设成可执行** !!
     * 没有这一步，客体第一条指令就会炸（内核不执行不可执行的页），
     * 而且症状是「宿主标记都正常、之后永远没下文」—— 第 48 轮才定位到。 */
    {
        int prot_fail = 0;
        for (unsigned i = 0; i < g_ctx.nmods; i++)
            prot_fail += dm_loader_finalize(&g_ctx.mods[i]);
        dm_diag_mark("页保护已设好（代码 R+X / rodata R / data RW / 补丁区 R+X）；失败 %d 段",
                     prot_fail);
        g_boot_info.prot_fail = prot_fail;
    }

    /* !! 执行自检：这台设备到底能不能执行「RW 映射 → mprotect 成 RX」的页 !!
     * 为什么必须亲自执行一条指令：上一次真机日志停在「调用 _init_libc1」之后再无下文，
     * 既没有崩溃行也没有看门狗行 —— 而 mprotect 的返回值看不出问题。
     * 现在改成**真的写入一条 ret 并调用它**：
     *   · 能返回 → 日志会写「执行自检：成功」
     *   · 进程直接没了 → 日志最后一行就是「即将调用 RW→RX 匿名页里的 ret」
     * 两种结果都会留在日志里，下次启动屏幕上就能看到。 */
    dm_diag_mark("即将做执行自检（匿名页 RW→RX 后执行一条 ret）");
    dm_diag_mark("执行自检结果 rc=%d（0=成功；-2= mprotect 失败）",
                 dm_loader_exec_selftest());

    /* 顺便问内核：各模块代码页**实际**的保护位（不是我们以为设了什么） */
    for (unsigned i = 0; i < g_ctx.nmods; i++) {
        char pr[8];
        int qrc = dm_loader_query_prot(&g_ctx.mods[i], pr, (int)sizeof pr);
        dm_diag_mark("代码页实际保护 %s = %s（查询 rc=%d）",
                     g_ctx.mods[i].name, (qrc == 0) ? pr : "?", qrc);
    }
    g_boot_info.n_tlsdesc = (unsigned)tot.n_tlsdesc;
    g_boot_info.nunres   = tot.n_unresolved;

    /* 门登记：把改写器产出的 gates.tsv 登记进 hle.c 的门表。
     *
     * 注意 dm_host 里是**单个有序表**（h->ents 指向它，dm_host_find 按返回点二分查找），
     * 而每个模块的 base 不同、返回点也不重叠 —— 所以把 5 个模块的门**合并进同一张表**
     * 再统一排序。逐模块各登记一次会让后一个覆盖前一个（这是我写第一版时的错）。 */
    {
        static dm_gate_ent tbl[1 << 16];
        size_t ntot = 0;
        for (unsigned i = 0; i < g_ctx.nmods; i++) {
            char gat[1400];
            snprintf(gat, sizeof gat, "%s/%s.gates.tsv", dir, g_mod_names[i]);
            int n = dm_loader_register_gates(&g_host, tbl + ntot,
                                             sizeof tbl / sizeof tbl[0] - ntot,
                                             gat, g_ctx.mods[i].base);
            if (n > 0) ntot += (size_t)n;
        }
        /* 合并后重新排序（各模块的表各自有序，合并后不一定） */
        for (size_t i = 1; i < ntot; i++) {
            dm_gate_ent k = tbl[i];
            size_t j = i;
            while (j > 0 && tbl[j-1].ret > k.ret) { tbl[j] = tbl[j-1]; j--; }
            tbl[j] = k;
        }
        g_host.ents = tbl;
        g_host.n    = ntot;
        g_boot_info.ngates = (unsigned)ntot;
    }

    /* RomFS 根（nn::fs 的路径映射用） */
    dm_nn_fs_set_romfs_root(g_romfs);
    if (g_save[0]) dm_nn_fs2_set_save_root(g_save);
    g_boot_info.fs_mounted = 1;

    /* 图形：登记 NVN 对象层与命令层实现 */
    dm_nvn_obj_register_all();
    dm_nvn_cmd_register_all();
    g_boot_info.vn_registered = (int)dm_nvn_registered();

    /* 着色器查找桥：载入映射表（Metal 目录在同级 metal/）。
       DM_SHADER_DIR 可覆盖 —— 开发时映射表与 Metal 源码在 work/shaders2/，
       出包时会被拷到与模块同级的 dm/ 下（见 work/tools/dmpack.py）。 */
    {
        const char *sdir = getenv("DM_SHADER_DIR");
        if (!sdir || !sdir[0]) sdir = dir;   /* 默认与模块同目录（包内 dm/） */
        char map[1400], metal[1400];
        snprintf(map,   sizeof map,   "%s/shader_map.tsv", sdir);
        snprintf(metal, sizeof metal, "%s/metal", sdir);
        g_boot_info.shaders_loaded = dm_nvn_shader_load(map, metal);
    }

    /* 客体启动计划：按名字解析 SDK 运行时初始化序列（**不是**去 MOD0 里猜）。
     *
     * NintendoSDK 的约定：游戏模块导出 `nnMain`，SDK 的 nn::init::Start 调它。
     * D2R 的 main.c 第 1 行就是这个做法。
     *
     * 实测地址（entry_selftest / libc_init2）：
     *   _init_libc1    -> sdk  + 0x56c210   （必须调，否则 putc 嵌套加锁自锁死）
     *   nnosInitialize -> sdk  + 0x1493e0
     *   nninitStartup  -> main + 0x9ac350
     *   nnMain         -> main + 0x9aec30   （客体入口）
     * 反汇编核对：nnMain 是 `ldr x30,[sp],#0x20 ; b`（尾调用跳板），
     * nninitStartup 是正常函数体 —— 两次独立确认。 */
    g_plan.init_libc1    = (void *)dm_loader_sym(g_ctx.mods, g_ctx.nmods, "_init_libc1");
    g_plan.init_array    = (void *)dm_loader_sym(g_ctx.mods, g_ctx.nmods, "_init");
    g_plan.os_initialize = (void *)dm_loader_sym(g_ctx.mods, g_ctx.nmods, "nnosInitialize");
    g_plan.init_startup  = (void *)dm_loader_sym(g_ctx.mods, g_ctx.nmods, "nninitStartup");
    g_plan.nn_main       = (void *)dm_loader_sym(g_ctx.mods, g_ctx.nmods, "nnMain");
    g_plan.n_found = (g_plan.init_libc1 ? 1 : 0) + (g_plan.init_array ? 1 : 0) +
                     (g_plan.os_initialize ? 1 : 0) + (g_plan.init_startup ? 1 : 0) +
                     (g_plan.nn_main ? 1 : 0);
    g_guest_entry = g_plan.nn_main;
    g_boot_info.guest_entry_found = g_plan.nn_main ? 1 : 0;
    g_boot_info.plan_found = g_plan.n_found;

    /* 装载完成 —— 这一行长期留在屏幕/日志上（真机排障的第一条线索） */
    dm_diag_boot("模块 %u 重定位 %llu 门 %u 未解析 %u 计划 %d/5 着色器 %d NVN %d 页保护失败 %d",
                 g_boot_info.nmods, (unsigned long long)g_boot_info.n_relocs,
                 g_boot_info.ngates, g_boot_info.nunres,
                 g_boot_info.plan_found, g_boot_info.shaders_loaded,
                 g_boot_info.vn_registered, g_boot_info.prot_fail);
    dm_diag_mark("装载完成（客体入口 %s）", g_guest_entry ? "已解析" : "**没解析到**");

    g_booted = 1;
    return 0;
}

/* ---------------- 逐帧 ---------------- */

/* 进客体：只有 aarch64 目标上才可能。
 *
 * Windows / x86 上**明确报未实现**，而不是假装走了。
 * 真机侧这里应当是：
 *     1. 取客体入口点（来自 nn::ro::LoadModule 的 HLE 截获，见文件头说明）
 *     2. 建客体主线程（栈 + TLS + 键页），x0 = 参数
 *     3. 跳进去 —— 之后客体自己跑，遇到 svc/平台指令由门回到宿主
 */
#if defined(__aarch64__) || defined(_M_ARM64)
  extern void dm_host_enter_guest(void);
  #define DM_CAN_ENTER_GUEST 1
#else
  #define DM_CAN_ENTER_GUEST 0
#endif

void dm_host_frame(void) {
    if (!g_booted) return;

    /* 只记第一次 —— 帧循环每帧都会调这里，不能每帧写日志 */
    static int g_frame_seen = 0;
    if (!g_frame_seen) { g_frame_seen = 1; dm_diag_mark("帧循环第一次调用 dm_host_frame"); }

#if DM_CAN_ENTER_GUEST
    /* 注意：这两个标记都用**一次性的 static 闸门**。
     * dm_host_frame 每帧都会被调（60 次/秒），不加闸门就会把日志刷爆
     * —— 日志文件写满是小事，「把日志刷到看不见真正的那一行」才是大事。 */
    static int g_logged_entry = 0, g_logged_noentry = 0;
    if (g_guest_entry) {
        if (!g_logged_entry) {
            g_logged_entry = 1;
            dm_diag_mark("dm_host_frame：进客体（入口 %p）", g_guest_entry);
        }
        dm_host_enter_guest();
        return;
    }
    /* 还没拿到入口点：说明 nn::ro::LoadModule 还没被调到 —— 真机侧应记录并等待 */
    if (!g_logged_noentry) {
        g_logged_noentry = 1;
        dm_diag_mark("dm_host_frame：没有客体入口点，等待中");
    }
#else
    /* 非 aarch64：宿主把能做的都做完了（装载/重定位/门/NVN/着色器表就绪），
       但**无法执行客体代码**。这里不静默成功 —— 让自测能看出这件事。 */
    static int warned = 0;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[dm] dm_host_frame: 本平台不是 aarch64，无法执行客体代码"
                        "（装载与门分发已就绪）\n");
    }
#endif
}

void dm_host_shutdown(void) {
    for (unsigned i = 0; i < g_ctx.nmods; i++) dm_loader_unmap(&g_ctx.mods[i]);
    g_ctx.nmods = 0;
    g_booted = 0;
}
