/* 宿主图形层 —— Metal 编码层：把回放器的 7 个后端操作翻成 Metal 调用。
 *
 * 【这是整个项目里唯一必须用 Mac 编译的部分】
 * 其余各层（接口/对象/录音/回放/着色器桥/翻译/glslc）都已在 Windows 上写完并单测过。
 *
 * 【设计：Apple 部分与平台无关部分严格分开】
 *   本文件（hle_nvn_mtl.c）：平台无关的**记账与决策** ——
 *      · 渲染通道的分组（哪些 draw 属于同一个 pass）
 *      · 状态变更的去重（同一个 setState 连续来两次只编码一次）
 *      · 绘制调用的统计与一致性检查（没有 program 就 draw → 记账并报告）
 *    这些是**真正的逻辑**，而且能在 Windows 上单测。
 *   hle_nvn_mtl_apply.m：真正调 Metal API 的部分（只有 Apple 上编译）。
 *
 * 【为什么这样分】
 *   真机侧最容易出的错不是「API 调错」而是「状态机搞错」（该 setState 的没 set、
 *   该开新 pass 的没开）。把状态机放在可测的一侧，能把调试时间砍掉一大半。
 */
#include "hle_nvn.h"
#include "hle_nvn_cmd.h"
#include "hle_nvn_replay.h"
#include "hle_nvn_metal.h"
#include "hle_nvn_mtl.h"

#include <stdio.h>
#include <string.h>

/* ---------------- 后端状态（平台无关） ---------------- */

typedef struct {
    /* 当前渲染通道 */
    int      in_pass;
    uint64_t pass_index;          /* 第几个 pass（诊断） */
    uint64_t pass_first_cmd;      /* 本 pass 第一条命令的序号 */
    uint64_t draws_in_pass;

    /* 当前状态（用于去重） */
    const char *last_state_name;
    uint64_t    last_state_a0, last_state_a1;
    int         have_program;
    int         have_viewport;
    int         have_target;      /* 是否已有渲染目标 */

    /* 统计 */
    uint64_t n_passes, n_state_coded, n_state_dedup, n_draws, n_draws_no_program;
    uint64_t n_draws_no_target, n_binds, n_clears, n_syncs, n_presents;
    uint64_t n_unknown;
} mtl_ctx;

static mtl_ctx g_m;

const void *dm_mtl_stats_ptr(void) { return &g_m; }

/* 窄接口给 hle_diag.c（诊断行用）—— 见 hle_nvn_mtl.h 的说明。
   注意这些都是**本帧**的计数：dm_mtl_frame_begin / dm_mtl_reset 会清零。 */
uint64_t dm_mtl_stat_passes(void)         { return g_m.n_passes; }
uint64_t dm_mtl_stat_draws(void)          { return g_m.n_draws; }
uint64_t dm_mtl_stat_presents(void)       { return g_m.n_presents; }
uint64_t dm_mtl_stat_draws_no_target(void){ return g_m.n_draws_no_target; }

void dm_mtl_reset(void) {
    memset(&g_m, 0, sizeof g_m);
    g_m.last_state_name = NULL;
}

/* 一帧的开始/结束（真机侧在这里建/提交 MTLCommandBuffer） */
void dm_mtl_frame_begin(void) {
    memset(&g_m, 0, sizeof g_m);
    g_m.last_state_name = NULL;
}

void dm_mtl_frame_end(void) {
    /* 通道没关就结束 —— 记账（真机侧应在这里显式 endEncoding） */
    if (g_m.in_pass) g_m.in_pass = 0;
}

/* ---------------- 7 个后端操作 ---------------- */

static void mtl_bind(void *ud, const char *name,
                     uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud; (void)a2; (void)a3;
    g_m.n_binds++;
    /* 绑定 program 是「有没有可画的东西」的判据 */
    if (strstr(name, "BindProgram")) g_m.have_program = 1;
    /* 绑定渲染目标是「pass 能不能开」的判据（真机侧按纹理/目标分类） */
    if (strstr(name, "BindTexture") || strstr(name, "BindImage")) {
        /* a0 常是命令缓冲，a1 是目标；不在这里猜太多 —— 只记「有过绑定」 */
        g_m.have_target = 1;
    }
    (void)a0; (void)a1;
}

static void mtl_state(void *ud, const char *name,
                      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud;
    /* 状态去重：连续两次同样的状态设置只编码一次。
       这在真机上是实打实的省（Metal 的 setState 有 CPU 开销）。 */
    if (g_m.last_state_name && !strcmp(g_m.last_state_name, name) &&
        g_m.last_state_a0 == a0 && g_m.last_state_a1 == a1) {
        g_m.n_state_dedup++;
        return;
    }
    g_m.n_state_coded++;
    g_m.last_state_name = name;      /* 注意：name 指向 NVN 名字表，生命周期是全程 */
    g_m.last_state_a0 = a0;
    g_m.last_state_a1 = a1;
    if (strstr(name, "SetViewport")) g_m.have_viewport = 1;
    (void)a2; (void)a3;
}

static void mtl_clear(void *ud, const char *name,
                      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud; (void)a0; (void)a1; (void)a2; (void)a3;
    g_m.n_clears++;
    /* 清除必须在 pass 里（Metal 的 loadAction=Clear）。
       真机侧若发现不在 pass 里，应在这里开一个。 */
    if (!g_m.in_pass) { g_m.in_pass = 1; g_m.n_passes++; g_m.draws_in_pass = 0; }
    (void)name;
    /* 清除改变了目标内容 —— 状态去重的缓存要作废（viewport 不一定变，
       但为了安全起见把 last_state 清掉，避免误去重） */
    g_m.last_state_name = NULL;
}

static void mtl_draw(void *ud, const char *name,
                     uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud; (void)a0; (void)a1; (void)a2; (void)a3;
    g_m.n_draws++;
    /* 一致性检查（真机上这些是「画面不对」的最常见原因，早发现早定位） */
    if (!g_m.have_program) g_m.n_draws_no_program++;
    if (!g_m.have_target)  g_m.n_draws_no_target++;
    if (!g_m.in_pass) {
        /* 没有 pass 就 draw → 开一个（Metal 要求先有 encoder） */
        g_m.in_pass = 1;
        g_m.n_passes++;
        g_m.draws_in_pass = 0;
        g_m.pass_first_cmd = g_m.n_draws;
    }
    g_m.draws_in_pass++;
    (void)name;
}

static void mtl_sync(void *ud, const char *name,
                     uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud; (void)a0; (void)a1; (void)a2; (void)a3;
    g_m.n_syncs++;
    /* 屏障会打断状态去重的连续性（GPU 顺序保证被切断）—— 作废缓存 */
    g_m.last_state_name = NULL;
    (void)name;
}

static void mtl_present(void *ud, const char *name,
                        uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud; (void)a0; (void)a1; (void)a2; (void)a3;
    g_m.n_presents++;
    /* 呈现 = 一帧结束。真机侧在这里 presentDrawable + commit。 */
    if (g_m.in_pass) { g_m.in_pass = 0; }
    (void)name;
}

static void mtl_other(void *ud, const char *name,
                      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud; (void)name; (void)a0; (void)a1; (void)a2; (void)a3;
    /* 不认识的命令：**不静默忽略**，真机侧应记进日志（这是发现「漏了什么」的入口） */
    g_m.n_unknown++;
}

static void mtl_unknown_idx(void *ud, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)ud; (void)a0; (void)a1; (void)a2; (void)a3;
    g_m.n_unknown++;
}

/* ---------------- 对外：一个后端对象 ---------------- */

const dm_nvn_backend *dm_mtl_backend(void) {
    static dm_nvn_backend be;
    static int inited = 0;
    if (!inited) {
        memset(&be, 0, sizeof be);
        be.bind    = mtl_bind;
        be.draw    = mtl_draw;
        be.clear   = mtl_clear;
        be.state   = mtl_state;
        be.sync    = mtl_sync;
        be.present = mtl_present;
        be.other   = mtl_other;
        be.unknown = mtl_unknown_idx;
        inited = 1;
    }
    return &be;
}
