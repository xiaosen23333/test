/* 宿主 HLE —— nn::audio / fontll / codec / swkbd / web / account / vi 的声明。 */
#ifndef DM_HLE_NN_AUDIO_H
#define DM_HLE_NN_AUDIO_H

#include <stdint.h>

const void *dm_nn_audio_stats_ptr(void);

/* nn::audio */
void     dm_nn_audio_InitializeAudioRendererParameter(void *p);
int      dm_nn_audio_IsValidAudioRendererParameter(const void *p);
uint64_t dm_nn_audio_GetAudioRendererWorkBufferSize(const void *p);
uint64_t dm_nn_audio_GetAudioRendererConfigWorkBufferSize(const void *p);
uint64_t dm_nn_audio_GetRequiredBufferSizeForI3dl2Reverb(int a, int b);
uint64_t dm_nn_audio_GetRequiredBufferSizeForDelay(int64_t t, int a, int b);
uint64_t dm_nn_audio_GetRequiredBufferSizeForAuxSendReturnBuffer(const void *p, int a, int b);
void     dm_nn_audio_InitializeAudioRendererConfig(void *cfg, const void *param, void *work, uint64_t sz);
int      dm_nn_audio_AddDeviceSink(void *cfg, void *sink, void *fmi, const int *rate, int ch, const char *name);
int      dm_nn_audio_AcquireFinalMix(void *cfg, void *fm, int count);
int      dm_nn_audio_AcquireSubMix(void *cfg, void *sm, int a, int b);
void     dm_nn_audio_ReleaseSubMix(void *cfg, void *sm);
void     dm_nn_audio_SetSubMixDestination(void *cfg, void *a, void *b);
void     dm_nn_audio_SetSubMixVolume(void *sm, float v);
int      dm_nn_audio_AcquireVoiceSlot(void *cfg, void *voice, int a, int b, int fmt, int pitch,
                                     const void *data, uint64_t size);
void     dm_nn_audio_SetVoiceDestination(void *cfg, void *voice, void *dest);
void     dm_nn_audio_SetVoicePitch(void *voice, float pitch);
int      dm_nn_audio_IsVoiceValid(const void *voice);
void     dm_nn_audio_AppendWaveBuffer(void *voice, const void *wb);
void    *dm_nn_audio_GetReleasedWaveBuffer(void *voice);
uint64_t dm_nn_audio_GetVoicePlayedSampleCount(const void *voice);
int      dm_nn_audio_ParseAdpcmHeader(void *info, const void *data, uint64_t size);
int      dm_nn_audio_RequestAttachMemoryPool(void *mp);
int      dm_nn_audio_RequestDetachMemoryPool(void *mp);
int      dm_nn_audio_IsMemoryPoolAttached(const void *mp);
int      dm_nn_audio_RequestUpdateAudioRenderer(uint64_t handle, const void *cfg);
int      dm_nn_audio_AddAux(void *cfg, void *aux, void *sm, void *a, void *b, uint64_t n);
void     dm_nn_audio_RemoveAux(void *cfg, void *aux, void *sm);
void     dm_nn_audio_SetAuxEnabled(void *aux, int en);
int      dm_nn_audio_IsAuxRemovable(const void *aux);
void     dm_nn_audio_WriteAuxReturnBuffer(void *aux, const int *a, int n);
int      dm_nn_audio_AddDelay(void *cfg, void *d, void *buf, uint64_t sz, void *sm, int64_t time, int ch);
void     dm_nn_audio_RemoveDelay(void *cfg, void *d, void *sm);
void     dm_nn_audio_SetDelayTime(void *d, int64_t t);
int64_t  dm_nn_audio_GetDelayTime(const void *d);
void     dm_nn_audio_SetDelayEnabled(void *d, int en);
void     dm_nn_audio_SetDelayInGain(void *d, float g);
void     dm_nn_audio_SetDelayDryGain(void *d, float g);
void     dm_nn_audio_SetDelayFeedbackGain(void *d, float g);
void     dm_nn_audio_SetDelayLowPassAmount(void *d, float a);
int      dm_nn_audio_IsDelayRemovable(const void *d);
int      dm_nn_audio_AddI3dl2Reverb(void *cfg, void *r, void *buf, uint64_t sz, void *sm, int ch);
void     dm_nn_audio_SetI3dl2ReverbEnabled(void *r, int en);
void     dm_nn_audio_SetI3dl2ReverbParameters(void *r, const void *p);
void     dm_nn_audio_SetI3dl2ReverbInputOutput(void *r, const int *a, const int *b, int n);

/* nn::fontll */
void dm_nn_fontll_ctor(void *self);
void dm_nn_fontll_dtor(void *self);
int  dm_nn_fontll_Initialize(void *self, void *buf, uint32_t sz);
int  dm_nn_fontll_Finalize(void *self);
int  dm_nn_fontll_SetScale(void *self, int a, int b, int c, int d);
int  dm_nn_fontll_SetFont(void *self, char *name);
int  dm_nn_fontll_LoadFont(void *self, char *name, const void *data, uint32_t a, uint32_t b);
int  dm_nn_fontll_AcquireGlyphmap(void *self, uint32_t cp, uint16_t a);
int  dm_nn_fontll_ReleasesGlyph(void *self, void *glyph);
int  dm_nn_fontll_GetFontMetrics(void *self, void *out);

/* nn::codec */
uint64_t dm_nn_codec_GetOpusDecoderWorkBufferSize(int ch, int rate);
void     dm_nn_codec_InitializeOpusDecoder(void *d, int ch, int rate, void *work, uint64_t sz);
void     dm_nn_codec_FinalizeOpusDecoder(void *d);
void     dm_nn_codec_ResetOpusDecoder(void *d);
int      dm_nn_codec_GetOpusDecoderSampleRate(const void *d);
int      dm_nn_codec_GetOpusDecoderChannelCount(const void *d);
int      dm_nn_codec_DecodeOpusInterleaved(void *d, uint64_t *out_samples, int *out_ch,
                                           int *out_rate, int16_t *out, const void *in,
                                           uint64_t in_sz, uint64_t a);

/* nn::swkbd */
void     dm_nn_swkbd_MakePreset(void *cfg, int preset);
uint64_t dm_nn_swkbd_GetRequiredWorkBufferSize(int b);
uint64_t dm_nn_swkbd_GetRequiredStringBufferSize(void);
void     dm_nn_swkbd_SetInitialTextUtf8(void *arg, const char *s);
void     dm_nn_swkbd_SetGuideTextUtf8(void *cfg, const char *s);
int      dm_nn_swkbd_ShowKeyboard(void *out_str, const void *arg);

/* nn::web */
int  dm_nn_web_ShowWebPage(void *ret, const void *arg);
int  dm_nn_web_ShowOfflineHtmlPage(void *ret, const void *arg);
void dm_nn_web_ShowWebPageArg_ctor(void *self, const char *url);
void dm_nn_web_ShowWebPageArg_dtor(void *self);
void dm_nn_web_ReturnValue_ctor(void *self);
void dm_nn_web_ShowOfflineArg_ctor(void *self, const char *url);

/* nn::account */
int dm_nn_account_Initialize(void);
int dm_nn_account_GetUserId(void *uid, const void *handle);
int dm_nn_account_TryOpenPreselectedUser(void *handle);
int dm_nn_account_ShowUserSelector(void *uid, const void *settings);
int dm_nn_account_GetNickname(void *out, const void *uid);

/* nn::vi */
int  dm_nn_vi_OpenDisplay(void *out, const char *name);
int  dm_nn_vi_CloseDisplay(uint64_t d);
void dm_nn_vi_GetDisplayResolution(uint64_t d, int64_t *w, int64_t *h);
int  dm_nn_vi_GetDisplayLogicalResolution(uint64_t d, int32_t *w, int32_t *h);
int  dm_nn_vi_SetDisplayResolution(uint64_t d, int64_t w, int64_t h);

/* 其余 */
int  dm_nn_oe_TryPopNotificationMessage(uint32_t *out);
int  dm_nn_oe_SetPerformanceConfiguration(int mode, int cfg);
void dm_nn_oe_EnterExitRequestHandlingSection(void);
void dm_nn_oe_LeaveExitRequestHandlingSection(void);
void dm_nn_oe_SetOperationModeChangedNotificationEnabled(int en);
int  dm_nn_ro_RegisterModuleInfo2(void *info, const void *mod);
void dm_nn_detail_UnexpectedDefaultImpl(void);

/* 通用空桩（配置/纯动作类） */
void dm_nn_stub_noop(void);

/* audio 的零散实现 */
void     dm_nn_audio_ReleaseVoiceSlot_stub(void *cfg, void *v);
void     dm_nn_audio_ReleaseMemoryPool_stub(void *cfg, void *mp);
void     dm_nn_audio_SetVoiceMixVolume_stub(void *v, void *sm, float f, int a, int b);
void     dm_nn_audio_SetSubMixMixVolume_stub(void *a, void *b, float f, int c, int d);
void     dm_nn_audio_SetVoicePlayState_stub(void *v, int st);
int      dm_nn_audio_GetVoicePlayState_stub(const void *v);
uint32_t dm_nn_audio_GetWaveBufferCount_stub(const void *v);
void     dm_nn_audio_GetMemoryPoolState_stub(const void *mp, void *out);
void     dm_nn_audio_SetDelayInputOutput_stub(void *d, const int *a, const int *b, int n);
uint32_t dm_nn_audio_GetAuxSampleCount_stub(const void *aux);
int      dm_nn_audio_ReadAuxSendBuffer_stub(void *aux, int *a, int n);
int      dm_nn_audio_AcquireMemoryPool(void *cfg, void *mp, void *buf, uint64_t sz);
int      dm_nn_audio_CopyMemoryPoolData(const void *mp, void *dst, const void *src, uint64_t sz);
void     dm_nn_audio_SetAuxInputOutput(void *aux, const int *a, const int *b, int n);
int      dm_nn_audio_OpenAudioRenderer(uint64_t *out_handle, void *event,
                                       const void *param, void *work, uint64_t work_sz);
int      dm_nn_audio_StartAudioRenderer(uint64_t handle);

/* vi 的零散实现 */
void dm_nn_vi_CreateLayer_stub(void *out, void *display);
void dm_nn_vi_GetNativeWindow_stub(void *out, void *layer);
void dm_nn_vi_GetDisplayVsyncEvent(void *ev, void *display);

/* ro */
int  dm_nn_ro_LookupModuleSymbol(uint64_t *out, const void *mod, const char *name);

#endif /* DM_HLE_NN_AUDIO_H */
