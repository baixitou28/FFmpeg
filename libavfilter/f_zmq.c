/*
 * Copyright (c) 2013 Stefano Sabatini
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
 * receive commands through libzeromq and broker them to filters
 */

#include <zmq.h>
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"
#include "audio.h"
#include "video.h"

typedef struct ZMQContext {
    const AVClass *class;
    void *zmq;
    void *responder;
    char *bind_address;
    int command_count;
} ZMQContext;

#define OFFSET(x) offsetof(ZMQContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {//参数
    { "bind_address", "set bind address", OFFSET(bind_address), AV_OPT_TYPE_STRING, {.str = "tcp://*:5555"}, 0, 0, FLAGS },
    { "b",            "set bind address", OFFSET(bind_address), AV_OPT_TYPE_STRING, {.str = "tcp://*:5555"}, 0, 0, FLAGS },
    { NULL }
};

static av_cold int init(AVFilterContext *ctx)
{
    ZMQContext *zmq = ctx->priv;
    //01.
    zmq->zmq = zmq_ctx_new();
    if (!zmq->zmq) {
        av_log(ctx, AV_LOG_ERROR,
               "Could not create ZMQ context: %s\n", zmq_strerror(errno));
        return AVERROR_EXTERNAL;
    }
    //02.
    zmq->responder = zmq_socket(zmq->zmq, ZMQ_REP);
    if (!zmq->responder) {
        av_log(ctx, AV_LOG_ERROR,
               "Could not create ZMQ socket: %s\n", zmq_strerror(errno));
        return AVERROR_EXTERNAL;
    }
    //03.
    if (zmq_bind(zmq->responder, zmq->bind_address) == -1) {
        av_log(ctx, AV_LOG_ERROR,
               "Could not bind ZMQ socket to address '%s': %s\n",
               zmq->bind_address, zmq_strerror(errno));
        return AVERROR_EXTERNAL;
    }

    zmq->command_count = -1;
    return 0;
}

static void av_cold uninit(AVFilterContext *ctx)
{
    ZMQContext *zmq = ctx->priv;

    zmq_close(zmq->responder);
    zmq_ctx_destroy(zmq->zmq);
}

typedef struct Command {
    char *target, *command, *arg;
} Command;

#define SPACES " \f\t\n\r"

static int parse_command(Command *cmd, const char *command_str, void *log_ctx)
{
    const char **buf = &command_str;
    //01. 对象
    cmd->target = av_get_token(buf, SPACES);
    if (!cmd->target || !cmd->target[0]) {
        av_log(log_ctx, AV_LOG_ERROR,
               "No target specified in command '%s'\n", command_str);
        return AVERROR(EINVAL);
    }
    //02. 命令
    cmd->command = av_get_token(buf, SPACES);
    if (!cmd->command || !cmd->command[0]) {
        av_log(log_ctx, AV_LOG_ERROR,
               "No command specified in command '%s'\n", command_str);
        return AVERROR(EINVAL);
    }
    //03. 参数
    cmd->arg = av_get_token(buf, SPACES);
    return 0;
}

static int recv_msg(AVFilterContext *ctx, char **buf, int *buf_size)
{
    ZMQContext *zmq = ctx->priv;
    zmq_msg_t msg;
    int ret = 0;
    //01.初始化消息
    if (zmq_msg_init(&msg) == -1) {
        av_log(ctx, AV_LOG_WARNING,
               "Could not initialize receive message: %s\n", zmq_strerror(errno));
        return AVERROR_EXTERNAL;
    }
    //02.非阻塞获取
    if (zmq_msg_recv(&msg, zmq->responder, ZMQ_DONTWAIT) == -1) {
        if (errno != EAGAIN)
            av_log(ctx, AV_LOG_WARNING,
                   "Could not receive message: %s\n", zmq_strerror(errno));
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    *buf_size = zmq_msg_size(&msg) + 1;
    *buf = av_malloc(*buf_size);
    if (!*buf) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    memcpy(*buf, zmq_msg_data(&msg), *buf_size);
    (*buf)[*buf_size-1] = 0;

end:
    zmq_msg_close(&msg);
    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *ref)
{
    AVFilterContext *ctx = inlink->dst;
    ZMQContext *zmq = ctx->priv;

    while (1) {//死循环
        char cmd_buf[1024];
        char *recv_buf, *send_buf;
        int recv_buf_size;
        Command cmd = {0};
        int ret;
        //01.获取消息
        /* receive command */
        if (recv_msg(ctx, &recv_buf, &recv_buf_size) < 0)
            break;
        zmq->command_count++;
        //02.解析消息
        /* parse command */
        if (parse_command(&cmd, recv_buf, ctx) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Could not parse command #%d\n", zmq->command_count);
            goto end;
        }
        //03.处理命令
        /* process command */
        av_log(ctx, AV_LOG_VERBOSE,
               "Processing command #%d target:%s command:%s arg:%s\n",
               zmq->command_count, cmd.target, cmd.command, cmd.arg);
        ret = avfilter_graph_send_command(inlink->graph,
                                          cmd.target, cmd.command, cmd.arg,
                                          cmd_buf, sizeof(cmd_buf),
                                          AVFILTER_CMD_FLAG_ONE);
        send_buf = av_asprintf("%d %s%s%s",
                               -ret, av_err2str(ret), cmd_buf[0] ? "\n" : "", cmd_buf);
        if (!send_buf) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        av_log(ctx, AV_LOG_VERBOSE,
               "Sending command reply for command #%d:\n%s\n",
               zmq->command_count, send_buf);
        if (zmq_send(zmq->responder, send_buf, strlen(send_buf), 0) == -1)//04.发送执行结果
            av_log(ctx, AV_LOG_ERROR, "Failed to send reply for command #%d: %s\n",
                   zmq->command_count, zmq_strerror(ret));

    end:
        av_freep(&send_buf);
        av_freep(&recv_buf);
        recv_buf_size = 0;
        av_freep(&cmd.target);
        av_freep(&cmd.command);
        av_freep(&cmd.arg);
    }

    return ff_filter_frame(ctx->outputs[0], ref);
}

#if CONFIG_ZMQ_FILTER

#define zmq_options options
AVFILTER_DEFINE_CLASS(zmq);

static const AVFilterPad zmq_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad zmq_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_zmq = {
    .name        = "zmq",
    .description = NULL_IF_CONFIG_SMALL("Receive commands through ZMQ and broker them to filters."),
    .init        = init,
    .uninit      = uninit,
    .priv_size   = sizeof(ZMQContext),
    .inputs      = zmq_inputs,
    .outputs     = zmq_outputs,
    .priv_class  = &zmq_class,
};

#endif

#if CONFIG_AZMQ_FILTER

#define azmq_options options
AVFILTER_DEFINE_CLASS(azmq);

static const AVFilterPad azmq_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad azmq_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_azmq = {//TIGER MQ的一个示例
    .name        = "azmq",
    .description = NULL_IF_CONFIG_SMALL("Receive commands through ZMQ and broker them to filters."),
    .init        = init,
    .uninit      = uninit,
    .priv_size   = sizeof(ZMQContext),
    .inputs      = azmq_inputs,
    .outputs     = azmq_outputs,
    .priv_class  = &azmq_class,
};
/*
https://blog.csdn.net/weixin_33970449/article/details/93743539?utm_medium=distribute.pc_aggpage_search_result.none-task-blog-2~all~first_rank_v2~rank_v25-1-93743539.nonecase&utm_term=ffmpeg%E4%BD%BF%E7%94%A8zmq

编译 FFmpeg 时，配置中添加 --enable-libzmq 就可以通过 tcp 通讯给 FFmpeg 发送命令

Usage: zmqsend [-h ip:port] TARGET COMMAND ARG
  Receive commands sent through a libzmq client, and forward them to filters in the filtergraph.
  default host is localhost:5555
    The send message must be in the form:
      TARGET COMMAND [ARG]
    Depending on the result, the filter will send a reply to the client, adopting the format:
      ERROR_CODE ERROR_REASON MESSAGE

  Examples
    zmqsend hue h 90
    zmqsend hue H PI/2
    zmqsend hue s 1
    zmqsend hue b 0

    zmqsend eq contrast 0.0
    zmqsend eq brightness 1.0
    zmqsend eq saturation 1.0
    zmqsend eq gamma 1.0
    zmqsend eq gamma_r 1.0
    zmqsend eq gamma_g 1.0
    zmqsend eq gamma_b 1.0
    zmqsend eq gamma_weight 1.0
*/
#endif
