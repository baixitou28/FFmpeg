/*
 * a very simple circular buffer FIFO implementation
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
 * Copyright (c) 2006 Roman Shaposhnik
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

#include "avassert.h"
#include "common.h"
#include "fifo.h"

static AVFifoBuffer *fifo_alloc_common(void *buffer, size_t size)//tiger 分配一个AVFifoBuffer实例，并初始化buffer和size
{
    AVFifoBuffer *f;
    if (!buffer)
        return NULL;
    f = av_mallocz(sizeof(AVFifoBuffer));
    if (!f) {
        av_free(buffer);
        return NULL;
    }
    f->buffer = buffer;//指向预分配的buff
    f->end    = f->buffer + size;
    av_fifo_reset(f);//重置指针和统计值
    return f;
}

AVFifoBuffer *av_fifo_alloc(unsigned int size)//分配一个AVFifoBuffer
{
    void *buffer = av_malloc(size);//分配大的音视频数据内存
    return fifo_alloc_common(buffer, size);//分配AVFifoBuffer的内存并初始化
}

AVFifoBuffer *av_fifo_alloc_array(size_t nmemb, size_t size)//分配一个AVFifoBuffer数组
{
    void *buffer = av_malloc_array(nmemb, size);
    return fifo_alloc_common(buffer, nmemb * size);
}

void av_fifo_free(AVFifoBuffer *f)
{
    if (f) {
        av_freep(&f->buffer);//先free分配的音视频数据内存
        av_free(f);
    }
}

void av_fifo_freep(AVFifoBuffer **f)
{
    if (f) {
        av_fifo_free(*f);
        *f = NULL;
    }
}

void av_fifo_reset(AVFifoBuffer *f)//重置
{
    f->wptr = f->rptr = f->buffer; //重置指针
    f->wndx = f->rndx = 0;//统计
}

int av_fifo_size(const AVFifoBuffer *f)//可读大小
{
    return (uint32_t)(f->wndx - f->rndx);
}

int av_fifo_space(const AVFifoBuffer *f)//剩余空间
{
    return f->end - f->buffer - av_fifo_size(f);
}

int av_fifo_realloc2(AVFifoBuffer *f, unsigned int new_size)//扩张new_size，重新设置指针
{
    unsigned int old_size = f->end - f->buffer;

    if (old_size < new_size) {//一般肯定是增长的，这是保护性编程
        int len          = av_fifo_size(f);
        AVFifoBuffer *f2 = av_fifo_alloc(new_size);

        if (!f2)
            return AVERROR(ENOMEM);
        av_fifo_generic_read(f, f2->buffer, len, NULL);//复制
        f2->wptr += len;//写指针  ，这时候，读指针为0
        f2->wndx += len;
        av_free(f->buffer);//注意这里的步骤，先释放buffer
        *f = *f2;//
        av_free(f2);//再释放AVFifoBuffer实例的内存
    }
    return 0;
}

int av_fifo_grow(AVFifoBuffer *f, unsigned int size)////至少以以2倍的old_size方式增长，至少增加size
{
    unsigned int old_size = f->end - f->buffer;//总长
    if(size + (unsigned)av_fifo_size(f) < size)//溢出
        return AVERROR(EINVAL);

    size += av_fifo_size(f);//加可用长度 

    if (old_size < size)//剩下的长度容不下size
        return av_fifo_realloc2(f, FFMAX(size, 2*old_size));//至少以以2倍的old_size方式增长
    return 0;
}

/* src must NOT be const as it can be a context for func that may need
 * updating (like a pointer or byte counter) */
int av_fifo_generic_write(AVFifoBuffer *f, void *src, int size,
                          int (*func)(void *, void *, int))
{
    int total = size;
    uint32_t wndx= f->wndx;
    uint8_t *wptr= f->wptr;

    do {
        int len = FFMIN(f->end - wptr, size);//写的总长
        if (func) {
            len = func(src, wptr, len);
            if (len <= 0)//写失败
                break;
        } else {
            memcpy(wptr, src, len);//最简单的复制
            src = (uint8_t *)src + len;//当前地址
        }
// Write memory barrier needed for SMP here in theory
        wptr += len;
        if (wptr >= f->end)
            wptr = f->buffer;
        wndx    += len;
        size    -= len;
    } while (size > 0);//如果还要写，可能需要等待读，所以这里就无条件循环阻塞了
    f->wndx= wndx;//写成功，更新指针，统计值等
    f->wptr= wptr;
    return total - size;
}

int av_fifo_generic_peek_at(AVFifoBuffer *f, void *dest, int offset, int buf_size, void (*func)(void*, void*, int))
{
    uint8_t *rptr = f->rptr;

    av_assert2(offset >= 0);

    /*
     * *ndx are indexes modulo 2^32, they are intended to overflow,
     * to handle *ndx greater than 4gb.
     */
    av_assert2(buf_size + (unsigned)offset <= f->wndx - f->rndx);

    if (offset >= f->end - rptr)
        rptr += offset - (f->end - f->buffer);
    else
        rptr += offset;

    while (buf_size > 0) {
        int len;

        if (rptr >= f->end)
            rptr -= f->end - f->buffer;

        len = FFMIN(f->end - rptr, buf_size);
        if (func)
            func(dest, rptr, len);
        else {
            memcpy(dest, rptr, len);
            dest = (uint8_t *)dest + len;
        }

        buf_size -= len;
        rptr     += len;
    }

    return 0;
}

int av_fifo_generic_peek(AVFifoBuffer *f, void *dest, int buf_size,
                         void (*func)(void *, void *, int))
{
// Read memory barrier needed for SMP here in theory
    uint8_t *rptr = f->rptr;

    do {
        int len = FFMIN(f->end - rptr, buf_size);
        if (func)
            func(dest, rptr, len);
        else {
            memcpy(dest, rptr, len);
            dest = (uint8_t *)dest + len;
        }
// memory barrier needed for SMP here in theory
        rptr += len;
        if (rptr >= f->end)
            rptr -= f->end - f->buffer;
        buf_size -= len;
    } while (buf_size > 0);

    return 0;
}

int av_fifo_generic_read(AVFifoBuffer *f, void *dest, int buf_size,//循环处理，将fifo的保存的比如AVPacket*的地址，逐一copy到dest里
                         void (*func)(void *, void *, int))//举例：buf_size = sizeof(AVFrame)
{
// Read memory barrier needed for SMP here in theory
    do {
        int len = FFMIN(f->end - f->rptr, buf_size);//buf_size 第一次是sizeof(AVFrame)，第二次是剩下的长度
        if (func)
            func(dest, f->rptr, len);//特别的函数func处理
        else {
            memcpy(dest, f->rptr, len);//仅copy一下AVFifoBuffer的读指针即可
            dest = (uint8_t *)dest + len;//换下一个地址，比如len = sizeof(AVFrame) 和 av_fifo_drain 里的f->rptr += size 是一样的
        }
// memory barrier needed for SMP here in theory
        av_fifo_drain(f, len);//跳过size，f->rptr += size
        buf_size -= len;//剩下的buff减少
    } while (buf_size > 0);//举例：原始的buf_size = sizeof(AVFrame) ， buf_size>0说明还没有读全  ==>真的会读不全吗？会可能fifo满了。这里是无条件阻塞
    return 0;
}

/** Discard data from the FIFO. */
void av_fifo_drain(AVFifoBuffer *f, int size)//丢弃已读的
{
    av_assert2(av_fifo_size(f) >= size);
    f->rptr += size;//地址增加
    if (f->rptr >= f->end)
        f->rptr -= f->end - f->buffer;
    f->rndx += size;//统计增加
}
