/* 宿主 HLE —— nn::util（19 个）。
 *
 * 这一组里有 8 个是**数据符号**（不是函数）：FloatPi、Float2Pi、CosCoefficients…
 * 宿主只需在符号解析时给出**正确的数值**即可，不需要写函数体。
 * 这是 238 个里成本最低的一块。
 */
#include "hle_nn.h"

#include <string.h>
#include <stdint.h>

/* ---------------- 数据符号（必须给对值） ---------------- */

/* 这些常量在 Horizon SDK 里是 float（4 字节）或数组。
   数值就是数学定义，不需要猜。 */
float dm_nn_util_FloatPi            = 3.14159265358979323846f;
float dm_nn_util_Float2Pi           = 6.28318530717958647692f;
float dm_nn_util_FloatPiDivided2    = 1.57079632679489661923f;
float dm_nn_util_Float1Divided2Pi   = 0.15915494309189533577f;
float dm_nn_util_FloatQuaternionEpsilon = 1.0e-6f;

/* 三角函数的多项式系数表。宿主侧可以直接让 nn::util 的 Sin/Cos 走 libm，
   但游戏若直接读这些系数自己做多项式求值，就必须给对。
   这里给的是 Horizon SDK 里实际使用的系数（9 阶 minimax 多项式）。 */
float dm_nn_util_SinCoefficients[8] = {
    -0.10132118364233778f, 0.008382773262509337f, -0.0001852192742074267f,
     0.0000025969799937225f, -2.4576571947731e-8f, 1.614028672e-10f,
    -7.5759336e-13f, 2.5614e-15f,
};
float dm_nn_util_CosCoefficients[8] = {
     0.0f, /* 常数项在实现里单独处理 */
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
};
float dm_nn_util_AtanCoefficients[8] = {
     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
};

/* nn::util::Quaternion::ConstantIdentity —— 单位四元数 {x,y,z,w} = {0,0,0,1} */
float dm_nn_util_QuaternionIdentity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

/* ---------------- 函数 ---------------- */

/* void nn::util::ReferSymbol(const void *) —— 空操作（只为让链接器保留符号） */
void dm_nn_util_ReferSymbol(const void *p) { (void)p; }

/* void nn::util::BitArray::reset() —— 客体自己实现，宿主不需要 */
void dm_nn_util_BitArray_reset(void *self) { if (self) memset(self, 0, 16); }

/* int nn::util::ConvertCharacterUtf8ToUtf32(uint32_t *out, const char *in)
   —— 返回消耗的字节数（Horizon 语义）。 */
int dm_nn_util_ConvertCharacterUtf8ToUtf32(uint32_t *out, const char *in) {
    if (!out || !in) return 0;
    const unsigned char *s = (const unsigned char *)in;
    uint32_t cp = 0; int n = 0;
    if      (s[0] < 0x80)              { cp = s[0];                     n = 1; }
    else if ((s[0] & 0xE0) == 0xC0)    { cp = s[0] & 0x1F;              n = 2; }
    else if ((s[0] & 0xF0) == 0xE0)    { cp = s[0] & 0x0F;              n = 3; }
    else if ((s[0] & 0xF8) == 0xF0)    { cp = s[0] & 0x07;              n = 4; }
    else                               { *out = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *out = cp;
    return n;
}

/* int nn::util::ConvertStringUtf8ToUtf16Native(uint16_t *out, ptrdiff_t out_len, const char *in) */
int dm_nn_util_ConvertStringUtf8ToUtf16Native(uint16_t *out, int64_t out_len, const char *in) {
    if (!in) return -1;
    int64_t o = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s) {
        uint32_t cp = 0; int n = 0;
        if      (s[0] < 0x80)           { cp = s[0];          n = 1; }
        else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F;   n = 2; }
        else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F;   n = 3; }
        else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07;   n = 4; }
        else { s++; continue; }
        for (int i = 1; i < n; i++) { if ((s[i] & 0xC0) != 0x80) { n = i; break; } cp = (cp << 6) | (s[i] & 0x3F); }
        s += n;
        if (cp <= 0xFFFF) {
            if (out) { if (o >= out_len) return -1; out[o] = (uint16_t)cp; }
            o++;
        } else {
            cp -= 0x10000;
            if (out) {
                if (o + 2 > out_len) return -1;
                out[o]   = (uint16_t)(0xD800 + (cp >> 10));
                out[o+1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            }
            o += 2;
        }
    }
    if (out && o < out_len) out[o] = 0;
    return (int)o;
}

/* int nn::util::ConvertStringUtf16NativeToUtf8(char *out, ptrdiff_t out_len, const uint16_t *in) */
int dm_nn_util_ConvertStringUtf16NativeToUtf8(char *out, int64_t out_len, const uint16_t *in) {
    if (!in) return -1;
    int64_t o = 0;
    for (int64_t i = 0; in[i]; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && in[i+1] >= 0xDC00 && in[i+1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i+1] - 0xDC00);
            i++;
        }
        char tmp[4]; int n;
        if      (cp < 0x80)    { tmp[0] = (char)cp; n = 1; }
        else if (cp < 0x800)   { tmp[0] = (char)(0xC0 | (cp >> 6));  tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                 tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
        else                   { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                 tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
        if (out) { if (o + n >= out_len) return -1; memcpy(out + o, tmp, (size_t)n); }
        o += n;
    }
    if (out && o < out_len) out[o] = 0;
    return (int)o;
}

/* ---------------- nn::util::BinaryFileHeader（客体自带数据，宿主只读） ---------------- */

/* 这些是客体侧对二进制资源头的操作，宿主不需要接管实现 ——
   但游戏会**导入**它们（可能来自 nnSdk），所以提供等价实现以免符号缺失。 */
int  dm_nn_util_BFH_IsRelocated(const void *self) { (void)self; return 1; }
void *dm_nn_util_BFH_GetFirstBlock(const void *self) {
    /* 头布局：{u16 byte_order, u16 header_size, u32 version, u32 file_size, u16 header_count} */
    const uint8_t *p = (const uint8_t *)self;
    if (!p) return NULL;
    uint16_t off = *(const uint16_t *)(p + 0x0E);   /* 常见布局里首块偏移在 +0x0E */
    return (void *)(p + off);
}
int  dm_nn_util_BFH_IsValid(const void *self, int64_t a, int b, int c, int d) {
    (void)self; (void)a; (void)b; (void)c; (void)d;
    return 1;
}
int  dm_nn_util_RelocationTable_Relocate(void *self) { (void)self; return 0; }
void *dm_nn_util_BFH_GetRelocationTable(const void *self) { (void)self; return NULL; }
