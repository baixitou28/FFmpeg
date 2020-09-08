/*
 * Copyright (c) 2010 Stefano Sabatini
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

/**
 * @file
 * libopencv wrapper functions
 */

#include "config.h"
#if HAVE_OPENCV2_CORE_CORE_C_H
#include <opencv2/core/core_c.h>
#include <opencv2/imgproc/imgproc_c.h>
#else
#include <opencv/cv.h>
#include <opencv/cxcore.h>
#endif
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

static void fill_iplimage_from_frame(IplImage *img, const AVFrame *frame, enum AVPixelFormat pixfmt)
{
    IplImage *tmpimg;//TIGER Intel Image Processing Library (IPL)
    int depth, channels_nb;

    if      (pixfmt == AV_PIX_FMT_GRAY8) { depth = IPL_DEPTH_8U;  channels_nb = 1; }
    else if (pixfmt == AV_PIX_FMT_BGRA)  { depth = IPL_DEPTH_8U;  channels_nb = 4; }
    else if (pixfmt == AV_PIX_FMT_BGR24) { depth = IPL_DEPTH_8U;  channels_nb = 3; }
    else return;

    tmpimg = cvCreateImageHeader((CvSize){frame->width, frame->height}, depth, channels_nb);//创建图像首地址，并不会初始化空间内的数据
    *img = *tmpimg;
    img->imageData = img->imageDataOrigin = frame->data[0];//视频时间
    img->dataOrder = IPL_DATA_ORDER_PIXEL;
    img->origin    = IPL_ORIGIN_TL;
    img->widthStep = frame->linesize[0];//TIGER size in bytes of each picture line
}

static void fill_frame_from_iplimage(AVFrame *frame, const IplImage *img, enum AVPixelFormat pixfmt)
{
    frame->linesize[0] = img->widthStep;//重新copy回来
    frame->data[0]     = img->imageData;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {//支持的格式
        AV_PIX_FMT_BGR24, AV_PIX_FMT_BGRA, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

typedef struct OCVContext {
    const AVClass *class;
    char *name;
    char *params;
    int (*init)(AVFilterContext *ctx, const char *args);
    void (*uninit)(AVFilterContext *ctx);
    void (*end_frame_filter)(AVFilterContext *ctx, IplImage *inimg, IplImage *outimg);//dialect,erode, smooth区别
    void *priv;
} OCVContext;

typedef struct SmoothContext {//smooth需要的额外参数
    int type;
    int    param1, param2;
    double param3, param4;
} SmoothContext;

static av_cold int smooth_init(AVFilterContext *ctx, const char *args)
{
    OCVContext *s = ctx->priv;
    SmoothContext *smooth = s->priv;
    char type_str[128] = "gaussian";
    //01. 初始化
    smooth->param1 = 3;
    smooth->param2 = 0;
    smooth->param3 = 0.0;
    smooth->param4 = 0.0;
    //02. 读入参数
    if (args)//tiger sscanf 的格式没看到过==>
        sscanf(args, "%127[^|]|%d|%d|%lf|%lf", type_str, &smooth->param1, &smooth->param2, &smooth->param3, &smooth->param4);
    //03. 验证参数
    if      (!strcmp(type_str, "blur"         )) smooth->type = CV_BLUR;
    else if (!strcmp(type_str, "blur_no_scale")) smooth->type = CV_BLUR_NO_SCALE;
    else if (!strcmp(type_str, "median"       )) smooth->type = CV_MEDIAN;
    else if (!strcmp(type_str, "gaussian"     )) smooth->type = CV_GAUSSIAN;
    else if (!strcmp(type_str, "bilateral"    )) smooth->type = CV_BILATERAL;
    else {
        av_log(ctx, AV_LOG_ERROR, "Smoothing type '%s' unknown.\n", type_str);
        return AVERROR(EINVAL);
    }

    if (smooth->param1 < 0 || !(smooth->param1%2)) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid value '%d' for param1, it has to be a positive odd number\n",
               smooth->param1);
        return AVERROR(EINVAL);
    }
    if ((smooth->type == CV_BLUR || smooth->type == CV_BLUR_NO_SCALE || smooth->type == CV_GAUSSIAN) &&
        (smooth->param2 < 0 || (smooth->param2 && !(smooth->param2%2)))) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid value '%d' for param2, it has to be zero or a positive odd number\n",
               smooth->param2);
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_VERBOSE, "type:%s param1:%d param2:%d param3:%f param4:%f\n",
           type_str, smooth->param1, smooth->param2, smooth->param3, smooth->param4);
    return 0;
}
//cvSmooth 可使用简单模糊、简单无缩放变换的模糊、中值模糊、高斯模糊、双边滤波的任何一种方法平滑图像。每一种方法都有自己的特点以及局限。 没有缩放的图像平滑仅支持单通道图像，并且支持8位到16位的转换（与cvSobel和cvLaplace相似）和32位浮点数到32位浮点数的变换格式。 简单模糊和高斯模糊支持 1- 或 3-通道， 8-比特 和 32-比特 浮点图像。这两种方法可以（in-place）方式处理图像
static void smooth_end_frame_filter(AVFilterContext *ctx, IplImage *inimg, IplImage *outimg)
{
    OCVContext *s = ctx->priv;
    SmoothContext *smooth = s->priv;
    cvSmooth(inimg, outimg, smooth->type, smooth->param1, smooth->param2, smooth->param3, smooth->param4);//降噪
}

static int read_shape_from_file(int *cols, int *rows, int **values, const char *filename,
                                void *log_ctx)
{
    uint8_t *buf, *p, *pend;
    size_t size;
    int ret, i, j, w;
    //01.文件映射
    if ((ret = av_file_map(filename, &buf, &size, 0, log_ctx)) < 0)
        return ret;
    //02.获取所有行和列
    /* prescan file to get the number of lines and the maximum width */
    w = 0;
    for (i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            if (*rows == INT_MAX) {
                av_log(log_ctx, AV_LOG_ERROR, "Overflow on the number of rows in the file\n");
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            ++(*rows);
            *cols = FFMAX(*cols, w);
            w = 0;
        } else if (w == INT_MAX) {
            av_log(log_ctx, AV_LOG_ERROR, "Overflow on the number of columns in the file\n");
            return AVERROR_INVALIDDATA;
        }
        w++;
    }
    if (*rows > (SIZE_MAX / sizeof(int) / *cols)) {
        av_log(log_ctx, AV_LOG_ERROR, "File with size %dx%d is too big\n",
               *rows, *cols);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (!(*values = av_mallocz_array(sizeof(int) * *rows, *cols))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    //03.
    /* fill *values */
    p    = buf;
    pend = buf + size-1;
    for (i = 0; i < *rows; i++) {
        for (j = 0;; j++) {
            if (p > pend || *p == '\n') {
                p++;
                break;
            } else
                (*values)[*cols*i + j] = !!av_isgraph(*(p++));
        }
    }

end:
    av_file_unmap(buf, size);

#ifdef DEBUG
    {
        char *line;
        if (!(line = av_malloc(*cols + 1)))
            return AVERROR(ENOMEM);
        for (i = 0; i < *rows; i++) {
            for (j = 0; j < *cols; j++)
                line[j] = (*values)[i * *cols + j] ? '@' : ' ';
            line[j] = 0;
            av_log(log_ctx, AV_LOG_DEBUG, "%3d: %s\n", i, line);
        }
        av_free(line);
    }
#endif

    return 0;
}

static int parse_iplconvkernel(IplConvKernel **kernel, char *buf, void *log_ctx)
{
    char shape_filename[128] = "", shape_str[32] = "rect";
    int cols = 0, rows = 0, anchor_x = 0, anchor_y = 0, shape = CV_SHAPE_RECT;
    int *values = NULL, ret = 0;
    //01.固定格式
    sscanf(buf, "%dx%d+%dx%d/%32[^=]=%127s", &cols, &rows, &anchor_x, &anchor_y, shape_str, shape_filename);
    //02.验证参数
    if      (!strcmp(shape_str, "rect"   )) shape = CV_SHAPE_RECT;
    else if (!strcmp(shape_str, "cross"  )) shape = CV_SHAPE_CROSS;
    else if (!strcmp(shape_str, "ellipse")) shape = CV_SHAPE_ELLIPSE;
    else if (!strcmp(shape_str, "custom" )) {
        shape = CV_SHAPE_CUSTOM;
        if ((ret = read_shape_from_file(&cols, &rows, &values, shape_filename, log_ctx)) < 0)
            return ret;
    } else {
        av_log(log_ctx, AV_LOG_ERROR,
               "Shape unspecified or type '%s' unknown.\n", shape_str);
        ret = AVERROR(EINVAL);
        goto out;
    }

    if (rows <= 0 || cols <= 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Invalid non-positive values for shape size %dx%d\n", cols, rows);
        ret = AVERROR(EINVAL);
        goto out;
    }

    if (anchor_x < 0 || anchor_y < 0 || anchor_x >= cols || anchor_y >= rows) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Shape anchor %dx%d is not inside the rectangle with size %dx%d.\n",
               anchor_x, anchor_y, cols, rows);
        ret = AVERROR(EINVAL);
        goto out;
    }
    //03.用于图像腐蚀，效果是图像缩小一圈且根据腐蚀元素的形状去除一些噪声
    *kernel = cvCreateStructuringElementEx(cols, rows, anchor_x, anchor_y, shape, values);
    if (!*kernel) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    av_log(log_ctx, AV_LOG_VERBOSE, "Structuring element: w:%d h:%d x:%d y:%d shape:%s\n",
           rows, cols, anchor_x, anchor_y, shape_str);
out:
    av_freep(&values);
    return ret;
}

typedef struct DilateContext {//Dialte参数
    int nb_iterations;
    IplConvKernel *kernel;
} DilateContext;

static av_cold int dilate_init(AVFilterContext *ctx, const char *args)
{
    OCVContext *s = ctx->priv;
    DilateContext *dilate = s->priv;
    char default_kernel_str[] = "3x3+0x0/rect";
    char *kernel_str = NULL;
    const char *buf = args;
    int ret;
    //01.参数
    if (args) {
        kernel_str = av_get_token(&buf, "|");

        if (!kernel_str)
            return AVERROR(ENOMEM);
    }
    //02.
    ret = parse_iplconvkernel(&dilate->kernel,
                              (!kernel_str || !*kernel_str) ? default_kernel_str
                                                            : kernel_str,
                              ctx);
    av_free(kernel_str);
    if (ret < 0)
        return ret;
    //03.参数nb_iterations验证
    if (!buf || sscanf(buf, "|%d", &dilate->nb_iterations) != 1)
        dilate->nb_iterations = 1;
    av_log(ctx, AV_LOG_VERBOSE, "iterations_nb:%d\n", dilate->nb_iterations);
    if (dilate->nb_iterations <= 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid non-positive value '%d' for nb_iterations\n",
               dilate->nb_iterations);
        return AVERROR(EINVAL);
    }
    return 0;
}

static av_cold void dilate_uninit(AVFilterContext *ctx)
{
    OCVContext *s = ctx->priv;
    DilateContext *dilate = s->priv;

    cvReleaseStructuringElement(&dilate->kernel);//释放
}

static void dilate_end_frame_filter(AVFilterContext *ctx, IplImage *inimg, IplImage *outimg)
{
    OCVContext *s = ctx->priv;
    DilateContext *dilate = s->priv;
    cvDilate(inimg, outimg, dilate->kernel, dilate->nb_iterations);//主要函数
}

static void erode_end_frame_filter(AVFilterContext *ctx, IplImage *inimg, IplImage *outimg)
{
    OCVContext *s = ctx->priv;
    DilateContext *dilate = s->priv;
    cvErode(inimg, outimg, dilate->kernel, dilate->nb_iterations);//主要函数
}

typedef struct OCVFilterEntry {
    const char *name;
    size_t priv_size;
    int  (*init)(AVFilterContext *ctx, const char *args);
    void (*uninit)(AVFilterContext *ctx);
    void (*end_frame_filter)(AVFilterContext *ctx, IplImage *inimg, IplImage *outimg);
} OCVFilterEntry;

static const OCVFilterEntry ocv_filter_entries[] = {//三种方式的初始化，主函数，析构
    { "dilate", sizeof(DilateContext), dilate_init, dilate_uninit, dilate_end_frame_filter },
    { "erode",  sizeof(DilateContext), dilate_init, dilate_uninit, erode_end_frame_filter  },
    { "smooth", sizeof(SmoothContext), smooth_init, NULL, smooth_end_frame_filter },
};

static av_cold int init(AVFilterContext *ctx)
{
    OCVContext *s = ctx->priv;
    int i;

    if (!s->name) {
        av_log(ctx, AV_LOG_ERROR, "No libopencv filter name specified\n");
        return AVERROR(EINVAL);
    }
    for (i = 0; i < FF_ARRAY_ELEMS(ocv_filter_entries); i++) {
        const OCVFilterEntry *entry = &ocv_filter_entries[i];
        if (!strcmp(s->name, entry->name)) {
            s->init             = entry->init;
            s->uninit           = entry->uninit;
            s->end_frame_filter = entry->end_frame_filter;

            if (!(s->priv = av_mallocz(entry->priv_size)))
                return AVERROR(ENOMEM);
            return s->init(ctx, s->params);
        }
    }

    av_log(ctx, AV_LOG_ERROR, "No libopencv filter named '%s'\n", s->name);
    return AVERROR(EINVAL);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    OCVContext *s = ctx->priv;

    if (s->uninit)
        s->uninit(ctx);
    av_freep(&s->priv);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    OCVContext *s = ctx->priv;
    AVFilterLink *outlink= inlink->dst->outputs[0];
    AVFrame *out;
    IplImage inimg, outimg;
    //01.得到帧
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);//复制属性

    fill_iplimage_from_frame(&inimg , in , inlink->format);//03.输入
    fill_iplimage_from_frame(&outimg, out, inlink->format);//04.输出
    s->end_frame_filter(ctx, &inimg, &outimg);//05.执行cvSmooth
    fill_frame_from_iplimage(out, &outimg, inlink->format);//06.输出
    //07.释放
    av_frame_free(&in);
    //08.
    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(OCVContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
static const AVOption ocv_options[] = {//参数
    { "filter_name",   NULL, OFFSET(name),   AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "filter_params", NULL, OFFSET(params), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(ocv);

static const AVFilterPad avfilter_vf_ocv_inputs[] = {//TIGER ？
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_ocv_outputs[] = {//tiger output没有filter_frame
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_ocv = {//tiger opencv
    .name          = "ocv",
    .description   = NULL_IF_CONFIG_SMALL("Apply transform using libopencv."),
    .priv_size     = sizeof(OCVContext),
    .priv_class    = &ocv_class,
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    .inputs        = avfilter_vf_ocv_inputs,
    .outputs       = avfilter_vf_ocv_outputs,
};
//高级图像处理初步 http://wiki.opencv.org.cn/index.php/%E9%AB%98%E7%BA%A7%E5%9B%BE%E5%83%8F%E5%A4%84%E7%90%86%E5%88%9D%E6%AD%A5