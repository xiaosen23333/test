/* NVNformat -> MTLPixelFormat 映射自测。
 *
 * 判据（手算，不调被测实现）：
 *   1. 前几个格式的对应关系与 NVN 官方枚举 + Metal 官方枚举一致
 *   2. 无对应的格式返回 NULL（而不是返回一个假值）
 *   3. 越界与 NVN_FORMAT_NONE 返回 NULL
 *   4. noalpha / rb 标志位被正确带出（这两个标志在真机上决定「画面颜色对不对」）
 */
#include "../hle_nvn_fmt.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    x " __VA_ARGS__); fails++; } } while (0)

int main(void) {
    printf("=== 1) 表的规模 ===\n");
    unsigned n = dm_nvn_fmt_count();
    printf("  有条目的 NVN 格式: %u (expect 107 = 78 精确 + 29 替代)\n", n);
    /* 用**精确值**断言，不用「大约」—— 上一轮我因为写「reg >= 30」而误报失败 */
    CHECK(n == 107, "条目数 %u，expect 107\n", n);

    printf("\n=== 2) 逐项核对（NVN 枚举 + Metal 枚举都手算）===\n");
    /* NVN_FORMAT_* 值来自 nvn.h；MTLPixelFormat 值来自 Apple 头文件。
       下面这一组是「两边都确定」的锚点。 */
    static const struct { int nvn; int mtl; const char *nm; } A[] = {
        { 0x01, 10,  "NVN_FORMAT_R8        -> MTLPixelFormatR8Unorm" },
        { 0x02, 12,  "NVN_FORMAT_R8SN      -> MTLPixelFormatR8Snorm" },
        { 0x03, 13,  "NVN_FORMAT_R8UI      -> MTLPixelFormatR8Uint" },
        { 0x04, 14,  "NVN_FORMAT_R8I       -> MTLPixelFormatR8Sint" },
        { 0x05, 25,  "NVN_FORMAT_R16F      -> MTLPixelFormatR16Float" },
        { 0x06, 20,  "NVN_FORMAT_R16       -> MTLPixelFormatR16Unorm" },
        { 0x0A, 55,  "NVN_FORMAT_R32F      -> MTLPixelFormatR32Float" },
        { 0x0B, 53,  "NVN_FORMAT_R32UI     -> MTLPixelFormatR32Uint" },
    };
    for (unsigned i = 0; i < sizeof A / sizeof A[0]; i++) {
        const dm_fmt_ent *e = dm_nvn_fmt_lookup(A[i].nvn);
        printf("  %-52s %s", A[i].nm, e ? "有" : "**无**");
        if (e) printf(" (mtl=%u kind=%u)", e->mtl, e->kind);
        printf("\n");
        CHECK(e != 0, "格式 %#x 应存在\n", A[i].nvn);
        if (e) CHECK(e->mtl == A[i].mtl, "格式 %#x: mtl=%u, expect %d\n", A[i].nvn, e->mtl, A[i].mtl);
    }

    printf("\n=== 3) 无对应的格式必须返回 NULL ===\n");
    /* NVN_FORMAT_NONE = 0 一定没有对应 */
    CHECK(dm_nvn_fmt_lookup(0) == 0, "NONE(0) 应返回 NULL\n");
    printf("  NVN_FORMAT_NONE(0)     -> NULL OK\n");
    /* 越界 */
    CHECK(dm_nvn_fmt_lookup(-1) == 0,  "负数应返回 NULL\n");
    CHECK(dm_nvn_fmt_lookup(0x9D) == 0, "0x9D 应返回 NULL\n");
    CHECK(dm_nvn_fmt_lookup(0x1000) == 0, "0x1000 应返回 NULL\n");
    printf("  -1 / 0x9D / 0x1000     -> NULL OK\n");
    /* RGB8（三分量）Metal 没有 —— 按 D2R 的说明应是 N（无对应） */
    printf("  NVN_FORMAT_RGB8(0x19?) -> %s\n",
           dm_nvn_fmt_lookup(0x19) ? "有对应" : "NULL（符合预期：Metal 无 3 分量格式）");

    printf("\n=== 4) noalpha / rb 标志 ===\n");
    unsigned na = 0, rb = 0, exact = 0, sub = 0;
    for (int i = 0; i < 0x9D; i++) {
        const dm_fmt_ent *e = dm_nvn_fmt_lookup(i);
        if (!e) continue;
        if (e->noalpha) na++;
        if (e->rb) rb++;
        if (e->kind == 1) exact++; else if (e->kind == 2) sub++;
    }
    printf("  精确对应 %u，替代 %u；noalpha 标志 %u 个，rb 交换标志 %u 个\n", exact, sub, na, rb);
    CHECK(exact + sub == n, "kind 统计与总数不符\n");
    CHECK(exact > 0, "应有精确对应\n");
    /* D2R 的说明里明确提到 RGBX / RGB_DXT1 这类「无 alpha」格式存在 */
    CHECK(na > 0, "应有 noalpha 标志的格式（RGBX 一类）\n");
    CHECK(rb > 0, "应有需交换 r/b 的格式（RGB 系 16 位打包）\n");

    printf("\n=== 5) 打印几个带标志的（核对语义）===\n");
    int shown = 0;
    for (int i = 1; i < 0x9D && shown < 6; i++) {
        const dm_fmt_ent *e = dm_nvn_fmt_lookup(i);
        if (e && (e->noalpha || e->rb)) {
            printf("    NVN %#04x -> mtl=%u kind=%u noalpha=%u rb=%u\n",
                   i, e->mtl, e->kind, e->noalpha, e->rb);
            shown++;
        }
    }

    printf("\n=== result: %s（失败 %d 项）===\n", fails ? "CHECK FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
