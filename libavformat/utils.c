/*
 * various utility functions for use within FFmpeg
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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

#include <stdarg.h>
#include <stdint.h>

#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"
#include "libavutil/time_internal.h"
#include "libavutil/timestamp.h"

#include "libavcodec/bytestream.h"
#include "libavcodec/internal.h"
#include "libavcodec/raw.h"

#include "audiointerleave.h"
#include "avformat.h"
#include "avio_internal.h"
#include "id3v2.h"
#include "internal.h"
#include "metadata.h"
#if CONFIG_NETWORK
#include "network.h"
#endif
#include "riff.h"
#include "url.h"

#include "libavutil/ffversion.h"
const char av_format_ffversion[] = "FFmpeg version " FFMPEG_VERSION;

static AVMutex avformat_mutex = AV_MUTEX_INITIALIZER;

/**
 * @file
 * various utility functions for use within FFmpeg
 */

unsigned avformat_version(void)
{
    av_assert0(LIBAVFORMAT_VERSION_MICRO >= 100);
    return LIBAVFORMAT_VERSION_INT;
}

const char *avformat_configuration(void)
{
    return FFMPEG_CONFIGURATION;
}

const char *avformat_license(void)
{
#define LICENSE_PREFIX "libavformat license: "
    return LICENSE_PREFIX FFMPEG_LICENSE + sizeof(LICENSE_PREFIX) - 1;
}

int ff_lock_avformat(void)
{
    return ff_mutex_lock(&avformat_mutex) ? -1 : 0;
}

int ff_unlock_avformat(void)
{
    return ff_mutex_unlock(&avformat_mutex) ? -1 : 0;
}

#define RELATIVE_TS_BASE (INT64_MAX - (1LL<<48))
//tiger
static int is_relative(int64_t ts) {
    return ts > (RELATIVE_TS_BASE - (1LL<<48));
}

/**
 * Wrap a given time stamp, if there is an indication for an overflow
 *
 * @param st stream
 * @param timestamp the time stamp to wrap
 * @return resulting time stamp
 */
static int64_t wrap_timestamp(const AVStream *st, int64_t timestamp)
{
    if (st->pts_wrap_behavior != AV_PTS_WRAP_IGNORE &&
        st->pts_wrap_reference != AV_NOPTS_VALUE && timestamp != AV_NOPTS_VALUE) {
        if (st->pts_wrap_behavior == AV_PTS_WRAP_ADD_OFFSET &&
            timestamp < st->pts_wrap_reference)
            return timestamp + (1ULL << st->pts_wrap_bits);
        else if (st->pts_wrap_behavior == AV_PTS_WRAP_SUB_OFFSET &&
            timestamp >= st->pts_wrap_reference)
            return timestamp - (1ULL << st->pts_wrap_bits);
    }
    return timestamp;
}

#if FF_API_FORMAT_GET_SET
MAKE_ACCESSORS(AVStream, stream, AVRational, r_frame_rate)//tiger 用宏，简洁，但查找困难
#if FF_API_LAVF_FFSERVER
FF_DISABLE_DEPRECATION_WARNINGS
MAKE_ACCESSORS(AVStream, stream, char *, recommended_encoder_configuration)
FF_ENABLE_DEPRECATION_WARNINGS
#endif
MAKE_ACCESSORS(AVFormatContext, format, AVCodec *, video_codec)
MAKE_ACCESSORS(AVFormatContext, format, AVCodec *, audio_codec)
MAKE_ACCESSORS(AVFormatContext, format, AVCodec *, subtitle_codec)
MAKE_ACCESSORS(AVFormatContext, format, AVCodec *, data_codec)
MAKE_ACCESSORS(AVFormatContext, format, int, metadata_header_padding)
MAKE_ACCESSORS(AVFormatContext, format, void *, opaque)
MAKE_ACCESSORS(AVFormatContext, format, av_format_control_message, control_message_cb)
#if FF_API_OLD_OPEN_CALLBACKS
FF_DISABLE_DEPRECATION_WARNINGS
MAKE_ACCESSORS(AVFormatContext, format, AVOpenCallback, open_cb)
FF_ENABLE_DEPRECATION_WARNINGS
#endif
#endif

int64_t av_stream_get_end_pts(const AVStream *st)
{
    if (st->internal->priv_pts) {
        return st->internal->priv_pts->val;
    } else
        return AV_NOPTS_VALUE;
}

struct AVCodecParserContext *av_stream_get_parser(const AVStream *st)
{
    return st->parser;
}

void av_format_inject_global_side_data(AVFormatContext *s)
{
    int i;
    s->internal->inject_global_side_data = 1;
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        st->inject_global_side_data = 1;
    }
}

int ff_copy_whiteblacklists(AVFormatContext *dst, const AVFormatContext *src)
{
    av_assert0(!dst->codec_whitelist &&
               !dst->format_whitelist &&
               !dst->protocol_whitelist &&
               !dst->protocol_blacklist);
    dst-> codec_whitelist = av_strdup(src->codec_whitelist);
    dst->format_whitelist = av_strdup(src->format_whitelist);
    dst->protocol_whitelist = av_strdup(src->protocol_whitelist);
    dst->protocol_blacklist = av_strdup(src->protocol_blacklist);
    if (   (src-> codec_whitelist && !dst-> codec_whitelist)
        || (src->  format_whitelist && !dst->  format_whitelist)
        || (src->protocol_whitelist && !dst->protocol_whitelist)
        || (src->protocol_blacklist && !dst->protocol_blacklist)) {
        av_log(dst, AV_LOG_ERROR, "Failed to duplicate black/whitelist\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static const AVCodec *find_decoder(AVFormatContext *s, const AVStream *st, enum AVCodecID codec_id)//TIGER 对于AVFormatContext，查找就是判断AVFormatContext是否已查找过并保存，不存在才再查找
{
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    if (st->codec->codec)//这个是老的接口
        return st->codec->codec;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    switch (st->codecpar->codec_type) {//一般已经存在，所以直接返回即可，这样可以快一点
    case AVMEDIA_TYPE_VIDEO:
        if (s->video_codec)    return s->video_codec;
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (s->audio_codec)    return s->audio_codec;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (s->subtitle_codec) return s->subtitle_codec;
        break;
    }

    return avcodec_find_decoder(codec_id);//以前没有查找过，就再次查找。
}
//TIGER 
static const AVCodec *find_probe_decoder(AVFormatContext *s, const AVStream *st, enum AVCodecID codec_id)
{
    const AVCodec *codec;
    //01. tiger CONFIG_H264_DECODER 在哪里定义？==> configure中
#if CONFIG_H264_DECODER
    /* Other parts of the code assume this decoder to be used for h264,
     * so force it if possible. */
    if (codec_id == AV_CODEC_ID_H264)
        return avcodec_find_decoder_by_name("h264");//兼容
#endif

    codec = find_decoder(s, st, codec_id);//02.找解码器
    if (!codec)
        return NULL;

    if (codec->capabilities & AV_CODEC_CAP_AVOID_PROBING) {//03.若允许probe
        const AVCodec *probe_codec = NULL;
        while (probe_codec = av_codec_next(probe_codec)) {//04.逐个对比
            if (probe_codec->id == codec_id &&
                    av_codec_is_decoder(probe_codec) &&//05.如果是ID相同的解码器
                    !(probe_codec->capabilities & (AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_EXPERIMENTAL))) {
                return probe_codec;
            }
        }
    }

    return codec;
}

#if FF_API_FORMAT_GET_SET
int av_format_get_probe_score(const AVFormatContext *s)
{
    return s->probe_score;
}
#endif

/* an arbitrarily chosen "sane" max packet size -- 50M */
#define SANE_CHUNK_SIZE (50000000)
//tiger 有点绕 ==> 没完全理解
int ffio_limit(AVIOContext *s, int size)
{
    if (s->maxsize>= 0) {//如果s->maxsize不为负
        int64_t remaining= s->maxsize - avio_tell(s);
        if (remaining < size) {//而且剩余的长度比最大值还小，
            int64_t newsize = avio_size(s);
            if (!s->maxsize || s->maxsize<newsize)
                s->maxsize = newsize - !newsize;//如果最大值为0 或者小于newsize，则设置为newsize，
            remaining= s->maxsize - avio_tell(s);//需要调整为
            remaining= FFMAX(remaining, 0);//至少为0
        }

        if (s->maxsize>= 0 && remaining+1 < size) {//如果？
            av_log(NULL, remaining ? AV_LOG_ERROR : AV_LOG_DEBUG, "Truncating packet of size %d to %"PRId64"\n", size, remaining+1);
            size = remaining+1;//多读1个字节？
        }
    }
    return size;
}

/* Read the data in sane-sized chunks and append to pkt.
 * Return the number of bytes read or an error. */
static int append_packet_chunked(AVIOContext *s, AVPacket *pkt, int size)//少量多次循环读avio_read，可能循环多次。内存不够自动分配
{
    int64_t orig_pos   = pkt->pos; // av_grow_packet might reset pos
    int orig_size      = pkt->size;//允许pkt原来有数据
    int ret;

    do {
        int prev_size = pkt->size;
        int read_size;

        /* When the caller requests a lot of data, limit it to the amount
         * left in file or SANE_CHUNK_SIZE when it is not known. */
        read_size = size;
        if (read_size > SANE_CHUNK_SIZE/10) {//修正read_size范围，不要太大，但SANE_CHUNK_SIZE/10 也有5M，够大的
            read_size = ffio_limit(s, read_size);//io 可能没这么大，根据io长度修正read_size读数。
            // If filesize/maxsize is unknown, limit to SANE_CHUNK_SIZE
            if (s->maxsize < 0)//tiger 注释说负数为不知道的情况
                read_size = FFMIN(read_size, SANE_CHUNK_SIZE);//好吧，我就只能填个50M
        }
        //tiger 分配read_size内存
        ret = av_grow_packet(pkt, read_size);
        if (ret < 0)
            break;

        ret = avio_read(s, pkt->data + prev_size, read_size);//pkt->data的基础上，再读取read_size。
        if (ret != read_size) {//如果没有读到修正后的read_size，说明读有点问题？还是放弃读比较安全
            av_shrink_packet(pkt, prev_size + FFMAX(ret, 0));//调整pkt的大小
            break;
        }
        //能够安全读，可以尝试再读。
        size -= read_size;//
    } while (size > 0);//如果还没有读全
    if (size > 0)
        pkt->flags |= AV_PKT_FLAG_CORRUPT;//如果size不为零，说明因为种种原因说明读不全，标记包未读全。

    pkt->pos = orig_pos;//恢复原来的位置
    if (!pkt->size)//如果没有读到数据，要自己释放pkt
        av_packet_unref(pkt);
    return pkt->size > orig_size ? pkt->size - orig_size : ret;//减去原来的长度orig_size，就是读到的长度
}
//读size的长度，可能要重新分配内存
int av_get_packet(AVIOContext *s, AVPacket *pkt, int size)
{
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->pos  = avio_tell(s);

    return append_packet_chunked(s, pkt, size);//循环读，内存不够自动分配
}
//追加读size长度，自己保证pkt有这个长度
int av_append_packet(AVIOContext *s, AVPacket *pkt, int size)
{
    if (!pkt->size)//如果pkt长度为0，先分配size的长度
        return av_get_packet(s, pkt, size);
    return append_packet_chunked(s, pkt, size);//循环读，内存不够自动分配
}

int av_filename_number_test(const char *filename)
{
    char buf[1024];
    return filename &&
           (av_get_frame_filename(buf, sizeof(buf), filename, 1) >= 0);
}

static int set_codec_from_probe_data(AVFormatContext *s, AVStream *st,//通过文件扩展名，或者打开文件内容的头部信息来判断输入的类型得出类型，再对比是否是仅有的fmt_id_type几个文件类型之一
                                     AVProbeData *pd)
{
    static const struct {
        const char *name;
        enum AVCodecID id;
        enum AVMediaType type;
    } fmt_id_type[] = {//tiger 为什么是这些类型？ aac h264 h265已包括
        { "aac",       AV_CODEC_ID_AAC,        AVMEDIA_TYPE_AUDIO },
        { "ac3",       AV_CODEC_ID_AC3,        AVMEDIA_TYPE_AUDIO },
        { "aptx",      AV_CODEC_ID_APTX,       AVMEDIA_TYPE_AUDIO },
        { "dts",       AV_CODEC_ID_DTS,        AVMEDIA_TYPE_AUDIO },
        { "dvbsub",    AV_CODEC_ID_DVB_SUBTITLE,AVMEDIA_TYPE_SUBTITLE },
        { "dvbtxt",    AV_CODEC_ID_DVB_TELETEXT,AVMEDIA_TYPE_SUBTITLE },
        { "eac3",      AV_CODEC_ID_EAC3,       AVMEDIA_TYPE_AUDIO },
        { "h264",      AV_CODEC_ID_H264,       AVMEDIA_TYPE_VIDEO },
        { "hevc",      AV_CODEC_ID_HEVC,       AVMEDIA_TYPE_VIDEO },
        { "loas",      AV_CODEC_ID_AAC_LATM,   AVMEDIA_TYPE_AUDIO },
        { "m4v",       AV_CODEC_ID_MPEG4,      AVMEDIA_TYPE_VIDEO },
        { "mjpeg_2000",AV_CODEC_ID_JPEG2000,   AVMEDIA_TYPE_VIDEO },
        { "mp3",       AV_CODEC_ID_MP3,        AVMEDIA_TYPE_AUDIO },
        { "mpegvideo", AV_CODEC_ID_MPEG2VIDEO, AVMEDIA_TYPE_VIDEO },
        { "truehd",    AV_CODEC_ID_TRUEHD,     AVMEDIA_TYPE_AUDIO },
        { 0 }
    };
    int score;
    const AVInputFormat *fmt = av_probe_input_format3(pd, 1, &score);//通过文件扩展名，或者打开文件内容的头部信息来判断输入的类型

    if (fmt) {
        int i;
        av_log(s, AV_LOG_DEBUG,
               "Probe with size=%d, packets=%d detected %s with score=%d\n",
               pd->buf_size, MAX_PROBE_PACKETS - st->probe_packets,
               fmt->name, score);
        for (i = 0; fmt_id_type[i].name; i++) {//fmt_id_type 是仅有的几个编码格式 ，在这几个里面逐一对比
            if (!strcmp(fmt->name, fmt_id_type[i].name)) {//如果名称相同
                if (fmt_id_type[i].type != AVMEDIA_TYPE_AUDIO &&//不是音频必须有采样率
                    st->codecpar->sample_rate)
                    continue;
                if (st->request_probe > score &&//有些解码会设置st->request_probe，如mpegvideo  
                    st->codecpar->codec_id != fmt_id_type[i].id)//名称相同但codec_id 也不同
                    continue;
                st->codecpar->codec_id   = fmt_id_type[i].id;
                st->codecpar->codec_type = fmt_id_type[i].type;
                st->internal->need_context_update = 1;//重要
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
                st->codec->codec_type = st->codecpar->codec_type;//只是为了避免告警而已
                st->codec->codec_id   = st->codecpar->codec_id;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
                return score;
            }
        }
    }
    return 0;
}

/************************************************************/
/* input media file */

int av_demuxer_open(AVFormatContext *ic) {//调用iformat->read_header)，来初始化容器AVFormatContext
    int err;
    //01.首先判断是否在名单上
    if (ic->format_whitelist && av_match_list(ic->iformat->name, ic->format_whitelist, ',') <= 0) {
        av_log(ic, AV_LOG_ERROR, "Format not on whitelist \'%s\'\n", ic->format_whitelist);
        return AVERROR(EINVAL);
    }
    //02.容器格式的read_header函数
    if (ic->iformat->read_header) {//看注释Read the format header and initialize the AVFormatContext structure
        err = ic->iformat->read_header(ic);//ff_pcm_alaw_demuxer中 .read_header    = pcm_read_header或ff_aac_demuxer .read_probe   = adts_aac_probe
        if (err < 0)
            return err;
    }

    if (ic->pb && !ic->internal->data_offset)
        ic->internal->data_offset = avio_tell(ic->pb);//及时告知当前位置，便于调试

    return 0;
}

/* Open input file and probe the format if necessary. */
static int init_input(AVFormatContext *s, const char *filename,//TIGER init_input 如果没有容器iformat,启用av_probe_input_format2函数探测，再调用io_open;或者直接使用av_probe_input_buffer2
                      AVDictionary **options)
{
    int ret;
    AVProbeData pd = { filename, NULL, 0 };//
    int score = AVPROBE_SCORE_RETRY;//默认值是25
    //01.如果有自定意义的IO
    if (s->pb) {
        s->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (!s->iformat)//如果没有容器的格式，探测buffer
            return av_probe_input_buffer2(s->pb, &s->iformat, filename,//直接用读文件头部或者文件名后缀来判断。
                                         s, 0, s->format_probesize);
        else if (s->iformat->flags & AVFMT_NOFILE)
            av_log(s, AV_LOG_WARNING, "Custom AVIOContext makes no sense and "
                                      "will be ignored with AVFMT_NOFILE format.\n");
        return 0;
    }
    //02.如果有容器格式且没有文件，直接返回。或者没有容器格式，使用probe处输入格式 //举例：s->iformat=ff_pcm_alaw_demuxer
    if ((s->iformat && s->iformat->flags & AVFMT_NOFILE) ||//如果是没有文件类型，直接返回25
        (!s->iformat && (s->iformat = av_probe_input_format2(&pd, 0, &score))))//或者没有容器格式，探测格式，分值大于25，则成功赋值，并返回分值退出。
        return score;
    //03. 调用io_open  
    if ((ret = s->io_open(s, &s->pb, filename, AVIO_FLAG_READ | s->avio_flags, options)) < 0)//io_open ==io_open_default
        return ret; //打开文件失败
    //04.如果已经有容器格式
    if (s->iformat)
        return 0;
    return av_probe_input_buffer2(s->pb, &s->iformat, filename,//05.探测容器格式
                                 s, 0, s->format_probesize);
}
//单向列表而已
int ff_packet_list_put(AVPacketList **packet_buffer,
                       AVPacketList **plast_pktl,
                       AVPacket      *pkt, int flags)//放入单向列表，如果元素可引用，采用引用的方式
{
    AVPacketList *pktl = av_mallocz(sizeof(AVPacketList));
    int ret;

    if (!pktl)
        return AVERROR(ENOMEM);

    if (flags & FF_PACKETLIST_FLAG_REF_PACKET) {
        if ((ret = av_packet_ref(&pktl->pkt, pkt)) < 0) {
            av_free(pktl);
            return ret;
        }
    } else {
        // TODO: Adapt callers in this file so the line below can use
        //       av_packet_move_ref() to effectively move the reference
        //       to the list.
        pktl->pkt = *pkt;
    }

    if (*packet_buffer)
        (*plast_pktl)->next = pktl;
    else
        *packet_buffer = pktl;

    /* Add the packet in the buffered packet list. */
    *plast_pktl = pktl;
    return 0;
}

int avformat_queue_attached_pictures(AVFormatContext *s)
{
    int i, ret;
    for (i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC &&
            s->streams[i]->discard < AVDISCARD_ALL) {
            if (s->streams[i]->attached_pic.size <= 0) {
                av_log(s, AV_LOG_WARNING,
                    "Attached picture on stream %d has invalid size, "
                    "ignoring\n", i);
                continue;
            }

            ret = ff_packet_list_put(&s->internal->raw_packet_buffer,
                                     &s->internal->raw_packet_buffer_end,
                                     &s->streams[i]->attached_pic,
                                     FF_PACKETLIST_FLAG_REF_PACKET);
            if (ret < 0)
                return ret;
        }
    return 0;
}

static int update_stream_avctx(AVFormatContext *s)//tiger AVCodecParameters复制到st->codec和st->internal->avctx AVFormatContext的AVStream里面 AVCodecParameters的信息复制到的st->codec和st->internal->avctx 这两个AVCodecContext
{
    int i, ret;
    for (i = 0; i < s->nb_streams; i++) {//逐一处理
        AVStream *st = s->streams[i];
        //01.是否已经设置
        if (!st->internal->need_context_update)
            continue;
        //02. 如果parser已经打开，先关闭
        /* close parser, because it depends on the codec */
        if (st->parser && st->internal->avctx->codec_id != st->codecpar->codec_id) {
            av_parser_close(st->parser);
            st->parser = NULL;
        }
        //03.AVCodecParameters复制到st->internal->avctx，以便find_stream_info里面使用
        /* update internal codec context, for the parser */
        ret = avcodec_parameters_to_context(st->internal->avctx, st->codecpar);
        if (ret < 0)
            return ret;
        //04.AVCodecParameters复制到st->codec
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        /* update deprecated public codec context */
        ret = avcodec_parameters_to_context(st->codec, st->codecpar);
        if (ret < 0)
            return ret;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
        //05.标识已经设置
        st->internal->need_context_update = 0;
    }
    return 0;
}

//如果没有AVFormatContext内存，先分配内存，如果没有s->iformat,就用av_probe_input_format2，通过文件后缀名或者文件内容头部的格式信息来判断。
int avformat_open_input(AVFormatContext **ps, const char *filename,//TIGER avformat_open_input 最重要的函数init_input ,其他一些函数处理metadata，mp3的一些信息
                        ff_const59 AVInputFormat *fmt, AVDictionary **options)
{
    AVFormatContext *s = *ps;
    int i, ret = 0;
    AVDictionary *tmp = NULL;
    ID3v2ExtraMeta *id3v2_extra_meta = NULL;
    //01.没有就自动创建一个解析上下文
    if (!s && !(s = avformat_alloc_context()))//这里埋了一个功能avformat_get_context_defaults-->io_open_default
        return AVERROR(ENOMEM);//后续的init_input：io_open_default--> ffio_open_whitelist -->(ffurl_open_whitelist + ffio_fdopen)
    if (!s->av_class) {
        av_log(NULL, AV_LOG_ERROR, "Input context has not been properly allocated by avformat_alloc_context() and is not NULL either\n");
        return AVERROR(EINVAL);
    }
    if (fmt)
        s->iformat = fmt;//02. 如果已经有输入的AVInputFormat 直接使用，没有的话，后面的函数会用init_input-->av_probe_input_format2函数探测，再调用io_open
    //03. 可选项等处理
    if (options)
        av_dict_copy(&tmp, *options, 0);//复制可选项

    if (s->pb) // must be before any goto fail
        s->flags |= AVFMT_FLAG_CUSTOM_IO;//是否使用自定义IO

    if ((ret = av_opt_set_dict(s, &tmp)) < 0)//设置可选项
        goto fail;

    if (!(s->url = av_strdup(filename ? filename : ""))) {//复制文件名
        ret = AVERROR(ENOMEM);
        goto fail;
    }

#if FF_API_FORMAT_FILENAME
FF_DISABLE_DEPRECATION_WARNINGS
    av_strlcpy(s->filename, filename ? filename : "", sizeof(s->filename));
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    if ((ret = init_input(s, filename, &tmp)) < 0)//04.若有s->iformat 则直接io_open打开文件；如果没有s->iformat用 av_probe_input_format2函数探测，再调用io_open;或者直接使用av_probe_input_buffer2
        goto fail;
    s->probe_score = ret;
    //05.处理白名单，黑名单等
    if (!s->protocol_whitelist && s->pb && s->pb->protocol_whitelist) {
        s->protocol_whitelist = av_strdup(s->pb->protocol_whitelist);
        if (!s->protocol_whitelist) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (!s->protocol_blacklist && s->pb && s->pb->protocol_blacklist) {
        s->protocol_blacklist = av_strdup(s->pb->protocol_blacklist);
        if (!s->protocol_blacklist) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (s->format_whitelist && av_match_list(s->iformat->name, s->format_whitelist, ',') <= 0) {
        av_log(s, AV_LOG_ERROR, "Format not on whitelist \'%s\'\n", s->format_whitelist);
        ret = AVERROR(EINVAL);
        goto fail;
    }
    //06. 某些时候可以设置，然后会跳过开始的n个字节
    avio_skip(s->pb, s->skip_initial_bytes);//tiger program 可以参考
    //07. 特例：文件名还带数字的
    /* Check filename in case an image number is expected. */
    if (s->iformat->flags & AVFMT_NEEDNUMBER) {
        if (!av_filename_number_test(filename)) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }
    //08.需要特别设置
    s->duration = s->start_time = AV_NOPTS_VALUE;
    //09. 复制iformat的私有结构，比如pcm需要声道数，采样率，在哪里保存呢？就放在priv_class
    /* Allocate private data. */
    if (s->iformat->priv_data_size > 0) {//ff_pcm_alaw_demuxer: priv_data_size = sizeof(PCMAudioDemuxerContext) PCMAudioDemuxerContext:声道数，采样率
        if (!(s->priv_data = av_mallocz(s->iformat->priv_data_size))) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        if (s->iformat->priv_class) {//ff_pcm_alaw_demuxer: .priv_class     = &name_ ## _demuxer_class,即alaw_demuxer_class
            *(const AVClass **) s->priv_data = s->iformat->priv_class; //ff_pcm_alaw_demuxer: AVClass name_ ## _demuxer_class = {.class_name = #name_ " demuxer",.item_name  = av_default_item_name,.option     = pcm_options,.version    = LIBAVUTIL_VERSION_INT,}
            av_opt_set_defaults(s->priv_data);//设置缺省值，因为都是AVClass结构，只要调用类的option进行相关操作即可
            if ((ret = av_opt_set_dict(s->priv_data, &tmp)) < 0)//设置用户输入的可选项。这里主要比可选项的名称来操作
                goto fail;
        }
    }
    //10.
    /* e.g. AVFMT_NOFILE formats will not have a AVIOContext */
    if (s->pb)
        ff_id3v2_read_dict(s->pb, &s->internal->id3v2_meta, ID3v2_DEFAULT_MAGIC, &id3v2_extra_meta);

    //11.读文件头
    if (!(s->flags&AVFMT_FLAG_PRIV_OPT) && s->iformat->read_header)//iformat:ff_pcm_alaw_demuxer:  read_header:pcm_read_header
        if ((ret = s->iformat->read_header(s)) < 0)//TIGER SDP 调用sdp_read_header-->ff_sdp_parse-->payload_type-->sdp_parse_line-->ff_parse_fmtp-->parse_fmtp-->parse_fmtp_config
            goto fail; 
    //12.
    if (!s->metadata) {
        s->metadata = s->internal->id3v2_meta;
        s->internal->id3v2_meta = NULL;
    } else if (s->internal->id3v2_meta) {
        int level = AV_LOG_WARNING;
        if (s->error_recognition & AV_EF_COMPLIANT)
            level = AV_LOG_ERROR;
        av_log(s, level, "Discarding ID3 tags because more suitable tags were found.\n");
        av_dict_free(&s->internal->id3v2_meta);
        if (s->error_recognition & AV_EF_EXPLODE)
            return AVERROR_INVALIDDATA;
    }
    //13. 读取mp3信息
    if (id3v2_extra_meta) {
        if (!strcmp(s->iformat->name, "mp3") || !strcmp(s->iformat->name, "aac") ||
            !strcmp(s->iformat->name, "tta") || !strcmp(s->iformat->name, "wav")) {//只有这些才有chapter等信息
            if ((ret = ff_id3v2_parse_apic(s, &id3v2_extra_meta)) < 0)
                goto fail;
            if ((ret = ff_id3v2_parse_chapters(s, &id3v2_extra_meta)) < 0)
                goto fail;
            if ((ret = ff_id3v2_parse_priv(s, &id3v2_extra_meta)) < 0)
                goto fail;
        } else
            av_log(s, AV_LOG_DEBUG, "demuxer does not support additional id3 data, skipping\n");
    }
    ff_id3v2_free_extra_meta(&id3v2_extra_meta);
    //14.
    if ((ret = avformat_queue_attached_pictures(s)) < 0)//TIGER 是mp3中的专辑图片吗？
        goto fail;

    if (!(s->flags&AVFMT_FLAG_PRIV_OPT) && s->pb && !s->internal->data_offset)
        s->internal->data_offset = avio_tell(s->pb);
    //15.不理解：
    s->internal->raw_packet_buffer_remaining_size = RAW_PACKET_BUFFER_SIZE;
    //16.更新context 
    update_stream_avctx(s);//用avcodec_parameters_to_context更新: AVCodecParameters复制到st->codec和st->internal->avctx
    //17.更新orig_codec_id  ==> 对比find_stream_info
    for (i = 0; i < s->nb_streams; i++)
        s->streams[i]->internal->orig_codec_id = s->streams[i]->codecpar->codec_id;//更新orig_codec_id
    //18.更新选项，如果原先有，则先释放
    if (options) {
        av_dict_free(options);
        *options = tmp;
    }
    *ps = s;
    return 0;

fail:
    ff_id3v2_free_extra_meta(&id3v2_extra_meta);
    av_dict_free(&tmp);
    if (s->pb && !(s->flags & AVFMT_FLAG_CUSTOM_IO))
        avio_closep(&s->pb);
    avformat_free_context(s);
    *ps = NULL;
    return ret;
}

/*******************************************************/

static void force_codec_ids(AVFormatContext *s, AVStream *st)
{
    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (s->video_codec_id)
            st->codecpar->codec_id = s->video_codec_id;
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (s->audio_codec_id)
            st->codecpar->codec_id = s->audio_codec_id;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (s->subtitle_codec_id)
            st->codecpar->codec_id = s->subtitle_codec_id;
        break;
    case AVMEDIA_TYPE_DATA:
        if (s->data_codec_id)
            st->codecpar->codec_id = s->data_codec_id;
        break;
    }
}

static int probe_codec(AVFormatContext *s, AVStream *st, const AVPacket *pkt)//TIGER 看是否应得到codec，是否设置probe为结束即request_probe =-1
{
    if (st->request_probe>0) {//probe中
        AVProbeData *pd = &st->probe_data;
        int end;
        av_log(s, AV_LOG_DEBUG, "probing stream %d pp:%d\n", st->index, st->probe_packets);
        --st->probe_packets;//probe 包数限制

        if (pkt) {//01.没有pkt，分配内存
            uint8_t *new_buf = av_realloc(pd->buf, pd->buf_size+pkt->size+AVPROBE_PADDING_SIZE);
            if (!new_buf) {
                av_log(s, AV_LOG_WARNING,
                       "Failed to reallocate probe buffer for stream %d\n",
                       st->index);
                goto no_packet;
            }
            pd->buf = new_buf;
            memcpy(pd->buf + pd->buf_size, pkt->data, pkt->size);
            pd->buf_size += pkt->size;
            memset(pd->buf + pd->buf_size, 0, AVPROBE_PADDING_SIZE);
        } else {
no_packet:
            st->probe_packets = 0;
            if (!pd->buf_size) {
                av_log(s, AV_LOG_WARNING,
                       "nothing to probe for stream %d\n", st->index);
            }
        }
        //02.如果内部的数据已经不足以分析了，或者probe_packets次数已经到限制了，end设置为1
        end=    s->internal->raw_packet_buffer_remaining_size <= 0
                || st->probe_packets<= 0;
        //03.
        if (end || av_log2(pd->buf_size) != av_log2(pd->buf_size - pkt->size)) {//为什么是av_log2?
            int score = set_codec_from_probe_data(s, st, pd);
            if (    (st->codecpar->codec_id != AV_CODEC_ID_NONE && score > AVPROBE_SCORE_STREAM_RETRY)
                || end) {
                pd->buf_size = 0;
                av_freep(&pd->buf);//释放probe buffer
                st->request_probe = -1;//设置为负数 //tiger import 这里是唯一设置request_probe为-1的地方 //TIGER TODO:查询仅wavdec.c,mpeg.c和mpegts.c
                if (st->codecpar->codec_id != AV_CODEC_ID_NONE) {
                    av_log(s, AV_LOG_DEBUG, "probed stream %d\n", st->index);//如果找到了，提示
                } else
                    av_log(s, AV_LOG_WARNING, "probed stream %d failed\n", st->index);
            }
            force_codec_ids(s, st);//强行设置
        }
    }
    return 0;
}

static int update_wrap_reference(AVFormatContext *s, AVStream *st, int stream_index, AVPacket *pkt)
{
    int64_t ref = pkt->dts;
    int i, pts_wrap_behavior;
    int64_t pts_wrap_reference;
    AVProgram *first_program;

    if (ref == AV_NOPTS_VALUE)
        ref = pkt->pts;
    if (st->pts_wrap_reference != AV_NOPTS_VALUE || st->pts_wrap_bits >= 63 || ref == AV_NOPTS_VALUE || !s->correct_ts_overflow)
        return 0;
    ref &= (1LL << st->pts_wrap_bits)-1;

    // reference time stamp should be 60 s before first time stamp
    pts_wrap_reference = ref - av_rescale(60, st->time_base.den, st->time_base.num);
    // if first time stamp is not more than 1/8 and 60s before the wrap point, subtract rather than add wrap offset
    pts_wrap_behavior = (ref < (1LL << st->pts_wrap_bits) - (1LL << st->pts_wrap_bits-3)) ||
        (ref < (1LL << st->pts_wrap_bits) - av_rescale(60, st->time_base.den, st->time_base.num)) ?
        AV_PTS_WRAP_ADD_OFFSET : AV_PTS_WRAP_SUB_OFFSET;

    first_program = av_find_program_from_stream(s, NULL, stream_index);

    if (!first_program) {
        int default_stream_index = av_find_default_stream_index(s);
        if (s->streams[default_stream_index]->pts_wrap_reference == AV_NOPTS_VALUE) {
            for (i = 0; i < s->nb_streams; i++) {
                if (av_find_program_from_stream(s, NULL, i))
                    continue;
                s->streams[i]->pts_wrap_reference = pts_wrap_reference;
                s->streams[i]->pts_wrap_behavior = pts_wrap_behavior;
            }
        }
        else {
            st->pts_wrap_reference = s->streams[default_stream_index]->pts_wrap_reference;
            st->pts_wrap_behavior = s->streams[default_stream_index]->pts_wrap_behavior;
        }
    }
    else {
        AVProgram *program = first_program;
        while (program) {
            if (program->pts_wrap_reference != AV_NOPTS_VALUE) {
                pts_wrap_reference = program->pts_wrap_reference;
                pts_wrap_behavior = program->pts_wrap_behavior;
                break;
            }
            program = av_find_program_from_stream(s, program, stream_index);
        }

        // update every program with differing pts_wrap_reference
        program = first_program;
        while (program) {
            if (program->pts_wrap_reference != pts_wrap_reference) {
                for (i = 0; i<program->nb_stream_indexes; i++) {
                    s->streams[program->stream_index[i]]->pts_wrap_reference = pts_wrap_reference;
                    s->streams[program->stream_index[i]]->pts_wrap_behavior = pts_wrap_behavior;
                }

                program->pts_wrap_reference = pts_wrap_reference;
                program->pts_wrap_behavior = pts_wrap_behavior;
            }
            program = av_find_program_from_stream(s, program, stream_index);
        }
    }
    return 1;
}
//av_read_frame-->read_frame_internal -->ff_read_packet -->ff_rtsp_fetch_packet -->ff_rtp_parse_packet -->rtp_parse_one_packet -->rtp_parse_packet_internal-->h264_handle_packet
int ff_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret, i, err;
    AVStream *st;

    for (;;) {//这个循环包含了内部队列插入数据的功能，probe_codec的功能，所以看起来相对代码多一些。
        AVPacketList *pktl = s->internal->raw_packet_buffer;//如果内部队列有数据
        //01. 如果是启动find_stream_info probe
        if (pktl) {
            *pkt = pktl->pkt;//第一个
            st   = s->streams[pkt->stream_index];
            if (s->internal->raw_packet_buffer_remaining_size <= 0)//如果没有数据，
                if ((err = probe_codec(s, st, NULL)) < 0)//是否已经找到codec
                    return err;
            if (st->request_probe <= 0) {
                s->internal->raw_packet_buffer                 = pktl->next;
                s->internal->raw_packet_buffer_remaining_size += pkt->size;
                av_free(pktl);
                return 0;
            }
        }
        //02.
        pkt->data = NULL;
        pkt->size = 0;
        av_init_packet(pkt);//举例rtp有点特别：av_read_frame-->read_frame_internal -->ff_read_packet -->ff_rtsp_fetch_packet -->ff_rtp_parse_packet -->rtp_parse_one_packet -->rtp_parse_packet_internal-->h264_handle_packet
        ret = s->iformat->read_packet(s, pkt);//TIGER flv_read_packet
        if (ret < 0) {//如果有异常
            /* Some demuxers return FFERROR_REDO when they consume
               data and discard it (ignored streams, junk, extradata).
               We must re-call the demuxer to get the real packet. */
            if (ret == FFERROR_REDO)//redo没有用到
                continue;
            if (!pktl || ret == AVERROR(EAGAIN))//获取不到
                return ret;
            for (i = 0; i < s->nb_streams; i++) {
                st = s->streams[i];
                if (st->probe_packets || st->request_probe > 0)
                    if ((err = probe_codec(s, st, NULL)) < 0)//最后出错时，尝试一下probe_codec
                        return err;
                av_assert0(st->request_probe <= 0);
            }
            continue;
        }
        //03.增加引用次数
        err = av_packet_make_refcounted(pkt);
        if (err < 0)
            return err;
        //04.校验
        if ((s->flags & AVFMT_FLAG_DISCARD_CORRUPT) &&
            (pkt->flags & AV_PKT_FLAG_CORRUPT)) {
            av_log(s, AV_LOG_WARNING,
                   "Dropped corrupted packet (stream = %d)\n",
                   pkt->stream_index);
            av_packet_unref(pkt);
            continue;
        }

        if (pkt->stream_index >= (unsigned)s->nb_streams) {
            av_log(s, AV_LOG_ERROR, "Invalid stream index %d\n", pkt->stream_index);
            continue;
        }
        
        st = s->streams[pkt->stream_index];
        //05. dts，pts
        if (update_wrap_reference(s, st, pkt->stream_index, pkt) && st->pts_wrap_behavior == AV_PTS_WRAP_SUB_OFFSET) {
            // correct first time stamps to negative values
            if (!is_relative(st->first_dts))
                st->first_dts = wrap_timestamp(st, st->first_dts);
            if (!is_relative(st->start_time))
                st->start_time = wrap_timestamp(st, st->start_time);
            if (!is_relative(st->cur_dts))
                st->cur_dts = wrap_timestamp(st, st->cur_dts);
        }

        pkt->dts = wrap_timestamp(st, pkt->dts);
        pkt->pts = wrap_timestamp(st, pkt->pts);
        //06.强行用AVFormatContext的id ==>
        force_codec_ids(s, st);

        /* TODO: audio: time filter; video: frame reordering (pts != dts) */
        if (s->use_wallclock_as_timestamps)
            pkt->dts = pkt->pts = av_rescale_q(av_gettime(), AV_TIME_BASE_Q, st->time_base);
        //07.如果pkt1为空，或者不是probe状态，直接返回
        if (!pktl && st->request_probe <= 0)//probe_codec 会设置request_probe 为-1
            return ret;
        //08.如果是probe即request_probe> 0，放入内部列表
        err = ff_packet_list_put(&s->internal->raw_packet_buffer,
                                 &s->internal->raw_packet_buffer_end,
                                 pkt, 0);
        if (err)
            return err;
        s->internal->raw_packet_buffer_remaining_size -= pkt->size;//空间又变小了
        //09.如果结束probe，检查codec，设置request_probe 为-1
        if ((err = probe_codec(s, st, pkt)) < 0)
            return err;
    }
}


/**********************************************************/

static int determinable_frame_size(AVCodecContext *avctx)
{
    switch(avctx->codec_id) {
    case AV_CODEC_ID_MP1:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_CODEC2:
        return 1;
    }

    return 0;
}

/**
 * Return the frame duration in seconds. Return 0 if not available.
 */
void ff_compute_frame_duration(AVFormatContext *s, int *pnum, int *pden, AVStream *st,//TIGER ALAW: pnum=320, pden=8000, 320/8000相当于40ms
                               AVCodecParserContext *pc, AVPacket *pkt)
{//01. alaw codec_framerate={num = 0, den = 1}
    AVRational codec_framerate = s->iformat ? st->internal->avctx->framerate :
                                              av_mul_q(av_inv_q(st->internal->avctx->time_base), (AVRational){1, st->internal->avctx->ticks_per_frame});
    int frame_size, sample_rate;
    //02.
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    if ((!codec_framerate.den || !codec_framerate.num) && st->codec->time_base.den && st->codec->time_base.num)
        codec_framerate = av_mul_q(av_inv_q(st->codec->time_base), (AVRational){1, st->codec->ticks_per_frame});
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    //03.
    *pnum = 0;
    *pden = 0;
    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO://03.01
        if (st->r_frame_rate.num && !pc && s->iformat) {
            *pnum = st->r_frame_rate.den;
            *pden = st->r_frame_rate.num;
        } else if (st->time_base.num * 1000LL > st->time_base.den) {
            *pnum = st->time_base.num;
            *pden = st->time_base.den;
        } else if (codec_framerate.den * 1000LL > codec_framerate.num) {
            av_assert0(st->internal->avctx->ticks_per_frame);
            av_reduce(pnum, pden,
                      codec_framerate.den,
                      codec_framerate.num * (int64_t)st->internal->avctx->ticks_per_frame,
                      INT_MAX);

            if (pc && pc->repeat_pict) {
                av_assert0(s->iformat); // this may be wrong for interlaced encoding but its not used for that case
                av_reduce(pnum, pden,
                          (*pnum) * (1LL + pc->repeat_pict),
                          (*pden),
                          INT_MAX);
            }
            /* If this codec can be interlaced or progressive then we need
             * a parser to compute duration of a packet. Thus if we have
             * no parser in such case leave duration undefined. */
            if (st->internal->avctx->ticks_per_frame > 1 && !pc)
                *pnum = *pden = 0;
        }
        break;
    case AVMEDIA_TYPE_AUDIO://03.02 alaw: 320/8000= 40ms
        if (st->internal->avctx_inited) {
            frame_size = av_get_audio_frame_duration(st->internal->avctx, pkt->size);//alaw:  frame_size =320 , pkt->size = 320
            sample_rate = st->internal->avctx->sample_rate;//alaw:  sample_rate=8000, 
        } else {
            frame_size = av_get_audio_frame_duration2(st->codecpar, pkt->size);
            sample_rate = st->codecpar->sample_rate;
        }
        if (frame_size <= 0 || sample_rate <= 0)
            break;
        *pnum = frame_size;
        *pden = sample_rate;
        break;
    default:
        break;
    }
}

static int is_intra_only(enum AVCodecID id)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(id);
    if (!d)
        return 0;
    if (d->type == AVMEDIA_TYPE_VIDEO && !(d->props & AV_CODEC_PROP_INTRA_ONLY))
        return 0;
    return 1;
}

static int has_decode_delay_been_guessed(AVStream *st)
{
    if (st->codecpar->codec_id != AV_CODEC_ID_H264) return 1;
    if (!st->info) // if we have left find_stream_info then nb_decoded_frames won't increase anymore for stream copy
        return 1;
#if CONFIG_H264_DECODER//编译中的ffbuid的config.make里：CONFIG_H264_DECODER=yes，config.h #define CONFIG_H264_DECODER 1, config.asm %define CONFIG_H264_DECODER 1
    if (st->internal->avctx->has_b_frames &&
       avpriv_h264_has_num_reorder_frames(st->internal->avctx) == st->internal->avctx->has_b_frames)
        return 1;
#endif
    if (st->internal->avctx->has_b_frames<3)
        return st->nb_decoded_frames >= 7;
    else if (st->internal->avctx->has_b_frames<4)
        return st->nb_decoded_frames >= 18;
    else
        return st->nb_decoded_frames >= 20;
}

static AVPacketList *get_next_pkt(AVFormatContext *s, AVStream *st, AVPacketList *pktl)
{
    if (pktl->next)
        return pktl->next;
    if (pktl == s->internal->packet_buffer_end)
        return s->internal->parse_queue;
    return NULL;
}

static int64_t select_from_pts_buffer(AVStream *st, int64_t *pts_buffer, int64_t dts) {
    int onein_oneout = st->codecpar->codec_id != AV_CODEC_ID_H264 &&
                       st->codecpar->codec_id != AV_CODEC_ID_HEVC;

    if(!onein_oneout) {
        int delay = st->internal->avctx->has_b_frames;
        int i;

        if (dts == AV_NOPTS_VALUE) {
            int64_t best_score = INT64_MAX;
            for (i = 0; i<delay; i++) {
                if (st->pts_reorder_error_count[i]) {
                    int64_t score = st->pts_reorder_error[i] / st->pts_reorder_error_count[i];
                    if (score < best_score) {
                        best_score = score;
                        dts = pts_buffer[i];
                    }
                }
            }
        } else {
            for (i = 0; i<delay; i++) {
                if (pts_buffer[i] != AV_NOPTS_VALUE) {
                    int64_t diff =  FFABS(pts_buffer[i] - dts)
                                    + (uint64_t)st->pts_reorder_error[i];
                    diff = FFMAX(diff, st->pts_reorder_error[i]);
                    st->pts_reorder_error[i] = diff;
                    st->pts_reorder_error_count[i]++;
                    if (st->pts_reorder_error_count[i] > 250) {
                        st->pts_reorder_error[i] >>= 1;
                        st->pts_reorder_error_count[i] >>= 1;
                    }
                }
            }
        }
    }

    if (dts == AV_NOPTS_VALUE)
        dts = pts_buffer[0];

    return dts;
}

/**
 * Updates the dts of packets of a stream in pkt_buffer, by re-ordering the pts
 * of the packets in a window.
 */
static void update_dts_from_pts(AVFormatContext *s, int stream_index,
                                AVPacketList *pkt_buffer)
{
    AVStream *st       = s->streams[stream_index];
    int delay          = st->internal->avctx->has_b_frames;
    int i;

    int64_t pts_buffer[MAX_REORDER_DELAY+1];

    for (i = 0; i<MAX_REORDER_DELAY+1; i++)
        pts_buffer[i] = AV_NOPTS_VALUE;

    for (; pkt_buffer; pkt_buffer = get_next_pkt(s, st, pkt_buffer)) {
        if (pkt_buffer->pkt.stream_index != stream_index)
            continue;

        if (pkt_buffer->pkt.pts != AV_NOPTS_VALUE && delay <= MAX_REORDER_DELAY) {
            pts_buffer[0] = pkt_buffer->pkt.pts;
            for (i = 0; i<delay && pts_buffer[i] > pts_buffer[i + 1]; i++)
                FFSWAP(int64_t, pts_buffer[i], pts_buffer[i + 1]);

            pkt_buffer->pkt.dts = select_from_pts_buffer(st, pts_buffer, pkt_buffer->pkt.dts);
        }
    }
}

static void update_initial_timestamps(AVFormatContext *s, int stream_index,
                                      int64_t dts, int64_t pts, AVPacket *pkt)
{
    AVStream *st       = s->streams[stream_index];
    AVPacketList *pktl = s->internal->packet_buffer ? s->internal->packet_buffer : s->internal->parse_queue;
    AVPacketList *pktl_it;

    uint64_t shift;

    if (st->first_dts != AV_NOPTS_VALUE ||
        dts           == AV_NOPTS_VALUE ||
        st->cur_dts   == AV_NOPTS_VALUE ||
        st->cur_dts < INT_MIN + RELATIVE_TS_BASE ||
        is_relative(dts))
        return;

    st->first_dts = dts - (st->cur_dts - RELATIVE_TS_BASE);
    st->cur_dts   = dts;
    shift         = (uint64_t)st->first_dts - RELATIVE_TS_BASE;

    if (is_relative(pts))
        pts += shift;

    for (pktl_it = pktl; pktl_it; pktl_it = get_next_pkt(s, st, pktl_it)) {
        if (pktl_it->pkt.stream_index != stream_index)
            continue;
        if (is_relative(pktl_it->pkt.pts))
            pktl_it->pkt.pts += shift;

        if (is_relative(pktl_it->pkt.dts))
            pktl_it->pkt.dts += shift;

        if (st->start_time == AV_NOPTS_VALUE && pktl_it->pkt.pts != AV_NOPTS_VALUE) {
            st->start_time = pktl_it->pkt.pts;
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && st->codecpar->sample_rate)
                st->start_time += av_rescale_q(st->skip_samples, (AVRational){1, st->codecpar->sample_rate}, st->time_base);
        }
    }

    if (has_decode_delay_been_guessed(st)) {
        update_dts_from_pts(s, stream_index, pktl);
    }

    if (st->start_time == AV_NOPTS_VALUE) {
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || !(pkt->flags & AV_PKT_FLAG_DISCARD)) {
            st->start_time = pts;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && st->codecpar->sample_rate)
            st->start_time += av_rescale_q(st->skip_samples, (AVRational){1, st->codecpar->sample_rate}, st->time_base);
    }
}

static void update_initial_durations(AVFormatContext *s, AVStream *st,
                                     int stream_index, int duration)
{
    AVPacketList *pktl = s->internal->packet_buffer ? s->internal->packet_buffer : s->internal->parse_queue;
    int64_t cur_dts    = RELATIVE_TS_BASE;

    if (st->first_dts != AV_NOPTS_VALUE) {
        if (st->update_initial_durations_done)
            return;
        st->update_initial_durations_done = 1;
        cur_dts = st->first_dts;
        for (; pktl; pktl = get_next_pkt(s, st, pktl)) {
            if (pktl->pkt.stream_index == stream_index) {
                if (pktl->pkt.pts != pktl->pkt.dts  ||
                    pktl->pkt.dts != AV_NOPTS_VALUE ||
                    pktl->pkt.duration)
                    break;
                cur_dts -= duration;
            }
        }
        if (pktl && pktl->pkt.dts != st->first_dts) {
            av_log(s, AV_LOG_DEBUG, "first_dts %s not matching first dts %s (pts %s, duration %"PRId64") in the queue\n",
                   av_ts2str(st->first_dts), av_ts2str(pktl->pkt.dts), av_ts2str(pktl->pkt.pts), pktl->pkt.duration);
            return;
        }
        if (!pktl) {
            av_log(s, AV_LOG_DEBUG, "first_dts %s but no packet with dts in the queue\n", av_ts2str(st->first_dts));
            return;
        }
        pktl          = s->internal->packet_buffer ? s->internal->packet_buffer : s->internal->parse_queue;
        st->first_dts = cur_dts;
    } else if (st->cur_dts != RELATIVE_TS_BASE)
        return;

    for (; pktl; pktl = get_next_pkt(s, st, pktl)) {
        if (pktl->pkt.stream_index != stream_index)
            continue;
        if ((pktl->pkt.pts == pktl->pkt.dts ||
             pktl->pkt.pts == AV_NOPTS_VALUE) &&
            (pktl->pkt.dts == AV_NOPTS_VALUE ||
             pktl->pkt.dts == st->first_dts ||
             pktl->pkt.dts == RELATIVE_TS_BASE) &&
            !pktl->pkt.duration) {
            pktl->pkt.dts = cur_dts;
            if (!st->internal->avctx->has_b_frames)
                pktl->pkt.pts = cur_dts;
//            if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                pktl->pkt.duration = duration;
        } else
            break;
        cur_dts = pktl->pkt.dts + pktl->pkt.duration;
    }
    if (!pktl)
        st->cur_dts = cur_dts;
}

static void compute_pkt_fields(AVFormatContext *s, AVStream *st,
                               AVCodecParserContext *pc, AVPacket *pkt,
                               int64_t next_dts, int64_t next_pts)//tiger 把这么多特例放一起，真是晕
{
    int num, den, presentation_delayed, delay, i;
    int64_t offset;
    AVRational duration;
    int onein_oneout = st->codecpar->codec_id != AV_CODEC_ID_H264 &&//01.读取out.al 时onein_oneout =1 ，特例：H264 H265不是
                       st->codecpar->codec_id != AV_CODEC_ID_HEVC;
    //02.特例
    if (s->flags & AVFMT_FLAG_NOFILLIN)
        return;
    //03.如果视频且pkt含dts
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && pkt->dts != AV_NOPTS_VALUE) {
        if (pkt->dts == pkt->pts && st->last_dts_for_order_check != AV_NOPTS_VALUE) {
            if (st->last_dts_for_order_check <= pkt->dts) {//是否是单调增长
                st->dts_ordered++;
            } else {
                av_log(s, st->dts_misordered ? AV_LOG_DEBUG : AV_LOG_WARNING,//打印
                       "DTS %"PRIi64" < %"PRIi64" out of order\n",
                       pkt->dts,
                       st->last_dts_for_order_check);
                st->dts_misordered++;//失序
            }
            if (st->dts_ordered + st->dts_misordered > 250) {//大于250，减半，便于统计吗？
                st->dts_ordered    >>= 1;
                st->dts_misordered >>= 1;
            }
        }

        st->last_dts_for_order_check = pkt->dts;//保存看是否失序
        if (st->dts_ordered < 8*st->dts_misordered && pkt->dts == pkt->pts)
            pkt->dts = AV_NOPTS_VALUE;
    }
    //04.
    if ((s->flags & AVFMT_FLAG_IGNDTS) && pkt->pts != AV_NOPTS_VALUE)
        pkt->dts = AV_NOPTS_VALUE;
    //05.视频的b帧
    if (pc && pc->pict_type == AV_PICTURE_TYPE_B
        && !st->internal->avctx->has_b_frames)
        //FIXME Set low_delay = 0 when has_b_frames = 1
        st->internal->avctx->has_b_frames = 1;
    
    /* do we have a video B-frame ? */
    delay = st->internal->avctx->has_b_frames;
    presentation_delayed = 0;

    /* XXX: need has_b_frame, but cannot get it if the codec is
     *  not initialized */
    if (delay &&
        pc && pc->pict_type != AV_PICTURE_TYPE_B)
        presentation_delayed = 1;
    //06.不理解 //tiger todo
    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE &&
        st->pts_wrap_bits < 63 &&//举例 alaw ：st->pts_wrap_bits =64
        pkt->dts - (1LL << (st->pts_wrap_bits - 1)) > pkt->pts) {
        if (is_relative(st->cur_dts) || pkt->dts - (1LL<<(st->pts_wrap_bits - 1)) > st->cur_dts) {
            pkt->dts -= 1LL << st->pts_wrap_bits;
        } else
            pkt->pts += 1LL << st->pts_wrap_bits;
    }
    //08.注释
    /* Some MPEG-2 in MPEG-PS lack dts (issue #171 / input_file.mpg).
     * We take the conservative approach and discard both.
     * Note: If this is misbehaving for an H.264 file, then possibly
     * presentation_delayed is not set correctly. */
    if (delay == 1 && pkt->dts == pkt->pts &&
        pkt->dts != AV_NOPTS_VALUE && presentation_delayed) {
        av_log(s, AV_LOG_DEBUG, "invalid dts/pts combination %"PRIi64"\n", pkt->dts);
        if (    strcmp(s->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2")
             && strcmp(s->iformat->name, "flv")) // otherwise we discard correct timestamps for vc1-wmapro.ism
            pkt->dts = AV_NOPTS_VALUE;
    }
    //09.
    duration = av_mul_q((AVRational) {pkt->duration, 1}, st->time_base);//ALAW st->time_base:{num = 1, den = 8000}， 计算结果duration：{num = 0, den = 1}
    if (pkt->duration == 0) {//
        ff_compute_frame_duration(s, &num, &den, st, pc, pkt);//AWAW:num =320，den=8000
        if (den && num) {
            duration = (AVRational) {num, den};
            pkt->duration = av_rescale_rnd(1,
                                           num * (int64_t) st->time_base.den,
                                           den * (int64_t) st->time_base.num,
                                           AV_ROUND_DOWN);
        }
    }
    //10.
    if (pkt->duration != 0 && (s->internal->packet_buffer || s->internal->parse_queue))
        update_initial_durations(s, st, pkt->stream_index, pkt->duration);
    //11.
    /* Correct timestamps with byte offset if demuxers only have timestamps
     * on packet boundaries */
    if (pc && st->need_parsing == AVSTREAM_PARSE_TIMESTAMPS && pkt->size) {
        /* this will estimate bitrate based on this frame's duration and size */
        offset = av_rescale(pc->offset, pkt->duration, pkt->size);
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts += offset;
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts += offset;
    }
    //12.
    /* This may be redundant, but it should not hurt. */
    if (pkt->dts != AV_NOPTS_VALUE &&
        pkt->pts != AV_NOPTS_VALUE &&
        pkt->pts > pkt->dts)
        presentation_delayed = 1;

    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG,
            "IN delayed:%d pts:%s, dts:%s cur_dts:%s st:%d pc:%p duration:%"PRId64" delay:%d onein_oneout:%d\n",
            presentation_delayed, av_ts2str(pkt->pts), av_ts2str(pkt->dts), av_ts2str(st->cur_dts),
            pkt->stream_index, pc, pkt->duration, delay, onein_oneout);
    //13.
    /* Interpolate PTS and DTS if they are not present. We skip H264
     * currently because delay and has_b_frames are not reliably set. */
    if ((delay == 0 || (delay == 1 && pc)) &&
        onein_oneout) {
        if (presentation_delayed) {
            /* DTS = decompression timestamp */
            /* PTS = presentation timestamp */
            if (pkt->dts == AV_NOPTS_VALUE)
                pkt->dts = st->last_IP_pts;
            update_initial_timestamps(s, pkt->stream_index, pkt->dts, pkt->pts, pkt);
            if (pkt->dts == AV_NOPTS_VALUE)
                pkt->dts = st->cur_dts;

            /* This is tricky: the dts must be incremented by the duration
             * of the frame we are displaying, i.e. the last I- or P-frame. */
            if (st->last_IP_duration == 0 && (uint64_t)pkt->duration <= INT32_MAX)
                st->last_IP_duration = pkt->duration;
            if (pkt->dts != AV_NOPTS_VALUE)
                st->cur_dts = pkt->dts + st->last_IP_duration;
            if (pkt->dts != AV_NOPTS_VALUE &&
                pkt->pts == AV_NOPTS_VALUE &&
                st->last_IP_duration > 0 &&
                ((uint64_t)st->cur_dts - (uint64_t)next_dts + 1) <= 2 &&
                next_dts != next_pts &&
                next_pts != AV_NOPTS_VALUE)
                pkt->pts = next_dts;

            if ((uint64_t)pkt->duration <= INT32_MAX)
                st->last_IP_duration = pkt->duration;
            st->last_IP_pts      = pkt->pts;
            /* Cannot compute PTS if not present (we can compute it only
             * by knowing the future. */
        } else if (pkt->pts != AV_NOPTS_VALUE ||
                   pkt->dts != AV_NOPTS_VALUE ||
                   pkt->duration                ) {

            /* presentation is not delayed : PTS and DTS are the same */
            if (pkt->pts == AV_NOPTS_VALUE)
                pkt->pts = pkt->dts;
            update_initial_timestamps(s, pkt->stream_index, pkt->pts,
                                      pkt->pts, pkt);
            if (pkt->pts == AV_NOPTS_VALUE)
                pkt->pts = st->cur_dts;
            pkt->dts = pkt->pts;
            if (pkt->pts != AV_NOPTS_VALUE)
                st->cur_dts = av_add_stable(st->time_base, pkt->pts, duration, 1);
        }
    }
    //14.
    if (pkt->pts != AV_NOPTS_VALUE && delay <= MAX_REORDER_DELAY) {
        st->pts_buffer[0] = pkt->pts;
        for (i = 0; i<delay && st->pts_buffer[i] > st->pts_buffer[i + 1]; i++)
            FFSWAP(int64_t, st->pts_buffer[i], st->pts_buffer[i + 1]);

        if(has_decode_delay_been_guessed(st))
            pkt->dts = select_from_pts_buffer(st, st->pts_buffer, pkt->dts);
    }
    // We skipped it above so we try here.
    if (!onein_oneout)//15.
        // This should happen on the first packet
        update_initial_timestamps(s, pkt->stream_index, pkt->dts, pkt->pts, pkt);
    if (pkt->dts > st->cur_dts)
        st->cur_dts = pkt->dts;

    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG, "OUTdelayed:%d/%d pts:%s, dts:%s cur_dts:%s st:%d (%d)\n",
            presentation_delayed, delay, av_ts2str(pkt->pts), av_ts2str(pkt->dts), av_ts2str(st->cur_dts), st->index, st->id);
    //16.
    /* update flags */
    if (st->codecpar->codec_type == AVMEDIA_TYPE_DATA || is_intra_only(st->codecpar->codec_id))
        pkt->flags |= AV_PKT_FLAG_KEY;
#if FF_API_CONVERGENCE_DURATION
FF_DISABLE_DEPRECATION_WARNINGS
    if (pc)
        pkt->convergence_duration = pc->convergence_duration;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
}

void ff_packet_list_free(AVPacketList **pkt_buf, AVPacketList **pkt_buf_end)//list删除比较慢
{
    AVPacketList *tmp = *pkt_buf;

    while (tmp) {
        AVPacketList *pktl = tmp;
        tmp = pktl->next;
        av_packet_unref(&pktl->pkt);
        av_freep(&pktl);
    }
    *pkt_buf     = NULL;
    *pkt_buf_end = NULL;
}

/**
 * Parse a packet, add all split parts to parse_queue.
 *
 * @param pkt Packet to parse, NULL when flushing the parser at end of stream.
 */
static int parse_packet(AVFormatContext *s, AVPacket *pkt, int stream_index)//TIGER 看注释，解析包，将各部分加入parse_queue
{
    AVPacket out_pkt = { 0 }, flush_pkt = { 0 };
    AVStream *st = s->streams[stream_index];
    uint8_t *data = pkt ? pkt->data : NULL;
    int size      = pkt ? pkt->size : 0;//空间够大
    int ret = 0, got_output = 0;
    //01.
    if (!pkt) {//如果pkt 为空，使用堆栈上的AVPacket
        av_init_packet(&flush_pkt);//01.01 初始化堆栈上的AVPacket
        pkt        = &flush_pkt;
        got_output = 1;//标记需要输出
    } else if (!size && st->parser->flags & PARSER_FLAG_COMPLETE_FRAMES) {//如果size为0，且需要完整的帧
        // preserve 0-size sync packets
        compute_pkt_fields(s, st, st->parser, pkt, AV_NOPTS_VALUE, AV_NOPTS_VALUE);//超大函数
    }
    //02.需要解析
    while (size > 0 || (pkt == &flush_pkt && got_output)) {//如果需要解析的size大于0，或者用的是堆栈上的AVPacket
        int len;
        int64_t next_pts = pkt->pts;
        int64_t next_dts = pkt->dts;
        //02.01. 编码器解析av_parser_parse2
        av_init_packet(&out_pkt);
        len = av_parser_parse2(st->parser, st->internal->avctx,//av_read_frame-->read_frame_internal-->parse_packet-->av_parser_parse2-->h264_parse
                               &out_pkt.data, &out_pkt.size, data, size,
                               pkt->pts, pkt->dts, pkt->pos);
        //02.02.初始化pts
        pkt->pts = pkt->dts = AV_NOPTS_VALUE;
        pkt->pos = -1;
        /* increment read pointer */
        data += len;//成功解析len长度，所以前进len
        size -= len;
        //02.03. 如果size大于，就是得到输出了
        got_output = !!out_pkt.size;
        //02.04.如果输出的长度为0，再次解析。
        if (!out_pkt.size)
            continue;
        //02.05. 看注释 out_pkt.data 不指向parser's internal buffer==？
        if (pkt->buf && out_pkt.data == pkt->data) {
            /* reference pkt->buf only when out_pkt.data is guaranteed to point
             * to data in it and not in the parser's internal buffer. */
            /* XXX: Ensure this is the case with all parsers when st->parser->flags
             * is PARSER_FLAG_COMPLETE_FRAMES and check for that instead? */
            out_pkt.buf = av_buffer_ref(pkt->buf);
            if (!out_pkt.buf) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        } else {
            ret = av_packet_make_refcounted(&out_pkt);
            if (ret < 0)
                goto fail;
        }
        //02.06.
        if (pkt->side_data) {
            out_pkt.side_data       = pkt->side_data;
            out_pkt.side_data_elems = pkt->side_data_elems;
            pkt->side_data          = NULL;
            pkt->side_data_elems    = 0;
        }
        //02.07.
        /* set the duration */
        out_pkt.duration = (st->parser->flags & PARSER_FLAG_COMPLETE_FRAMES) ? pkt->duration : 0;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (st->internal->avctx->sample_rate > 0) {
                out_pkt.duration =
                    av_rescale_q_rnd(st->parser->duration,
                                     (AVRational) { 1, st->internal->avctx->sample_rate },
                                     st->time_base,
                                     AV_ROUND_DOWN);
            }
        }
        //02.08.
        out_pkt.stream_index = st->index;
        out_pkt.pts          = st->parser->pts;
        out_pkt.dts          = st->parser->dts;
        out_pkt.pos          = st->parser->pos;
        out_pkt.flags       |= pkt->flags & AV_PKT_FLAG_DISCARD;

        if (st->need_parsing == AVSTREAM_PARSE_FULL_RAW)
            out_pkt.pos = st->parser->frame_offset;

        if (st->parser->key_frame == 1 ||
            (st->parser->key_frame == -1 &&
             st->parser->pict_type == AV_PICTURE_TYPE_I))
            out_pkt.flags |= AV_PKT_FLAG_KEY;

        if (st->parser->key_frame == -1 && st->parser->pict_type ==AV_PICTURE_TYPE_NONE && (pkt->flags&AV_PKT_FLAG_KEY))
            out_pkt.flags |= AV_PKT_FLAG_KEY;
        //02.09.
        compute_pkt_fields(s, st, st->parser, &out_pkt, next_dts, next_pts);
        //02.10. 放入parse_queue队列
        ret = ff_packet_list_put(&s->internal->parse_queue,
                                 &s->internal->parse_queue_end,
                                 &out_pkt, 0);
        if (ret < 0) {
            av_packet_unref(&out_pkt);
            goto fail;
        }
    }

    /* end of the stream => close and free the parser */
    if (pkt == &flush_pkt) {//如果是堆栈上的AVPacket，关闭parser
        av_parser_close(st->parser);//为什么关闭？
        st->parser = NULL;
    }

fail:
    av_packet_unref(pkt);
    return ret;
}

int ff_packet_list_get(AVPacketList **pkt_buffer,
                       AVPacketList **pkt_buffer_end,
                       AVPacket      *pkt)//取单向列表第一个
{
    AVPacketList *pktl;
    av_assert0(*pkt_buffer);
    pktl        = *pkt_buffer;
    *pkt        = pktl->pkt;
    *pkt_buffer = pktl->next;
    if (!pktl->next)
        *pkt_buffer_end = NULL;
    av_freep(&pktl);
    return 0;
}

static int64_t ts_to_samples(AVStream *st, int64_t ts)//时间从AVStream的时间基准变为采样值
{
    return av_rescale(ts, st->time_base.num * st->codecpar->sample_rate, st->time_base.den);
}
//av_read_frame-->read_frame_internal -->ff_read_packet -->ff_rtsp_fetch_packet -->ff_rtp_parse_packet -->rtp_parse_one_packet -->rtp_parse_packet_internal-->h264_handle_packet
static int read_frame_internal(AVFormatContext *s, AVPacket *pkt)//TIGER read_frame_internal
{
    int ret = 0, i, got_packet = 0;
    AVDictionary *metadata = NULL;

    av_init_packet(pkt);//01.ffmpeg内部已经初始化，所以使用ffmpeg api中，再次初始化是不必要的 //tiger program
    //02.如果没有获取packet且parse_queue还没创建,读数据
    while (!got_packet && !s->internal->parse_queue) {
        AVStream *st;
        AVPacket cur_pkt;

        /* read next packet */
        ret = ff_read_packet(s, &cur_pkt);//02.01 实际读入口 ff_read_packet -->ff_rtsp_fetch_packet -->ff_rtp_parse_packet -->rtp_parse_one_packet -->rtp_parse_packet_internal-->h264_handle_packet
        if (ret < 0) {//如果读取失败，尝试最后一次解析所有流
            if (ret == AVERROR(EAGAIN))
                return ret;
            /* flush the parsers */
            for (i = 0; i < s->nb_streams; i++) {
                st = s->streams[i];
                if (st->parser && st->need_parsing)//parse_packet需要解码器支持，如果不支持，直接返回，但不会报错。
                    parse_packet(s, NULL, st->index);//放入null，最后一次解析各个流，并结束： av_read_frame-->read_frame_internal-->parse_packet-->av_parser_parse2-->h264_parse
            }
            /* all remaining packets are now in parse_queue =>
             * really terminate parsing */
            break;
        }
        ret = 0;//设置返回值是0
        st  = s->streams[cur_pkt.stream_index];
        //02.02 参数变化
        /* update context if required */
        if (st->internal->need_context_update) {//更新参数  ==>一般也就flv，hls会设置，或者初始化stream
            if (avcodec_is_open(st->internal->avctx)) {//已打开的AVCodecContext要关闭
                av_log(s, AV_LOG_DEBUG, "Demuxer context update while decoder is open, closing and trying to re-open\n");
                avcodec_close(st->internal->avctx);
                st->info->found_decoder = 0;//这个也设置为0
            }

            /* close parser, because it depends on the codec */
            if (st->parser && st->internal->avctx->codec_id != st->codecpar->codec_id) {//如果id变了，关闭parser
                av_parser_close(st->parser);
                st->parser = NULL;
            }

            ret = avcodec_parameters_to_context(st->internal->avctx, st->codecpar);//从AVCodecParameters复制到AVCodecContext *avctx
            if (ret < 0)
                return ret;

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
            /* update deprecated public codec context */
            ret = avcodec_parameters_to_context(st->codec, st->codecpar);//从AVCodecParameters复制到AVCodecContext* codec
            if (ret < 0)
                return ret;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

            st->internal->need_context_update = 0;//标记已处理
        }
        //02.03 如果pts <dts,提示错误  ==>但不理解
        if (cur_pkt.pts != AV_NOPTS_VALUE &&
            cur_pkt.dts != AV_NOPTS_VALUE &&
            cur_pkt.pts < cur_pkt.dts) {
            av_log(s, AV_LOG_WARNING,
                   "Invalid timestamps stream=%d, pts=%s, dts=%s, size=%d\n",
                   cur_pkt.stream_index,
                   av_ts2str(cur_pkt.pts),
                   av_ts2str(cur_pkt.dts),
                   cur_pkt.size);
        }
        if (s->debug & FF_FDEBUG_TS)//更多显示
            av_log(s, AV_LOG_DEBUG,
                   "ff_read_packet stream=%d, pts=%s, dts=%s, size=%d, duration=%"PRId64", flags=%d\n",
                   cur_pkt.stream_index,
                   av_ts2str(cur_pkt.pts),
                   av_ts2str(cur_pkt.dts),
                   cur_pkt.size, cur_pkt.duration, cur_pkt.flags);
        //02.04 需要使用的parser，但为空，初始化parser
        if (st->need_parsing && !st->parser && !(s->flags & AVFMT_FLAG_NOPARSE)) {
            st->parser = av_parser_init(st->codecpar->codec_id);//找到parser
            if (!st->parser) {
                av_log(s, AV_LOG_VERBOSE, "parser not found for codec "
                       "%s, packets or times may be invalid.\n",
                       avcodec_get_name(st->codecpar->codec_id));
                /* no parser available: just output the raw packets */
                st->need_parsing = AVSTREAM_PARSE_NONE;//已经有parser了
            } else if (st->need_parsing == AVSTREAM_PARSE_HEADERS)
                st->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;//TIGER TODO 理解多个flag
            else if (st->need_parsing == AVSTREAM_PARSE_FULL_ONCE)
                st->parser->flags |= PARSER_FLAG_ONCE;
            else if (st->need_parsing == AVSTREAM_PARSE_FULL_RAW)
                st->parser->flags |= PARSER_FLAG_USE_CODEC_TS;
        }
        //02.05 不需要parse  ==>举例: alaw 就没有st->parser ==0，st->need_parsing==0
        if (!st->need_parsing || !st->parser) {//st->codec:
            /* no parsing needed: we just output the packet as is */
            *pkt = cur_pkt;//直接输出即可
            compute_pkt_fields(s, st, NULL, pkt, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
            if ((s->iformat->flags & AVFMT_GENERIC_INDEX) &&
                (pkt->flags & AV_PKT_FLAG_KEY) && pkt->dts != AV_NOPTS_VALUE) {//TIGER ALAW 文件再次停留：
                ff_reduce_index(s, st->index);//若条目太多，舍弃一半
                av_add_index_entry(st, pkt->pos, pkt->dts,
                                   0, 0, AVINDEX_KEYFRAME); //加入st->index_entries,以便更好索引
            }
            got_packet = 1;
        } else if (st->discard < AVDISCARD_ALL) {//如果无需丢弃所有
            if ((ret = parse_packet(s, &cur_pkt, cur_pkt.stream_index)) < 0)
                return ret;
            st->codecpar->sample_rate = st->internal->avctx->sample_rate;
            st->codecpar->bit_rate = st->internal->avctx->bit_rate;
            st->codecpar->channels = st->internal->avctx->channels;
            st->codecpar->channel_layout = st->internal->avctx->channel_layout;
            st->codecpar->codec_id = st->internal->avctx->codec_id;
        } else {
            /* free packet */
            av_packet_unref(&cur_pkt);
        }
        if (pkt->flags & AV_PKT_FLAG_KEY)//02.06
            st->skip_to_keyframe = 0;
        if (st->skip_to_keyframe) {//02.07
            av_packet_unref(&cur_pkt);
            if (got_packet) {
                *pkt = cur_pkt;
            }
            got_packet = 0;
        }
    }
    //03.如果没有得到数据，而且队列已有解析好的数据，直接从队列中取一个
    if (!got_packet && s->internal->parse_queue)
        ret = ff_packet_list_get(&s->internal->parse_queue, &s->internal->parse_queue_end, pkt);

    //04.如果AVPacket获取成功，设置side_data，(但alaw，几乎不用做什么，没有discard，没有side_data， metadata)
    if (ret >= 0) {//?如果find_stream_info等，要跳过sample，那后期数据需要减去？
        AVStream *st = s->streams[pkt->stream_index];
        int discard_padding = 0;
        if (st->first_discard_sample && pkt->pts != AV_NOPTS_VALUE) {//04.01 pkt->pts 不为空，且需要跳过n个采样值，则计算跳过的具体位置
            int64_t pts = pkt->pts - (is_relative(pkt->pts) ? RELATIVE_TS_BASE : 0);
            int64_t sample = ts_to_samples(st, pts);
            int duration = ts_to_samples(st, pkt->duration);//时间折算
            int64_t end_sample = sample + duration;
            if (duration > 0 && end_sample >= st->first_discard_sample &&
                sample < st->last_discard_sample)
                discard_padding = FFMIN(end_sample - st->first_discard_sample, duration);//有时候还要考虑padding问题
        }
        if (st->start_skip_samples && (pkt->pts == 0 || pkt->pts == RELATIVE_TS_BASE))//04.02 从pts为0开始
            st->skip_samples = st->start_skip_samples;
        if (st->skip_samples || discard_padding) {//04.03写到side_data里面即可
            uint8_t *p = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);//pkt的side_data加入 AV_PKT_DATA_SKIP_SAMPLES一项
            if (p) {
                AV_WL32(p, st->skip_samples);
                AV_WL32(p + 4, discard_padding);
                av_log(s, AV_LOG_DEBUG, "demuxer injecting skip %d / discard %d\n", st->skip_samples, discard_padding);
            }
            st->skip_samples = 0;//清零
        }

        if (st->inject_global_side_data) {//04.04 pkt里面注入AVStream的side_data
            for (i = 0; i < st->nb_side_data; i++) {
                AVPacketSideData *src_sd = &st->side_data[i];
                uint8_t *dst_data;

                if (av_packet_get_side_data(pkt, src_sd->type, NULL))
                    continue;

                dst_data = av_packet_new_side_data(pkt, src_sd->type, src_sd->size);
                if (!dst_data) {
                    av_log(s, AV_LOG_WARNING, "Could not inject global side data\n");
                    continue;
                }

                memcpy(dst_data, src_sd->data, src_sd->size);
            }
            st->inject_global_side_data = 0;//清零
        }
    }
    //05.如果有metadata更新需要通知
    av_opt_get_dict_val(s, "metadata", AV_OPT_SEARCH_CHILDREN, &metadata);//TIGER 如果有元数据
    if (metadata) {//TIGER 是否更新meta， 设置一个事件
        s->event_flags |= AVFMT_EVENT_FLAG_METADATA_UPDATED;//TIGER PROGRAM
        av_dict_copy(&s->metadata, metadata, 0);//保存在AVFormatContext的s->metadata中
        av_dict_free(&metadata);
        av_opt_set_dict_val(s, "metadata", NULL, AV_OPT_SEARCH_CHILDREN);
    }
    //06.几个流里面逐一处理：从流里面获取参数更新到st->internal->avctx 和st->codec
#if FF_API_LAVF_AVCTX
    update_stream_avctx(s);//TIGER 如果未更新，更新一下 avcodec_parameters_to_context(st->codec, st->codecpar); 和 avcodec_parameters_to_context(st->internal->avctx, st->codecpar);
#endif

    if (s->debug & FF_FDEBUG_TS)//06.TIGER program 编程参考 可以增加日志
        av_log(s, AV_LOG_DEBUG,//日志输出
               "read_frame_internal stream=%d, pts=%s, dts=%s, "
               "size=%d, duration=%"PRId64", flags=%d\n",
               pkt->stream_index,
               av_ts2str(pkt->pts),
               av_ts2str(pkt->dts),
               pkt->size, pkt->duration, pkt->flags);

    return ret;
}
//TIGER av_read_frame对av_read_packet进行了封装，使读出的数据总是完整的帧。 Vs av_read_packet的区别是读出的是包，它可能是半帧或多帧，不保证帧的完整性。
int av_read_frame(AVFormatContext *s, AVPacket *pkt)
{
    const int genpts = s->flags & AVFMT_FLAG_GENPTS;
    int eof = 0;
    int ret;
    AVStream *st;

    if (!genpts) {//如果不是自动产生时间戳
        ret = s->internal->packet_buffer//见注释:如果队列里有数据，比如get the codec parameters in MPEG解析后的
              ? ff_packet_list_get(&s->internal->packet_buffer,
                                        &s->internal->packet_buffer_end, pkt)
              : read_frame_internal(s, pkt);
        if (ret < 0)
            return ret;//
        goto return_packet;//如果读到packet,处理一下
    }

    for (;;) {
        AVPacketList *pktl = s->internal->packet_buffer;

        if (pktl) {
            AVPacket *next_pkt = &pktl->pkt;

            if (next_pkt->dts != AV_NOPTS_VALUE) {
                int wrap_bits = s->streams[next_pkt->stream_index]->pts_wrap_bits;
                // last dts seen for this stream. if any of packets following
                // current one had no dts, we will set this to AV_NOPTS_VALUE.
                int64_t last_dts = next_pkt->dts;
                av_assert2(wrap_bits <= 64);
                while (pktl && next_pkt->pts == AV_NOPTS_VALUE) {
                    if (pktl->pkt.stream_index == next_pkt->stream_index &&
                        av_compare_mod(next_pkt->dts, pktl->pkt.dts, 2ULL << (wrap_bits - 1)) < 0) {
                        if (av_compare_mod(pktl->pkt.pts, pktl->pkt.dts, 2ULL << (wrap_bits - 1))) {
                            // not B-frame
                            next_pkt->pts = pktl->pkt.dts;
                        }
                        if (last_dts != AV_NOPTS_VALUE) {
                            // Once last dts was set to AV_NOPTS_VALUE, we don't change it.
                            last_dts = pktl->pkt.dts;//只有在没有的时候才填上dts
                        }
                    }
                    pktl = pktl->next;
                }
                if (eof && next_pkt->pts == AV_NOPTS_VALUE && last_dts != AV_NOPTS_VALUE) {
                    // Fixing the last reference frame had none pts issue (For MXF etc).
                    // We only do this when
                    // 1. eof.
                    // 2. we are not able to resolve a pts value for current packet.
                    // 3. the packets for this stream at the end of the files had valid dts.
                    next_pkt->pts = last_dts + next_pkt->duration;////只有在没有的时候才填上pts
                }
                pktl = s->internal->packet_buffer;
            }

            /* read packet from packet buffer, if there is data */
            st = s->streams[next_pkt->stream_index];
            if (!(next_pkt->pts == AV_NOPTS_VALUE && st->discard < AVDISCARD_ALL &&
                  next_pkt->dts != AV_NOPTS_VALUE && !eof)) {
                ret = ff_packet_list_get(&s->internal->packet_buffer,
                                               &s->internal->packet_buffer_end, pkt);
                goto return_packet;
            }
        }
        //真正的read_frame
        ret = read_frame_internal(s, pkt);//读frame
        if (ret < 0) {
            if (pktl && ret != AVERROR(EAGAIN)) {
                eof = 1;
                continue;
            } else
                return ret;
        }

        ret = ff_packet_list_put(&s->internal->packet_buffer,//放入单向列表list
                                 &s->internal->packet_buffer_end,
                                 pkt, FF_PACKETLIST_FLAG_REF_PACKET);
        av_packet_unref(pkt);
        if (ret < 0)
            return ret;
    }

return_packet:
    //tiger
    st = s->streams[pkt->stream_index];
    if ((s->iformat->flags & AVFMT_GENERIC_INDEX) && pkt->flags & AV_PKT_FLAG_KEY) {
        ff_reduce_index(s, st->index);
        av_add_index_entry(st, pkt->pos, pkt->dts, 0, 0, AVINDEX_KEYFRAME);//TIGER TODO5:啥意思？ 
    }

    if (is_relative(pkt->dts))//TIGER TODO
        pkt->dts -= RELATIVE_TS_BASE;//为什么为负数
    if (is_relative(pkt->pts))
        pkt->pts -= RELATIVE_TS_BASE;

    return ret;
}

/* XXX: suppress the packet queue */
static void flush_packet_queue(AVFormatContext *s)
{
    if (!s->internal)
        return;
    ff_packet_list_free(&s->internal->parse_queue,       &s->internal->parse_queue_end);
    ff_packet_list_free(&s->internal->packet_buffer,     &s->internal->packet_buffer_end);
    ff_packet_list_free(&s->internal->raw_packet_buffer, &s->internal->raw_packet_buffer_end);

    s->internal->raw_packet_buffer_remaining_size = RAW_PACKET_BUFFER_SIZE;
}

/*******************************************************/
/* seek support */

int av_find_default_stream_index(AVFormatContext *s)
{
    int i;
    AVStream *st;
    int best_stream = 0;
    int best_score = INT_MIN;

    if (s->nb_streams <= 0)
        return -1;
    for (i = 0; i < s->nb_streams; i++) {
        int score = 0;
        st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                score -= 400;
            if (st->codecpar->width && st->codecpar->height)
                score += 50;
            score+= 25;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (st->codecpar->sample_rate)
                score += 50;
        }
        if (st->codec_info_nb_frames)
            score += 12;

        if (st->discard != AVDISCARD_ALL)
            score += 200;

        if (score > best_score) {
            best_score = score;
            best_stream = i;
        }
    }
    return best_stream;
}

/** Flush the frame reader. */
void ff_read_frame_flush(AVFormatContext *s)
{
    AVStream *st;
    int i, j;

    flush_packet_queue(s);

    /* Reset read state for each stream. */
    for (i = 0; i < s->nb_streams; i++) {
        st = s->streams[i];

        if (st->parser) {
            av_parser_close(st->parser);
            st->parser = NULL;
        }
        st->last_IP_pts = AV_NOPTS_VALUE;
        st->last_dts_for_order_check = AV_NOPTS_VALUE;
        if (st->first_dts == AV_NOPTS_VALUE)
            st->cur_dts = RELATIVE_TS_BASE;
        else
            /* We set the current DTS to an unspecified origin. */
            st->cur_dts = AV_NOPTS_VALUE;

        st->probe_packets = MAX_PROBE_PACKETS;

        for (j = 0; j < MAX_REORDER_DELAY + 1; j++)
            st->pts_buffer[j] = AV_NOPTS_VALUE;

        if (s->internal->inject_global_side_data)
            st->inject_global_side_data = 1;

        st->skip_samples = 0;
    }
}

void ff_update_cur_dts(AVFormatContext *s, AVStream *ref_st, int64_t timestamp)
{
    int i;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];

        st->cur_dts =
            av_rescale(timestamp,
                       st->time_base.den * (int64_t) ref_st->time_base.num,
                       st->time_base.num * (int64_t) ref_st->time_base.den);
    }
}

void ff_reduce_index(AVFormatContext *s, int stream_index)//如果st->index_entries插入条目太多，舍弃其中的一半
{
    AVStream *st             = s->streams[stream_index];
    unsigned int max_entries = s->max_index_size / sizeof(AVIndexEntry);//01.alaw：s->max_index_size= 1048576， sizeof(AVIndexEntry) = 24， max_entries=43690

    if ((unsigned) st->nb_index_entries >= max_entries) {//02.如果entry太多了
        int i;
        for (i = 0; 2 * i < st->nb_index_entries; i++)//03.循环直到最大值
            st->index_entries[i] = st->index_entries[2 * i];//04.  st->index_entries看注释 舍弃其中的一半
        st->nb_index_entries = i;//05.最后的值
    }
}

int ff_add_index_entry(AVIndexEntry **index_entries,
                       int *nb_index_entries,
                       unsigned int *index_entries_allocated_size,
                       int64_t pos, int64_t timestamp,
                       int size, int distance, int flags)//TIGER 加入以便更好索引: 加一个entry，比如alaw，需要加入一条
{
    AVIndexEntry *entries, *ie;
    int index;

    if ((unsigned) *nb_index_entries + 1 >= UINT_MAX / sizeof(AVIndexEntry))
        return -1;

    if (timestamp == AV_NOPTS_VALUE)
        return AVERROR(EINVAL);

    if (size < 0 || size > 0x3FFFFFFF)
        return AVERROR(EINVAL);

    if (is_relative(timestamp)) //FIXME this maintains previous behavior but we should shift by the correct offset once known
        timestamp -= RELATIVE_TS_BASE;

    entries = av_fast_realloc(*index_entries,
                              index_entries_allocated_size,
                              (*nb_index_entries + 1) *//举例 alaw ： (*nb_index_entries + 1) = 1， sizeof(AVIndexEntry)=24
                              sizeof(AVIndexEntry));
    if (!entries)
        return -1;

    *index_entries = entries;

    index = ff_index_search_timestamp(*index_entries, *nb_index_entries,
                                      timestamp, AVSEEK_FLAG_ANY);

    if (index < 0) {
        index = (*nb_index_entries)++;
        ie    = &entries[index];
        av_assert0(index == 0 || ie[-1].timestamp < timestamp);
    } else {
        ie = &entries[index];
        if (ie->timestamp != timestamp) {
            if (ie->timestamp <= timestamp)
                return -1;
            memmove(entries + index + 1, entries + index,
                    sizeof(AVIndexEntry) * (*nb_index_entries - index));
            (*nb_index_entries)++;
        } else if (ie->pos == pos && distance < ie->min_distance)
            // do not reduce the distance
            distance = ie->min_distance;
    }

    ie->pos          = pos;
    ie->timestamp    = timestamp;
    ie->min_distance = distance;
    ie->size         = size;
    ie->flags        = flags;

    return index;
}

int av_add_index_entry(AVStream *st, int64_t pos, int64_t timestamp,//加入&st->index_entries以便更好索引
                       int size, int distance, int flags)
{
    timestamp = wrap_timestamp(st, timestamp);
    return ff_add_index_entry(&st->index_entries, &st->nb_index_entries,
                              &st->index_entries_allocated_size, pos,
                              timestamp, size, distance, flags);
}

int ff_index_search_timestamp(const AVIndexEntry *entries, int nb_entries,
                              int64_t wanted_timestamp, int flags)
{
    int a, b, m;
    int64_t timestamp;

    a = -1;
    b = nb_entries;

    // Optimize appending index entries at the end.
    if (b && entries[b - 1].timestamp < wanted_timestamp)
        a = b - 1;

    while (b - a > 1) {
        m         = (a + b) >> 1;

        // Search for the next non-discarded packet.
        while ((entries[m].flags & AVINDEX_DISCARD_FRAME) && m < b && m < nb_entries - 1) {
            m++;
            if (m == b && entries[m].timestamp >= wanted_timestamp) {
                m = b - 1;
                break;
            }
        }

        timestamp = entries[m].timestamp;
        if (timestamp >= wanted_timestamp)
            b = m;
        if (timestamp <= wanted_timestamp)
            a = m;
    }
    m = (flags & AVSEEK_FLAG_BACKWARD) ? a : b;

    if (!(flags & AVSEEK_FLAG_ANY))
        while (m >= 0 && m < nb_entries &&
               !(entries[m].flags & AVINDEX_KEYFRAME))
            m += (flags & AVSEEK_FLAG_BACKWARD) ? -1 : 1;

    if (m == nb_entries)
        return -1;
    return m;
}

void ff_configure_buffers_for_index(AVFormatContext *s, int64_t time_tolerance)
{
    int ist1, ist2;
    int64_t pos_delta = 0;
    int64_t skip = 0;
    //We could use URLProtocol flags here but as many user applications do not use URLProtocols this would be unreliable
    const char *proto = avio_find_protocol_name(s->url);

    if (!proto) {
        av_log(s, AV_LOG_INFO,
               "Protocol name not provided, cannot determine if input is local or "
               "a network protocol, buffers and access patterns cannot be configured "
               "optimally without knowing the protocol\n");
    }

    if (proto && !(strcmp(proto, "file") && strcmp(proto, "pipe") && strcmp(proto, "cache")))
        return;

    for (ist1 = 0; ist1 < s->nb_streams; ist1++) {
        AVStream *st1 = s->streams[ist1];
        for (ist2 = 0; ist2 < s->nb_streams; ist2++) {
            AVStream *st2 = s->streams[ist2];
            int i1, i2;

            if (ist1 == ist2)
                continue;

            for (i1 = i2 = 0; i1 < st1->nb_index_entries; i1++) {
                AVIndexEntry *e1 = &st1->index_entries[i1];
                int64_t e1_pts = av_rescale_q(e1->timestamp, st1->time_base, AV_TIME_BASE_Q);

                skip = FFMAX(skip, e1->size);
                for (; i2 < st2->nb_index_entries; i2++) {
                    AVIndexEntry *e2 = &st2->index_entries[i2];
                    int64_t e2_pts = av_rescale_q(e2->timestamp, st2->time_base, AV_TIME_BASE_Q);
                    if (e2_pts - e1_pts < time_tolerance)
                        continue;
                    pos_delta = FFMAX(pos_delta, e1->pos - e2->pos);
                    break;
                }
            }
        }
    }

    pos_delta *= 2;
    /* XXX This could be adjusted depending on protocol*/
    if (s->pb->buffer_size < pos_delta && pos_delta < (1<<24)) {
        av_log(s, AV_LOG_VERBOSE, "Reconfiguring buffers to size %"PRId64"\n", pos_delta);
        ffio_set_buf_size(s->pb, pos_delta);
        s->pb->short_seek_threshold = FFMAX(s->pb->short_seek_threshold, pos_delta/2);
    }

    if (skip < (1<<23)) {
        s->pb->short_seek_threshold = FFMAX(s->pb->short_seek_threshold, skip);
    }
}

int av_index_search_timestamp(AVStream *st, int64_t wanted_timestamp, int flags)
{
    return ff_index_search_timestamp(st->index_entries, st->nb_index_entries,
                                     wanted_timestamp, flags);
}

static int64_t ff_read_timestamp(AVFormatContext *s, int stream_index, int64_t *ppos, int64_t pos_limit,
                                 int64_t (*read_timestamp)(struct AVFormatContext *, int , int64_t *, int64_t ))
{
    int64_t ts = read_timestamp(s, stream_index, ppos, pos_limit);
    if (stream_index >= 0)
        ts = wrap_timestamp(s->streams[stream_index], ts);
    return ts;
}

int ff_seek_frame_binary(AVFormatContext *s, int stream_index,
                         int64_t target_ts, int flags)
{
    const AVInputFormat *avif = s->iformat;
    int64_t av_uninit(pos_min), av_uninit(pos_max), pos, pos_limit;
    int64_t ts_min, ts_max, ts;
    int index;
    int64_t ret;
    AVStream *st;

    if (stream_index < 0)
        return -1;

    av_log(s, AV_LOG_TRACE, "read_seek: %d %s\n", stream_index, av_ts2str(target_ts));

    ts_max =
    ts_min = AV_NOPTS_VALUE;
    pos_limit = -1; // GCC falsely says it may be uninitialized.

    st = s->streams[stream_index];
    if (st->index_entries) {
        AVIndexEntry *e;

        /* FIXME: Whole function must be checked for non-keyframe entries in
         * index case, especially read_timestamp(). */
        index = av_index_search_timestamp(st, target_ts,
                                          flags | AVSEEK_FLAG_BACKWARD);
        index = FFMAX(index, 0);
        e     = &st->index_entries[index];

        if (e->timestamp <= target_ts || e->pos == e->min_distance) {
            pos_min = e->pos;
            ts_min  = e->timestamp;
            av_log(s, AV_LOG_TRACE, "using cached pos_min=0x%"PRIx64" dts_min=%s\n",
                    pos_min, av_ts2str(ts_min));
        } else {
            av_assert1(index == 0);
        }

        index = av_index_search_timestamp(st, target_ts,
                                          flags & ~AVSEEK_FLAG_BACKWARD);
        av_assert0(index < st->nb_index_entries);
        if (index >= 0) {
            e = &st->index_entries[index];
            av_assert1(e->timestamp >= target_ts);
            pos_max   = e->pos;
            ts_max    = e->timestamp;
            pos_limit = pos_max - e->min_distance;
            av_log(s, AV_LOG_TRACE, "using cached pos_max=0x%"PRIx64" pos_limit=0x%"PRIx64
                    " dts_max=%s\n", pos_max, pos_limit, av_ts2str(ts_max));
        }
    }

    pos = ff_gen_search(s, stream_index, target_ts, pos_min, pos_max, pos_limit,
                        ts_min, ts_max, flags, &ts, avif->read_timestamp);
    if (pos < 0)
        return -1;

    /* do the seek */
    if ((ret = avio_seek(s->pb, pos, SEEK_SET)) < 0)
        return ret;

    ff_read_frame_flush(s);
    ff_update_cur_dts(s, st, ts);

    return 0;
}

int ff_find_last_ts(AVFormatContext *s, int stream_index, int64_t *ts, int64_t *pos,
                    int64_t (*read_timestamp)(struct AVFormatContext *, int , int64_t *, int64_t ))
{
    int64_t step = 1024;
    int64_t limit, ts_max;
    int64_t filesize = avio_size(s->pb);
    int64_t pos_max  = filesize - 1;
    do {
        limit = pos_max;
        pos_max = FFMAX(0, (pos_max) - step);
        ts_max  = ff_read_timestamp(s, stream_index,
                                    &pos_max, limit, read_timestamp);
        step   += step;
    } while (ts_max == AV_NOPTS_VALUE && 2*limit > step);
    if (ts_max == AV_NOPTS_VALUE)
        return -1;

    for (;;) {
        int64_t tmp_pos = pos_max + 1;
        int64_t tmp_ts  = ff_read_timestamp(s, stream_index,
                                            &tmp_pos, INT64_MAX, read_timestamp);
        if (tmp_ts == AV_NOPTS_VALUE)
            break;
        av_assert0(tmp_pos > pos_max);
        ts_max  = tmp_ts;
        pos_max = tmp_pos;
        if (tmp_pos >= filesize)
            break;
    }

    if (ts)
        *ts = ts_max;
    if (pos)
        *pos = pos_max;

    return 0;
}

int64_t ff_gen_search(AVFormatContext *s, int stream_index, int64_t target_ts,
                      int64_t pos_min, int64_t pos_max, int64_t pos_limit,
                      int64_t ts_min, int64_t ts_max,
                      int flags, int64_t *ts_ret,
                      int64_t (*read_timestamp)(struct AVFormatContext *, int,
                                                int64_t *, int64_t))
{
    int64_t pos, ts;
    int64_t start_pos;
    int no_change;
    int ret;

    av_log(s, AV_LOG_TRACE, "gen_seek: %d %s\n", stream_index, av_ts2str(target_ts));

    if (ts_min == AV_NOPTS_VALUE) {
        pos_min = s->internal->data_offset;
        ts_min  = ff_read_timestamp(s, stream_index, &pos_min, INT64_MAX, read_timestamp);
        if (ts_min == AV_NOPTS_VALUE)
            return -1;
    }

    if (ts_min >= target_ts) {
        *ts_ret = ts_min;
        return pos_min;
    }

    if (ts_max == AV_NOPTS_VALUE) {
        if ((ret = ff_find_last_ts(s, stream_index, &ts_max, &pos_max, read_timestamp)) < 0)
            return ret;
        pos_limit = pos_max;
    }

    if (ts_max <= target_ts) {
        *ts_ret = ts_max;
        return pos_max;
    }

    av_assert0(ts_min < ts_max);

    no_change = 0;
    while (pos_min < pos_limit) {
        av_log(s, AV_LOG_TRACE,
                "pos_min=0x%"PRIx64" pos_max=0x%"PRIx64" dts_min=%s dts_max=%s\n",
                pos_min, pos_max, av_ts2str(ts_min), av_ts2str(ts_max));
        av_assert0(pos_limit <= pos_max);

        if (no_change == 0) {
            int64_t approximate_keyframe_distance = pos_max - pos_limit;
            // interpolate position (better than dichotomy)
            pos = av_rescale(target_ts - ts_min, pos_max - pos_min,
                             ts_max - ts_min) +
                  pos_min - approximate_keyframe_distance;
        } else if (no_change == 1) {
            // bisection if interpolation did not change min / max pos last time
            pos = (pos_min + pos_limit) >> 1;
        } else {
            /* linear search if bisection failed, can only happen if there
             * are very few or no keyframes between min/max */
            pos = pos_min;
        }
        if (pos <= pos_min)
            pos = pos_min + 1;
        else if (pos > pos_limit)
            pos = pos_limit;
        start_pos = pos;

        // May pass pos_limit instead of -1.
        ts = ff_read_timestamp(s, stream_index, &pos, INT64_MAX, read_timestamp);
        if (pos == pos_max)
            no_change++;
        else
            no_change = 0;
        av_log(s, AV_LOG_TRACE, "%"PRId64" %"PRId64" %"PRId64" / %s %s %s"
                " target:%s limit:%"PRId64" start:%"PRId64" noc:%d\n",
                pos_min, pos, pos_max,
                av_ts2str(ts_min), av_ts2str(ts), av_ts2str(ts_max), av_ts2str(target_ts),
                pos_limit, start_pos, no_change);
        if (ts == AV_NOPTS_VALUE) {
            av_log(s, AV_LOG_ERROR, "read_timestamp() failed in the middle\n");
            return -1;
        }
        if (target_ts <= ts) {
            pos_limit = start_pos - 1;
            pos_max   = pos;
            ts_max    = ts;
        }
        if (target_ts >= ts) {
            pos_min = pos;
            ts_min  = ts;
        }
    }

    pos     = (flags & AVSEEK_FLAG_BACKWARD) ? pos_min : pos_max;
    ts      = (flags & AVSEEK_FLAG_BACKWARD) ? ts_min  : ts_max;
#if 0
    pos_min = pos;
    ts_min  = ff_read_timestamp(s, stream_index, &pos_min, INT64_MAX, read_timestamp);
    pos_min++;
    ts_max = ff_read_timestamp(s, stream_index, &pos_min, INT64_MAX, read_timestamp);
    av_log(s, AV_LOG_TRACE, "pos=0x%"PRIx64" %s<=%s<=%s\n",
            pos, av_ts2str(ts_min), av_ts2str(target_ts), av_ts2str(ts_max));
#endif
    *ts_ret = ts;
    return pos;
}

static int seek_frame_byte(AVFormatContext *s, int stream_index,
                           int64_t pos, int flags)
{
    int64_t pos_min, pos_max;

    pos_min = s->internal->data_offset;
    pos_max = avio_size(s->pb) - 1;

    if (pos < pos_min)
        pos = pos_min;
    else if (pos > pos_max)
        pos = pos_max;

    avio_seek(s->pb, pos, SEEK_SET);

    s->io_repositioned = 1;

    return 0;
}

static int seek_frame_generic(AVFormatContext *s, int stream_index,
                              int64_t timestamp, int flags)
{
    int index;
    int64_t ret;
    AVStream *st;
    AVIndexEntry *ie;

    st = s->streams[stream_index];

    index = av_index_search_timestamp(st, timestamp, flags);

    if (index < 0 && st->nb_index_entries &&
        timestamp < st->index_entries[0].timestamp)
        return -1;

    if (index < 0 || index == st->nb_index_entries - 1) {
        AVPacket pkt;
        int nonkey = 0;

        if (st->nb_index_entries) {
            av_assert0(st->index_entries);
            ie = &st->index_entries[st->nb_index_entries - 1];
            if ((ret = avio_seek(s->pb, ie->pos, SEEK_SET)) < 0)
                return ret;
            ff_update_cur_dts(s, st, ie->timestamp);
        } else {
            if ((ret = avio_seek(s->pb, s->internal->data_offset, SEEK_SET)) < 0)
                return ret;
        }
        for (;;) {
            int read_status;
            do {
                read_status = av_read_frame(s, &pkt);
            } while (read_status == AVERROR(EAGAIN));
            if (read_status < 0)
                break;
            if (stream_index == pkt.stream_index && pkt.dts > timestamp) {
                if (pkt.flags & AV_PKT_FLAG_KEY) {
                    av_packet_unref(&pkt);
                    break;
                }
                if (nonkey++ > 1000 && st->codecpar->codec_id != AV_CODEC_ID_CDGRAPHICS) {
                    av_log(s, AV_LOG_ERROR,"seek_frame_generic failed as this stream seems to contain no keyframes after the target timestamp, %d non keyframes found\n", nonkey);
                    av_packet_unref(&pkt);
                    break;
                }
            }
            av_packet_unref(&pkt);
        }
        index = av_index_search_timestamp(st, timestamp, flags);
    }
    if (index < 0)
        return -1;

    ff_read_frame_flush(s);
    if (s->iformat->read_seek)
        if (s->iformat->read_seek(s, stream_index, timestamp, flags) >= 0)
            return 0;
    ie = &st->index_entries[index];
    if ((ret = avio_seek(s->pb, ie->pos, SEEK_SET)) < 0)
        return ret;
    ff_update_cur_dts(s, st, ie->timestamp);

    return 0;
}

static int seek_frame_internal(AVFormatContext *s, int stream_index,
                               int64_t timestamp, int flags)
{
    int ret;
    AVStream *st;

    if (flags & AVSEEK_FLAG_BYTE) {
        if (s->iformat->flags & AVFMT_NO_BYTE_SEEK)
            return -1;
        ff_read_frame_flush(s);
        return seek_frame_byte(s, stream_index, timestamp, flags);
    }

    if (stream_index < 0) {
        stream_index = av_find_default_stream_index(s);
        if (stream_index < 0)
            return -1;

        st = s->streams[stream_index];
        /* timestamp for default must be expressed in AV_TIME_BASE units */
        timestamp = av_rescale(timestamp, st->time_base.den,
                               AV_TIME_BASE * (int64_t) st->time_base.num);
    }

    /* first, we try the format specific seek */
    if (s->iformat->read_seek) {
        ff_read_frame_flush(s);
        ret = s->iformat->read_seek(s, stream_index, timestamp, flags);
    } else
        ret = -1;
    if (ret >= 0)
        return 0;

    if (s->iformat->read_timestamp &&
        !(s->iformat->flags & AVFMT_NOBINSEARCH)) {
        ff_read_frame_flush(s);
        return ff_seek_frame_binary(s, stream_index, timestamp, flags);
    } else if (!(s->iformat->flags & AVFMT_NOGENSEARCH)) {
        ff_read_frame_flush(s);
        return seek_frame_generic(s, stream_index, timestamp, flags);
    } else
        return -1;
}

int av_seek_frame(AVFormatContext *s, int stream_index,
                  int64_t timestamp, int flags)
{
    int ret;

    if (s->iformat->read_seek2 && !s->iformat->read_seek) {
        int64_t min_ts = INT64_MIN, max_ts = INT64_MAX;
        if ((flags & AVSEEK_FLAG_BACKWARD))
            max_ts = timestamp;
        else
            min_ts = timestamp;
        return avformat_seek_file(s, stream_index, min_ts, timestamp, max_ts,
                                  flags & ~AVSEEK_FLAG_BACKWARD);
    }

    ret = seek_frame_internal(s, stream_index, timestamp, flags);

    if (ret >= 0)
        ret = avformat_queue_attached_pictures(s);

    return ret;
}

int avformat_seek_file(AVFormatContext *s, int stream_index, int64_t min_ts,
                       int64_t ts, int64_t max_ts, int flags)
{
    if (min_ts > ts || max_ts < ts)
        return -1;
    if (stream_index < -1 || stream_index >= (int)s->nb_streams)
        return AVERROR(EINVAL);

    if (s->seek2any>0)
        flags |= AVSEEK_FLAG_ANY;
    flags &= ~AVSEEK_FLAG_BACKWARD;

    if (s->iformat->read_seek2) {
        int ret;
        ff_read_frame_flush(s);

        if (stream_index == -1 && s->nb_streams == 1) {
            AVRational time_base = s->streams[0]->time_base;
            ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);
            min_ts = av_rescale_rnd(min_ts, time_base.den,
                                    time_base.num * (int64_t)AV_TIME_BASE,
                                    AV_ROUND_UP   | AV_ROUND_PASS_MINMAX);
            max_ts = av_rescale_rnd(max_ts, time_base.den,
                                    time_base.num * (int64_t)AV_TIME_BASE,
                                    AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX);
            stream_index = 0;
        }

        ret = s->iformat->read_seek2(s, stream_index, min_ts,
                                     ts, max_ts, flags);

        if (ret >= 0)
            ret = avformat_queue_attached_pictures(s);
        return ret;
    }

    if (s->iformat->read_timestamp) {
        // try to seek via read_timestamp()
    }

    // Fall back on old API if new is not implemented but old is.
    // Note the old API has somewhat different semantics.
    if (s->iformat->read_seek || 1) {
        int dir = (ts - (uint64_t)min_ts > (uint64_t)max_ts - ts ? AVSEEK_FLAG_BACKWARD : 0);
        int ret = av_seek_frame(s, stream_index, ts, flags | dir);
        if (ret<0 && ts != min_ts && max_ts != ts) {
            ret = av_seek_frame(s, stream_index, dir ? max_ts : min_ts, flags | dir);
            if (ret >= 0)
                ret = av_seek_frame(s, stream_index, ts, flags | (dir^AVSEEK_FLAG_BACKWARD));
        }
        return ret;
    }

    // try some generic seek like seek_frame_generic() but with new ts semantics
    return -1; //unreachable
}

int avformat_flush(AVFormatContext *s)
{
    ff_read_frame_flush(s);
    return 0;
}

/*******************************************************/

/**
 * Return TRUE if the stream has accurate duration in any stream.
 *
 * @return TRUE if the stream has accurate duration for at least one component.
 */
static int has_duration(AVFormatContext *ic)
{
    int i;
    AVStream *st;

    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->duration != AV_NOPTS_VALUE)
            return 1;
    }
    if (ic->duration != AV_NOPTS_VALUE)
        return 1;
    return 0;
}

/**
 * Estimate the stream timings from the one of each components.
 *
 * Also computes the global bitrate if possible.
 */
static void update_stream_timings(AVFormatContext *ic)
{
    int64_t start_time, start_time1, start_time_text, end_time, end_time1, end_time_text;
    int64_t duration, duration1, duration_text, filesize;
    int i;
    AVProgram *p;

    start_time = INT64_MAX;
    start_time_text = INT64_MAX;
    end_time   = INT64_MIN;
    end_time_text   = INT64_MIN;
    duration   = INT64_MIN;
    duration_text = INT64_MIN;

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        int is_text = st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE ||
                      st->codecpar->codec_type == AVMEDIA_TYPE_DATA;
        if (st->start_time != AV_NOPTS_VALUE && st->time_base.den) {
            start_time1 = av_rescale_q(st->start_time, st->time_base,
                                       AV_TIME_BASE_Q);
            if (is_text)
                start_time_text = FFMIN(start_time_text, start_time1);
            else
                start_time = FFMIN(start_time, start_time1);
            end_time1 = av_rescale_q_rnd(st->duration, st->time_base,
                                         AV_TIME_BASE_Q,
                                         AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
            if (end_time1 != AV_NOPTS_VALUE && (end_time1 > 0 ? start_time1 <= INT64_MAX - end_time1 : start_time1 >= INT64_MIN - end_time1)) {
                end_time1 += start_time1;
                if (is_text)
                    end_time_text = FFMAX(end_time_text, end_time1);
                else
                    end_time = FFMAX(end_time, end_time1);
            }
            for (p = NULL; (p = av_find_program_from_stream(ic, p, i)); ) {
                if (p->start_time == AV_NOPTS_VALUE || p->start_time > start_time1)
                    p->start_time = start_time1;
                if (p->end_time < end_time1)
                    p->end_time = end_time1;
            }
        }
        if (st->duration != AV_NOPTS_VALUE) {
            duration1 = av_rescale_q(st->duration, st->time_base,
                                     AV_TIME_BASE_Q);
            if (is_text)
                duration_text = FFMAX(duration_text, duration1);
            else
                duration = FFMAX(duration, duration1);
        }
    }
    if (start_time == INT64_MAX || (start_time > start_time_text && start_time - (uint64_t)start_time_text < AV_TIME_BASE))
        start_time = start_time_text;
    else if (start_time > start_time_text)
        av_log(ic, AV_LOG_VERBOSE, "Ignoring outlier non primary stream starttime %f\n", start_time_text / (float)AV_TIME_BASE);

    if (end_time == INT64_MIN || (end_time < end_time_text && end_time_text - (uint64_t)end_time < AV_TIME_BASE))
        end_time = end_time_text;
    else if (end_time < end_time_text)
        av_log(ic, AV_LOG_VERBOSE, "Ignoring outlier non primary stream endtime %f\n", end_time_text / (float)AV_TIME_BASE);

     if (duration == INT64_MIN || (duration < duration_text && duration_text - duration < AV_TIME_BASE))
         duration = duration_text;
     else if (duration < duration_text)
         av_log(ic, AV_LOG_VERBOSE, "Ignoring outlier non primary stream duration %f\n", duration_text / (float)AV_TIME_BASE);

    if (start_time != INT64_MAX) {
        ic->start_time = start_time;
        if (end_time != INT64_MIN) {
            if (ic->nb_programs > 1) {
                for (i = 0; i < ic->nb_programs; i++) {
                    p = ic->programs[i];
                    if (p->start_time != AV_NOPTS_VALUE &&
                        p->end_time > p->start_time &&
                        p->end_time - (uint64_t)p->start_time <= INT64_MAX)
                        duration = FFMAX(duration, p->end_time - p->start_time);
                }
            } else if (end_time >= start_time && end_time - (uint64_t)start_time <= INT64_MAX) {
                duration = FFMAX(duration, end_time - start_time);
            }
        }
    }
    if (duration != INT64_MIN && duration > 0 && ic->duration == AV_NOPTS_VALUE) {
        ic->duration = duration;
    }
    if (ic->pb && (filesize = avio_size(ic->pb)) > 0 && ic->duration > 0) {
        /* compute the bitrate */
        double bitrate = (double) filesize * 8.0 * AV_TIME_BASE /
                         (double) ic->duration;
        if (bitrate >= 0 && bitrate <= INT64_MAX)
            ic->bit_rate = bitrate;
    }
}

static void fill_all_stream_timings(AVFormatContext *ic)
{
    int i;
    AVStream *st;

    update_stream_timings(ic);
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->start_time == AV_NOPTS_VALUE) {
            if (ic->start_time != AV_NOPTS_VALUE)
                st->start_time = av_rescale_q(ic->start_time, AV_TIME_BASE_Q,
                                              st->time_base);
            if (ic->duration != AV_NOPTS_VALUE)
                st->duration = av_rescale_q(ic->duration, AV_TIME_BASE_Q,
                                            st->time_base);
        }
    }
}

static void estimate_timings_from_bit_rate(AVFormatContext *ic)
{
    int64_t filesize, duration;
    int i, show_warning = 0;
    AVStream *st;

    /* if bit_rate is already set, we believe it */
    if (ic->bit_rate <= 0) {
        int64_t bit_rate = 0;
        for (i = 0; i < ic->nb_streams; i++) {
            st = ic->streams[i];
            if (st->codecpar->bit_rate <= 0 && st->internal->avctx->bit_rate > 0)
                st->codecpar->bit_rate = st->internal->avctx->bit_rate;
            if (st->codecpar->bit_rate > 0) {
                if (INT64_MAX - st->codecpar->bit_rate < bit_rate) {
                    bit_rate = 0;
                    break;
                }
                bit_rate += st->codecpar->bit_rate;
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && st->codec_info_nb_frames > 1) {
                // If we have a videostream with packets but without a bitrate
                // then consider the sum not known
                bit_rate = 0;
                break;
            }
        }
        ic->bit_rate = bit_rate;
    }

    /* if duration is already set, we believe it */
    if (ic->duration == AV_NOPTS_VALUE &&
        ic->bit_rate != 0) {
        filesize = ic->pb ? avio_size(ic->pb) : 0;
        if (filesize > ic->internal->data_offset) {
            filesize -= ic->internal->data_offset;
            for (i = 0; i < ic->nb_streams; i++) {
                st      = ic->streams[i];
                if (   st->time_base.num <= INT64_MAX / ic->bit_rate
                    && st->duration == AV_NOPTS_VALUE) {
                    duration = av_rescale(8 * filesize, st->time_base.den,
                                          ic->bit_rate *
                                          (int64_t) st->time_base.num);
                    st->duration = duration;
                    show_warning = 1;
                }
            }
        }
    }
    if (show_warning)
        av_log(ic, AV_LOG_WARNING,
               "Estimating duration from bitrate, this may be inaccurate\n");
}

#define DURATION_MAX_READ_SIZE 250000LL
#define DURATION_MAX_RETRY 6

/* only usable for MPEG-PS streams */
static void estimate_timings_from_pts(AVFormatContext *ic, int64_t old_offset)
{
    AVPacket pkt1, *pkt = &pkt1;
    AVStream *st;
    int num, den, read_size, i, ret;
    int found_duration = 0;
    int is_end;
    int64_t filesize, offset, duration;
    int retry = 0;

    /* flush packet queue */
    flush_packet_queue(ic);

    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->start_time == AV_NOPTS_VALUE &&
            st->first_dts == AV_NOPTS_VALUE &&
            st->codecpar->codec_type != AVMEDIA_TYPE_UNKNOWN)
            av_log(ic, AV_LOG_WARNING,
                   "start time for stream %d is not set in estimate_timings_from_pts\n", i);

        if (st->parser) {
            av_parser_close(st->parser);
            st->parser = NULL;
        }
    }

    if (ic->skip_estimate_duration_from_pts) {
        av_log(ic, AV_LOG_INFO, "Skipping duration calculation in estimate_timings_from_pts\n");
        goto skip_duration_calc;
    }

    av_opt_set(ic, "skip_changes", "1", AV_OPT_SEARCH_CHILDREN);
    /* estimate the end time (duration) */
    /* XXX: may need to support wrapping */
    filesize = ic->pb ? avio_size(ic->pb) : 0;
    do {
        is_end = found_duration;
        offset = filesize - (DURATION_MAX_READ_SIZE << retry);
        if (offset < 0)
            offset = 0;

        avio_seek(ic->pb, offset, SEEK_SET);
        read_size = 0;
        for (;;) {
            if (read_size >= DURATION_MAX_READ_SIZE << (FFMAX(retry - 1, 0)))
                break;

            do {
                ret = ff_read_packet(ic, pkt);
            } while (ret == AVERROR(EAGAIN));
            if (ret != 0)
                break;
            read_size += pkt->size;
            st         = ic->streams[pkt->stream_index];
            if (pkt->pts != AV_NOPTS_VALUE &&
                (st->start_time != AV_NOPTS_VALUE ||
                 st->first_dts  != AV_NOPTS_VALUE)) {
                if (pkt->duration == 0) {
                    ff_compute_frame_duration(ic, &num, &den, st, st->parser, pkt);
                    if (den && num) {
                        pkt->duration = av_rescale_rnd(1,
                                           num * (int64_t) st->time_base.den,
                                           den * (int64_t) st->time_base.num,
                                           AV_ROUND_DOWN);
                    }
                }
                duration = pkt->pts + pkt->duration;
                found_duration = 1;
                if (st->start_time != AV_NOPTS_VALUE)
                    duration -= st->start_time;
                else
                    duration -= st->first_dts;
                if (duration > 0) {
                    if (st->duration == AV_NOPTS_VALUE || st->info->last_duration<= 0 ||
                        (st->duration < duration && FFABS(duration - st->info->last_duration) < 60LL*st->time_base.den / st->time_base.num))
                        st->duration = duration;
                    st->info->last_duration = duration;
                }
            }
            av_packet_unref(pkt);
        }

        /* check if all audio/video streams have valid duration */
        if (!is_end) {
            is_end = 1;
            for (i = 0; i < ic->nb_streams; i++) {
                st = ic->streams[i];
                switch (st->codecpar->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                    case AVMEDIA_TYPE_AUDIO:
                        if (st->duration == AV_NOPTS_VALUE)
                            is_end = 0;
                }
            }
        }
    } while (!is_end &&
             offset &&
             ++retry <= DURATION_MAX_RETRY);

    av_opt_set(ic, "skip_changes", "0", AV_OPT_SEARCH_CHILDREN);

    /* warn about audio/video streams which duration could not be estimated */
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->duration == AV_NOPTS_VALUE) {
            switch (st->codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_AUDIO:
                if (st->start_time != AV_NOPTS_VALUE || st->first_dts  != AV_NOPTS_VALUE) {
                    av_log(ic, AV_LOG_DEBUG, "stream %d : no PTS found at end of file, duration not set\n", i);
                } else
                    av_log(ic, AV_LOG_DEBUG, "stream %d : no TS found at start of file, duration not set\n", i);
            }
        }
    }
skip_duration_calc:
    fill_all_stream_timings(ic);

    avio_seek(ic->pb, old_offset, SEEK_SET);
    for (i = 0; i < ic->nb_streams; i++) {
        int j;

        st              = ic->streams[i];
        st->cur_dts     = st->first_dts;
        st->last_IP_pts = AV_NOPTS_VALUE;
        st->last_dts_for_order_check = AV_NOPTS_VALUE;
        for (j = 0; j < MAX_REORDER_DELAY + 1; j++)
            st->pts_buffer[j] = AV_NOPTS_VALUE;
    }
}

static void estimate_timings(AVFormatContext *ic, int64_t old_offset)
{
    int64_t file_size;

    /* get the file size, if possible */
    if (ic->iformat->flags & AVFMT_NOFILE) {
        file_size = 0;
    } else {
        file_size = avio_size(ic->pb);
        file_size = FFMAX(0, file_size);
    }

    if ((!strcmp(ic->iformat->name, "mpeg") ||
         !strcmp(ic->iformat->name, "mpegts")) &&
        file_size && (ic->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        /* get accurate estimate from the PTSes */
        estimate_timings_from_pts(ic, old_offset);
        ic->duration_estimation_method = AVFMT_DURATION_FROM_PTS;
    } else if (has_duration(ic)) {
        /* at least one component has timings - we use them for all
         * the components */
        fill_all_stream_timings(ic);
        ic->duration_estimation_method = AVFMT_DURATION_FROM_STREAM;
    } else {
        /* less precise: use bitrate info */
        estimate_timings_from_bit_rate(ic);
        ic->duration_estimation_method = AVFMT_DURATION_FROM_BITRATE;
    }
    update_stream_timings(ic);

    {
        int i;
        AVStream av_unused *st;
        for (i = 0; i < ic->nb_streams; i++) {
            st = ic->streams[i];
            if (st->time_base.den)
            av_log(ic, AV_LOG_TRACE, "stream %d: start_time: %0.3f duration: %0.3f\n", i,
                   (double) st->start_time * av_q2d(st->time_base),
                   (double) st->duration   * av_q2d(st->time_base));
        }
        av_log(ic, AV_LOG_TRACE,
                "format: start_time: %0.3f duration: %0.3f bitrate=%"PRId64" kb/s\n",
                (double) ic->start_time / AV_TIME_BASE,
                (double) ic->duration   / AV_TIME_BASE,
                (int64_t)ic->bit_rate / 1000);
    }
}
//返回0 是异常
static int has_codec_parameters(AVStream *st, const char **errmsg_ptr)//AVStream是否有足够的音频或视频参数
{
    AVCodecContext *avctx = st->internal->avctx;

#define FAIL(errmsg) do {                                         \
        if (errmsg_ptr)                                           \
            *errmsg_ptr = errmsg;                                 \
        return 0;                                                 \
    } while (0)

    if (   avctx->codec_id == AV_CODEC_ID_NONE
        && avctx->codec_type != AVMEDIA_TYPE_DATA)
        FAIL("unknown codec");
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO://音频必须要包含
        if (!avctx->frame_size && determinable_frame_size(avctx))
            FAIL("unspecified frame size");//某些音频必须制定frame size
        if (st->info->found_decoder >= 0 &&
            avctx->sample_fmt == AV_SAMPLE_FMT_NONE)//01.采样编码类型
            FAIL("unspecified sample format");//ALAW 文件从这里返回
        if (!avctx->sample_rate)//02.采样率
            FAIL("unspecified sample rate");
        if (!avctx->channels)//03.声道数
            FAIL("unspecified number of channels");
        if (st->info->found_decoder >= 0 && !st->nb_decoded_frames && avctx->codec_id == AV_CODEC_ID_DTS)
            FAIL("no decodable DTS frames");//特例:
        break;
    case AVMEDIA_TYPE_VIDEO:
        if (!avctx->width)//01. 宽
            FAIL("unspecified size");
        if (st->info->found_decoder >= 0 && avctx->pix_fmt == AV_PIX_FMT_NONE)
            FAIL("unspecified pixel format");//02.像素格式
        if (st->codecpar->codec_id == AV_CODEC_ID_RV30 || st->codecpar->codec_id == AV_CODEC_ID_RV40)
            if (!st->sample_aspect_ratio.num && !st->codecpar->sample_aspect_ratio.num && !st->codec_info_nb_frames)
                FAIL("no frame in rv30/40 and no sar");//特例：
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (avctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE && !avctx->width)
            FAIL("unspecified size");//01.字幕的宽带
        break;
    case AVMEDIA_TYPE_DATA:
        if (avctx->codec_id == AV_CODEC_ID_NONE) return 1;
    }

    return 1;
}

/* returns 1 or 0 if or if not decoded data was returned, or a negative error */
static int try_decode_frame(AVFormatContext *s, AVStream *st, AVPacket *avpkt,//tiger try_decode_frame 未打开解码器，则打开，对于alaw,AAC和H264仅此而已
                            AVDictionary **options)//尝试解开一帧
{
    AVCodecContext *avctx = st->internal->avctx;
    const AVCodec *codec;
    int got_picture = 1, ret = 0;
    AVFrame *frame = av_frame_alloc();
    AVSubtitle subtitle;
    AVPacket pkt = *avpkt;
    int do_skip_frame = 0;
    enum AVDiscard skip_frame;
    //01.校验
    if (!frame)
        return AVERROR(ENOMEM);
    //02. 打开decoder，初始化decoder
    if (!avcodec_is_open(avctx) &&//未打开
        st->info->found_decoder <= 0 &&
        (st->codecpar->codec_id != -st->info->found_decoder || !st->codecpar->codec_id)) {
        AVDictionary *thread_opt = NULL;
        //打开解码器
        codec = find_probe_decoder(s, st, st->codecpar->codec_id);

        if (!codec) {
            st->info->found_decoder = -st->codecpar->codec_id;//为负数
            ret                     = -1;
            goto fail;
        }

        /* Force thread count to 1 since the H.264 decoder will not extract
         * SPS and PPS to extradata during multi-threaded decoding. */
        av_dict_set(options ? options : &thread_opt, "threads", "1", 0);
        if (s->codec_whitelist)
            av_dict_set(options ? options : &thread_opt, "codec_whitelist", s->codec_whitelist, 0);
        ret = avcodec_open2(avctx, codec, options ? options : &thread_opt);//打开并初始化解码器
        if (!options)
            av_dict_free(&thread_opt);
        if (ret < 0) {
            st->info->found_decoder = -avctx->codec_id;//设置为负数？
            goto fail;
        }
        st->info->found_decoder = 1;
    } else if (!st->info->found_decoder)
        st->info->found_decoder = 1;
    //03.
    if (st->info->found_decoder < 0) {
        ret = -1;
        goto fail;
    }
    //04. 这个函数对于alaw,AAC和H264,都在这里直接返回
    if (avpriv_codec_get_cap_skip_frame_fill_param(avctx->codec)) {//TIGER ALAW 从这里返回 因为ALAW和h264都没有FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM 只有h263dec,flvdec,
        do_skip_frame = 1;
        skip_frame = avctx->skip_frame;
        avctx->skip_frame = AVDISCARD_ALL;
    }
    //05.编码解码 举例：alaw不会进入这个循环
    while ((pkt.size > 0 || (!pkt.data && got_picture)) &&
           ret >= 0 &&
           (!has_codec_parameters(st, NULL) || !has_decode_delay_been_guessed(st) ||//举例：对于alaw has_codec_parameters(st, NULL) =1 ，has_decode_delay_been_guessed(st)=1
            (!st->codec_info_nb_frames &&
             (avctx->codec->capabilities & AV_CODEC_CAP_CHANNEL_CONF)))) {//如果是要替代容器的配置的
        got_picture = 0;
        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
            avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            ret = avcodec_send_packet(avctx, &pkt);//05.01.
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                break;
            if (ret >= 0)
                pkt.size = 0;
            ret = avcodec_receive_frame(avctx, frame);//05.02.解出一帧
            if (ret >= 0)
                got_picture = 1;//解出一帧
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
        } else if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {//字幕
            ret = avcodec_decode_subtitle2(avctx, &subtitle,
                                           &got_picture, &pkt);
            if (ret >= 0)
                pkt.size = 0;
        }
        if (ret >= 0) {
            if (got_picture)
                st->nb_decoded_frames++;
            ret       = got_picture;
        }
    }
    //06.
    if (!pkt.data && !got_picture)
        ret = -1;

fail:
    if (do_skip_frame) {
        avctx->skip_frame = skip_frame;
    }

    av_frame_free(&frame);
    return ret;
}

unsigned int ff_codec_get_tag(const AVCodecTag *tags, enum AVCodecID id)
{
    while (tags->id != AV_CODEC_ID_NONE) {
        if (tags->id == id)
            return tags->tag;
        tags++;
    }
    return 0;
}

enum AVCodecID ff_codec_get_id(const AVCodecTag *tags, unsigned int tag)
{
    int i;
    for (i = 0; tags[i].id != AV_CODEC_ID_NONE; i++)
        if (tag == tags[i].tag)
            return tags[i].id;
    for (i = 0; tags[i].id != AV_CODEC_ID_NONE; i++)
        if (avpriv_toupper4(tag) == avpriv_toupper4(tags[i].tag))
            return tags[i].id;
    return AV_CODEC_ID_NONE;
}

enum AVCodecID ff_get_pcm_codec_id(int bps, int flt, int be, int sflags)
{
    if (bps <= 0 || bps > 64)
        return AV_CODEC_ID_NONE;

    if (flt) {
        switch (bps) {
        case 32:
            return be ? AV_CODEC_ID_PCM_F32BE : AV_CODEC_ID_PCM_F32LE;
        case 64:
            return be ? AV_CODEC_ID_PCM_F64BE : AV_CODEC_ID_PCM_F64LE;
        default:
            return AV_CODEC_ID_NONE;
        }
    } else {
        bps  += 7;
        bps >>= 3;
        if (sflags & (1 << (bps - 1))) {
            switch (bps) {
            case 1:
                return AV_CODEC_ID_PCM_S8;
            case 2:
                return be ? AV_CODEC_ID_PCM_S16BE : AV_CODEC_ID_PCM_S16LE;
            case 3:
                return be ? AV_CODEC_ID_PCM_S24BE : AV_CODEC_ID_PCM_S24LE;
            case 4:
                return be ? AV_CODEC_ID_PCM_S32BE : AV_CODEC_ID_PCM_S32LE;
            case 8:
                return be ? AV_CODEC_ID_PCM_S64BE : AV_CODEC_ID_PCM_S64LE;
            default:
                return AV_CODEC_ID_NONE;
            }
        } else {
            switch (bps) {
            case 1:
                return AV_CODEC_ID_PCM_U8;
            case 2:
                return be ? AV_CODEC_ID_PCM_U16BE : AV_CODEC_ID_PCM_U16LE;
            case 3:
                return be ? AV_CODEC_ID_PCM_U24BE : AV_CODEC_ID_PCM_U24LE;
            case 4:
                return be ? AV_CODEC_ID_PCM_U32BE : AV_CODEC_ID_PCM_U32LE;
            default:
                return AV_CODEC_ID_NONE;
            }
        }
    }
}

unsigned int av_codec_get_tag(const AVCodecTag *const *tags, enum AVCodecID id)
{
    unsigned int tag;
    if (!av_codec_get_tag2(tags, id, &tag))
        return 0;
    return tag;
}

int av_codec_get_tag2(const AVCodecTag * const *tags, enum AVCodecID id,
                      unsigned int *tag)
{
    int i;
    for (i = 0; tags && tags[i]; i++) {
        const AVCodecTag *codec_tags = tags[i];
        while (codec_tags->id != AV_CODEC_ID_NONE) {
            if (codec_tags->id == id) {
                *tag = codec_tags->tag;
                return 1;
            }
            codec_tags++;
        }
    }
    return 0;
}

enum AVCodecID av_codec_get_id(const AVCodecTag *const *tags, unsigned int tag)//通过tag名称来获取id
{
    int i;
    for (i = 0; tags && tags[i]; i++) {
        enum AVCodecID id = ff_codec_get_id(tags[i], tag);
        if (id != AV_CODEC_ID_NONE)
            return id;
    }
    return AV_CODEC_ID_NONE;
}

static void compute_chapters_end(AVFormatContext *s)
{
    unsigned int i, j;
    int64_t max_time = 0;

    if (s->duration > 0 && s->start_time < INT64_MAX - s->duration)//
        max_time = s->duration +
                       ((s->start_time == AV_NOPTS_VALUE) ? 0 : s->start_time);

    for (i = 0; i < s->nb_chapters; i++)
        if (s->chapters[i]->end == AV_NOPTS_VALUE) {//如果没填，填写所有chapter里面最大的时间？
            AVChapter *ch = s->chapters[i];
            int64_t end = max_time ? av_rescale_q(max_time, AV_TIME_BASE_Q,//用ch->time_base折算，每个chapter可能都不一样
                                                  ch->time_base)
                                   : INT64_MAX;

            for (j = 0; j < s->nb_chapters; j++) {
                AVChapter *ch1     = s->chapters[j];
                int64_t next_start = av_rescale_q(ch1->start, ch1->time_base,//折算下一个开始的时间
                                                  ch->time_base);
                if (j != i && next_start > ch->start && next_start < end)
                    end = next_start;
            }
            ch->end = (end == INT64_MAX || end < ch->start) ? ch->start : end;//填写所有chapter里面最大的时间？
        }
}

static int get_std_framerate(int i)
{
    if (i < 30*12)
        return (i + 1) * 1001;
    i -= 30*12;

    if (i < 30)
        return (i + 31) * 1001 * 12;
    i -= 30;

    if (i < 3)
        return ((const int[]) { 80, 120, 240})[i] * 1001 * 12;

    i -= 3;

    return ((const int[]) { 24, 30, 60, 12, 15, 48 })[i] * 1000 * 12;
}

/* Is the time base unreliable?
 * This is a heuristic to balance between quick acceptance of the values in
 * the headers vs. some extra checks.
 * Old DivX and Xvid often have nonsense timebases like 1fps or 2fps.
 * MPEG-2 commonly misuses field repeat flags to store different framerates.
 * And there are "variable" fps files this needs to detect as well. */
static int tb_unreliable(AVCodecContext *c)
{
    if (c->time_base.den >= 101LL * c->time_base.num ||
        c->time_base.den <    5LL * c->time_base.num ||
        // c->codec_tag == AV_RL32("DIVX") ||
        // c->codec_tag == AV_RL32("XVID") ||
        c->codec_tag == AV_RL32("mp4v") ||
        c->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
        c->codec_id == AV_CODEC_ID_GIF ||
        c->codec_id == AV_CODEC_ID_HEVC ||
        c->codec_id == AV_CODEC_ID_H264)
        return 1;
    return 0;
}

int ff_alloc_extradata(AVCodecParameters *par, int size)
{
    av_freep(&par->extradata);
    par->extradata_size = 0;

    if (size < 0 || size >= INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(EINVAL);

    par->extradata = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata)
        return AVERROR(ENOMEM);

    memset(par->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = size;

    return 0;
}

int ff_get_extradata(AVFormatContext *s, AVCodecParameters *par, AVIOContext *pb, int size)
{
    int ret = ff_alloc_extradata(par, size);
    if (ret < 0)
        return ret;
    ret = avio_read(pb, par->extradata, size);
    if (ret != size) {
        av_freep(&par->extradata);
        par->extradata_size = 0;
        av_log(s, AV_LOG_ERROR, "Failed to read extradata of size %d\n", size);
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    }

    return ret;
}

int ff_rfps_add_frame(AVFormatContext *ic, AVStream *st, int64_t ts)
{
    int i, j;
    int64_t last = st->info->last_dts;

    if (   ts != AV_NOPTS_VALUE && last != AV_NOPTS_VALUE && ts > last
       && ts - (uint64_t)last < INT64_MAX) {
        double dts = (is_relative(ts) ?  ts - RELATIVE_TS_BASE : ts) * av_q2d(st->time_base);
        int64_t duration = ts - last;

        if (!st->info->duration_error)
            st->info->duration_error = av_mallocz(sizeof(st->info->duration_error[0])*2);
        if (!st->info->duration_error)
            return AVERROR(ENOMEM);

//         if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
//             av_log(NULL, AV_LOG_ERROR, "%f\n", dts);
        for (i = 0; i<MAX_STD_TIMEBASES; i++) {
            if (st->info->duration_error[0][1][i] < 1e10) {
                int framerate = get_std_framerate(i);
                double sdts = dts*framerate/(1001*12);
                for (j= 0; j<2; j++) {
                    int64_t ticks = llrint(sdts+j*0.5);
                    double error= sdts - ticks + j*0.5;
                    st->info->duration_error[j][0][i] += error;
                    st->info->duration_error[j][1][i] += error*error;
                }
            }
        }
        if (st->info->rfps_duration_sum <= INT64_MAX - duration) {
            st->info->duration_count++;
            st->info->rfps_duration_sum += duration;
        }

        if (st->info->duration_count % 10 == 0) {
            int n = st->info->duration_count;
            for (i = 0; i<MAX_STD_TIMEBASES; i++) {
                if (st->info->duration_error[0][1][i] < 1e10) {
                    double a0     = st->info->duration_error[0][0][i] / n;
                    double error0 = st->info->duration_error[0][1][i] / n - a0*a0;
                    double a1     = st->info->duration_error[1][0][i] / n;
                    double error1 = st->info->duration_error[1][1][i] / n - a1*a1;
                    if (error0 > 0.04 && error1 > 0.04) {
                        st->info->duration_error[0][1][i] = 2e10;
                        st->info->duration_error[1][1][i] = 2e10;
                    }
                }
            }
        }

        // ignore the first 4 values, they might have some random jitter
        if (st->info->duration_count > 3 && is_relative(ts) == is_relative(last))
            st->info->duration_gcd = av_gcd(st->info->duration_gcd, duration);
    }
    if (ts != AV_NOPTS_VALUE)
        st->info->last_dts = ts;

    return 0;
}
//tiger 简写rf:Real base framerate 
void ff_rfps_calculate(AVFormatContext *ic)
{
    int i, j;

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];

        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        // the check for tb_unreliable() is not completely correct, since this is not about handling
        // an unreliable/inexact time base, but a time base that is finer than necessary, as e.g.
        // ipmovie.c produces.
        if (tb_unreliable(st->internal->avctx) && st->info->duration_count > 15 && st->info->duration_gcd > FFMAX(1, st->time_base.den/(500LL*st->time_base.num)) && !st->r_frame_rate.num)
            av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den, st->time_base.den, st->time_base.num * st->info->duration_gcd, INT_MAX);
        if (st->info->duration_count>1 && !st->r_frame_rate.num
            && tb_unreliable(st->internal->avctx)) {
            int num = 0;
            double best_error= 0.01;
            AVRational ref_rate = st->r_frame_rate.num ? st->r_frame_rate : av_inv_q(st->time_base);

            for (j= 0; j<MAX_STD_TIMEBASES; j++) {
                int k;

                if (st->info->codec_info_duration &&
                    st->info->codec_info_duration*av_q2d(st->time_base) < (1001*11.5)/get_std_framerate(j))
                    continue;
                if (!st->info->codec_info_duration && get_std_framerate(j) < 1001*12)
                    continue;

                if (av_q2d(st->time_base) * st->info->rfps_duration_sum / st->info->duration_count < (1001*12.0 * 0.8)/get_std_framerate(j))
                    continue;

                for (k= 0; k<2; k++) {
                    int n = st->info->duration_count;
                    double a= st->info->duration_error[k][0][j] / n;
                    double error= st->info->duration_error[k][1][j]/n - a*a;

                    if (error < best_error && best_error> 0.000000001) {
                        best_error= error;
                        num = get_std_framerate(j);
                    }
                    if (error < 0.02)
                        av_log(ic, AV_LOG_DEBUG, "rfps: %f %f\n", get_std_framerate(j) / 12.0/1001, error);
                }
            }
            // do not increase frame rate by more than 1 % in order to match a standard rate.
            if (num && (!ref_rate.num || (double)num/(12*1001) < 1.01 * av_q2d(ref_rate)))
                av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den, num, 12*1001, INT_MAX);
        }
        if (   !st->avg_frame_rate.num
            && st->r_frame_rate.num && st->info->rfps_duration_sum
            && st->info->codec_info_duration <= 0
            && st->info->duration_count > 2
            && fabs(1.0 / (av_q2d(st->r_frame_rate) * av_q2d(st->time_base)) - st->info->rfps_duration_sum / (double)st->info->duration_count) <= 1.0
            ) {
            av_log(ic, AV_LOG_DEBUG, "Setting avg frame rate based on r frame rate\n");
            st->avg_frame_rate = st->r_frame_rate;
        }

        av_freep(&st->info->duration_error);
        st->info->last_dts = AV_NOPTS_VALUE;
        st->info->duration_count = 0;
        st->info->rfps_duration_sum = 0;
    }
}

static int extract_extradata_check(AVStream *st)
{
    const AVBitStreamFilter *f;

    f = av_bsf_get_by_name("extract_extradata");
    if (!f)
        return 0;

    if (f->codec_ids) {
        const enum AVCodecID *ids;
        for (ids = f->codec_ids; *ids != AV_CODEC_ID_NONE; ids++)
            if (*ids == st->codecpar->codec_id)
                return 1;
    }

    return 0;
}

static int extract_extradata_init(AVStream *st)
{
    AVStreamInternal *sti = st->internal;
    const AVBitStreamFilter *f;
    int ret;

    f = av_bsf_get_by_name("extract_extradata");
    if (!f)
        goto finish;

    /* check that the codec id is supported */
    ret = extract_extradata_check(st);
    if (!ret)
        goto finish;

    sti->extract_extradata.pkt = av_packet_alloc();
    if (!sti->extract_extradata.pkt)
        return AVERROR(ENOMEM);

    ret = av_bsf_alloc(f, &sti->extract_extradata.bsf);
    if (ret < 0)
        goto fail;

    ret = avcodec_parameters_copy(sti->extract_extradata.bsf->par_in,
                                  st->codecpar);
    if (ret < 0)
        goto fail;

    sti->extract_extradata.bsf->time_base_in = st->time_base;

    ret = av_bsf_init(sti->extract_extradata.bsf);
    if (ret < 0)
        goto fail;

finish:
    sti->extract_extradata.inited = 1;

    return 0;
fail:
    av_bsf_free(&sti->extract_extradata.bsf);
    av_packet_free(&sti->extract_extradata.pkt);
    return ret;
}

static int extract_extradata(AVStream *st, AVPacket *pkt)//TIGER extract_extradata
{
    AVStreamInternal *sti = st->internal;
    AVPacket *pkt_ref;
    int ret;
    //01.
    if (!sti->extract_extradata.inited) {
        ret = extract_extradata_init(st);
        if (ret < 0)
            return ret;
    }
    //02.
    if (sti->extract_extradata.inited && !sti->extract_extradata.bsf)
        return 0;
    //03.
    pkt_ref = sti->extract_extradata.pkt;
    ret = av_packet_ref(pkt_ref, pkt);
    if (ret < 0)
        return ret;
    //04.
    ret = av_bsf_send_packet(sti->extract_extradata.bsf, pkt_ref);
    if (ret < 0) {
        av_packet_unref(pkt_ref);
        return ret;
    }
    //05.
    while (ret >= 0 && !sti->avctx->extradata) {//
        int extradata_size;
        uint8_t *extradata;
        //05.01
        ret = av_bsf_receive_packet(sti->extract_extradata.bsf, pkt_ref);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                return ret;
            continue;
        }
        //05.02
        extradata = av_packet_get_side_data(pkt_ref, AV_PKT_DATA_NEW_EXTRADATA,
                                            &extradata_size);
        //05.03
        if (extradata) {
            av_assert0(!sti->avctx->extradata);
            if ((unsigned)extradata_size < FF_MAX_EXTRADATA_SIZE)
                sti->avctx->extradata = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!sti->avctx->extradata) {
                av_packet_unref(pkt_ref);
                return AVERROR(ENOMEM);
            }
            memcpy(sti->avctx->extradata, extradata, extradata_size);
            sti->avctx->extradata_size = extradata_size;
        }
        av_packet_unref(pkt_ref);
    }

    return 0;
}
//tiger 重要，功能看.h中的注释。理解各个函数的使用，options 是每个stream 一个option
int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options)//看一下注释 On return each dictionary will be filled with options that were not found.
{
    int i, count = 0, ret = 0, j;
    int64_t read_size;
    AVStream *st;//流
    AVCodecContext *avctx;//指向内部的结构体st->internal->avctx
    AVPacket pkt1, *pkt;//包
    int64_t old_offset  = avio_tell(ic->pb);//自定义流的位置
    // new streams might appear, no options for those
    int orig_nb_streams = ic->nb_streams;//原始的流个数
    int flush_codecs;//probe需要不停分析解碼器
    int64_t max_analyze_duration = ic->max_analyze_duration;//最大分析时长
    int64_t max_stream_analyze_duration;//最大流分析时长
    int64_t max_subtitle_analyze_duration;//最大字幕分析时长
    int64_t probesize = ic->probesize;//分析的字节数
    int eof_reached = 0;//是否到末尾
    int *missing_streams = av_opt_ptr(ic->iformat->priv_class, ic->priv_data, "missing_streams");//特殊选项
    //01.参数初始化
    flush_codecs = probesize > 0;
    //增加一个选项
    av_opt_set(ic, "skip_clear", "1", AV_OPT_SEARCH_CHILDREN);
    //设置默认的一些时间
    max_stream_analyze_duration = max_analyze_duration;//最大的探测时间
    max_subtitle_analyze_duration = max_analyze_duration;
    if (!max_analyze_duration) {//如果用户没有设置时间，就写默认值
        max_stream_analyze_duration =
        max_analyze_duration        = 5*AV_TIME_BASE;//5秒
        max_subtitle_analyze_duration = 30*AV_TIME_BASE;//30秒,比较长，还好目前一般没有字幕，
        if (!strcmp(ic->iformat->name, "flv"))
            max_stream_analyze_duration = 90*AV_TIME_BASE;//FLV 90秒，有点太长.....
        if (!strcmp(ic->iformat->name, "mpeg") || !strcmp(ic->iformat->name, "mpegts"))
            max_stream_analyze_duration = 7*AV_TIME_BASE;//mpeg 7秒？==>为什么
    }
    //如果是自定义IO，则打印当前位置
    if (ic->pb)
        av_log(ic, AV_LOG_DEBUG, "Before avformat_find_stream_info() pos: %"PRId64" bytes read:%"PRId64" seeks:%d nb_streams:%d\n",
               avio_tell(ic->pb), ic->pb->bytes_read, ic->pb->seek_count, ic->nb_streams);
    //02.每个流:找到parser，复制avcodec_parameters_to_context，find_probe_decoder，打开输入流
    for (i = 0; i < ic->nb_streams; i++) {//逐一执行每个流
        const AVCodec *codec;
        AVDictionary *thread_opt = NULL;
        st = ic->streams[i];
        avctx = st->internal->avctx;//02.01 取内部的AVCodecContext地址，每个流私有空间，避免线程冲突

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {//
/*            if (!st->time_base.num)
                st->time_base = */
            if (!avctx->time_base.num)//02.02 如果视频和字幕流的AVCodecContext没有设置time_base，采用流的设定值--> 不理解是什么特例
                avctx->time_base = st->time_base;
        }

        /* check if the caller has overridden the codec id */
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        if (st->codec->codec_id != st->internal->orig_codec_id) {//02.03是否以前运行过find_stream_info？ st->internal->orig_codec_id有遗留，用现在的覆盖
            st->codecpar->codec_id   = st->codec->codec_id;
            st->codecpar->codec_type = st->codec->codec_type;
            st->internal->orig_codec_id = st->codec->codec_id;
        }
FF_ENABLE_DEPRECATION_WARNINGS
#endif  //重要步骤01.parser 
        // only for the split stuff
        if (!st->parser && !(ic->flags & AVFMT_FLAG_NOPARSE) && st->request_probe <= 0) {//02.04如果：没设置parse的flag，也没有parser，probe已结束或者不需要
            st->parser = av_parser_init(st->codecpar->codec_id);//parser_list中configure定义 ff_aac_parser(调用ff_aac_ac3_parse) ff_aac_latm_parse ff_h264_parser  但pcm，sdp 什么都没有
            if (st->parser) {//找到编码器，设置flag
                if (st->need_parsing == AVSTREAM_PARSE_HEADERS) {
                    st->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;//？
                } else if (st->need_parsing == AVSTREAM_PARSE_FULL_RAW) {
                    st->parser->flags |= PARSER_FLAG_USE_CODEC_TS;//？
                }
            } else if (st->need_parsing) {//如果需要解析，但找不到编码器
                av_log(ic, AV_LOG_VERBOSE, "parser not found for codec "
                       "%s, packets or times may be invalid.\n",
                       avcodec_get_name(st->codecpar->codec_id));//找不到解码器。 如果是sdp文件，那这里应该是sdp文件解析过了。
            }
        }

        if (st->codecpar->codec_id != st->internal->orig_codec_id)//如果是st->codecpar->codec_id 不一样，那codec_tag 之类能保持一致吗？==>TIGER todo:
            st->internal->orig_codec_id = st->codecpar->codec_id;//02.05 更新，前面3632行已经判断，这里相当于设置了标志位，已更改
        //重要步骤01. 复制AVCodecParameters信息(音视频三要素等)
        ret = avcodec_parameters_to_context(avctx, st->codecpar);//02.06 将AVCodecParameters解码器的信息更新到内部find_stream_info的AVCodecContext context里
        if (ret < 0)
            goto find_stream_info_err;
        if (st->request_probe <= 0)//02.07 这个何解==>已经probe，或者probe不需要
            st->internal->avctx_inited = 1;//已经初始化
        //重要步骤03. 解码器
        codec = find_probe_decoder(ic, st, st->codecpar->codec_id);//02.08 这个何解==>已经probe，或者probe不需要查找probe的解码器，如ff_pcm_alaw_decoder

        /* Force thread count to 1 since the H.264 decoder will not extract
         * SPS and PPS to extradata during multi-threaded decoding. */
        av_dict_set(options ? &options[i] : &thread_opt, "threads", "1", 0);//避免多线程时候，SPS，PPS落后解析，导致某些帧解析错误。

        if (ic->codec_whitelist)//02.09 白名单
            av_dict_set(options ? &options[i] : &thread_opt, "codec_whitelist", ic->codec_whitelist, 0);
        
        /* Ensure that subtitle_header is properly set. */
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE
            && codec && !avctx->codec) {//02.10字幕为什么要这么做？
            if (avcodec_open2(avctx, codec, options ? &options[i] : &thread_opt) < 0)
                av_log(ic, AV_LOG_WARNING,
                       "Failed to open codec in %s\n",__FUNCTION__);
        }
        //重要步骤04. 解码器上下文
        // Try to just open decoders, in case this is enough to get parameters.
        if (!has_codec_parameters(st, NULL) && st->request_probe <= 0) {//02.11 是否包含足够的参数 如视频需要足够的长和宽，音频一般需要采样率，声道数等
            if (codec && !avctx->codec)//如果有解码器，且上下文未打开这个解码
                if (avcodec_open2(avctx, codec, options ? &options[i] : &thread_opt) < 0)//这里相当于find_stream_info的初始化
                    av_log(ic, AV_LOG_WARNING,
                           "Failed to open codec in %s\n",__FUNCTION__);
        }
        if (!options)//02.12如果没有options，也不需要thread_opt
            av_dict_free(&thread_opt);
    }
    //03. dts 初始化
    for (i = 0; i < ic->nb_streams; i++) {//初始化所有流的dts ==>
#if FF_API_R_FRAME_RATE
        ic->streams[i]->info->last_dts = AV_NOPTS_VALUE;
#endif
        ic->streams[i]->info->fps_first_dts = AV_NOPTS_VALUE;
        ic->streams[i]->info->fps_last_dts  = AV_NOPTS_VALUE;
    }
    //04.
    read_size = 0;
    for (;;) {
        int analyzed_all_streams;
        if (ff_check_interrupt(&ic->interrupt_callback)) {//04.01 tiger program 是否可以从这里强行退出
            ret = AVERROR_EXIT;
            av_log(ic, AV_LOG_DEBUG, "interrupted\n");
            break;
        }
        
        /* check if one codec still needs to be handled */
        for (i = 0; i < ic->nb_streams; i++) {//04.02几个stream 轮流执行
            int fps_analyze_framecount = 20;//默认每秒20帧
            int count;

            st = ic->streams[i];
            if (!has_codec_parameters(st, NULL))//01. 编解码参数不对 但avctx->width不是find出来的，怎么在这里判断？是找到了复制到这里吗？==>再次判断吧
                break;//返回0，是异常
            /* If the timebase is coarse (like the usual millisecond precision
             * of mkv), we need to analyze more frames to reliably arrive at
             * the correct fps. */
            if (av_q2d(st->time_base) > 0.0005)//02.毫秒级的时间，怕你不准
                fps_analyze_framecount *= 2;
            if (!tb_unreliable(st->internal->avctx))//考虑很多别的编码器不准
                fps_analyze_framecount = 0;
            if (ic->fps_probe_size >= 0)//既然参数已经定义了，重设
                fps_analyze_framecount = ic->fps_probe_size;
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)//又是特例
                fps_analyze_framecount = 0;
            /* variable fps and no guess at the real fps */
            count = (ic->iformat->flags & AVFMT_NOTIMESTAMPS) ?//特例：没有需要时间戳的情况  
                       st->info->codec_info_duration_fields/2 :
                       st->info->duration_count;
            if (!(st->r_frame_rate.num && st->avg_frame_rate.num) &&
                st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (count < fps_analyze_framecount)//举例：alaw:count = 0,fps_analyze_framecount=20
                    break;//时间已经到了
            }
            // Look at the first 3 frames if there is evidence of frame delay
            // but the decoder delay is not set.
            if (st->info->frame_delay_evidence && count < 2 && st->internal->avctx->has_b_frames == 0)
                break;
            if (!st->internal->avctx->extradata &&
                (!st->internal->extract_extradata.inited ||
                 st->internal->extract_extradata.bsf) &&
                extract_extradata_check(st))//?
                break;
            if (st->first_dts == AV_NOPTS_VALUE &&//特例?
                !(ic->iformat->flags & AVFMT_NOTIMESTAMPS) &&
                st->codec_info_nb_frames < ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) ? 1 : ic->max_ts_probe) &&
                (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
                 st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO))
                break;
        }
        analyzed_all_streams = 0;
        if (!missing_streams || !*missing_streams)
        if (i == ic->nb_streams) {//04.03?
            analyzed_all_streams = 1;
            /* NOTE: If the format has no header, then we need to read some
             * packets to get most of the streams, so we cannot stop here. */
            if (!(ic->ctx_flags & AVFMTCTX_NOHEADER)) {
                /* If we found the info for all the codecs, we can stop. */
                ret = count;
                av_log(ic, AV_LOG_DEBUG, "All info found\n");
                flush_codecs = 0;
                break;
            }
        }
        /* We did not get all the codec info, but we read too much data. */
        if (read_size >= probesize) {//04.04 字节数超了
            ret = count;
            av_log(ic, AV_LOG_DEBUG,
                   "Probe buffer size limit of %"PRId64" bytes reached\n", probesize);
            for (i = 0; i < ic->nb_streams; i++)
                if (!ic->streams[i]->r_frame_rate.num &&
                    ic->streams[i]->info->duration_count <= 1 &&
                    ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    strcmp(ic->iformat->name, "image2"))
                    av_log(ic, AV_LOG_WARNING,
                           "Stream #%d: not enough frames to estimate rate; "
                           "consider increasing probesize\n", i);
            break;
        }

        /* NOTE: A new stream can be added there if no header in file
         * (AVFMTCTX_NOHEADER). */
        ret = read_frame_internal(ic, &pkt1);//04.05 主函数读 av_read_frame-->read_frame_internal -->ff_read_packet -->ff_rtsp_fetch_packet -->ff_rtp_parse_packet -->rtp_parse_one_packet -->read_frame_internal -->ff_read_packet -->ff_rtsp_fetch_packet -->ff_rtp_parse_packet -->rtp_parse_one_packet -->rtp_parse_packet_internal-->h264_handle_packet
        if (ret == AVERROR(EAGAIN))//主函数 av_read_frame-->read_frame_internal-->parse_packet-->av_parser_parse2-->h264_parse
            continue;

        if (ret < 0) {//04.06末尾了
            /* EOF or error*/
            eof_reached = 1;
            break;
        }
        
        pkt = &pkt1;
        //04.07 放入队列，以便重新读取
        if (!(ic->flags & AVFMT_FLAG_NOBUFFER)) {
            ret = ff_packet_list_put(&ic->internal->packet_buffer,//
                                     &ic->internal->packet_buffer_end,
                                     pkt, 0);
            if (ret < 0)
                goto find_stream_info_err;
        }
        //04.08 统计值read_size 增加
        st = ic->streams[pkt->stream_index];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            read_size += pkt->size;
        //04.09 更新内部AVCodecContext
        avctx = st->internal->avctx;
        if (!st->internal->avctx_inited) {
            ret = avcodec_parameters_to_context(avctx, st->codecpar);
            if (ret < 0)
                goto find_stream_info_err;
            st->internal->avctx_inited = 1;
        }
        //04.10
        if (pkt->dts != AV_NOPTS_VALUE && st->codec_info_nb_frames > 1) {//举例 alaw out.al ：st->codec_info_nb_frames = 0
            /* check for non-increasing dts */
            if (st->info->fps_last_dts != AV_NOPTS_VALUE &&
                st->info->fps_last_dts >= pkt->dts) {
                av_log(ic, AV_LOG_DEBUG,
                       "Non-increasing DTS in stream %d: packet %d with DTS "
                       "%"PRId64", packet %d with DTS %"PRId64"\n",
                       st->index, st->info->fps_last_dts_idx,
                       st->info->fps_last_dts, st->codec_info_nb_frames,
                       pkt->dts);
                st->info->fps_first_dts =
                st->info->fps_last_dts  = AV_NOPTS_VALUE;
            }
            /* Check for a discontinuity in dts. If the difference in dts
             * is more than 1000 times the average packet duration in the
             * sequence, we treat it as a discontinuity. */
            if (st->info->fps_last_dts != AV_NOPTS_VALUE &&
                st->info->fps_last_dts_idx > st->info->fps_first_dts_idx &&
                (pkt->dts - (uint64_t)st->info->fps_last_dts) / 1000 >
                (st->info->fps_last_dts     - (uint64_t)st->info->fps_first_dts) /
                (st->info->fps_last_dts_idx - st->info->fps_first_dts_idx)) {
                av_log(ic, AV_LOG_WARNING,
                       "DTS discontinuity in stream %d: packet %d with DTS "
                       "%"PRId64", packet %d with DTS %"PRId64"\n",
                       st->index, st->info->fps_last_dts_idx,
                       st->info->fps_last_dts, st->codec_info_nb_frames,
                       pkt->dts);
                st->info->fps_first_dts =
                st->info->fps_last_dts  = AV_NOPTS_VALUE;
            }

            /* update stored dts values */
            if (st->info->fps_first_dts == AV_NOPTS_VALUE) {
                st->info->fps_first_dts     = pkt->dts;
                st->info->fps_first_dts_idx = st->codec_info_nb_frames;
            }
            st->info->fps_last_dts     = pkt->dts;
            st->info->fps_last_dts_idx = st->codec_info_nb_frames;
        }
        if (st->codec_info_nb_frames>1) {//04.11  //举例 alaw out.al ：st->codec_info_nb_frames = 0
            int64_t t = 0;
            int64_t limit;

            if (st->time_base.den > 0)//如果基于分子分母为计时，
                t = av_rescale_q(st->info->codec_info_duration, st->time_base, AV_TIME_BASE_Q);//转化为为毫秒的单位
            if (st->avg_frame_rate.num > 0)//如果是基于帧率的
                t = FFMAX(t, av_rescale_q(st->codec_info_nb_frames, av_inv_q(st->avg_frame_rate), AV_TIME_BASE_Q));//转化为为毫秒的单位

            if (   t == 0
                && st->codec_info_nb_frames>30//如果帧率大于30==>什么时候?
                && st->info->fps_first_dts != AV_NOPTS_VALUE
                && st->info->fps_last_dts  != AV_NOPTS_VALUE)//用dts差值来计算
                t = FFMAX(t, av_rescale_q(st->info->fps_last_dts - st->info->fps_first_dts, st->time_base, AV_TIME_BASE_Q));
            //不同的流，不同的超时设置
            if (analyzed_all_streams)                                limit = max_analyze_duration;
            else if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) limit = max_subtitle_analyze_duration;
            else                                                     limit = max_stream_analyze_duration;

            if (t >= limit) {//时间超了
                av_log(ic, AV_LOG_VERBOSE, "max_analyze_duration %"PRId64" reached at %"PRId64" microseconds st:%d\n",
                       limit,
                       t, pkt->stream_index);
                if (ic->flags & AVFMT_FLAG_NOBUFFER)
                    av_packet_unref(pkt);
                break;
            }
            if (pkt->duration) {
                if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE && pkt->pts != AV_NOPTS_VALUE && st->start_time != AV_NOPTS_VALUE && pkt->pts >= st->start_time) {
                    st->info->codec_info_duration = FFMIN(pkt->pts - st->start_time, st->info->codec_info_duration + pkt->duration);
                } else
                    st->info->codec_info_duration += pkt->duration;
                st->info->codec_info_duration_fields += st->parser && st->need_parsing && avctx->ticks_per_frame ==2 ? st->parser->repeat_pict + 1 : 2;
            }
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {//04.12
#if FF_API_R_FRAME_RATE
            ff_rfps_add_frame(ic, st, pkt->dts);
#endif
            if (pkt->dts != pkt->pts && pkt->dts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
                st->info->frame_delay_evidence = 1;
        }
        if (!st->internal->avctx->extradata) {//04.13
            ret = extract_extradata(st, pkt);
            if (ret < 0)
                goto find_stream_info_err;
        }

        /* If still no information, we try to open the codec and to
         * decompress the frame. We try to avoid that in most cases as
         * it takes longer and uses more memory. For MPEG-4, we need to
         * decompress for QuickTime.
         *
         * If AV_CODEC_CAP_CHANNEL_CONF is set this will force decoding of at //tiger program 至少解一帧
         * least one frame of codec data, this makes sure the codec initializes
         * the channel configuration and does not only trust the values from//TIGER 不相信容器的配置
         * the container. */
        try_decode_frame(ic, st, pkt,//04.14 有什么用 ==>
                         (options && i < orig_nb_streams) ? &options[i] : NULL);
        
        if (ic->flags & AVFMT_FLAG_NOBUFFER)//04.15也可以将分析过的数据释放掉的
            av_packet_unref(pkt);

        st->codec_info_nb_frames++;//04.16计数
        count++;
    }
    //05. 若到达文件末尾，检查每个流是否OK
    if (eof_reached) {
        int stream_index;
        for (stream_index = 0; stream_index < ic->nb_streams; stream_index++) {
            st = ic->streams[stream_index];
            avctx = st->internal->avctx;
            if (!has_codec_parameters(st, NULL)) {//如果解码器参数还不够
                const AVCodec *codec = find_probe_decoder(ic, st, st->codecpar->codec_id);//?
                if (codec && !avctx->codec) {
                    AVDictionary *opts = NULL;
                    if (ic->codec_whitelist)
                        av_dict_set(&opts, "codec_whitelist", ic->codec_whitelist, 0);
                    if (avcodec_open2(avctx, codec, (options && stream_index < orig_nb_streams) ? &options[stream_index] : &opts) < 0)
                        av_log(ic, AV_LOG_WARNING,
                               "Failed to open codec in %s\n",__FUNCTION__);
                    av_dict_free(&opts);
                }
            }

            // EOF already reached while reading the stream above.
            // So continue with reoordering DTS with whatever delay we have.
            if (ic->internal->packet_buffer && !has_decode_delay_been_guessed(st)) {   //
                update_dts_from_pts(ic, stream_index, ic->internal->packet_buffer);
            }
        }
    }
    //06.如果是probe，尝试放入空的packet，不停解帧。
    if (flush_codecs) {//如果是probe模式的，要填入一个空的pkt
        AVPacket empty_pkt = { 0 };
        int err = 0;
        av_init_packet(&empty_pkt);//空的pkt

        for (i = 0; i < ic->nb_streams; i++) {

            st = ic->streams[i];

            /* flush the decoders */
            if (st->info->found_decoder == 1) {//如果找到解码器了，尝试继续解帧
                do {
                    err = try_decode_frame(ic, st, &empty_pkt,
                                            (options && i < orig_nb_streams)
                                            ? &options[i] : NULL);
                } while (err > 0 && !has_codec_parameters(st, NULL));//拖还有数据解出来,且编码器信息不够，则继续循环.

                if (err < 0) {
                    av_log(ic, AV_LOG_INFO,//无法解
                        "decoding for stream %d failed\n", st->index);
                }
            }
        }
    }
    //07.
    ff_rfps_calculate(ic);//real frame rate，H.264是25帧每秒，rational{3600, 90000},按TIME_BASE计算的是{25000,1000000}; 
    //08.
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        avctx = st->internal->avctx;
        if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (avctx->codec_id == AV_CODEC_ID_RAWVIDEO && !avctx->codec_tag && !avctx->bits_per_coded_sample) {
                uint32_t tag= avcodec_pix_fmt_to_codec_tag(avctx->pix_fmt);
                if (avpriv_find_pix_fmt(avpriv_get_raw_pix_fmt_tags(), tag) == avctx->pix_fmt)
                    avctx->codec_tag= tag;
            }

            /* estimate average framerate if not set by demuxer */
            if (st->info->codec_info_duration_fields &&
                !st->avg_frame_rate.num &&
                st->info->codec_info_duration) {
                int best_fps      = 0;
                double best_error = 0.01;
                AVRational codec_frame_rate = avctx->framerate;

                if (st->info->codec_info_duration        >= INT64_MAX / st->time_base.num / 2||
                    st->info->codec_info_duration_fields >= INT64_MAX / st->time_base.den ||
                    st->info->codec_info_duration        < 0)
                    continue;
                av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                          st->info->codec_info_duration_fields * (int64_t) st->time_base.den,
                          st->info->codec_info_duration * 2 * (int64_t) st->time_base.num, 60000);

                /* Round guessed framerate to a "standard" framerate if it's
                 * within 1% of the original estimate. */
                for (j = 0; j < MAX_STD_TIMEBASES; j++) {
                    AVRational std_fps = { get_std_framerate(j), 12 * 1001 };
                    double error       = fabs(av_q2d(st->avg_frame_rate) /
                                              av_q2d(std_fps) - 1);

                    if (error < best_error) {
                        best_error = error;
                        best_fps   = std_fps.num;
                    }

                    if (ic->internal->prefer_codec_framerate && codec_frame_rate.num > 0 && codec_frame_rate.den > 0) {
                        error       = fabs(av_q2d(codec_frame_rate) /
                                           av_q2d(std_fps) - 1);
                        if (error < best_error) {
                            best_error = error;
                            best_fps   = std_fps.num;
                        }
                    }
                }
                if (best_fps)
                    av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                              best_fps, 12 * 1001, INT_MAX);
            }

            if (!st->r_frame_rate.num) {
                if (    avctx->time_base.den * (int64_t) st->time_base.num
                    <= avctx->time_base.num * avctx->ticks_per_frame * (int64_t) st->time_base.den) {
                    av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den,
                              avctx->time_base.den, (int64_t)avctx->time_base.num * avctx->ticks_per_frame, INT_MAX);
                } else {
                    st->r_frame_rate.num = st->time_base.den;
                    st->r_frame_rate.den = st->time_base.num;
                }
            }
            if (st->display_aspect_ratio.num && st->display_aspect_ratio.den) {
                AVRational hw_ratio = { avctx->height, avctx->width };
                st->sample_aspect_ratio = av_mul_q(st->display_aspect_ratio,
                                                   hw_ratio);
            }
        } else if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!avctx->bits_per_coded_sample)
                avctx->bits_per_coded_sample =
                    av_get_bits_per_sample(avctx->codec_id);
            // set stream disposition based on audio service type
            switch (avctx->audio_service_type) {
            case AV_AUDIO_SERVICE_TYPE_EFFECTS:
                st->disposition = AV_DISPOSITION_CLEAN_EFFECTS;
                break;
            case AV_AUDIO_SERVICE_TYPE_VISUALLY_IMPAIRED:
                st->disposition = AV_DISPOSITION_VISUAL_IMPAIRED;
                break;
            case AV_AUDIO_SERVICE_TYPE_HEARING_IMPAIRED:
                st->disposition = AV_DISPOSITION_HEARING_IMPAIRED;
                break;
            case AV_AUDIO_SERVICE_TYPE_COMMENTARY:
                st->disposition = AV_DISPOSITION_COMMENT;
                break;
            case AV_AUDIO_SERVICE_TYPE_KARAOKE:
                st->disposition = AV_DISPOSITION_KARAOKE;
                break;
            }
        }
    }
    //09.
    if (probesize)
        estimate_timings(ic, old_offset);

    av_opt_set(ic, "skip_clear", "0", AV_OPT_SEARCH_CHILDREN);
    //10.
    if (ret >= 0 && ic->nb_streams)
        /* We could not have all the codec parameters before EOF. */
        ret = -1;
    for (i = 0; i < ic->nb_streams; i++) {
        const char *errmsg;
        st = ic->streams[i];

        /* if no packet was ever seen, update context now for has_codec_parameters */
        if (!st->internal->avctx_inited) {//曾赋值过吗，不需要probe
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                st->codecpar->format == AV_SAMPLE_FMT_NONE)//如果音频没有st->codecpar->format
                st->codecpar->format = st->internal->avctx->sample_fmt;//尝试使用st->internal->avctx->sample_fmt
            ret = avcodec_parameters_to_context(st->internal->avctx, st->codecpar);//赋值
            if (ret < 0)
                goto find_stream_info_err;
        }
        if (!has_codec_parameters(st, &errmsg)) {
            char buf[256];//若没有足够的编码信息，打印告警消息
            avcodec_string(buf, sizeof(buf), st->internal->avctx, 0);//告警信息
            av_log(ic, AV_LOG_WARNING,
                   "Could not find codec parameters for stream %d (%s): %s\n"
                   "Consider increasing the value for the 'analyzeduration' and 'probesize' options\n",
                   i, buf, errmsg);
        } else {
            ret = 0;//有一个流有信息，返回值就为0
        }
    }
    //11.chapter的结束时间没有，则计算chapter的结束时间 ==>chapter是多段的意思吗？
    compute_chapters_end(ic);//?
    //12.更新：从find的codec contexts来更新st->codecpar信息
    /* update the stream parameters from the internal codec contexts */
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        //12.01 
        if (st->internal->avctx_inited) {
            int orig_w = st->codecpar->width;//只是低分辨率情况使用
            int orig_h = st->codecpar->height;
            ret = avcodec_parameters_from_context(st->codecpar, st->internal->avctx);//从find的结果里面更新信息
            if (ret < 0)
                goto find_stream_info_err;
#if FF_API_LOWRES
            // The decoder might reduce the video size by the lowres factor.
            if (st->internal->avctx->lowres && orig_w) {//特例:低分辨率，如果st->codecpar->width有值，还是用原来的
                st->codecpar->width = orig_w;
                st->codecpar->height = orig_h;
            }
#endif
        }
        //12.02 
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        ret = avcodec_parameters_to_context(st->codec, st->codecpar);
        if (ret < 0)
            goto find_stream_info_err;
        //12.03
#if FF_API_LOWRES
        // The old API (AVStream.codec) "requires" the resolution to be adjusted
        // by the lowres factor.
        if (st->internal->avctx->lowres && st->internal->avctx->width) {//兼容老版本
            st->codec->lowres = st->internal->avctx->lowres;
            st->codec->width = st->internal->avctx->width;
            st->codec->height = st->internal->avctx->height;
        }
#endif
        //12.04
        if (st->codec->codec_tag != MKTAG('t','m','c','d')) {//如果是tmcd
            st->codec->time_base = st->internal->avctx->time_base;
            st->codec->ticks_per_frame = st->internal->avctx->ticks_per_frame;
        }
        st->codec->framerate = st->avg_frame_rate;//12.05

        if (st->internal->avctx->subtitle_header) {//12.06
            st->codec->subtitle_header = av_malloc(st->internal->avctx->subtitle_header_size);
            if (!st->codec->subtitle_header)
                goto find_stream_info_err;
            st->codec->subtitle_header_size = st->internal->avctx->subtitle_header_size;
            memcpy(st->codec->subtitle_header, st->internal->avctx->subtitle_header,
                   st->codec->subtitle_header_size);
        }

        // Fields unavailable in AVCodecParameters
        st->codec->coded_width = st->internal->avctx->coded_width;//12.07
        st->codec->coded_height = st->internal->avctx->coded_height;
        st->codec->properties = st->internal->avctx->properties;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

        st->internal->avctx_inited = 0;//12.08标志已处理
    }
    //13. 异常或正常退出都从这里关闭
find_stream_info_err:
    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->info)
            av_freep(&st->info->duration_error);
        avcodec_close(ic->streams[i]->internal->avctx);//内部find时候的AVCodecContext
        av_freep(&ic->streams[i]->info);//find 存放的信息
        av_bsf_free(&ic->streams[i]->internal->extract_extradata.bsf);//find 用到的bitstream filter state //疑问find 的时候还回用到filter吗？
        av_packet_free(&ic->streams[i]->internal->extract_extradata.pkt);
    }
    if (ic->pb)//14.再次显示读了多少字节
        av_log(ic, AV_LOG_DEBUG, "After avformat_find_stream_info() pos: %"PRId64" bytes read:%"PRId64" seeks:%d frames:%d\n",
               avio_tell(ic->pb), ic->pb->bytes_read, ic->pb->seek_count, count);
    return ret;
}

AVProgram *av_find_program_from_stream(AVFormatContext *ic, AVProgram *last, int s)
{
    int i, j;

    for (i = 0; i < ic->nb_programs; i++) {
        if (ic->programs[i] == last) {
            last = NULL;
        } else {
            if (!last)
                for (j = 0; j < ic->programs[i]->nb_stream_indexes; j++)
                    if (ic->programs[i]->stream_index[j] == s)
                        return ic->programs[i];
        }
    }
    return NULL;
}

int av_find_best_stream(AVFormatContext *ic, enum AVMediaType type,
                        int wanted_stream_nb, int related_stream,
                        AVCodec **decoder_ret, int flags)
{
    int i, nb_streams = ic->nb_streams;
    int ret = AVERROR_STREAM_NOT_FOUND;
    int best_count = -1, best_multiframe = -1, best_disposition = -1;
    int count, multiframe, disposition;
    int64_t best_bitrate = -1;
    int64_t bitrate;
    unsigned *program = NULL;
    const AVCodec *decoder = NULL, *best_decoder = NULL;

    if (related_stream >= 0 && wanted_stream_nb < 0) {
        AVProgram *p = av_find_program_from_stream(ic, NULL, related_stream);
        if (p) {
            program    = p->stream_index;
            nb_streams = p->nb_stream_indexes;
        }
    }
    for (i = 0; i < nb_streams; i++) {
        int real_stream_index = program ? program[i] : i;
        AVStream *st          = ic->streams[real_stream_index];
        AVCodecParameters *par = st->codecpar;
        if (par->codec_type != type)
            continue;
        if (wanted_stream_nb >= 0 && real_stream_index != wanted_stream_nb)
            continue;
        if (type == AVMEDIA_TYPE_AUDIO && !(par->channels && par->sample_rate))
            continue;
        if (decoder_ret) {
            decoder = find_decoder(ic, st, par->codec_id);
            if (!decoder) {
                if (ret < 0)
                    ret = AVERROR_DECODER_NOT_FOUND;
                continue;
            }
        }
        disposition = !(st->disposition & (AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_VISUAL_IMPAIRED))
                      + !! (st->disposition & AV_DISPOSITION_DEFAULT);
        count = st->codec_info_nb_frames;
        bitrate = par->bit_rate;
        multiframe = FFMIN(5, count);
        if ((best_disposition >  disposition) ||
            (best_disposition == disposition && best_multiframe >  multiframe) ||
            (best_disposition == disposition && best_multiframe == multiframe && best_bitrate >  bitrate) ||
            (best_disposition == disposition && best_multiframe == multiframe && best_bitrate == bitrate && best_count >= count))
            continue;
        best_disposition = disposition;
        best_count   = count;
        best_bitrate = bitrate;
        best_multiframe = multiframe;
        ret          = real_stream_index;
        best_decoder = decoder;
        if (program && i == nb_streams - 1 && ret < 0) {
            program    = NULL;
            nb_streams = ic->nb_streams;
            /* no related stream found, try again with everything */
            i = 0;
        }
    }
    if (decoder_ret)
        *decoder_ret = (AVCodec*)best_decoder;
    return ret;
}

/*******************************************************/

int av_read_play(AVFormatContext *s)
{
    if (s->iformat->read_play)
        return s->iformat->read_play(s);
    if (s->pb)
        return avio_pause(s->pb, 0);
    return AVERROR(ENOSYS);
}

int av_read_pause(AVFormatContext *s)
{
    if (s->iformat->read_pause)
        return s->iformat->read_pause(s);
    if (s->pb)
        return avio_pause(s->pb, 1);
    return AVERROR(ENOSYS);
}

int ff_stream_encode_params_copy(AVStream *dst, const AVStream *src)
{
    int ret, i;

    dst->id                  = src->id;
    dst->time_base           = src->time_base;
    dst->nb_frames           = src->nb_frames;
    dst->disposition         = src->disposition;
    dst->sample_aspect_ratio = src->sample_aspect_ratio;
    dst->avg_frame_rate      = src->avg_frame_rate;
    dst->r_frame_rate        = src->r_frame_rate;

    av_dict_free(&dst->metadata);
    ret = av_dict_copy(&dst->metadata, src->metadata, 0);
    if (ret < 0)
        return ret;

    ret = avcodec_parameters_copy(dst->codecpar, src->codecpar);
    if (ret < 0)
        return ret;

    /* Free existing side data*/
    for (i = 0; i < dst->nb_side_data; i++)
        av_free(dst->side_data[i].data);
    av_freep(&dst->side_data);
    dst->nb_side_data = 0;

    /* Copy side data if present */
    if (src->nb_side_data) {
        dst->side_data = av_mallocz_array(src->nb_side_data,
                                          sizeof(AVPacketSideData));
        if (!dst->side_data)
            return AVERROR(ENOMEM);
        dst->nb_side_data = src->nb_side_data;

        for (i = 0; i < src->nb_side_data; i++) {
            uint8_t *data = av_memdup(src->side_data[i].data,
                                      src->side_data[i].size);
            if (!data)
                return AVERROR(ENOMEM);
            dst->side_data[i].type = src->side_data[i].type;
            dst->side_data[i].size = src->side_data[i].size;
            dst->side_data[i].data = data;
        }
    }

#if FF_API_LAVF_FFSERVER
FF_DISABLE_DEPRECATION_WARNINGS
    av_freep(&dst->recommended_encoder_configuration);
    if (src->recommended_encoder_configuration) {
        const char *conf_str = src->recommended_encoder_configuration;
        dst->recommended_encoder_configuration = av_strdup(conf_str);
        if (!dst->recommended_encoder_configuration)
            return AVERROR(ENOMEM);
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    return 0;
}

static void free_stream(AVStream **pst)//释放
{
    AVStream *st = *pst;
    int i;

    if (!st)
        return;

    for (i = 0; i < st->nb_side_data; i++)
        av_freep(&st->side_data[i].data);
    av_freep(&st->side_data);

    if (st->parser)
        av_parser_close(st->parser);

    if (st->attached_pic.data)
        av_packet_unref(&st->attached_pic);

    if (st->internal) {
        avcodec_free_context(&st->internal->avctx);
        for (i = 0; i < st->internal->nb_bsfcs; i++) {
            av_bsf_free(&st->internal->bsfcs[i]);
            av_freep(&st->internal->bsfcs);
        }
        av_freep(&st->internal->priv_pts);
        av_bsf_free(&st->internal->extract_extradata.bsf);
        av_packet_free(&st->internal->extract_extradata.pkt);
    }
    av_freep(&st->internal);

    av_dict_free(&st->metadata);
    avcodec_parameters_free(&st->codecpar);
    av_freep(&st->probe_data.buf);
    av_freep(&st->index_entries);
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    avcodec_free_context(&st->codec);
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    av_freep(&st->priv_data);
    if (st->info)
        av_freep(&st->info->duration_error);
    av_freep(&st->info);
#if FF_API_LAVF_FFSERVER
FF_DISABLE_DEPRECATION_WARNINGS
    av_freep(&st->recommended_encoder_configuration);
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    av_freep(pst);
}

void ff_free_stream(AVFormatContext *s, AVStream *st)//
{
    av_assert0(s->nb_streams>0);
    av_assert0(s->streams[ s->nb_streams - 1 ] == st);

    free_stream(&s->streams[ --s->nb_streams ]);
}

void avformat_free_context(AVFormatContext *s)//释放AVFormatContext
{
    int i;

    if (!s)
        return;

    av_opt_free(s);
    if (s->iformat && s->iformat->priv_class && s->priv_data)
        av_opt_free(s->priv_data);
    if (s->oformat && s->oformat->priv_class && s->priv_data)
        av_opt_free(s->priv_data);

    for (i = s->nb_streams - 1; i >= 0; i--)
        ff_free_stream(s, s->streams[i]);


    for (i = s->nb_programs - 1; i >= 0; i--) {
        av_dict_free(&s->programs[i]->metadata);
        av_freep(&s->programs[i]->stream_index);
        av_freep(&s->programs[i]);
    }
    av_freep(&s->programs);
    av_freep(&s->priv_data);
    while (s->nb_chapters--) {
        av_dict_free(&s->chapters[s->nb_chapters]->metadata);
        av_freep(&s->chapters[s->nb_chapters]);
    }
    av_freep(&s->chapters);
    av_dict_free(&s->metadata);
    av_dict_free(&s->internal->id3v2_meta);
    av_freep(&s->streams);
    flush_packet_queue(s);
    av_freep(&s->internal);
    av_freep(&s->url);
    av_free(s);
}

void avformat_close_input(AVFormatContext **ps)
{
    AVFormatContext *s;
    AVIOContext *pb;

    if (!ps || !*ps)
        return;

    s  = *ps;
    pb = s->pb;

    if ((s->iformat && strcmp(s->iformat->name, "image2") && s->iformat->flags & AVFMT_NOFILE) ||
        (s->flags & AVFMT_FLAG_CUSTOM_IO))
        pb = NULL;

    flush_packet_queue(s);

    if (s->iformat)
        if (s->iformat->read_close)
            s->iformat->read_close(s);

    avformat_free_context(s);

    *ps = NULL;

    avio_close(pb);
}
// 创建avformat_find_stream_info需要的额外信息
AVStream *avformat_new_stream(AVFormatContext *s, const AVCodec *c)//TIGER avformat_new_stream
{
    AVStream *st;
    int i;
    AVStream **streams;
    //01.流个数限制
    if (s->nb_streams >= FFMIN(s->max_streams, INT_MAX/sizeof(*streams))) {
        if (s->max_streams < INT_MAX/sizeof(*streams))
            av_log(s, AV_LOG_ERROR, "Number of streams exceeds max_streams parameter (%d), see the documentation if you wish to increase it\n", s->max_streams);
        return NULL;
    }
    streams = av_realloc_array(s->streams, s->nb_streams + 1, sizeof(*streams));//02.AVFormatContext的流数组扩展一个位置
    if (!streams)
        return NULL;
    s->streams = streams;//只有在流成功的时候才更新，但这句可能是废话 //TIGER IMPROVEMENT
    //03.创建一个AVStream实例
    st = av_mallocz(sizeof(AVStream));
    if (!st)
        return NULL;
    if (!(st->info = av_mallocz(sizeof(*st->info)))) {//04.创建一个内部avformat_find_stream_info用的信息
        av_free(st);
        return NULL;
    }
    st->info->last_dts = AV_NOPTS_VALUE;//05.设置

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    st->codec = avcodec_alloc_context3(c);//06.给 AVCodec *c分配AVCodecContext
    if (!st->codec) {
        av_free(st->info);
        av_free(st);
        return NULL;
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    st->internal = av_mallocz(sizeof(*st->internal));//07.avformat_find_stream_info
    if (!st->internal)
        goto fail;

    st->codecpar = avcodec_parameters_alloc();//08.分配AVCodecParameters //重要
    if (!st->codecpar)
        goto fail;

    st->internal->avctx = avcodec_alloc_context3(NULL);//09.avformat_find_stream_info的AVCodecContext，这里没有指定 AVCodec *c
    if (!st->internal->avctx)
        goto fail;
    //输入容器存在
    if (s->iformat) {//10. 存在
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
        /* no default bitrate if decoding */
        st->codec->bit_rate = 0;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

        /* default pts setting is MPEG-like */
        avpriv_set_pts_info(st, 33, 1, 90000);//TIGER TODO
        /* we set the current DTS to 0 so that formats without any timestamps
         * but durations get some timestamps, formats with some unknown
         * timestamps have their first few packets buffered and the
         * timestamps corrected before they are returned to the user */
        st->cur_dts = RELATIVE_TS_BASE;
    } else {//不存在
        st->cur_dts = AV_NOPTS_VALUE;
    }
    //11. 常规设置
    st->index      = s->nb_streams;
    st->start_time = AV_NOPTS_VALUE;
    st->duration   = AV_NOPTS_VALUE;
    st->first_dts     = AV_NOPTS_VALUE;
    st->probe_packets = MAX_PROBE_PACKETS;
    st->pts_wrap_reference = AV_NOPTS_VALUE;
    st->pts_wrap_behavior = AV_PTS_WRAP_IGNORE;
    //12.
    st->last_IP_pts = AV_NOPTS_VALUE;
    st->last_dts_for_order_check = AV_NOPTS_VALUE;
    for (i = 0; i < MAX_REORDER_DELAY + 1; i++)//
        st->pts_buffer[i] = AV_NOPTS_VALUE;
    //13.
    st->sample_aspect_ratio = (AVRational) { 0, 1 };

#if FF_API_R_FRAME_RATE
    st->info->last_dts      = AV_NOPTS_VALUE;
#endif
    st->info->fps_first_dts = AV_NOPTS_VALUE;
    st->info->fps_last_dts  = AV_NOPTS_VALUE;

    st->inject_global_side_data = s->internal->inject_global_side_data;

    st->internal->need_context_update = 1;//重要的参数
    //14.更新
    s->streams[s->nb_streams++] = st;//添加成功后，计数加1
    return st;
fail:
    free_stream(&st);
    return NULL;
}

AVProgram *av_new_program(AVFormatContext *ac, int id)
{
    AVProgram *program = NULL;
    int i;

    av_log(ac, AV_LOG_TRACE, "new_program: id=0x%04x\n", id);

    for (i = 0; i < ac->nb_programs; i++)
        if (ac->programs[i]->id == id)
            program = ac->programs[i];

    if (!program) {
        program = av_mallocz(sizeof(AVProgram));
        if (!program)
            return NULL;
        dynarray_add(&ac->programs, &ac->nb_programs, program);
        program->discard = AVDISCARD_NONE;
        program->pmt_version = -1;
    }
    program->id = id;
    program->pts_wrap_reference = AV_NOPTS_VALUE;
    program->pts_wrap_behavior = AV_PTS_WRAP_IGNORE;

    program->start_time =
    program->end_time   = AV_NOPTS_VALUE;

    return program;
}

AVChapter *avpriv_new_chapter(AVFormatContext *s, int id, AVRational time_base,
                              int64_t start, int64_t end, const char *title)
{
    AVChapter *chapter = NULL;
    int i;

    if (end != AV_NOPTS_VALUE && start > end) {
        av_log(s, AV_LOG_ERROR, "Chapter end time %"PRId64" before start %"PRId64"\n", end, start);
        return NULL;
    }

    for (i = 0; i < s->nb_chapters; i++)
        if (s->chapters[i]->id == id)
            chapter = s->chapters[i];

    if (!chapter) {
        chapter = av_mallocz(sizeof(AVChapter));
        if (!chapter)
            return NULL;
        dynarray_add(&s->chapters, &s->nb_chapters, chapter);
    }
    av_dict_set(&chapter->metadata, "title", title, 0);
    chapter->id        = id;
    chapter->time_base = time_base;
    chapter->start     = start;
    chapter->end       = end;

    return chapter;
}

void av_program_add_stream_index(AVFormatContext *ac, int progid, unsigned idx)
{
    int i, j;
    AVProgram *program = NULL;
    void *tmp;

    if (idx >= ac->nb_streams) {
        av_log(ac, AV_LOG_ERROR, "stream index %d is not valid\n", idx);
        return;
    }

    for (i = 0; i < ac->nb_programs; i++) {
        if (ac->programs[i]->id != progid)
            continue;
        program = ac->programs[i];
        for (j = 0; j < program->nb_stream_indexes; j++)
            if (program->stream_index[j] == idx)
                return;

        tmp = av_realloc_array(program->stream_index, program->nb_stream_indexes+1, sizeof(unsigned int));
        if (!tmp)
            return;
        program->stream_index = tmp;
        program->stream_index[program->nb_stream_indexes++] = idx;
        return;
    }
}

uint64_t ff_ntp_time(void)
{
    return (av_gettime() / 1000) * 1000 + NTP_OFFSET_US;
}

uint64_t ff_get_formatted_ntp_time(uint64_t ntp_time_us)
{
    uint64_t ntp_ts, frac_part, sec;
    uint32_t usec;

    //current ntp time in seconds and micro seconds
    sec = ntp_time_us / 1000000;
    usec = ntp_time_us % 1000000;

    //encoding in ntp timestamp format
    frac_part = usec * 0xFFFFFFFFULL;
    frac_part /= 1000000;

    if (sec > 0xFFFFFFFFULL)
        av_log(NULL, AV_LOG_WARNING, "NTP time format roll over detected\n");

    ntp_ts = sec << 32;
    ntp_ts |= frac_part;

    return ntp_ts;
}

int av_get_frame_filename2(char *buf, int buf_size, const char *path, int number, int flags)
{
    const char *p;
    char *q, buf1[20], c;
    int nd, len, percentd_found;

    q = buf;
    p = path;
    percentd_found = 0;
    for (;;) {
        c = *p++;
        if (c == '\0')
            break;
        if (c == '%') {
            do {
                nd = 0;
                while (av_isdigit(*p))
                    nd = nd * 10 + *p++ - '0';
                c = *p++;
            } while (av_isdigit(c));

            switch (c) {
            case '%':
                goto addchar;
            case 'd':
                if (!(flags & AV_FRAME_FILENAME_FLAGS_MULTIPLE) && percentd_found)
                    goto fail;
                percentd_found = 1;
                if (number < 0)
                    nd += 1;
                snprintf(buf1, sizeof(buf1), "%0*d", nd, number);
                len = strlen(buf1);
                if ((q - buf + len) > buf_size - 1)
                    goto fail;
                memcpy(q, buf1, len);
                q += len;
                break;
            default:
                goto fail;
            }
        } else {
addchar:
            if ((q - buf) < buf_size - 1)
                *q++ = c;
        }
    }
    if (!percentd_found)
        goto fail;
    *q = '\0';
    return 0;
fail:
    *q = '\0';
    return -1;
}

int av_get_frame_filename(char *buf, int buf_size, const char *path, int number)
{
    return av_get_frame_filename2(buf, buf_size, path, number, 0);
}

void av_url_split(char *proto, int proto_size,
                  char *authorization, int authorization_size,
                  char *hostname, int hostname_size,
                  int *port_ptr, char *path, int path_size, const char *url)
{
    const char *p, *ls, *ls2, *at, *at2, *col, *brk;

    if (port_ptr)
        *port_ptr = -1;
    if (proto_size > 0)
        proto[0] = 0;
    if (authorization_size > 0)
        authorization[0] = 0;
    if (hostname_size > 0)
        hostname[0] = 0;
    if (path_size > 0)
        path[0] = 0;

    /* parse protocol */
    if ((p = strchr(url, ':'))) {
        av_strlcpy(proto, url, FFMIN(proto_size, p + 1 - url));
        p++; /* skip ':' */
        if (*p == '/')
            p++;
        if (*p == '/')
            p++;
    } else {
        /* no protocol means plain filename */
        av_strlcpy(path, url, path_size);
        return;
    }

    /* separate path from hostname */
    ls = strchr(p, '/');
    ls2 = strchr(p, '?');
    if (!ls)
        ls = ls2;
    else if (ls && ls2)
        ls = FFMIN(ls, ls2);
    if (ls)
        av_strlcpy(path, ls, path_size);
    else
        ls = &p[strlen(p)];  // XXX

    /* the rest is hostname, use that to parse auth/port */
    if (ls != p) {
        /* authorization (user[:pass]@hostname) */
        at2 = p;
        while ((at = strchr(p, '@')) && at < ls) {
            av_strlcpy(authorization, at2,
                       FFMIN(authorization_size, at + 1 - at2));
            p = at + 1; /* skip '@' */
        }

        if (*p == '[' && (brk = strchr(p, ']')) && brk < ls) {
            /* [host]:port */
            av_strlcpy(hostname, p + 1,
                       FFMIN(hostname_size, brk - p));
            if (brk[1] == ':' && port_ptr)
                *port_ptr = atoi(brk + 2);
        } else if ((col = strchr(p, ':')) && col < ls) {
            av_strlcpy(hostname, p,
                       FFMIN(col + 1 - p, hostname_size));
            if (port_ptr)
                *port_ptr = atoi(col + 1);
        } else
            av_strlcpy(hostname, p,
                       FFMIN(ls + 1 - p, hostname_size));
    }
}

int ff_mkdir_p(const char *path)
{
    int ret = 0;
    char *temp = av_strdup(path);
    char *pos = temp;
    char tmp_ch = '\0';

    if (!path || !temp) {
        return -1;
    }

    if (!av_strncasecmp(temp, "/", 1) || !av_strncasecmp(temp, "\\", 1)) {
        pos++;
    } else if (!av_strncasecmp(temp, "./", 2) || !av_strncasecmp(temp, ".\\", 2)) {
        pos += 2;
    }

    for ( ; *pos != '\0'; ++pos) {
        if (*pos == '/' || *pos == '\\') {
            tmp_ch = *pos;
            *pos = '\0';
            ret = mkdir(temp, 0755);
            *pos = tmp_ch;
        }
    }

    if ((*(pos - 1) != '/') || (*(pos - 1) != '\\')) {
        ret = mkdir(temp, 0755);
    }

    av_free(temp);
    return ret;
}

char *ff_data_to_hex(char *buff, const uint8_t *src, int s, int lowercase)//转为16进制字符
{
    int i;
    static const char hex_table_uc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'A', 'B',
                                           'C', 'D', 'E', 'F' };
    static const char hex_table_lc[16] = { '0', '1', '2', '3',
                                           '4', '5', '6', '7',
                                           '8', '9', 'a', 'b',
                                           'c', 'd', 'e', 'f' };
    const char *hex_table = lowercase ? hex_table_lc : hex_table_uc;

    for (i = 0; i < s; i++) {
        buff[i * 2]     = hex_table[src[i] >> 4];
        buff[i * 2 + 1] = hex_table[src[i] & 0xF];
    }

    return buff;
}

int ff_hex_to_data(uint8_t *data, const char *p)//16进制字符转数字
{
    int c, len, v;

    len = 0;
    v   = 1;
    for (;;) {
        p += strspn(p, SPACE_CHARS);
        if (*p == '\0')
            break;
        c = av_toupper((unsigned char) *p++);
        if (c >= '0' && c <= '9')
            c = c - '0';
        else if (c >= 'A' && c <= 'F')
            c = c - 'A' + 10;
        else
            break;
        v = (v << 4) | c;
        if (v & 0x100) {
            if (data)
                data[len] = v;
            len++;
            v = 1;
        }
    }
    return len;
}

void avpriv_set_pts_info(AVStream *s, int pts_wrap_bits,
                         unsigned int pts_num, unsigned int pts_den)
{
    AVRational new_tb;
    if (av_reduce(&new_tb.num, &new_tb.den, pts_num, pts_den, INT_MAX)) {
        if (new_tb.num != pts_num)
            av_log(NULL, AV_LOG_DEBUG,
                   "st:%d removing common factor %d from timebase\n",
                   s->index, pts_num / new_tb.num);
    } else
        av_log(NULL, AV_LOG_WARNING,
               "st:%d has too large timebase, reducing\n", s->index);

    if (new_tb.num <= 0 || new_tb.den <= 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Ignoring attempt to set invalid timebase %d/%d for st:%d\n",
               new_tb.num, new_tb.den,
               s->index);
        return;
    }
    s->time_base     = new_tb;
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    s->codec->pkt_timebase = new_tb;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    s->internal->avctx->pkt_timebase = new_tb;
    s->pts_wrap_bits = pts_wrap_bits;
}

void ff_parse_key_value(const char *str, ff_parse_key_val_cb callback_get_buf,//解析
                        void *context)
{
    const char *ptr = str;

    /* Parse key=value pairs. */
    for (;;) {
        const char *key;
        char *dest = NULL, *dest_end;
        int key_len, dest_len = 0;

        /* Skip whitespace and potential commas. */
        while (*ptr && (av_isspace(*ptr) || *ptr == ','))
            ptr++;
        if (!*ptr)
            break;

        key = ptr;

        if (!(ptr = strchr(key, '=')))
            break;
        ptr++;
        key_len = ptr - key;

        callback_get_buf(context, key, key_len, &dest, &dest_len);
        dest_end = dest + dest_len - 1;

        if (*ptr == '\"') {
            ptr++;
            while (*ptr && *ptr != '\"') {
                if (*ptr == '\\') {
                    if (!ptr[1])
                        break;
                    if (dest && dest < dest_end)
                        *dest++ = ptr[1];
                    ptr += 2;
                } else {
                    if (dest && dest < dest_end)
                        *dest++ = *ptr;
                    ptr++;
                }
            }
            if (*ptr == '\"')
                ptr++;
        } else {
            for (; *ptr && !(av_isspace(*ptr) || *ptr == ','); ptr++)
                if (dest && dest < dest_end)
                    *dest++ = *ptr;
        }
        if (dest)
            *dest = 0;
    }
}

int ff_find_stream_index(AVFormatContext *s, int id)
{
    int i;
    for (i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == id)
            return i;
    return -1;
}

int avformat_query_codec(const AVOutputFormat *ofmt, enum AVCodecID codec_id,
                         int std_compliance)
{
    if (ofmt) {
        unsigned int codec_tag;
        if (ofmt->query_codec)
            return ofmt->query_codec(codec_id, std_compliance);
        else if (ofmt->codec_tag)
            return !!av_codec_get_tag2(ofmt->codec_tag, codec_id, &codec_tag);
        else if (codec_id == ofmt->video_codec ||
                 codec_id == ofmt->audio_codec ||
                 codec_id == ofmt->subtitle_codec ||
                 codec_id == ofmt->data_codec)
            return 1;
    }
    return AVERROR_PATCHWELCOME;
}

int avformat_network_init(void)
{
#if CONFIG_NETWORK
    int ret;
    if ((ret = ff_network_init()) < 0)
        return ret;
    if ((ret = ff_tls_init()) < 0)
        return ret;
#endif
    return 0;
}

int avformat_network_deinit(void)//退出网络
{
#if CONFIG_NETWORK
    ff_network_close();//windows 影响比较大
    ff_tls_deinit();//tls 
#endif
    return 0;
}

int ff_add_param_change(AVPacket *pkt, int32_t channels,
                        uint64_t channel_layout, int32_t sample_rate,
                        int32_t width, int32_t height)
{
    uint32_t flags = 0;
    int size = 4;
    uint8_t *data;
    if (!pkt)
        return AVERROR(EINVAL);
    if (channels) {
        size  += 4;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_COUNT;
    }
    if (channel_layout) {
        size  += 8;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_LAYOUT;
    }
    if (sample_rate) {
        size  += 4;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_SAMPLE_RATE;
    }
    if (width || height) {
        size  += 8;
        flags |= AV_SIDE_DATA_PARAM_CHANGE_DIMENSIONS;
    }
    data = av_packet_new_side_data(pkt, AV_PKT_DATA_PARAM_CHANGE, size);
    if (!data)
        return AVERROR(ENOMEM);
    bytestream_put_le32(&data, flags);
    if (channels)
        bytestream_put_le32(&data, channels);
    if (channel_layout)
        bytestream_put_le64(&data, channel_layout);
    if (sample_rate)
        bytestream_put_le32(&data, sample_rate);
    if (width || height) {
        bytestream_put_le32(&data, width);
        bytestream_put_le32(&data, height);
    }
    return 0;
}

AVRational av_guess_sample_aspect_ratio(AVFormatContext *format, AVStream *stream, AVFrame *frame)
{
    AVRational undef = {0, 1};
    AVRational stream_sample_aspect_ratio = stream ? stream->sample_aspect_ratio : undef;
    AVRational codec_sample_aspect_ratio  = stream && stream->codecpar ? stream->codecpar->sample_aspect_ratio : undef;
    AVRational frame_sample_aspect_ratio  = frame  ? frame->sample_aspect_ratio  : codec_sample_aspect_ratio;

    av_reduce(&stream_sample_aspect_ratio.num, &stream_sample_aspect_ratio.den,
               stream_sample_aspect_ratio.num,  stream_sample_aspect_ratio.den, INT_MAX);
    if (stream_sample_aspect_ratio.num <= 0 || stream_sample_aspect_ratio.den <= 0)
        stream_sample_aspect_ratio = undef;

    av_reduce(&frame_sample_aspect_ratio.num, &frame_sample_aspect_ratio.den,
               frame_sample_aspect_ratio.num,  frame_sample_aspect_ratio.den, INT_MAX);
    if (frame_sample_aspect_ratio.num <= 0 || frame_sample_aspect_ratio.den <= 0)
        frame_sample_aspect_ratio = undef;

    if (stream_sample_aspect_ratio.num)
        return stream_sample_aspect_ratio;
    else
        return frame_sample_aspect_ratio;
}

AVRational av_guess_frame_rate(AVFormatContext *format, AVStream *st, AVFrame *frame)
{
    AVRational fr = st->r_frame_rate;
    AVRational codec_fr = st->internal->avctx->framerate;
    AVRational   avg_fr = st->avg_frame_rate;

    if (avg_fr.num > 0 && avg_fr.den > 0 && fr.num > 0 && fr.den > 0 &&
        av_q2d(avg_fr) < 70 && av_q2d(fr) > 210) {
        fr = avg_fr;
    }


    if (st->internal->avctx->ticks_per_frame > 1) {
        if (   codec_fr.num > 0 && codec_fr.den > 0 &&
            (fr.num == 0 || av_q2d(codec_fr) < av_q2d(fr)*0.7 && fabs(1.0 - av_q2d(av_div_q(avg_fr, fr))) > 0.1))
            fr = codec_fr;
    }

    return fr;
}

/**
 * Matches a stream specifier (but ignores requested index).
 *
 * @param indexptr set to point to the requested stream index if there is one
 *
 * @return <0 on error
 *         0  if st is NOT a matching stream
 *         >0 if st is a matching stream
 */
static int match_stream_specifier(AVFormatContext *s, AVStream *st,
                                  const char *spec, const char **indexptr, AVProgram **p)
{
    int match = 1;                      /* Stores if the specifier matches so far. */
    while (*spec) {
        if (*spec <= '9' && *spec >= '0') { /* opt:index */
            if (indexptr)
                *indexptr = spec;
            return match;
        } else if (*spec == 'v' || *spec == 'a' || *spec == 's' || *spec == 'd' ||
                   *spec == 't' || *spec == 'V') { /* opt:[vasdtV] */
            enum AVMediaType type;
            int nopic = 0;

            switch (*spec++) {
            case 'v': type = AVMEDIA_TYPE_VIDEO;      break;
            case 'a': type = AVMEDIA_TYPE_AUDIO;      break;
            case 's': type = AVMEDIA_TYPE_SUBTITLE;   break;
            case 'd': type = AVMEDIA_TYPE_DATA;       break;
            case 't': type = AVMEDIA_TYPE_ATTACHMENT; break;
            case 'V': type = AVMEDIA_TYPE_VIDEO; nopic = 1; break;
            default:  av_assert0(0);
            }
            if (*spec && *spec++ != ':')         /* If we are not at the end, then another specifier must follow. */
                return AVERROR(EINVAL);

#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
            if (type != st->codecpar->codec_type
               && (st->codecpar->codec_type != AVMEDIA_TYPE_UNKNOWN || st->codec->codec_type != type))
                match = 0;
    FF_ENABLE_DEPRECATION_WARNINGS
#else
            if (type != st->codecpar->codec_type)
                match = 0;
#endif
            if (nopic && (st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                match = 0;
        } else if (*spec == 'p' && *(spec + 1) == ':') {
            int prog_id, i, j;
            int found = 0;
            char *endptr;
            spec += 2;
            prog_id = strtol(spec, &endptr, 0);
            /* Disallow empty id and make sure that if we are not at the end, then another specifier must follow. */
            if (spec == endptr || (*endptr && *endptr++ != ':'))
                return AVERROR(EINVAL);
            spec = endptr;
            if (match) {
                for (i = 0; i < s->nb_programs; i++) {
                    if (s->programs[i]->id != prog_id)
                        continue;

                    for (j = 0; j < s->programs[i]->nb_stream_indexes; j++) {
                        if (st->index == s->programs[i]->stream_index[j]) {
                            found = 1;
                            if (p)
                                *p = s->programs[i];
                            i = s->nb_programs;
                            break;
                        }
                    }
                }
            }
            if (!found)
                match = 0;
        } else if (*spec == '#' ||
                   (*spec == 'i' && *(spec + 1) == ':')) {
            int stream_id;
            char *endptr;
            spec += 1 + (*spec == 'i');
            stream_id = strtol(spec, &endptr, 0);
            if (spec == endptr || *endptr)                /* Disallow empty id and make sure we are at the end. */
                return AVERROR(EINVAL);
            return match && (stream_id == st->id);
        } else if (*spec == 'm' && *(spec + 1) == ':') {
            AVDictionaryEntry *tag;
            char *key, *val;
            int ret;

            if (match) {
               spec += 2;
               val = strchr(spec, ':');

               key = val ? av_strndup(spec, val - spec) : av_strdup(spec);
               if (!key)
                   return AVERROR(ENOMEM);

               tag = av_dict_get(st->metadata, key, NULL, 0);
               if (tag) {
                   if (!val || !strcmp(tag->value, val + 1))
                       ret = 1;
                   else
                       ret = 0;
               } else
                   ret = 0;

               av_freep(&key);
            }
            return match && ret;
        } else if (*spec == 'u' && *(spec + 1) == '\0') {
            AVCodecParameters *par = st->codecpar;
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
            AVCodecContext *codec = st->codec;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
            int val;
            switch (par->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                val = par->sample_rate && par->channels;
#if FF_API_LAVF_AVCTX
                val = val || (codec->sample_rate && codec->channels);
#endif
                if (par->format == AV_SAMPLE_FMT_NONE
#if FF_API_LAVF_AVCTX
                    && codec->sample_fmt == AV_SAMPLE_FMT_NONE
#endif
                    )
                    return 0;
                break;
            case AVMEDIA_TYPE_VIDEO:
                val = par->width && par->height;
#if FF_API_LAVF_AVCTX
                val = val || (codec->width && codec->height);
#endif
                if (par->format == AV_PIX_FMT_NONE
#if FF_API_LAVF_AVCTX
                    && codec->pix_fmt == AV_PIX_FMT_NONE
#endif
                    )
                    return 0;
                break;
            case AVMEDIA_TYPE_UNKNOWN:
                val = 0;
                break;
            default:
                val = 1;
                break;
            }
#if FF_API_LAVF_AVCTX
            return match && ((par->codec_id != AV_CODEC_ID_NONE || codec->codec_id != AV_CODEC_ID_NONE) && val != 0);
#else
            return match && (par->codec_id != AV_CODEC_ID_NONE && val != 0);
#endif
        } else {
            return AVERROR(EINVAL);
        }
    }

    return match;
}


int avformat_match_stream_specifier(AVFormatContext *s, AVStream *st,
                                    const char *spec)
{
    int ret, index;
    char *endptr;
    const char *indexptr = NULL;
    AVProgram *p = NULL;
    int nb_streams;

    ret = match_stream_specifier(s, st, spec, &indexptr, &p);
    if (ret < 0)
        goto error;

    if (!indexptr)
        return ret;

    index = strtol(indexptr, &endptr, 0);
    if (*endptr) {                  /* We can't have anything after the requested index. */
        ret = AVERROR(EINVAL);
        goto error;
    }

    /* This is not really needed but saves us a loop for simple stream index specifiers. */
    if (spec == indexptr)
        return (index == st->index);

    /* If we requested a matching stream index, we have to ensure st is that. */
    nb_streams = p ? p->nb_stream_indexes : s->nb_streams;
    for (int i = 0; i < nb_streams && index >= 0; i++) {
        AVStream *candidate = p ? s->streams[p->stream_index[i]] : s->streams[i];
        ret = match_stream_specifier(s, candidate, spec, NULL, NULL);
        if (ret < 0)
            goto error;
        if (ret > 0 && index-- == 0 && st == candidate)
            return 1;
    }
    return 0;

error:
    if (ret == AVERROR(EINVAL))
        av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
    return ret;
}

int ff_generate_avci_extradata(AVStream *st)//产生sps和pps
{
    static const uint8_t avci100_1080p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x22, 0x33, 0x19, 0xc6, 0x63,
        0x23, 0x21, 0x01, 0x11, 0x98, 0xce, 0x33, 0x19,
        0x18, 0x21, 0x02, 0x56, 0xb9, 0x3d, 0x7d, 0x7e,
        0x4f, 0xe3, 0x3f, 0x11, 0xf1, 0x9e, 0x08, 0xb8,
        0x8c, 0x54, 0x43, 0xc0, 0x78, 0x02, 0x27, 0xe2,
        0x70, 0x1e, 0x30, 0x10, 0x10, 0x14, 0x00, 0x00,
        0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xca,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x33, 0x48,
        0xd0
    };
    static const uint8_t avci100_1080i_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x22, 0x33, 0x19, 0xc6, 0x63,
        0x23, 0x21, 0x01, 0x11, 0x98, 0xce, 0x33, 0x19,
        0x18, 0x21, 0x03, 0x3a, 0x46, 0x65, 0x6a, 0x65,
        0x24, 0xad, 0xe9, 0x12, 0x32, 0x14, 0x1a, 0x26,
        0x34, 0xad, 0xa4, 0x41, 0x82, 0x23, 0x01, 0x50,
        0x2b, 0x1a, 0x24, 0x69, 0x48, 0x30, 0x40, 0x2e,
        0x11, 0x12, 0x08, 0xc6, 0x8c, 0x04, 0x41, 0x28,
        0x4c, 0x34, 0xf0, 0x1e, 0x01, 0x13, 0xf2, 0xe0,
        0x3c, 0x60, 0x20, 0x20, 0x28, 0x00, 0x00, 0x03,
        0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0x94, 0x20,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x33, 0x48,
        0xd0
    };
    static const uint8_t avci50_1080p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x6e, 0x10, 0x28,
        0xa6, 0xd4, 0x20, 0x32, 0x33, 0x0c, 0x71, 0x18,
        0x88, 0x62, 0x10, 0x19, 0x19, 0x86, 0x38, 0x8c,
        0x44, 0x30, 0x21, 0x02, 0x56, 0x4e, 0x6f, 0x37,
        0xcd, 0xf9, 0xbf, 0x81, 0x6b, 0xf3, 0x7c, 0xde,
        0x6e, 0x6c, 0xd3, 0x3c, 0x05, 0xa0, 0x22, 0x7e,
        0x5f, 0xfc, 0x00, 0x0c, 0x00, 0x13, 0x8c, 0x04,
        0x04, 0x05, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
        0x00, 0x03, 0x00, 0x32, 0x84, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x31, 0x12,
        0x11
    };
    static const uint8_t avci50_1080i_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x6e, 0x10, 0x28,
        0xa6, 0xd4, 0x20, 0x32, 0x33, 0x0c, 0x71, 0x18,
        0x88, 0x62, 0x10, 0x19, 0x19, 0x86, 0x38, 0x8c,
        0x44, 0x30, 0x21, 0x02, 0x56, 0x4e, 0x6e, 0x61,
        0x87, 0x3e, 0x73, 0x4d, 0x98, 0x0c, 0x03, 0x06,
        0x9c, 0x0b, 0x73, 0xe6, 0xc0, 0xb5, 0x18, 0x63,
        0x0d, 0x39, 0xe0, 0x5b, 0x02, 0xd4, 0xc6, 0x19,
        0x1a, 0x79, 0x8c, 0x32, 0x34, 0x24, 0xf0, 0x16,
        0x81, 0x13, 0xf7, 0xff, 0x80, 0x02, 0x00, 0x01,
        0xf1, 0x80, 0x80, 0x80, 0xa0, 0x00, 0x00, 0x03,
        0x00, 0x20, 0x00, 0x00, 0x06, 0x50, 0x80, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x31, 0x12,
        0x11
    };
    static const uint8_t avci100_720p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x7a, 0x10, 0x29,
        0xb6, 0xd4, 0x20, 0x2a, 0x33, 0x1d, 0xc7, 0x62,
        0xa1, 0x08, 0x40, 0x54, 0x66, 0x3b, 0x8e, 0xc5,
        0x42, 0x02, 0x10, 0x25, 0x64, 0x2c, 0x89, 0xe8,
        0x85, 0xe4, 0x21, 0x4b, 0x90, 0x83, 0x06, 0x95,
        0xd1, 0x06, 0x46, 0x97, 0x20, 0xc8, 0xd7, 0x43,
        0x08, 0x11, 0xc2, 0x1e, 0x4c, 0x91, 0x0f, 0x01,
        0x40, 0x16, 0xec, 0x07, 0x8c, 0x04, 0x04, 0x05,
        0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03,
        0x00, 0x64, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x31, 0x12,
        0x11
    };
    static const uint8_t avci50_720p_extradata[] = {
        // SPS
        0x00, 0x00, 0x00, 0x01, 0x67, 0x6e, 0x10, 0x20,
        0xa6, 0xd4, 0x20, 0x32, 0x33, 0x0c, 0x71, 0x18,
        0x88, 0x62, 0x10, 0x19, 0x19, 0x86, 0x38, 0x8c,
        0x44, 0x30, 0x21, 0x02, 0x56, 0x4e, 0x6f, 0x37,
        0xcd, 0xf9, 0xbf, 0x81, 0x6b, 0xf3, 0x7c, 0xde,
        0x6e, 0x6c, 0xd3, 0x3c, 0x0f, 0x01, 0x6e, 0xff,
        0xc0, 0x00, 0xc0, 0x01, 0x38, 0xc0, 0x40, 0x40,
        0x50, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00,
        0x06, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
        // PPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x31, 0x12,
        0x11
    };

    const uint8_t *data = NULL;
    int size            = 0;

    if (st->codecpar->width == 1920) {
        if (st->codecpar->field_order == AV_FIELD_PROGRESSIVE) {
            data = avci100_1080p_extradata;
            size = sizeof(avci100_1080p_extradata);
        } else {
            data = avci100_1080i_extradata;
            size = sizeof(avci100_1080i_extradata);
        }
    } else if (st->codecpar->width == 1440) {
        if (st->codecpar->field_order == AV_FIELD_PROGRESSIVE) {
            data = avci50_1080p_extradata;
            size = sizeof(avci50_1080p_extradata);
        } else {
            data = avci50_1080i_extradata;
            size = sizeof(avci50_1080i_extradata);
        }
    } else if (st->codecpar->width == 1280) {
        data = avci100_720p_extradata;
        size = sizeof(avci100_720p_extradata);
    } else if (st->codecpar->width == 960) {
        data = avci50_720p_extradata;
        size = sizeof(avci50_720p_extradata);
    }

    if (!size)
        return 0;

    av_freep(&st->codecpar->extradata);
    if (ff_alloc_extradata(st->codecpar, size))
        return AVERROR(ENOMEM);
    memcpy(st->codecpar->extradata, data, size);

    return 0;
}

uint8_t *av_stream_get_side_data(const AVStream *st,
                                 enum AVPacketSideDataType type, int *size)
{
    int i;

    for (i = 0; i < st->nb_side_data; i++) {
        if (st->side_data[i].type == type) {
            if (size)
                *size = st->side_data[i].size;
            return st->side_data[i].data;
        }
    }
    return NULL;
}

int av_stream_add_side_data(AVStream *st, enum AVPacketSideDataType type,
                            uint8_t *data, size_t size)
{
    AVPacketSideData *sd, *tmp;
    int i;

    for (i = 0; i < st->nb_side_data; i++) {
        sd = &st->side_data[i];

        if (sd->type == type) {
            av_freep(&sd->data);
            sd->data = data;
            sd->size = size;
            return 0;
        }
    }

    if ((unsigned)st->nb_side_data + 1 >= INT_MAX / sizeof(*st->side_data))
        return AVERROR(ERANGE);

    tmp = av_realloc(st->side_data, (st->nb_side_data + 1) * sizeof(*tmp));
    if (!tmp) {
        return AVERROR(ENOMEM);
    }

    st->side_data = tmp;
    st->nb_side_data++;

    sd = &st->side_data[st->nb_side_data - 1];
    sd->type = type;
    sd->data = data;
    sd->size = size;

    return 0;
}

uint8_t *av_stream_new_side_data(AVStream *st, enum AVPacketSideDataType type,
                                 int size)
{
    int ret;
    uint8_t *data = av_malloc(size);

    if (!data)
        return NULL;

    ret = av_stream_add_side_data(st, type, data, size);
    if (ret < 0) {
        av_freep(&data);
        return NULL;
    }

    return data;
}

int ff_stream_add_bitstream_filter(AVStream *st, const char *name, const char *args)
{
    int ret;
    const AVBitStreamFilter *bsf;
    AVBSFContext *bsfc;
    AVCodecParameters *in_par;

    if (!(bsf = av_bsf_get_by_name(name))) {
        av_log(NULL, AV_LOG_ERROR, "Unknown bitstream filter '%s'\n", name);
        return AVERROR_BSF_NOT_FOUND;
    }

    if ((ret = av_bsf_alloc(bsf, &bsfc)) < 0)
        return ret;

    if (st->internal->nb_bsfcs) {
        in_par = st->internal->bsfcs[st->internal->nb_bsfcs - 1]->par_out;
        bsfc->time_base_in = st->internal->bsfcs[st->internal->nb_bsfcs - 1]->time_base_out;
    } else {
        in_par = st->codecpar;
        bsfc->time_base_in = st->time_base;
    }

    if ((ret = avcodec_parameters_copy(bsfc->par_in, in_par)) < 0) {
        av_bsf_free(&bsfc);
        return ret;
    }

    if (args && bsfc->filter->priv_class) {
        const AVOption *opt = av_opt_next(bsfc->priv_data, NULL);
        const char * shorthand[2] = {NULL};

        if (opt)
            shorthand[0] = opt->name;

        if ((ret = av_opt_set_from_string(bsfc->priv_data, args, shorthand, "=", ":")) < 0) {
            av_bsf_free(&bsfc);
            return ret;
        }
    }

    if ((ret = av_bsf_init(bsfc)) < 0) {
        av_bsf_free(&bsfc);
        return ret;
    }

    if ((ret = av_dynarray_add_nofree(&st->internal->bsfcs, &st->internal->nb_bsfcs, bsfc))) {
        av_bsf_free(&bsfc);
        return ret;
    }

    av_log(NULL, AV_LOG_VERBOSE,
           "Automatically inserted bitstream filter '%s'; args='%s'\n",
           name, args ? args : "");
    return 1;
}

#if FF_API_OLD_BSF
FF_DISABLE_DEPRECATION_WARNINGS
int av_apply_bitstream_filters(AVCodecContext *codec, AVPacket *pkt,
                               AVBitStreamFilterContext *bsfc)
{
    int ret = 0;
    while (bsfc) {
        AVPacket new_pkt = *pkt;
        int a = av_bitstream_filter_filter(bsfc, codec, NULL,
                                           &new_pkt.data, &new_pkt.size,
                                           pkt->data, pkt->size,
                                           pkt->flags & AV_PKT_FLAG_KEY);
        if (a == 0 && new_pkt.size == 0 && new_pkt.side_data_elems == 0) {
            av_packet_unref(pkt);
            memset(pkt, 0, sizeof(*pkt));
            return 0;
        }
        if(a == 0 && new_pkt.data != pkt->data) {
            uint8_t *t = av_malloc(new_pkt.size + AV_INPUT_BUFFER_PADDING_SIZE); //the new should be a subset of the old so cannot overflow
            if (t) {
                memcpy(t, new_pkt.data, new_pkt.size);
                memset(t + new_pkt.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                new_pkt.data = t;
                new_pkt.buf = NULL;
                a = 1;
            } else {
                a = AVERROR(ENOMEM);
            }
        }
        if (a > 0) {
            new_pkt.buf = av_buffer_create(new_pkt.data, new_pkt.size,
                                           av_buffer_default_free, NULL, 0);
            if (new_pkt.buf) {
                pkt->side_data = NULL;
                pkt->side_data_elems = 0;
                av_packet_unref(pkt);
            } else {
                av_freep(&new_pkt.data);
                a = AVERROR(ENOMEM);
            }
        }
        if (a < 0) {
            av_log(codec, AV_LOG_ERROR,
                   "Failed to open bitstream filter %s for stream %d with codec %s",
                   bsfc->filter->name, pkt->stream_index,
                   codec->codec ? codec->codec->name : "copy");
            ret = a;
            break;
        }
        *pkt = new_pkt;

        bsfc = bsfc->next;
    }
    return ret;
}
FF_ENABLE_DEPRECATION_WARNINGS
#endif

int ff_format_output_open(AVFormatContext *s, const char *url, AVDictionary **options)//AVFormatContext
{
    if (!s->oformat)
        return AVERROR(EINVAL);

    if (!(s->oformat->flags & AVFMT_NOFILE))//如果有问题，打开
        return s->io_open(s, &s->pb, url, AVIO_FLAG_WRITE, options);
    return 0;
}

void ff_format_io_close(AVFormatContext *s, AVIOContext **pb)
{
    if (*pb)
        s->io_close(s, *pb);
    *pb = NULL;
}

int ff_is_http_proto(char *filename) {
    const char *proto = avio_find_protocol_name(filename);
    return proto ? (!av_strcasecmp(proto, "http") || !av_strcasecmp(proto, "https")) : 0;
}

int ff_parse_creation_time_metadata(AVFormatContext *s, int64_t *timestamp, int return_seconds)
{
    AVDictionaryEntry *entry;
    int64_t parsed_timestamp;
    int ret;
    if ((entry = av_dict_get(s->metadata, "creation_time", NULL, 0))) {
        if ((ret = av_parse_time(&parsed_timestamp, entry->value, 0)) >= 0) {
            *timestamp = return_seconds ? parsed_timestamp / 1000000 : parsed_timestamp;
            return 1;
        } else {
            av_log(s, AV_LOG_WARNING, "Failed to parse creation_time %s\n", entry->value);
            return ret;
        }
    }
    return 0;
}

int ff_standardize_creation_time(AVFormatContext *s)
{
    int64_t timestamp;
    int ret = ff_parse_creation_time_metadata(s, &timestamp, 0);
    if (ret == 1)
        return avpriv_dict_set_timestamp(&s->metadata, "creation_time", timestamp);
    return ret;
}

int ff_get_packet_palette(AVFormatContext *s, AVPacket *pkt, int ret, uint32_t *palette)
{
    uint8_t *side_data;
    int size;

    side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_PALETTE, &size);
    if (side_data) {
        if (size != AVPALETTE_SIZE) {
            av_log(s, AV_LOG_ERROR, "Invalid palette side data\n");
            return AVERROR_INVALIDDATA;
        }
        memcpy(palette, side_data, AVPALETTE_SIZE);
        return 1;
    }

    if (ret == CONTAINS_PAL) {
        int i;
        for (i = 0; i < AVPALETTE_COUNT; i++)
            palette[i] = AV_RL32(pkt->data + pkt->size - AVPALETTE_SIZE + i*4);
        return 1;
    }

    return 0;
}

int ff_bprint_to_codecpar_extradata(AVCodecParameters *par, struct AVBPrint *buf)
{
    int ret;
    char *str;

    ret = av_bprint_finalize(buf, &str);
    if (ret < 0)
        return ret;
    if (!av_bprint_is_complete(buf)) {
        av_free(str);
        return AVERROR(ENOMEM);
    }

    par->extradata = str;
    /* Note: the string is NUL terminated (so extradata can be read as a
     * string), but the ending character is not accounted in the size (in
     * binary formats you are likely not supposed to mux that character). When
     * extradata is copied, it is also padded with AV_INPUT_BUFFER_PADDING_SIZE
     * zeros. */
    par->extradata_size = buf->len;
    return 0;
}

int avformat_transfer_internal_stream_timing_info(const AVOutputFormat *ofmt,
                                                  AVStream *ost, const AVStream *ist,
                                                  enum AVTimebaseSource copy_tb)
{
    //TODO: use [io]st->internal->avctx
    const AVCodecContext *dec_ctx = ist->codec;
    AVCodecContext       *enc_ctx = ost->codec;

    enc_ctx->time_base = ist->time_base;
    /*
     * Avi is a special case here because it supports variable fps but
     * having the fps and timebase differe significantly adds quite some
     * overhead
     */
    if (!strcmp(ofmt->name, "avi")) {
#if FF_API_R_FRAME_RATE
        if (copy_tb == AVFMT_TBCF_AUTO && ist->r_frame_rate.num
            && av_q2d(ist->r_frame_rate) >= av_q2d(ist->avg_frame_rate)
            && 0.5/av_q2d(ist->r_frame_rate) > av_q2d(ist->time_base)
            && 0.5/av_q2d(ist->r_frame_rate) > av_q2d(dec_ctx->time_base)
            && av_q2d(ist->time_base) < 1.0/500 && av_q2d(dec_ctx->time_base) < 1.0/500
            || copy_tb == AVFMT_TBCF_R_FRAMERATE) {
            enc_ctx->time_base.num = ist->r_frame_rate.den;
            enc_ctx->time_base.den = 2*ist->r_frame_rate.num;
            enc_ctx->ticks_per_frame = 2;
        } else
#endif
            if (copy_tb == AVFMT_TBCF_AUTO && av_q2d(dec_ctx->time_base)*dec_ctx->ticks_per_frame > 2*av_q2d(ist->time_base)
                   && av_q2d(ist->time_base) < 1.0/500
                   || copy_tb == AVFMT_TBCF_DECODER) {
            enc_ctx->time_base = dec_ctx->time_base;
            enc_ctx->time_base.num *= dec_ctx->ticks_per_frame;
            enc_ctx->time_base.den *= 2;
            enc_ctx->ticks_per_frame = 2;
        }
    } else if (!(ofmt->flags & AVFMT_VARIABLE_FPS)
               && !av_match_name(ofmt->name, "mov,mp4,3gp,3g2,psp,ipod,ismv,f4v")) {
        if (copy_tb == AVFMT_TBCF_AUTO && dec_ctx->time_base.den
            && av_q2d(dec_ctx->time_base)*dec_ctx->ticks_per_frame > av_q2d(ist->time_base)
            && av_q2d(ist->time_base) < 1.0/500
            || copy_tb == AVFMT_TBCF_DECODER) {
            enc_ctx->time_base = dec_ctx->time_base;
            enc_ctx->time_base.num *= dec_ctx->ticks_per_frame;
        }
    }

    if ((enc_ctx->codec_tag == AV_RL32("tmcd") || ost->codecpar->codec_tag == AV_RL32("tmcd"))
        && dec_ctx->time_base.num < dec_ctx->time_base.den
        && dec_ctx->time_base.num > 0
        && 121LL*dec_ctx->time_base.num > dec_ctx->time_base.den) {
        enc_ctx->time_base = dec_ctx->time_base;
    }

    if (ost->avg_frame_rate.num)
        enc_ctx->time_base = av_inv_q(ost->avg_frame_rate);

    av_reduce(&enc_ctx->time_base.num, &enc_ctx->time_base.den,
              enc_ctx->time_base.num, enc_ctx->time_base.den, INT_MAX);

    return 0;
}

AVRational av_stream_get_codec_timebase(const AVStream *st)
{
    // See avformat_transfer_internal_stream_timing_info() TODO.
#if FF_API_LAVF_AVCTX
FF_DISABLE_DEPRECATION_WARNINGS
    return st->codec->time_base;
FF_ENABLE_DEPRECATION_WARNINGS
#else
    return st->internal->avctx->time_base;
#endif
}

void ff_format_set_url(AVFormatContext *s, char *url)
{
    av_assert0(url);
    av_freep(&s->url);
    s->url = url;
#if FF_API_FORMAT_FILENAME
FF_DISABLE_DEPRECATION_WARNINGS
    av_strlcpy(s->filename, url, sizeof(s->filename));
FF_ENABLE_DEPRECATION_WARNINGS
#endif
}
