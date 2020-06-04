/*
 * Generic frame queue
 * Copyright (c) 2016 Nicolas George
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

#include "libavutil/avassert.h"
#include "framequeue.h"

static inline FFFrameBucket *bucket(FFFrameQueue *fq, size_t idx)//当前的bucket
{
    return &fq->queue[(fq->tail + idx) & (fq->allocated - 1)];//01.防止溢出  //02.这里的idx 是加fq->tail 
}

void ff_framequeue_global_init(FFFrameQueueGlobal *fqg)
{
}

static void check_consistency(FFFrameQueue *fq)
{
#if defined(ASSERT_LEVEL) && ASSERT_LEVEL >= 2
    uint64_t nb_samples = 0;
    size_t i;

    av_assert0(fq->queued == fq->total_frames_head - fq->total_frames_tail);
    for (i = 0; i < fq->queued; i++)
        nb_samples += bucket(fq, i)->frame->nb_samples;
    av_assert0(nb_samples == fq->total_samples_head - fq->total_samples_tail);
#endif
}

void ff_framequeue_init(FFFrameQueue *fq, FFFrameQueueGlobal *fqg)
{
    fq->queue = &fq->first_bucket;//很多时候，我们用一个就够了。
    fq->allocated = 1;
}

void ff_framequeue_free(FFFrameQueue *fq)
{
    while (fq->queued) {
        AVFrame *frame = ff_framequeue_take(fq);//提前取出fifo的数据，释放
        av_frame_free(&frame);
    }
    if (fq->queue != &fq->first_bucket)
        av_freep(&fq->queue);//释放fifo队列
}

int ff_framequeue_add(FFFrameQueue *fq, AVFrame *frame)//加入一帧，空间不够，按2的指数增加，且这是一个循环队列
{
    FFFrameBucket *b;
    
    check_consistency(fq);//校验
    if (fq->queued == fq->allocated) {//01. 如果空间不够
        if (fq->allocated == 1) {//01.01 如果只有1，fq->queue = &fq->first_bucket，需要重新分配
            size_t na = 8;//取8，减少从1，2，4，8变化的3次调用次数为1次
            FFFrameBucket *nq = av_realloc_array(NULL, na, sizeof(*nq));//分配8
            if (!nq)
                return AVERROR(ENOMEM);
            nq[0] = fq->queue[0];
            fq->queue = nq;
            fq->allocated = na;
        } else {
            size_t na = fq->allocated << 1;//空间倍增，特别注意大小都是2的指数，否则fq->tail就不对了。
            FFFrameBucket *nq = av_realloc_array(fq->queue, na, sizeof(*nq));//分配
            if (!nq)
                return AVERROR(ENOMEM);
            if (fq->tail + fq->queued > fq->allocated)//因为tail是循环的，倍增后，循环周期变了，所以要提前处理
                memmove(nq + fq->allocated, nq,// EFG---ABCD   ==> EFG---ABCDEFG-------
                        (fq->tail + fq->queued - fq->allocated) * sizeof(*nq));
            fq->queue = nq;
            fq->allocated = na;
        }
    }
    b = bucket(fq, fq->queued);//02. 定位
    b->frame = frame;//赋值
    fq->queued++;//03.统计值
    fq->total_frames_head++;
    fq->total_samples_head += frame->nb_samples;
    check_consistency(fq);//校验
    return 0;
}

AVFrame *ff_framequeue_take(FFFrameQueue *fq)//从循环队列中取
{
    FFFrameBucket *b;

    check_consistency(fq);
    av_assert1(fq->queued);
    b = bucket(fq, 0);//取当前帧
    fq->queued--;//总量减少
    fq->tail++;//增加
    fq->tail &= fq->allocated - 1;//01.防止溢出 02.指针位置周期性循环
    fq->total_frames_tail++;
    fq->total_samples_tail += b->frame->nb_samples;
    fq->samples_skipped = 0;//没有skip的采样值
    check_consistency(fq);
    return b->frame;
}

AVFrame *ff_framequeue_peek(FFFrameQueue *fq, size_t idx)//PEEK第几个帧
{
    FFFrameBucket *b;

    check_consistency(fq);
    av_assert1(idx < fq->queued);
    b = bucket(fq, idx);
    check_consistency(fq);
    return b->frame;
}

void ff_framequeue_skip_samples(FFFrameQueue *fq, size_t samples, AVRational time_base)//跳过n个采样值
{
    FFFrameBucket *b;
    size_t bytes;
    int planar, planes, i;

    check_consistency(fq);
    av_assert1(fq->queued);
    b = bucket(fq, 0);//取当前帧
    av_assert1(samples < b->frame->nb_samples);
    planar = av_sample_fmt_is_planar(b->frame->format);//是否是abab类型
    planes = planar ? b->frame->channels : 1;//有几个通道
    bytes = samples * av_get_bytes_per_sample(b->frame->format);
    if (!planar)
        bytes *= b->frame->channels;//乘以声道数
    if (b->frame->pts != AV_NOPTS_VALUE)
        b->frame->pts += av_rescale_q(samples, av_make_q(1, b->frame->sample_rate), time_base);//时间校准
    b->frame->nb_samples -= samples;//减去samples值
    b->frame->linesize[0] -= bytes;
    for (i = 0; i < planes; i++)
        b->frame->extended_data[i] += bytes;//多个通道，都要累加
    for (i = 0; i < planes && i < AV_NUM_DATA_POINTERS; i++)
        b->frame->data[i] = b->frame->extended_data[i];
    fq->total_samples_tail += samples;//
    fq->samples_skipped = 1;//标记需要跳过几位
    ff_framequeue_update_peeked(fq, 0);
}
