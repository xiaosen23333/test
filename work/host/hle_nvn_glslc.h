/* 宿主图形层 —— glslc 接管（NVIDIA 离线 GLSL 着色器编译器）的声明。 */
#ifndef DM_HLE_NVN_GLSLC_H
#define DM_HLE_NVN_GLSLC_H

#include <stdint.h>
#include <stddef.h>

/* 从「引擎给的着色器数据」里定位控制段 → 算 key → 查表。
 * 成功返回 Metal 着色器路径；失败返回 NULL。 */
const char *dm_glslc_identify(const void *data, size_t size, int *out_stage_is_vs);

/* 最近一次成功识别的结果（诊断/自测用） */
const char *dm_glslc_last_key(void);
const char *dm_glslc_last_name(void);
const void *dm_glslc_stats_ptr(void);

/* glslc 的 8 个入口 */
void        dm_glslc_Initialize(void);
void        dm_glslc_Finalize(void);
const char *dm_glslc_GetVersion(void);
void        dm_glslc_SetAllocator(void *alloc, void *freefn);
void        dm_glslc_GetDefaultOptions(void *out);
void       *dm_glslc_Compile(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5);
void       *dm_glslc_CompilePreSpecialized(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5);
void       *dm_glslc_CompileSpecialized(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5);

/* 装载器的符号解析钩子 */
void *dm_glslc_lookup(const char *name);

#endif /* DM_HLE_NVN_GLSLC_H */
