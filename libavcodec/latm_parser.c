/*
 * copyright (c) 2008 Paul Kendall <paul@kcbbs.gen.nz>
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
 * AAC LATM parser
 */

#include <stdint.h>
#include "parser.h"

#define LATM_HEADER     0x56e000        // 0x2b7 (11 bits)
#define LATM_MASK       0xFFE000        // top 11 bits
#define LATM_SIZE_MASK  0x001FFF        // bottom 13 bits

typedef struct LATMParseContext{
    ParseContext pc;
    int count;
} LATMParseContext;

/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or -1
 */
static int latm_find_frame_end(AVCodecParserContext *s1, const uint8_t *buf,//tiger 得参看协议
                               int buf_size)
{
    LATMParseContext *s = s1->priv_data;
    ParseContext *pc    = &s->pc;
    int pic_found, i;
    uint32_t state;

    pic_found = pc->frame_start_found;
    state     = pc->state;

    if (!pic_found) {//01.
        for (i = 0; i < buf_size; i++) {//02.
            state = (state<<8) | buf[i];
            if ((state & LATM_MASK) == LATM_HEADER) {//03. latm的标识头
                i++;
                s->count  = -i;
                pic_found = 1;
                break;
            }
        }
    }

    if (pic_found) {//04.如果找到头
        /* EOF considered as end of frame */
        if (buf_size == 0)
            return 0;
        if ((state & LATM_SIZE_MASK) - s->count <= buf_size) {//05.
            pc->frame_start_found = 0;
            pc->state             = -1;
            return (state & LATM_SIZE_MASK) - s->count;//06.
        }
    }
    //07.
    s->count             += buf_size;
    pc->frame_start_found = pic_found;
    pc->state             = state;

    return END_NOT_FOUND;
}

static int latm_parse(AVCodecParserContext *s1, AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    LATMParseContext *s = s1->priv_data;
    ParseContext *pc    = &s->pc;
    int next;

    if (s1->flags & PARSER_FLAG_COMPLETE_FRAMES) {//01.不需要等待解析其他帧，只要重新开始即可
        next = buf_size;
    } else {
        next = latm_find_frame_end(s1, buf, buf_size);//02. 找到了

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {//03. 如果合并成功
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;//返回合并值
        }
    }
    *poutbuf      = buf;//返回
    *poutbuf_size = buf_size;
    return next;//next：顾名思义：返回最大值buf_size，或者比buf_size小的
}

AVCodecParser ff_aac_latm_parser = {//TIGER AAC
    .codec_ids      = { AV_CODEC_ID_AAC_LATM },
    .priv_data_size = sizeof(LATMParseContext),
    .parser_parse   = latm_parse,//为什么latm需要：
    .parser_close   = ff_parse_close
};
