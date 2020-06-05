/*
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
 * sample format and channel layout conversion audio filter
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"

#include "libavresample/avresample.h"

#include "audio.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"

typedef struct ResampleContext {
    const AVClass *class;
    AVAudioResampleContext *avr;
    AVDictionary *options;

    int resampling;
    int64_t next_pts;
    int64_t next_in_pts;

    /* set by filter_frame() to signal an output frame to request_frame() */
    int got_output;
} ResampleContext;

static av_cold int init(AVFilterContext *ctx, AVDictionary **opts)
{
    ResampleContext *s = ctx->priv;
    const AVClass *avr_class = avresample_get_class();
    AVDictionaryEntry *e = NULL;
    //01.�ɺ��Ե�
    while ((e = av_dict_get(*opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (av_opt_find(&avr_class, e->key, NULL, 0,
                        AV_OPT_SEARCH_FAKE_OBJ | AV_OPT_SEARCH_CHILDREN))
            av_dict_set(&s->options, e->key, e->value, 0);
    }
    //02.�ɺ��Ե�
    e = NULL;
    while ((e = av_dict_get(s->options, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_dict_set(opts, e->key, NULL, 0);
    //03.��ֹ�û��������²���
    /* do not allow the user to override basic format options */
    av_dict_set(&s->options,  "in_channel_layout", NULL, 0);
    av_dict_set(&s->options, "out_channel_layout", NULL, 0);
    av_dict_set(&s->options,  "in_sample_fmt",     NULL, 0);
    av_dict_set(&s->options, "out_sample_fmt",     NULL, 0);
    av_dict_set(&s->options,  "in_sample_rate",    NULL, 0);
    av_dict_set(&s->options, "out_sample_rate",    NULL, 0);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ResampleContext *s = ctx->priv;

    if (s->avr) {
        avresample_close(s->avr);//�رպܶ���ſ���
        avresample_free(&s->avr);
    }
    av_dict_free(&s->options);//�ͷ�option
}

static int query_formats(AVFilterContext *ctx)//��ѯ��ʽ
{
    AVFilterLink *inlink  = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterFormats *in_formats, *out_formats, *in_samplerates, *out_samplerates;
    AVFilterChannelLayouts *in_layouts, *out_layouts;
    int ret;
    //01.
    if (!(in_formats      = ff_all_formats         (AVMEDIA_TYPE_AUDIO)) ||
        !(out_formats     = ff_all_formats         (AVMEDIA_TYPE_AUDIO)) ||
        !(in_samplerates  = ff_all_samplerates     (                  )) ||
        !(out_samplerates = ff_all_samplerates     (                  )) ||
        !(in_layouts      = ff_all_channel_layouts (                  )) ||
        !(out_layouts     = ff_all_channel_layouts (                  )))
        return AVERROR(ENOMEM);
    //02. ����
    if ((ret = ff_formats_ref         (in_formats,      &inlink->out_formats        )) < 0 ||
        (ret = ff_formats_ref         (out_formats,     &outlink->in_formats        )) < 0 ||
        (ret = ff_formats_ref         (in_samplerates,  &inlink->out_samplerates    )) < 0 ||
        (ret = ff_formats_ref         (out_samplerates, &outlink->in_samplerates    )) < 0 ||
        (ret = ff_channel_layouts_ref (in_layouts,      &inlink->out_channel_layouts)) < 0 ||
        (ret = ff_channel_layouts_ref (out_layouts,     &outlink->in_channel_layouts)) < 0)
        return ret;

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    ResampleContext   *s = ctx->priv;
    char buf1[64], buf2[64];
    int ret;

    int64_t resampling_forced;
    //01.
    if (s->avr) {
        avresample_close(s->avr);
        avresample_free(&s->avr);
    }
    //02.
    if (inlink->channel_layout == outlink->channel_layout &&
        inlink->sample_rate    == outlink->sample_rate    &&
        (inlink->format        == outlink->format ||
        (av_get_channel_layout_nb_channels(inlink->channel_layout)  == 1 &&
         av_get_channel_layout_nb_channels(outlink->channel_layout) == 1 &&
         av_get_planar_sample_fmt(inlink->format) ==
         av_get_planar_sample_fmt(outlink->format))))
        return 0;
    //03.
    if (!(s->avr = avresample_alloc_context()))
        return AVERROR(ENOMEM);
    //04.
    if (s->options) {
        int ret;
        AVDictionaryEntry *e = NULL;
        while ((e = av_dict_get(s->options, "", e, AV_DICT_IGNORE_SUFFIX)))
            av_log(ctx, AV_LOG_VERBOSE, "lavr option: %s=%s\n", e->key, e->value);

        ret = av_opt_set_dict(s->avr, &s->options);
        if (ret < 0)
            return ret;
    }
    //05.
    av_opt_set_int(s->avr,  "in_channel_layout", inlink ->channel_layout, 0);
    av_opt_set_int(s->avr, "out_channel_layout", outlink->channel_layout, 0);
    av_opt_set_int(s->avr,  "in_sample_fmt",     inlink ->format,         0);
    av_opt_set_int(s->avr, "out_sample_fmt",     outlink->format,         0);
    av_opt_set_int(s->avr,  "in_sample_rate",    inlink ->sample_rate,    0);
    av_opt_set_int(s->avr, "out_sample_rate",    outlink->sample_rate,    0);
    //06.
    if ((ret = avresample_open(s->avr)) < 0)
        return ret;
    //07.
    av_opt_get_int(s->avr, "force_resampling", 0, &resampling_forced);
    s->resampling = resampling_forced || (inlink->sample_rate != outlink->sample_rate);

    if (s->resampling) {
        outlink->time_base = (AVRational){ 1, outlink->sample_rate };
        s->next_pts        = AV_NOPTS_VALUE;
        s->next_in_pts     = AV_NOPTS_VALUE;
    } else
        outlink->time_base = inlink->time_base;
    //08.
    av_get_channel_layout_string(buf1, sizeof(buf1),
                                 -1, inlink ->channel_layout);
    av_get_channel_layout_string(buf2, sizeof(buf2),
                                 -1, outlink->channel_layout);
    av_log(ctx, AV_LOG_VERBOSE,
           "fmt:%s srate:%d cl:%s -> fmt:%s srate:%d cl:%s\n",
           av_get_sample_fmt_name(inlink ->format), inlink ->sample_rate, buf1,
           av_get_sample_fmt_name(outlink->format), outlink->sample_rate, buf2);

    return 0;
}

static int request_frame(AVFilterLink *outlink)//�����������request_frame ��1.ff_request_frame ����״̬�� 2.avresample_convert ת�� 3. ff_filter_frame link->fifo
{
    AVFilterContext *ctx = outlink->src;
    ResampleContext   *s = ctx->priv;
    int ret = 0;
    //01.
    s->got_output = 0;
    while (ret >= 0 && !s->got_output)
        ret = ff_request_frame(ctx->inputs[0]);//����AVFilterLink* link״̬�� ���û���쳣��û�����ݣ���ͣѭ��

    /* flush the lavr delay buffer */
    if (ret == AVERROR_EOF && s->avr) {//02.
        AVFrame *frame;
        int nb_samples = avresample_get_out_samples(s->avr, 0);//02.01

        if (!nb_samples)
            return ret;

        frame = ff_get_audio_buffer(outlink, nb_samples);//02.02 ����һ��ʵ��
        if (!frame)
            return AVERROR(ENOMEM);

        ret = avresample_convert(s->avr, frame->extended_data,//02.03 �ز���
                                 frame->linesize[0], nb_samples,
                                 NULL, 0, 0);
        if (ret <= 0) {
            av_frame_free(&frame);
            return (ret == 0) ? AVERROR_EOF : ret;
        }
        //02.04
        frame->nb_samples = ret;
        frame->pts = s->next_pts;
        return ff_filter_frame(outlink, frame);//02.05 outlink->fifo ����һ֡
    }
    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)//������������filter_frame
{
    AVFilterContext  *ctx = inlink->dst;
    ResampleContext    *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;
    //01.
    if (s->avr) {
        AVFrame *out;
        int delay, nb_samples;

        /* maximum possible samples lavr can output */
        delay      = avresample_get_delay(s->avr);//01.01.
        nb_samples = avresample_get_out_samples(s->avr, in->nb_samples);//01.02.

        out = ff_get_audio_buffer(outlink, nb_samples);//01.03.
        if (!out) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        //01.04.
        ret = avresample_convert(s->avr, out->extended_data, out->linesize[0],
                                 nb_samples, in->extended_data, in->linesize[0],
                                 in->nb_samples);
        if (ret <= 0) {
            av_frame_free(&out);
            if (ret < 0)
                goto fail;
        }

        av_assert0(!avresample_available(s->avr));
        //01.05.
        if (s->resampling && s->next_pts == AV_NOPTS_VALUE) {
            if (in->pts == AV_NOPTS_VALUE) {
                av_log(ctx, AV_LOG_WARNING, "First timestamp is missing, "
                       "assuming 0.\n");
                s->next_pts = 0;
            } else
                s->next_pts = av_rescale_q(in->pts, inlink->time_base,
                                           outlink->time_base);
        }
        //01.06.
        if (ret > 0) {
            out->nb_samples = ret;
            //01.06.01
            ret = av_frame_copy_props(out, in);
            if (ret < 0) {
                av_frame_free(&out);
                goto fail;
            }
            //01.06.02
            if (s->resampling) {
                out->sample_rate = outlink->sample_rate;
                /* Only convert in->pts if there is a discontinuous jump.
                   This ensures that out->pts tracks the number of samples actually
                   output by the resampler in the absence of such a jump.
                   Otherwise, the rounding in av_rescale_q() and av_rescale()
                   causes off-by-1 errors. */
                if (in->pts != AV_NOPTS_VALUE && in->pts != s->next_in_pts) {
                    out->pts = av_rescale_q(in->pts, inlink->time_base,
                                                outlink->time_base) -
                                   av_rescale(delay, outlink->sample_rate,
                                              inlink->sample_rate);
                } else
                    out->pts = s->next_pts;

                s->next_pts = out->pts + out->nb_samples;
                s->next_in_pts = in->pts + in->nb_samples;
            } else
                out->pts = in->pts;
            //01.06.03
            ret = ff_filter_frame(outlink, out);
            s->got_output = 1;
        }

fail:
        av_frame_free(&in);
    } else {//02.û��context ==>��û����
        in->format = outlink->format;
        ret = ff_filter_frame(outlink, in);
        s->got_output = 1;
    }

    return ret;
}

static const AVClass *resample_child_class_next(const AVClass *prev)
{
    return prev ? NULL : avresample_get_class();
}

static void *resample_child_next(void *obj, void *prev)
{
    ResampleContext *s = obj;
    return prev ? NULL : s->avr;
}

static const AVClass resample_class = {
    .class_name       = "resample",
    .item_name        = av_default_item_name,
    .version          = LIBAVUTIL_VERSION_INT,
    .child_class_next = resample_child_class_next,
    .child_next       = resample_child_next,
};

static const AVFilterPad avfilter_af_resample_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .filter_frame  = filter_frame,//���봦��������
    },
    { NULL }
};

static const AVFilterPad avfilter_af_resample_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_output,
        .request_frame = request_frame//�������������
    },
    { NULL }
};

AVFilter ff_af_resample = {//TIGER ff_af_resample 
    .name          = "resample",
    .description   = NULL_IF_CONFIG_SMALL("Audio resampling and conversion."),
    .priv_size     = sizeof(ResampleContext),
    .priv_class    = &resample_class,
    .init_dict     = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = avfilter_af_resample_inputs,
    .outputs       = avfilter_af_resample_outputs,
};
