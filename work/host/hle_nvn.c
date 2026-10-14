/* 宿主图形层 —— NVN 接口层骨架。
 *
 * 机制（与 dm/gfx/nvn/hle_nvn.c 一致）：
 *   1. 客体（NmplDLL.nro）调用 nvnBootstrapLoader("nvn") 取 NVN 函数表；
 *   2. 宿主提供自己的 nvnBootstrapLoader，返回**宿主自己的函数表**；
 *   3. 之后客体的每次 nvnXxx 调用都进宿主实现 —— **客体代码一行不改**。
 *
 * 为什么这是最干净的接管点：
 *   - 客体不直接碰驱动（NmplDLL 里 /dev/、nvhost、nvdrv、ioctl 全是 0 处）；
 *   - NVN 的全部入口都经由 nvnBootstrapLoader 返回的表；
 *   - 所以「换一张表」就完成了整个图形层的接管。
 *
 * 本文件提供：
 *   - 名字表（538 个基名 + 534 个 E 变体，从 NmplDLL.nro 真实提取）
 *   - 槽表与分发（照 D2R 的 nvn_slot_t 布局：16 字节步长，跳板按 i*16 寻址）
 *   - 计数与「被调到即报名字」的未实现入口（诊断用）
 *   - Metal 后端的接入点（nvn_mtl_* 由后续实现填充）
 *
 * 复用情况（实测）：本游戏 538 个基名里 **521 个与 D2R 的 523 个重合（96.8%）**，
 * 所以 D2R 的 nvn_mtl.m（626 KB）是**可直接参考/移植**的主体。
 */
#include "hle_nvn.h"
#include "nvn_names.h"

#include <stdio.h>
#include <string.h>

/* ---------------- 槽表 ----------------
 * 布局必须与跳板一致：跳板按 i*16 访问槽（与 D2R 的 nvn_slot_t 相同）。
 * 类型在 hle_nvn.h 里声明；**定义在这里**（头里只放 extern 声明）。 */
nvn_slot_t nvn_slot[NVN_NFUNC];
void *nvn_real[NVN_NFUNC];        /* 计时包装时用（可选） */

static uint8_t g_unimpl_seen[NVN_NFUNC];
static struct {
    uint64_t n_calls, n_unimpl_calls, n_distinct_called, n_distinct_unimpl;
} g_nvn_stats;

const void *dm_nvn_stats_ptr(void) { return &g_nvn_stats; }

/* ---------------- 名字 → 下标 ---------------- */

/* 按 idx 取名字（回放器要按 idx 还原命令身份） */
const char *dm_nvn_name_of(int idx) {
    if (idx < 0 || idx >= (int)NVN_NFUNC) return NULL;
    return nvn_names[idx];
}

int dm_nvn_index_of(const char *name) {
    if (!name) return -1;
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return (int)i;
    return -1;
}

/* ---------------- 未实现入口 ----------------
 * 被调到即记录名字并计数。宿主**不 abort** —— 图形调用失败的最坏结果是
 * 这一帧画错，而不是整个进程死掉。
 *
 * 与 D2R 的 nvn_unimpl_entry 同语义：报告 + 计数，让诊断能列出「哪些 NVN 还没做」。 */
uint64_t dm_nvn_unimpl(int idx, uint64_t x0, uint64_t x1) {
    (void)x0; (void)x1;
    g_nvn_stats.n_unimpl_calls++;
    if (idx >= 0 && idx < (int)NVN_NFUNC) {
        nvn_slot[idx].cnt++;
        if (!g_unimpl_seen[idx]) {
            g_unimpl_seen[idx] = 1;
            g_nvn_stats.n_distinct_unimpl++;
            /* 真机侧这里写日志；Windows 自测里也能看到 */
            fprintf(stderr, "[nvn] 未实现: %s\n", nvn_names[idx]);
        }
    }
    return 0;      /* NVN 的多数函数返回 void；返回值调用方一般不检查 */
}

/* ---------------- 分发 ----------------
 * 客体拿到的是「槽的地址」，跳板从槽里取 fn 并调用。
 * Windows 侧没有 aarch64 跳板，所以这里提供**直接调用的等价入口**，
 * 供自测验证「按名字找实现 → 调用 → 计数」这条链。 */
uint64_t dm_nvn_dispatch(const char *name, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4) {
    int idx = dm_nvn_index_of(name);
    g_nvn_stats.n_calls++;
    if (idx < 0) return 0;
    nvn_slot[idx].cnt++;
    if (nvn_slot[idx].cnt == 1) g_nvn_stats.n_distinct_called++;
    if (!nvn_slot[idx].fn) return dm_nvn_unimpl(idx, a0, a1);
    /* 有实现：按各自原型调用（下面的函数指针类型只是示意，具体实现由 nvn_mtl 提供） */
    typedef uint64_t (*fn5)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    return ((fn5)nvn_slot[idx].fn)(a0, a1, a2, a3, a4);
}

/* ---------------- 注册实现 ----------------
 * 返回 1 = 注册成功；0 = 名字不在表里（**静默忽略是刻意的**，但调用方应据此计数，
 * 而不是假定成功 —— 我在命令类注册里踩过这个坑）。 */
int dm_nvn_register(const char *name, void *impl) {
    int idx = dm_nvn_index_of(name);
    if (idx < 0) return 0;
    nvn_slot[idx].fn = impl;
    return 1;
}

unsigned dm_nvn_registered(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) if (nvn_slot[i].fn) n++;
    return n;
}

/* ---------------- nvnBootstrapLoader ----------------
 * 客体的入口：返回 NVN 函数表。
 * NVN 的实际约定是 nvnBootstrapLoader(const char *name) 返回一个函数指针，
 * 该函数再按需给出具体实现（nvnDeviceGetProcAddress）。
 *
 * 宿主的最小可用实现：返回一个「按名字解析」的通用入口。
 * 真机侧应返回跳板表首地址（与 entry.S 的 nvn_thunk 配合）。 */
void *dm_nvn_bootstrap(const char *name) {
    if (!name) return NULL;
    /* 客体先要 "nvn"（库名） */
    if (!strcmp(name, "nvn")) return (void *)dm_nvn_dispatch;
    /* 有些引擎会直接按函数名要 */
    int idx = dm_nvn_index_of(name);
    if (idx >= 0) return nvn_slot[idx].fn ? nvn_slot[idx].fn : (void *)dm_nvn_unimpl;
    return NULL;
}

/* ---------------- 诊断：列出未实现清单 ---------------- */
void dm_nvn_dump_unimpl(void (*emit)(const char *name, void *ud), void *ud) {
    if (!emit) return;
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (g_unimpl_seen[i] && !nvn_slot[i].fn)
            emit(nvn_names[i], ud);
}
