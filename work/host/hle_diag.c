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
#include <stdint.h>     /* uintptr_t —— 模块表用 */
#include <string.h>
#include <stdlib.h>

#if defined(__unix__) || defined(__APPLE__)
  #include <unistd.h>          /* write —— 崩溃处理里唯一敢用的输出方式 */
#endif

#define DM_DIAG_MAX_BYTES   (256 * 1024)   /* 超过它就只留尾部 */
#define DM_DIAG_KEEP_BYTES  (64  * 1024)   /* 留多少 */

static FILE *g_log = NULL;
static int   g_logfd = -1;      /* 与 g_log 同一个文件；崩溃时用它直写 */

/* 三段长期保存的文字 */
static char g_boot[192]  = "（还没装载）";
static char g_stage[160] = "还没进客体";
static char g_build[64]  = "(未标记)";
static volatile unsigned long long g_ticks = 0;
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
    if (g_log) { fclose(g_log); g_log = NULL; g_logfd = -1; }
    if (!path || !path[0]) return;
    shrink_if_big(path);
    g_log = fopen(path, "a");
    if (g_log) g_logfd = fileno(g_log);
}

int dm_diag_log_fd(void) { return g_logfd; }

void dm_diag_set_build(const char *stamp) {
    if (!stamp) return;
    snprintf(g_build, sizeof g_build, "%s", stamp);
}
unsigned long long dm_diag_ticks(void) { return g_ticks; }

static void emit(const char *line) {
    g_ticks++;
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
    snprintf(g_stage, sizeof g_stage, "第 %d 步 %s", stage, s);

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
                    "[%s] 客体 %s | 录 %llu 放 %llu 丢 %llu | 本帧 pass %llu draw %llu present %llu",
                    g_build, g_stage,
                    (unsigned long long)dm_nvn_stat_recorded(),
                    (unsigned long long)dm_nvn_stat_replayed(),
                    (unsigned long long)dm_nvn_stat_dropped(),
                    (unsigned long long)dm_mtl_stat_passes(),
                    (unsigned long long)dm_mtl_stat_draws(),
                    (unsigned long long)dm_mtl_stat_presents());
}

/* ------------------------------------------------------------------ */
/* 崩溃诊断                                                            */
/* ------------------------------------------------------------------ */
/* 这一段**不用 stdio、不分配内存**：信号处理里只有 write(2) 之类的少数调用是安全的，
 * snprintf/malloc 都可能踩到锁或分配器而二次崩溃（那就什么痕迹都留不下了）。
 * 所以这里手写 hex/dec 格式化，字符串拼好后一次 write。 */

typedef struct { char *p; int n; int cap; } sb_t;

static void sb_c(sb_t *b, char c)            { if (b->n < b->cap - 1) b->p[b->n++] = c; }
static void sb_s(sb_t *b, const char *s)     { while (*s) sb_c(b, *s++); }
static void sb_hex(sb_t *b, unsigned long long v) {
    char t[17]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { unsigned d = (unsigned)(v & 0xF); t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    sb_s(b, "0x");
    while (k) sb_c(b, t[--k]);
}
static void sb_dec(sb_t *b, unsigned long long v) {
    char t[24]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k) sb_c(b, t[--k]);
}

/* 模块表：装载时登记，崩溃时用来把 PC 翻成「模块 + 偏移」 */
#define DM_DIAG_MAX_MODS 8
typedef struct { char name[24]; unsigned long long base, size; } dm_diag_mod;
static dm_diag_mod g_mods[DM_DIAG_MAX_MODS];
static int          g_nmods = 0;

void dm_diag_set_module(const char *name, const void *base, unsigned long long size) {
    if (g_nmods >= DM_DIAG_MAX_MODS) return;
    dm_diag_mod *m = &g_mods[g_nmods++];
    int i = 0;
    for (; name && name[i] && i < (int)sizeof m->name - 1; i++) m->name[i] = name[i];
    m->name[i] = 0;
    m->base = (unsigned long long)(uintptr_t)base;
    m->size = size;
}

int dm_diag_addr(unsigned long long addr, char *out, int n) {    for (int i = 0; i < g_nmods; i++) {
        unsigned long long b = g_mods[i].base;
        if (addr >= b && addr < b + g_mods[i].size) {
            int k = 0;
            for (const char *p = g_mods[i].name; *p && k < n - 1; p++) out[k++] = *p;
            if (k < n - 1) out[k++] = '+';
            char h[20]; int hk = 0;
            unsigned long long v = addr - b;
            if (!v) h[hk++] = '0';
            while (v) { unsigned d = (unsigned)(v & 0xF); h[hk++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
            if (k < n - 1) out[k++] = '0';
            if (k < n - 1) out[k++] = 'x';
            while (hk && k < n - 1) out[k++] = h[--hk];
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- 门调用记录 ---- */
#define DM_DIAG_GATE_N 4
static unsigned long long g_gate_ret[DM_DIAG_GATE_N];
static unsigned long long g_gate_total = 0;
static int                g_gate_pos = 0;

void dm_diag_gate(unsigned long long ret) {
    g_gate_total++;
    g_gate_ret[g_gate_pos] = ret;
    g_gate_pos = (g_gate_pos + 1) % DM_DIAG_GATE_N;
}

/* 把最近的门调用写成「模块+偏移」追加到 sb 里（最近的在最后） */
static void sb_gates(sb_t *b) {
    char nm[40];
    if (!g_gate_total) { sb_s(b, "（还没有门调用）"); return; }
    sb_s(b, "共 "); sb_dec(b, g_gate_total); sb_s(b, " 次，最近 ");
    unsigned n = (g_gate_total < (unsigned long long)DM_DIAG_GATE_N)
                 ? (unsigned)g_gate_total : DM_DIAG_GATE_N;
    for (unsigned i = 0; i < n; i++) {
        /* 从最老的开始打印 */
        int idx = (g_gate_pos - n + (int)i + DM_DIAG_GATE_N * 2) % DM_DIAG_GATE_N;
        unsigned long long v = g_gate_ret[idx];
        sb_s(b, " ");
        if (dm_diag_addr(v, nm, (int)sizeof nm)) sb_s(b, nm);
        else sb_hex(b, v);
    }
}

void dm_diag_hang_line(unsigned long long seconds, unsigned long long pc,
                       unsigned long long lr, const char *host_sym) {
    char buf[640];
    sb_t b = { buf, 0, (int)sizeof buf };
    char nm[40];
    sb_s(&b, "\n[看门狗] 主线程 ");
    sb_dec(&b, seconds);
    sb_s(&b, " 秒没有进展（帧循环不再前进）");
    if (pc || lr) {
        sb_s(&b, "\n[看门狗] 采样 PC ");
        if (pc && dm_diag_addr(pc, nm, (int)sizeof nm)) { sb_s(&b, nm); }
        else sb_hex(&b, pc);
        sb_s(&b, "  LR ");
        if (lr && dm_diag_addr(lr, nm, (int)sizeof nm)) { sb_s(&b, nm); }
        else sb_hex(&b, lr);
        if (host_sym && host_sym[0]) { sb_s(&b, "  （PC 在宿主里："); sb_s(&b, host_sym); sb_s(&b, "）"); }
    } else {
        sb_s(&b, "\n[看门狗] （没能采到 PC —— 线程可能已不存在）");
    }
    sb_s(&b, "\n[看门狗] 当前阶段: ");
    sb_s(&b, g_stage);
    sb_s(&b, "\n[看门狗] 最近门调用: ");
    sb_gates(&b);
    sb_s(&b, "\n");
    sb_c(&b, 0);
#if defined(__unix__) || defined(__APPLE__)
    if (g_logfd >= 0) { ssize_t w = write(g_logfd, buf, (size_t)b.n - 1); (void)w; }
#else
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
#endif
}

void dm_diag_crash_line(int sig, unsigned long long fault_addr,
                        unsigned long long pc, unsigned long long lr,
                        unsigned long long sp, const unsigned long long *x, int nx) {
    char buf[640];
    sb_t b = { buf, 0, (int)sizeof buf };
    char nm[40];

    sb_s(&b, "\n[崩溃] 信号 "); sb_dec(&b, (unsigned)sig);
    sb_s(&b, "  故障地址 ");    sb_hex(&b, fault_addr);
    sb_s(&b, "\n[崩溃] PC ");   sb_hex(&b, pc);
    if (dm_diag_addr(pc, nm, (int)sizeof nm)) { sb_s(&b, " ("); sb_s(&b, nm); sb_s(&b, ")"); }
    sb_s(&b, "  LR ");          sb_hex(&b, lr);
    if (dm_diag_addr(lr, nm, (int)sizeof nm)) { sb_s(&b, " ("); sb_s(&b, nm); sb_s(&b, ")"); }
    sb_s(&b, "  SP ");          sb_hex(&b, sp);

    if (x && nx > 0) {
        sb_s(&b, "\n[崩溃] 参数");
        for (int i = 0; i < nx; i++) { sb_s(&b, " x"); sb_dec(&b, (unsigned)i); sb_s(&b, "="); sb_hex(&b, x[i]); }
    }
    sb_s(&b, "\n[崩溃] 最后阶段: "); sb_s(&b, g_stage);
    sb_s(&b, "\n[崩溃] 最近门调用: "); sb_gates(&b);
    sb_s(&b, "\n");
    sb_c(&b, 0);

    /* 只有 write(2) —— 崩溃路径上唯一可靠的方式 */
#if defined(__unix__) || defined(__APPLE__)
    if (g_logfd >= 0) {
        ssize_t w = write(g_logfd, buf, (size_t)b.n - 1);
        (void)w;
    }
#else
    if (g_logfd >= 0) {
        /* Windows 上不会走这条路（没有真机崩溃），留个能编译的实现 */
        if (g_log) { fputs(buf, g_log); fflush(g_log); }
    }
#endif
}
