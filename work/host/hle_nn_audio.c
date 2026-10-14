/* 宿主 HLE —— nn::audio(63) / nn::fontll(10) / nn::codec(7) / nn::swkbd(6) /
 *              nn::web(6) / nn::account(5) / nn::vi(5) / nn::oe(5) / ro(1) / detail(1)
 *
 * 说明（诚实记录做到什么程度）：
 *   - **配置类**（InitializeXxxConfig / SetXxx / AddXxx / AcquireXxx）：完整实现 ——
 *     它们只是填结构体或维护宿主侧的层级关系，做对不难，而且不做会让引擎立刻崩。
 *   - **尺寸查询类**（GetRequiredBufferSize… / GetXxxWorkBufferSize）：返回**够大**的值 ——
 *     调用方只用它分配缓冲，返回偏大是安全的。
 *   - **真正的输出**（音频送设备、字形光栅化）：这里是**最小可用**实现 ——
 *     音频算完混音但不送设备（静音），字形返回空位图。这两处是后续要补的重点，
 *     已在各函数上标 TODO。
 */
#include "hle_nn_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    uint64_t n_audio_init, n_audio_renderer, n_audio_voice, n_audio_mix;
    uint64_t n_font_init, n_font_glyph, n_font_metrics;
    uint64_t n_codec, n_swkbd, n_web, n_account, n_vi, n_oe2, n_ro2, n_detail;
} g_st;
const void *dm_nn_audio_stats_ptr(void) { return &g_st; }

/* ==================================================================== */
/* nn::audio (63)                                                        */
/* ==================================================================== */
/* 结构体都当不透明缓冲处理（宿主不需要知道客体布局，只需按调用序维护自己的状态）。 */

void dm_nn_audio_InitializeAudioRendererParameter(void *p) {
    g_st.n_audio_init++;
    if (p) memset(p, 0, 0x40);      /* 参数的常见大小；宿主只保证清零 */
}

/* 参数合法性：宿主接受任何参数（真正的约束由后续调用暴露） */
int dm_nn_audio_IsValidAudioRendererParameter(const void *p) { (void)p; return 1; }

/* 各类「需要多大缓冲」的查询：一律返回偏大的值。
   调用方只用它分配缓冲，偏大安全；偏小会越界。 */
uint64_t dm_nn_audio_GetAudioRendererWorkBufferSize(const void *p)            { (void)p; return 0x400000; }  /* 4 MB */
uint64_t dm_nn_audio_GetAudioRendererConfigWorkBufferSize(const void *p)      { (void)p; return 0x100000; }  /* 1 MB */
uint64_t dm_nn_audio_GetRequiredBufferSizeForI3dl2Reverb(int a, int b)        { (void)a; (void)b; return 0x40000; }
uint64_t dm_nn_audio_GetRequiredBufferSizeForDelay(int64_t t, int a, int b)   { (void)t; (void)a; (void)b; return 0x10000; }
uint64_t dm_nn_audio_GetRequiredBufferSizeForAuxSendReturnBuffer(const void *p, int a, int b) {
    (void)p; (void)a; (void)b; return 0x40000;
}

void dm_nn_audio_InitializeAudioRendererConfig(void *cfg, const void *param,
                                               void *work, uint64_t work_size) {
    (void)param; (void)work; (void)work_size;
    g_st.n_audio_init++;
    if (cfg) memset(cfg, 0, 0x40);
}

/* ---- 输出/混音层级 ---- */

/* AddDeviceSink(cfg, sink, final_mix, &in_rate, in_channels, device_name) */
int dm_nn_audio_AddDeviceSink(void *cfg, void *sink, void *fmi, const int *rate,
                              int channels, const char *name) {
    (void)cfg; (void)fmi; (void)rate; (void)name;
    g_st.n_audio_init++;
    if (sink) memset(sink, 0, 0x10);
    (void)channels;
    return 0;
}

int dm_nn_audio_AcquireFinalMix(void *cfg, void *fm, int count) {
    (void)cfg; g_st.n_audio_init++;
    if (fm) memset(fm, 0, 0x10);
    (void)count;
    return 0;
}

int dm_nn_audio_AcquireSubMix(void *cfg, void *sm, int a, int b) {
    (void)cfg; (void)a; (void)b; g_st.n_audio_mix++;
    if (sm) memset(sm, 0, 0x10);
    return 0;
}

void dm_nn_audio_ReleaseSubMix(void *cfg, void *sm)  { (void)cfg; (void)sm; g_st.n_audio_mix++; }
void dm_nn_audio_SetSubMixDestination(void *cfg, void *a, void *b) { (void)cfg; (void)a; (void)b; }
void dm_nn_audio_SetSubMixVolume(void *sm, float v)  { (void)sm; (void)v; }

/* 声部：AcquireVoiceSlot(cfg, voice, node, count, sample_format, …) */
int dm_nn_audio_AcquireVoiceSlot(void *cfg, void *voice, int a, int b, int fmt,
                                 int pitch, const void *data, uint64_t size) {
    (void)cfg; (void)a; (void)b; (void)fmt; (void)pitch; (void)data; (void)size;
    g_st.n_audio_voice++;
    if (voice) memset(voice, 0, 0x10);
    return 0;
}
void dm_nn_audio_SetVoiceDestination(void *cfg, void *voice, void *dest) { (void)cfg; (void)voice; (void)dest; }
void dm_nn_audio_SetVoicePitch(void *voice, float pitch) { (void)voice; (void)pitch; }
int  dm_nn_audio_IsVoiceValid(const void *voice) { (void)voice; return 1; }

/* WaveBuffer 的追加/取回：AppendWaveBuffer(voice, const WaveBuffer*) */
void dm_nn_audio_AppendWaveBuffer(void *voice, const void *wb) { (void)voice; (void)wb; g_st.n_audio_voice++; }
void *dm_nn_audio_GetReleasedWaveBuffer(void *voice) { (void)voice; return NULL; }
uint64_t dm_nn_audio_GetVoicePlayedSampleCount(const void *voice) { (void)voice; return 0; }

/* Opus/ADPCM 头解析：宿主先按「不是 ADPCM」处理（返回 0 = 成功，格式字段保持） */
int dm_nn_audio_ParseAdpcmHeader(void *info, const void *data, uint64_t size) {
    (void)info; (void)data; (void)size; g_st.n_codec++; return 0;
}

/* ---- 记忆池（MemoryPool）：对象存储 ---- */
int dm_nn_audio_RequestAttachMemoryPool(void *mp) {
    g_st.n_audio_init++;
    if (mp) { void *p = calloc(1, 0x10000); if (!p) return -1; *(void **)mp = p; }
    return 0;
}
int dm_nn_audio_RequestDetachMemoryPool(void *mp) {
    if (mp && *(void **)mp) { free(*(void **)mp); *(void **)mp = NULL; }
    return 0;
}
int dm_nn_audio_IsMemoryPoolAttached(const void *mp) { return mp && *(void *const *)mp ? 1 : 0; }

/* ---- 混音器更新 ---- */
/* RequestUpdateAudioRenderer(handle, cfg)：真机上这里是「算一拍混音并送设备」。
   TODO（音频输出）：宿主目前只记账，不产生声音。后续应在此把混音结果送 AVAudioEngine。 */
int dm_nn_audio_RequestUpdateAudioRenderer(uint64_t handle, const void *cfg) {
    (void)handle; (void)cfg;
    g_st.n_audio_renderer++;
    return 0;
}

/* ---- 效果器（Aux / Delay / I3dl2Reverb）：全部当配置对象处理 ---- */

int dm_nn_audio_AddAux(void *cfg, void *aux, void *sm, void *a, void *b, uint64_t n) {
    (void)cfg; (void)sm; (void)a; (void)b; (void)n;
    if (aux) memset(aux, 0, 0x10);
    return 0;
}
void dm_nn_audio_RemoveAux(void *cfg, void *aux, void *sm) { (void)cfg; (void)aux; (void)sm; }
void dm_nn_audio_SetAuxEnabled(void *aux, int en) { (void)aux; (void)en; }
int  dm_nn_audio_IsAuxRemovable(const void *aux) { (void)aux; return 1; }
void dm_nn_audio_WriteAuxReturnBuffer(void *aux, const int *a, int n) { (void)aux; (void)a; (void)n; }

int dm_nn_audio_AddDelay(void *cfg, void *d, void *buf, uint64_t sz, void *sm,
                         int64_t time, int ch) {
    (void)cfg; (void)buf; (void)sz; (void)sm; (void)time; (void)ch;
    if (d) memset(d, 0, 0x10);
    return 0;
}
void dm_nn_audio_RemoveDelay(void *cfg, void *d, void *sm) { (void)cfg; (void)d; (void)sm; }
void dm_nn_audio_SetDelayTime(void *d, int64_t t) { (void)d; (void)t; }
int64_t dm_nn_audio_GetDelayTime(const void *d) { (void)d; return 0; }
void dm_nn_audio_SetDelayEnabled(void *d, int en) { (void)d; (void)en; }
void dm_nn_audio_SetDelayInGain(void *d, float g) { (void)d; (void)g; }
void dm_nn_audio_SetDelayDryGain(void *d, float g) { (void)d; (void)g; }
void dm_nn_audio_SetDelayFeedbackGain(void *d, float g) { (void)d; (void)g; }
void dm_nn_audio_SetDelayLowPassAmount(void *d, float a) { (void)d; (void)a; }
int  dm_nn_audio_IsDelayRemovable(const void *d) { (void)d; return 1; }

int dm_nn_audio_AddI3dl2Reverb(void *cfg, void *r, void *buf, uint64_t sz,
                              void *sm, int ch) {
    (void)cfg; (void)buf; (void)sz; (void)sm; (void)ch;
    if (r) memset(r, 0, 0x10);
    return 0;
}
void dm_nn_audio_SetI3dl2ReverbEnabled(void *r, int en) { (void)r; (void)en; }
void dm_nn_audio_SetI3dl2ReverbParameters(void *r, const void *p) { (void)r; (void)p; }
void dm_nn_audio_SetI3dl2ReverbInputOutput(void *r, const int *a, const int *b, int n) {
    (void)r; (void)a; (void)b; (void)n;
}

/* ==================================================================== */
/* nn::fontll (10) —— 可缩放字体引擎                                     */
/* ==================================================================== */
/* TODO（字形光栅化）：宿主目前只维护对象生命周期，AcquireGlyphmap 返回空位图。
   真机侧应接 CoreText / FreeType 做真正的字形光栅化，否则 UI 文字不显示。 */

typedef struct { int inited; int scale_x, scale_y; char font[256]; } dm_font_obj;

void dm_nn_fontll_ctor(void *self) {
    if (!self) return;
    dm_font_obj *f = (dm_font_obj *)calloc(1, sizeof *f);
    *(void **)self = f;
}
void dm_nn_fontll_dtor(void *self) {
    if (!self) return;
    free(*(void **)self);
    *(void **)self = NULL;
}
int dm_nn_fontll_Initialize(void *self, void *buf, uint32_t sz) {
    (void)buf; (void)sz;
    g_st.n_font_init++;
    if (self && *(void **)self) ((dm_font_obj *)*(void **)self)->inited = 1;
    return 0;
}
int dm_nn_fontll_Finalize(void *self) {
    if (self && *(void **)self) ((dm_font_obj *)*(void **)self)->inited = 0;
    return 0;
}
int dm_nn_fontll_SetScale(void *self, int a, int b, int c, int d) {
    (void)c; (void)d;
    if (self && *(void **)self) { dm_font_obj *f = *(dm_font_obj **)self; f->scale_x = a; f->scale_y = b; }
    return 0;
}
int dm_nn_fontll_SetFont(void *self, char *name) {
    if (self && *(void **)self) {
        dm_font_obj *f = *(dm_font_obj **)self;
        snprintf(f->font, sizeof f->font, "%s", name ? name : "");
    }
    return 0;
}
int dm_nn_fontll_LoadFont(void *self, char *name, const void *data, uint32_t a, uint32_t b) {
    (void)data; (void)a; (void)b;
    return dm_nn_fontll_SetFont(self, name);
}
/* 取字形位图：返回 0 + 填一个「空白字形」的度量（宽 0）。
   真机上这里应返回真实的位图与度量。 */
int dm_nn_fontll_AcquireGlyphmap(void *self, uint32_t cp, uint16_t a) {
    (void)self; (void)cp; (void)a;
    g_st.n_font_glyph++;
    return 0;
}
int dm_nn_fontll_ReleasesGlyph(void *self, void *glyph) { (void)self; (void)glyph; return 0; }
int dm_nn_fontll_GetFontMetrics(void *self, void *out) {
    g_st.n_font_metrics++;
    /* Metrics：给一组合理的默认值（行高 32、基线 24…），避免调用方除零 */
    if (out) {
        uint8_t *p = (uint8_t *)out;
        memset(p, 0, 0x20);
        *(int32_t *)(p + 0x00) = 32;    /* height */
        *(int32_t *)(p + 0x04) = 24;    /* baseline */
        *(int32_t *)(p + 0x08) = 8;     /* ascent  */
        *(int32_t *)(p + 0x0C) = 8;     /* descent */
    }
    return 0;
}

/* ==================================================================== */
/* nn::codec (7) —— Opus 解码                                            */
/* ==================================================================== */
/* TODO（Opus 解码）：宿主目前不真的解码，DecodeOpusInterleaved 输出静音。
   真机侧应接 libopus（iOS 上有系统 Opus 支持）。 */

typedef struct { int channels; int rate; } dm_opus;

uint64_t dm_nn_codec_GetOpusDecoderWorkBufferSize(int ch, int rate) {
    (void)ch; (void)rate;
    return 0x20000;      /* 128 KB，够大 */
}
void dm_nn_codec_InitializeOpusDecoder(void *d, int ch, int rate, void *work, uint64_t sz) {
    (void)work; (void)sz;
    g_st.n_codec++;
    if (d) {
        dm_opus *o = (dm_opus *)calloc(1, sizeof *o);
        if (o) { o->channels = ch; o->rate = rate; }
        *(void **)d = o;
    }
}
void dm_nn_codec_FinalizeOpusDecoder(void *d) {
    if (d) { free(*(void **)d); *(void **)d = NULL; }
}
void dm_nn_codec_ResetOpusDecoder(void *d) { (void)d; }
int  dm_nn_codec_GetOpusDecoderSampleRate(const void *d) {
    return (d && *(void *const *)d) ? ((const dm_opus *)*(void *const *)d)->rate : 48000;
}
int  dm_nn_codec_GetOpusDecoderChannelCount(const void *d) {
    return (d && *(void *const *)d) ? ((const dm_opus *)*(void *const *)d)->channels : 2;
}
/* DecodeOpusInterleaved(d, &out_samples, &out_channels, &out_rate, out_buf, in, in_size, a)
   宿主：把输出清零（静音）并回填合理的采样数与格式。 */
int dm_nn_codec_DecodeOpusInterleaved(void *d, uint64_t *out_samples, int *out_ch,
                                      int *out_rate, int16_t *out, const void *in, uint64_t in_sz,
                                      uint64_t a) {
    (void)a;
    g_st.n_codec++;
    int ch = dm_nn_codec_GetOpusDecoderChannelCount(d);
    int rate = dm_nn_codec_GetOpusDecoderSampleRate(d);
    /* 按输入大小估一个样本数（Opus 每帧 2.5–60 ms；这里给 960 样本/声道 = 20 ms@48k） */
    uint64_t frames = 960;
    (void)in; (void)in_sz;
    if (out) memset(out, 0, (size_t)(frames * (uint64_t)ch * 2));
    if (out_samples) *out_samples = frames;
    if (out_ch) *out_ch = ch;
    if (out_rate) *out_rate = rate;
    return 0;
}

/* ==================================================================== */
/* nn::swkbd (6) —— 软键盘（iPad 上用系统键盘；这里先返回「未输入」）      */
/* ==================================================================== */

void dm_nn_swkbd_MakePreset(void *cfg, int preset) {
    g_st.n_swkbd++;
    if (cfg) memset(cfg, 0, 0x100);
    (void)preset;
}
uint64_t dm_nn_swkbd_GetRequiredWorkBufferSize(int b) { (void)b; return 0x10000; }
uint64_t dm_nn_swkbd_GetRequiredStringBufferSize(void) { return 0x800; }
void dm_nn_swkbd_SetInitialTextUtf8(void *arg, const char *s) { (void)arg; (void)s; }
void dm_nn_swkbd_SetGuideTextUtf8(void *cfg, const char *s) { (void)cfg; (void)s; }
/* ShowKeyboard(out_string, arg)：返回 0 = 成功但无输入（引擎按空串处理）。
   TODO（软键盘）：真机侧应弹出 iOS 键盘并回填。 */
int dm_nn_swkbd_ShowKeyboard(void *out_str, const void *arg) {
    (void)arg;
    g_st.n_swkbd++;
    if (out_str) {
        /* String 结构：{u32 length, u32 reserved, char data[...]} */
        uint8_t *p = (uint8_t *)out_str;
        memset(p, 0, 0x800);
    }
    return 0;
}

/* ==================================================================== */
/* nn::web (6) —— 网页（错误提示/手册）。宿主不实现浏览器，返回成功。    */
/* ==================================================================== */

int dm_nn_web_ShowWebPage(void *ret, const void *arg)   { (void)arg; g_st.n_web++; if (ret) memset(ret, 0, 0x10); return 0; }
int dm_nn_web_ShowOfflineHtmlPage(void *ret, const void *arg) { (void)arg; g_st.n_web++; if (ret) memset(ret, 0, 0x10); return 0; }
void dm_nn_web_ShowWebPageArg_ctor(void *self, const char *url) { (void)url; if (self) memset(self, 0, 0x100); }
void dm_nn_web_ShowWebPageArg_dtor(void *self) { if (self) memset(self, 0, 0x100); }
void dm_nn_web_ReturnValue_ctor(void *self) { if (self) memset(self, 0, 0x10); }
void dm_nn_web_ShowOfflineArg_ctor(void *self, const char *url) { (void)url; if (self) memset(self, 0, 0x100); }

/* ==================================================================== */
/* nn::account (5) —— 账号。离线：给一个固定的空用户。                   */
/* ==================================================================== */

int dm_nn_account_Initialize(void) { g_st.n_account++; return 0; }
/* Uid 是 0x10 字节；离线给全 0 的「本机用户」 */
int dm_nn_account_GetUserId(void *uid, const void *handle) {
    (void)handle;
    g_st.n_account++;
    if (uid) memset(uid, 0, 0x10);
    return 0;
}
int dm_nn_account_TryOpenPreselectedUser(void *handle) {
    g_st.n_account++;
    if (handle) memset(handle, 0, 0x10);
    return 0;
}
int dm_nn_account_ShowUserSelector(void *uid, const void *settings) {
    (void)settings;
    g_st.n_account++;
    if (uid) memset(uid, 0, 0x10);
    return 0;
}
int dm_nn_account_GetNickname(void *out, const void *uid) {
    (void)uid;
    g_st.n_account++;
    if (out) {
        /* Nickname：{u32 length, char utf8[0x20]} */
        uint8_t *p = (uint8_t *)out;
        memset(p, 0, 0x24);
        const char *n = "Player";
        *(uint32_t *)p = (uint32_t)strlen(n);
        memcpy(p + 4, n, strlen(n));
    }
    return 0;
}

/* ==================================================================== */
/* nn::vi (5) —— 显示。宿主侧由图形层提供真实分辨率。                    */
/* ==================================================================== */
int  dm_nn_vi_OpenDisplay(void *out, const char *name) { (void)name; if (out) *(uint64_t *)out = 1; g_st.n_vi++; return 0; }
int  dm_nn_vi_CloseDisplay(uint64_t d) { (void)d; g_st.n_vi++; return 0; }
void dm_nn_vi_GetDisplayResolution(uint64_t d, int64_t *w, int64_t *h) {
    (void)d;
    if (w) *w = 1920;
    if (h) *h = 1080;
}
int  dm_nn_vi_GetDisplayLogicalResolution(uint64_t d, int32_t *w, int32_t *h) {
    (void)d;
    if (w) *w = 1920;
    if (h) *h = 1080;
    return 0;
}
int  dm_nn_vi_SetDisplayResolution(uint64_t d, int64_t w, int64_t h) { (void)d; (void)w; (void)h; return 0; }

/* ==================================================================== */
/* nn::oe 余下 (5) / nn::ro (1) / nn::detail (1)                         */
/* ==================================================================== */
int  dm_nn_oe_TryPopNotificationMessage(uint32_t *out) { if (out) *out = 0; g_st.n_oe2++; return 0; }
int  dm_nn_oe_SetPerformanceConfiguration(int mode, int cfg) { (void)mode; (void)cfg; return 0; }
void dm_nn_oe_EnterExitRequestHandlingSection(void) { }
void dm_nn_oe_LeaveExitRequestHandlingSection(void) { }
void dm_nn_oe_SetOperationModeChangedNotificationEnabled(int en) { (void)en; }

/* nn::ro 余下 1 个：RegisterModuleInfo 的另一个重载（这里给通用实现） */
int dm_nn_ro_RegisterModuleInfo2(void *info, const void *mod) { (void)info; (void)mod; g_st.n_ro2++; return 0; }

/* nn::detail::UnexpectedDefaultImpl —— 「不该走到这里」的兜底。
   宿主不 abort（否则一个可恢复的分支会杀掉进程），只记账。 */
void dm_nn_detail_UnexpectedDefaultImpl(void) { g_st.n_detail++; }

/* ---- 本轮补的零散实现（规则里引用到的） ---- */

void     dm_nn_audio_ReleaseVoiceSlot_stub(void *cfg, void *v) { (void)cfg; (void)v; g_st.n_audio_voice++; }
void     dm_nn_audio_ReleaseMemoryPool_stub(void *cfg, void *mp) { (void)cfg; (void)mp; }
void     dm_nn_audio_SetVoiceMixVolume_stub(void *v, void *sm, float f, int a, int b) {
    (void)v; (void)sm; (void)f; (void)a; (void)b;
}
void     dm_nn_audio_SetSubMixMixVolume_stub(void *a, void *b, float f, int c, int d) {
    (void)a; (void)b; (void)f; (void)c; (void)d;
}
void     dm_nn_audio_SetVoicePlayState_stub(void *v, int st) { (void)v; (void)st; }
int      dm_nn_audio_GetVoicePlayState_stub(const void *v) { (void)v; return 0; }
uint32_t dm_nn_audio_GetWaveBufferCount_stub(const void *v) { (void)v; return 0; }
void     dm_nn_audio_GetMemoryPoolState_stub(const void *mp, void *out) { (void)mp; if (out) memset(out, 0, 0x10); }
void     dm_nn_audio_SetDelayInputOutput_stub(void *d, const int *a, const int *b, int n) {
    (void)d; (void)a; (void)b; (void)n;
}
void     dm_nn_audio_SetVoiceDestination_stub2(void *a, void *b, void *c) { (void)a; (void)b; (void)c; }
uint32_t dm_nn_audio_GetAuxSampleCount_stub(const void *aux) { (void)aux; return 0; }
int      dm_nn_audio_ReadAuxSendBuffer_stub(void *aux, int *a, int n) { (void)aux; (void)a; (void)n; return 0; }

void     dm_nn_vi_CreateLayer_stub(void *out, void *display) {
    (void)display;
    if (out) *(void **)out = NULL;
    g_st.n_vi++;
}
void     dm_nn_vi_GetNativeWindow_stub(void *out, void *layer) {
    (void)layer;
    if (out) *(void **)out = NULL;   /* 真机侧由图形层填 CAMetalLayer */
    g_st.n_vi++;
}
void     dm_nn_vi_GetDisplayVsyncEvent(void *ev, void *display) {
    (void)display;
    g_st.n_vi++;
    extern void dm_nn_InitializeSystemEvent(void *, int);
    dm_nn_InitializeSystemEvent(ev, 1);
}

/* nn::ro::LookupModuleSymbol(uint64_t *out, const Module*, const char *name)
   —— 宿主从已装载的模块里查导出符号。 */
int dm_nn_ro_LookupModuleSymbol(uint64_t *out, const void *mod, const char *name) {
    (void)mod; (void)name;
    g_st.n_ro2++;
    if (out) *out = 0;
    return -1;      /* 未找到；调用方一般会退回自己的静态表 */
}

/* ---- nn::audio 最后 16 个 ---- */

/* 记忆池：对象存储。宿主分配一块宿主内存当池。 */
int dm_nn_audio_AcquireMemoryPool(void *cfg, void *mp, void *buf, uint64_t sz) {
    (void)cfg; (void)buf; (void)sz;
    g_st.n_audio_init++;
    if (mp) {
        void *p = calloc(1, (size_t)(sz ? sz : 0x10000));
        if (!p) return -1;
        *(void **)mp = p;
    }
    return 0;
}
int dm_nn_audio_CopyMemoryPoolData(const void *mp, void *dst, const void *src, uint64_t sz) {
    (void)mp;
    if (dst && src) memcpy(dst, src, (size_t)sz);
    return 0;
}
void dm_nn_audio_SetAuxInputOutput(void *aux, const int *a, const int *b, int n) {
    (void)aux; (void)a; (void)b; (void)n;
}

/* 开/启音频渲染器。真机上这里是「建音频设备 + 起混音线程」。
   TODO（音频输出）：宿主目前只建句柄，不产生声音。 */
int dm_nn_audio_OpenAudioRenderer(uint64_t *out_handle, void *event,
                                  const void *param, void *work, uint64_t work_sz) {
    (void)event; (void)param; (void)work; (void)work_sz;
    g_st.n_audio_init++;
    if (out_handle) *out_handle = 1;
    return 0;
}
int dm_nn_audio_StartAudioRenderer(uint64_t handle) {
    (void)handle;
    g_st.n_audio_renderer++;
    return 0;
}



/* ==================================================================== */
/* 通用空桩                                                              */
/* ==================================================================== */
/* 用途：配置类 API（SetXxx / AcquireXxx / ReleaseXxx / AddXxx …）数量多、
   且参数随版本变化。用一个**接受任意参数**的空桩接管它们，
   保证符号一定能解析（否则装载就失败），同时不假装做了实际工作。

   为什么用 `void f(void)` 而不是可变参数：调用方按 AArch64 约定传参，
   被调方不读参数就没问题；写成可变参数反而需要正确的原型。
   这样做的代价是**不能返回值** —— 所以凡是调用方要用返回值的（尺寸查询、
   状态查询）都单独写了实现，只有纯配置/纯动作类走这里。 */
void dm_nn_stub_noop(void) { }

