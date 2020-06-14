/*
 * generic encoding-related code
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

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/samplefmt.h"

#include "avcodec.h"
#include "frame_thread_encoder.h"
#include "internal.h"

int ff_alloc_packet2(AVCodecContext *avctx, AVPacket *avpkt, int64_t size, int64_t min_size)
{
    if (avpkt->size < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid negative user packet size %d\n", avpkt->size);
        return AVERROR(EINVAL);
    }
    if (size < 0 || size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "Invalid minimum required packet size %"PRId64" (max allowed is %d)\n",
               size, INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE);
        return AVERROR(EINVAL);
    }

    if (avctx && 2*min_size < size) { // FIXME The factor needs to be finetuned
        av_assert0(!avpkt->data || avpkt->data != avctx->internal->byte_buffer);
        if (!avpkt->data || avpkt->size < size) {
            av_fast_padded_malloc(&avctx->internal->byte_buffer, &avctx->internal->byte_buffer_size, size);
            avpkt->data = avctx->internal->byte_buffer;
            avpkt->size = avctx->internal->byte_buffer_size;
        }
    }

    if (avpkt->data) {
        AVBufferRef *buf = avpkt->buf;

        if (avpkt->size < size) {
            av_log(avctx, AV_LOG_ERROR, "User packet is too small (%d < %"PRId64")\n", avpkt->size, size);
            return AVERROR(EINVAL);
        }

        av_init_packet(avpkt);
        avpkt->buf      = buf;
        avpkt->size     = size;
        return 0;
    } else {
        int ret = av_new_packet(avpkt, size);
        if (ret < 0)
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate packet of size %"PRId64"\n", size);
        return ret;
    }
}

int ff_alloc_packet(AVPacket *avpkt, int size)
{
    return ff_alloc_packet2(NULL, avpkt, size, 0);
}

/**
 * Pad last frame with silence.
 */
static int pad_last_frame(AVCodecContext *s, AVFrame **dst, const AVFrame *src)//填0，变成静音
{
    AVFrame *frame = NULL;
    int ret;
    //01.
    if (!(frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    //02.
    frame->format         = src->format;
    frame->channel_layout = src->channel_layout;
    frame->channels       = src->channels;
    frame->nb_samples     = s->frame_size;
    ret = av_frame_get_buffer(frame, 32);//03.
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(frame, src);//04，
    if (ret < 0)
        goto fail;
    
    if ((ret = av_samples_copy(frame->extended_data, src->extended_data, 0, 0,//05.
                               src->nb_samples, s->channels, s->sample_fmt)) < 0)
        goto fail;
    if ((ret = av_samples_set_silence(frame->extended_data, src->nb_samples,//06.填0，使之变成静音
                                      frame->nb_samples - src->nb_samples,
                                      s->channels, s->sample_fmt)) < 0)
        goto fail;
    //07.
    *dst = frame;

    return 0;

fail:
    av_frame_free(&frame);
    return ret;
}

int attribute_align_arg avcodec_encode_audio2(AVCodecContext *avctx,
                                              AVPacket *avpkt,
                                              const AVFrame *frame,
                                              int *got_packet_ptr)//从原始的采样值，变为指定的编码，从AVFrame-->AVPacket
{
    AVFrame *extended_frame = NULL;
    AVFrame *padded_frame = NULL;
    int ret;
    AVPacket user_pkt = *avpkt;//先缓存一下老的AVPacket
    int needs_realloc = !user_pkt.data;//是否AVPacket未分配内存，需要分配

    *got_packet_ptr = 0;
    //01.校验
    if (!avctx->codec->encode2) {//总得有编码器吧 举例 ff_aac_encoder 的 .encode2        = aac_encode_frame,//tiger aac 编码主函数
        av_log(avctx, AV_LOG_ERROR, "This encoder requires using the avcodec_send_frame() API.\n");
        return AVERROR(ENOSYS);
    }

    if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY) && !frame) {//如果编码器有AV_CODEC_CAP_DELAY能力，允许frame为空，否则就是来一个AVFrame frame，得到一个AVPacket avpkt
        av_packet_unref(avpkt);//记得引用计数减1  ==>tiger program 如何判断和使用
        return 0;//返回的是0
    }
    //02.将源帧frame重新分配extended_frame
    /* ensure that extended_data is properly set */
    if (frame && !frame->extended_data) {//举例：
        if (av_sample_fmt_is_planar(avctx->sample_fmt) &&
            avctx->channels > AV_NUM_DATA_POINTERS) {//02.01.如果是按平面来保存，且声道大于8，必须设置extended_data
            av_log(avctx, AV_LOG_ERROR, "Encoding to a planar sample format, "
                                        "with more than %d channels, but extended_data is not set.\n",
                   AV_NUM_DATA_POINTERS);
            return AVERROR(EINVAL);
        }
        av_log(avctx, AV_LOG_WARNING, "extended_data is not set.\n");
        //02.02. 分配
        extended_frame = av_frame_alloc();
        if (!extended_frame)
            return AVERROR(ENOMEM);
        //02.03. 复制
        memcpy(extended_frame, frame, sizeof(AVFrame));
        extended_frame->extended_data = extended_frame->data;
        frame = extended_frame;//02.04.  老的frame不需要删除？==> 只是源，临时用
    }
    //03. 复制
    /* extract audio service type metadata */
    if (frame) {//举例:
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_AUDIO_SERVICE_TYPE);
        if (sd && sd->size >= sizeof(enum AVAudioServiceType))
            avctx->audio_service_type = *(enum AVAudioServiceType*)sd->data;
    }
    //04.
    /* check for valid frame size */
    if (frame) {
        if (avctx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) {//04.01. 编码器允许帧小一点
            if (frame->nb_samples > avctx->frame_size) {//如果源frame的采样值比编码上下文的帧大
                av_log(avctx, AV_LOG_ERROR, "more samples than frame size (avcodec_encode_audio2)\n");//不允许
                ret = AVERROR(EINVAL);
                goto end;
            }
        } else if (!(avctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) {//04.02 如果编码器有变帧(长度)的能力
            if (frame->nb_samples < avctx->frame_size &&//04.02.01如果上下文未标记最后帧，源frame的采样值比编码上下文的帧小，补充静音，标记上下文最后帧
                !avctx->internal->last_audio_frame) {
                ret = pad_last_frame(avctx, &padded_frame, frame);//填0变成静音
                if (ret < 0)
                    goto end;

                frame = padded_frame;//用新的帧，//那老的frame呢？==>TIGER todo
                avctx->internal->last_audio_frame = 1;//上下文标记最后帧
            }

            if (frame->nb_samples != avctx->frame_size) {//04.02.02 否则，还是要求源frame的采样值和编码上下文的帧相等
                av_log(avctx, AV_LOG_ERROR, "nb_samples (%d) != frame_size (%d) (avcodec_encode_audio2)\n", frame->nb_samples, avctx->frame_size);
                ret = AVERROR(EINVAL);
                goto end;
            }
        }
    }

    av_assert0(avctx->codec->encode2);
    //05. 主函数,调用编码器的encode2
    ret = avctx->codec->encode2(avctx, avpkt, frame, got_packet_ptr);//举例 ff_aac_encoder 的 .encode2        = aac_encode_frame,
    if (!ret) {//如果ret==0，解码成功
        if (*got_packet_ptr) {
            if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY)) {//看枚举值的注释If this flag is not set, the pts and duration will be determined by libavcodec from the input frame.
                if (avpkt->pts == AV_NOPTS_VALUE)//如果AV_CODEC_CAP_DELAY未设置，由输入的AVFrame* frame来决定， 常见的amr,opus,speedxaac和H264都是带AV_CODEC_CAP_DELAY，所以不是由输入决定pts
                    avpkt->pts = frame->pts;
                if (!avpkt->duration)
                    avpkt->duration = ff_samples_to_time_base(avctx,
                                                              frame->nb_samples);//采样数量换算出时间
            }
            avpkt->dts = avpkt->pts;//设置为相同？
        } else {
            avpkt->size = 0;//标记不可用
        }
    }//06. 如果已分配内存，且指向内部临时的buffer，需要copy出来，可能比如find_stream_info
    if (avpkt->data && avpkt->data == avctx->internal->byte_buffer) {
        needs_realloc = 0;
        if (user_pkt.data) {//如果原先有user_pkt.data
            if (user_pkt.size >= avpkt->size) {
                memcpy(user_pkt.data, avpkt->data, avpkt->size);//空间够，复制得下
            } else {
                av_log(avctx, AV_LOG_ERROR, "Provided packet is too small, needs to be %d\n", avpkt->size);//编码目标的buffer太小，没办法
                avpkt->size = user_pkt.size;
                ret = -1;
            }
            avpkt->buf      = user_pkt.buf;//直接使用老的buffer即可   //avctx->internal->byte_buffer可自行释放
            avpkt->data     = user_pkt.data;
        } else if (!avpkt->buf) {//如果原先没有user_pkt.data，这是不允许的
            ret = av_packet_make_refcounted(avpkt);
            if (ret < 0)
                goto end;
        }
    }
    //07.needs_realloc=1说明原先没，avpkt->data说明现在有 ===>?
    if (!ret) {
        if (needs_realloc && avpkt->data) {//如果原先avpkt->data没有值，但现在avpkt->data 有值，需要考虑一下多分配一点
            ret = av_buffer_realloc(&avpkt->buf, avpkt->size + AV_INPUT_BUFFER_PADDING_SIZE);//看data注释，避免解码空间不够
            if (ret >= 0)
                avpkt->data = avpkt->buf->data;//重新指向
        }
        if (frame)
            avctx->frame_number++;//增加计数
    }

    if (ret < 0 || !*got_packet_ptr) {
        av_packet_unref(avpkt);//异常的话，主动计数减1
        goto end;
    }
    //08.
    /* NOTE: if we add any audio encoders which output non-keyframe packets,
     *       this needs to be moved to the encoders, but for now we can do it
     *       here to simplify things */
    avpkt->flags |= AV_PKT_FLAG_KEY;//

end:
    av_frame_free(&padded_frame);
    av_free(extended_frame);

    return ret;
}

int attribute_align_arg avcodec_encode_video2(AVCodecContext *avctx,
                                              AVPacket *avpkt,
                                              const AVFrame *frame,
                                              int *got_packet_ptr)
{
    int ret;
    AVPacket user_pkt = *avpkt;
    int needs_realloc = !user_pkt.data;//如果原先avpkt->data没有数据

    *got_packet_ptr = 0; 
    //01.验证
    if (!avctx->codec->encode2) {//例如avctx->codec->encode2：X264_frame  ;avctx->codec= ff_libx264_encoder>
        av_log(avctx, AV_LOG_ERROR, "This encoder requires using the avcodec_send_frame() API.\n");
        return AVERROR(ENOSYS);
    }
    //采用独立线程解一帧
    if(CONFIG_FRAME_THREAD_ENCODER &&
       avctx->internal->frame_thread_encoder && (avctx->active_thread_type&FF_THREAD_FRAME))
        return ff_thread_video_encode_frame(avctx, avpkt, frame, got_packet_ptr);
    //统计字符串设为空
    if ((avctx->flags&AV_CODEC_FLAG_PASS1) && avctx->stats_out)
        avctx->stats_out[0] = '\0';
    //如果不含delay，是不需要用null帧来关闭
    if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY) && !frame) {
        av_packet_unref(avpkt);
        return 0;//直接返回0，主要是为了兼容含AV_CODEC_CAP_DELAY接口的实现。
    }
    //tiger 这个需要吗?感觉调试才用的着
    if (av_image_check_size2(avctx->width, avctx->height, avctx->max_pixels, AV_PIX_FMT_NONE, 0, avctx))
        return AVERROR(EINVAL);
    //视频三要素一定要设置，否则告警
    if (frame && frame->format == AV_PIX_FMT_NONE)
        av_log(avctx, AV_LOG_WARNING, "AVFrame.format is not set\n");
    if (frame && (frame->width == 0 || frame->height == 0))
        av_log(avctx, AV_LOG_WARNING, "AVFrame.width or height is not set\n");

    av_assert0(avctx->codec->encode2);
    //02.调用编码器的encode2函数
    ret = avctx->codec->encode2(avctx, avpkt, frame, got_packet_ptr);
    av_assert0(ret <= 0);
    
    emms_c();
    //03. 如果是探测的avctx->internal->byte_buffer？
    if (avpkt->data && avpkt->data == avctx->internal->byte_buffer) {
        needs_realloc = 0;
        if (user_pkt.data) {
            if (user_pkt.size >= avpkt->size) {
                memcpy(user_pkt.data, avpkt->data, avpkt->size);//复制
            } else {
                av_log(avctx, AV_LOG_ERROR, "Provided packet is too small, needs to be %d\n", avpkt->size);
                avpkt->size = user_pkt.size;
                ret = -1;
            }
            avpkt->buf      = user_pkt.buf;// 这样avpkt 释放的时候buf如何释放？==>
            avpkt->data     = user_pkt.data;
        } else if (!avpkt->buf) {//如果user_pkt.data为空，那直接用avpkt
            ret = av_packet_make_refcounted(avpkt);
            if (ret < 0)
                return ret;
        }
    }
    //04.如果编码成功
    if (!ret) {
        if (!*got_packet_ptr)
            avpkt->size = 0;//如果没有得到一帧，设置为0
        else if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY))
            avpkt->pts = avpkt->dts = frame->pts;//如果编码没有delay设置，直接用源frame设置目标AVPacket的pts，dts

        if (needs_realloc && avpkt->data) {//realloc是为了（看注释）避免解码问题 //This is mainly needed because some optimized bitstream readers read 32 or 64 bit at once and could read over the end.
            ret = av_buffer_realloc(&avpkt->buf, avpkt->size + AV_INPUT_BUFFER_PADDING_SIZE);//读可能导致异常，所以加64 // tiger todo ==>但show me the code
            if (ret >= 0)
                avpkt->data = avpkt->buf->data;//重新指向
        }

        if (frame)
            avctx->frame_number++;//计数
    }
    //05.如果返回值是编码失败等原因或者没得到包
    if (ret < 0 || !*got_packet_ptr)
        av_packet_unref(avpkt);

    return ret;
}

int avcodec_encode_subtitle(AVCodecContext *avctx, uint8_t *buf, int buf_size,
                            const AVSubtitle *sub)
{
    int ret;
    if (sub->start_display_time) {
        av_log(avctx, AV_LOG_ERROR, "start_display_time must be 0.\n");
        return -1;
    }

    ret = avctx->codec->encode_sub(avctx, buf, buf_size, sub);
    avctx->frame_number++;
    return ret;
}

static int do_encode(AVCodecContext *avctx, const AVFrame *frame, int *got_packet)
{
    int ret;
    *got_packet = 0;
    //01.
    av_packet_unref(avctx->internal->buffer_pkt);
    avctx->internal->buffer_pkt_valid = 0;
    //02.根据音视频，调用不同函数
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = avcodec_encode_video2(avctx, avctx->internal->buffer_pkt,
                                    frame, got_packet);
    } else if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        ret = avcodec_encode_audio2(avctx, avctx->internal->buffer_pkt,
                                    frame, got_packet);
    } else {
        ret = AVERROR(EINVAL);
    }
    //03.
    if (ret >= 0 && *got_packet) {
        // Encoders must always return ref-counted buffers.
        // Side-data only packets have no data and can be not ref-counted.
        av_assert0(!avctx->internal->buffer_pkt->data || avctx->internal->buffer_pkt->buf);
        avctx->internal->buffer_pkt_valid = 1;
        ret = 0;
    } else {
        av_packet_unref(avctx->internal->buffer_pkt);
    }

    return ret;
}

int attribute_align_arg avcodec_send_frame(AVCodecContext *avctx, const AVFrame *frame)//TIGER avcodec_send_frame-->do_encode-->avcodec_encode_audio2/avcodec_encode_video2
{   
    if (!avcodec_is_open(avctx) || !av_codec_is_encoder(avctx->codec))
        return AVERROR(EINVAL);
    //01.
    if (avctx->internal->draining)
        return AVERROR_EOF;
    //02.
    if (!frame) {
        avctx->internal->draining = 1;

        if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY))
            return 0;
    }
    //03.
    if (avctx->codec->send_frame)//举例
        return avctx->codec->send_frame(avctx, frame);

    // Emulation via old API. Do it here instead of avcodec_receive_packet, because:
    // 1. if the AVFrame is not refcounted, the copying will be much more
    //    expensive than copying the packet data
    // 2. assume few users use non-refcounted AVPackets, so usually no copy is
    //    needed
    //04.
    if (avctx->internal->buffer_pkt_valid)
        return AVERROR(EAGAIN);
    //05.
    return do_encode(avctx, frame, &(int){0});
}

int attribute_align_arg avcodec_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    av_packet_unref(avpkt);
    //01.
    if (!avcodec_is_open(avctx) || !av_codec_is_encoder(avctx->codec))
        return AVERROR(EINVAL);
    //02.
    if (avctx->codec->receive_packet) {//举例只有几个硬件编码才有 如：ff_nvenc_encoder .receive_packet = ff_nvenc_receive_packet,
        if (avctx->internal->draining && !(avctx->codec->capabilities & AV_CODEC_CAP_DELAY))
            return AVERROR_EOF;
        return avctx->codec->receive_packet(avctx, avpkt);
    }

    // Emulation via old API.
    //03. 暂时不看吧
    if (!avctx->internal->buffer_pkt_valid) {
        int got_packet;
        int ret;
        if (!avctx->internal->draining)
            return AVERROR(EAGAIN);
        ret = do_encode(avctx, NULL, &got_packet);
        if (ret < 0)
            return ret;
        if (ret >= 0 && !got_packet)
            return AVERROR_EOF;
    }

    av_packet_move_ref(avpkt, avctx->internal->buffer_pkt);
    avctx->internal->buffer_pkt_valid = 0;
    return 0;
}
