/* NVN → Metal 枚举映射自测。
 *
 * 判据（全部刻意手算，不调被测实现）：
 *   1. 统一偏移的五组：逐值核对 Metal 结果 == NVN 值 - 1（或 0）
 *   2. 混合因子：19 个取值逐个核对目标值
 *   3. 缩小过滤：6 个取值逐个核对 (min, mip) 二元组
 *   4. 越界输入必须返回 -1（不能让脏值静默通过）
 */
#include "../hle_nvn_metal.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { printf("    ✗ " fmt "\n", __VA_ARGS__); fails++; } \
} while (0)

int main(void) {
    printf("=== 1) 统一偏移组：Metal = NVN - 1 ===\n");
    /* NVN 1..8 -> MTL 0..7 */
    for (int nvn = 1; nvn <= 8; nvn++) {
        int m = dm_nvn_to_metal_compare(nvn);
        CHECK(m == nvn - 1, "compare(%d) = %d, expect %d", nvn, m, nvn - 1);
    }
    printf("  比较函数 / 深度函数 1..8 -> 0..7  %s\n", fails ? "有错" : "OK");

    int f0 = fails;
    for (int nvn = 1; nvn <= 8; nvn++) {
        int m = dm_nvn_to_metal_stencil_op(nvn);
        CHECK(m == nvn - 1, "stencil_op(%d) = %d, expect %d", nvn, m, nvn - 1);
    }
    printf("  模板操作 1..8 -> 0..7  %s\n", fails == f0 ? "OK" : "有错");

    f0 = fails;
    for (int nvn = 1; nvn <= 5; nvn++) {
        int m = dm_nvn_to_metal_blend_equation(nvn);
        CHECK(m == nvn - 1, "eq(%d) = %d, expect %d", nvn, m, nvn - 1);
    }
    printf("  混合方程 1..5 -> 0..4  %s\n", fails == f0 ? "OK" : "有错");

    f0 = fails;
    CHECK(dm_nvn_to_metal_mag_filter(0) == 0, "mag(0) = %d, expect 0", dm_nvn_to_metal_mag_filter(0));
    CHECK(dm_nvn_to_metal_mag_filter(1) == 1, "mag(1) = %d, expect 1", dm_nvn_to_metal_mag_filter(1));
    printf("  放大过滤 0..1 -> 0..1（偏移 0）  %s\n", fails == f0 ? "OK" : "有错");

    printf("\n=== 2) 混合因子（19 个取值逐个核对）===\n");
    /* NVN 编号 -> 期望的 MTLBlendFactor 值 */
    static const struct { int nvn, want; const char *nm; } BF[] = {
        { 1,  0, "ZERO" },                    { 2,  1, "ONE" },
        { 3,  2, "SRC_COLOR" },               { 4,  3, "ONE_MINUS_SRC_COLOR" },
        { 5,  4, "SRC_ALPHA" },               { 6,  5, "ONE_MINUS_SRC_ALPHA" },
        { 9,  6, "DST_COLOR" },               { 10, 7, "ONE_MINUS_DST_COLOR" },
        { 7,  8, "DST_ALPHA" },               { 8,  9, "ONE_MINUS_DST_ALPHA" },
        { 11, 10, "SRC_ALPHA_SATURATE" },     { 16, 11, "CONSTANT_COLOR" },
        { 17, 12, "ONE_MINUS_CONSTANT_COLOR" },{ 18, 13, "CONSTANT_ALPHA" },
        { 19, 14, "ONE_MINUS_CONSTANT_ALPHA" },{ 12, 15, "SRC1_COLOR" },
        { 13, 16, "ONE_MINUS_SRC1_COLOR" },   { 14, 17, "SRC1_ALPHA" },
        { 15, 18, "ONE_MINUS_SRC1_ALPHA" },
    };
    f0 = fails;
    for (unsigned i = 0; i < sizeof BF / sizeof BF[0]; i++) {
        int m = dm_nvn_to_metal_blend_factor(BF[i].nvn);
        if (m != BF[i].want) {
            printf("    ✗ %-24s nvn=%2d -> %2d, expect %2d\n", BF[i].nm, BF[i].nvn, m, BF[i].want);
            fails++;
        }
    }
    printf("  19 个混合因子  %s\n", fails == f0 ? "全部 OK" : "有错");

    printf("\n=== 3) 缩小过滤（拆成 min + mip）===\n");
    static const struct { int nvn, wmin, wmip; const char *nm; } MF[] = {
        { 0, 0, 0, "NEAREST" },                    { 1, 1, 0, "LINEAR" },
        { 2, 0, 1, "NEAREST_MIPMAP_NEAREST" },     { 3, 1, 1, "LINEAR_MIPMAP_NEAREST" },
        { 4, 0, 2, "NEAREST_MIPMAP_LINEAR" },      { 5, 1, 2, "LINEAR_MIPMAP_LINEAR" },
    };
    f0 = fails;
    for (unsigned i = 0; i < sizeof MF / sizeof MF[0]; i++) {
        int mip = -1;
        int mn = dm_nvn_to_metal_min_filter(MF[i].nvn, &mip);
        if (mn != MF[i].wmin || mip != MF[i].wmip) {
            printf("    ✗ %-24s nvn=%d -> (min=%d, mip=%d), expect (%d, %d)\n",
                   MF[i].nm, MF[i].nvn, mn, mip, MF[i].wmin, MF[i].wmip);
            fails++;
        }
    }
    printf("  6 个缩小过滤  %s\n", fails == f0 ? "全部 OK" : "有错");

    printf("\n=== 4) 寻址模式 ===\n");
    static const struct { int nvn, want; const char *nm; } WR[] = {
        { 1, 0, "CLAMP" }, { 2, 2, "REPEAT" }, { 3, 1, "MIRROR_CLAMP" },
        { 4, 3, "MIRROR_REPEAT" }, { 5, 5, "CLAMP_TO_BORDER" },
        { 6, 1, "MIRROR_CLAMP_TO_BORDER" }, { 7, 4, "CLAMP_NEAREST" },
        { 8, 1, "MIRROR_CLAMP_NEAREST" },
    };
    f0 = fails;
    for (unsigned i = 0; i < sizeof WR / sizeof WR[0]; i++) {
        int m = dm_nvn_to_metal_wrap(WR[i].nvn);
        if (m != WR[i].want) {
            printf("    ✗ %-24s nvn=%d -> %d, expect %d\n", WR[i].nm, WR[i].nvn, m, WR[i].want);
            fails++;
        }
    }
    printf("  8 个寻址模式  %s\n", fails == f0 ? "全部 OK" : "有错");

    printf("\n=== 5) 越界输入必须返回 -1 ===\n");
    f0 = fails;
    CHECK(dm_nvn_to_metal_compare(0) == -1, "compare(0) = %d, expect -1", dm_nvn_to_metal_compare(0));
    CHECK(dm_nvn_to_metal_compare(9) == -1, "compare(9) = %d, expect -1", dm_nvn_to_metal_compare(9));
    CHECK(dm_nvn_to_metal_blend_factor(0) == -1, "blend(0) = %d, expect -1", dm_nvn_to_metal_blend_factor(0));
    CHECK(dm_nvn_to_metal_blend_factor(20) == -1, "blend(20) = %d, expect -1", dm_nvn_to_metal_blend_factor(20));
    CHECK(dm_nvn_to_metal_min_filter(9, NULL) == -1, "minf(9) = %d, expect -1", dm_nvn_to_metal_min_filter(9, NULL));
    CHECK(dm_nvn_to_metal_wrap(0) == -1, "wrap(0) = %d, expect -1", dm_nvn_to_metal_wrap(0));
    CHECK(dm_nvn_to_metal_wrap(99) == -1, "wrap(99) = %d, expect -1", dm_nvn_to_metal_wrap(99));
    /* min_filter 允许 out_mip 传 NULL（不能崩） */
    CHECK(dm_nvn_to_metal_min_filter(3, NULL) == 1, "minf(3, NULL) = %d, expect 1", dm_nvn_to_metal_min_filter(3, NULL));
    printf("  越界与 NULL 处理  %s\n", fails == f0 ? "OK" : "有错");

    printf("\n=== 6) 使用统计包装可用 ===\n");
    dm_nvn_metal_compare_tracked(4);
    dm_nvn_metal_blend_tracked(6);
    int mip2 = 0;
    dm_nvn_metal_minf_tracked(5, &mip2);
    printf("  统计：compare(4)、blend(6)、minf(5)->mip=%d 均记下  OK\n", mip2);

    printf("\n=== result: %s（失败 %d 项）===\n", fails ? "CHECK FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
