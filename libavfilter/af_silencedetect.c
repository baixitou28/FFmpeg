/*
 * Copyright (c) 2012 Clément Bœsch <u pkh me>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Audio silence detector
 */

#include <float.h> /* DBL_MAX */

#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "audio.h"
#include "formats.h"
#include "avfilter.h"
#include "internal.h"

typedef struct SilenceDetectContext {
    const AVClass *class;
    double noise;               ///< noise amplitude ratio
    double duration;            ///< minimum duration of silence until notification
    int mono;                   ///< mono mode : check each channel separately (default = check when ALL channels are silent)
    int channels;               ///< number of channels
    int independent_channels;   ///< number of entries in following arrays (always 1 in mono mode)
    int64_t *nb_null_samples;   ///< (array) current number of continuous zero samples
    int64_t *start;             ///< (array) if silence is detected, this value contains the time of the first zero sample (default/unset = INT64_MIN)
    int64_t frame_end;          ///< pts of the end of the current frame (used to compute duration of silence at EOS)
    int last_sample_rate;       ///< last sample rate to check for sample rate changes
    AVRational time_base;       ///< time_base

    void (*silencedetect)(struct SilenceDetectContext *s, AVFrame *insamples,
                          int nb_samples, int64_t nb_samples_notify,
                          AVRational time_base);
} SilenceDetectContext;

#define OFFSET(x) offsetof(SilenceDetectContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption silencedetect_options[] = {//TODO:dB解析需要调试才能看，查代码不知道在哪里解析和转换 -60dB到0.001
    { "n",         "set noise tolerance",              OFFSET(noise),     AV_OPT_TYPE_DOUBLE, {.dbl=0.001},          0, DBL_MAX,  FLAGS },//默认Default is -60dB, or 0.001 即log10((10^-3)*(10^-3))//示例参数是 -af silencedetect=n=-50dB:d=1 ，为什么是dB
    { "noise",     "set noise tolerance",              OFFSET(noise),     AV_OPT_TYPE_DOUBLE, {.dbl=0.001},          0, DBL_MAX,  FLAGS },
    { "d",         "set minimum duration in seconds",  OFFSET(duration),  AV_OPT_TYPE_DOUBLE, {.dbl=2.},             0, 24*60*60, FLAGS },//默认2秒，最大24小时
    { "duration",  "set minimum duration in seconds",  OFFSET(duration),  AV_OPT_TYPE_DOUBLE, {.dbl=2.},             0, 24*60*60, FLAGS },
    { "mono",      "check each channel separately",    OFFSET(mono),      AV_OPT_TYPE_BOOL,   {.i64=0.},             0, 1,        FLAGS },//默认是0，每个通道单独检测
    { "m",         "check each channel separately",    OFFSET(mono),      AV_OPT_TYPE_BOOL,   {.i64=0.},             0, 1,        FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(silencedetect);
//设置到metadata里，以便后面帧可以查看
static void set_meta(AVFrame *insamples, int channel, const char *key, char *value)
{
    char key2[128];

    if (channel)
        snprintf(key2, sizeof(key2), "lavfi.%s.%d", key, channel);
    else
        snprintf(key2, sizeof(key2), "lavfi.%s", key);
    av_dict_set(&insamples->metadata, key2, value, 0);
}
static av_always_inline void update(SilenceDetectContext *s, AVFrame *insamples,
                                    int is_silence, int current_sample, int64_t nb_samples_notify,
                                    AVRational time_base)
{
    int channel = current_sample % s->independent_channels;
    if (is_silence) {//静音判断标志silencedetect_xxx关键函数*p < noise && *p > -noise ==> 比如默认设置的0.001，则 [-0.001*2^15,0.001*2^15](16位为例)或[-33，33] 都会被认为静音
        if (s->start[channel] == INT64_MIN) {//判断条件不是很明白  ==> 如果是初始化的INT64_MIN，填入silence_start开始的相对时间
            s->nb_null_samples[channel]++;
            if (s->nb_null_samples[channel] >= nb_samples_notify) {
                s->start[channel] = insamples->pts + av_rescale_q(current_sample / s->channels + 1 - nb_samples_notify * s->independent_channels / s->channels,
                        (AVRational){ 1, s->last_sample_rate }, time_base);
                set_meta(insamples, s->mono ? channel + 1 : 0, "silence_start",//设置开始
                        av_ts2timestr(s->start[channel], &time_base));//时间转化为相对自然秒
                if (s->mono)
                    av_log(s, AV_LOG_INFO, "channel: %d | ", channel);
                av_log(s, AV_LOG_INFO, "silence_start: %s\n",//打印
                        av_ts2timestr(s->start[channel], &time_base));
            }
        }
    } else {
        if (s->start[channel] > INT64_MIN) {//显然silence_start已经有了，才会需要end
            int64_t end_pts = insamples ? insamples->pts + av_rescale_q(current_sample / s->channels,//观察这里时间计算的方式
                    (AVRational){ 1, s->last_sample_rate }, time_base)
                    : s->frame_end;
            int64_t duration_ts = end_pts - s->start[channel];
            if (insamples) {
                set_meta(insamples, s->mono ? channel + 1 : 0, "silence_end",//设置结束
                        av_ts2timestr(end_pts, &time_base));
                set_meta(insamples, s->mono ? channel + 1 : 0, "silence_duration",//设置时长
                        av_ts2timestr(duration_ts, &time_base));
            }
            if (s->mono)
                av_log(s, AV_LOG_INFO, "channel: %d | ", channel);
            av_log(s, AV_LOG_INFO, "silence_end: %s | silence_duration: %s\n",
                    av_ts2timestr(end_pts, &time_base),
                    av_ts2timestr(duration_ts, &time_base));
        }
        s->nb_null_samples[channel] = 0;
        s->start[channel] = INT64_MIN;//重新初始化，以便记录下一次silence_start
    }
}
//不同的数据类型采用不同的函数 //为什么是data[0]==>音频一般就是这种形式，参看AVFrame注释 //判断的标准*p < noise， *p > -noise==> 比如默认noise设置的0.001，则 [-0.001*2^15,0.001*2^15](16位为例)或[-33，33] 都会被认为静音
#define SILENCE_DETECT(name, type)                                               \
static void silencedetect_##name(SilenceDetectContext *s, AVFrame *insamples,    \
                                 int nb_samples, int64_t nb_samples_notify,      \
                                 AVRational time_base)                           \
{                                                                                \
    const type *p = (const type *)insamples->data[0];                            \
    const type noise = s->noise;                                                 \
    int i;                                                                       \
                                                                                 \
    for (i = 0; i < nb_samples; i++, p++)                                        \
        update(s, insamples, *p < noise && *p > -noise, i,                       \
               nb_samples_notify, time_base);                                    \
}

SILENCE_DETECT(dbl, double)//用0.001的比例
SILENCE_DETECT(flt, float)
SILENCE_DETECT(s32, int32_t)
SILENCE_DETECT(s16, int16_t)//用0.001*32767i16=33的数量比

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    SilenceDetectContext *s = ctx->priv;
    int c;
    //01.确认通道数
    s->channels = inlink->channels;
    s->independent_channels = s->mono ? s->channels : 1;//默认单声道，一个通道统计就够了
    s->nb_null_samples = av_mallocz_array(sizeof(*s->nb_null_samples), s->independent_channels);//02.分配内存
    if (!s->nb_null_samples)
        return AVERROR(ENOMEM);
    s->start = av_malloc_array(sizeof(*s->start), s->independent_channels);//03.通道分配内存用于统计
    if (!s->start)
        return AVERROR(ENOMEM);
    for (c = 0; c < s->independent_channels; c++)//默认independent_channels为1
        s->start[c] = INT64_MIN;//04.初始化为最小值即0

    switch (inlink->format) {//05.根据不同数据类型，采用不同的函数
    case AV_SAMPLE_FMT_DBL: s->silencedetect = silencedetect_dbl; break;//浮点数只要存最大值的一个比例即可？比如s->noise=0.001
    case AV_SAMPLE_FMT_FLT: s->silencedetect = silencedetect_flt; break;
    case AV_SAMPLE_FMT_S32:
        s->noise *= INT32_MAX;//0.001*2147483647i32=2147484
        s->silencedetect = silencedetect_s32;
        break;
    case AV_SAMPLE_FMT_S16:
        s->noise *= INT16_MAX;//-60dB:0.001*32767i16=33 ,如果是-33dB,
        s->silencedetect = silencedetect_s16;
        break;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *insamples)
{
    SilenceDetectContext *s         = inlink->dst->priv;
    const int nb_channels           = inlink->channels;
    const int srate                 = inlink->sample_rate;
    const int nb_samples            = insamples->nb_samples     * nb_channels;
    const int64_t nb_samples_notify = srate * s->duration * (s->mono ? 1 : nb_channels);
    int c;

    // scale number of null samples to the new sample rate
    if (s->last_sample_rate && s->last_sample_rate != srate)//考虑比特率变化，这时候用户一般没有觉察，也不太关心，
        for (c = 0; c < s->independent_channels; c++) {
            s->nb_null_samples[c] = srate * s->nb_null_samples[c] / s->last_sample_rate;//根据比特率，调整历史统计值
        }
    s->last_sample_rate = srate;//调整相关参数
    s->time_base = inlink->time_base;
    s->frame_end = insamples->pts + av_rescale_q(insamples->nb_samples,
            (AVRational){ 1, s->last_sample_rate }, inlink->time_base);

    // TODO: document metadata
    s->silencedetect(s, insamples, nb_samples, nb_samples_notify,
                     inlink->time_base);

    return ff_filter_frame(inlink->dst->outputs[0], insamples);//对音频源数据只做统计，没做其他额外处理
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    static const enum AVSampleFormat sample_fmts[] = {//支持的格式，有符号，无符号，32位，16位都可以
        AV_SAMPLE_FMT_DBL,
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_S32,
        AV_SAMPLE_FMT_S16,//在另外一个模块volumedetecct，只支持S16格式
        AV_SAMPLE_FMT_NONE
    };
    int ret;

    layouts = ff_all_channel_layouts();
    if (!layouts)
        return AVERROR(ENOMEM);
    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        return ret;

    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    ret = ff_set_common_formats(ctx, formats);
    if (ret < 0)
        return ret;

    formats = ff_all_samplerates();
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_samplerates(ctx, formats);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SilenceDetectContext *s = ctx->priv;
    int c;

    for (c = 0; c < s->independent_channels; c++)
        if (s->start[c] > INT64_MIN)
            update(s, NULL, 0, c, 0, s->time_base);
    av_freep(&s->nb_null_samples);
    av_freep(&s->start);
}

static const AVFilterPad silencedetect_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
        .filter_frame = filter_frame,//统计
    },
    { NULL }
};

static const AVFilterPad silencedetect_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};
//默认0.001倍的最高声音的以下认为是静音，将默认静音超过2秒的以打印日志的方式输出
AVFilter ff_af_silencedetect = {
    .name          = "silencedetect",
    .description   = NULL_IF_CONFIG_SMALL("Detect silence."),
    .priv_size     = sizeof(SilenceDetectContext),
    .query_formats = query_formats,//01.最重要的入口1：支持的格式
    .uninit        = uninit,
    .inputs        = silencedetect_inputs,//02.最重要的入口2：输入的入口
    .outputs       = silencedetect_outputs,//03.最重要的入口3：什么也不做
    .priv_class    = &silencedetect_class,//04.每个模块都要有个自己的状态
};
