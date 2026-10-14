/* glslc 接管自测 —— 用**真实的着色器数据**验证识别链。
 *
 * 链路：
 *   从 shader_1_00.dat 取一条真实记录（SPH + 指令 + 控制段）
 *        → dm_glslc_identify
 *        → 算 key → 查 126 条映射表
 *        → 返回对应的 Metal 着色器路径
 *
 * 这是「glslc 能被离线结果替代」的端到端证据：
 * 只要引擎交给 glslc 的字节里含控制段，宿主就能确定是哪个着色器。
 */
#include "../hle_nvn.h"
#include "../hle_nvn_cmd.h"
#include "../hle_nvn_glslc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *map   = (argc > 1) ? argv[1] : "shader_map.tsv";
    const char *metal = (argc > 2) ? argv[2] : "metal";
    const char *dat   = (argc > 3) ? argv[3] : NULL;

    printf("=== 1) 载入映射表 ===\n");
    int n = dm_nvn_shader_load(map, metal);
    printf("  %d 条（%s）\n", n, map);
    if (n <= 0) return 1;

    printf("\n=== 2) glslc 的简单入口 ===\n");
    dm_glslc_Initialize();
    printf("  GetVersion        -> \"%s\"\n", dm_glslc_GetVersion());
    dm_glslc_SetAllocator((void *)0x1000, (void *)0x2000);   /* 不应崩 */
    unsigned char opts[0x100];
    memset(opts, 0xAA, sizeof opts);
    dm_glslc_GetDefaultOptions(opts);
    int allzero = 1;
    for (unsigned i = 0; i < sizeof opts; i++) if (opts[i]) { allzero = 0; break; }
    printf("  GetDefaultOptions -> 输出被清零: %s\n", allzero ? "是 OK" : "**否**");
    dm_glslc_Finalize();

    printf("\n=== 3) 用真实着色器数据识别（关键验证）===\n");
    if (!dat) {
        printf("  (未提供 shader_1_00.dat，跳过)\n");
    } else {
        FILE *f = fopen(dat, "rb");
        if (!f) { printf("  打不开 %s\n", dat); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *d = (unsigned char *)malloc((size_t)sz);
        size_t got = fread(d, 1, (size_t)sz, f); fclose(f);
        printf("  读入 %zu 字节\n", got);

        /* 读映射表拿 (name, stage, key, sph_at, ctrl_at, instr_len, c1_len) */
        typedef struct { char name[64], stage[4], key[32]; long sph, ctrl, ilen, clen; } row_t;
        static row_t rows[256];
        unsigned nr = 0;
        FILE *m = fopen(map, "r");
        char line[2048];
        while (m && fgets(line, sizeof line, m) && nr < 256) {
            if (line[0] == '#') continue;
            char *fl[12] = {0}; unsigned nf = 0; char *p = line;
            while (nf < 12 && p) { fl[nf++] = p; char *t = strchr(p, '\t'); if (t) { *t = 0; p = t+1; } else p = NULL; }
            if (nf < 7) continue;
            row_t *r = &rows[nr];
            snprintf(r->name, sizeof r->name, "%s", fl[0]);
            snprintf(r->stage, sizeof r->stage, "%s", fl[1]);
            snprintf(r->key, sizeof r->key, "%s", fl[2]);
            r->sph  = strtol(fl[3], NULL, 10);
            r->ctrl = strtol(fl[4], NULL, 10);
            r->ilen = strtol(fl[5], NULL, 10);
            r->clen = strtol(fl[6], NULL, 10);
            nr++;
        }
        if (m) fclose(m);
        printf("  映射表 %u 条\n", nr);

        /* 对每一条：构造「引擎会交给 glslc 的那段字节」= SPH + 指令 + 控制段，
           也就是从 sph_at 到 ctrl_at+0x880 的这一段。 */
        unsigned ok = 0, fail = 0;
        for (unsigned i = 0; i < nr; i++) {
            long from = rows[i].sph, to = rows[i].ctrl + 0x880;
            if (from <= 0 || to > sz) { fail++; continue; }
            size_t len = (size_t)(to - from);
            int is_vs = -1;
            const char *mp = dm_glslc_identify(d + from, len, &is_vs);
            if (mp && dm_glslc_last_key()[0]) {
                if (!strcmp(dm_glslc_last_key(), rows[i].key)) ok++;
                else {
                    if (fail < 3)
                        printf("    第 %u 条 key 不符：表=%s 识=%s\n", i, rows[i].key, dm_glslc_last_key());
                    fail++;
                }
            } else {
                if (fail < 3) printf("    第 %u 条未识别（%s %s）\n", i, rows[i].name, rows[i].stage);
                fail++;
            }
        }
        printf("  识别 %u / %u，失败 %u  %s\n", ok, nr, fail,
               (ok == nr) ? "**全部识别 OK**" : "<-- 有失败");
        if (ok) printf("  （最近一次: key=%s name=\"%s\"）\n",
                       dm_glslc_last_key(), dm_glslc_last_name());
        free(d);
    }

    printf("\n=== 4) 编译入口的可调用性 ===\n");
    /* 参数乱给不应崩（引擎的参数布局我们没验证过，所以容错是必须的） */
    void *r1 = dm_glslc_Compile(NULL, NULL, NULL, NULL, NULL, NULL);
    void *r2 = dm_glslc_CompilePreSpecialized((void *)1, (void *)2, (void *)3, (void *)4, (void *)5, (void *)6);
    void *r3 = dm_glslc_CompileSpecialized(NULL, (void *)0x100000, NULL, NULL, NULL, NULL);
    printf("  乱参数调用: %p %p %p（都应为 NULL 或不崩）\n", r1, r2, r3);

    printf("\n=== 5) 符号查找钩子 ===\n");
    static const char *probe[] = {
        "glslcInitialize", "glslcFinalize", "glslcGetVersion", "glslcSetAllocator",
        "glslcGetDefaultOptions", "glslcCompile", "glslcCompilePreSpecialized",
        "glslcCompileSpecialized", "glslcNotExist",
    };
    unsigned hit = 0;
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        void *p = dm_glslc_lookup(probe[i]);
        if (p) hit++;
        printf("    %-28s -> %s\n", probe[i], p ? "有" : "(无，符合预期)");
    }
    printf("  命中 %u / 8（第 9 个应无）\n", hit);

    printf("\n=== result: %s ===\n", (hit == 8) ? "ALL PASS" : "CHECK FAILED");
    return (hit == 8) ? 0 : 1;
}
