/*
 * ffmpeg filter configuration
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

#include <stdint.h>

#include "ffmpeg.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include "libavresample/avresample.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/display.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "libavutil/samplefmt.h"

static const enum AVPixelFormat *get_compliance_unofficial_pix_fmts(enum AVCodecID codec_id, const enum AVPixelFormat default_formats[])
{
    static const enum AVPixelFormat mjpeg_formats[] =
        { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P,
          AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV444P,
          AV_PIX_FMT_NONE };
    static const enum AVPixelFormat ljpeg_formats[] =
        { AV_PIX_FMT_BGR24   , AV_PIX_FMT_BGRA    , AV_PIX_FMT_BGR0,
          AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ422P,
          AV_PIX_FMT_YUV420P , AV_PIX_FMT_YUV444P , AV_PIX_FMT_YUV422P,
          AV_PIX_FMT_NONE};

    if (codec_id == AV_CODEC_ID_MJPEG) {
        return mjpeg_formats;
    } else if (codec_id == AV_CODEC_ID_LJPEG) {
        return ljpeg_formats;
    } else {
        return default_formats;
    }
}

enum AVPixelFormat choose_pixel_fmt(AVStream *st, AVCodecContext *enc_ctx, AVCodec *codec, enum AVPixelFormat target)
{
    if (codec && codec->pix_fmts) {
        const enum AVPixelFormat *p = codec->pix_fmts;
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(target);
        //FIXME: This should check for AV_PIX_FMT_FLAG_ALPHA after PAL8 pixel format without alpha is implemented
        int has_alpha = desc ? desc->nb_components % 2 == 0 : 0;
        enum AVPixelFormat best= AV_PIX_FMT_NONE;

        if (enc_ctx->strict_std_compliance <= FF_COMPLIANCE_UNOFFICIAL) {
            p = get_compliance_unofficial_pix_fmts(enc_ctx->codec_id, p);
        }
        for (; *p != AV_PIX_FMT_NONE; p++) {
            best= avcodec_find_best_pix_fmt_of_2(best, *p, target, has_alpha, NULL);
            if (*p == target)
                break;
        }
        if (*p == AV_PIX_FMT_NONE) {
            if (target != AV_PIX_FMT_NONE)
                av_log(NULL, AV_LOG_WARNING,
                       "Incompatible pixel format '%s' for codec '%s', auto-selecting format '%s'\n",
                       av_get_pix_fmt_name(target),
                       codec->name,
                       av_get_pix_fmt_name(best));
            return best;
        }
    }
    return target;
}

void choose_sample_fmt(AVStream *st, AVCodec *codec)
{
    if (codec && codec->sample_fmts) {
        const enum AVSampleFormat *p = codec->sample_fmts;
        for (; *p != -1; p++) {
            if (*p == st->codecpar->format)
                break;
        }
        if (*p == -1) {
            if((codec->capabilities & AV_CODEC_CAP_LOSSLESS) && av_get_sample_fmt_name(st->codecpar->format) > av_get_sample_fmt_name(codec->sample_fmts[0]))
                av_log(NULL, AV_LOG_ERROR, "Conversion will not be lossless.\n");
            if(av_get_sample_fmt_name(st->codecpar->format))
            av_log(NULL, AV_LOG_WARNING,
                   "Incompatible sample format '%s' for codec '%s', auto-selecting format '%s'\n",
                   av_get_sample_fmt_name(st->codecpar->format),
                   codec->name,
                   av_get_sample_fmt_name(codec->sample_fmts[0]));
            st->codecpar->format = codec->sample_fmts[0];
        }
    }
}

static char *choose_pix_fmts(OutputFilter *ofilter)
{
    OutputStream *ost = ofilter->ost;
    AVDictionaryEntry *strict_dict = av_dict_get(ost->encoder_opts, "strict", NULL, 0);
    if (strict_dict)
        // used by choose_pixel_fmt() and below
        av_opt_set(ost->enc_ctx, "strict", strict_dict->value, 0);

     if (ost->keep_pix_fmt) {
        avfilter_graph_set_auto_convert(ofilter->graph->graph,
                                            AVFILTER_AUTO_CONVERT_NONE);
        if (ost->enc_ctx->pix_fmt == AV_PIX_FMT_NONE)
            return NULL;
        return av_strdup(av_get_pix_fmt_name(ost->enc_ctx->pix_fmt));
    }
    if (ost->enc_ctx->pix_fmt != AV_PIX_FMT_NONE) {
        return av_strdup(av_get_pix_fmt_name(choose_pixel_fmt(ost->st, ost->enc_ctx, ost->enc, ost->enc_ctx->pix_fmt)));
    } else if (ost->enc && ost->enc->pix_fmts) {
        const enum AVPixelFormat *p;
        AVIOContext *s = NULL;
        uint8_t *ret;
        int len;

        if (avio_open_dyn_buf(&s) < 0)
            exit_program(1);

        p = ost->enc->pix_fmts;
        if (ost->enc_ctx->strict_std_compliance <= FF_COMPLIANCE_UNOFFICIAL) {
            p = get_compliance_unofficial_pix_fmts(ost->enc_ctx->codec_id, p);
        }

        for (; *p != AV_PIX_FMT_NONE; p++) {
            const char *name = av_get_pix_fmt_name(*p);
            avio_printf(s, "%s|", name);
        }
        len = avio_close_dyn_buf(s, &ret);
        ret[len - 1] = 0;
        return ret;
    } else
        return NULL;
}

/* Define a function for building a string containing a list of
 * allowed formats. */
#define DEF_CHOOSE_FORMAT(suffix, type, var, supported_list, none, get_name)   \
static char *choose_ ## suffix (OutputFilter *ofilter)                         \
{                                                                              \
    if (ofilter->var != none) {                                                \
        get_name(ofilter->var);                                                \
        return av_strdup(name);                                                \
    } else if (ofilter->supported_list) {                                      \
        const type *p;                                                         \
        AVIOContext *s = NULL;                                                 \
        uint8_t *ret;                                                          \
        int len;                                                               \
                                                                               \
        if (avio_open_dyn_buf(&s) < 0)                                         \
            exit_program(1);                                                           \
                                                                               \
        for (p = ofilter->supported_list; *p != none; p++) {                   \
            get_name(*p);                                                      \
            avio_printf(s, "%s|", name);                                       \
        }                                                                      \
        len = avio_close_dyn_buf(s, &ret);                                     \
        ret[len - 1] = 0;                                                      \
        return ret;                                                            \
    } else                                                                     \
        return NULL;                                                           \
}

//DEF_CHOOSE_FORMAT(pix_fmts, enum AVPixelFormat, format, formats, AV_PIX_FMT_NONE,
//                  GET_PIX_FMT_NAME)

DEF_CHOOSE_FORMAT(sample_fmts, enum AVSampleFormat, format, formats,
                  AV_SAMPLE_FMT_NONE, GET_SAMPLE_FMT_NAME)

DEF_CHOOSE_FORMAT(sample_rates, int, sample_rate, sample_rates, 0,
                  GET_SAMPLE_RATE_NAME)

DEF_CHOOSE_FORMAT(channel_layouts, uint64_t, channel_layout, channel_layouts, 0,
                  GET_CH_LAYOUT_NAME)//TIGER 01 调用分析：open_output_file --> init_simple_filtergraph
//TIGER 02 //tiger program gdb watch filtergraphs //通过ist 可以找InputFilter：ist->filters[ist->nb_filters - 1] ; InputFilter 可以直接找到fg, ist; 通过fg,可以查找 fg->outputs[0]->ost, fg->inputs[0]->ist 
int init_simple_filtergraph(InputStream *ist, OutputStream *ost)//TIGER 03 init_simple_filtergraph 理解filtergraph和输入出流的关联 //TIGER IMPORTANT
{//TIGER 03.创建FilterGraph，创建第一个OutputFilter，所以用fg->outputs[0]表示，创建第一个InputFilter，所以用fg->inputs[0]表示 ,真正初始化在 decode_video-->send_frame_to_filter-->ifilter_send_frame-->configure_filtergraph-->configure_output_video_filter
    FilterGraph *fg = av_mallocz(sizeof(*fg));//01.创建FilterGraph实例 ,这里隐含graph_desc为空，通过这个来判断是不是simple graph， 反之就是complex graph

    if (!fg)
        exit_program(1);
    fg->index = nb_filtergraphs;//这是全局数组的index，nb_filtergraphs 会在末尾增加
    //02.创建并初始化FilterGraph的OutputFilter，使得和OutputStream，FilterGraph关联
    GROW_ARRAY(fg->outputs, fg->nb_outputs);//扩展输出 fg->outputs，并fg->nb_outputs自增，如果失败，直接退出程序
    if (!(fg->outputs[0] = av_mallocz(sizeof(*fg->outputs[0]))))//创建OutputFilter实例
        exit_program(1);
    fg->outputs[0]->ost   = ost;//初始化OutputFilter，且OutputFilter和OutputStream相互关联
    fg->outputs[0]->graph = fg;//OutputFilter和FilterGraph关联，相互指向   //注：OutputFilter和InputFilter都有graph
    fg->outputs[0]->format = -1;//格式无

    ost->filter = fg->outputs[0];//OutputStream和OutputFilter相互关联
    //03.创建并初始化FilterGraph的InputFilter，使得和InputStream，FilterGraph关联
    GROW_ARRAY(fg->inputs, fg->nb_inputs);//扩展输入 fg->inputs，并nb_inputs自增
    if (!(fg->inputs[0] = av_mallocz(sizeof(*fg->inputs[0]))))//创建InputFilter实例
        exit_program(1);
    fg->inputs[0]->ist   = ist;//初始化InputFilter，且InputFilter和InputStream相互关联
    fg->inputs[0]->graph = fg;//InputFilter和FilterGraph关联，相互指向   //注：OutputFilter和InputFilter都有graph
    fg->inputs[0]->format = -1;
    //04.创建FilterGraph的InputFilter的输入队列
    fg->inputs[0]->frame_queue = av_fifo_alloc(8 * sizeof(AVFrame*));//输入的队列，默认8个，只有输入需要缓存
    if (!fg->inputs[0]->frame_queue)
        exit_program(1);
    //05.输入流的filter里面绑定FilterGraph的InputFilter
    GROW_ARRAY(ist->filters, ist->nb_filters);//扩展ist->filters，并nb_filters自增，如果失败，直接退出程序
    ist->filters[ist->nb_filters - 1] = fg->inputs[0];//ist->filters加入一个InputFilter，但不一定是ist的第一个
    //06.放到全局数组中
    GROW_ARRAY(filtergraphs, nb_filtergraphs);//扩展filtergraphs，并nb_filtergraphs自增，如果失败，直接退出程序
    filtergraphs[nb_filtergraphs - 1] = fg;
    
    return 0;
}

static char *describe_filter_link(FilterGraph *fg, AVFilterInOut *inout, int in)
{
    AVFilterContext *ctx = inout->filter_ctx;
    AVFilterPad *pads = in ? ctx->input_pads  : ctx->output_pads;
    int       nb_pads = in ? ctx->nb_inputs   : ctx->nb_outputs;
    AVIOContext *pb;
    uint8_t *res = NULL;

    if (avio_open_dyn_buf(&pb) < 0)
        exit_program(1);

    avio_printf(pb, "%s", ctx->filter->name);
    if (nb_pads > 1)
        avio_printf(pb, ":%s", avfilter_pad_get_name(pads, inout->pad_idx));
    avio_w8(pb, 0);
    avio_close_dyn_buf(pb, &res);
    return res;
}

static void init_input_filter(FilterGraph *fg, AVFilterInOut *in)//初始化FilterGraph fg，放入输入出的流
{
    InputStream *ist = NULL;
    enum AVMediaType type = avfilter_pad_get_type(in->filter_ctx->input_pads, in->pad_idx);
    int i;

    // TODO: support other filter types
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_FATAL, "Only video and audio filters supported "
               "currently.\n");
        exit_program(1);
    }

    if (in->name) {
        AVFormatContext *s;
        AVStream       *st = NULL;
        char *p;
        int file_idx = strtol(in->name, &p, 0);

        if (file_idx < 0 || file_idx >= nb_input_files) {
            av_log(NULL, AV_LOG_FATAL, "Invalid file index %d in filtergraph description %s.\n",
                   file_idx, fg->graph_desc);
            exit_program(1);
        }
        s = input_files[file_idx]->ctx;

        for (i = 0; i < s->nb_streams; i++) {
            enum AVMediaType stream_type = s->streams[i]->codecpar->codec_type;
            if (stream_type != type &&
                !(stream_type == AVMEDIA_TYPE_SUBTITLE &&
                  type == AVMEDIA_TYPE_VIDEO /* sub2video hack */))
                continue;
            if (check_stream_specifier(s, s->streams[i], *p == ':' ? p + 1 : p) == 1) {
                st = s->streams[i];
                break;
            }
        }
        if (!st) {
            av_log(NULL, AV_LOG_FATAL, "Stream specifier '%s' in filtergraph description %s "
                   "matches no streams.\n", p, fg->graph_desc);
            exit_program(1);
        }
        ist = input_streams[input_files[file_idx]->ist_index + st->index];
        if (ist->user_set_discard == AVDISCARD_ALL) {
            av_log(NULL, AV_LOG_FATAL, "Stream specifier '%s' in filtergraph description %s "
                   "matches a disabled input stream.\n", p, fg->graph_desc);
            exit_program(1);
        }
    } else {
        /* find the first unused stream of corresponding type */
        for (i = 0; i < nb_input_streams; i++) {
            ist = input_streams[i];
            if (ist->user_set_discard == AVDISCARD_ALL)
                continue;
            if (ist->dec_ctx->codec_type == type && ist->discard)
                break;
        }
        if (i == nb_input_streams) {
            av_log(NULL, AV_LOG_FATAL, "Cannot find a matching stream for "
                   "unlabeled input pad %d on filter %s\n", in->pad_idx,
                   in->filter_ctx->name);
            exit_program(1);
        }
    }
    av_assert0(ist);

    ist->discard         = 0;
    ist->decoding_needed |= DECODING_FOR_FILTER;
    ist->st->discard = AVDISCARD_NONE;

    GROW_ARRAY(fg->inputs, fg->nb_inputs);
    if (!(fg->inputs[fg->nb_inputs - 1] = av_mallocz(sizeof(*fg->inputs[0]))))
        exit_program(1);
    fg->inputs[fg->nb_inputs - 1]->ist   = ist;
    fg->inputs[fg->nb_inputs - 1]->graph = fg;
    fg->inputs[fg->nb_inputs - 1]->format = -1;
    fg->inputs[fg->nb_inputs - 1]->type = ist->st->codecpar->codec_type;
    fg->inputs[fg->nb_inputs - 1]->name = describe_filter_link(fg, in, 1);

    fg->inputs[fg->nb_inputs - 1]->frame_queue = av_fifo_alloc(8 * sizeof(AVFrame*));
    if (!fg->inputs[fg->nb_inputs - 1]->frame_queue)
        exit_program(1);
    //放入输入流里
    GROW_ARRAY(ist->filters, ist->nb_filters);
    ist->filters[ist->nb_filters - 1] = fg->inputs[fg->nb_inputs - 1];
}
//complex filtergraph 的说明看ffmpeg.texi
int init_complex_filtergraph(FilterGraph *fg)//TIGER 看注释，FilterGraph的初始化，比如有多个输入，一个输出
{
    AVFilterInOut *inputs, *outputs, *cur;
    AVFilterGraph *graph;
    int ret = 0;
    //TIGER 重要的注释
    /* this graph is only used for determining the kinds of inputs
     * and outputs we have, and is discarded on exit from this function */
    graph = avfilter_graph_alloc();//01.分配AVFilterGraph内存，并初始化：分配internal内存，设置av_class， option，internal->frame_queues
    if (!graph)
        return AVERROR(ENOMEM);
    graph->nb_threads = 1;//02.avfilter_graph_parse2时候只允许一个线程
    //03.根据名称fg->graph_desc解析，得到AVFilterInOut的输入和输出
    ret = avfilter_graph_parse2(graph, fg->graph_desc, &inputs, &outputs);
    if (ret < 0)
        goto fail;
    //04.处理InputputFilter
    for (cur = inputs; cur; cur = cur->next)
        init_input_filter(fg, cur);//尝试初始化一下，判断是否正常
    //05.处理OutputFilter
    for (cur = outputs; cur;) {
        GROW_ARRAY(fg->outputs, fg->nb_outputs);
        fg->outputs[fg->nb_outputs - 1] = av_mallocz(sizeof(*fg->outputs[0]));
        if (!fg->outputs[fg->nb_outputs - 1])
            exit_program(1);

        fg->outputs[fg->nb_outputs - 1]->graph   = fg;
        fg->outputs[fg->nb_outputs - 1]->out_tmp = cur;//为什么暂时放这里?
        fg->outputs[fg->nb_outputs - 1]->type    = avfilter_pad_get_type(cur->filter_ctx->output_pads,
                                                                         cur->pad_idx);
        fg->outputs[fg->nb_outputs - 1]->name = describe_filter_link(fg, cur, 0);
        cur = cur->next;
        fg->outputs[fg->nb_outputs - 1]->out_tmp->next = NULL;
    }

fail://06.释放graph和inputs，但保留了fg
    avfilter_inout_free(&inputs);//为什么是释放filter？ ==>
    avfilter_graph_free(&graph);
    return ret;
}

static int insert_trim(int64_t start_time, int64_t duration,
                       AVFilterContext **last_filter, int *pad_idx,
                       const char *filter_name)
{
    AVFilterGraph *graph = (*last_filter)->graph;
    AVFilterContext *ctx;
    const AVFilter *trim;
    enum AVMediaType type = avfilter_pad_get_type((*last_filter)->output_pads, *pad_idx);
    const char *name = (type == AVMEDIA_TYPE_VIDEO) ? "trim" : "atrim";
    int ret = 0;

    if (duration == INT64_MAX && start_time == AV_NOPTS_VALUE)
        return 0;

    trim = avfilter_get_by_name(name);
    if (!trim) {
        av_log(NULL, AV_LOG_ERROR, "%s filter not present, cannot limit "
               "recording time.\n", name);
        return AVERROR_FILTER_NOT_FOUND;
    }

    ctx = avfilter_graph_alloc_filter(graph, trim, filter_name);
    if (!ctx)
        return AVERROR(ENOMEM);

    if (duration != INT64_MAX) {
        ret = av_opt_set_int(ctx, "durationi", duration,
                                AV_OPT_SEARCH_CHILDREN);
    }
    if (ret >= 0 && start_time != AV_NOPTS_VALUE) {
        ret = av_opt_set_int(ctx, "starti", start_time,
                                AV_OPT_SEARCH_CHILDREN);
    }
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error configuring the %s filter", name);
        return ret;
    }

    ret = avfilter_init_str(ctx, NULL);
    if (ret < 0)
        return ret;

    ret = avfilter_link(*last_filter, *pad_idx, ctx, 0);
    if (ret < 0)
        return ret;

    *last_filter = ctx;
    *pad_idx     = 0;
    return 0;
}

static int insert_filter(AVFilterContext **last_filter, int *pad_idx,
                         const char *filter_name, const char *args)
{
    AVFilterGraph *graph = (*last_filter)->graph;
    AVFilterContext *ctx;
    int ret;

    ret = avfilter_graph_create_filter(&ctx,
                                       avfilter_get_by_name(filter_name),
                                       filter_name, args, NULL, graph);
    if (ret < 0)
        return ret;

    ret = avfilter_link(*last_filter, *pad_idx, ctx, 0);
    if (ret < 0)
        return ret;

    *last_filter = ctx;
    *pad_idx     = 0;
    return 0;
}
//TIGER transcode_step--> prcess_input-->prcess_input_packet-->decode_video-->send_frame_to_filter-->ifilter_send_frame-->configure_filtergraph-->configure_output_video_filter
static int configure_output_video_filter(FilterGraph *fg, OutputFilter *ofilter, AVFilterInOut *out)//创建buffersink为第一个？或最后一个filter？的grapch
{
    char *pix_fmts;
    OutputStream *ost = ofilter->ost;
    OutputFile    *of = output_files[ost->file_index];
    AVFilterContext *last_filter = out->filter_ctx;
    int pad_idx = out->pad_idx;
    int ret;
    char name[255];
    //01. 用buffersink创建一个filter，和相关的AVFilterContext，插入 fg->graph->filters
    snprintf(name, sizeof(name), "out_%d_%d", ost->file_index, ost->index);
    ret = avfilter_graph_create_filter(&ofilter->filter,//tiger avfilter_graph_create_filter-->avfilter_graph_alloc_filter-->avfilter_graph_alloc_filter
                                       avfilter_get_by_name("buffersink"),//先创建"buffersink" 的filter
                                       name, NULL, NULL, fg->graph);

    if (ret < 0)
        return ret;
    //02. 如果有视频参数， 如果是simple graph 是在open_output_file中的init_simple_graph 之后的函数设置，filter的值是从ost获取的
    if (ofilter->width || ofilter->height) {
        char args[255];
        AVFilterContext *filter;
        AVDictionaryEntry *e = NULL;

        snprintf(args, sizeof(args), "%d:%d",
                 ofilter->width, ofilter->height);

        while ((e = av_dict_get(ost->sws_dict, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), ":%s=%s", e->key, e->value);
        }

        snprintf(name, sizeof(name), "scaler_out_%d_%d",
                 ost->file_index, ost->index);
        if ((ret = avfilter_graph_create_filter(&filter, avfilter_get_by_name("scale"),
                                                name, args, NULL, fg->graph)) < 0)
            return ret;
        if ((ret = avfilter_link(last_filter, pad_idx, filter, 0)) < 0)
            return ret;

        last_filter = filter;
        pad_idx = 0;
    }
    //03.
    if ((pix_fmts = choose_pix_fmts(ofilter))) {
        AVFilterContext *filter;
        snprintf(name, sizeof(name), "format_out_%d_%d",
                 ost->file_index, ost->index);
        ret = avfilter_graph_create_filter(&filter,
                                           avfilter_get_by_name("format"),
                                           "format", pix_fmts, NULL, fg->graph);
        av_freep(&pix_fmts);
        if (ret < 0)
            return ret;
        if ((ret = avfilter_link(last_filter, pad_idx, filter, 0)) < 0)
            return ret;

        last_filter = filter;
        pad_idx     = 0;
    }
    //04.
    if (ost->frame_rate.num && 0) {
        AVFilterContext *fps;
        char args[255];

        snprintf(args, sizeof(args), "fps=%d/%d", ost->frame_rate.num,
                 ost->frame_rate.den);
        snprintf(name, sizeof(name), "fps_out_%d_%d",
                 ost->file_index, ost->index);
        ret = avfilter_graph_create_filter(&fps, avfilter_get_by_name("fps"),
                                           name, args, NULL, fg->graph);
        if (ret < 0)
            return ret;

        ret = avfilter_link(last_filter, pad_idx, fps, 0);
        if (ret < 0)
            return ret;
        last_filter = fps;
        pad_idx = 0;
    }

    snprintf(name, sizeof(name), "trim_out_%d_%d",
             ost->file_index, ost->index);
    ret = insert_trim(of->start_time, of->recording_time,//05.从哪帧开始和结束
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;

    //06.
    if ((ret = avfilter_link(last_filter, pad_idx, ofilter->filter, 0)) < 0)
        return ret;

    return 0;
}

static int configure_output_audio_filter(FilterGraph *fg, OutputFilter *ofilter, AVFilterInOut *out)
{
    OutputStream *ost = ofilter->ost;
    OutputFile    *of = output_files[ost->file_index];
    AVCodecContext *codec  = ost->enc_ctx;
    AVFilterContext *last_filter = out->filter_ctx;
    int pad_idx = out->pad_idx;
    char *sample_fmts, *sample_rates, *channel_layouts;
    char name[255];
    int ret;
    //01.
    snprintf(name, sizeof(name), "out_%d_%d", ost->file_index, ost->index);
    ret = avfilter_graph_create_filter(&ofilter->filter,
                                       avfilter_get_by_name("abuffersink"),
                                       name, NULL, NULL, fg->graph);
    if (ret < 0)
        return ret;
    if ((ret = av_opt_set_int(ofilter->filter, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        return ret;

#define AUTO_INSERT_FILTER(opt_name, filter_name, arg) do {                 \
    AVFilterContext *filt_ctx;                                              \
                                                                            \
    av_log(NULL, AV_LOG_INFO, opt_name " is forwarded to lavfi "            \
           "similarly to -af " filter_name "=%s.\n", arg);                  \
                                                                            \
    ret = avfilter_graph_create_filter(&filt_ctx,                           \
                                       avfilter_get_by_name(filter_name),   \
                                       filter_name, arg, NULL, fg->graph);  \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    ret = avfilter_link(last_filter, pad_idx, filt_ctx, 0);                 \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    last_filter = filt_ctx;                                                 \
    pad_idx = 0;                                                            \
} while (0)
    if (ost->audio_channels_mapped) {
        int i;
        AVBPrint pan_buf;
        av_bprint_init(&pan_buf, 256, 8192);
        av_bprintf(&pan_buf, "0x%"PRIx64,
                   av_get_default_channel_layout(ost->audio_channels_mapped));
        for (i = 0; i < ost->audio_channels_mapped; i++)
            if (ost->audio_channels_map[i] != -1)
                av_bprintf(&pan_buf, "|c%d=c%d", i, ost->audio_channels_map[i]);

        AUTO_INSERT_FILTER("-map_channel", "pan", pan_buf.str);
        av_bprint_finalize(&pan_buf, NULL);
    }

    if (codec->channels && !codec->channel_layout)
        codec->channel_layout = av_get_default_channel_layout(codec->channels);

    sample_fmts     = choose_sample_fmts(ofilter);
    sample_rates    = choose_sample_rates(ofilter);
    channel_layouts = choose_channel_layouts(ofilter);
    if (sample_fmts || sample_rates || channel_layouts) {
        AVFilterContext *format;
        char args[256];
        args[0] = 0;

        if (sample_fmts)
            av_strlcatf(args, sizeof(args), "sample_fmts=%s:",
                            sample_fmts);
        if (sample_rates)
            av_strlcatf(args, sizeof(args), "sample_rates=%s:",
                            sample_rates);
        if (channel_layouts)
            av_strlcatf(args, sizeof(args), "channel_layouts=%s:",
                            channel_layouts);

        av_freep(&sample_fmts);
        av_freep(&sample_rates);
        av_freep(&channel_layouts);

        snprintf(name, sizeof(name), "format_out_%d_%d",
                 ost->file_index, ost->index);
        ret = avfilter_graph_create_filter(&format,
                                           avfilter_get_by_name("aformat"),
                                           name, args, NULL, fg->graph);
        if (ret < 0)
            return ret;

        ret = avfilter_link(last_filter, pad_idx, format, 0);
        if (ret < 0)
            return ret;

        last_filter = format;
        pad_idx = 0;
    }

    if (audio_volume != 256 && 0) {
        char args[256];

        snprintf(args, sizeof(args), "%f", audio_volume / 256.);
        AUTO_INSERT_FILTER("-vol", "volume", args);
    }

    if (ost->apad && of->shortest) {
        char args[256];
        int i;

        for (i=0; i<of->ctx->nb_streams; i++)
            if (of->ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                break;

        if (i<of->ctx->nb_streams) {
            snprintf(args, sizeof(args), "%s", ost->apad);
            AUTO_INSERT_FILTER("-apad", "apad", args);
        }
    }

    snprintf(name, sizeof(name), "trim for output stream %d:%d",
             ost->file_index, ost->index);
    ret = insert_trim(of->start_time, of->recording_time,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;

    if ((ret = avfilter_link(last_filter, pad_idx, ofilter->filter, 0)) < 0)
        return ret;

    return 0;
}

int configure_output_filter(FilterGraph *fg, OutputFilter *ofilter, AVFilterInOut *out)//创建buffersink为初始的graph
{
    if (!ofilter->ost) {
        av_log(NULL, AV_LOG_FATAL, "Filter %s has an unconnected output\n", ofilter->name);
        exit_program(1);
    }

    switch (avfilter_pad_get_type(out->filter_ctx->output_pads, out->pad_idx)) {
    case AVMEDIA_TYPE_VIDEO: return configure_output_video_filter(fg, ofilter, out);//创建buffersink为初始的graph
    case AVMEDIA_TYPE_AUDIO: return configure_output_audio_filter(fg, ofilter, out);
    default: av_assert0(0);
    }
}

void check_filter_outputs(void)
{
    int i;
    for (i = 0; i < nb_filtergraphs; i++) {
        int n;
        for (n = 0; n < filtergraphs[i]->nb_outputs; n++) {
            OutputFilter *output = filtergraphs[i]->outputs[n];
            if (!output->ost) {//日志说是没有连接，说明是异常的 ==>什么时候呢？
                av_log(NULL, AV_LOG_FATAL, "Filter %s has an unconnected output\n", output->name);
                exit_program(1);
            }
        }
    }
}

static int sub2video_prepare(InputStream *ist, InputFilter *ifilter)
{
    AVFormatContext *avf = input_files[ist->file_index]->ctx;
    int i, w, h;

    /* Compute the size of the canvas for the subtitles stream.
       If the subtitles codecpar has set a size, use it. Otherwise use the
       maximum dimensions of the video streams in the same file. */
    w = ifilter->width;
    h = ifilter->height;
    if (!(w && h)) {
        for (i = 0; i < avf->nb_streams; i++) {
            if (avf->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                w = FFMAX(w, avf->streams[i]->codecpar->width);
                h = FFMAX(h, avf->streams[i]->codecpar->height);
            }
        }
        if (!(w && h)) {
            w = FFMAX(w, 720);
            h = FFMAX(h, 576);
        }
        av_log(avf, AV_LOG_INFO, "sub2video: using %dx%d canvas\n", w, h);
    }
    ist->sub2video.w = ifilter->width  = w;
    ist->sub2video.h = ifilter->height = h;

    ifilter->width  = ist->dec_ctx->width  ? ist->dec_ctx->width  : ist->sub2video.w;
    ifilter->height = ist->dec_ctx->height ? ist->dec_ctx->height : ist->sub2video.h;

    /* rectangles are AV_PIX_FMT_PAL8, but we have no guarantee that the
       palettes for all rectangles are identical or compatible */
    ifilter->format = AV_PIX_FMT_RGB32;

    ist->sub2video.frame = av_frame_alloc();
    if (!ist->sub2video.frame)
        return AVERROR(ENOMEM);
    ist->sub2video.last_pts = INT64_MIN;
    ist->sub2video.end_pts  = INT64_MIN;
    return 0;
}
//配置graph transcode_step-->process_input-->process_input_packet-->decode_video-->send_frame_to_filter-->ifilter_send_frame-->configure_filtergraph-->configure_output_video_filter
static int configure_input_video_filter(FilterGraph *fg, InputFilter *ifilter,//先创建一个buffer的filter
                                        AVFilterInOut *in)
{
    AVFilterContext *last_filter;//01.
    const AVFilter *buffer_filt = avfilter_get_by_name("buffer");//buffer是视频ff_vsrc_buffer, 这个是默认的带个filter，后面的才能串起来
    InputStream *ist = ifilter->ist;//02.
    InputFile     *f = input_files[ist->file_index];
    AVRational tb = ist->framerate.num ? av_inv_q(ist->framerate) ://03.
                                         ist->st->time_base;
    AVRational fr = ist->framerate;
    AVRational sar;
    AVBPrint args;//04.
    char name[255];
    int ret, pad_idx = 0;
    int64_t tsoffset = 0;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();//05.

    if (!par)
        return AVERROR(ENOMEM);
    memset(par, 0, sizeof(*par));
    par->format = AV_PIX_FMT_NONE;
    //不允许音频
    if (ist->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_ERROR, "Cannot connect video filter to audio input\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }
    //06.
    if (!fr.num)
        fr = av_guess_frame_rate(input_files[ist->file_index]->ctx, ist->st, NULL);

    if (ist->dec_ctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        ret = sub2video_prepare(ist, ifilter);
        if (ret < 0)
            goto fail;
    }
    //07.
    sar = ifilter->sample_aspect_ratio;
    if(!sar.den)
        sar = (AVRational){0,1};
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);//08.
    av_bprintf(&args,
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:"
             "pixel_aspect=%d/%d:sws_param=flags=%d",
             ifilter->width, ifilter->height, ifilter->format,
             tb.num, tb.den, sar.num, sar.den,
             SWS_BILINEAR + ((ist->dec_ctx->flags&AV_CODEC_FLAG_BITEXACT) ? SWS_BITEXACT:0));
    if (fr.num && fr.den)
        av_bprintf(&args, ":frame_rate=%d/%d", fr.num, fr.den);
    snprintf(name, sizeof(name), "graph %d input from stream %d:%d", fg->index,
             ist->file_index, ist->st->index);

    //09. 用buffer_filt 即ff_vsrc_buffer 创建filter graph，后面根据需要不停加filter
    if ((ret = avfilter_graph_create_filter(&ifilter->filter, buffer_filt, name,
                                            args.str, NULL, fg->graph)) < 0)
        goto fail;
    par->hw_frames_ctx = ifilter->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(ifilter->filter, par);//10.
    if (ret < 0)
        goto fail;
    av_freep(&par);
    last_filter = ifilter->filter;
    //11. 插入自动旋转的filter
    if (ist->autorotate) {
        double theta = get_rotation(ist->st);

        if (fabs(theta - 90) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "hflip", NULL);
            if (ret < 0)
                return ret;
            ret = insert_filter(&last_filter, &pad_idx, "vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            ret = insert_filter(&last_filter, &pad_idx, "rotate", rotate_buf);
        }
        if (ret < 0)
            return ret;
    }
    //12. interlace的filter
    if (do_deinterlace) {
        AVFilterContext *yadif;

        snprintf(name, sizeof(name), "deinterlace_in_%d_%d",
                 ist->file_index, ist->st->index);
        if ((ret = avfilter_graph_create_filter(&yadif,
                                                avfilter_get_by_name("yadif"),
                                                name, "", NULL,
                                                fg->graph)) < 0)
            return ret;

        if ((ret = avfilter_link(last_filter, 0, yadif, 0)) < 0)
            return ret;

        last_filter = yadif;
    }

    snprintf(name, sizeof(name), "trim_in_%d_%d",
             ist->file_index, ist->st->index);
    if (copy_ts) {//13.
        tsoffset = f->start_time == AV_NOPTS_VALUE ? 0 : f->start_time;
        if (!start_at_zero && f->ctx->start_time != AV_NOPTS_VALUE)
            tsoffset += f->ctx->start_time;
    }//14. 从哪一帧开始和结束 trim filter
    ret = insert_trim(((f->start_time == AV_NOPTS_VALUE) || !f->accurate_seek) ?
                      AV_NOPTS_VALUE : tsoffset, f->recording_time,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;
    //15.
    if ((ret = avfilter_link(last_filter, 0, in->filter_ctx, in->pad_idx)) < 0)//不理解
        return ret;
    return 0;
fail:
    av_freep(&par);

    return ret;
}
//配置graph transcode_step-->process_input-->process_input_packet-->decode_video-->send_frame_to_filter-->ifilter_send_frame-->configure_filtergraph-->configure_output_video_filter
static int configure_input_audio_filter(FilterGraph *fg, InputFilter *ifilter,
                                        AVFilterInOut *in)
{
    AVFilterContext *last_filter;//01.
    const AVFilter *abuffer_filt = avfilter_get_by_name("abuffer");//abuffer是视频ff_asrc_abuffer,将是fitlergraph的以一个
    InputStream *ist = ifilter->ist;//02.
    InputFile     *f = input_files[ist->file_index];
    AVBPrint args;//03.
    char name[255];
    int ret, pad_idx = 0;
    int64_t tsoffset = 0;

    if (ist->dec_ctx->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_ERROR, "Cannot connect audio filter to non audio input\n");
        return AVERROR(EINVAL);
    }
    //04. 打印音频的参数
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&args, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s",
             1, ifilter->sample_rate,
             ifilter->sample_rate,
             av_get_sample_fmt_name(ifilter->format));
    if (ifilter->channel_layout)
        av_bprintf(&args, ":channel_layout=0x%"PRIx64,
                   ifilter->channel_layout);
    else
        av_bprintf(&args, ":channels=%d", ifilter->channels);
    snprintf(name, sizeof(name), "graph_%d_in_%d_%d", fg->index,
             ist->file_index, ist->st->index);
    //05.用ff_asrc_abuffer 创建一个filtergraph
    if ((ret = avfilter_graph_create_filter(&ifilter->filter, abuffer_filt,
                                            name, args.str, NULL,
                                            fg->graph)) < 0)
        return ret;
    last_filter = ifilter->filter;

#define AUTO_INSERT_FILTER_INPUT(opt_name, filter_name, arg) do {                 \
    AVFilterContext *filt_ctx;                                              \
                                                                            \
    av_log(NULL, AV_LOG_INFO, opt_name " is forwarded to lavfi "            \
           "similarly to -af " filter_name "=%s.\n", arg);                  \
                                                                            \
    snprintf(name, sizeof(name), "graph_%d_%s_in_%d_%d",      \
                fg->index, filter_name, ist->file_index, ist->st->index);   \
    ret = avfilter_graph_create_filter(&filt_ctx,                           \
                                       avfilter_get_by_name(filter_name),   \
                                       name, arg, NULL, fg->graph);         \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    ret = avfilter_link(last_filter, 0, filt_ctx, 0);                       \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    last_filter = filt_ctx;                                                 \
} while (0)
    //06. 加async filter
    if (audio_sync_method > 0) {
        char args[256] = {0};

        av_strlcatf(args, sizeof(args), "async=%d", audio_sync_method);
        if (audio_drift_threshold != 0.1)
            av_strlcatf(args, sizeof(args), ":min_hard_comp=%f", audio_drift_threshold);
        if (!fg->reconfiguration)
            av_strlcatf(args, sizeof(args), ":first_pts=0");
        AUTO_INSERT_FILTER_INPUT("-async", "aresample", args);
    }

//     if (ost->audio_channels_mapped) {
//         int i;
//         AVBPrint pan_buf;
//         av_bprint_init(&pan_buf, 256, 8192);
//         av_bprintf(&pan_buf, "0x%"PRIx64,
//                    av_get_default_channel_layout(ost->audio_channels_mapped));
//         for (i = 0; i < ost->audio_channels_mapped; i++)
//             if (ost->audio_channels_map[i] != -1)
//                 av_bprintf(&pan_buf, ":c%d=c%d", i, ost->audio_channels_map[i]);
//         AUTO_INSERT_FILTER_INPUT("-map_channel", "pan", pan_buf.str);
//         av_bprint_finalize(&pan_buf, NULL);
//     }
    //07.
    if (audio_volume != 256) {
        char args[256];

        av_log(NULL, AV_LOG_WARNING, "-vol has been deprecated. Use the volume "
               "audio filter instead.\n");

        snprintf(args, sizeof(args), "%f", audio_volume / 256.);
        AUTO_INSERT_FILTER_INPUT("-vol", "volume", args);
    }

    snprintf(name, sizeof(name), "trim for input stream %d:%d",
             ist->file_index, ist->st->index);
    if (copy_ts) {//08.
        tsoffset = f->start_time == AV_NOPTS_VALUE ? 0 : f->start_time;
        if (!start_at_zero && f->ctx->start_time != AV_NOPTS_VALUE)
            tsoffset += f->ctx->start_time;
    }//09. 从哪一帧开始和结束 trim filter
    ret = insert_trim(((f->start_time == AV_NOPTS_VALUE) || !f->accurate_seek) ?
                      AV_NOPTS_VALUE : tsoffset, f->recording_time,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;
    //10.
    if ((ret = avfilter_link(last_filter, 0, in->filter_ctx, in->pad_idx)) < 0)//不理解
        return ret;

    return 0;
}
//tiger send_frame_to_filter-->ifilter_send_frame-->configure_filtergraph-->configure_input_filter-->configure_input_video_filter
static int configure_input_filter(FilterGraph *fg, InputFilter *ifilter,//视频加入第一个名为“buffer”的filter，后面的filter根据实际配置再添加，串起来；音频则是abuffer
                                  AVFilterInOut *in)
{
    if (!ifilter->ist->dec) {
        av_log(NULL, AV_LOG_ERROR,
               "No decoder for stream #%d:%d, filtering impossible\n",
               ifilter->ist->file_index, ifilter->ist->st->index);
        return AVERROR_DECODER_NOT_FOUND;
    }
    switch (avfilter_pad_get_type(in->filter_ctx->input_pads, in->pad_idx)) {
    case AVMEDIA_TYPE_VIDEO: return configure_input_video_filter(fg, ifilter, in);
    case AVMEDIA_TYPE_AUDIO: return configure_input_audio_filter(fg, ifilter, in);
    default: av_assert0(0);
    }
}

static void cleanup_filtergraph(FilterGraph *fg)//释放输入出filter，再释放graph
{
    int i;
    for (i = 0; i < fg->nb_outputs; i++)
        fg->outputs[i]->filter = (AVFilterContext *)NULL;//释放输出filter
    for (i = 0; i < fg->nb_inputs; i++)
        fg->inputs[i]->filter = (AVFilterContext *)NULL//释放输入filter
    avfilter_graph_free(&fg->graph);//释放filter graph
}
//配置graph transcode_step-->process_input-->process_input_packet-->decode_video-->send_frame_to_filter-->ifilter_send_frame-->configure_filtergraph-->configure_output_video_filter
int configure_filtergraph(FilterGraph *fg)//vs init_simple_filtergraph 只分配实例，并初始化相互指向
{
    AVFilterInOut *inputs, *outputs, *cur;
    int ret, i, simple = filtergraph_is_simple(fg);//01.是否是simple
    const char *graph_desc = simple ? fg->outputs[0]->ost->avfilter :
                                      fg->graph_desc;
    //02.
    cleanup_filtergraph(fg);
    if (!(fg->graph = avfilter_graph_alloc()))//03.分配AVFilterGraph实例
        return AVERROR(ENOMEM);
    //04. 
    if (simple) {//04.01
        OutputStream *ost = fg->outputs[0]->ost;
        char args[512];
        AVDictionaryEntry *e = NULL;
        //04.01.01
        fg->graph->nb_threads = filter_nbthreads;
        //04.01.02 取sws_dict 放fg->graph->scale_sws_opts
        args[0] = 0;
        while ((e = av_dict_get(ost->sws_dict, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), "%s=%s:", e->key, e->value);
        }
        if (strlen(args))
            args[strlen(args)-1] = 0;
        fg->graph->scale_sws_opts = av_strdup(args);
        //04.01.02 取swr_opts 放fg->graph的aresample_swr_opts
        args[0] = 0;
        while ((e = av_dict_get(ost->swr_opts, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), "%s=%s:", e->key, e->value);
        }
        if (strlen(args))
            args[strlen(args)-1] = 0;
        av_opt_set(fg->graph, "aresample_swr_opts", args, 0);
        //04.01.03 取resample_opts 没用起来？
        args[0] = '\0';
        while ((e = av_dict_get(fg->outputs[0]->ost->resample_opts, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), "%s=%s:", e->key, e->value);
        }
        if (strlen(args))
            args[strlen(args) - 1] = '\0';
        //04.01.02 取threads 放fg->graph的threads
        e = av_dict_get(ost->encoder_opts, "threads", NULL, 0);
        if (e)
            av_opt_set(fg->graph, "threads", e->value, 0);
    } else {//04.02 只考虑线程
        fg->graph->nb_threads = filter_complex_nbthreads;
    }
    //05. 用graph_desc查找对应的graph
    if ((ret = avfilter_graph_parse2(fg->graph, graph_desc, &inputs, &outputs)) < 0)
        goto fail;
    //06. 硬件
    if (filter_hw_device || hw_device_ctx) {
        AVBufferRef *device = filter_hw_device ? filter_hw_device->device_ref
                                               : hw_device_ctx;
        for (i = 0; i < fg->graph->nb_filters; i++) {
            fg->graph->filters[i]->hw_device_ctx = av_buffer_ref(device);
            if (!fg->graph->filters[i]->hw_device_ctx) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        }
    }
    //07.如果是simple，且inputs或outputs有多个AVFilterInOut
    if (simple && (!inputs || inputs->next || !outputs || outputs->next)) {
        const char *num_inputs;
        const char *num_outputs;
        if (!outputs) {
            num_outputs = "0";
        } else if (outputs->next) {
            num_outputs = ">1";
        } else {
            num_outputs = "1";
        }
        if (!inputs) {
            num_inputs = "0";
        } else if (inputs->next) {
            num_inputs = ">1";
        } else {
            num_inputs = "1";
        }
        av_log(NULL, AV_LOG_ERROR, "Simple filtergraph '%s' was expected "
               "to have exactly 1 input and 1 output."
               " However, it had %s input(s) and %s output(s)."
               " Please adjust, or use a complex filtergraph (-filter_complex) instead.\n",
               graph_desc, num_inputs, num_outputs);
        ret = AVERROR(EINVAL);
        goto fail;
    }
    //08.每个输入，加入相应的filter，一般视频第一个filter是buffer，如果是音频第一个是abuffer
    for (cur = inputs, i = 0; cur; cur = cur->next, i++)
        if ((ret = configure_input_filter(fg, fg->inputs[i], cur)) < 0) {//tiger 主要函数
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            goto fail;
        }
    avfilter_inout_free(&inputs);//?
    //09.每个输出，加入相应的filter，一般视频第一个filter是buffersink，如果是音频第一个是buffersink
    for (cur = outputs, i = 0; cur; cur = cur->next, i++)
        configure_output_filter(fg, fg->outputs[i], cur);//tiger 主要函数
    avfilter_inout_free(&outputs);//?
    //10. TODO: CONFIG
    if ((ret = avfilter_graph_config(fg->graph, NULL)) < 0)
        goto fail;
    //11. 调整参数一致   ==>容易忘记？
    /* limit the lists of allowed formats to the ones selected, to
     * make sure they stay the same if the filtergraph is reconfigured later */
    for (i = 0; i < fg->nb_outputs; i++) {
        OutputFilter *ofilter = fg->outputs[i];
        AVFilterContext *sink = ofilter->filter;
        //关键的一些配置， 和sink对齐?
        ofilter->format = av_buffersink_get_format(sink);

        ofilter->width  = av_buffersink_get_w(sink);
        ofilter->height = av_buffersink_get_h(sink);

        ofilter->sample_rate    = av_buffersink_get_sample_rate(sink);
        ofilter->channel_layout = av_buffersink_get_channel_layout(sink);
    }
    //
    fg->reconfiguration = 1;
    //12.
    for (i = 0; i < fg->nb_outputs; i++) {
        OutputStream *ost = fg->outputs[i]->ost;
        if (!ost->enc) {
            /* identical to the same check in ffmpeg.c, needed because
               complex filter graphs are initialized earlier */
            av_log(NULL, AV_LOG_ERROR, "Encoder (codec %s) not found for output stream #%d:%d\n",
                     avcodec_get_name(ost->st->codecpar->codec_id), ost->file_index, ost->index);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&//一般音频的frame size是固定的，所以最好设置一下
            !(ost->enc->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
            av_buffersink_set_frame_size(ost->filter->filter,
                                         ost->enc_ctx->frame_size);
    }
    //13.filter graph 已经有数据了吗？
    for (i = 0; i < fg->nb_inputs; i++) {
        while (av_fifo_size(fg->inputs[i]->frame_queue)) {//是否已有数据？
            AVFrame *tmp;
            av_fifo_generic_read(fg->inputs[i]->frame_queue, &tmp, sizeof(tmp), NULL);//读
            ret = av_buffersrc_add_frame(fg->inputs[i]->filter, tmp);//加入对应的filter
            av_frame_free(&tmp);//别忘记释放
            if (ret < 0)
                goto fail;
        }
    }
    //14. 如果流结束了
    /* send the EOFs for the finished inputs */
    for (i = 0; i < fg->nb_inputs; i++) {
        if (fg->inputs[i]->eof) {
            ret = av_buffersrc_add_frame(fg->inputs[i]->filter, NULL);
            if (ret < 0)
                goto fail;
        }
    }
    //15.字幕先不看
    /* process queued up subtitle packets */
    for (i = 0; i < fg->nb_inputs; i++) {
        InputStream *ist = fg->inputs[i]->ist;
        if (ist->sub2video.sub_queue && ist->sub2video.frame) {
            while (av_fifo_size(ist->sub2video.sub_queue)) {
                AVSubtitle tmp;
                av_fifo_generic_read(ist->sub2video.sub_queue, &tmp, sizeof(tmp), NULL);
                sub2video_update(ist, &tmp);
                avsubtitle_free(&tmp);
            }
        }
    }

    return 0;

fail://16.释放filter 再filter graph
    cleanup_filtergraph(fg);
    return ret;
}

int ifilter_parameters_from_frame(InputFilter *ifilter, const AVFrame *frame)
{
    av_buffer_unref(&ifilter->hw_frames_ctx);

    ifilter->format = frame->format;

    ifilter->width               = frame->width;
    ifilter->height              = frame->height;
    ifilter->sample_aspect_ratio = frame->sample_aspect_ratio;

    ifilter->sample_rate         = frame->sample_rate;
    ifilter->channels            = frame->channels;
    ifilter->channel_layout      = frame->channel_layout;

    if (frame->hw_frames_ctx) {
        ifilter->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
        if (!ifilter->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }

    return 0;
}

int ist_in_filtergraph(FilterGraph *fg, InputStream *ist)
{
    int i;
    for (i = 0; i < fg->nb_inputs; i++)
        if (fg->inputs[i]->ist == ist)
            return 1;
    return 0;
}

int filtergraph_is_simple(FilterGraph *fg)
{
    return !fg->graph_desc;
}
