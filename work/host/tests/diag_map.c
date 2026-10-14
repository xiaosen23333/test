/* 诊断 shader_map.tsv 的解析。 */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *p = (argc > 1) ? argv[1] : "shader_map.tsv";
    FILE *f = fopen(p, "r");
    if (!f) { printf("打不开 %s\n", p); return 1; }
    char line[2048];
    int total = 0, data = 0;
    while (fgets(line, sizeof line, f)) {
        total++;
        if (total <= 3) {
            printf("--- 行 %d (len=%zu) ---\n", total, strlen(line));
            printf("  原样: %s\n", line);
            /* 拆 tab 并打印每个字段 */
            char buf[2048]; snprintf(buf, sizeof buf, "%s", line);
            char *fields[16] = {0}; unsigned nf = 0; char *q = buf;
            while (nf < 16 && q) {
                fields[nf++] = q;
                char *t = strchr(q, '\t');
                if (t) { *t = 0; q = t + 1; } else q = NULL;
            }
            printf("  字段数: %u\n", nf);
            for (unsigned i = 0; i < nf; i++)
                printf("    [%u] \"%s\" (len=%zu)\n", i, fields[i], strlen(fields[i]));
        }
        if (line[0] != '#') data++;
    }
    fclose(f);
    printf("总行数 %d，数据行 %d\n", total, data);
    return 0;
}
