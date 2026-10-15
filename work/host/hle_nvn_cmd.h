/* 宿主图形层 —— NVN 命令录音机 + 着色器查找桥的声明。 */
#ifndef DM_HLE_NVN_CMD_H
#define DM_HLE_NVN_CMD_H

#include <stdint.h>

/* ---- 着色器查找桥 ---- */
/* key = sha256(控制段[0:0x880])[:16] 的十六进制（32 字符，小写）。
   必须与离线翻译侧逐字节一致。 */
void dm_nvn_shader_key(const void *control, char out_hex17[17]);

/* 载入 shader_map.tsv 与 Metal 目录，返回条目数（<0 = 失败） */
int  dm_nvn_shader_load(const char *map_tsv, const char *metal_dir);
unsigned dm_nvn_shader_count(void);
const void *dm_nvn_shader_find(const char *key);
const char *dm_nvn_shader_metal_path(const void *ent);
const char *dm_nvn_shader_name(const void *ent);
const char *dm_nvn_shader_stage(const void *ent);
const void *dm_nvn_shader_stats_ptr(void);

/* nvnProgramInitialize：算 key → 查 Metal 着色器 */
const void *dm_nvn_ProgramInitialize(void *program, const void *dev,
                                     const void *code, const void *control);

/* ---- 命令录音机 ---- */
void     dm_nvn_cmd_enable(int on);
void     dm_nvn_cmd_record(int idx, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
void     dm_nvn_cmd_generic(void *a0, void *a1, void *a2, void *a3);
unsigned dm_nvn_cmd_pending(void);
unsigned dm_nvn_cmd_replay(void);
void     dm_nvn_cmd_note(int idx, int kind);
void     dm_nvn_cmd_register_all(void);
unsigned dm_nvn_cmd_registered(void);
const void *dm_nvn_cmd_stats_ptr(void);

/* 累计计数（诊断用；结构体本身是私有的，这几个是给 hle_diag.c 的窄接口）。
 * 意义：n_recorded 在涨 = 客体真的在调 NVN 的绘制/状态类函数；
 *       n_replayed 在涨 = 宿主真的在把它们翻成后端操作。 */
uint64_t dm_nvn_stat_recorded(void);
uint64_t dm_nvn_stat_replayed(void);
uint64_t dm_nvn_stat_dropped(void);    /* 环形缓冲满而丢掉的条数（>0 说明录不过来） */

#endif /* DM_HLE_NVN_CMD_H */
