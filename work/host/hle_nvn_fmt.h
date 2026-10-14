/* NVNformat -> MTLPixelFormat 映射（平台无关：只返回枚举整数值）。 */
#ifndef DM_HLE_NVN_FMT_H
#define DM_HLE_NVN_FMT_H

typedef struct { unsigned short mtl; unsigned char kind, noalpha, rb; } dm_fmt_ent;

/* 返回 NULL 表示该 NVN 格式在 Metal 里没有对应 */
const dm_fmt_ent *dm_nvn_fmt_lookup(int nvn_format);
unsigned dm_nvn_fmt_count(void);

#endif
