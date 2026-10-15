/* 宿主侧诊断通道 —— 实现（平台无关，Windows 上也能编、也能测）。
 *
 * 设计取舍（都是有原因的，别改回去）：
 *   · **只用 C 标准库**（fopen/fprintf/vsnprintf）——
 *     这样这段代码在 Windows 自测里也能跑，不必等真机。
 *   · 写日志后**立刻 fflush** —— 真机上常常是崩溃之后才去读这个文件，
 *     靠 exit 时的缓冲区刷新是等不到的。
 *   · 不锁、不加临界区：客体与主线程都可能打标记，出错最坏是日志行交错，
 *     比「为了加锁引入新的死锁」好得多（这个项目已经吃过自锁死的亏）。
 */
#include "hle_diag.h"

#include "hle_nvn_cmd.h"     /* dm_nvn_stat_* */
#include "hle_nvn_mtl.h"     /* dm_mtl_stat_* */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define DM_DIAG_MAX_BYTES   (256 * 1024)   /* 超过它就只留尾部 */
#define DM_DIAG_KEEP_BYTES  (64  * 1024)   /* 留多少 */

static FILE *g_log = NULL;

/* 三段长期保存的文字 */
static char g_boot[192]  = "（还没装载）";
static char g_stage[160] = "还没进客体";
static char g_event[160] = "（无事件）";
static int  g_stage_no   = 0;

/* ------------------------------------------------------------------ */
/* 日志文件                                                            */
/* ------------------------------------------------------------------ */

static long file_size(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* 太大了就只保留尾部 —— 用「读到临时文件再改名」的做法，
   避免一边读一边写同一个文件把内容搅坏。 */
static void shrink_if_big(const char *path) {
    long sz = file_size(path);
    if (sz <= DM_DIAG_MAX_BYTES) return;

    FILE *in = fopen(path, "rb");
    if (!in) return;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return; }

    fseek(in, sz - DM_DIAG_KEEP_BYTES, SEEK_SET);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);

    remove(path);
    rename(tmp, path);
}

void dm_diag_set_log(const char *path) {
    if (g_log) { fclose(g_log); g_log = NULL; }
    if (!path || !path[0]) return;
    shrink_if_big(path);
    g_log = fopen(path, "a");
}

static void emit(const char *line) {
    if (!g_log) return;
    fputs(line, g_log);
    fputc('\n', g_log);
    fflush(g_log);       /* 崩溃之后才来读这个文件 —— 必须现在就落盘 */
}

/* ------------------------------------------------------------------ */
/* 打标记                                                              */
/* ------------------------------------------------------------------ */

void dm_diag_mark(const char *fmt, ...) {
    char s[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    snprintf(g_event, sizeof g_event, "%s", s);

    char line[256];
    snprintf(line, sizeof line, "[事件] %s", s);
    emit(line);
}

void dm_diag_stage(int stage, const char *fmt, ...) {
    char s[160];
    va_list ap; va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);

    g_stage_no = stage;
    snprintf(g_stage, sizeof g_stage, "%d/5 %s", stage, s);

    char line[224];
    snprintf(line, sizeof line, "[客体] 第 %d 步：%s", stage, s);
    emit(line);
}

void dm_diag_boot(const char *fmt, ...) {
    char s[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    snprintf(g_boot, sizeof g_boot, "%s", s);

    char line[224];
    snprintf(line, sizeof line, "[装载] %s", s);
    emit(line);
}

int         dm_diag_stage_no(void)   { return g_stage_no; }
const char *dm_diag_stage_text(void) { return g_stage; }
const char *dm_diag_boot_text(void)  { return g_boot; }
const char *dm_diag_last_event(void) { return g_event; }

/* ------------------------------------------------------------------ */
/* 一行总览                                                            */
/* ------------------------------------------------------------------ */

int dm_host_diag(char *buf, int n) {
    if (!buf || n <= 0) return 0;
    return snprintf(buf, (size_t)n,
                    "客体 %s | 录 %llu 放 %llu 丢 %llu | 本帧 pass %llu draw %llu present %llu",
                    g_stage,
                    (unsigned long long)dm_nvn_stat_recorded(),
                    (unsigned long long)dm_nvn_stat_replayed(),
                    (unsigned long long)dm_nvn_stat_dropped(),
                    (unsigned long long)dm_mtl_stat_passes(),
                    (unsigned long long)dm_mtl_stat_draws(),
                    (unsigned long long)dm_mtl_stat_presents());
}
