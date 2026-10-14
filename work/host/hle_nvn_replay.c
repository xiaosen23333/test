/* 宿主图形层 —— 命令回放器。
 *
 * 分工：
 *   录音机（hle_nvn_cmd.c）：客体调 nvnCommandBuffer 的 Bind/Draw 系列时，把命令记进环形缓冲
 *   回放器（本文件）      ：提交时按序取出命令，翻成**后端操作**（下面那张函数表）
 *   后端（真机侧）        ：把后端操作翻成 Metal 调用（nvn_mtl.m 那一层）
 *
 * !! 写这个文件时踩的坑 !! 注释里出现 `Draw*` 紧跟 `/Bind` 会拼出 `*&#47;`，
 *   于是**块注释提前结束**，编译器把后面的中文当成代码 → 一堆 C2143/C2061。
 *   教训：注释里不要写「星号 + 斜杠」相邻的片段（通配符写法最容易撞上）。
 *
 * 为什么要有这一层（而不是直接录 Metal 调用）：
 *   Metal 的编码需要 MTLCommandBuffer/Encoder，而它们在**提交时**才存在；
 *   客体却可能在提交前任意时刻调 Bind/Draw。所以必须把「客体时刻」与
 *   「Metal 时刻」分开 —— 录音在客体时刻，编码在提交时刻。
 *   这一层就是两者的接缝，并且**它是纯逻辑，不需要 Mac 就能写与测**。
 *
 * 后端接口设计原则：
 *   · 只暴露「有语义」的操作（bind/draw/clear/viewport/barrier/present）
 *   · 后端不需要知道 NVN 的枚举 —— 回放器已经把 NVN 值翻成通用语义
 *   · **参数里的地址就是 GPU 地址**（= 客体内存的 CPU 地址，零拷贝），
 *     所以后端可以直接当指针用
 */
#include "hle_nvn.h"
#include "hle_nvn_cmd.h"
#include "hle_nvn_replay.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* 命令分类                                                          */
/* ---------------------------------------------------------------- */

/* 按名字前缀判定类别。用名字而不是 idx 表，是因为这里要能对
   「未注册的命令」也给出合理分类（诊断时有用）。 */
dm_cmd_kind dm_nvn_cmd_kind_of(const char *name) {
    if (!name) return DM_CK_OTHER;
    if (strstr(name, "Draw"))                             return DM_CK_DRAW;
    if (strstr(name, "Clear"))                            return DM_CK_CLEAR;
    if (strstr(name, "SetViewport") || strstr(name, "SetScissor") ||
        strstr(name, "SetDepthRange") || strstr(name, "SetBlend") ||
        strstr(name, "SetDepth") || strstr(name, "SetStencil") ||
        strstr(name, "SetCull") || strstr(name, "SetFrontFace") ||
        strstr(name, "SetPolygon") || strstr(name, "SetPointSize") ||
        strstr(name, "SetLineWidth"))                     return DM_CK_STATE;
    if (strstr(name, "Bind"))                             return DM_CK_BIND;
    if (strstr(name, "Barrier") || strstr(name, "Wait") ||
        strstr(name, "Signal") || strstr(name, "Fence"))   return DM_CK_SYNC;
    if (strstr(name, "Present"))                          return DM_CK_PRESENT;
    return DM_CK_OTHER;
}

/* ---------------------------------------------------------------- */
/* 回放                                                              */
/* ---------------------------------------------------------------- */

/* 索引表：idx → 名字与类别。由 dm_nvn_cmd_build_index 填。
   注意 idx 只在 0..NVN_NFUNC 之内；DM_CMD_IDX_UNKNOWN 单独处理。 */
typedef struct {
    int  idx;
    char name[64];
    int  kind;
} dm_replay_ent;

static dm_replay_ent g_idx_map[DM_REPLAY_MAX_CMDS];
static unsigned      g_idx_n = 0;

void dm_nvn_cmd_build_index(void) {
    g_idx_n = 0;
    const char *nm = NULL;
    for (unsigned i = 0; i < NVN_NFUNC && g_idx_n < DM_REPLAY_MAX_CMDS; i++) {
        nm = dm_nvn_name_of(i);          /* hle_nvn.c 提供 */
        if (!nm) continue;
        dm_replay_ent *e = &g_idx_map[g_idx_n++];
        e->idx = (int)i;
        snprintf(e->name, sizeof e->name, "%s", nm);
        e->kind = (int)dm_nvn_cmd_kind_of(nm);
    }
}

static const dm_replay_ent *idx_lookup(int idx) {
    for (unsigned i = 0; i < g_idx_n; i++)
        if (g_idx_map[i].idx == idx) return &g_idx_map[i];
    return NULL;
}

const char *dm_nvn_cmd_name_of_idx(int idx) {
    const dm_replay_ent *e = idx_lookup(idx);
    return e ? e->name : NULL;
}

unsigned dm_nvn_cmd_replay_to(const dm_nvn_backend *be, unsigned limit, dm_replay_stats *out) {
    dm_replay_stats st;
    memset(&st, 0, sizeof st);
    if (!be) { if (out) *out = st; return 0; }

    /* 取录音（hle_nvn_cmd.c 提供按序取出的接口） */
    dm_cmd_view v;
    if (!dm_nvn_cmd_begin_replay(&v)) { if (out) *out = st; return 0; }

    unsigned done = 0;
    /* !! 注意 limit == 0 表示「不限」—— 头文件里是这么约定的。
       上一版写成 `done < limit`，于是 limit=0 时 0<0 恒假，**一条都不回放**
       却「成功」返回 0 —— 静默不干活是最难查的一类 bug。 */
    unsigned cap = (limit == 0) ? v.n : limit;
    for (unsigned k = 0; k < v.n && done < cap; k++) {
        int idx = v.idx[k];
        const uint64_t *a = v.a[k];
        st.n_total++;

        if (idx == DM_CMD_IDX_UNKNOWN) {
            /* 身份未知（通用入口录的）—— 不猜，只计数并交给后端的 unknown 回调 */
            st.n_unknown++;
            if (be->unknown) be->unknown(be->ud, a[0], a[1], a[2], a[3]);
            done++;
            continue;
        }

        const dm_replay_ent *e = idx_lookup(idx);
        dm_cmd_kind kind = e ? (dm_cmd_kind)e->kind : DM_CK_OTHER;
        const char *nm = e ? e->name : "";

        switch (kind) {
        case DM_CK_BIND:
            st.n_bind++;
            if (be->bind) be->bind(be->ud, nm, a[0], a[1], a[2], a[3]);
            break;
        case DM_CK_DRAW:
            st.n_draw++;
            if (be->draw) be->draw(be->ud, nm, a[0], a[1], a[2], a[3]);
            break;
        case DM_CK_CLEAR:
            st.n_clear++;
            if (be->clear) be->clear(be->ud, nm, a[0], a[1], a[2], a[3]);
            break;
        case DM_CK_STATE:
            st.n_state++;
            if (be->state) be->state(be->ud, nm, a[0], a[1], a[2], a[3]);
            break;
        case DM_CK_SYNC:
            st.n_sync++;
            if (be->sync) be->sync(be->ud, nm, a[0], a[1], a[2], a[3]);
            break;
        case DM_CK_PRESENT:
            st.n_present++;
            if (be->present) be->present(be->ud, nm, a[0], a[1], a[2], a[3]);
            break;
        default:
            st.n_other++;
            if (be->other) be->other(be->ud, nm, a[0], a[1], a[2], a[3]);
            break;
        }
        done++;
    }
    dm_nvn_cmd_end_replay();
    if (out) *out = st;
    return done;
}
