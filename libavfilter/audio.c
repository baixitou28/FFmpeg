/*
 * Copyright (c) Stefano Sabatini | stefasab at gmail.com
 * Copyright (c) S.N. Hemanth Meenakshisundaram | smeenaks at ucsd.edu
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

#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"

#include "audio.h"
#include "avfilter.h"
#include "internal.h"

#define BUFFER_ALIGN 0


AVFrame *ff_null_get_audio_buffer(AVFilterLink *link, int nb_samples)
{
    return ff_get_audio_buffer(link->dst->outputs[0], nb_samples);
}

AVFrame *ff_default_get_audio_buffer(AVFilterLink *link, int nb_samples)//tiger 获取指定nb_samples的
{
    AVFrame *frame = NULL;
    int channels = link->channels;

    av_assert0(channels == av_get_channel_layout_nb_channels(link->channel_layout) || !av_get_channel_layout_nb_channels(link->channel_layout));
    //01.
    if (!link->frame_pool) {//01.01 没有pool直接初始化一个
        link->frame_pool = ff_frame_pool_audio_init(av_buffer_allocz, channels,//01.01.01
                                                    nb_samples, link->format, BUFFER_ALIGN);
        if (!link->frame_pool)
            return NULL;
    } else {//01.02 如果存在，还需要每帧对比，是不是会比较慢?
        int pool_channels = 0;
        int pool_nb_samples = 0;
        int pool_align = 0;
        enum AVSampleFormat pool_format = AV_SAMPLE_FMT_NONE;
        //01.02.01 取参数，看是否不一致
        if (ff_frame_pool_get_audio_config(link->frame_pool,
                                           &pool_channels, &pool_nb_samples,
                                           &pool_format, &pool_align) < 0) {
            return NULL;
        }

        if (pool_channels != channels || pool_nb_samples < nb_samples ||
            pool_format != link->format || pool_align != BUFFER_ALIGN) {
            //01.02.02 如果参数不一致，释放再重新初始化一个新的，==>什么情况下出现？
            ff_frame_pool_uninit((FFFramePool **)&link->frame_pool);
            link->frame_pool = ff_frame_pool_audio_init(av_buffer_allocz, channels, //01.02.03 初始化
                                                        nb_samples, link->format, BUFFER_ALIGN);
            if (!link->frame_pool)
                return NULL;
        }
    }
    //02. 从pool里面分配AVFrame，并pool获取参数，设置AVFrame
    frame = ff_frame_pool_get(link->frame_pool);
    if (!frame)
        return NULL;
    //03.以link为准
    frame->nb_samples = nb_samples;
    frame->channel_layout = link->channel_layout;
    frame->sample_rate = link->sample_rate;
    //04.
    av_samples_set_silence(frame->extended_data, 0, nb_samples, channels, link->format);

    return frame;
}

AVFrame *ff_get_audio_buffer(AVFilterLink *link, int nb_samples)//分配AVFrame实例，并设置
{
    AVFrame *ret = NULL;

    if (link->dstpad->get_audio_buffer)//01. PAD还没有用到
        ret = link->dstpad->get_audio_buffer(link, nb_samples);

    if (!ret)//02. 
        ret = ff_default_get_audio_buffer(link, nb_samples);

    return ret;
}
