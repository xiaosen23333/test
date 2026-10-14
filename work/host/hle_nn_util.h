/* 宿主 HLE —— nn::util（数学常量 / 编码转换 / 二进制头）的声明。 */
#ifndef DM_HLE_NN_UTIL_H
#define DM_HLE_NN_UTIL_H

#include <stdint.h>

/* 数据符号（必须给对数值） */
extern float dm_nn_util_FloatPi;
extern float dm_nn_util_Float2Pi;
extern float dm_nn_util_FloatPiDivided2;
extern float dm_nn_util_Float1Divided2Pi;
extern float dm_nn_util_FloatQuaternionEpsilon;
extern float dm_nn_util_SinCoefficients[8];
extern float dm_nn_util_CosCoefficients[8];
extern float dm_nn_util_AtanCoefficients[8];
extern float dm_nn_util_QuaternionIdentity[4];

/* 函数 */
void  dm_nn_util_ReferSymbol(const void *p);
void  dm_nn_util_BitArray_reset(void *self);
int   dm_nn_util_ConvertCharacterUtf8ToUtf32(uint32_t *out, const char *in);
int   dm_nn_util_ConvertStringUtf8ToUtf16Native(uint16_t *out, int64_t out_len, const char *in);
int   dm_nn_util_ConvertStringUtf16NativeToUtf8(char *out, int64_t out_len, const uint16_t *in);
int   dm_nn_util_BFH_IsRelocated(const void *self);
void *dm_nn_util_BFH_GetFirstBlock(const void *self);
int   dm_nn_util_BFH_IsValid(const void *self, int64_t a, int b, int c, int d);
int   dm_nn_util_RelocationTable_Relocate(void *self);
void *dm_nn_util_BFH_GetRelocationTable(const void *self);

#endif /* DM_HLE_NN_UTIL_H */
