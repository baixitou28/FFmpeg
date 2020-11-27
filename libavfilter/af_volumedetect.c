/*
 * Copyright (c) 2012 Nicolas George
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/channel_layout.h"
#include "libavutil/avassert.h"
#include "audio.h"
#include "avfilter.h"
#include "internal.h"

typedef struct VolDetectContext {
    /**
     * Number of samples at each PCM value.
     * histogram[0x8000 + i] is the number of samples at value i.
     * The extra element is there for symmetry.�Գ�û���ǵ�
     */
    uint64_t histogram[0x10001];//
} VolDetectContext;

static int query_formats(AVFilterContext *ctx)//TIGER ��ѯ
{
    static const enum AVSampleFormat sample_fmts[] = {//01.ֻ֧���з���16λ����
        AV_SAMPLE_FMT_S16,//�з��ŵ�16λ
        AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_NONE
    };
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    int ret;
    //02.
    if (!(formats = ff_make_format_list(sample_fmts)))
        return AVERROR(ENOMEM);
    //03.
    layouts = ff_all_channel_counts();
    if (!layouts)
        return AVERROR(ENOMEM);
    ret = ff_set_common_channel_layouts(ctx, layouts);//04.
    if (ret < 0)
        return ret;

    return ff_set_common_formats(ctx, formats);//05.
}

static int filter_frame(AVFilterLink *inlink, AVFrame *samples)//TIGER ͳ��
{
    AVFilterContext *ctx = inlink->dst;
    VolDetectContext *vd = ctx->priv;
    int nb_samples  = samples->nb_samples;
    int nb_channels = samples->channels;
    int nb_planes   = nb_channels;
    int plane, i;
    int16_t *pcm;

    if (!av_sample_fmt_is_planar(samples->format)) {//���洢��ʽ��ͬ
        nb_samples *= nb_channels;
        nb_planes = 1;//01.����
    }
    for (plane = 0; plane < nb_planes; plane++) {//02.ͳ�ƣ��з���16λתΪ�޷���ͳ�ƣ��μ�query_formats
        pcm = (int16_t *)samples->extended_data[plane];//ת��Ϊint16�����������16λ���أ�==>query_format����ֻ֧��16λ
        for (i = 0; i < nb_samples; i++)
            vd->histogram[pcm[i] + 0x8000]++;//pcm�Ĳ���ֵ��2^15 ==> Ϊʲô��0x8000? A.������int16,������ֵ�ĸ������Ƶ�������������������޷�����������ͳ��
    }

    return ff_filter_frame(inlink->dst->outputs[0], samples);
}
//Ϊʲô��91��60DB��0.001 (10^(-60/20��)��16λ���з�����33��91dB��10^(-4.55)= 10^(0.45)*10^(-5)=2.81*10^(-5),16λ���з�����0.92��1,���ܱ������С��
#define MAX_DB 91

static inline double logdb(uint64_t v)//����2^15��db
{
    double d = v / (double)(0x8000 * 0x8000);//Ϊʲô��д��2^30����2^15*2^15��������ѭ��������
    if (!v)//�����0����Ϊ�����dbֵ
        return MAX_DB;
    return -log10(d) * 10;//��log10����log2�����d�ǳ�2^15��db
}

static void print_stats(AVFilterContext *ctx)
{
    VolDetectContext *vd = ctx->priv;
    int i, max_volume, shift;
    uint64_t nb_samples = 0, power = 0, nb_samples_shift = 0, sum = 0;
    uint64_t histdb[MAX_DB + 1] = { 0 };//��ʼ������Ϊ0
    //01.�޷���16λ(0x10000),ÿ������ֵ����ͳ��ֵ
    for (i = 0; i < 0x10000; i++)
        nb_samples += vd->histogram[i];
    av_log(ctx, AV_LOG_INFO, "n_samples: %"PRId64"\n", nb_samples);//��ӡͳ��ֵ�����еĲ�������
    if (!nb_samples)
        return;
    //���һ�㶼���ῼ����ôϸ�� ����ʱ��Ƚϳ�������ͳ������̫�࣬����2^32����pcm��8k*PCM A��u��,2^32/2^13=2^18�룬���6��
    /* If nb_samples > 1<<34, there is a risk of overflow in the
       multiplication or the sum: shift all histogram values to avoid that.
       The total number of samples must be recomputed to avoid rounding
       errors. */
    shift = av_log2(nb_samples >> 33);//02. 33����εó��ģ�//TODO:Ϊʲô����32���з��ŵ�i*i��16*16=32
    for (i = 0; i < 0x10000; i++) {//03.2^17��17λ��histogram��uint64_t��64λ��ֵ���ж�һ��������shift���Ϊ64-33λ
        nb_samples_shift += vd->histogram[i] >> shift;
        power += (i - 0x8000) * (i - 0x8000) * (vd->histogram[i] >> shift);//Ϊ��ͳ�Ʒ��㣬�Ѹ���+0x8000����ͳ�ƣ������ʱ��Ҫ���ȥ��������(i - 0x8000) ��ƽ��
    }
    if (!nb_samples_shift)//04.
        return;
    power = (power + nb_samples_shift / 2) / nb_samples_shift;//05.��Ҫ����nb_samples_shift���������룻shiftΪ0ʱnb_samples_shiftΪһ��ͳ��ֵ���ȽϺ����
    av_assert0(power <= 0x8000 * 0x8000);//С��15+15λ
    av_log(ctx, AV_LOG_INFO, "mean_volume: %.1f dB\n", -logdb(power));

    max_volume = 0x8000;
    while (max_volume > 0 && !vd->histogram[0x8000 + max_volume] &&//�����2^15
                             !vd->histogram[0x8000 - max_volume])//����������û�����ĳ��֣������ҵ������˳�
        max_volume--;//06.
    av_log(ctx, AV_LOG_INFO, "max_volume: %.1f dB\n", -logdb(max_volume * max_volume));//

    for (i = 0; i < 0x10000; i++)//2^16
        histdb[(int)logdb((i - 0x8000) * (i - 0x8000))] += vd->histogram[i];//07.ת��Ϊ��db��ʽ��KV��ʽ��ת��Ϊdb��KV������Ϊ����
    for (i = 0; i <= MAX_DB && !histdb[i]; i++);//08.����histdb[i]��Ϊ��ģ�����ӡʱ����ǰ��Ϊ0��ͳ�ơ�//ע�������Ϊ0����ʵ10^(0/20��=1�����ǵ�+0x8000��ͳ�Ƶ�ֵ��������0������91DB����
    for (; i <= MAX_DB && sum < nb_samples / 1000; i++) {//09.��ӡ��Ϊ������ݣ������i������0�����з�������0��Ϊʲô������Ϊͳ�Ƽ���0x8000
        av_log(ctx, AV_LOG_INFO, "histogram_%ddb: %"PRId64"\n", i, histdb[i]);//ע�⣺ʵ�����У���Щͳ��ֵ��nb_samples���Ƚϴ�==>�ԣ���Ϊsum< nb_samples / 1000��ֻ��ʾǧ��֮һ����(������������С�ıȽ϶࣬�ϴ�Ӧ��û��ô�������)
        sum += histdb[i];//��Ҫ������sum < nb_samples / 1000��ֻ��ʵǰ��1%�����ݣ�ɶԭ��
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    print_stats(ctx);//�����Ŵ�ӡ��ص�ͳ��
}

static const AVFilterPad volumedetect_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,//ͳ��
    },
    { NULL }
};

static const AVFilterPad volumedetect_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};
//ͳ�ƣ�����ӡƽ��ֵ�����ǧ��֮һ������
AVFilter ff_af_volumedetect = {
    .name          = "volumedetect",
    .description   = NULL_IF_CONFIG_SMALL("Detect audio volume."),
    .priv_size     = sizeof(VolDetectContext),
    .query_formats = query_formats,//֧�ֵĸ�ʽ
    .uninit        = uninit,//��ӡ��Ϣ
    .inputs        = volumedetect_inputs,//ͳ��
    .outputs       = volumedetect_outputs,//ɶҲ������
};
