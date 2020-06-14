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
static int pad_last_frame(AVCodecContext *s, AVFrame **dst, const AVFrame *src)//��0����ɾ���
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

    ret = av_frame_copy_props(frame, src);//04��
    if (ret < 0)
        goto fail;
    
    if ((ret = av_samples_copy(frame->extended_data, src->extended_data, 0, 0,//05.
                               src->nb_samples, s->channels, s->sample_fmt)) < 0)
        goto fail;
    if ((ret = av_samples_set_silence(frame->extended_data, src->nb_samples,//06.��0��ʹ֮��ɾ���
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
                                              int *got_packet_ptr)//��ԭʼ�Ĳ���ֵ����Ϊָ���ı��룬��AVFrame-->AVPacket
{
    AVFrame *extended_frame = NULL;
    AVFrame *padded_frame = NULL;
    int ret;
    AVPacket user_pkt = *avpkt;//�Ȼ���һ���ϵ�AVPacket
    int needs_realloc = !user_pkt.data;//�Ƿ�AVPacketδ�����ڴ棬��Ҫ����

    *got_packet_ptr = 0;
    //01.У��
    if (!avctx->codec->encode2) {//�ܵ��б������� ���� ff_aac_encoder �� .encode2        = aac_encode_frame,//tiger aac ����������
        av_log(avctx, AV_LOG_ERROR, "This encoder requires using the avcodec_send_frame() API.\n");
        return AVERROR(ENOSYS);
    }

    if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY) && !frame) {//�����������AV_CODEC_CAP_DELAY����������frameΪ�գ����������һ��AVFrame frame���õ�һ��AVPacket avpkt
        av_packet_unref(avpkt);//�ǵ����ü�����1  ==>tiger program ����жϺ�ʹ��
        return 0;//���ص���0
    }
    //02.��Դ֡frame���·���extended_frame
    /* ensure that extended_data is properly set */
    if (frame && !frame->extended_data) {//������
        if (av_sample_fmt_is_planar(avctx->sample_fmt) &&
            avctx->channels > AV_NUM_DATA_POINTERS) {//02.01.����ǰ�ƽ�������棬����������8����������extended_data
            av_log(avctx, AV_LOG_ERROR, "Encoding to a planar sample format, "
                                        "with more than %d channels, but extended_data is not set.\n",
                   AV_NUM_DATA_POINTERS);
            return AVERROR(EINVAL);
        }
        av_log(avctx, AV_LOG_WARNING, "extended_data is not set.\n");
        //02.02. ����
        extended_frame = av_frame_alloc();
        if (!extended_frame)
            return AVERROR(ENOMEM);
        //02.03. ����
        memcpy(extended_frame, frame, sizeof(AVFrame));
        extended_frame->extended_data = extended_frame->data;
        frame = extended_frame;//02.04.  �ϵ�frame����Ҫɾ����==> ֻ��Դ����ʱ��
    }
    //03. ����
    /* extract audio service type metadata */
    if (frame) {//����:
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_AUDIO_SERVICE_TYPE);
        if (sd && sd->size >= sizeof(enum AVAudioServiceType))
            avctx->audio_service_type = *(enum AVAudioServiceType*)sd->data;
    }
    //04.
    /* check for valid frame size */
    if (frame) {
        if (avctx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) {//04.01. ����������֡Сһ��
            if (frame->nb_samples > avctx->frame_size) {//���Դframe�Ĳ���ֵ�ȱ��������ĵ�֡��
                av_log(avctx, AV_LOG_ERROR, "more samples than frame size (avcodec_encode_audio2)\n");//������
                ret = AVERROR(EINVAL);
                goto end;
            }
        } else if (!(avctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) {//04.02 ����������б�֡(����)������
            if (frame->nb_samples < avctx->frame_size &&//04.02.01���������δ������֡��Դframe�Ĳ���ֵ�ȱ��������ĵ�֡С�����侲����������������֡
                !avctx->internal->last_audio_frame) {
                ret = pad_last_frame(avctx, &padded_frame, frame);//��0��ɾ���
                if (ret < 0)
                    goto end;

                frame = padded_frame;//���µ�֡��//���ϵ�frame�أ�==>TIGER todo
                avctx->internal->last_audio_frame = 1;//�����ı�����֡
            }

            if (frame->nb_samples != avctx->frame_size) {//04.02.02 ���򣬻���Ҫ��Դframe�Ĳ���ֵ�ͱ��������ĵ�֡���
                av_log(avctx, AV_LOG_ERROR, "nb_samples (%d) != frame_size (%d) (avcodec_encode_audio2)\n", frame->nb_samples, avctx->frame_size);
                ret = AVERROR(EINVAL);
                goto end;
            }
        }
    }

    av_assert0(avctx->codec->encode2);
    //05. ������,���ñ�������encode2
    ret = avctx->codec->encode2(avctx, avpkt, frame, got_packet_ptr);//���� ff_aac_encoder �� .encode2        = aac_encode_frame,
    if (!ret) {//���ret==0������ɹ�
        if (*got_packet_ptr) {
            if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY)) {//��ö��ֵ��ע��If this flag is not set, the pts and duration will be determined by libavcodec from the input frame.
                if (avpkt->pts == AV_NOPTS_VALUE)//���AV_CODEC_CAP_DELAYδ���ã��������AVFrame* frame�������� ������amr,opus,speedxaac��H264���Ǵ�AV_CODEC_CAP_DELAY�����Բ������������pts
                    avpkt->pts = frame->pts;
                if (!avpkt->duration)
                    avpkt->duration = ff_samples_to_time_base(avctx,
                                                              frame->nb_samples);//�������������ʱ��
            }
            avpkt->dts = avpkt->pts;//����Ϊ��ͬ��
        } else {
            avpkt->size = 0;//��ǲ�����
        }
    }//06. ����ѷ����ڴ棬��ָ���ڲ���ʱ��buffer����Ҫcopy���������ܱ���find_stream_info
    if (avpkt->data && avpkt->data == avctx->internal->byte_buffer) {
        needs_realloc = 0;
        if (user_pkt.data) {//���ԭ����user_pkt.data
            if (user_pkt.size >= avpkt->size) {
                memcpy(user_pkt.data, avpkt->data, avpkt->size);//�ռ乻�����Ƶ���
            } else {
                av_log(avctx, AV_LOG_ERROR, "Provided packet is too small, needs to be %d\n", avpkt->size);//����Ŀ���buffer̫С��û�취
                avpkt->size = user_pkt.size;
                ret = -1;
            }
            avpkt->buf      = user_pkt.buf;//ֱ��ʹ���ϵ�buffer����   //avctx->internal->byte_buffer�������ͷ�
            avpkt->data     = user_pkt.data;
        } else if (!avpkt->buf) {//���ԭ��û��user_pkt.data�����ǲ������
            ret = av_packet_make_refcounted(avpkt);
            if (ret < 0)
                goto end;
        }
    }
    //07.needs_realloc=1˵��ԭ��û��avpkt->data˵�������� ===>?
    if (!ret) {
        if (needs_realloc && avpkt->data) {//���ԭ��avpkt->dataû��ֵ��������avpkt->data ��ֵ����Ҫ����һ�¶����һ��
            ret = av_buffer_realloc(&avpkt->buf, avpkt->size + AV_INPUT_BUFFER_PADDING_SIZE);//��dataע�ͣ��������ռ䲻��
            if (ret >= 0)
                avpkt->data = avpkt->buf->data;//����ָ��
        }
        if (frame)
            avctx->frame_number++;//���Ӽ���
    }

    if (ret < 0 || !*got_packet_ptr) {
        av_packet_unref(avpkt);//�쳣�Ļ�������������1
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
    int needs_realloc = !user_pkt.data;//���ԭ��avpkt->dataû������

    *got_packet_ptr = 0; 
    //01.��֤
    if (!avctx->codec->encode2) {//����avctx->codec->encode2��X264_frame  ;avctx->codec= ff_libx264_encoder>
        av_log(avctx, AV_LOG_ERROR, "This encoder requires using the avcodec_send_frame() API.\n");
        return AVERROR(ENOSYS);
    }
    //���ö����߳̽�һ֡
    if(CONFIG_FRAME_THREAD_ENCODER &&
       avctx->internal->frame_thread_encoder && (avctx->active_thread_type&FF_THREAD_FRAME))
        return ff_thread_video_encode_frame(avctx, avpkt, frame, got_packet_ptr);
    //ͳ���ַ�����Ϊ��
    if ((avctx->flags&AV_CODEC_FLAG_PASS1) && avctx->stats_out)
        avctx->stats_out[0] = '\0';
    //�������delay���ǲ���Ҫ��null֡���ر�
    if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY) && !frame) {
        av_packet_unref(avpkt);
        return 0;//ֱ�ӷ���0����Ҫ��Ϊ�˼��ݺ�AV_CODEC_CAP_DELAY�ӿڵ�ʵ�֡�
    }
    //tiger �����Ҫ��?�о����Բ��õ���
    if (av_image_check_size2(avctx->width, avctx->height, avctx->max_pixels, AV_PIX_FMT_NONE, 0, avctx))
        return AVERROR(EINVAL);
    //��Ƶ��Ҫ��һ��Ҫ���ã�����澯
    if (frame && frame->format == AV_PIX_FMT_NONE)
        av_log(avctx, AV_LOG_WARNING, "AVFrame.format is not set\n");
    if (frame && (frame->width == 0 || frame->height == 0))
        av_log(avctx, AV_LOG_WARNING, "AVFrame.width or height is not set\n");

    av_assert0(avctx->codec->encode2);
    //02.���ñ�������encode2����
    ret = avctx->codec->encode2(avctx, avpkt, frame, got_packet_ptr);
    av_assert0(ret <= 0);
    
    emms_c();
    //03. �����̽���avctx->internal->byte_buffer��
    if (avpkt->data && avpkt->data == avctx->internal->byte_buffer) {
        needs_realloc = 0;
        if (user_pkt.data) {
            if (user_pkt.size >= avpkt->size) {
                memcpy(user_pkt.data, avpkt->data, avpkt->size);//����
            } else {
                av_log(avctx, AV_LOG_ERROR, "Provided packet is too small, needs to be %d\n", avpkt->size);
                avpkt->size = user_pkt.size;
                ret = -1;
            }
            avpkt->buf      = user_pkt.buf;// ����avpkt �ͷŵ�ʱ��buf����ͷţ�==>
            avpkt->data     = user_pkt.data;
        } else if (!avpkt->buf) {//���user_pkt.dataΪ�գ���ֱ����avpkt
            ret = av_packet_make_refcounted(avpkt);
            if (ret < 0)
                return ret;
        }
    }
    //04.�������ɹ�
    if (!ret) {
        if (!*got_packet_ptr)
            avpkt->size = 0;//���û�еõ�һ֡������Ϊ0
        else if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY))
            avpkt->pts = avpkt->dts = frame->pts;//�������û��delay���ã�ֱ����Դframe����Ŀ��AVPacket��pts��dts

        if (needs_realloc && avpkt->data) {//realloc��Ϊ�ˣ���ע�ͣ������������ //This is mainly needed because some optimized bitstream readers read 32 or 64 bit at once and could read over the end.
            ret = av_buffer_realloc(&avpkt->buf, avpkt->size + AV_INPUT_BUFFER_PADDING_SIZE);//�����ܵ����쳣�����Լ�64 // tiger todo ==>��show me the code
            if (ret >= 0)
                avpkt->data = avpkt->buf->data;//����ָ��
        }

        if (frame)
            avctx->frame_number++;//����
    }
    //05.�������ֵ�Ǳ���ʧ�ܵ�ԭ�����û�õ���
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
    //02.��������Ƶ�����ò�ͬ����
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
    if (avctx->codec->send_frame)//����
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
    if (avctx->codec->receive_packet) {//����ֻ�м���Ӳ��������� �磺ff_nvenc_encoder .receive_packet = ff_nvenc_receive_packet,
        if (avctx->internal->draining && !(avctx->codec->capabilities & AV_CODEC_CAP_DELAY))
            return AVERROR_EOF;
        return avctx->codec->receive_packet(avctx, avpkt);
    }

    // Emulation via old API.
    //03. ��ʱ������
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
