/* 宿主图形层 —— NVN 命令录音机 + 着色器查找桥。
 *
 * 两类工作：
 *
 * 【一】状态与绘制类（约 300 个 NVN 函数）的处理办法
 *   按 D2R 的分法，这类函数「只改 GPU 状态、没有 CPU 可见结果」——
 *   所以**录成命令、提交时回放**（`nvnQueueSubmitCommands` 触发回放）。
 *   录音机是有容量上限的环形缓冲：满了就丢弃最旧的并计数，
 *   保证「卡顿可诊断」而不是「无声地丢帧」。
 *
 * 【二】着色器查找桥 —— 这是把**离线翻译**与**运行期**连起来的关键
 *   运行期：客体调 `nvnProgramInitialize(program, dev, code, control, ...)`
 *   离线：我们把着色器翻成 Metal，并按
 *         `key = sha256(控制段[0:0x880])[:16]` 命名成 `<key>.metal`
 *   桥：宿主在 `nvnProgramInitialize` 里**算同样的哈希**，查表得到 Metal 着色器。
 *   查表数据就是 `work/shaders2/shader_map.tsv`（126 条，每条 <名字, 阶段, key, ...>）。
 *
 * 哈希算法必须与改写器/离线翻译**逐字节一致**，否则查不到 —— 所以这里
 * 用同一套 SHA-256 实现（下面自带一个，不依赖平台库，便于跨平台一致）。
 */
#include "hle_nvn.h"
#include "hle_nvn_obj.h"
#include "hle_nvn_cmd.h"
#include "hle_nvn_replay.h"      /* dm_cmd_view */
#include "hle_nvn_obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================================================================== */
/* SHA-256（自带，保证与离线侧逐字节一致）                              */
/* ==================================================================== */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR32(x,n) (((x) >> (n)) | ((x) << (32-(n))))

static void sha256_block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t a = w[i-15], b = w[i-2];
        uint32_t s0 = ROR32(a,7) ^ ROR32(a,18) ^ (a >> 3);
        uint32_t s1 = ROR32(b,17) ^ ROR32(b,19) ^ (b >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],
             e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}
static void sha256_init(sha256_t *s) {
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->len = 0; s->n = 0;
}
static void sha256_update(sha256_t *s, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += len;
    while (len) {
        size_t take = 64 - s->n;
        if (take > len) take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) { sha256_block(s, s->buf); s->n = 0; }
    }
}
static void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha256_update(s, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i*8));
    sha256_update(s, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24); out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);  out[i*4+3] = (uint8_t)s->h[i];
    }
}

/* 与离线侧同口径：key = sha256(控制段[0:0x880]).hexdigest()[:16]
 *
 * !! 踩过的坑 !! 离线的 Python 里 `[:16]` 是**前 16 个十六进制字符**（= 前 8 字节），
 * 不是前 16 字节。我一开始按 32 字符（16 字节）写，与表里的 16 字符 key 对不上，
 * 结果映射表载入 0 条。
 * 实测核对（ctrl@6176）：
 *   sha256 = 9db31a56d99078db7ac6d44d2a74b2de…
 *   表里 key = 9db31a56d99078db        ← 前 8 字节的十六进制
 */
void dm_nvn_shader_key(const void *control, char out_hex17[17]) {
    uint8_t d[32];
    sha256_t s; sha256_init(&s);
    sha256_update(&s, control, 0x880);
    sha256_final(&s, d);
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out_hex17[i*2]   = H[d[i] >> 4];
        out_hex17[i*2+1] = H[d[i] & 0xF];
    }
    out_hex17[16] = 0;
}

/* ==================================================================== */
/* 着色器查找桥                                                          */
/* ==================================================================== */

#define DM_MAX_SHADERS 512

typedef struct {
    char key[17];
    char name[64];
    char stage[4];
    char metal_path[512];     /* 对应的 .metal 文件路径 */
} dm_shader_ent;

static dm_shader_ent g_shaders[DM_MAX_SHADERS];
static unsigned      g_nshaders = 0;
static char          g_metal_dir[512] = {0};

static struct {
    uint64_t n_prog_init, n_key_hit, n_key_miss;
    uint64_t n_map_rows, n_metal_files;
} g_sh_stat;
const void *dm_nvn_shader_stats_ptr(void) { return &g_sh_stat; }

/* 载入 shader_map.tsv 与 Metal 目录。
   格式：name \t stage \t key \t sph_at \t ctrl_at \t instr_len \t c1_len \t path \t … */
int dm_nvn_shader_load(const char *map_tsv, const char *metal_dir) {
    FILE *f = fopen(map_tsv, "r");
    if (!f) return -1;
    if (metal_dir) snprintf(g_metal_dir, sizeof g_metal_dir, "%s", metal_dir);
    char line[2048];
    g_nshaders = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (g_nshaders >= DM_MAX_SHADERS) break;
        /* 拆 tab */
        char *fields[12] = {0};
        unsigned nf = 0;
        char *p = line;
        while (nf < 12 && p) {
            fields[nf++] = p;
            char *t = strchr(p, '\t');
            if (t) { *t = 0; p = t + 1; } else p = NULL;
        }
        if (nf < 3) continue;
        dm_shader_ent *e = &g_shaders[g_nshaders];
        snprintf(e->name,  sizeof e->name,  "%s", fields[0]);
        snprintf(e->stage, sizeof e->stage, "%s", fields[1]);
        snprintf(e->key,   sizeof e->key,   "%s", fields[2]);
        if (g_metal_dir[0])
            snprintf(e->metal_path, sizeof e->metal_path, "%s/%s.metal", g_metal_dir, e->key);
        else
            e->metal_path[0] = 0;
        if (strlen(e->key) == 16) g_nshaders++;
    }
    fclose(f);
    g_sh_stat.n_map_rows = g_nshaders;
    return (int)g_nshaders;
}

unsigned dm_nvn_shader_count(void) { return g_nshaders; }

/* 按 key 查（32 字符十六进制） */
const void *dm_nvn_shader_find(const char *key) {
    if (!key) return NULL;
    for (unsigned i = 0; i < g_nshaders; i++)
        if (!strcmp(g_shaders[i].key, key)) return &g_shaders[i];
    return NULL;
}

const char *dm_nvn_shader_metal_path(const void *ent) {
    const dm_shader_ent *e = (const dm_shader_ent *)ent;
    return (e && e->metal_path[0]) ? e->metal_path : NULL;
}
const char *dm_nvn_shader_name(const void *ent) {
    const dm_shader_ent *e = (const dm_shader_ent *)ent;
    return e ? e->name : NULL;
}
const char *dm_nvn_shader_stage(const void *ent) {
    const dm_shader_ent *e = (const dm_shader_ent *)ent;
    return e ? e->stage : NULL;
}

/* nvnProgramInitialize(program, dev, code, control, ...)
   —— 桥的核心：算 key，查 Metal 着色器。
   `code` 是 SPH（0x50 字节）+ 指令；`control` 是控制段（0x880 字节）。 */
const void *dm_nvn_ProgramInitialize(void *program, const void *dev,
                                     const void *code, const void *control) {
    (void)program; (void)dev; (void)code;
    g_sh_stat.n_prog_init++;
    if (!control) { g_sh_stat.n_key_miss++; return NULL; }
    char key[17];
    dm_nvn_shader_key(control, key);
    const void *e = dm_nvn_shader_find(key);
    if (e) g_sh_stat.n_key_hit++; else g_sh_stat.n_key_miss++;
    return e;      /* 宿主把找到的 Metal 着色器挂在 program 上（真机侧） */
}

/* ==================================================================== */
/* 命令录音机（状态与绘制类）                                            */
/* ==================================================================== */

#define DM_CMD_RING 65536

typedef struct {
    uint32_t id;         /* NVN 名字在表里的下标 */
    uint64_t a[4];       /* 前 4 个参数（够覆盖绝大多数绘制类） */
} dm_cmd_t;

static dm_cmd_t     g_ring[DM_CMD_RING];
static unsigned     g_head = 0, g_n = 0;
static int          g_recording = 1;
static struct {
    uint64_t n_recorded, n_dropped, n_replayed;
    uint64_t n_draw, n_bind, n_state, n_clear;
} g_cmd_stats;
const void *dm_nvn_cmd_stats_ptr(void) { return &g_cmd_stats; }

void dm_nvn_cmd_enable(int on) { g_recording = on ? 1 : 0; }

/* 命令类实际注册上的个数（用于自测与诊断）。
   注意：C 里必须先声明后用 —— 上一版把这个定义放在 register_all 之后，
   MSVC 报 C2065「未声明的标识符」。 */
static unsigned g_cmd_registered = 0;
unsigned dm_nvn_cmd_registered(void) { return g_cmd_registered; }

void dm_nvn_cmd_record(int idx, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    g_cmd_stats.n_recorded++;
    if (!g_recording) return;
    if (g_n == DM_CMD_RING) {
        /* 环满：丢最旧的，并计数（让「掉帧/丢命令」可诊断，而不是无声） */
        g_head = (g_head + 1) % DM_CMD_RING;
        g_n--;
        g_cmd_stats.n_dropped++;
    }
    unsigned slot = (g_head + g_n) % DM_CMD_RING;
    g_ring[slot].id = (uint32_t)idx;
    g_ring[slot].a[0] = a0; g_ring[slot].a[1] = a1;
    g_ring[slot].a[2] = a2; g_ring[slot].a[3] = a3;
    g_n++;
}

unsigned dm_nvn_cmd_pending(void) { return g_n; }

/* 回放：真机侧这里是「把录下的命令编成 Metal 命令缓冲」。
   TODO（图形输出）：宿主目前只统计回放条数，不产生画面。
   返回回放条数。 */
unsigned dm_nvn_cmd_replay(void) {
    unsigned done = g_n;
    g_cmd_stats.n_replayed += done;
    g_head = 0; g_n = 0;
    return done;
}

/* ---------------- 回放视图 ----------------
 * 回放器需要「按录制顺序」读到 idx 与 4 个参数。
 * 环形缓冲的顺序是 head..head+n-1（取模），环形本身不保证连续，
 * 所以这里展开成两份平行数组再交给回放器。
 *
 * 为什么不用「回放时逐条取」的迭代器：展开一次是 O(n) 且 n 有上界
 * （DM_CMD_RING = 65536），比让回放器理解环形索引简单也更不易错。
 * 代价是两份静态数组（65536×(4+32) ≈ 2.3 MB）—— 只在回放瞬间用，可接受。
 * （真机侧若嫌大，可改成让回放器直接按 slot 取。） */
static int      g_view_idx[DM_CMD_RING];
static uint64_t g_view_a[DM_CMD_RING][4];

int dm_nvn_cmd_begin_replay(dm_cmd_view *v) {
    if (!v) return 0;
    unsigned n = g_n;
    for (unsigned k = 0; k < n; k++) {
        unsigned slot = (g_head + k) % DM_CMD_RING;
        g_view_idx[k]  = (int)g_ring[slot].id;
        g_view_a[k][0] = g_ring[slot].a[0];
        g_view_a[k][1] = g_ring[slot].a[1];
        g_view_a[k][2] = g_ring[slot].a[2];
        g_view_a[k][3] = g_ring[slot].a[3];
    }
    v->n   = n;
    v->idx = g_view_idx;
    v->a   = (const uint64_t (*)[4])g_view_a;
    return 1;
}

void dm_nvn_cmd_end_replay(void) {
    g_cmd_stats.n_replayed += g_n;
    g_head = 0;
    g_n = 0;
}

/* 分类计数（供诊断：看游戏主要在做什么） */
void dm_nvn_cmd_note(int idx, int kind) {
    (void)idx;
    switch (kind) {
    case 0: g_cmd_stats.n_draw++;  break;
    case 1: g_cmd_stats.n_bind++;  break;
    case 2: g_cmd_stats.n_state++; break;
    case 3: g_cmd_stats.n_clear++; break;
    default: break;
    }
}

/* 通用命令入口：所有「状态与绘制类」NVN 都接到这里。
 *
 * 为什么用一个通用入口而不是给每个函数写实现：
 *   这一类的语义就是「改 GPU 状态 / 发绘制」，在宿主侧**没有 CPU 可见结果** ——
 *   D2R 的做法也是「录成命令、提交时回放」。所以统一录音 + 提交时按 idx 回放，
 *   比给 300 个函数各写一份空实现更省、也更不容易漏。
 *
 * !! 但「命令身份」必须是 idx，不能丢 !! —— 这是上一版的设计缺陷：
 *   真机侧由跳板把 idx 传进来（D2R 的 nvn_thunk 就是这么做的），
 *   而**这个通用入口拿不到 idx**（C 的可变参数也拿不到「谁调了我」）。
 *   上一版记 -1，等于把 300 个函数录成同一种命令 —— 回放时无法区分绑定与绘制。
 *
 *   正解：**给每个名字注册一个「只记自己 idx」的薄包装** ——
 *   dm_nvn_cmd_register_all 就是这么做的。本函数保留为「身份未知」的兜底，
 *   记 DM_CMD_IDX_UNKNOWN 而不是假装知道。 */
void dm_nvn_cmd_generic(void *a0, void *a1, void *a2, void *a3) {
    dm_nvn_cmd_record(DM_CMD_IDX_UNKNOWN,
                      (uint64_t)(uintptr_t)a0, (uint64_t)(uintptr_t)a1,
                      (uint64_t)(uintptr_t)a2, (uint64_t)(uintptr_t)a3);
}

/* 给某个 idx 生成薄包装 —— 「不丢身份」的正解。
   真机侧与 Windows 自测都走这里。
   用宏展开出固定数量（不做运行期生成机器码 —— 那在 iOS 上不可行，
   JIT 页面需要 MAP_JIT 且与代码签名冲突）。 */
#define DM_CMD_WRAP_MAX 32
static int g_wrap_idx[DM_CMD_WRAP_MAX];
static int g_wrap_used = 0;
static void wrap_body(int w, void *a0, void *a1, void *a2, void *a3) {
    dm_nvn_cmd_record(g_wrap_idx[w],
                      (uint64_t)(uintptr_t)a0, (uint64_t)(uintptr_t)a1,
                      (uint64_t)(uintptr_t)a2, (uint64_t)(uintptr_t)a3);
}
#define WD(n) static void wrap_##n(void*a,void*b,void*c,void*d){wrap_body(n,a,b,c,d);}
WD(0)  WD(1)  WD(2)  WD(3)  WD(4)  WD(5)  WD(6)  WD(7)
WD(8)  WD(9)  WD(10) WD(11) WD(12) WD(13) WD(14) WD(15)
WD(16) WD(17) WD(18) WD(19) WD(20) WD(21) WD(22) WD(23)
WD(24) WD(25) WD(26) WD(27) WD(28) WD(29) WD(30) WD(31)
static void *const g_wrap_fn[DM_CMD_WRAP_MAX] = {
    (void *)wrap_0,  (void *)wrap_1,  (void *)wrap_2,  (void *)wrap_3,
    (void *)wrap_4,  (void *)wrap_5,  (void *)wrap_6,  (void *)wrap_7,
    (void *)wrap_8,  (void *)wrap_9,  (void *)wrap_10, (void *)wrap_11,
    (void *)wrap_12, (void *)wrap_13, (void *)wrap_14, (void *)wrap_15,
    (void *)wrap_16, (void *)wrap_17, (void *)wrap_18, (void *)wrap_19,
    (void *)wrap_20, (void *)wrap_21, (void *)wrap_22, (void *)wrap_23,
    (void *)wrap_24, (void *)wrap_25, (void *)wrap_26, (void *)wrap_27,
    (void *)wrap_28, (void *)wrap_29, (void *)wrap_30, (void *)wrap_31,
};

/* 给名字注册一个带 idx 的包装。返回 1 = 成功。 */
int dm_nvn_cmd_register_wrapped(const char *name) {
    int idx = dm_nvn_index_of(name);
    if (idx < 0) return 0;
    if (g_wrap_used >= DM_CMD_WRAP_MAX) return 0;
    int w = g_wrap_used++;
    g_wrap_idx[w] = idx;
    return dm_nvn_register(name, g_wrap_fn[w]);
}

/* 注册命令类实现：把一批名字接到「录音」入口。 */
void dm_nvn_cmd_register_all(void) {
    static const char *names[] = {
        /* 绘制 */
        "nvnCommandBufferDrawElements", "nvnCommandBufferDrawArrays",
        "nvnCommandBufferDrawElementsBaseVertex", "nvnCommandBufferDrawArraysInstanced",
        "nvnCommandBufferDrawElementsInstanced",
        /* 清除 */
        "nvnCommandBufferClearColor", "nvnCommandBufferClearDepthStencil",
        "nvnCommandBufferClearTexture",
        /* 视口/裁剪 */
        "nvnCommandBufferSetViewport", "nvnCommandBufferSetScissor",
        "nvnCommandBufferSetDepthRange",
        /* 绑定 */
        "nvnCommandBufferBindVertexBuffer", "nvnCommandBufferBindVertexBuffers",
        "nvnCommandBufferBindIndexBuffer", "nvnCommandBufferBindUniformBuffer",
        "nvnCommandBufferBindTexture", "nvnCommandBufferBindTextures",
        "nvnCommandBufferBindSampler", "nvnCommandBufferBindProgram",
        "nvnCommandBufferBindBlendState", "nvnCommandBufferBindDepthStencilState",
        "nvnCommandBufferBindRasterizerState", "nvnCommandBufferBindVertexAttribState",
        "nvnCommandBufferBindVertexStreamState", "nvnCommandBufferBindColorState",
        /* 同步类（表里存在的会被注册，不存在的自动跳过） */
        "nvnCommandBufferBarrier", "nvnCommandBufferSignalEvent",
        "nvnCommandBufferWaitEvent", "nvnCommandBufferWaitSync", "nvnCommandBufferSignalSync",
        NULL
    };
    /* !! 注意 !! `dm_nvn_register` 对**表里没有的名字会静默忽略** ——
       上面这 25 个名字里只有一部分真的在 538 名的表里。
       所以统计「实际注册上几个」，而不是假定全部成功。 */
    g_cmd_registered = 0;
    for (int i = 0; names[i]; i++) {
        /* 用**带 idx 的包装**而不是通用入口 —— 否则回放时无法区分命令（见上） */
        if (!dm_nvn_cmd_register_wrapped(names[i])) continue;
        g_cmd_registered++;
        if (strstr(names[i], "Draw"))       dm_nvn_cmd_note(0, 0);
        else if (strstr(names[i], "Clear")) dm_nvn_cmd_note(0, 3);
        else if (strstr(names[i], "Bind"))  dm_nvn_cmd_note(0, 1);
        else                                dm_nvn_cmd_note(0, 2);
    }
}
