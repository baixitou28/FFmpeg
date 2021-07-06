/*
 * MPEG-1/2 demuxer
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

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mpeg.h"

#if CONFIG_VOBSUB_DEMUXER
# include "subtitles.h"
# include "libavutil/bprint.h"
# include "libavutil/opt.h"
#endif

#include "libavutil/avassert.h"

/*********************************************/
/* demux code */

#define MAX_SYNC_SIZE 100000

static int check_pes(const uint8_t *p, const uint8_t *end)
{
    int pes1;
    int pes2 = (p[3] & 0xC0) == 0x80 &&
               (p[4] & 0xC0) != 0x40 &&
               ((p[4] & 0xC0) == 0x00 ||
                (p[4] & 0xC0) >> 2 == (p[6] & 0xF0));

    for (p += 3; p < end && *p == 0xFF; p++) ;
    if ((*p & 0xC0) == 0x40)
        p += 2;

    if ((*p & 0xF0) == 0x20)
        pes1 = p[0] & p[2] & p[4] & 1;
    else if ((*p & 0xF0) == 0x30)
        pes1 = p[0] & p[2] & p[4] & p[5] & p[7] & p[9] & 1;
    else
        pes1 = *p == 0x0F;

    return pes1 || pes2;
}

static int check_pack_header(const uint8_t *buf)
{
    return (buf[1] & 0xC0) == 0x40 || (buf[1] & 0xF0) == 0x20;
}

static int mpegps_probe(const AVProbeData *p)
{
    uint32_t code = -1;
    int i;
    int sys = 0, pspack = 0, priv1 = 0, vid = 0;
    int audio = 0, invalid = 0, score = 0;
    int endpes = 0;

    for (i = 0; i < p->buf_size; i++) {
        code = (code << 8) + p->buf[i];
        if ((code & 0xffffff00) == 0x100) {
            int len  = p->buf[i + 1] << 8 | p->buf[i + 2];
            int pes  = endpes <= i && check_pes(p->buf + i, p->buf + p->buf_size);
            int pack = check_pack_header(p->buf + i);

            if (code == SYSTEM_HEADER_START_CODE)
                sys++;
            else if (code == PACK_START_CODE && pack)
                pspack++;
            else if ((code & 0xf0) == VIDEO_ID && pes) {
                endpes = i + len;
                vid++;
            }
            // skip pes payload to avoid start code emulation for private
            // and audio streams
            else if ((code & 0xe0) == AUDIO_ID &&  pes) {audio++; i+=len;}
            else if (code == PRIVATE_STREAM_1  &&  pes) {priv1++; i+=len;}
            else if (code == 0x1fd             &&  pes) vid++; //VC1

            else if ((code & 0xf0) == VIDEO_ID && !pes) invalid++;
            else if ((code & 0xe0) == AUDIO_ID && !pes) invalid++;
            else if (code == PRIVATE_STREAM_1  && !pes) invalid++;
        }
    }

    if (vid + audio > invalid + 1) /* invalid VDR files nd short PES streams */
        score = AVPROBE_SCORE_EXTENSION / 2;

//     av_log(NULL, AV_LOG_ERROR, "vid:%d aud:%d sys:%d pspack:%d invalid:%d size:%d \n",
//            vid, audio, sys, pspack, invalid, p->buf_size);

    if (sys > invalid && sys * 9 <= pspack * 10)
        return (audio > 12 || vid > 3 || pspack > 2) ? AVPROBE_SCORE_EXTENSION + 2
                                                     : AVPROBE_SCORE_EXTENSION / 2 + 1; // 1 more than mp3
    if (pspack > invalid && (priv1 + vid + audio) * 10 >= pspack * 9)
        return pspack > 2 ? AVPROBE_SCORE_EXTENSION + 2
                          : AVPROBE_SCORE_EXTENSION / 2; // 1 more than .mpg
    if ((!!vid ^ !!audio) && (audio > 4 || vid > 1) && !sys &&
        !pspack && p->buf_size > 2048 && vid + audio > invalid) /* PES stream */
        return (audio > 12 || vid > 6 + 2 * invalid) ? AVPROBE_SCORE_EXTENSION + 2
                                                     : AVPROBE_SCORE_EXTENSION / 2;

    // 02-Penguin.flac has sys:0 priv1:0 pspack:0 vid:0 audio:1
    // mp3_misidentified_2.mp3 has sys:0 priv1:0 pspack:0 vid:0 audio:6
    // Have\ Yourself\ a\ Merry\ Little\ Christmas.mp3 0 0 0 5 0 1 len:21618
    return score;
}

typedef struct MpegDemuxContext {
    AVClass *class;
    int32_t header_state;
    unsigned char psm_es_type[256];
    int sofdec;
    int dvd;
    int imkh_cctv;
    int raw_ac3;
#if CONFIG_VOBSUB_DEMUXER
    AVFormatContext *sub_ctx;
    FFDemuxSubtitlesQueue q[32];
    char *sub_name;
#endif
} MpegDemuxContext;

static int mpegps_read_header(AVFormatContext *s)
{
    MpegDemuxContext *m = s->priv_data;
    char buffer[7] = { 0 };
    int64_t last_pos = avio_tell(s->pb);

    m->header_state = 0xff;
    s->ctx_flags   |= AVFMTCTX_NOHEADER;

    avio_get_str(s->pb, 6, buffer, sizeof(buffer));
    if (!memcmp("IMKH", buffer, 4)) {
        m->imkh_cctv = 1;
    } else if (!memcmp("Sofdec", buffer, 6)) {
        m->sofdec = 1;
    } else
       avio_seek(s->pb, last_pos, SEEK_SET);

    /* no need to do more */
    return 0;
}

static int64_t get_pts(AVIOContext *pb, int c)
{
    uint8_t buf[5];

    buf[0] = c < 0 ? avio_r8(pb) : c;
    avio_read(pb, buf + 1, 4);

    return ff_parse_pes_pts(buf);
}

static int find_next_start_code(AVIOContext *pb, int *size_ptr,//查找0x000001，返回的是0x000001左移+下一个字节
                                int32_t *header_state)
{
    unsigned int state, v;
    int val, n;

    state = *header_state;//mpegps_read_pes_header 中是0xff
    n     = *size_ptr;
    while (n > 0) {//最大循环次数到了吗？MAX_SYNC_SIZE=100000
        if (avio_feof(pb))
            break;
        v = avio_r8(pb);//读1个字节
        n--;
        if (state == 0x000001) {//类似H264，先赋值0XFF，再不停左移，直到找到0x000001
            state = ((state << 8) | v) & 0xffffff;
            val   = state;//往左移8位
            goto found;//找到了0x000001
        }
        state = ((state << 8) | v) & 0xffffff;//往左移8位
    }
    val = -1;

found:
    *header_state = state;
    *size_ptr     = n;//n是还剩多少
    return val;//返回的是0x000001左移+下一个字节
}

/**
 * Extract stream types from a program stream map
 * According to ISO/IEC 13818-1 ('MPEG-2 Systems') table 2-35
 *
 * @return number of bytes occupied by PSM in the bitstream
 */
static long mpegps_psm_parse(MpegDemuxContext *m, AVIOContext *pb)//解析psm，返回解析的总长度 字节实例分析参见 https://blog.csdn.net/weixin_44517656/article/details/108412988
{
    int psm_length, ps_info_length, es_map_length;

    psm_length = avio_rb16(pb);//psm长度，占2个字节
    avio_r8(pb);//跳过
    avio_r8(pb);
    ps_info_length = avio_rb16(pb);//ps_info长度

    /* skip program_stream_info */
    avio_skip(pb, ps_info_length);
    /*es_map_length = */avio_rb16(pb);
    /* Ignore es_map_length, trust psm_length */
    es_map_length = psm_length - ps_info_length - 10;

    /* at least one es available? */
    while (es_map_length >= 4) {
        unsigned char type      = avio_r8(pb);
        unsigned char es_id     = avio_r8(pb);//其中0x(C0~DF)指音频，0x(E0 ~ EF)为视频
        uint16_t es_info_length = avio_rb16(pb);

        /* remember mapping from stream id to stream type */
        m->psm_es_type[es_id] = type;//基本流所在PES分组的PES分组标题中stream_id字段的值。 一般是es_id对应的编码类型，后面mpegps_read_packet会用到
        /* skip program_stream_info */
        avio_skip(pb, es_info_length);
        es_map_length -= 4 + es_info_length;
    }
    avio_rb32(pb); /* crc32 *///这个偷懒不处理了？相信群众...
    return 2 + psm_length;//2是psm_length长度占用的字节，类似TLV中的LV结构
}

/* read the next PES header. Return its position in ppos
 * (if not NULL), and its start code, pts and dts.
 */
static int mpegps_read_pes_header(AVFormatContext *s,
                                  int64_t *ppos, int *pstart_code,
                                  int64_t *ppts, int64_t *pdts)
{
    MpegDemuxContext *m = s->priv_data;
    int len, size, startcode, c, flags, header_len;
    int pes_ext, ext2_len, id_ext, skip;
    int64_t pts, dts;
    int64_t last_sync = avio_tell(s->pb);

error_redo:
    avio_seek(s->pb, last_sync, SEEK_SET);//01.
redo:
    /* next start code (should be immediately after) */
    m->header_state = 0xff;
    size      = MAX_SYNC_SIZE;
    startcode = find_next_start_code(s->pb, &size, &m->header_state);//01.查找0x000001，返回0x000001左移+下一个字节,返回下一个字节，代码比较简单
    last_sync = avio_tell(s->pb);//同步当前位置
    if (startcode < 0) {
        if (avio_feof(s->pb))
            return AVERROR_EOF;
        // FIXME we should remember header_state
        return FFERROR_REDO;
    }
    //01. 各种跳过
    if (startcode == PACK_START_CODE)//BA  对于海康可能有问题  avio_skip(s->pb,9);avio_skip(s->pb,avio_r8(s->pb)&0x7);goto redo; //https://blog.csdn.net/andyshengjl/article/details/79319195
        goto redo;//参见一般跳过ba头的方法是：4+6+3+1+补充字节长度，https://blog.csdn.net/weixin_44517656/article/details/108412988
    if (startcode == SYSTEM_HEADER_START_CODE)//BB 
        goto redo;//这里是不是也是跳过比较安全？ https://blog.csdn.net/weixin_44517656/article/details/108412988
    if (startcode == PADDING_STREAM) {//BE
        avio_skip(s->pb, avio_rb16(s->pb));
        goto redo;
    }
    if (startcode == PRIVATE_STREAM_2) {//02. 私有bf
        if (!m->sofdec) {
            /* Need to detect whether this from a DVD or a 'Sofdec' stream */
            int len = avio_rb16(s->pb);
            int bytesread = 0;
            uint8_t *ps2buf = av_malloc(len);

            if (ps2buf) {
                bytesread = avio_read(s->pb, ps2buf, len);

                if (bytesread != len) {
                    avio_skip(s->pb, len - bytesread);
                } else {
                    uint8_t *p = 0;
                    if (len >= 6)
                        p = memchr(ps2buf, 'S', len - 5);

                    if (p)
                        m->sofdec = !memcmp(p+1, "ofdec", 5);

                    m->sofdec -= !m->sofdec;

                    if (m->sofdec < 0) {
                        if (len == 980  && ps2buf[0] == 0) {
                            /* PCI structure? */
                            uint32_t startpts = AV_RB32(ps2buf + 0x0d);
                            uint32_t endpts = AV_RB32(ps2buf + 0x11);
                            uint8_t hours = ((ps2buf[0x19] >> 4) * 10) + (ps2buf[0x19] & 0x0f);
                            uint8_t mins  = ((ps2buf[0x1a] >> 4) * 10) + (ps2buf[0x1a] & 0x0f);
                            uint8_t secs  = ((ps2buf[0x1b] >> 4) * 10) + (ps2buf[0x1b] & 0x0f);

                            m->dvd = (hours <= 23 &&
                                      mins  <= 59 &&
                                      secs  <= 59 &&
                                      (ps2buf[0x19] & 0x0f) < 10 &&
                                      (ps2buf[0x1a] & 0x0f) < 10 &&
                                      (ps2buf[0x1b] & 0x0f) < 10 &&
                                      endpts >= startpts);
                        } else if (len == 1018 && ps2buf[0] == 1) {
                            /* DSI structure? */
                            uint8_t hours = ((ps2buf[0x1d] >> 4) * 10) + (ps2buf[0x1d] & 0x0f);
                            uint8_t mins  = ((ps2buf[0x1e] >> 4) * 10) + (ps2buf[0x1e] & 0x0f);
                            uint8_t secs  = ((ps2buf[0x1f] >> 4) * 10) + (ps2buf[0x1f] & 0x0f);

                            m->dvd = (hours <= 23 &&
                                      mins  <= 59 &&
                                      secs  <= 59 &&
                                      (ps2buf[0x1d] & 0x0f) < 10 &&
                                      (ps2buf[0x1e] & 0x0f) < 10 &&
                                      (ps2buf[0x1f] & 0x0f) < 10);
                        }
                    }
                }

                av_free(ps2buf);

                /* If this isn't a DVD packet or no memory
                 * could be allocated, just ignore it.
                 * If we did, move back to the start of the
                 * packet (plus 'length' field) */
                if (!m->dvd || avio_skip(s->pb, -(len + 2)) < 0) {
                    /* Skip back failed.
                     * This packet will be lost but that can't be helped
                     * if we can't skip back
                     */
                    goto redo;
                }
            } else {
                /* No memory */
                avio_skip(s->pb, len);
                goto redo;
            }
        } else if (!m->dvd) {
            int len = avio_rb16(s->pb);
            avio_skip(s->pb, len);
            goto redo;
        }
    }
    if (startcode == PROGRAM_STREAM_MAP) {//03. 解析psm 字节实例分析参见 https://blog.csdn.net/weixin_44517656/article/details/108412988
        mpegps_psm_parse(m, s->pb);//重要函数psm
        goto redo;
    }

    /* find matching stream */
    if (!((startcode >= 0x1c0 && startcode <= 0x1df) ||//04.0x(C0~DF)指音频，0x(E0~EF)为视频。
          (startcode >= 0x1e0 && startcode <= 0x1ef) ||
          (startcode == 0x1bd) ||//为什么不写PRIVATE_STREAM_1
          (startcode == PRIVATE_STREAM_2) ||
          (startcode == 0x1fd)))//这个是？
        goto redo;
    if (ppos) {//05.
        *ppos = avio_tell(s->pb) - 4;
    }
    len = avio_rb16(s->pb);
    pts =
    dts = AV_NOPTS_VALUE;//初始值设为一个特殊值
    if (startcode != PRIVATE_STREAM_2)//06.
    {
    /* stuffing */
    for (;;) {//07. 非0xff开始  以下部分不理解：
        if (len < 1)
            goto error_redo;
        c = avio_r8(s->pb);//这里有很多标志位
        len--;
        /* XXX: for MPEG-1, should test only bit 7 */
        if (c != 0xff)
            break;
    }
    if ((c & 0xc0) == 0x40) {//08.0b1100,0000 == 0b0100,0000
        /* buffer scale & size */
        avio_r8(s->pb);//??P-STD缓冲区比例字段 P-STD_ buffer_scale
        c    = avio_r8(s->pb);//??P-STD缓冲区大小字段 P-STD_buffer_size
        len -= 2;
    }
    if ((c & 0xe0) == 0x20) {//09.0b1110,0000 //???当值为'10'时，PTS字段应出现在PES分组标题中；当值为'11'时，PTS字段和DTS字段都应出现在PES分组标题中；当值为'00'时，PTS字段和DTS字段都不出现在PES分组标题中。值'01'是不允许的。
        dts  =
        pts  = get_pts(s->pb, c);//获取dts //???当值为'11'时，PTS字段和DTS字段都应出现在PES分组标题中
        len -= 4;
        if (c & 0x10) {//??当值为'10'时，PTS字段应出现在PES分组标题中
            dts  = get_pts(s->pb, -1);
            len -= 5;
        }
    } else if ((c & 0xc0) == 0x80) {//10.
        /* mpeg 2 PES */
        flags      = avio_r8(s->pb);//
        header_len = avio_r8(s->pb);
        len       -= 2;
        if (header_len > len)
            goto error_redo;
        len -= header_len;
        if (flags & 0x80) {
            dts         = pts = get_pts(s->pb, -1);
            header_len -= 5;
            if (flags & 0x40) {
                dts         = get_pts(s->pb, -1);
                header_len -= 5;
            }
        }
        if (flags & 0x3f && header_len == 0) {
            flags &= 0xC0;
            av_log(s, AV_LOG_WARNING, "Further flags set but no bytes left\n");
        }
        if (flags & 0x01) { /* PES extension */
            pes_ext = avio_r8(s->pb);
            header_len--;
            /* Skip PES private data, program packet sequence counter
             * and P-STD buffer */
            skip  = (pes_ext >> 4) & 0xb;
            skip += skip & 0x9;
            if (pes_ext & 0x40 || skip > header_len) {
                av_log(s, AV_LOG_WARNING, "pes_ext %X is invalid\n", pes_ext);
                pes_ext = skip = 0;
            }
            avio_skip(s->pb, skip);
            header_len -= skip;

            if (pes_ext & 0x01) { /* PES extension 2 */
                ext2_len = avio_r8(s->pb);
                header_len--;
                if ((ext2_len & 0x7f) > 0) {
                    id_ext = avio_r8(s->pb);
                    if ((id_ext & 0x80) == 0)
                        startcode = ((startcode & 0xff) << 8) | id_ext;
                    header_len--;
                }
            }
        }
        if (header_len < 0)
            goto error_redo;
        avio_skip(s->pb, header_len);
    } else if (c != 0xf)
        goto redo;
    }

    if (startcode == PRIVATE_STREAM_1) {//11.
        int ret = ffio_ensure_seekback(s->pb, 2);

        if (ret < 0)
            return ret;

        startcode = avio_r8(s->pb);
        m->raw_ac3 = 0;
        if (startcode == 0x0b) {
            if (avio_r8(s->pb) == 0x77) {
                startcode = 0x80;
                m->raw_ac3 = 1;
                avio_skip(s->pb, -2);
            } else {
                avio_skip(s->pb, -1);
            }
        } else {
            len--;
        }
    }
    if (len < 0)//12.异常
        goto error_redo;
    if (dts != AV_NOPTS_VALUE && ppos) {//13.如果解析出dts，加入st->index_entries索引中
        int i;
        for (i = 0; i < s->nb_streams; i++) {
            if (startcode == s->streams[i]->id &&
                (s->pb->seekable & AVIO_SEEKABLE_NORMAL) /* index useless on streams anyway */) {
                ff_reduce_index(s, i);
                av_add_index_entry(s->streams[i], *ppos, dts, 0, 0,
                                   AVINDEX_KEYFRAME /* FIXME keyframe? */);
            }
        }
    }

    *pstart_code = startcode;
    *ppts        = pts;//返回解析后的pts
    *pdts        = dts;//返回解析后的dts
    return len;
}
//TIGER 参考 FFMPEG之海康实时回调出来的PS流格式  https://blog.csdn.net/weixin_44517656/article/details/108412988
static int mpegps_read_packet(AVFormatContext *s,
                              AVPacket *pkt)
{
    MpegDemuxContext *m = s->priv_data;
    AVStream *st;
    int len, startcode, i, es_type, ret;
    int lpcm_header_len = -1; //Init to suppress warning
    int request_probe= 0;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    enum AVMediaType type;
    int64_t pts, dts, dummy_pos; // dummy_pos is needed for the index building to work

redo://不停查找
    len = mpegps_read_pes_header(s, &dummy_pos, &startcode, &pts, &dts);//01. 主函数，比较难理解
    if (len < 0)
        return len;//返回失败原因

    if (startcode >= 0x80 && startcode <= 0xcf) {//02.
        if (len < 4)
            goto skip;//跳过异常部分

        if (!m->raw_ac3) {//02.01
            /* audio: skip header */
            avio_r8(s->pb);
            lpcm_header_len = avio_rb16(s->pb);//02.02
            len -= 3;//跳过3个字节
            if (startcode >= 0xb0 && startcode <= 0xbf) {//02.03 特殊处理
                /* MLP/TrueHD audio has a 4-byte header */
                avio_r8(s->pb);
                len--;
            }
        }
    }

    /* now find stream */
    for (i = 0; i < s->nb_streams; i++) {//03.
        st = s->streams[i];
        if (st->id == startcode)//03.01 前提是startcode 是匹配的，这个是代码里约定的
            goto found;
    }

    es_type = m->psm_es_type[startcode & 0xff];//04. 类型是startcode 后2位， m->psm_es_type是mpegps_read_pes_headerD的mpegps_psm_parse中预先解析好的
    if (es_type == STREAM_TYPE_VIDEO_MPEG1) {
        codec_id = AV_CODEC_ID_MPEG2VIDEO;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_VIDEO_MPEG2) {
        codec_id = AV_CODEC_ID_MPEG2VIDEO;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_AUDIO_MPEG1 ||
               es_type == STREAM_TYPE_AUDIO_MPEG2) {
        codec_id = AV_CODEC_ID_MP3;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (es_type == STREAM_TYPE_AUDIO_AAC) {//
        codec_id = AV_CODEC_ID_AAC;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (es_type == STREAM_TYPE_VIDEO_MPEG4) {
        codec_id = AV_CODEC_ID_MPEG4;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_VIDEO_H264) {//解码类型 H264
        codec_id = AV_CODEC_ID_H264;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_VIDEO_HEVC) {//H265
        codec_id = AV_CODEC_ID_HEVC;
        type     = AVMEDIA_TYPE_VIDEO;
    } else if (es_type == STREAM_TYPE_AUDIO_AC3) {
        codec_id = AV_CODEC_ID_AC3;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (m->imkh_cctv && es_type == 0x91) {//为什么这里写91？ ulaw //https://blog.csdn.net/garefield/article/details/45113313
        codec_id = AV_CODEC_ID_PCM_MULAW;
        type     = AVMEDIA_TYPE_AUDIO;
    } else if (startcode >= 0x1e0 && startcode <= 0x1ef) {//私有视频
        static const unsigned char avs_seqh[4] = { 0, 0, 1, 0xb0 };
        unsigned char buf[8];

        avio_read(s->pb, buf, 8);//先读
        avio_seek(s->pb, -8, SEEK_CUR);//
        if (!memcmp(buf, avs_seqh, 4) && (buf[6] != 0 || buf[7] != 1))
            codec_id = AV_CODEC_ID_CAVS;
        else
            request_probe= 1;
        type = AVMEDIA_TYPE_VIDEO;
    } else if (startcode == PRIVATE_STREAM_2) {
        type = AVMEDIA_TYPE_DATA;
        codec_id = AV_CODEC_ID_DVD_NAV;
    } else if (startcode >= 0x1c0 && startcode <= 0x1df) {//自定义音频，音频也分多种
        type     = AVMEDIA_TYPE_AUDIO;
        if (m->sofdec > 0) {
            codec_id = AV_CODEC_ID_ADPCM_ADX;
            // Auto-detect AC-3
            request_probe = 50;
        } else if (m->imkh_cctv && startcode == 0x1c0 && len > 80) {//海康里面用alaw G.711 音频流： 0x90 //https://blog.csdn.net/weixin_44517656/article/details/108412988
            codec_id = AV_CODEC_ID_PCM_ALAW;
            request_probe = 50;//忘记含义了==>
        } else {
            codec_id = AV_CODEC_ID_MP2;
            if (m->imkh_cctv)
                request_probe = 25;
        }
    } else if (startcode >= 0x80 && startcode <= 0x87) {//自定义音频
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_AC3;
    } else if ((startcode >= 0x88 && startcode <= 0x8f) ||
               (startcode >= 0x98 && startcode <= 0x9f)) {//自定义音频
        /* 0x90 - 0x97 is reserved for SDDS in DVD specs */
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_DTS;
    } else if (startcode >= 0xa0 && startcode <= 0xaf) {
        type     = AVMEDIA_TYPE_AUDIO;
        if (lpcm_header_len >= 6 && startcode == 0xa1) {
            codec_id = AV_CODEC_ID_MLP;
        } else {
            codec_id = AV_CODEC_ID_PCM_DVD;
        }
    } else if (startcode >= 0xb0 && startcode <= 0xbf) {
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_TRUEHD;
    } else if (startcode >= 0xc0 && startcode <= 0xcf) {
        /* Used for both AC-3 and E-AC-3 in EVOB files */
        type     = AVMEDIA_TYPE_AUDIO;
        codec_id = AV_CODEC_ID_AC3;
    } else if (startcode >= 0x20 && startcode <= 0x3f) {//字幕
        type     = AVMEDIA_TYPE_SUBTITLE;
        codec_id = AV_CODEC_ID_DVD_SUBTITLE;
    } else if (startcode >= 0xfd55 && startcode <= 0xfd5f) {
        type     = AVMEDIA_TYPE_VIDEO;
        codec_id = AV_CODEC_ID_VC1;
    } else {
skip://一般是跳过异常len部分
        /* skip packet */
        avio_skip(s->pb, len);//05.因为startcode 不是意料之中的，无法分析出编码格式，只能跳过len
        goto redo;
    }
    /* no stream found: add a new stream */
    st = avformat_new_stream(s, NULL);//06.创建
    if (!st)
        goto skip;
    st->id                = startcode;
    st->codecpar->codec_type = type;
    st->codecpar->codec_id   = codec_id;
    if (   st->codecpar->codec_id == AV_CODEC_ID_PCM_MULAW
        || st->codecpar->codec_id == AV_CODEC_ID_PCM_ALAW) {//如果是pcm，将默认值设置好
        st->codecpar->channels = 1;
        st->codecpar->channel_layout = AV_CH_LAYOUT_MONO;
        st->codecpar->sample_rate = 8000;
    }
    st->request_probe     = request_probe;
    st->need_parsing      = AVSTREAM_PARSE_FULL;

found://直接找到流
    if (st->discard >= AVDISCARD_ALL)
        goto skip;
    if (startcode >= 0xa0 && startcode <= 0xaf) {
      if (st->codecpar->codec_id == AV_CODEC_ID_MLP) {//没见过的这种编码
            if (len < 6)
                goto skip;
            avio_skip(s->pb, 6);
            len -=6;
      }
    }
    ret = av_get_packet(s->pb, pkt, len);//08.终于可以真正读取音视频的数据

    pkt->pts          = pts;
    pkt->dts          = dts;
    pkt->pos          = dummy_pos;
    pkt->stream_index = st->index;

    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG, "%d: pts=%0.3f dts=%0.3f size=%d\n",
            pkt->stream_index, pkt->pts / 90000.0, pkt->dts / 90000.0,
            pkt->size);

    return (ret < 0) ? ret : 0;//返回最后一次读取的状态
}

static int64_t mpegps_read_dts(AVFormatContext *s, int stream_index,
                               int64_t *ppos, int64_t pos_limit)
{
    int len, startcode;
    int64_t pos, pts, dts;

    pos = *ppos;
    if (avio_seek(s->pb, pos, SEEK_SET) < 0)//跳到pos位置
        return AV_NOPTS_VALUE;

    for (;;) {
        len = mpegps_read_pes_header(s, &pos, &startcode, &pts, &dts);//01. 只分析头，实际位置未改变
        if (len < 0) {
            if (s->debug & FF_FDEBUG_TS)
                av_log(s, AV_LOG_DEBUG, "none (ret=%d)\n", len);
            return AV_NOPTS_VALUE;
        }
        if (startcode == s->streams[stream_index]->id &&//有流找到了，且dts也找到了，就可以停止循环了。
            dts != AV_NOPTS_VALUE) {
            break;
        }
        avio_skip(s->pb, len);//跳过已解析的长度，再次查找
    }
    if (s->debug & FF_FDEBUG_TS)
        av_log(s, AV_LOG_DEBUG, "pos=0x%"PRIx64" dts=0x%"PRIx64" %0.3f\n",
            pos, dts, dts / 90000.0);
    *ppos = pos;//跳到实际的位置
    return dts;
}
//TIGER 20210705 这个一定得熟悉，海康都是PS流 //参看实际的字节实例分析https://blog.csdn.net/weixin_44517656/article/details/108412988
AVInputFormat ff_mpegps_demuxer = {
    .name           = "mpeg",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-PS (MPEG-2 Program Stream)"),
    .priv_data_size = sizeof(MpegDemuxContext),
    .read_probe     = mpegps_probe,
    .read_header    = mpegps_read_header,
    .read_packet    = mpegps_read_packet,
    .read_timestamp = mpegps_read_dts,
    .flags          = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT,
};

#if CONFIG_VOBSUB_DEMUXER

#define REF_STRING "# VobSub index file,"
#define MAX_LINE_SIZE 2048

static int vobsub_probe(const AVProbeData *p)
{
    if (!strncmp(p->buf, REF_STRING, sizeof(REF_STRING) - 1))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int vobsub_read_header(AVFormatContext *s)
{
    int i, ret = 0, header_parsed = 0, langidx = 0;
    MpegDemuxContext *vobsub = s->priv_data;
    size_t fname_len;
    char *header_str;
    AVBPrint header;
    int64_t delay = 0;
    AVStream *st = NULL;
    int stream_id = -1;
    char id[64] = {0};
    char alt[MAX_LINE_SIZE] = {0};
    ff_const59 AVInputFormat *iformat;

    if (!vobsub->sub_name) {
        char *ext;
        vobsub->sub_name = av_strdup(s->url);
        if (!vobsub->sub_name) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        fname_len = strlen(vobsub->sub_name);
        ext = vobsub->sub_name - 3 + fname_len;
        if (fname_len < 4 || *(ext - 1) != '.') {
            av_log(s, AV_LOG_ERROR, "The input index filename is too short "
                   "to guess the associated .SUB file\n");
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
        memcpy(ext, !strncmp(ext, "IDX", 3) ? "SUB" : "sub", 3);
        av_log(s, AV_LOG_VERBOSE, "IDX/SUB: %s -> %s\n", s->url, vobsub->sub_name);
    }

    if (!(iformat = av_find_input_format("mpeg"))) {
        ret = AVERROR_DEMUXER_NOT_FOUND;
        goto end;
    }

    vobsub->sub_ctx = avformat_alloc_context();
    if (!vobsub->sub_ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = ff_copy_whiteblacklists(vobsub->sub_ctx, s)) < 0)
        goto end;

    ret = avformat_open_input(&vobsub->sub_ctx, vobsub->sub_name, iformat, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Unable to open %s as MPEG subtitles\n", vobsub->sub_name);
        goto end;
    }

    av_bprint_init(&header, 0, AV_BPRINT_SIZE_UNLIMITED);
    while (!avio_feof(s->pb)) {
        char line[MAX_LINE_SIZE];
        int len = ff_get_line(s->pb, line, sizeof(line));

        if (!len)
            break;

        line[strcspn(line, "\r\n")] = 0;

        if (!strncmp(line, "id:", 3)) {
            if (sscanf(line, "id: %63[^,], index: %u", id, &stream_id) != 2) {
                av_log(s, AV_LOG_WARNING, "Unable to parse index line '%s', "
                       "assuming 'id: und, index: 0'\n", line);
                strcpy(id, "und");
                stream_id = 0;
            }

            if (stream_id >= FF_ARRAY_ELEMS(vobsub->q)) {
                av_log(s, AV_LOG_ERROR, "Maximum number of subtitles streams reached\n");
                ret = AVERROR(EINVAL);
                goto end;
            }

            header_parsed = 1;
            alt[0] = '\0';
            /* We do not create the stream immediately to avoid adding empty
             * streams. See the following timestamp entry. */

            av_log(s, AV_LOG_DEBUG, "IDX stream[%d] id=%s\n", stream_id, id);

        } else if (!strncmp(line, "timestamp:", 10)) {
            AVPacket *sub;
            int hh, mm, ss, ms;
            int64_t pos, timestamp;
            const char *p = line + 10;

            if (stream_id == -1) {
                av_log(s, AV_LOG_ERROR, "Timestamp declared before any stream\n");
                ret = AVERROR_INVALIDDATA;
                goto end;
            }

            if (!st || st->id != stream_id) {
                st = avformat_new_stream(s, NULL);
                if (!st) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }
                st->id = stream_id;
                st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
                st->codecpar->codec_id   = AV_CODEC_ID_DVD_SUBTITLE;
                avpriv_set_pts_info(st, 64, 1, 1000);
                av_dict_set(&st->metadata, "language", id, 0);
                if (alt[0])
                    av_dict_set(&st->metadata, "title", alt, 0);
            }

            if (sscanf(p, "%02d:%02d:%02d:%03d, filepos: %"SCNx64,
                       &hh, &mm, &ss, &ms, &pos) != 5) {
                av_log(s, AV_LOG_ERROR, "Unable to parse timestamp line '%s', "
                       "abort parsing\n", line);
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            timestamp = (hh*3600LL + mm*60LL + ss) * 1000LL + ms + delay;
            timestamp = av_rescale_q(timestamp, av_make_q(1, 1000), st->time_base);

            sub = ff_subtitles_queue_insert(&vobsub->q[s->nb_streams - 1], "", 0, 0);
            if (!sub) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            sub->pos = pos;
            sub->pts = timestamp;
            sub->stream_index = s->nb_streams - 1;

        } else if (!strncmp(line, "alt:", 4)) {
            const char *p = line + 4;

            while (*p == ' ')
                p++;
            av_log(s, AV_LOG_DEBUG, "IDX stream[%d] name=%s\n", stream_id, p);
            av_strlcpy(alt, p, sizeof(alt));
            header_parsed = 1;

        } else if (!strncmp(line, "delay:", 6)) {
            int sign = 1, hh = 0, mm = 0, ss = 0, ms = 0;
            const char *p = line + 6;

            while (*p == ' ')
                p++;
            if (*p == '-' || *p == '+') {
                sign = *p == '-' ? -1 : 1;
                p++;
            }
            sscanf(p, "%d:%d:%d:%d", &hh, &mm, &ss, &ms);
            delay = ((hh*3600LL + mm*60LL + ss) * 1000LL + ms) * sign;

        } else if (!strncmp(line, "langidx:", 8)) {
            const char *p = line + 8;

            if (sscanf(p, "%d", &langidx) != 1)
                av_log(s, AV_LOG_ERROR, "Invalid langidx specified\n");

        } else if (!header_parsed) {
            if (line[0] && line[0] != '#')
                av_bprintf(&header, "%s\n", line);
        }
    }

    if (langidx < s->nb_streams)
        s->streams[langidx]->disposition |= AV_DISPOSITION_DEFAULT;

    for (i = 0; i < s->nb_streams; i++) {
        vobsub->q[i].sort = SUB_SORT_POS_TS;
        vobsub->q[i].keep_duplicates = 1;
        ff_subtitles_queue_finalize(s, &vobsub->q[i]);
    }

    if (!av_bprint_is_complete(&header)) {
        av_bprint_finalize(&header, NULL);
        ret = AVERROR(ENOMEM);
        goto end;
    }
    av_bprint_finalize(&header, &header_str);
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *sub_st = s->streams[i];
        sub_st->codecpar->extradata      = av_strdup(header_str);
        sub_st->codecpar->extradata_size = header.len;
    }
    av_free(header_str);

end:
    return ret;
}

static int vobsub_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MpegDemuxContext *vobsub = s->priv_data;
    FFDemuxSubtitlesQueue *q;
    AVIOContext *pb = vobsub->sub_ctx->pb;
    int ret, psize, total_read = 0, i;
    AVPacket idx_pkt = { 0 };

    int64_t min_ts = INT64_MAX;
    int sid = 0;
    for (i = 0; i < s->nb_streams; i++) {
        FFDemuxSubtitlesQueue *tmpq = &vobsub->q[i];
        int64_t ts;
        av_assert0(tmpq->nb_subs);
        ts = tmpq->subs[tmpq->current_sub_idx].pts;
        if (ts < min_ts) {
            min_ts = ts;
            sid = i;
        }
    }
    q = &vobsub->q[sid];
    ret = ff_subtitles_queue_read_packet(q, &idx_pkt);
    if (ret < 0)
        return ret;

    /* compute maximum packet size using the next packet position. This is
     * useful when the len in the header is non-sense */
    if (q->current_sub_idx < q->nb_subs) {
        psize = q->subs[q->current_sub_idx].pos - idx_pkt.pos;
    } else {
        int64_t fsize = avio_size(pb);
        psize = fsize < 0 ? 0xffff : fsize - idx_pkt.pos;
    }

    avio_seek(pb, idx_pkt.pos, SEEK_SET);

    av_init_packet(pkt);
    pkt->size = 0;
    pkt->data = NULL;

    do {
        int n, to_read, startcode;
        int64_t pts, dts;
        int64_t old_pos = avio_tell(pb), new_pos;
        int pkt_size;

        ret = mpegps_read_pes_header(vobsub->sub_ctx, NULL, &startcode, &pts, &dts);
        if (ret < 0) {
            if (pkt->size) // raise packet even if incomplete
                break;
            goto fail;
        }
        to_read = ret & 0xffff;
        new_pos = avio_tell(pb);
        pkt_size = ret + (new_pos - old_pos);

        /* this prevents reads above the current packet */
        if (total_read + pkt_size > psize)
            break;
        total_read += pkt_size;

        /* the current chunk doesn't match the stream index (unlikely) */
        if ((startcode & 0x1f) != s->streams[idx_pkt.stream_index]->id)
            break;

        ret = av_grow_packet(pkt, to_read);
        if (ret < 0)
            goto fail;

        n = avio_read(pb, pkt->data + (pkt->size - to_read), to_read);
        if (n < to_read)
            pkt->size -= to_read - n;
    } while (total_read < psize);

    pkt->pts = pkt->dts = idx_pkt.pts;
    pkt->pos = idx_pkt.pos;
    pkt->stream_index = idx_pkt.stream_index;

    av_packet_unref(&idx_pkt);
    return 0;

fail:
    av_packet_unref(pkt);
    av_packet_unref(&idx_pkt);
    return ret;
}

static int vobsub_read_seek(AVFormatContext *s, int stream_index,
                            int64_t min_ts, int64_t ts, int64_t max_ts, int flags)
{
    MpegDemuxContext *vobsub = s->priv_data;

    /* Rescale requested timestamps based on the first stream (timebase is the
     * same for all subtitles stream within a .idx/.sub). Rescaling is done just
     * like in avformat_seek_file(). */
    if (stream_index == -1 && s->nb_streams != 1) {
        int i, ret = 0;
        AVRational time_base = s->streams[0]->time_base;
        ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);
        min_ts = av_rescale_rnd(min_ts, time_base.den,
                                time_base.num * (int64_t)AV_TIME_BASE,
                                AV_ROUND_UP   | AV_ROUND_PASS_MINMAX);
        max_ts = av_rescale_rnd(max_ts, time_base.den,
                                time_base.num * (int64_t)AV_TIME_BASE,
                                AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX);
        for (i = 0; i < s->nb_streams; i++) {
            int r = ff_subtitles_queue_seek(&vobsub->q[i], s, stream_index,
                                            min_ts, ts, max_ts, flags);
            if (r < 0)
                ret = r;
        }
        return ret;
    }

    if (stream_index == -1) // only 1 stream
        stream_index = 0;
    return ff_subtitles_queue_seek(&vobsub->q[stream_index], s, stream_index,
                                   min_ts, ts, max_ts, flags);
}

static int vobsub_read_close(AVFormatContext *s)
{
    int i;
    MpegDemuxContext *vobsub = s->priv_data;

    for (i = 0; i < s->nb_streams; i++)
        ff_subtitles_queue_clean(&vobsub->q[i]);
    if (vobsub->sub_ctx)
        avformat_close_input(&vobsub->sub_ctx);
    return 0;
}

static const AVOption options[] = {
    { "sub_name", "URI for .sub file", offsetof(MpegDemuxContext, sub_name), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass vobsub_demuxer_class = {
    .class_name = "vobsub",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_vobsub_demuxer = {
    .name           = "vobsub",
    .long_name      = NULL_IF_CONFIG_SMALL("VobSub subtitle format"),
    .priv_data_size = sizeof(MpegDemuxContext),
    .read_probe     = vobsub_probe,
    .read_header    = vobsub_read_header,
    .read_packet    = vobsub_read_packet,
    .read_seek2     = vobsub_read_seek,
    .read_close     = vobsub_read_close,
    .flags          = AVFMT_SHOW_IDS,
    .extensions     = "idx",
    .priv_class     = &vobsub_demuxer_class,
};
#endif
