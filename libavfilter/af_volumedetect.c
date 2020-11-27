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
     * The extra element is there for symmetry.对称没考虑到
     */
    uint64_t histogram[0x10001];//
} VolDetectContext;

static int query_formats(AVFilterContext *ctx)//TIGER 查询
{
    static const enum AVSampleFormat sample_fmts[] = {//01.只支持有符号16位！！
        AV_SAMPLE_FMT_S16,//有符号的16位
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

static int filter_frame(AVFilterLink *inlink, AVFrame *samples)//TIGER 统计
{
    AVFilterContext *ctx = inlink->dst;
    VolDetectContext *vd = ctx->priv;
    int nb_samples  = samples->nb_samples;
    int nb_channels = samples->channels;
    int nb_planes   = nb_channels;
    int plane, i;
    int16_t *pcm;

    if (!av_sample_fmt_is_planar(samples->format)) {//按存储方式不同
        nb_samples *= nb_channels;
        nb_planes = 1;//01.几个
    }
    for (plane = 0; plane < nb_planes; plane++) {//02.统计，有符号16位转为无符号统计，参见query_formats
        pcm = (int16_t *)samples->extended_data[plane];//转化为int16，那如果不是16位的呢？==>query_format声明只支持16位
        for (i = 0; i < nb_samples; i++)
            vd->histogram[pcm[i] + 0x8000]++;//pcm的采样值加2^15 ==> 为什么加0x8000? A.假设是int16,将采样值的负数搬移到正整数，即整数变成无符号数，方便统计
    }

    return ff_filter_frame(inlink->dst->outputs[0], samples);
}
//为什么是91？60DB是0.001 (10^(-60/20）)，16位的有符号是33；91dB是10^(-4.55)= 10^(0.45)*10^(-5)=2.81*10^(-5),16位的有符号是0.92即1,不能比这个更小了
#define MAX_DB 91

static inline double logdb(uint64_t v)//除以2^15的db
{
    double d = v / (double)(0x8000 * 0x8000);//为什么不写：2^30而是2^15*2^15，这样遵循物理意义
    if (!v)//如果是0，认为是最大db值
        return MAX_DB;
    return -log10(d) * 10;//是log10不是log2，这个d是除2^15的db
}

static void print_stats(AVFilterContext *ctx)
{
    VolDetectContext *vd = ctx->priv;
    int i, max_volume, shift;
    uint64_t nb_samples = 0, power = 0, nb_samples_shift = 0, sum = 0;
    uint64_t histdb[MAX_DB + 1] = { 0 };//初始化，均为0
    //01.无符号16位(0x10000),每个采样值都有统计值
    for (i = 0; i < 0x10000; i++)
        nb_samples += vd->histogram[i];
    av_log(ctx, AV_LOG_INFO, "n_samples: %"PRId64"\n", nb_samples);//打印统计值：所有的采样次数
    if (!nb_samples)
        return;
    //这个一般都不会考虑这么细， 比如时间比较长，导致统计数量太多，举例2^32，若pcm：8k*PCM A或u律,2^32/2^13=2^18秒，大概6天
    /* If nb_samples > 1<<34, there is a risk of overflow in the
       multiplication or the sum: shift all histogram values to avoid that.
       The total number of samples must be recomputed to avoid rounding
       errors. */
    shift = av_log2(nb_samples >> 33);//02. 33是如何得出的？//TODO:为什么不是32？有符号的i*i即16*16=32
    for (i = 0; i < 0x10000; i++) {//03.2^17即17位，histogram是uint64_t是64位的值，判断一下总数。shift最大为64-33位
        nb_samples_shift += vd->histogram[i] >> shift;
        power += (i - 0x8000) * (i - 0x8000) * (vd->histogram[i] >> shift);//为了统计方便，把负数+0x8000进行统计，计算的时候要变回去，所以是(i - 0x8000) 的平方
    }
    if (!nb_samples_shift)//04.
        return;
    power = (power + nb_samples_shift / 2) / nb_samples_shift;//05.还要除以nb_samples_shift，四舍五入；shift为0时nb_samples_shift为一般统计值，比较好理解
    av_assert0(power <= 0x8000 * 0x8000);//小于15+15位
    av_log(ctx, AV_LOG_INFO, "mean_volume: %.1f dB\n", -logdb(power));

    max_volume = 0x8000;
    while (max_volume > 0 && !vd->histogram[0x8000 + max_volume] &&//最大是2^15
                             !vd->histogram[0x8000 - max_volume])//看看正负有没有最大的出现，有则找到，并退出
        max_volume--;//06.
    av_log(ctx, AV_LOG_INFO, "max_volume: %.1f dB\n", -logdb(max_volume * max_volume));//

    for (i = 0; i < 0x10000; i++)//2^16
        histdb[(int)logdb((i - 0x8000) * (i - 0x8000))] += vd->histogram[i];//07.转化为按db方式的KV形式，转化为db，KV个数大为减少
    for (i = 0; i <= MAX_DB && !histdb[i]; i++);//08.查找histdb[i]不为零的，即打印时忽略前面为0的统计。//注意这里的为0，其实10^(0/20）=1，考虑到+0x8000，统计的值是在声音0附近的91DB的量
    for (; i <= MAX_DB && sum < nb_samples / 1000; i++) {//09.打印不为零的数据，这里的i是声音0，即有符号数的0，为什么这样因为统计加了0x8000
        av_log(ctx, AV_LOG_INFO, "histogram_%ddb: %"PRId64"\n", i, histdb[i]);//注意：实际运行，这些统计值和nb_samples相差比较大，==>对（因为sum< nb_samples / 1000，只显示千分之一）错(估计是声音较小的比较多，较大应该没这么大的声音)
        sum += histdb[i];//主要看这里sum < nb_samples / 1000，只现实前面1%的数据，啥原理？
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    print_stats(ctx);//在最后才打印相关的统计
}

static const AVFilterPad volumedetect_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,//统计
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
//统计，并打印平均值和最大千分之一的数据
AVFilter ff_af_volumedetect = {
    .name          = "volumedetect",
    .description   = NULL_IF_CONFIG_SMALL("Detect audio volume."),
    .priv_size     = sizeof(VolDetectContext),
    .query_formats = query_formats,//支持的格式
    .uninit        = uninit,//打印信息
    .inputs        = volumedetect_inputs,//统计
    .outputs       = volumedetect_outputs,//啥也不需做
};
