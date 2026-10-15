/* 宿主 HLE 骨架 —— 门入口与门分发。
 *
 * 与 dm/host/entry.S 的 _dm_gate 配对：
 *   客体执行到门块 → 门块把「宿主门地址」取到 x30 并 blr → 进入 _dm_gate
 *   _dm_gate 保存 x0..x30 / q0..q31 / nzcv,fpsr,fpcr 后：
 *       x0 = 返回点（= 门块 ldp 的地址，门的唯一标识）
 *       x1 = 寄存器区指针（regs[0..30] = x0..x30）
 *   本函数据此查门表、实现语义、把结果写回 regs。
 *
 * 平台的差别（本文件刻意做成平台无关）：
 *   - macOS/iOS 用 entry.S 的 _dm_gate（汇编）
 *   - Windows 上没有 aarch64 执行环境，用 tests/host_selftest.c 直接喂 regs 验证分发表逻辑
 */
#ifndef DM_HOST_HLE_H
#define DM_HOST_HLE_H

#include <stdint.h>
#include <stddef.h>

/* regs[0..30] = x0..x30；regs[31] 保留（不用）。 */
#define DM_REG_COUNT 31
#define DM_REG_X0    0
#define DM_REG_X1    1
#define DM_REG_X2    2
#define DM_REG_X3    3
#define DM_REG_X4    4
#define DM_REG_X5    5
#define DM_REG_X8    8
#define DM_REG_X29   29
#define DM_REG_X30   30

/* 门的类别：与改写器 gates.tsv 的第 2 列一致 */
typedef enum {
    DM_GATE_NONE = 0,
    DM_GATE_SVC,        /* svc #n —— n 在 arg */
    DM_GATE_CNTPCT,     /* mrs xN, cntpct_el0 —— N 在 arg */
    DM_GATE_CNTVCT,     /* mrs xN, cntvct_el0 —— N 在 arg */
} dm_gate_kind;

typedef struct {
    uint64_t     ret;      /* 返回点（门块 ldp 的地址）—— 主键 */
    dm_gate_kind kind;
    uint32_t     arg;      /* svc 号 或 目标寄存器号 */
    uint64_t     orig;     /* 原指令地址（诊断用） */
    const char  *name;     /* 可选：符号名（诊断用） */
} dm_gate_ent;

/* 宿主返回值：0 = 已处理；非 0 = 未实现（宿主会记录并给出安全缺省） */
typedef int (*dm_svc_impl)(uint64_t *regs, uint32_t svc_no);

typedef struct {
    const dm_gate_ent *ents;
    size_t             n;
    dm_svc_impl        svc;          /* svc 实现，可为空 */
    /* 诊断计数 */
    uint64_t           n_svc, n_cntpct, n_cntvct, n_unknown;
} dm_host;

/* 单调时钟（客体 cntpct/cntvct 的作答来源）。平台侧实现。 */
uint64_t dm_host_ticks(void);        /* 频率见 DM_HOST_TICK_HZ */
#define DM_HOST_TICK_HZ 19200000ull  /* Horizon 的 cntpct/cntvct 为 19.2 MHz */

/* 从 gates.tsv 装载门表（返回 0 成功）。table 由调用方提供存储。 */
int dm_host_load_gates(dm_host *h, dm_gate_ent *table, size_t cap, const char *path);

/* 按返回点二分查找。返回 NULL 表示该返回点未登记。 */
const dm_gate_ent *dm_host_find(const dm_host *h, uint64_t ret);

/* 客体地址空间区域查询（hle_nvn_boot.c 实现，svc 6 QueryMemory 用；第 57 轮加）。
 * 返回 1 = 命中（base/size/state/perm 填好），0 = 未知区域。 */
int dm_boot_query_region(uint64_t addr, uint64_t *base, uint64_t *size,
                         uint32_t *state, uint32_t *perm);

/* 门分发入口 —— _dm_gate 调用它（x0=ret, x1=regs）。 */
void dm_gate_dispatch(dm_host *h, uint64_t ret, uint64_t *regs);

#endif /* DM_HOST_HLE_H */
