/* 宿主图形层 —— NVN → Metal 枚举映射（翻译层）。
 *
 * 本文件不依赖任何 Apple 头文件 —— 返回的是 **Metal 枚举的整数值**，
 * 真机侧再转成实际的 MTL* 常量。这样翻译层可以在 Windows 上完整编译与单测。
 */
#ifndef DM_HLE_NVN_METAL_H
#define DM_HLE_NVN_METAL_H

#include <stdint.h>      /* uint64_t —— 少了它 MSVC 报 C2061「语法错误: 标识符 uint64_t」 */

/* 统一偏移的五组（Metal = NVN - 1，返回 -1 表示越界） */
int dm_nvn_to_metal_compare(int nvn);
int dm_nvn_to_metal_stencil_op(int nvn);
int dm_nvn_to_metal_blend_equation(int nvn);
int dm_nvn_to_metal_mag_filter(int nvn);     /* 偏移 0，无需转换 */

/* 例外：NVN_BLEND_FUNC 的 19 个取值（显式映射） */
int dm_nvn_to_metal_blend_factor(int nvn);

/* 例外：NVN_MIN_FILTER 拆成 (min, mip) 两个属性 */
int dm_nvn_to_metal_min_filter(int nvn, int *out_mip);

/* NVN_WRAP → MTLSamplerAddressMode */
int dm_nvn_to_metal_wrap(int nvn);

/* 正面绕序（偏移 0） */
int dm_nvn_to_metal_front_face(int nvn);

/* 带使用统计的包装（诊断：看游戏实际用了哪些取值） */
int dm_nvn_metal_compare_tracked(int nvn);
int dm_nvn_metal_blend_tracked(int nvn);
int dm_nvn_metal_wrap_tracked(int nvn);
int dm_nvn_metal_stencil_tracked(int nvn);
int dm_nvn_metal_eq_tracked(int nvn);
int dm_nvn_metal_minf_tracked(int nvn, int *mip);
const void *dm_nvn_enum_usage_ptr(void);

#endif /* DM_HLE_NVN_METAL_H */
