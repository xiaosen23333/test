/* 宿主 HLE —— 门表装载、分发、以及系统调用的实现。
 *
 * 设计要点（与改写器的约定）：
 *   1. 门的唯一标识是**返回点**（门块 ldp 的地址）。_dm_gate 把它放在 x0 传进来。
 *   2. 门的类别与参数由 gates.tsv 提供（改写器产出）：
 *        返回点 \t 类别 \t 参数 \t 原指令地址
 *      类别 = svc | cntpct | cntvct；参数 = svc 号 或 目标寄存器号 Rt。
 *   3. 回写规则：
 *        svc      → 结果写 regs[x0..x7]（Horizon 的出参在 x0..x7）
 *        cntpct/vct → 时间戳写 regs[arg]（**目标可以是任意 xN**，实测有 x19..x23）
 *   4. 未登记的门：不改任何寄存器，只在诊断里记一笔（保持客体原行为）。
 *
 * 实测依据（见 work/porting/PHASE3-GATES-REPORT.md）：
 *   - sdk 101 个门 = 101 个唯一返回点，一对一，无歧义
 *   - svc 存根的 x8 里**没有**调用号（形状是 `svc #n · ret`），所以必须靠返回点辨别
 */
#include "hle.h"
#include "hle_diag.h"    /* dm_diag_gate：记下门调用站点（区分「客体自旋」与「死在宿主 HLE」） */
#include "hle_plat.h"    /* 第 57 轮：堆/线程的平台侧实现（hle_plat_aarch64.c；Windows 有假桩） */

/* MSVC 把 sscanf 标为不安全；这里输入是我们自己产出的 tsv，不需要 _s 版本 */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>       /* VirtualAlloc / VirtualProtect —— alias 区的 Windows 侧 */
#else
#include <sys/mman.h>      /* mmap / mprotect —— alias 区（svc 0x2C/0x2D）用 */
#endif

/* Windows 自测覆盖 dm_plat_tlr_addr() 的返回值（hle_plat.h 已声明 extern）。
 * 放在 hle.c 里是为了让每个链接 hle.c 的 exe 都有定义（避免其它自测 LNK2019）。 */
#if !defined(__aarch64__) && !defined(_M_ARM64)
uint64_t dm_test_tlr_override = 0;
#endif

/* ---------------- 门表 ---------------- */

static int cmp_ent(const void *a, const void *b) {
    uint64_t x = ((const dm_gate_ent *)a)->ret, y = ((const dm_gate_ent *)b)->ret;
    return (x < y) ? -1 : (x > y);
}

int dm_host_load_gates(dm_host *h, dm_gate_ent *table, size_t cap, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (n >= cap) break;
        unsigned long long ret = 0, orig = 0;
        char kind[16] = {0};
        long arg = 0;
        /* 行格式：返回点 \t 类别 \t 参数 \t 原指令地址 */
        if (sscanf(line, "%llu\t%15[^\t]\t%ld\t%llu", &ret, kind, &arg, &orig) < 4) continue;
        dm_gate_ent *e = &table[n++];
        e->ret  = ret;
        e->orig = orig;
        e->arg  = (uint32_t)arg;
        e->name = NULL;
        if      (!strcmp(kind, "svc"))     e->kind = DM_GATE_SVC;
        else if (!strcmp(kind, "cntpct"))  e->kind = DM_GATE_CNTPCT;
        else if (!strcmp(kind, "cntvct"))  e->kind = DM_GATE_CNTVCT;
        else { n--; continue; }
    }
    fclose(f);
    qsort(table, n, sizeof *table, cmp_ent);
    h->ents = table;
    h->n    = n;
    return 0;
}

const dm_gate_ent *dm_host_find(const dm_host *h, uint64_t ret) {
    size_t lo = 0, hi = h->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (h->ents[mid].ret == ret) return &h->ents[mid];
        if (h->ents[mid].ret < ret) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/* ---------------- 系统调用 ---------------- */

/* 58 个调用的名字表（只为诊断可读性；未列出的显示为 ?）。
   注意下标上界：最大用到的下标是 0x73 = 115，所以数组要 116 项。 */
static const char *svc_name(uint32_t n) {
    static const char *t[116] = {
        [0x01]="SetHeapSize", [0x02]="SetMemoryPermission", [0x03]="SetMemoryAttribute",
        [0x04]="MapMemory", [0x05]="UnmapMemory", [0x06]="QueryMemory",
        [0x07]="ExitProcess", [0x08]="CreateThread", [0x09]="StartThread",
        [0x0a]="ExitThread", [0x0b]="SleepThread", [0x0c]="GetThreadPriority",
        [0x0d]="SetThreadPriority", [0x0e]="GetThreadCoreMask", [0x0f]="SetThreadCoreMask",
        [0x10]="GetCurrentProcessorNumber", [0x11]="SignalEvent", [0x12]="ClearEvent",
        [0x13]="MapSharedMemory", [0x14]="UnmapSharedMemory", [0x15]="CreateTransferMemory",
        [0x16]="CloseHandle", [0x17]="ResetSignal", [0x18]="WaitSynchronization",
        [0x19]="CancelSynchronization", [0x1a]="ArbitrateLock", [0x1b]="ArbitrateUnlock",
        [0x1c]="WaitProcessWideKeyAtomic", [0x1d]="SignalProcessWideKey",
        [0x1e]="GetSystemTick", [0x1f]="ConnectToNamedPort",
        /* 第 59 轮按 switchbrew SVC 表 + D2R hle_svc.c/hle_thread.c 对齐（本段编号之前
           错位了一位：Break/ODS 曾被记成 0x27/0x28，真号是 0x26/0x27；见 [svc] 日志的核对） */
        [0x20]="SendSyncRequestLight",
        [0x21]="SendSyncRequest", [0x22]="SendSyncRequestWithUserBuffer",
        [0x23]="SendAsyncRequestWithUserBuffer", [0x24]="GetProcessId", [0x25]="GetThreadId",
        [0x26]="Break", [0x27]="OutputDebugString", [0x28]="ReturnFromException",
        [0x29]="GetInfo", [0x2a]="FlushEntireDataCache", [0x2b]="FlushDataCache",
        [0x2c]="MapPhysicalMemory", [0x2d]="UnmapPhysicalMemory",
        [0x2f]="UnmapProcessMemory", [0x30]="QueryProcessMemory",
        [0x32]="TerminateProcess", [0x33]="GetProcessList", [0x34]="GetThreadList",
        [0x35]="GetSystemInfo", [0x36]="GetSystemInfo2",
        [0x40]="CreateSession", [0x41]="AcceptSession", [0x42]="ReplyAndReceive",
        [0x43]="ReplyAndReceiveWithUserBuffer", [0x44]="CreateEvent",
        [0x45]="MapTransferMemory", [0x46]="UnmapTransferMemory",
        [0x47]="CreateInterruptEvent", [0x48]="QueryPhysicalAddress",
        [0x49]="QueryIoMapping", [0x4a]="CreateDeviceAddressSpace",
        [0x4b]="SetThreadActivity", [0x4c]="GetThreadContext3",
        [0x50]="WaitForAddress", [0x51]="SignalToAddress",
        [0x52]="SleepSystem", [0x53]="ReadWriteRegister",
        [0x54]="SetProcessActivity", [0x55]="CreateSharedMemory",
        [0x56]="MapTransferMemory2", [0x57]="UnmapTransferMemory2",
        [0x70]="GetThreadLocalRegion", [0x71]="SetThreadLocalRegion",
        [0x72]="GetProcessInfo", [0x73]="GetThreadInfo",
    };
    return (n < 116 && t[n]) ? t[n] : "?";
}

/* 该调用的 x0 是「结果码」还是「返回值」？
 *
 * Horizon 的惯例：**除下面两条外，全部 svc 的 x0 = 结果码（0 = 成功）**，
 * 输出值走 x1..x7（带输出参数的 Get* 尤其如此 —— D2R 的 hle_thread.c 同款）。
 * 第 55 轮真机教训：曾把 0x0c/0x25/0x26 判成「x0 = 值」，于是
 * GetThreadPriority 返回 x0=0x2C 被客体当错误码 → SDK 走进
 * nn::diag::detail::Abort → PrepareAbort 写 TLR+0x100 → 崩溃（.ips 实证）。
 */
static int x0_is_result_code(uint32_t n) {
    switch (n) {
    /* x0 = 返回值（无结果码）—— 只有这两个 */
    case 0x10:  /* GetCurrentProcessorNumber */
    case 0x1e:  /* GetSystemTick */
        return 0;
    default:
        return 1;
    }
}

/* svc 实现：返回 0 = 已处理（regs 已写好内容）；2 = 未实现（dispatch 按「假成功」
   x0=0 处理并记日志）。-1 这个旧语义（未实现→错误码）第 59 轮已弃用。 */
/* ---------------- alias 区（SDK 的 VammManager 用；照 D2R hle_svc.c）----------------
 * Horizon 里「别名区」是一段**预留的地址空间**：MapPhysicalMemory 把其中的页变成
 * 可读写，UnmapPhysicalMemory 再收回去。SDK 初始化时靠 GetInfo(2/3) 拿它的位置和
 * 大小，之后 SDK 自己在这段里挑地址、再用 svc 0x2C 把那一页「打开」。
 *
 * 我们之前把 GetInfo(2/3) 报成堆池、又没实现 0x2C —— SDK 拿到「成功」但页根本没
 * 打开，回头一写就 SIGSEGV。第 64 轮 .ips 实证：
 *   [svc] 44 MapPhysicalMemory ... **未实现>假成功**
 *   [崩溃] 信号 11、故障地址 0x0、PC 0x10ab833b8 (sdk+0x1133b8)、第 4 步
 *
 * 所以这里照 D2R：单独预留一段（512 MB，PROT_NONE，只占地址空间不占内存），
 * 0x2C 把它 mprotect 成 RW、0x2D 收回成 NONE，越界则照 D2R 报 0xD401。 */
#define DM_ALIAS_CAP 0x20000000ull      /* 512 MB，与 D2R 的 g_alias_cap 同 */
static uint8_t *g_alias;
static size_t   g_alias_cap;

static void dm_alias_ensure(void) {
    if (g_alias) return;
#if defined(_WIN32)
    g_alias = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)DM_ALIAS_CAP, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap(NULL, (size_t)DM_ALIAS_CAP, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    g_alias = (p == MAP_FAILED) ? NULL : (uint8_t *)p;
#endif
    g_alias_cap = g_alias ? (size_t)DM_ALIAS_CAP : 0;
    dm_diag_mark("alias 区 %p +%#zx（SDK 的 VammManager 用）", (void *)g_alias, g_alias_cap);
}

/* 给 svc 6 QueryMemory 用（hle_nvn_boot.c 的 dm_boot_query_region）：查询落在 alias
   区里就回报整段为「空闲」，SDK 才知道去哪儿挑空位。 */
void dm_guest_alias_range(void **base, size_t *size) {
    dm_alias_ensure();
    if (base) *base = g_alias;
    if (size) *size = g_alias_cap;
}

/* ---------------- ASLR 区（GetInfo 12/13；照 D2R hle_svc.c 的 RESERVE）----------------
 * 为什么必须有：第 66 轮真机日志实证 —— 客体请求堆大小
 * `0xfffffffffe000000`（17592186044384 MB），正好等于 **0 − 0x2000000**，
 * 而 0x2000000 就是我们 GetInfo(16) SystemResource 总量。也就是说客体算堆大小时
 * 用到了一项我们回报 0、而 D2R 回报真值的量 —— 就是 ASLR 区大小（id 13）。
 * 报 0 让公式出负数 → 客体拿这个负数当堆大小 → 我们拒了它又不会算 → 走不通。
 * 照 D2R：预留一大块只占地址空间的 PROT_NONE 区，把基址和长度报出去。
 * 预留失败就逐级退让（5G → 2G → 1G），总比报 0 强。 */
#define DM_ASLR_WANT  (5ull << 30)
static uint8_t *g_as;
static size_t   g_as_cap;

static void dm_aslr_ensure(void) {
    if (g_as) return;
    static const size_t tries[] = { (size_t)DM_ASLR_WANT, 2u << 30, 1u << 30 };
    for (unsigned i = 0; i < sizeof tries / sizeof tries[0] && !g_as; i++) {
#if defined(_WIN32)
        void *p = VirtualAlloc(NULL, tries[i], MEM_RESERVE, PAGE_NOACCESS);
#else
        void *p = mmap(NULL, tries[i], PROT_NONE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) p = NULL;
#endif
        if (p) { g_as = (uint8_t *)p; g_as_cap = tries[i]; }
    }
    dm_diag_mark("ASLR 区 %p +%#zx（GetInfo 12/13）", (void *)g_as, g_as_cap);
}

void dm_guest_aslr_range(void **base, size_t *size) {
    dm_aslr_ensure();
    if (base) *base = g_as;
    if (size) *size = g_as_cap;
}

static int host_svc_impl(uint64_t *r, uint32_t n) {
    switch (n) {
    case 0x1e:  /* GetSystemTick —— x0 就是 tick 值 */
        r[DM_REG_X0] = dm_host_ticks();
        return 0;
    case 0x10:  /* GetCurrentProcessorNumber —— x0 就是编号 */
        r[DM_REG_X0] = 0;
        return 0;
    /* ---- 以下全部是「x0 = 结果码、值在 x1..x2」（第 55 轮按 D2R hle_thread.c 核对）---- */
    case 0x01: {  /* SetHeapSize —— x1=期望大小 → 成功 x1=堆基址（D2R hle_svc.c 0x01）。
                     失败给可重试的结果码：游戏启动按 2MB 递减重试（main+0x1b66184 同款）。 */
        uint64_t base = 0;
        int hrc = dm_plat_heap_set_size(r[DM_REG_X1], &base);
        if (hrc == 0) {
            /* 第 66 轮：把「请求的大小」和「回报的基址」都打出来 —— 上一轮只看到
               x1=fffffffffe000000，分不清那是请求值还是基址，白猜一轮。 */
            dm_diag_mark("[svc] 1 SetHeapSize 成功：请求 %#llx（%llu MB）→ 基址 %#llx",
                         (unsigned long long)r[DM_REG_X1],
                         (unsigned long long)(r[DM_REG_X1] >> 20),
                         (unsigned long long)base);
            r[DM_REG_X0] = 0; r[DM_REG_X1] = base; return 0;
        }
        /* 失败：x0 写真正的错误码并 return 3（分发器**不得**把它抹成成功）。
           第 66 轮 .ips：之前抹成成功后，客体把入参 size 当成堆基址写 → 段错误。 */
        dm_diag_mark("[svc] 1 SetHeapSize **失败** 请求 %#llx（%llu MB）→ %s",
                     (unsigned long long)r[DM_REG_X1],
                     (unsigned long long)(r[DM_REG_X1] >> 20),
                     hrc == 1 ? "超出应用内存池 0x10801" : "分配失败 -1");
        r[DM_REG_X0] = (hrc == 1) ? 0x10801ull : (uint64_t)-1;
        return 3; }
    case 0x06: {  /* QueryMemory(MemoryInfo* x0, PageInfo* x1, addr x2) —— 第 57 轮加。
                     SDK 初始化主线程时拿当时的 sp 查询、把回报的整段当主线程栈；
                     宿主直接穿 x0 指针写 MemoryInfo（D2R hle_svc.c 0x06 同款）。
                     布局：base(0) size(8) state(0x10) attr(0x14) perm(0x18)。 */
        uint64_t mi = r[DM_REG_X0], base = 0, size = 0;
        uint32_t state = 3, perm = 5;
        if (!dm_boot_query_region(r[DM_REG_X2], &base, &size, &state, &perm)) {
            base = r[DM_REG_X2] & ~0xFFFull; size = 0x1000;   /* 未知区域：单页 Code 兜底 */
        }
        if (mi) {
            uint64_t *m = (uint64_t *)mi;
            uint32_t *w = (uint32_t *)mi;
            m[0] = base; m[1] = size;
            w[4] = state; w[5] = 0; w[6] = perm; w[7] = 0; w[8] = 0;
        }
        r[DM_REG_X0] = 0; r[DM_REG_X1] = 0;    /* x1 → 包装函数写 *PageInfo */
        return 0; }
    case 0x08: {  /* CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) */
        uint64_t handle = 0;
        if (dm_plat_thread_create(r[DM_REG_X1], r[DM_REG_X2], r[DM_REG_X3],
                                  r[DM_REG_X4], r[DM_REG_X5], &handle) == 0) {
            r[DM_REG_X0] = 0; r[DM_REG_X1] = handle;
        } else {
            r[DM_REG_X0] = 0xCE01ull;          /* ResultOutOfResource */
        }
        return 0; }
    case 0x09: {  /* StartThread(w0 句柄) */
        dm_plat_thread_start(r[DM_REG_X0]);
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x0a:    /* ExitThread()：终止当前宿主 pthread（不返回；Windows 桩为空） */
        dm_plat_thread_exit();
        r[DM_REG_X0] = 0;
        return 0;
    case 0x1f: {  /* ConnectToNamedPort(name=x1) → x1=会话句柄（第 58 轮）。
                     目前只认 "sm:"：SDK 的 nn::sm 初始化带 50ms 重试循环
                     （sdk+0x15900c，连上后只查 svc 返回值，不解析应答缓冲）。
                     第 59 轮：名字指针先做下限检查（小整数/近零地址一律当不存在）——
                     「把某样东西当指针读」正是这一轮崩溃的教训。 */
        const char *name = (const char *)r[DM_REG_X1];
        static int n_sm;
        if ((uintptr_t)name > 0x10000 &&
            name[0] == 's' && name[1] == 'm' && name[2] == ':' && name[3] == 0) {
            if (n_sm++ < 3) dm_diag_mark("[sm:] 连接 \"sm:\" → 成功（假会话，IPC 只回成功）");
            r[DM_REG_X0] = 0;
            r[DM_REG_X1] = 0xC0000001ull + (unsigned)n_sm;
        } else {
            char nb[12]; int k = 0;
            if ((uintptr_t)name > 0x10000)
                for (; name[k] && k < 8; k++) nb[k] = name[k];
            nb[k] = 0;
            dm_diag_mark("[端口] ConnectToNamedPort(\"%s\") → 不存在", nb);
            r[DM_REG_X0] = 0xE701ull;      /* NoSuchPort */
        }
        return 0; }
    case 0x20:   /* SendSyncRequestLight(句柄=x0)：与 0x21 同形，只是不走「会话打开态」检查 */
    case 0x21: { /* SendSyncRequest(句柄=x0) —— 第 59 轮修正（此前把 0x21 当 WithUserBuffer，是崩溃直接原因）。
                     .ips 实证（r59）：ConnectToNamedPort 返回的会话句柄 0xC0000002 被客体原样放进
                     x0 调本调用；上一版却去读「x0 指向的缓冲」→ SIGSEGV，故障地址 = 0xC0000002
                     （.ips：esr 是 svc、崩溃指令 ldr w10,[x9]、x9 = 句柄本身）。
                     switchbrew SVC 表：0x21 = SendSyncRequest(Handle)（**无缓冲参数**）；
                     0x22 才是 SendSyncRequestWithUserBuffer(buf, size, handle)。
                     ==> 本调用**绝不把句柄当指针读**，只回成功（与 D2R hle_svc.c 的默认分支同语义）。
                     第 60 轮：SDK 的 sm: 初始化在调用前后会用 TLR 头做「内核有没有真处理」探针：
                       调用前：TLR[0..7] = 0x8000000000000010, TLR[8..11] = 1
                       调用后：TLR[0..7] = 0x0000000100000010, TLR[8..11] = 0
                       若探针未被清零，SDK 认为 IPC 失败并 Abort（r59 真机停在第 121 帧）。
                     因此当 TLR 头呈现上述请求形态时，我们把它改写成标准应答形态。 */
        uint64_t sess = r[DM_REG_X0];
        uint64_t tlr = dm_plat_tlr_addr();
        int wrote_response = 0;
        if (tlr) {
            volatile uint64_t *h = (volatile uint64_t *)tlr;
            volatile uint32_t *w = (volatile uint32_t *)tlr;
            uint64_t hdr = h[0];
            uint32_t probe = w[2];
            if (probe == 1 && hdr == 0x8000000000000010ULL) {
                /* Horizon CMIF 请求 → 应答：清请求标志、写结果头、清探针。
                 * 这是 sm: 初始化唯一需要的最小欺骗；后续服务若用不同请求头再补。 */
                h[0] = 0x0000000100000010ULL;
                w[2] = 0;
                wrote_response = 1;
                dm_diag_mark("[ipc] svc%u SendSyncRequest(会话=%llx) ok -> TLR=%llx 写应答头 0x%lx 探针 1->0",
                             n, (unsigned long long)sess,
                             (unsigned long long)tlr, 0x0000000100000010ULL);
            } else if (n == 0x21) {
                /* 其它形态：只读日志，暂不盲写 —— 等日志告诉我们这是什么服务。 */
                static unsigned n_ssr_other;
                if (n_ssr_other++ < 16)
                    dm_diag_mark("[ipc] svc%u SendSyncRequest(会话=%llx) ok；TLR头=%08x %08x %08x %08x %08x %08x",
                                 n, (unsigned long long)sess,
                                 w[0], w[1], w[2], w[3], w[4], w[5]);
            }
        } else {
            static unsigned n_ssr_no_tlr;
            if (n_ssr_no_tlr++ < 4)
                dm_diag_mark("[ipc] svc%u SendSyncRequest(会话=%llx) ok（无 TLR）", n,
                             (unsigned long long)sess);
        }
        (void)wrote_response;  /* 未来调试用 */
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x22: { /* SendSyncRequestWithUserBuffer(buf=x0, size=x1, 句柄=x2) —— 第 59 轮加。
                     真语义下 buf 确实是用户缓冲；但 0x21 的教训还热着，本轮**先不 deref**：
                     只记录三个入参，等真机日志确认它确实被调用、且 buf 像真实指针后
                     再上内容读取（宁可少一个诊断，也不要再崩在「把值当指针」上）。 */
        static unsigned n_wub;
        if (n_wub++ < 16)
            dm_diag_mark("[ipc] svc22 WithUserBuffer 会话=%llx buf=%llx size=%llx（暂不读内容）",
                         (unsigned long long)r[DM_REG_X2], (unsigned long long)r[DM_REG_X0],
                         (unsigned long long)r[DM_REG_X1]);
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x23:   /* SendAsyncRequestWithUserBuffer：先只回成功、不碰内存（出现再说） */
        r[DM_REG_X0] = 0;
        return 0;
    case 0x24:   /* GetProcessId(句柄) —— 真号 0x24（第 59 轮修正：之前记在 0x25） */
        r[DM_REG_X0] = 0; r[DM_REG_X1] = 1;
        return 0;
    case 0x25:   /* GetThreadId(句柄) —— 真号 0x25（第 59 轮修正：之前 0x25 记的是 GetProcessId）。
                    宿主线程层还没做 id 表，先给自洽的值 1（SDK 只做簿记/断言用）。 */
        r[DM_REG_X0] = 0; r[DM_REG_X1] = 1;
        return 0;
    case 0x26: {  /* Break(reason, ptr, size) —— 真号 0x26（第 59 轮修正：之前记成 0x27）。
                     只记不拦（真致命时 SDK 自己会走 diag Abort，那里有完整停车+门历史）。
                     注：D2R 对低值 reason 直接 _exit(42) 快速失败；我们选择记录后放行 ——
                     让宿主带着状态继续跑能多拿日志，真 Abort 时日志会点名它。 */
        static int n_br;
        if (n_br++ < 5) dm_diag_mark("[Break] reason=%llx ptr=%llx",
                                     (unsigned long long)r[DM_REG_X0],
                                     (unsigned long long)r[DM_REG_X1]);
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x27: {  /* OutputDebugString(str, len) —— 真号 0x27（第 59 轮修正：之前记成 0x28）。
                     SDK 的内部报错/警告走这条路；抄进日志（限频 20 行、限长 95、
                     指针下限 + 长度上限检查 —— 读之前先确认它像个真字符串）。 */
        const char *s = (const char *)r[DM_REG_X0];
        uint64_t len = r[DM_REG_X1];
        static int n_ods;
        if (n_ods++ < 20) {
            if ((uintptr_t)s > 0x10000 && len > 0 && len <= 4096) {
                char t[96]; int k = 0;
                for (; (uint64_t)k < len && k < 95 && s[k]; k++) t[k] = s[k];
                t[k] = 0;
                dm_diag_mark("[客体输出] %s", t);
            } else {
                dm_diag_mark("[客体输出] （str=%llx len=%llu 不像字符串，不读）",
                             (unsigned long long)r[DM_REG_X0], (unsigned long long)len);
            }
        }
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x28:   /* ReturnFromException（void）—— 真号 0x28（第 59 轮修正） */
        r[DM_REG_X0] = 0;
        return 0;
    case 0x2a:   /* FlushEntireDataCache（void）：宿主不需要做 */
    case 0x2b:   /* FlushDataCache(addr, size) */
        r[DM_REG_X0] = 0;
        return 0;
    /* 第 64 轮顺带补的「内存族」（都是**承诺了一块内存可用**的 svc：假成功比报错更糟，
       SDK 拿到成功就回去读写，页没打开立刻段错误 —— 0x2C 已经实证过一次）。 */
    case 0x02:   /* SetMemoryPermission(dst, size, perm)：D2R 直接回成功（我们不改页权限） */
    case 0x03:   /* SetMemoryAttribute(dst, size, attr0, attr1)：同上 */
        r[DM_REG_X0] = 0;
        return 0;
    case 0x04: {  /* MapMemory(dst=x0, src=x1, size=x2) —— 搬移语义：把 src 的内容搬到 dst，
                     期间 src 不可访问。D2R：目标是栈区，mprotect 成可读写后 memcpy。
                     我们没有预留栈区，所以保守处理：先试 mprotect（说明本来就有映射），
                     失败再 mmap 一段固定的匿名内存补上；然后搬内容。 */
        uint8_t *dst = (uint8_t *)r[DM_REG_X0], *src = (uint8_t *)r[DM_REG_X1];
        size_t sz = (size_t)r[DM_REG_X2];
        if (!dst || !sz || sz > (256u << 20)) {
            dm_diag_mark("**svc 0x04 MapMemory 参数可疑** dst=%p src=%p size=%#zx",
                         (void *)dst, (void *)src, sz);
            r[DM_REG_X0] = 0xD401ull;
            return 0;
        }
        int rc;
#if defined(_WIN32)
        { DWORD old; rc = VirtualProtect(dst, sz, PAGE_READWRITE, &old) ? 0 : 1;
          if (rc) rc = (VirtualAlloc(dst, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == NULL); }
#else
        rc = mprotect(dst, sz, PROT_READ | PROT_WRITE);
        if (rc) {
            void *q = mmap(dst, sz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            rc = (q == MAP_FAILED);
        }
#endif
        if (rc) {
            dm_diag_mark("**svc 0x04 MapMemory 打不开目标** %p+%#zx", (void *)dst, sz);
            r[DM_REG_X0] = 0xDC01ull;
            return 0;
        }
        if (src) memcpy(dst, src, sz);
        dm_diag_mark("svc MapMemory %p <- %p +%#zx", (void *)dst, (void *)src, sz);
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x05: {  /* UnmapMemory(dst=x0, src=x1, size=x2)：内容搬回源区（D2R 同款）。
                     16K 页可能与相邻映射共用，所以不收回权限（收了会误伤）。 */
        uint8_t *dst = (uint8_t *)r[DM_REG_X0], *src = (uint8_t *)r[DM_REG_X1];
        size_t sz = (size_t)r[DM_REG_X2];
        if (dst && src && sz && sz <= (256u << 20)) {
            memcpy(src, dst, sz);
            dm_diag_mark("svc UnmapMemory %p -> %p +%#zx", (void *)dst, (void *)src, sz);
        }
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x2c: case 0x2d: {  /* MapPhysicalMemory / UnmapPhysicalMemory(addr=x0, size=x1)
                     第 64 轮加，照 D2R hle_svc.c 的 0x2C/0x2D：只许在 alias 区内，
                     0x2C 把这段 mprotect 成可读写、0x2D 收回成不可访问。
                     注意 x1 是**入参 size**、不是出参，别动它（D2R 也不动）。 */
        dm_alias_ensure();
        uint8_t *a = (uint8_t *)r[DM_REG_X0];
        size_t sz = (size_t)r[DM_REG_X1];
        if (!g_alias || !a || (uintptr_t)a < (uintptr_t)g_alias ||
            (uintptr_t)a + sz > (uintptr_t)g_alias + g_alias_cap) {
            dm_diag_mark("**svc %#x 越出 alias 区** %p+%#zx（区 %p+%#zx）",
                         n, (void *)a, sz, (void *)g_alias, g_alias_cap);
            r[DM_REG_X0] = 0xD401ull;      /* 无效地址（D2R 同款） */
            return 0;
        }
        int rc;
#if defined(_WIN32)
        DWORD old;
        rc = VirtualProtect(a, sz, (n == 0x2c) ? PAGE_READWRITE : PAGE_NOACCESS, &old) ? 0 : 1;
#else
        rc = mprotect(a, sz, (n == 0x2c) ? (PROT_READ | PROT_WRITE) : PROT_NONE);
#endif
        if (rc) {
            dm_diag_mark("**svc %#x mprotect 失败** %p+%#zx", n, (void *)a, sz);
            r[DM_REG_X0] = 0xD401ull;
            return 0;
        }
        dm_diag_mark("svc %sPhysicalMemory %p+%#zx",
                     n == 0x2c ? "Map" : "Unmap", (void *)a, sz);
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x29: {  /* GetInfo(id=x1, handle=x2, subid=x3) → x1 = 值（第 58 轮照 D2R 补全。
                     注意：之前这里被错标成 OutputDebugString，SDK 的 4 次 GetInfo
                     一直被垃圾 x1 糊弄 —— InfoType 表 = Atmosphère 标准编号）。 */
        extern void dm_guest_stack_range(void **base, size_t *size);   /* aarch64: hle_nvn_guest.c；Windows: hle_nvn_boot.c */
        uint64_t id = r[DM_REG_X1], v = 0;
        uint64_t hbase = 0, hcap = 0, hcur = 0;
        dm_plat_heap_peek(&hbase, &hcap, &hcur);
        void *sb = NULL; size_t ssz = 0;
        dm_guest_stack_range(&sb, &ssz);
        switch (id) {
        /* 第 66 轮：这两项 D2R 报真值、我们原来走 default 报 0 —— 客体拿它算堆大小，
           报 0 就算出负数（实证：请求 0xfffffffffe000000 = 0 − 0x2000000）。 */
        case 0:  v = 7; break;                                      /* CoreMask（D2R 同款） */
        case 1:  v = 0; break;                                      /* PriorityMask */
        /* 第 64 轮：别名区**必须**是独立的一段，不能拿堆池顶替 —— SDK 的 VammManager
           拿它挑地址后再用 svc 0x2C 打开那页；报成堆池会让 0x2C 的越界检查全落空，
           页没打开就回去写 → SIGSEGV（.ips 实证）。 */
        case 2:  dm_alias_ensure(); v = (uint64_t)g_alias; break;   /* AliasRegionAddress */
        case 3:  dm_alias_ensure(); v = g_alias_cap; break;         /* AliasRegionSize */
        case 4:  v = hbase; break;                       /* HeapRegionAddress */
        case 5:  v = hcap; break;                        /* HeapRegionSize */
        case 6:  v = hcap; break;                        /* TotalMemorySize */
        case 7:  v = hcur; break;                        /* UsedMemorySize */
        case 8:  v = 0; break;                           /* DebuggerAttached */
        case 11: v = dm_host_ticks(); break;             /* RandomEntropy */
        case 12: dm_aslr_ensure(); v = (uint64_t)g_as; break;   /* AslrRegionAddress */
        case 13: dm_aslr_ensure(); v = g_as_cap;   break;       /* AslrRegionSize */
        case 14: v = (uint64_t)sb; break;                /* StackRegionAddress */
        case 15: v = ssz; break;                         /* StackRegionSize */
        case 16: v = 0x2000000ull; break;                /* SystemResource 总量（D2R） */
        case 17: v = 0x400000ull; break;                 /* SystemResource 已用 */
        case 18: v = 0x01001BA027DD8000ull; break;       /* ProgramId（本游戏 TID） */
        case 21: v = hcap; break;                        /* TotalNonSystemMemorySize */
        case 22: v = hcur; break;                        /* UsedNonSystemMemorySize */
        case 23: v = 1; break;                           /* IsApplication */
        default: v = 0; break;
        }
        r[DM_REG_X0] = 0; r[DM_REG_X1] = v; return 0; }
    case 0x0c:  /* GetThreadPriority(句柄) —— x1 = 优先级 */
        r[DM_REG_X0] = 0; r[DM_REG_X1] = 0x2C;
        return 0;
    case 0x0d:  /* SetThreadPriority */
        r[DM_REG_X0] = 0;
        return 0;
    case 0x0e:  /* GetThreadCoreMask —— x1 = 理想核，x2 = 核掩码 */
        r[DM_REG_X0] = 0; r[DM_REG_X1] = 0; r[DM_REG_X2] = 7;
        return 0;
    case 0x0f:  /* SetThreadCoreMask：宿主只记账，如实回成功 */
        r[DM_REG_X0] = 0;
        return 0;
    case 0x16:  /* CloseHandle —— x0 = 结果码 */
        r[DM_REG_X0] = 0;
        return 0;
    case 0x0b:  /* SleepThread —— 纳秒在 x1，负值 = yield */
        return 0;
    case 0x1a: {  /* ArbitrateLock(w0 持锁者, x1 锁地址, w2 本线程标签) —— 第 55 轮加 */
        volatile uint32_t *w = (volatile uint32_t *)r[DM_REG_X1];
        uint32_t v = *w, owner = (uint32_t)r[DM_REG_X0];
        /* 标签字协议（D2R hle_thread.c）：持有者句柄 | 0x40000000(有等待者)。
           第 56 轮真机：*addr=0x6e808200 vs 期待 0x2e808200 —— 恰好只差等待位，
           所以「持有者|等待位」也要认。 */
        if (v == owner || v == (owner | 0x40000000u) || v == 0) {
            *w = (uint32_t)r[DM_REG_X2];   /* 内核语义：等到 *addr==owner 后写入自己的标签 */
            r[DM_REG_X0] = 0;
        } else {
            /* 单线程引导期 SDK 的用户态 CAS 成功就不会进这里；真争用说明有多线程了，
               那时必须换成 D2R 的完整等待层。拒答会触发 nn::diag::Abort（本次崩溃链），
               所以记录后放行。 */
            static unsigned n_log;
            if (n_log++ < 8)
                dm_diag_mark("[svc] ArbitrateLock 争用：*%llx=%x 期待=%x —— 放行（单线程下不应发生）",
                             (unsigned long long)r[DM_REG_X1], (unsigned)v,
                             (unsigned)r[DM_REG_X0]);
            *w = (uint32_t)r[DM_REG_X2];
            r[DM_REG_X0] = 0;
        }
        return 0; }
    case 0x1b:  /* ArbitrateUnlock(x0 锁地址)：还没有等待者追踪，直接清零
                   （= D2R lock_handoff 在「无等待者」时的行为） */
        *(volatile uint32_t *)r[DM_REG_X0] = 0;
        r[DM_REG_X0] = 0;
        return 0;
    default:
        /* 第 59 轮：未实现的调用 → 按「占位返回成功」处理（**不再回 x0=-1**）。
           理由（三次真机事故的教训）：r55 Abort 崩溃 / r57 停车挂起 / 都由「SDK 拿到
           非 0 结果码 → 走错误路径」触发。D2R hle_svc.c 的默认分支就是回成功
           （regs[0]=0）——这是真机验证过的策略。诊断层（dm_gate_dispatch）会把
           「未实现→假成功」的调用号全部点名，后续逐个补真实现。
           返回 2 = 「未实现（已按假成功处理）」，dispatch 据此写 x0=0 并记日志。 */
        return 2;
    }
}

/* ---------------- 分发 ---------------- */

void dm_gate_dispatch(dm_host *h, uint64_t ret, uint64_t *regs) {
    /* 记下这一笔（便宜的一次数组写）：卡死/崩溃时靠它看出最后几个门是哪些客体站点 */
    dm_diag_gate(ret);
    /* 第 59 轮：运行期槽核对 + 自愈（发现被改写就恢复并记日志）。真机日志里 K18 槽
       运行中变 0，但没抓到是谁写的 —— 先在这里设保险（每门 4 次槽读，L1 级开销）。
       Windows 自测是空桩（hle_plat.h）。 */
    dm_plat_slots_guard(ret);

    const dm_gate_ent *e = dm_host_find(h, ret);
    if (!e) {
        h->n_unknown++;
        return;                     /* 未登记：不动寄存器，保持客体原行为 */
    }
    switch (e->kind) {
    case DM_GATE_CNTPCT:
    case DM_GATE_CNTVCT:
        /* 目标寄存器可以是任意 xN（实测有 x19..x23），必须按 arg 写 */
        if (e->arg < DM_REG_COUNT) regs[e->arg] = dm_host_ticks();
        if (e->kind == DM_GATE_CNTPCT) h->n_cntpct++; else h->n_cntvct++;
        return;

    case DM_GATE_SVC: {
        h->n_svc++;
        /* 先问外部分发器；没有或它不认，再走内建实现。
           之前写成 `if (h->svc && h->svc(...) == 0)`，导致 h->svc 为空时
           连内建实现都不试，直接报未实现 —— 实测 GetSystemTick/CloseHandle 都因此失败。 */
        int rc = -1;
        if (h->svc) rc = h->svc(regs, e->arg);
        if (rc != 0) rc = host_svc_impl(regs, e->arg);
        if (rc == 3) {
            /* !! 第 66 轮：host_svc_impl 已经在 x0 写了**真正的错误码**，别再抹成 0 !!
             * 之前所有失败分支（SetHeapSize 超限等）都被下面那句
             * `if (x0_is_result_code) x0 = 0` 抹掉 —— 于是客体看到「成功」，
             * 而 x1 还是它自己传进来的入参（比如那个垃圾堆大小 0xfffffffffe000000），
             * 客体拿它当堆基址一写就段错误（.ips DmGame-2026-10-16-002722 实证：
             *   SetHeapSize 成功 x1=fffffffffe000000 → 故障地址 0xfffffffffe000010）。
             * D2R 的语义正是「超限要报 0x10801，游戏按 2MB 递减重试」。 */
            dm_diag_mark("[svc] %u (%s) **失败** x0=%#llx（保留错误码，不抹成成功）",
                         e->arg, svc_name(e->arg), (unsigned long long)regs[DM_REG_X0]);
        } else if (rc == 0) {
            /* 实现已写好内容。若该调用的 x0 是「结果码」，成功时置 0；
               若是「返回值」（如 GetSystemTick），保留实现写的数据。 */
            if (x0_is_result_code(e->arg)) regs[DM_REG_X0] = 0;
        } else {
            /* 未实现（rc==2）或异常（rc 其它值）：**照 D2R 占位返回成功** ——
               只写 x0=0，不动其它寄存器（理由见 host_svc_impl default 的长注释）。 */
            regs[DM_REG_X0] = 0;
            rc = 2;
        }
        /* 第 57 轮：svc 分发诊断 —— 真机日志直接点名「哪个 svc 被调、实现没、x0 出了什么」。
           第 59 轮改成「按调用号限频（每号最多 3 行）＋标注『未实现→假成功』」：
           既不刷爆日志，也能一眼列出本运行踩到的所有未实现调用（含名字与站点）。 */
        {
            static unsigned char seen[256];
            unsigned key = e->arg & 0xff;
            if (seen[key] < 3) {
                seen[key]++;
                char nm[40];
                const char *sn = svc_name(e->arg);
                const char *st = dm_diag_addr(ret, nm, (int)sizeof nm) ? nm : "?";
                if (rc == 2)
                    dm_diag_mark("[svc] %u %s (%s) **未实现→假成功**（x0=0；x1=%llx 原样）",
                                 e->arg, sn, st, (unsigned long long)regs[DM_REG_X1]);
                else if (x0_is_result_code(e->arg) && regs[DM_REG_X0] == 0)
                    dm_diag_mark("[svc] %u %s (%s) 成功 x1=%llx", e->arg, sn, st,
                                 (unsigned long long)regs[DM_REG_X1]);
                else
                    dm_diag_mark("[svc] %u %s (%s) rc=%d x0出=%llx", e->arg, sn, st,
                                 rc, (unsigned long long)regs[DM_REG_X0]);
            }
        }
        return;
    }

    default:
        return;
    }
}
