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
#define DM_DIAG_MAX_MODS 12
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

/* 第 75 轮：addr 落在某模块里时，返回「从这里到模块末尾还剩多少字节」；
 * 不落在任何模块里返回 0 —— 给 Break 的内存倒出当**读取长度上限**用，
 * 免得 96 字节的转储读到模块末尾之外（下一页可能没映射，诊断自己崩掉）。 */
unsigned long long dm_diag_addr_left(unsigned long long addr) {
    for (int i = 0; i < g_nmods; i++) {
        unsigned long long b = g_mods[i].base;
        if (addr >= b && addr < b + g_mods[i].size)
            return b + g_mods[i].size - addr;
    }
    return 0;
}

/* 第 79 轮：安全读 8 字节 —— 只在**已登记区间**里读，读不动就回 0。
 * 用途：Break 时按 x29 帧链回溯真正的调用者（见 hle.c 的 0x26 分支）。
 * 为什么不直接 deref：帧指针是客体给的值，指到未映射页就是「诊断自己把
 * 进程弄崩」，那以后什么也看不到了。宁可少读一帧。 */
int dm_diag_read_u64(unsigned long long addr, unsigned long long *out) {
    if (!addr || (addr & 7)) return 0;
    if (dm_diag_addr_left(addr) < 8) return 0;
    if (out) *out = *(volatile unsigned long long *)(uintptr_t)addr;
    return 1;
}

/* ---- 门调用记录 ---- */
#define DM_DIAG_GATE_N 4
static unsigned long long g_gate_ret[DM_DIAG_GATE_N];
static unsigned long long g_gate_total = 0;
static int                g_gate_pos = 0;

void dm_diag_gate(unsigned long long ret, unsigned long long site) {
    g_gate_total++;
    /* 第 80 轮：优先存「原指令地址」——返回点是门 thunk，四台一模一样的地址，
       翻成模块+偏移永远指在同一个小区域，看不出客体在跑什么。 */
    g_gate_ret[g_gate_pos] = site ? site : ret;
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
    char buf[1024];
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
    /* 第 83 轮：把最后一条诊断（例如 [os] AllocateMemoryBlock 的结果）印在崩溃块里。
     * 目的：真机照片只有 16 行、日志文件不一定方便拷，一行「最近事件」就能定位
     * 「我们的实现到底跑没跑、拿到的是指针还是 NULL」。 */
    sb_s(&b, "\n[崩溃] 最近事件: "); sb_s(&b, g_event);
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

/* 第 80 轮：崩溃时的客体栈回溯。
 *
 * !! 为什么需要 !!
 * 崩溃行里的 LR 是**门 thunk 里的地址**（客体调 svc 时 x30 被 thunk 拿去算门地址，
 * 我们的 _dm_gate 又在 blr 时把它覆盖成 thunk 的返回点）。于是 LR 永远指向
 * sdk+0xc7xxxx 那一簇 thunk，「谁调的」永远看不出来。
 * 真正没被破坏的调用链只有一处：**栈上保存的返回地址**。所以：
 *   ① 有帧指针就顺着 [fp] / [fp+8] 走；
 *   ② 另外把 SP 往上 0x600 字节扫一遍，凡是落在已登记模块里的值都当成候选返回地址。
 * 安全：只读 dm_diag 登记过的区间（读不动就停），输出只用 write(2)。
 * 绝不能在这里调 snprintf/malloc —— 崩溃路径上它们会二次崩溃，那就一条线索都没了。 */
void dm_diag_crash_bt(unsigned long long sp, unsigned long long fp) {
    char buf[1500];
    sb_t b = { buf, 0, (int)sizeof buf };
    char nm[40];

    sb_s(&b, "\n[崩溃] 栈回溯（SP="); sb_hex(&b, sp); sb_s(&b, "）");

    /* ① 帧链 */
    if (fp && dm_diag_addr_left(fp) >= 16) {
        unsigned long long cur = fp;
        for (int i = 0; i < 8 && cur; i++) {
            unsigned long long nx = 0, ra = 0;
            if (!dm_diag_read_u64(cur, &nx)) break;
            if (!dm_diag_read_u64(cur + 8, &ra)) break;
            sb_s(&b, "\n[崩溃] 帧"); sb_dec(&b, (unsigned)i); sb_s(&b, " 返回 ");
            if (dm_diag_addr(ra, nm, (int)sizeof nm)) sb_s(&b, nm); else sb_hex(&b, ra);
            if (nx <= cur) break;              /* 不前进就停（防环） */
            cur = nx;
        }
    }

    /* ② 扫栈 —— 帧链断了也能看出个大概（作用域按最近的作用域优先） */
    if (sp) {
        int shown = 0;
        sb_s(&b, "\n[崩溃] 栈扫描");
        for (unsigned long long a = sp; a < sp + 0x600 && shown < 18; a += 8) {
            unsigned long long v = 0;
            if (!dm_diag_read_u64(a, &v)) break;
            if (dm_diag_addr(v, nm, (int)sizeof nm)) {
                sb_s(&b, "\n[崩溃]   +"); sb_hex(&b, a - sp);
                sb_s(&b, " -> "); sb_s(&b, nm);
                shown++;
            }
        }
        if (!shown) sb_s(&b, "（栈上没有像返回地址的值）");
    }
    sb_s(&b, "\n");
    sb_c(&b, 0);

#if defined(__unix__) || defined(__APPLE__)
    if (g_logfd >= 0) { ssize_t w = write(g_logfd, buf, (size_t)b.n - 1); (void)w; }
#else
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
#endif
}

void dm_diag_append_crash_text(const char *s, int n) {
    if (!s || n <= 0) return;
#if defined(__unix__) || defined(__APPLE__)
    if (g_logfd >= 0) { ssize_t w = write(g_logfd, s, (size_t)n); (void)w; }
#else
    if (g_log) { fwrite(s, 1, (size_t)n, g_log); fflush(g_log); }
#endif
}
