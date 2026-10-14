/* 宿主图形层 —— NVN → Metal 枚举映射。
 *
 * 本文件是「翻译层」里逻辑最密、最容易错、**且不需要 Mac 就能写与验证**的部分。
 *
 * 实测发现（work/porting/verify_nvn_enums.py 的输出）：
 *   **五组枚举与 Metal 的偏移恰好都是 +1**（Metal = NVN - 1）：
 *     NVN_COMPARE_FUNC  / NVN_DEPTH_FUNC / NVN_STENCIL_OP / NVN_BLEND_EQUATION / NVN_MAG_FILTER
 *   所以这几组用一条公式即可 —— 不需要几百行硬编码表，也就不会漏项。
 *
 *   两类例外必须显式映射：
 *     · NVN_BLEND_FUNC：NVN 把 src/dst 合在一起编号，而 Metal 用同一套 MTLBlendFactor
 *       给 src 与 dst 两个槽，所以**一个 NVN 值要拆成 (src_factor, dst_factor)**
 *     · NVN_MIN_FILTER：NVN 把 min 与 mip 合成一个值，而 Metal 分成
 *       MTLSamplerMinMagFilter 与 MTLSamplerMipFilter 两个属性
 *
 * 元数据（NVN 枚举值）来自 D2R 的 dm/gfx/nvn/include/nvn.h（从 nnheaders 抄的官方枚举）。
 */
#include "hle_nvn_metal.h"

/* ==================================================================== */
/* 统一偏移的五组：Metal = NVN - 1                                       */
/* ==================================================================== */

/* 返回对应关系是否成立（值域内）。MV_* 是 Metal 侧的枚举值。 */
int dm_nvn_to_metal_compare(int nvn) {
    /* NVN: NEVER=1 LESS=2 EQUAL=3 LEQUAL=4 GREATER=5 NOTEQUAL=6 GEQUAL=7 ALWAYS=8
       MTL: Never=0 Less=1 Equal=2 LessEqual=3 Greater=4 NotEqual=5 GreaterEqual=6 Always=7 */
    if (nvn < 1 || nvn > 8) return -1;
    return nvn - 1;
}
int dm_nvn_to_metal_stencil_op(int nvn) {
    /* NVN: KEEP=1 ZERO=2 REPLACE=3 INCR=4 DECR=5 INVERT=6 INCR_WRAP=7 DECR_WRAP=8
       MTL: Keep=0 Zero=1 Replace=2 IncrementClamp=3 DecrementClamp=4 Invert=5
            IncrementWrap=6 DecrementWrap=7 */
    if (nvn < 1 || nvn > 8) return -1;
    return nvn - 1;
}
int dm_nvn_to_metal_blend_equation(int nvn) {
    /* NVN: ADD=1 SUB=2 REVERSE_SUB=3 MIN=4 MAX=5
       MTL: Add=0 Subtract=1 ReverseSubtract=2 Min=3 Max=4 */
    if (nvn < 1 || nvn > 5) return -1;
    return nvn - 1;
}
int dm_nvn_to_metal_mag_filter(int nvn) {
    /* NVN: NEAREST=0 LINEAR=1
       MTL: Nearest=0 Linear=1  —— **偏移 0，无需转换** */
    if (nvn < 0 || nvn > 1) return -1;
    return nvn;
}

/* ==================================================================== */
/* 例外一：NVN_BLEND_FUNC —— 一个 NVN 值拆成 (src, dst) 两个 Metal 因子   */
/* ==================================================================== */

/* Metal MTLBlendFactor 值（按官方声明顺序）
     0 Zero                1 One                  2 SourceColor
     3 OneMinusSourceColor 4 SourceAlpha          5 OneMinusSourceAlpha
     6 DestinationColor    7 OneMinusDestinationColor
     8 DestinationAlpha    9 OneMinusDestinationAlpha
    10 SourceAlphaSaturated 11 BlendColor         12 OneMinusBlendColor
    13 BlendAlpha          14 OneMinusBlendAlpha
    15 Source1Color        16 OneMinusSource1Color
    17 Source1Alpha        18 OneMinusSource1Alpha */

/* NVN 混合因子编号（实测自 nvn.h）：
     ZERO=1 ONE=2 SRC_COLOR=3 ONE_MINUS_SRC_COLOR=4 SRC_ALPHA=5 ONE_MINUS_SRC_ALPHA=6
     DST_ALPHA=7 ONE_MINUS_DST_ALPHA=8 DST_COLOR=9 ONE_MINUS_DST_COLOR=10
     SRC_ALPHA_SATURATE=11 SRC1_COLOR=12 ONE_MINUS_SRC1_COLOR=13
     SRC1_ALPHA=14 ONE_MINUS_SRC1_ALPHA=15 CONSTANT_COLOR=16
     ONE_MINUS_CONSTANT_COLOR=17 CONSTANT_ALPHA=18 ONE_MINUS_CONSTANT_ALPHA=19
   注意：上面这是**编号**，不是「src/dst 组合」。
   本游戏的引擎通过 `nvnBlendStateSetBlendFunc(bs, src, dst)` 分别传两个，
   所以这里其实是**逐个因子**映射 —— 不是拆组合。 */
int dm_nvn_to_metal_blend_factor(int nvn) {
    switch (nvn) {
    case 1:  return 0;    /* ZERO                    -> Zero */
    case 2:  return 1;    /* ONE                     -> One */
    case 3:  return 2;    /* SRC_COLOR               -> SourceColor */
    case 4:  return 3;    /* ONE_MINUS_SRC_COLOR     -> OneMinusSourceColor */
    case 5:  return 4;    /* SRC_ALPHA               -> SourceAlpha */
    case 6:  return 5;    /* ONE_MINUS_SRC_ALPHA     -> OneMinusSourceAlpha */
    case 9:  return 6;    /* DST_COLOR               -> DestinationColor */
    case 10: return 7;    /* ONE_MINUS_DST_COLOR     -> OneMinusDestinationColor */
    case 7:  return 8;    /* DST_ALPHA               -> DestinationAlpha */
    case 8:  return 9;    /* ONE_MINUS_DST_ALPHA     -> OneMinusDestinationAlpha */
    case 11: return 10;   /* SRC_ALPHA_SATURATE      -> SourceAlphaSaturated */
    case 16: return 11;   /* CONSTANT_COLOR          -> BlendColor */
    case 17: return 12;   /* ONE_MINUS_CONSTANT_COLOR-> OneMinusBlendColor */
    case 18: return 13;   /* CONSTANT_ALPHA          -> BlendAlpha */
    case 19: return 14;   /* ONE_MINUS_CONSTANT_ALPHA-> OneMinusBlendAlpha */
    case 12: return 15;   /* SRC1_COLOR              -> Source1Color */
    case 13: return 16;   /* ONE_MINUS_SRC1_COLOR    -> OneMinusSource1Color */
    case 14: return 17;   /* SRC1_ALPHA              -> Source1Alpha */
    case 15: return 18;   /* ONE_MINUS_SRC1_ALPHA    -> OneMinusSource1Alpha */
    default: return -1;
    }
}

/* ==================================================================== */
/* 例外二：NVN_MIN_FILTER —— 一个 NVN 值拆成 (min_filter, mip_filter)     */
/* ==================================================================== */

int dm_nvn_to_metal_min_filter(int nvn, int *out_mip) {
    /* NVN: NEAREST=0 LINEAR=1 NEAREST_MIPMAP_NEAREST=2 LINEAR_MIPMAP_NEAREST=3
            NEAREST_MIPMAP_LINEAR=4 LINEAR_MIPMAP_LINEAR=5
       MTL 的 MTLSamplerMinMagFilter: Nearest=0 Linear=1
       MTL 的 MTLSamplerMipFilter : NotMipmapped=0 Nearest=1 Linear=2 */
    switch (nvn) {
    case 0: if (out_mip) *out_mip = 0; return 0;   /* Nearest                    / NotMipmapped */
    case 1: if (out_mip) *out_mip = 0; return 1;   /* Linear                     / NotMipmapped */
    case 2: if (out_mip) *out_mip = 1; return 0;   /* NearestMipmapNearest        / Nearest */
    case 3: if (out_mip) *out_mip = 1; return 1;   /* LinearMipmapNearest         / Nearest */
    case 4: if (out_mip) *out_mip = 2; return 0;   /* NearestMipmapLinear         / Linear */
    case 5: if (out_mip) *out_mip = 2; return 1;   /* LinearMipmapLinear          / Linear */
    default: if (out_mip) *out_mip = 0; return -1;
    }
}

/* ==================================================================== */
/* NVN_WRAP —— 寻址模式（NVN 的名字与 Metal 不同，需显式映射）            */
/* ==================================================================== */

int dm_nvn_to_metal_wrap(int nvn) {
    /* NVN 的寻址模式（按 nvn.h 实测）：
         NVN_WRAP_CLAMP=1  NVN_WRAP_REPEAT=2  NVN_WRAP_MIRROR_CLAMP=3
         NVN_WRAP_MIRROR_REPEAT=4  NVN_WRAP_CLAMP_TO_BORDER=5
         NVN_WRAP_MIRROR_CLAMP_TO_BORDER=6  NVN_WRAP_CLAMP_NEAREST=7
         NVN_WRAP_MIRROR_CLAMP_NEAREST=8
       MTL: ClampToEdge=0 MirrorClampToEdge=1 Repeat=2 MirrorRepeat=3
            ClampToZero=4 ClampToBorderColor=5 */
    switch (nvn) {
    case 1: return 0;   /* CLAMP                      -> ClampToEdge */
    case 3: return 1;   /* MIRROR_CLAMP               -> MirrorClampToEdge */
    case 2: return 2;   /* REPEAT                     -> Repeat */
    case 4: return 3;   /* MIRROR_REPEAT              -> MirrorRepeat */
    case 7: return 4;   /* CLAMP_NEAREST              -> ClampToZero（近似：最近邻钳制） */
    case 5: return 5;   /* CLAMP_TO_BORDER            -> ClampToBorderColor */
    /* 6 / 8 是「镜像 + 边界/最近邻」的复合模式，Metal 无直接对应 —— 退到镜像钳制 */
    case 6: return 1;
    case 8: return 1;
    default: return -1;
    }
}

/* ==================================================================== */
/* 正面绕序                                                              */
/* ==================================================================== */

int dm_nvn_to_metal_front_face(int nvn) {
    /* NVN: CW=0 CCW=1     MTL: Clockwise=0 CounterClockwise=1 —— 偏移 0 */
    if (nvn < 0 || nvn > 1) return -1;
    return nvn;
}

/* ==================================================================== */
/* 覆盖统计（诊断：看游戏实际用了哪些取值，便于只做「真的用到」的那些）  */
/* ==================================================================== */

static struct {
    uint64_t cmp[9], blend[20], wrap[9], stencil[9], eq[6], minf[6];
    uint64_t unknown;
} g_use;
const void *dm_nvn_enum_usage_ptr(void) { return &g_use; }

int dm_nvn_metal_compare_tracked(int nvn) {
    if (nvn >= 0 && nvn <= 8) g_use.cmp[nvn]++;
    else g_use.unknown++;
    return dm_nvn_to_metal_compare(nvn);
}
int dm_nvn_metal_blend_tracked(int nvn) {
    if (nvn >= 0 && nvn <= 19) g_use.blend[nvn]++;
    else g_use.unknown++;
    return dm_nvn_to_metal_blend_factor(nvn);
}
int dm_nvn_metal_wrap_tracked(int nvn) {
    if (nvn >= 0 && nvn <= 8) g_use.wrap[nvn]++;
    else g_use.unknown++;
    return dm_nvn_to_metal_wrap(nvn);
}
int dm_nvn_metal_stencil_tracked(int nvn) {
    if (nvn >= 0 && nvn <= 8) g_use.stencil[nvn]++;
    else g_use.unknown++;
    return dm_nvn_to_metal_stencil_op(nvn);
}
int dm_nvn_metal_eq_tracked(int nvn) {
    if (nvn >= 0 && nvn <= 5) g_use.eq[nvn]++;
    else g_use.unknown++;
    return dm_nvn_to_metal_blend_equation(nvn);
}
int dm_nvn_metal_minf_tracked(int nvn, int *mip) {
    if (nvn >= 0 && nvn <= 5) g_use.minf[nvn]++;
    else g_use.unknown++;
    return dm_nvn_to_metal_min_filter(nvn, mip);
}
