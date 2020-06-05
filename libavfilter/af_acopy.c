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

#include "audio.h"
#include "avfilter.h"
#include "internal.h"

static int filter_frame(AVFilterLink *inlink, AVFrame *in)//TIGER 统一入口函数
{
    AVFilterLink *outlink = inlink->dst->outputs[0];//01. 取第一个输出的AVFilterLink， 因为copy 只允许一个
    AVFrame *out = ff_get_audio_buffer(outlink, in->nb_samples);//02. AVFilterLink 的pool里分配AVFrame实例，并设置参数。

    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);//03.复制关键的参数，很多参数！
    av_frame_copy(out, in);//04.复制AVFrame *in到out
    av_frame_free(&in);//05. 释放in，copy后不允许其他操作
    return ff_filter_frame(outlink, out);//06.调用ff_filter_frame，输出
}

static const AVFilterPad acopy_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,//统一接口函数
    },
    { NULL }
};

static const AVFilterPad acopy_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_acopy = {
    .name          = "acopy",
    .description   = NULL_IF_CONFIG_SMALL("Copy the input audio unchanged to the output."),
    .inputs        = acopy_inputs,
    .outputs       = acopy_outputs,
};
