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
        [0x21]="SendSyncRequest", [0x22]="SendSyncRequestWithUserBuffer",
        [0x23]="SendAsyncRequestWithUserBuffer", [0x25]="GetProcessId", [0x26]="GetThreadId",
        [0x27]="Break", [0x28]="OutputDebugString", [0x29]="GetInfo", [0x2b]="ConnectToPort",
        [0x2c]="SetProcessMemoryPermission", [0x2d]="MapProcessMemory",
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

/* svc 实现：返回 0 = 已处理（regs 已写好内容）；非 0 = 未实现 */
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
        if (hrc == 0) { r[DM_REG_X0] = 0; r[DM_REG_X1] = base; }
        else if (hrc == 1) { r[DM_REG_X0] = 0x10801ull; }   /* ResultLimitReached */
        else { r[DM_REG_X0] = (uint64_t)-1; }
        return 0; }
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
                     （sdk+0x15900c，连上后只查 svc 返回值，不解析应答缓冲）。 */
        const char *name = (const char *)r[DM_REG_X1];
        static int n_sm;
        if (name && name[0] == 's' && name[1] == 'm' && name[2] == ':' && name[3] == 0) {
            if (n_sm++ < 3) dm_diag_mark("[sm:] 连接 \"sm:\" → 成功（假会话，IPC 只回成功）");
            r[DM_REG_X0] = 0;
            r[DM_REG_X1] = 0xC0000001ull + (unsigned)n_sm;
        } else {
            char nb[12]; int k = 0;
            for (; name && name[k] && k < 8; k++) nb[k] = name[k];
            nb[k] = 0;
            dm_diag_mark("[端口] ConnectToNamedPort(\"%s\") → 不存在", nb);
            r[DM_REG_X0] = 0xE701ull;      /* NoSuchPort */
        }
        return 0; }
    case 0x20: case 0x21: {  /* SendSyncRequest [WithUserBuffer]（第 58 轮）。
                               回成功；把消息头记进诊断 —— CMIF 请求 magic 'SFCI'
                               后面跟命令号，下轮需要应答时照这个补。 */
        uint64_t buf = (n == 0x21) ? r[DM_REG_X0] : 0;
        uint64_t sess = (n == 0x21) ? r[DM_REG_X2] : r[DM_REG_X0];
        static unsigned n_ipc;
        if (n_ipc++ < 24) {
            unsigned w0 = 0, w1 = 0, w2 = 0, w3 = 0;
            if (buf) {
                w0 = *(volatile unsigned *)buf;
                w1 = *((volatile unsigned *)buf + 1);
                w2 = *((volatile unsigned *)buf + 2);
                w3 = *((volatile unsigned *)buf + 3);
            }
            dm_diag_mark("[ipc] 会话=%llx buf=%llx 头=%08x %08x %08x %08x",
                         (unsigned long long)sess, (unsigned long long)buf, w0, w1, w2, w3);
        }
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x27: {  /* Break(reason, ptr, size)：SDK 致命中止意图 —— 只记不放行崩溃
                     （真致命时 SDK 自己会走 diag Abort，那里有完整停车+门历史） */
        static int n_br;
        if (n_br++ < 5) dm_diag_mark("[Break] reason=%llx ptr=%llx",
                                     (unsigned long long)r[DM_REG_X0],
                                     (unsigned long long)r[DM_REG_X1]);
        r[DM_REG_X0] = 0;
        return 0; }
    case 0x28:  /* OutputDebugString(str, len) */
        r[DM_REG_X0] = 0;
        return 0;
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
        case 2:  v = hbase; break;                       /* AliasRegionAddress：暂用堆池 */
        case 3:  v = hcap; break;                        /* AliasRegionSize */
        case 4:  v = hbase; break;                       /* HeapRegionAddress */
        case 5:  v = hcap; break;                        /* HeapRegionSize */
        case 6:  v = hcap; break;                        /* TotalMemorySize */
        case 7:  v = hcur; break;                        /* UsedMemorySize */
        case 8:  v = 0; break;                           /* DebuggerAttached */
        case 11: v = dm_host_ticks(); break;             /* RandomEntropy */
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
    case 0x25:  /* GetProcessId(句柄) —— x1 = 进程号 */
        r[DM_REG_X0] = 0; r[DM_REG_X1] = 1;
        return 0;
    case 0x26:  /* GetThreadId(句柄) —— x1 = 线程号 */
        r[DM_REG_X0] = 0; r[DM_REG_X1] = 1;
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
        return -1;
    }
}

/* ---------------- 分发 ---------------- */

void dm_gate_dispatch(dm_host *h, uint64_t ret, uint64_t *regs) {
    /* 记下这一笔（便宜的一次数组写）：卡死/崩溃时靠它看出最后几个门是哪些客体站点 */
    dm_diag_gate(ret);

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
        if (rc == 0) {
            /* 实现已写好内容。若该调用的 x0 是「结果码」，成功时置 0；
               若是「返回值」（如 GetSystemTick），保留实现写的数据。 */
            if (x0_is_result_code(e->arg)) regs[DM_REG_X0] = 0;
        } else {
            /* 未实现的调用：给一个「未实现」的结果码（x0 非 0），
               避免客体把垃圾当成有效句柄继续用。诊断里计数。 */
            regs[DM_REG_X0] = (uint64_t)-1;
        }
        /* 第 57 轮：svc 分发诊断（限频）—— 真机日志直接点名
           「哪个 svc 被调、实现没、x0 出了什么」。
           起因：r56 卡死在 ThreadManagerImplByHorizon 构造，真凶是
           svc 6 QueryMemory 未实现 —— 之前只能靠反汇编猜。 */
        {
            static unsigned n_svc_diag;
            if (n_svc_diag < 60) {
                n_svc_diag++;
                char nm[40];
                if (rc == 0 && x0_is_result_code(e->arg) && regs[DM_REG_X0] == 0)
                    dm_diag_mark("[svc] %u (%s) 成功 x1=%llx", e->arg,
                                 dm_diag_addr(ret, nm, (int)sizeof nm) ? nm : "?",
                                 (unsigned long long)regs[DM_REG_X1]);
                else
                    dm_diag_mark("[svc] %u (%s) rc=%d x0出=%llx", e->arg,
                                 dm_diag_addr(ret, nm, (int)sizeof nm) ? nm : "?",
                                 rc, (unsigned long long)regs[DM_REG_X0]);
            }
        }
        return;
    }

    default:
        return;
    }
}
