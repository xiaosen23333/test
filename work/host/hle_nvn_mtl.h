/* 宿主图形层 —— Metal 编码层（平台无关部分）的接口。
 *
 * 7 个后端操作 → Metal 的映射：
 *   bind    → [encoder setRenderPipelineState:] / setVertexBuffer: / setFragmentTexture: …
 *   state   → setViewport: / setScissorRect: / setDepthStencilState: / setCullMode: …（带去重）
 *   clear   → 通道的 loadAction = Clear
 *   draw    → drawPrimitives: / drawIndexedPrimitives:（图元拓扑见 hle_nvn_metal.c 的映射）
 *   sync    → memoryBarrierWithScope: / encodeSignalEvent:value:
 *   present → presentDrawable: + commit
 *   other   → 记录（**不静默忽略**）
 */
#ifndef DM_HLE_NVN_MTL_H
#define DM_HLE_NVN_MTL_H

#include "hle_nvn_replay.h"

/* 取后端对象（回放器用它）。真机侧把它交给 dm_nvn_cmd_replay_to。 */
const dm_nvn_backend *dm_mtl_backend(void);

/* 一帧的边界（真机侧在这里建/提交 MTLCommandBuffer） */
void dm_mtl_frame_begin(void);
void dm_mtl_frame_end(void);
void dm_mtl_reset(void);

/* 统计（画不出东西时靠它定位） */
const void *dm_mtl_stats_ptr(void);

/* ---- 以下只有 Apple 上编译（hle_nvn_mtl_apply.m 提供） ---- */
/* 真正调 Metal API 的入口。Windows 上不存在这些符号，调用方须自行按平台隔开。 */
#if defined(__APPLE__)
int  dm_mtl_apply_init(void *mtl_device, void *mtl_layer);
void dm_mtl_apply_pass_begin(void);
void dm_mtl_apply_pass_end(void);
#endif

#endif /* DM_HLE_NVN_MTL_H */
