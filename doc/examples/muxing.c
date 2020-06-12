/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * libavformat API example.
 *
 * Output a media file in any supported libavformat format. The default
 * codecs are used.
 * @example muxing.c
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
//不需要使用常规的文件输入，模拟音频数据open_audio产生某个频率的声音， 模拟视频fill_yuv_image数据，写入用户指定的文件，格式根据文件后缀来猜测
#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

// a wrapper around a single output AVStream
typedef struct OutputStream {//使用看注释，本程序自义定的一个结构，是AVStream的一个包装
    AVStream *st;
    AVCodecContext *enc;//编码context

    /* pts of the next frame that will be generated */
    int64_t next_pts;
    int samples_count;

    AVFrame *frame;
    AVFrame *tmp_frame;//临时帧，用于产生音频帧用

    float t, tincr, tincr2;//频率，每次增加频率

    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;
} OutputStream;

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)//打印日志
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),//tiger program 打印时间戳函数
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)//tiger program 更新时间，流的id后才能写
{
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, *time_base, st->time_base);//01. TIGER PROGRAM  时间转化从packet的AVFormatContext时间转化AVStream 流的时间
    pkt->stream_index = st->index;//02. 更新id

    /* Write the compressed frame to the media file. */
    log_packet(fmt_ctx, pkt);//打印调试
    return av_interleaved_write_frame(fmt_ctx, pkt);//写
}

/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,//加入一个流
                       AVCodec **codec,
                       enum AVCodecID codec_id)
{
    AVCodecContext *c;
    int i;
    //01.找到编码器
    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n",
                avcodec_get_name(codec_id));
        exit(1);
    }
    //02. 分配一个AVStream实例
    ost->st = avformat_new_stream(oc, NULL);//TIGER PROGRAM avcodec_find_encoder/avformat_new_stream/avcodec_alloc_context3
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;//02. 01 流的id，是stream和ost相互指向
    c = avcodec_alloc_context3(*codec);//03.用编码器创建AVCodecContext
    if (!c) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;//赋值
    //设置AVCodecContext的参数，同时记得设置AVStream的时间基准
    switch ((*codec)->type) {//最常用的一些设置
    case AVMEDIA_TYPE_AUDIO://03.01. TIGER PROGRAM
        c->sample_fmt  = (*codec)->sample_fmts ?//03.01.01 设置采样位数
            (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;//优先采用缺省的第一个sample_fmts[0]
        c->bit_rate    = 64000;//比特率
        c->sample_rate = 44100;//使用采样率，CD音质
        if ((*codec)->supported_samplerates) {//参看ff_pcm_dvd_encoder  包含supported_samplerates，channel_layouts
            c->sample_rate = (*codec)->supported_samplerates[0];//优先采用第一个  //支持多个channel，一般都是aac，dvd等用于家庭影院的高级设备
            for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                if ((*codec)->supported_samplerates[i] == 44100)//如果支持的采样率有44100，优先采用44100
                    c->sample_rate = 44100;//tiger improvement 加break
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
        c->channel_layout = AV_CH_LAYOUT_STEREO;//强行指定立体声
        if ((*codec)->channel_layouts) {//如果codec 自带channel_layouts 如：aac aac_channel_layout
            c->channel_layout = (*codec)->channel_layouts[0];//优先采用第一个
            for (i = 0; (*codec)->channel_layouts[i]; i++) {
                if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                    c->channel_layout = AV_CH_LAYOUT_STEREO;//tiger improvement 加break， //如果支持立体声，优先采用立体声，一般都有
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);//重新设定
        ost->st->time_base = (AVRational){ 1, c->sample_rate };//03.01.02用AVCodecContext的采样率，来设置流的时间， 用AVCodecContext的采样率根据前面多个步骤获取到的
        break;

    case AVMEDIA_TYPE_VIDEO://03.02tiger program
        c->codec_id = codec_id;//03.02.01 设置context，特别注意：如果是视频还要重新设置codec_id,额外设置，vs 音频

        c->bit_rate = 400000;//比特率最好设置一下
        /* Resolution must be a multiple of two. */
        c->width    = 352;//看注释，必须是偶数
        c->height   = 288;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };//03.02.02 设置流的时间单位  一般是1/25
        c->time_base       = ost->st->time_base;//额外设置：AVCodecContext时间单位也设置为 1/25：对比音频 

        c->gop_size      = 12; /* emit one intra frame every twelve frames at most */ //这个需要设置吗？ //tiger program
        c->pix_fmt       = STREAM_PIX_FMT;//一般还需要设置这个
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            /* just for testing, we also add B-frames */
            c->max_b_frames = 2;//是否设置b帧
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            c->mb_decision = 2;//chrome 和luma 平面：文盲了
        }
    break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)//04. 良好的习惯 //TIGER PROGRAM AVFMT_GLOBALHEADER
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/**************************************************************/
/* audio output */

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  uint64_t channel_layout,
                                  int sample_rate, int nb_samples)//分配一帧，初始化，并按需分配buffer
{
    AVFrame *frame = av_frame_alloc();//01.分配AVFrame帧
    int ret;

    if (!frame) {
        fprintf(stderr, "Error allocating an audio frame\n");
        exit(1);
    }
    //02. 初始化
    frame->format = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;
    //03. 是否分配buffer
    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}

static void open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)//打开上下文， 声音频率设置，重采样设置
{
    AVCodecContext *c;
    int nb_samples;
    int ret;
    AVDictionary *opt = NULL;

    c = ost->enc;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);//01. 可选项
    ret = avcodec_open2(c, codec, &opt);//02. 打开上下文
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
        exit(1);
    }
    //03.自定义的
    /* init signal generator */
    ost->t     = 0;//初始为0
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;//110Hz 频率
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;//增加频率
    //04.
    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;//tiger program 如果可变，可采用自定义的长度
    else
        nb_samples = c->frame_size;

    ost->frame     = alloc_audio_frame(c->sample_fmt, c->channel_layout,//目标帧，比如aac
                                       c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, c->channel_layout,//临时存储的帧，AV_SAMPLE_FMT_S16隐含了是packed模式保存，如果是planar是AV_SAMPLE_FMT_S16P
                                       c->sample_rate, nb_samples);
    //05.从AVCodecContext中复制参数到AVStream的AVCodecParameters流里
    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
    //06.重采样SwrContext
    /* create resampler context */
        ost->swr_ctx = swr_alloc();
        if (!ost->swr_ctx) {
            fprintf(stderr, "Could not allocate resampler context\n");
            exit(1);
        }
    //07. 初始化SwrContext，即转码，从PCM的AV_SAMPLE_FMT_S16 转到指定的如AAC
        /* set options */
        av_opt_set_int       (ost->swr_ctx, "in_channel_count",   c->channels,       0);
        av_opt_set_int       (ost->swr_ctx, "in_sample_rate",     c->sample_rate,    0);
        av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);//AV_SAMPLE_FMT_S16隐含了是packed模式保存，如果是planar是AV_SAMPLE_FMT_S16P
        av_opt_set_int       (ost->swr_ctx, "out_channel_count",  c->channels,       0);
        av_opt_set_int       (ost->swr_ctx, "out_sample_rate",    c->sample_rate,    0);
        av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt",     c->sample_fmt,     0);
  
        /* initialize the resampling context */
        if ((ret = swr_init(ost->swr_ctx)) < 0) {
            fprintf(stderr, "Failed to initialize the resampling context\n");
            exit(1);
        }
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
static AVFrame *get_audio_frame(OutputStream *ost)
{
    AVFrame *frame = ost->tmp_frame;
    int j, i, v;
    int16_t *q = (int16_t*)frame->data[0];//实际的地址
    //01.时间戳是否已经领先10秒以上了，停止处理
    /* check if we want to generate more frames */
    if (av_compare_ts(ost->next_pts, ost->enc->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) >= 0)//tiger program 实际使用中超过10秒？是否要断开呢？是否要上报
        return NULL;
    //02. 疑问：不是pcm，数据也是这么放吗？还是frame里面放的还是pcm的数据
    for (j = 0; j <frame->nb_samples; j++) {
        v = (int)(sin(ost->t) * 10000);//
        for (i = 0; i < ost->enc->channels; i++)//这里的排列是packed，不是planar
            *q++ = v;//可能有多个声道，简单的都弄成sin函数产生的值， //疑问 *q是unsigned short， 和swr_init AV_SAMPLE_FMT_S16对应。 但v是int类型，这里是要溢出的
        ost->t     += ost->tincr;//某个频率
        ost->tincr += ost->tincr2;//每次增加频率
    }
    //03.
    frame->pts = ost->next_pts;//取OutputStream* ost的下一帧时间，ost->next_pts仅仅是一个自定义记住pts的地方
    ost->next_pts  += frame->nb_samples;//记录OutputStream下一个帧，直接加sample，因为这个是pcm，frame 时间基准就是 {1, 采样率}
    return frame;
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_audio_frame(AVFormatContext *oc, OutputStream *ost)
{
    AVCodecContext *c;
    AVPacket pkt = { 0 }; // data and size must be 0;
    AVFrame *frame;
    int ret;
    int got_packet;
    int dst_nb_samples;
    //01.
    av_init_packet(&pkt);
    c = ost->enc;//上下文
    //02. 取模拟产生的一帧
    frame = get_audio_frame(ost);//数据放在frame->data[0]中
    //03. 将模拟的帧数据，重采样后转化为aac
    if (frame) {
        /* convert samples from native format to destination codec format, using the resampler */
            /* compute destination number of samples */
            int delay = swr_get_delay(ost->swr_ctx, c->sample_rate);//swr_get_delay tiger program
            dst_nb_samples = av_rescale_rnd(delay + frame->nb_samples,
                                            c->sample_rate, c->sample_rate, AV_ROUND_UP);//03.01 因为采样率变化了，所以计算对应原始的采样点长度
            av_assert0(dst_nb_samples == frame->nb_samples);

        /* when we pass a frame to the encoder, it may keep a reference to it
         * internally;
         * make sure we do not overwrite it here
         */
        ret = av_frame_make_writable(ost->frame);//03.02
        if (ret < 0)
            exit(1);
        
        /* convert to destination format */
        ret = swr_convert(ost->swr_ctx,//03.03 转化为指定格式
                          ost->frame->data, dst_nb_samples,//
                          (const uint8_t **)frame->data, frame->nb_samples);
        if (ret < 0) {
            fprintf(stderr, "Error while converting\n");
            exit(1);
        }
        frame = ost->frame;//03.04
        //03.05 换算成新的pts
        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, c->sample_rate}, c->time_base);//从采样率，转换为AVFormatContext的时间单位
        ost->samples_count += dst_nb_samples;//计算采样值
    }
    //04.打包成AVPacket
    ret = avcodec_encode_audio2(c, &pkt, frame, &got_packet);
    if (ret < 0) {
        fprintf(stderr, "Error encoding audio frame: %s\n", av_err2str(ret));
        exit(1);
    }
    //05.如果有AVPacket产生，才能写到输出
    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);//还包括时间转化从AVFormatContext时间到AVStream的时间
        if (ret < 0) {
            fprintf(stderr, "Error while writing audio frame: %s\n",
                    av_err2str(ret));
            exit(1);
        }
    }

    return (frame || got_packet) ? 0 : 1;//返回0，说明有帧产生或者写成功
}

/**************************************************************/
/* video output */

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;
    //01.
    picture = av_frame_alloc();
    if (!picture)
        return NULL;
    //02.参数
    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;
    //03.帧的buffer
    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 64);//tiger program 改进，如果是HAVE_AVX512 应该是64不是32 //TIGER improvement
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return picture;
}

static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;
    //可选项
    av_dict_copy(&opt, opt_arg, 0);
    //上下文
    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }
    //目标帧，重复使用
    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    //临时帧，重复使用
    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);//指定AV_PIX_FMT_YUV420P格式，宽，高
        if (!ost->tmp_frame) {
            fprintf(stderr, "Could not allocate temporary picture\n");
            exit(1);
        }
    }
    //ost->st->codecpar（AVStream(st)的AVCodecParameters）清零，复制Context里的参数，
    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}

/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, int frame_index,//模拟产生一帧
                           int width, int height)
{
    int x, y, i;

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

static AVFrame *get_video_frame(OutputStream *ost)
{
    AVCodecContext *c = ost->enc;

    /* check if we want to generate more frames */
    if (av_compare_ts(ost->next_pts, c->time_base,//10秒以上
                      STREAM_DURATION, (AVRational){ 1, 1 }) >= 0)
        return NULL;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)//要变成可写模式
        exit(1);

    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        if (!ost->sws_ctx) {
            ost->sws_ctx = sws_getContext(c->width, c->height,//初始化
                                          AV_PIX_FMT_YUV420P,
                                          c->width, c->height,
                                          c->pix_fmt,
                                          SCALE_FLAGS, NULL, NULL, NULL);
            if (!ost->sws_ctx) {
                fprintf(stderr,
                        "Could not initialize the conversion context\n");
                exit(1);
            }
        }
        fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
        sws_scale(ost->sws_ctx, (const uint8_t * const *) ost->tmp_frame->data,//按比例缩放
                  ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
                  ost->frame->linesize);
    } else {
        fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
    }

    ost->frame->pts = ost->next_pts++;//这里只要加1即可

    return ost->frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
    int ret;
    AVCodecContext *c;
    AVFrame *frame;
    int got_packet = 0;
    AVPacket pkt = { 0 };

    c = ost->enc;
    //01.取一帧
    frame = get_video_frame(ost);
    //02.初始化
    av_init_packet(&pkt);
    //03.编码 调用ff_libx264_encoder.encode2即X264_frame，编码成AVPacket
    /* encode the image */
    ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
    if (ret < 0) {
        fprintf(stderr, "Error encoding video frame: %s\n", av_err2str(ret));
        exit(1);
    }
    //04. 写入
    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);//时间转换 
    } else {
        ret = 0;
    }

    if (ret < 0) {
        fprintf(stderr, "Error while writing video frame: %s\n", av_err2str(ret));
        exit(1);
    }

    return (frame || got_packet) ? 0 : 1;//返回为0说明有帧产生或者写成功
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}

/**************************************************************/
/* media file output */

int main(int argc, char **argv)
{
    OutputStream video_st = { 0 }, audio_st = { 0 };//这个是本程序自定义的一个结构
    const char *filename;
    AVOutputFormat *fmt;
    AVFormatContext *oc;
    AVCodec *audio_codec, *video_codec;
    int ret;
    int have_video = 0, have_audio = 0;
    int encode_video = 0, encode_audio = 0;
    AVDictionary *opt = NULL;
    int i;
	//01. 参数
    if (argc < 2) {
        printf("usage: %s output_file\n"
               "API example program to output a media file with libavformat.\n"
               "This program generates a synthetic audio and video stream, encodes and\n"
               "muxes them into a file named output_file.\n"
               "The output format is automatically guessed according to the file extension.\n"
               "Raw images can also be output by using '%%d' in the filename.\n"
               "\n", argv[0]);
        //return 1;
        //filename = "mux.ul";//wav
//#define MY_ALIGN (HAVE_AVX512 ? 64 : (HAVE_AVX ? 32 : 16))
 //       int align = MY_ALIGN;
        filename = "mux.mp4";//根据格式猜测格式，如果mp4，虽然是容器，可能允许多种格式，但如果不指定本身有默认的音频和视频格式，程序里面利用了这一点，所以没看到指定音视频的具体格式
    }
    else {
        filename = argv[1];
    }
    //可选项
    for (i = 2; i+1 < argc; i+=2) {
        if (!strcmp(argv[i], "-flags") || !strcmp(argv[i], "-fflags"))
            av_dict_set(&opt, argv[i]+1, argv[i+1], 0);
    }
    //02. 用filename 创建AVFormatContext
    /* allocate the output media context */
    avformat_alloc_output_context2(&oc, NULL, NULL, filename);//因为未指定格式，只能根据输出文件名猜一下格式
    if (!oc) {
        printf("Could not deduce output format from file extension: using MPEG.\n");
        avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);//如果不行，假定一个格式mpeg
    }
    if (!oc)//查找失败则退出
        return 1;
    //tiger program: watch oc->oformat
    fmt = oc->oformat;//03. 使用猜出来的编码器， 如果是ul文件：PCM mu-law: audio_codec=AV_CODEC_ID_PCM_MULAW，如果是mp4则是容易默认的格式
    //04. 创建video_st和audio_st
    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (fmt->video_codec != AV_CODEC_ID_NONE) {//如果输出格式包含视频，用输出的编码格式，构建一个流 add_stream 是一个比较大的函数
        add_stream(&video_st, oc, &video_codec, fmt->video_codec);//复杂的过程：创建video_st
        have_video = 1;//标记
        encode_video = 1;
    }
    if (fmt->audio_codec != AV_CODEC_ID_NONE) {//如果输出格式包含音频， add_stream 是一个比较大的函数
        add_stream(&audio_st, oc, &audio_codec, fmt->audio_codec);//如果是wav文件 fmt->audio_codec 默认是AV_CODEC_ID_PCM_S16LE即AV_CODEC_ID_FIRST_AUDIO, 默认ff_w64_muxer .audio_codec       = AV_CODEC_ID_PCM_S16LE
        have_audio = 1;//标记
        encode_audio = 1;
    }
    //04. 逐一打开
    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (have_video)
        open_video(oc, video_codec, &video_st, opt);//打开视频

    if (have_audio)
        open_audio(oc, audio_codec, &audio_st, opt);//打开音频

    av_dump_format(oc, 0, filename, 1);//打印流格式是个好习惯

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {//TIGER PROGRRAM 最好判断一下
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);//05.打开输出文件
        if (ret < 0) {
            fprintf(stderr, "Could not open '%s': %s\n", filename,
                    av_err2str(ret));
            return 1;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(oc, &opt);//06.写头文件, opt可带输出的可选项
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                av_err2str(ret));
        return 1;
    }

    while (encode_video || encode_audio) {//07. 如果音频或视频有一个允许，写帧
        /* select the stream to encode */
        if (encode_video &&//1. 允许视频，不允许音频，直接写视频 2. 允许视频，允许音频，但视频的时间比音频小，拉后了，得补上视频
            (!encode_audio || av_compare_ts(video_st.next_pts, video_st.enc->time_base,
                                            audio_st.next_pts, audio_st.enc->time_base) <= 0)) {
            encode_video = !write_video_frame(oc, &video_st);//如果成功返回0，允许继续写，encode_video 设为1
        } else {
            encode_audio = !write_audio_frame(oc, &audio_st);//如果成功返回0，允许继续写，encode_audio 设为1
        }
    }

    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(oc);//08.写尾部

    /* Close each codec. */
    if (have_video)
        close_stream(oc, &video_st);
    if (have_audio)
        close_stream(oc, &audio_st);

    if (!(fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&oc->pb);

    /* free the stream */
    avformat_free_context(oc);

    return 0;
}
