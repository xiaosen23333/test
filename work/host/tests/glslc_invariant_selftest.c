/* glslc 接管的**不变量**自测。
 *
 * 第 31 轮我把 glslc 描述成「硬阻塞：glslcCompile 必须返回正确的着色器二进制」。
 * 第 33 轮反汇编核对后，结论变了 —— 本文件把**真实的不变量**锁住：
 *
 *   客体（NmplDLL.nro）对 glslc 只做两件事：
 *     ① 把 8 个函数指针填进一个表（0xccfd4 那个函数，8 次 bl+str）
 *     ② 校验 8 个都非零（cmp/ccmp/cset → 返回布尔）
 *   **没有任何代码真正调用它们**（8 个槽的引用点后续 12 条指令里无 blr）。
 *
 *   原因：游戏自带**预编译好的着色器**（shader_1_00.dat 是 FAFULLFS 容器），
 *   不需要运行期编译器。glslc 是**开发期**用的（这也是 NVIDIA 的定位）。
 *
 * 所以真正必须满足的不变量是：
 *   **dm_glslc_lookup 对这 8 个名字都必须返回非 NULL。**
 * 这一条不满足 → 那个校验函数返回假 → 引擎可能走「环境没装好」的分支。
 *
 * 本自测就是把这个不变量写成断言，避免以后改代码时无意破坏它。
 * （shader_1_00.dat 的识别能力仍然保留 —— 它是**外部插件**真调 glslc 时的回退路径，
 *   不是关键路径。第 31 轮的「识别 126/126」自测继续有效，但优先级应降级。）
 */
#include "../hle_nvn.h"
#include "../hle_nvn_cmd.h"
#include "../hle_nvn_glslc.h"

#include <stdio.h>
#include <string.h>

/* 客体装载函数（0xccfd4）会按这个顺序把 8 个指针填进表 [0..0x38]。
   顺序由访问器地址决定（实测）：0xcd090→槽0x7e3ae0 … 0xcd100→槽0x7e3b18 */
static const char *k_order[8] = {
    "glslcCompilePreSpecialized",   /* 槽 [0]  0x7e3ae0 */
    "glslcCompileSpecialized",      /* 槽 [8]  0x7e3ae8 */
    "glslcInitialize",              /* 槽 [10] 0x7e3af0 */
    "glslcFinalize",                /* 槽 [18] 0x7e3af8 */
    "glslcCompile",                 /* 槽 [20] 0x7e3b00 */
    "glslcGetVersion",              /* 槽 [28] 0x7e3b08 */
    "glslcSetAllocator",            /* 槽 [30] 0x7e3b10 */
    "glslcGetDefaultOptions",       /* 槽 [38] 0x7e3b18 */
};

int main(int argc, char **argv) {
    int fails = 0;

    printf("=== 1) 不变量：8 个名字都必须解析出非 NULL ===\n");
    void *slot[8];
    for (int i = 0; i < 8; i++) {
        slot[i] = dm_glslc_lookup(k_order[i]);
        printf("  [%d] %-28s -> %s\n", i, k_order[i], slot[i] ? "非 NULL OK" : "**NULL**");
        if (!slot[i]) fails++;
    }
    printf("  8 个全部非 NULL: %s\n", fails ? "否" : "是");

    printf("\n=== 2) 模拟客体的校验函数（cmp/ccmp/cset）===\n");
    /* 客体逻辑：8 个都非零则返回 1，否则 0 */
    int all = 1;
    for (int i = 0; i < 8; i++) if (!slot[i]) { all = 0; break; }
    printf("  客体式校验结果: %d (expect 1)\n", all);
    if (!all) fails++;

    printf("\n=== 3) 8 个函数都是不同的地址（不能填重了）===\n");
    int dup = 0;
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
            if (slot[i] == slot[j]) { printf("  ✗ [%d] 与 [%d] 相同\n", i, j); dup++; }
    printf("  重复地址: %d (expect 0)  %s\n", dup, dup ? "<-- 有重复" : "OK");
    if (dup) fails++;

    printf("\n=== 4) 表外的名字不应被解析（避免误命中）===\n");
    static const char *neg[] = { "glslc", "glslcCompileX", "glslcCompilePre", "" };
    int bad = 0;
    for (unsigned i = 0; i < sizeof neg / sizeof neg[0]; i++) {
        void *p = dm_glslc_lookup(neg[i]);
        printf("  %-20s -> %s\n", neg[i][0] ? neg[i] : "(空串)", p ? "**非 NULL（不该）**" : "NULL OK");
        if (p) bad++;
    }
    printf("  误命中: %d (expect 0)\n", bad);
    if (bad) fails++;

    printf("\n=== 5) 返回值：按「从不被调用」的前提，只要求能安全返回 ===\n");
    printf("  GetVersion -> \"%s\"（非 NULL 即可）\n", dm_glslc_GetVersion());
    if (!dm_glslc_GetVersion()) fails++;
    /* 乱参数不能崩（真机上万一被外部插件调到） */
    dm_glslc_Initialize();
    dm_glslc_Finalize();
    dm_glslc_SetAllocator(NULL, NULL);
    dm_glslc_GetDefaultOptions(NULL);      /* 传 NULL 不能崩 */
    void *r = dm_glslc_Compile(NULL, NULL, NULL, NULL, NULL, NULL);
    printf("  Compile(NULL×6) -> %p，未崩 OK\n", r);

    printf("\n=== result: %s（失败 %d 项）===\n", fails ? "CHECK FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
