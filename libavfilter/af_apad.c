/*
 * Copyright (c) 2012 Michael Niedermayer
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
 * audio pad filter.
 *
 * Based on af_aresample.c
 */

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "avfilter.h"
#include "audio.h"
#include "internal.h"

typedef struct APadContext {
    const AVClass *class;
    int64_t next_pts;

    int packet_size;
    int64_t pad_len, pad_len_left;
    int64_t whole_len, whole_len_left;
    int64_t pad_dur;
    int64_t whole_dur;
} APadContext;

#define OFFSET(x) offsetof(APadContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
//tiger ����
static const AVOption apad_options[] = {
    { "packet_size", "set silence packet size",                                  OFFSET(packet_size), AV_OPT_TYPE_INT,   { .i64 = 4096 }, 0, INT_MAX, A },
    { "pad_len",     "set number of samples of silence to add",                  OFFSET(pad_len),     AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, A },
    { "whole_len",   "set minimum target number of samples in the audio stream", OFFSET(whole_len),   AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, A },
    { "pad_dur",     "set duration of silence to add",                           OFFSET(pad_dur),     AV_OPT_TYPE_DURATION, { .i64 = 0 }, 0, INT64_MAX, A },
    { "whole_dur",   "set minimum target duration in the audio stream",          OFFSET(whole_dur),   AV_OPT_TYPE_DURATION, { .i64 = 0 }, 0, INT64_MAX, A },
    { NULL }
};

AVFILTER_DEFINE_CLASS(apad);

static av_cold int init(AVFilterContext *ctx)
{
    APadContext *s = ctx->priv;

    s->next_pts = AV_NOPTS_VALUE;
    if (s->whole_len >= 0 && s->pad_len >= 0) {
        av_log(ctx, AV_LOG_ERROR, "Both whole and pad length are set, this is not possible\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)//��ͣ��¼���ȣ����ĳ��Ⱥ�pts����request_frame��Ҫ�õ��ı���
{
    AVFilterContext *ctx = inlink->dst;
    APadContext *s = ctx->priv;//ÿ��filter�����Լ��Ľṹ��
    //01.���ж�������
    if (s->whole_len >= 0) {
        s->whole_len_left = FFMAX(s->whole_len_left - frame->nb_samples, 0);
        av_log(ctx, AV_LOG_DEBUG,
               "n_out:%d whole_len_left:%"PRId64"\n", frame->nb_samples, s->whole_len_left);
    }
    //02.�¸�ʱ��
    s->next_pts = frame->pts + av_rescale_q(frame->nb_samples, (AVRational){1, inlink->sample_rate}, inlink->time_base);
    return ff_filter_frame(ctx->outputs[0], frame);//03. ���ݵ���һ��AVFilterLink
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    APadContext *s = ctx->priv;
    int ret;
    //01.����״̬
    ret = ff_request_frame(ctx->inputs[0]);
    //02.��������pad��ʼ����
    if (ret == AVERROR_EOF && !ctx->is_disabled) {
        int n_out = s->packet_size;
        AVFrame *outsamplesref;
        //01.����У�鳤��
        if (s->whole_len >= 0 && s->pad_len < 0) {
            s->pad_len = s->pad_len_left = s->whole_len_left;
        }
        if (s->pad_len >=0 || s->whole_len >= 0) {
            n_out = FFMIN(n_out, s->pad_len_left);
            s->pad_len_left -= n_out;
            av_log(ctx, AV_LOG_DEBUG,
                   "padding n_out:%d pad_len_left:%"PRId64"\n", n_out, s->pad_len_left);
        }

        if (!n_out)
            return AVERROR_EOF;
        //02. ȡ����Ϊn_out��AVFrame
        outsamplesref = ff_get_audio_buffer(outlink, n_out);
        if (!outsamplesref)
            return AVERROR(ENOMEM);

        av_assert0(outsamplesref->sample_rate == outlink->sample_rate);
        av_assert0(outsamplesref->nb_samples  == n_out);
        //03.ֱ������
        av_samples_set_silence(outsamplesref->extended_data, 0,
                               n_out,
                               outsamplesref->channels,
                               outsamplesref->format);
        //04.����pts
        outsamplesref->pts = s->next_pts;
        if (s->next_pts != AV_NOPTS_VALUE)
            s->next_pts += av_rescale_q(n_out, (AVRational){1, outlink->sample_rate}, outlink->time_base);
        //05.�Ž�ĩβ
        return ff_filter_frame(outlink, outsamplesref);
    }
    return ret;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    APadContext *s  = ctx->priv;

    if (s->pad_dur)
        s->pad_len = av_rescale(s->pad_dur, outlink->sample_rate, AV_TIME_BASE);
    if (s->whole_dur)
        s->whole_len = av_rescale(s->whole_dur, outlink->sample_rate, AV_TIME_BASE);

    s->pad_len_left   = s->pad_len;
    s->whole_len_left = s->whole_len;

    return 0;
}

static const AVFilterPad apad_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad apad_outputs[] = {
    {
        .name          = "default",
        .request_frame = request_frame,
        .config_props  = config_output,
        .type          = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_apad = {//tiger pad ������ĩβ���뾲����
    .name          = "apad",
    .description   = NULL_IF_CONFIG_SMALL("Pad audio with silence."),
    .init          = init,
    .priv_size     = sizeof(APadContext),
    .inputs        = apad_inputs,
    .outputs       = apad_outputs,
    .priv_class    = &apad_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};
