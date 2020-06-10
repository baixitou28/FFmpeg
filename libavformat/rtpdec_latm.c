/*
 * RTP Depacketization of MP4A-LATM, RFC 3016
 * Copyright (c) 2010 Martin Storsjo
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

#include "avio_internal.h"
#include "rtpdec_formats.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavcodec/get_bits.h"

struct PayloadContext {
    AVIOContext *dyn_buf;
    uint8_t *buf;
    int pos, len;
    uint32_t timestamp;
};

static void latm_close_context(PayloadContext *data)
{
    ffio_free_dyn_buf(&data->dyn_buf);//释放AVIOContext
    av_freep(&data->buf);
}

static int latm_parse_packet(AVFormatContext *ctx, PayloadContext *data,//tiger rtp aac 计算rtp中实际的aac 音频数据
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    int ret, cur_len;
    //01.分配一个AVIOContext 读取buff
    if (buf) {
        if (!data->dyn_buf || data->timestamp != *timestamp) {//TIGER improvement 感觉用这个有点 折腾，没效率，不必要？
            av_freep(&data->buf);
            ffio_free_dyn_buf(&data->dyn_buf);

            data->timestamp = *timestamp;
            if ((ret = avio_open_dyn_buf(&data->dyn_buf)) < 0)
                return ret;
        }
        avio_write(data->dyn_buf, buf, len);//复制

        if (!(flags & RTP_FLAG_MARKER))
            return AVERROR(EAGAIN);
        av_freep(&data->buf);
        data->len = avio_close_dyn_buf(data->dyn_buf, &data->buf);
        data->dyn_buf = NULL;
        data->pos = 0;
    }

    if (!data->buf) {
        av_log(ctx, AV_LOG_ERROR, "No data available yet\n");
        return AVERROR(EIO);
    }
    //02. 计算长度
    cur_len = 0;
    while (data->pos < data->len) {//tiger 形如 0xff ff 38 或者0x 38
        uint8_t val = data->buf[data->pos++];//取
        cur_len += val;
        if (val != 0xff)
            break;//直到不是0xff，就是总长度了
    }
    if (data->pos + cur_len > data->len) {//验证长度，如果剩余长度比实际数据长，返回错误。
        av_log(ctx, AV_LOG_ERROR, "Malformed LATM packet\n");
        return AVERROR(EIO);
    }
    //03.复制,因为数据可以自定义长度，所以这里应该是一个AVPacket
    if ((ret = av_new_packet(pkt, cur_len)) < 0)//
        return ret;
    memcpy(pkt->data, data->buf + data->pos, cur_len);//复制
    data->pos += cur_len;
    pkt->stream_index = st->index;
    return data->pos < data->len;
}

static int parse_fmtp_config(AVStream *st, const char *value)//TIGER SDP FMTP  //TIGER AAC
{
    int len = ff_hex_to_data(NULL, value), i, ret = 0;//ff_hex_to_data 计算长度，以便分配内存
    GetBitContext gb;
    uint8_t *config;
    int audio_mux_version, same_time_framing, num_programs, num_layers;

    /* Pad this buffer, too, to avoid out of bounds reads with get_bits below */
    config = av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!config)
        return AVERROR(ENOMEM);
    ff_hex_to_data(config, value);//16进制的文本转化为内部的字符类型
    init_get_bits(&gb, config, len*8);//初始化一个GetBitContext 来读位bit
    audio_mux_version = get_bits(&gb, 1);
    same_time_framing = get_bits(&gb, 1);
    skip_bits(&gb, 6); /* num_sub_frames */
    num_programs      = get_bits(&gb, 4);
    num_layers        = get_bits(&gb, 3);
    if (audio_mux_version != 0 || same_time_framing != 1 || num_programs != 0 ||
        num_layers != 0) {
        avpriv_report_missing_feature(NULL, "LATM config (%d,%d,%d,%d)",
                                      audio_mux_version, same_time_framing,
                                      num_programs, num_layers);
        ret = AVERROR_PATCHWELCOME;
        goto end;
    }
    av_freep(&st->codecpar->extradata);
    if (ff_alloc_extradata(st->codecpar, (get_bits_left(&gb) + 7)/8)) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (i = 0; i < st->codecpar->extradata_size; i++)
        st->codecpar->extradata[i] = get_bits(&gb, 8);//将剩余的位放在extradata 中

end:
    av_free(config);
    return ret;
}

static int parse_fmtp(AVFormatContext *s,
                      AVStream *stream, PayloadContext *data,
                      const char *attr, const char *value)
{
    int res;

    if (!strcmp(attr, "config")) {
        res = parse_fmtp_config(stream, value);//TIGER TODO:疑问：是sdp的优先？还是inband 的优先？
        if (res < 0)
            return res;
    } else if (!strcmp(attr, "cpresent")) {
        int cpresent = atoi(value);
        if (cpresent != 0)
            avpriv_request_sample(s,//不支持
                                  "RTP MP4A-LATM with in-band configuration");
    }

    return 0;
}

static int latm_parse_sdp_line(AVFormatContext *s, int st_index,
                               PayloadContext *data, const char *line)
{
    const char *p;

    if (st_index < 0)
        return 0;

    if (av_strstart(line, "fmtp:", &p))
        return ff_parse_fmtp(s, s->streams[st_index], data, p, parse_fmtp);

    return 0;
}

const RTPDynamicProtocolHandler ff_mp4a_latm_dynamic_handler = {//TIGER AAC LATM
    .enc_name           = "MP4A-LATM",
    .codec_type         = AVMEDIA_TYPE_AUDIO,
    .codec_id           = AV_CODEC_ID_AAC,
    .priv_data_size     = sizeof(PayloadContext),
    .parse_sdp_a_line   = latm_parse_sdp_line,//TIGER RTP //TIGER SDP 
    .close              = latm_close_context,
    .parse_packet       = latm_parse_packet,//拆解rtp头，得到实际的aac数据
};
//AAC打包成TS流通常有两种方式，分别是先打包成ADTS或LATM。ADTS的每一帧都有个帧头，在每个帧头信息都一样的状况下，会有很大的冗余。LATM格式具有很大的灵活性，每帧的音频配置单元既可以带内传输，有可以带外传输。正因为如此，LATM不仅适用于流传输还可以用于RTP传输，RTP传输时，若音频数据配置信息是保持不变，可以先通过SDP会话先传输StreamMuxConfig（AudioSpecificConfig）信息，由于LATM流由一个包含了一个或多个音频帧的audioMuxElements序列组成。一个完整或部分完整的audioMuxElement可直接映射到一个RTP负载上。