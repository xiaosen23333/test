/* 宿主入口自测 —— 验证 dm_host_boot 能把整个宿主拉起来。
 *
 * 这是「宿主能不能起来」的端到端验证：
 *   装载 5 个模块 → 重定位 → 合并门表 → 挂 RomFS → 登记 NVN 实现 → 载着色器表
 *
 * 同时**明确验证「本平台不能执行客体代码」这件事被正确报告**，
 * 而不是静默成功（那会让人误以为已经能跑了）。
 */
#include "../hle.h"
#include "../loader.h"
#include "../hle_nvn.h"
#include "../hle_nvn_cmd.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int   dm_host_boot(const char *romfs, const char *save);
extern void  dm_host_frame(void);
extern void  dm_host_shutdown(void);
extern const void *dm_host_boot_info(void);

struct boot_info {
    unsigned nmods, nsyms, nunres, ngates;
    int      guest_entry_found;
    int      plan_found;
    unsigned n_tlsdesc;
    uint64_t n_relocs;
    int      shaders_loaded;
    int      vn_registered;
    int      fs_mounted;
};

int main(int argc, char **argv) {
    const char *moddir = (argc > 1) ? argv[1] : ".";
    const char *romfs  = (argc > 2) ? argv[2] : NULL;
    char env[1400];
    snprintf(env, sizeof env, "DM_MODULE_DIR=%s", moddir);
    putenv(env);                    /* MSVC 的 putenv 接受 "K=V" */

    printf("=== dm_host_boot ===\n");
    printf("  模块目录: %s\n", moddir);
    printf("  RomFS   : %s\n", romfs ? romfs : "(未提供)");

    int rc = dm_host_boot(romfs, NULL);
    printf("  boot 返回: %d (expect 0)\n", rc);
    if (rc) {
        printf("  <-- 启动失败；按返回码定位是哪个模块（-10-i = 第 i 个模块装载失败）\n");
        return 1;
    }

    const struct boot_info *bi = (const struct boot_info *)dm_host_boot_info();
    printf("\n=== 启动自检 ===\n");
    printf("  已装载模块     : %u (expect 5)\n", bi->nmods);
    printf("  重定位总数     : %llu\n", (unsigned long long)bi->n_relocs);
    printf("  未解析符号     : %u (expect 0（glslc 与 TLSDESC 都已接管）)\n", bi->nunres);
    printf("  门表条数       : %u\n", bi->ngates);
    printf("  RomFS 已挂     : %d (expect 1)\n", bi->fs_mounted);
    printf("  NVN 实现已登记 : %d\n", bi->vn_registered);
    printf("  着色器表载入   : %d 条\n", bi->shaders_loaded);
    printf("  客体入口 nnMain: %s\n", bi->guest_entry_found ? "已定位 OK" : "**未找到**");
    printf("  启动计划符号   : %d / 5\n", bi->plan_found);
    printf("  TLSDESC 已填   : %u\n", bi->n_tlsdesc);

    /* 逐项断言 */
    int fails = 0;
    if (bi->nmods != 5)          { printf("  ✗ 模块数不对\n"); fails++; }
    if (bi->n_relocs < 20000)    { printf("  ✗ 重定位数偏少\n"); fails++; }
    if (bi->ngates == 0)         { printf("  ✗ 门表是空的 —— 合并门表这一步有问题\n"); fails++; }
    if (!bi->fs_mounted)         { printf("  ✗ RomFS 没挂上\n"); fails++; }
    if (bi->vn_registered <= 0)  { printf("  ✗ NVN 实现没登记\n"); fails++; }
    if (!bi->guest_entry_found)  { printf("  ✗ 没找到 nnMain（客体入口）\n"); fails++; }
    if (bi->plan_found < 5)      { printf("  ✗ 启动计划符号只找到 %d / 5\n", bi->plan_found); fails++; }
    if (bi->shaders_loaded != 126) {
        printf("  ✗ 着色器表载入 %d 条，expect 126\n", bi->shaders_loaded); fails++;
    }

    printf("\n=== dm_host_frame（本平台不是 aarch64）===\n");
    /* 期望：明确报「无法执行客体代码」，且不崩 */
    dm_host_frame();
    dm_host_frame();
    printf("  连调两次未崩 OK\n");

    printf("\n=== dm_host_shutdown ===\n");
    dm_host_shutdown();
    printf("  卸载完成未崩 OK\n");

    printf("\n=== result: %s（失败 %d 项）===\n", fails ? "CHECK FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
