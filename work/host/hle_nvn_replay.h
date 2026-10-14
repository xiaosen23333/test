/* 宿主图形层 —— 命令回放器的接口。
 *
 * 三层分工：
 *   录音机（hle_nvn_cmd.c）  客体时刻：把 nvnCommandBuffer* 调用记进环形缓冲
 *   回放器（hle_nvn_replay.c）提交时刻：按序取出，翻成**后端操作**
 *   后端（真机侧 nvn_mtl）   把后端操作翻成 Metal 调用
 *
 * 「后端操作」只有 7 种，且参数语义与 NVN 无关（只带通用值）——
 * 这样回放器是**纯逻辑**，能在 Windows 上完整测；真机侧只需实现这张表。
 */
#ifndef DM_HLE_NVN_REPLAY_H
#define DM_HLE_NVN_REPLAY_H

#include <stdint.h>

/* 「身份未知」的 idx（通用入口录的命令 —— 拿不到 idx；不假装知道） */
#define DM_CMD_IDX_UNKNOWN (-1)

#define DM_REPLAY_MAX_CMDS 8192

typedef enum {
    DM_CK_OTHER = 0, DM_CK_BIND, DM_CK_DRAW, DM_CK_CLEAR,
    DM_CK_STATE, DM_CK_SYNC, DM_CK_PRESENT
} dm_cmd_kind;

/* 录音视图：回放期间由 dm_nvn_cmd_begin_replay 填好。
 * 三个平行数组（idx / 4 个参数 / 顺序）—— 避免为每次回放分配内存。 */
typedef struct {
    unsigned  n;
    const int      *idx;
    const uint64_t (*a)[4];
} dm_cmd_view;

/* ---- 后端接口：真机侧实现这张表 ---- */
typedef struct dm_nvn_backend {
    void *ud;      /* 后端自己的上下文（真机侧放 MTLCommandBuffer 等） */

    void (*bind)   (void *ud, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*draw)   (void *ud, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*clear)  (void *ud, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*state)  (void *ud, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*sync)   (void *ud, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*present)(void *ud, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*other)  (void *ud, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
    /* 身份未知的命令（通用入口录的）。后端**不应猜**它是什么，只应记录/忽略。 */
    void (*unknown)(void *ud, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
} dm_nvn_backend;

typedef struct {
    uint64_t n_total, n_bind, n_draw, n_clear, n_state, n_sync, n_present, n_other, n_unknown;
} dm_replay_stats;

/* ---- 接口 ---- */

/* 建 idx → 名字/类别 的索引（在 dm_nvn_cmd_register_all 之后调一次） */
void dm_nvn_cmd_build_index(void);
const char *dm_nvn_cmd_name_of_idx(int idx);
dm_cmd_kind dm_nvn_cmd_kind_of(const char *name);

/* 回放（最多 limit 条；limit = 0 表示不限）。
 * 返回实际回放的条数。out 可空。 */
unsigned dm_nvn_cmd_replay_to(const dm_nvn_backend *be, unsigned limit, dm_replay_stats *out);

/* 由 hle_nvn_cmd.c 提供 */
int  dm_nvn_cmd_begin_replay(dm_cmd_view *v);
void dm_nvn_cmd_end_replay(void);
const char *dm_nvn_name_of(int idx);   /* 由 hle_nvn.c 提供 */

#endif /* DM_HLE_NVN_REPLAY_H */
