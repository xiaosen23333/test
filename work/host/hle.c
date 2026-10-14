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
        [0x27]="Break", [0x29]="OutputDebugString", [0x2a]="GetInfo", [0x2b]="ConnectToPort",
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
 * Horizon 的惯例：多数调用 x0 = 结果码（0 = 成功），返回值在 x1；
 * 但有一部分调用的 x0 **本身就是返回值**（没有结果码）。
 * 分错了会导致客体把有效数据当成错误码，或把错误码当成句柄。
 * 下表按 Horizon 的 svc 手册逐条登记 —— 这是必须查准的地方，不能想当然。
 */
static int x0_is_result_code(uint32_t n) {
    switch (n) {
    /* x0 = 返回值（无结果码） */
    case 0x10:  /* GetCurrentProcessorNumber */
    case 0x16:  /* CloseHandle */
    case 0x0c:  /* GetThreadPriority */
    case 0x1e:  /* GetSystemTick */
    case 0x25:  /* GetProcessId */
    case 0x26:  /* GetThreadId */
    case 0x49:  /* QueryIoMapping（x0..x2 都是输出） */
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
    case 0x25:  /* GetProcessId —— x0 就是 pid */
        r[DM_REG_X0] = 1;
        return 0;
    case 0x26:  /* GetThreadId —— x0 就是 tid */
        r[DM_REG_X0] = 1;
        return 0;
    case 0x16:  /* CloseHandle —— x0 = 0 表示成功关闭 */
        r[DM_REG_X0] = 0;
        return 0;
    case 0x0c:  /* GetThreadPriority —— x0 = 优先级 */
        r[DM_REG_X0] = 0x2C;    /* Horizon 默认普通优先级 */
        return 0;
    case 0x29:  /* OutputDebugString */
        return 0;
    case 0x0b:  /* SleepThread —— 纳秒在 x1，负值 = yield */
        return 0;
    default:
        return -1;
    }
}

/* ---------------- 分发 ---------------- */

void dm_gate_dispatch(dm_host *h, uint64_t ret, uint64_t *regs) {
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
            return;
        }
        /* 未实现的调用：给一个「未实现」的结果码（x0 非 0），
           避免客体把垃圾当成有效句柄继续用。诊断里计数。 */
        regs[DM_REG_X0] = (uint64_t)-1;
        return;
    }

    default:
        return;
    }
}
