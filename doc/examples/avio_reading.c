/*
 * Copyright (c) 2014 Stefano Sabatini
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
  * libavformat AVIOContext API example.
  *
  * Make libavformat demuxer access media content through a custom
  * AVIOContext read callback.
  * @example avio_reading.c
  */

  //#include <libavformat/rtsp.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include <sys/uio.h>
#include <fcntl.h>

#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/file.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h> 

#define BAND_INSIDE

struct timeval callback_tm;
int g_force_quit = 0;
int interrupt_callback(void* ctx) {
	AVFormatContext* fmtCtx = (AVFormatContext*)ctx;
	struct timeval now_tm;
	if (g_force_quit) {
		printf("I do not want run avformat_find_stream_info this time.");
		return 1;
	}
	gettimeofday(&now_tm, NULL);
	long int delay = (now_tm.tv_sec - callback_tm.tv_sec) * 1000000 + (now_tm.tv_usec - callback_tm.tv_usec);
	if (delay > 7000*1000 * 1000L) {//
		return 1;
	}

	return 0;
}
static int ffmpeg_lockmgr_cb(void** arg, enum AVLockOp op)
{
	pthread_mutex_t* mutex = *arg;
	int err;

	switch (op) {
	case AV_LOCK_CREATE:
		mutex = malloc(sizeof(*mutex));
		if (!mutex)
			return AVERROR(ENOMEM);
		if ((err = pthread_mutex_init(mutex, NULL))) {
			free(mutex);
			return AVERROR(err);
		}
		*arg = mutex;
		return 0;
	case AV_LOCK_OBTAIN:
		if ((err = pthread_mutex_lock(mutex)))
			return AVERROR(err);

		return 0;
	case AV_LOCK_RELEASE:
		if ((err = pthread_mutex_unlock(mutex)))
			return AVERROR(err);

		return 0;
	case AV_LOCK_DESTROY:
		if (mutex)
			pthread_mutex_destroy(mutex);
		free(mutex);
		*arg = NULL;
		return 0;
	}
	return 1;
}

int g_fd = 0;
int g_fd2 = 0;
void my_simple_bind_comm(int* fd, const char* portname) {
	const char* hostname = 0; /* wildcard */
	//const char* portname="22345";
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = 0;
	hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
	struct addrinfo* res = 0;
	int flags;
	int sock;
	int err = getaddrinfo(hostname, portname, &hints, &res);
	if (err != 0) {
		printf("my_simple_bind failed to resolve local socket address (err=%d)\n", err);
	}
	sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);


	if (sock == -1) {
		printf("my_simple_bind %s", strerror(errno));
	}
	if (bind(sock, res->ai_addr, res->ai_addrlen) == -1) {
		printf("my_simple_bind %s", strerror(errno));
	}
	*fd = sock;
	 //flags = fcntl(g_fd, F_GETFL, 0);//TODO avformat_find_stream_info will stuck
	 //err = fcntl(g_fd, F_SETFL, flags|O_NONBLOCK);
	// printf("my_simple_bind fcntl%s, err:%d\n", strerror(errno), err);
}

void my_simple_bind() {
	const char* portname = "22345";
	my_simple_bind_comm(&g_fd, portname);
}
void my_simple_bind2() {
	const char* portname = "22347";
	my_simple_bind_comm(&g_fd2, portname);
}

char g_buffer[4096];
int my_simple_recv() {
	int local_error = 0;
	struct sockaddr_storage src_addr;
	socklen_t src_addr_len = sizeof(src_addr);
	ssize_t count = recvfrom(g_fd, g_buffer, sizeof(g_buffer), 0, (struct sockaddr*) & src_addr, &src_addr_len);
	//ssize_t count = read(g_fd, g_buffer, sizeof(g_buffer));
	local_error = errno;
	//printf("my_simple_recv count:%d\n", count);
	if (count == -1) {
		if (local_error == EAGAIN || local_error == EWOULDBLOCK) {
			count = -EWOULDBLOCK;
		}else
		  printf("my_simple_recv error:%s, errno:%d, count:%d\n", strerror(local_error), local_error, count);   
	}
	else if (count == sizeof(g_buffer)) {
		printf("my_simple_recv datagram too large for buffer: truncated\n");
	}
	else {
		//handle_datagram(buffer,count);
	}
	return count;
}

char g_buffer2[4096];
int my_simple_recv2() {
	int local_error = 0;
	struct sockaddr_storage src_addr;
	socklen_t src_addr_len = sizeof(src_addr);
	ssize_t count = recvfrom(g_fd2, g_buffer2, sizeof(g_buffer2), 0, (struct sockaddr*) & src_addr, &src_addr_len);
	local_error = errno;
	//printf("my_simple_recv2 count:%d\n", count);
	if (count == -1) {
		if (local_error == EAGAIN || local_error == EWOULDBLOCK) {
			count = -EWOULDBLOCK;
		}else
			printf("my_simple_recv2 error:%s, errno:%d, count:%d\n", strerror(local_error), local_error, count);   
	}
	else if (count == sizeof(g_buffer2)) {
		printf("my_simple_recv2 datagram too large for buffer: truncated\n");
	}
	else {
		//handle_datagram(buffer,count);
	}
	return count;
}


struct buffer_data {
	uint8_t* ptr;
	size_t size; ///< size left in the buffer
};

static int my_read_packet(void* opaque, uint8_t * buf, int buf_size)
{
	struct buffer_data* bd = (struct buffer_data*)opaque;
	buf_size = FFMIN(buf_size, bd->size);
	//printf("read_packet bd:%p, ptr:%p, size:%zu\n", bd, bd->ptr, bd->size);
	if (!buf_size)
		return AVERROR_EOF;
	/* copy internal buffer data to buf */
	memcpy(buf, bd->ptr, buf_size);
	bd->ptr += buf_size;
	bd->size -= buf_size;

	return buf_size;
}
static int my_write_packet(void* opaque, uint8_t * buf, int buf_size)
{
	// av_hex_dump_log(NULL, AV_LOG_INFO, buf, buf_size);
	printf("my_write_packet neglect all buf:%p, buf_size:%zu\n", buf, buf_size);
	return buf_size;
}

int g_pos = 0;
int loop = 1;
static int get_packet_from_mem(void* opaque, uint8_t * buf, int buf_size) {
	//printf("get_packet_from_mem start...\n");    
	if (loop == 1) {
		struct buffer_data* bd = (struct buffer_data*)opaque;
		buf_size = FFMIN(buf_size, bd->size);

		// printf("get_packet_from_mem first bd:%p, ptr:%p, size:%zu\n", bd, bd->ptr, bd->size);

		 /* copy internal buffer data to buf */
		memcpy(buf, bd->ptr, buf_size);
		bd->ptr += buf_size;
		bd->size -= buf_size;
		loop += 1;
		av_hex_dump_log(NULL, AV_LOG_INFO, buf, FFMIN(buf_size, 200));
		return buf_size;

	}
	else if (loop == 2) {
		buf_size = 0;
		printf("\nget_packet_from_mem set buf to 0 and quit sdp parsing. size:%u, loop:%d\n", buf_size, loop);
		loop += 1;
		return AVERROR_EOF;
	}
/*
	else if (loop > 2 && loop < 300) {//delete tthis
		buf_size = 2;
		printf("\nget_packet_from_mem set buf size to 2 and test timeout size:%u, loop:%d\n", buf_size, loop);
		loop += 1;
		return  buf_size;
	}

	else if (loop >= 300) {
		buf_size = 2;
		printf("\nget_packet_from_mem AVERROR(ENOMEM). size:%u, loop:%d\n", buf_size, loop);
		loop += 1;
		return  AVERROR(ENOMEM);
	}
*/
	else {
		int got = my_simple_recv();
		buf_size = got;

		//printf("\nget_packet_from_mem  size:%u, loop:%d,buf:%p, g_buffer:%p\n",  buf_size, loop, buf, g_buffer);
		if (buf_size > 0) {
			memcpy(buf, (char*)g_buffer, buf_size);
			loop += 1;
		}

		//av_hex_dump_log(NULL, AV_LOG_INFO, buf, FFMIN(buf_size,200));
		return buf_size;
	}
}


int loop2 = 1;
static int get_packet_from_mem2(void* opaque, uint8_t * buf, int buf_size) {
	//printf("get_packet_from_mem2 start...\n");    
	if (loop2 == 1) {
		struct buffer_data* bd = (struct buffer_data*)opaque;
		buf_size = FFMIN(buf_size, bd->size);

		//printf("get_packet_from_mem2 first bd:%p, ptr:%p, size:%zu\n", bd, bd->ptr, bd->size);

		/* copy internal buffer data to buf */
		memcpy(buf, bd->ptr, buf_size);
		bd->ptr += buf_size;
		bd->size -= buf_size;
		loop2 += 1;
		av_hex_dump_log(NULL, AV_LOG_INFO, buf, FFMIN(buf_size, 200));
		return buf_size;

	}
	else if (loop2 == 2) {
		buf_size = 0;
		printf("\nget_packet_from_mem2 set buf to 0 and quit sdp parsing. size:%u, loop:%d\n", buf_size, loop);
		loop2 += 1;
		return AVERROR_EOF;
	}
/*
	else if (loop2 > 2 && loop2 < 300) {//delete this
		buf_size = 2;
		printf("\nget_packet_from_mem2 set buf to 0 and quit sdp parsing. size:%u, loop2:%d\n", buf_size, loop2);
		loop2 += 1;
		return  buf_size; 
	}

	else if (loop2 >= 300) {
		buf_size = 2;
		printf("\nget_packet_from_mem2 AVERROR(ENOMEM). size:%u, loop2:%d\n", buf_size, loop2);
		loop2 += 1;
		return  AVERROR(ENOMEM);
	}
*/
	else {
		int got = my_simple_recv2();
		buf_size = got;

		//printf("\nget_packet_from_mem2  size:%u, loop2:%d,buf:%p, g_buffer:%p\n",  buf_size, loop2, buf, g_buffer);
		if (buf_size > 0) {
			memcpy(buf, (char*)g_buffer2, buf_size);
			loop2 += 1;
		}

		//av_hex_dump_log(NULL, AV_LOG_INFO, buf, FFMIN(buf_size, 200));
		return buf_size;
	}
}

void print_ifmt_context(AVFormatContext* fmt_ctx) {
	if (fmt_ctx->nb_streams && fmt_ctx->streams[0]->codec) {
		int codec_type0 = fmt_ctx->streams[0] && fmt_ctx->streams[0]->codec ? fmt_ctx->streams[0]->codec->codec_type : 0;
		int codec_type1 = fmt_ctx->nb_streams>= 2 && fmt_ctx->streams[1] && fmt_ctx->streams[1]->codec ? fmt_ctx->streams[1]->codec->codec_type : 0;
		printf("nb_streams:%d,codec_type:%d,%d, probesize:%d,max_analyze_duration2:%d. fps_probe_size:%d. format_probesize:%d, packet_size:%d video_codec_id:%d, audio_codec_id:%d\n",
			fmt_ctx->nb_streams, codec_type0, codec_type1, fmt_ctx->probesize, fmt_ctx->max_analyze_duration, fmt_ctx->fps_probe_size, fmt_ctx->format_probesize, fmt_ctx->packet_size, fmt_ctx->video_codec_id, fmt_ctx->audio_codec_id);
	}
	else {
		printf("nb_streams:%d, probesize:%d,max_analyze_duration:%d. fps_probe_size:%d. format_probesize:%d, packet_size:%d video_codec_id:%d, audio_codec_id:%d\n",
			fmt_ctx->nb_streams,  fmt_ctx->probesize, fmt_ctx->max_analyze_duration, fmt_ctx->fps_probe_size, fmt_ctx->format_probesize, fmt_ctx->packet_size, fmt_ctx->video_codec_id, fmt_ctx->audio_codec_id);

	}
}
int open_stream(AVFormatContext * *pfmt_ctx, char* input_filename, int* index,
	int custom_read(void* opaque, uint8_t * buf, int buf_size),
	int custom_write(void* opaque, uint8_t * buf, int buf_size)
) {
	AVFormatContext* fmt_ctx = 0;
	uint8_t* buffer = malloc(4096 * 64 * 10);
	uint8_t* avio_ctx_buffer = NULL;

	size_t buffer_size = 4096 * 64 * 10;
	AVIOContext* avio_ctx = NULL;
	size_t avio_ctx_buffer_size = 4096 * 4 * 10;

	int ret = 0;
	struct buffer_data bd1 = { 0 };
	struct buffer_data* bd = &bd1;


	//
	int64_t avio_pos = 0;
	//    int frame_index=0;
	//    int loop = 0;
	int i = 0;
	//   int videoindex=-1;
	//   int audioindex=-1;    
	   //AVOutputFormat *ofmt = NULL;
	  // AVFormatContext *ofmt_ctx = NULL;


	   /* slurp file content into buffer */
	ret = av_file_map(input_filename, &buffer, &buffer_size, 0, NULL);
	if (ret < 0)
		return -1;


	/* fill opaque structure used by the AVIOContext read callback */
	bd->ptr = buffer;
	bd->size = buffer_size;


	if (!(fmt_ctx = avformat_alloc_context())) {
		ret = AVERROR(ENOMEM);
		return -2;
	}

	*pfmt_ctx = fmt_ctx;


	avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
	if (!avio_ctx_buffer) {
		ret = AVERROR(ENOMEM);
		return -3;
	}

	avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
		0, bd, custom_read, custom_write, NULL);
	if (!avio_ctx) {
		ret = AVERROR(ENOMEM);
		return -4;
	}
	fmt_ctx->pb = avio_ctx;
	//
	AVDictionary* options = NULL;
	av_dict_set(&options, "sdp_flags", "custom_io", 0);
	//ff_rtsp_options
	av_dict_set(&options, "rtsp_flags", "4", 0);
	av_dict_set(&options, "analyzeduration", "20000", 0);
	av_dict_set(&options, "stimeout", "1000000", 0);//rtsp uS 
	//av_dict_set(&options, "timeout", "3000000", 0);//udp s？
	//   avio_pos = avio_tell(avio_ctx);
   //printf("avio_ctx:%p, pos:%lld\n", avio_ctx, avio_pos);   

	fmt_ctx->iformat = av_find_input_format("sdp");//sdp, rtp 
	
	fmt_ctx->interrupt_callback.callback = interrupt_callback;//is it right?
	fmt_ctx->interrupt_callback.opaque = fmt_ctx;

//    fmt_ctx->debug = AV_LOG_TRACE;
//printf("fmt_ctx->iformat->flags:%d ..\n", fmt_ctx->iformat->flags);  
//      avio_pos = avio_tell(avio_ctx);
//printf("av_find_input_format avio_pos:%lld, avio_ctx->pos:%lld,bytes_read:%lld, eof_reached:%d , max_packet_size:%d, error:%d\n", 
//    avio_pos, avio_ctx->pos, avio_ctx->bytes_read,  avio_ctx->eof_reached, avio_ctx->max_packet_size, avio_ctx->error);
	  //avio_skip(avio_ctx, avio_pos); 
	 // avio_ctx->eof_reached = 0;  
//printf("av_find_input_format avio_pos:%lld, avio_ctx->pos:%lld,bytes_read:%lld, eof_reached:%d , max_packet_size:%d, error:%d\n", 
//    avio_pos, avio_ctx->pos, avio_ctx->bytes_read,  avio_ctx->eof_reached, avio_ctx->max_packet_size, avio_ctx->error);

	//fmt_ctx->flags |= AVFMT_FLAG_NONBLOCK;
//printf("AVFMT_FLAG_NONBLOCK:%d\n", fmt_ctx->flags );  
	// fmt_ctx->iformat->flags |= 0x20000;  
//printf("fmt_ctx->iformat->flags:%d ...\n", fmt_ctx->iformat->flags);  
	printf(" avformat_open_input SDP ...... start\n");
	print_ifmt_context(fmt_ctx);


	ret = avformat_open_input(&fmt_ctx, NULL, NULL, &options);
	if (ret < 0) {
		fprintf(stderr, "Could not open input SDP (error '%s')\n", av_err2str(ret));
		return -5;
	}
	av_dump_format(fmt_ctx, 0, "sdp", 0);
	print_ifmt_context(fmt_ctx);
	printf(" avformat_open_input SDP ...... end...AVFMT_FLAG_NONBLOCK:%d\n", fmt_ctx->flags);



	//SDP解析结束了，需要重新标记一下
	avio_pos = avio_tell(avio_ctx);
	printf("avformat_open_input avio_pos:%lld, avio_ctx->pos:%lld,bytes_read:%lld, eof_reached:%d , max_packet_size:%d, error:%d\n",
		avio_pos, avio_ctx->pos, avio_ctx->bytes_read, avio_ctx->eof_reached, avio_ctx->max_packet_size, avio_ctx->error);
	avio_skip(avio_ctx, avio_pos);
	avio_ctx->eof_reached = 0;//ffplay里面就是用这个hack
	avio_pos = avio_tell(avio_ctx);
	printf("avformat_open_input avio_pos:%lld, avio_ctx->pos:%lld,bytes_read:%lld, eof_reached:%d , max_packet_size:%d, error:%d\n",
		avio_pos, avio_ctx->pos, avio_ctx->bytes_read, avio_ctx->eof_reached, avio_ctx->max_packet_size, avio_ctx->error);


	//fmt_ctx->pb = 0;
	//    avformat_flush(fmt_ctx);
	//fmt_ctx->pb = avio_ctx;
	//    avformat_flush(fmt_ctx);
	//    avio_flush(fmt_ctx->pb);
	//printf(" avformat_flush...... end...avio_ctx:%p, avio_ctx->write_flag:%d\n", avio_ctx, avio_ctx->write_flag);
	if (fmt_ctx->pb && avio_tell(fmt_ctx->pb))
		printf("tell:%d, bytes_read:%d\n", avio_tell(fmt_ctx->pb), fmt_ctx->pb->bytes_read);
	
#ifdef BAND_INSIDE
	printf(" avformat_find_stream_info...... start...avio_ctx->write_flag:%d\n", avio_ctx->write_flag);
	//if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {//delete here ...................................
	{
		//int flags = fmt_ctx->flags;
		//fmt_ctx->flags |= AVFMT_FLAG_NOPARSE;
		g_force_quit = 1;
		ret = avformat_find_stream_info(fmt_ctx, NULL);
		g_force_quit = 0;
		//fmt_ctx->flags = flags;
	}
		if (ret < 0) {
			fprintf(stderr, "Could not find stream information (error '%s')\n", av_err2str(ret));

			for (i = 0; i < fmt_ctx->nb_streams; i++) {
				if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO || fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {// AVMEDIA_TYPE_AUDIO
				   // RTSPState* rt = s->priv_data;
				   // RTSPSteam* rtsp_st = 0;            
					*index = i;

					if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
						printf("\n****stream is found: nb_streams:%d, index:%d, AVMEDIA_TYPE_AUDIO codec_type:%d ***\n", fmt_ctx->nb_streams, *index, fmt_ctx->streams[i]->codec->codec_type);
					else
						printf("\n****stream is found: nb_streams:%d, index:%d, AVMEDIA_TYPE_VIDEO codec_type:%d ***\n", fmt_ctx->nb_streams, *index, fmt_ctx->streams[i]->codec->codec_type);

					//rtsp_st = rt->rtsp_streams[rt->nb_rtsp_streams - 1];
				   // printf( " rtsp_st->feedback:%d \n", rtsp_st->feedback);
				   // rtsp_st->feedback = 0; 
					break;
				}
			}

			return -6;
		}
		print_ifmt_context(fmt_ctx);
		printf(" avformat_find_stream_info...... end...avio_ctx->write_flag:%d\n", avio_ctx->write_flag);
	//}
#else
	if (0) {
		AVStream* st = fmt_ctx->streams[0];
		if (st->internal->avctx) {

		}

		}
		if(0){
			AVStream* stream = fmt_ctx->streams[0];
			AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);//获取解码器
			AVCodecContext* codec_ctx;
			if (!dec) {
				av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
				return AVERROR_DECODER_NOT_FOUND;
			}
			codec_ctx = avcodec_alloc_context3(dec);//创建context
			if (!codec_ctx) {
				av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
				return AVERROR(ENOMEM);
			}
			ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);//从流里面获取格式信息，//TIGER 也可以自己赋值
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
					"for stream #%u\n", i);
				return ret;
			}

			/* Reencode video & audio and remux subtitles etc. */
			if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
				|| codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
				if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
					codec_ctx->framerate = av_guess_frame_rate(fmt_ctx, stream, NULL);//如果是视频还要猜测帧率
				/* Open decoder */
				ret = avcodec_open2(codec_ctx, dec, NULL);//打开解码的上下文
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
					return ret;
				}
			}
			//stream_ctx[i].dec_ctx = codec_ctx;//放在流的dec_ctx结构中
		}

#endif
	avio_ctx->write_flag = 1;//this is important...  用函数？AVIO_FLAG_READ



		//av_dump_format(fmt_ctx, 0, input_filename, 0);
		//RTSPState* rt = fmt_ctx->priv_data;
		//rt->rtsp_flags |= 4;//RTPSP_FLAG_CUSTOM_IO;
	for (i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO || fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {// AVMEDIA_TYPE_AUDIO
		   // RTSPState* rt = s->priv_data;
		   // RTSPSteam* rtsp_st = 0;            
			*index = i;

			if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
				printf("\n****stream is found: nb_streams:%d, index:%d, AVMEDIA_TYPE_AUDIO codec_type:%d ***\n", fmt_ctx->nb_streams, *index, fmt_ctx->streams[i]->codec->codec_type);
			else
				printf("\n****stream is found: nb_streams:%d, index:%d, AVMEDIA_TYPE_VIDEO codec_type:%d ***\n", fmt_ctx->nb_streams, *index, fmt_ctx->streams[i]->codec->codec_type);

			//rtsp_st = rt->rtsp_streams[rt->nb_rtsp_streams - 1];
		   // printf( " rtsp_st->feedback:%d \n", rtsp_st->feedback);
		   // rtsp_st->feedback = 0; 
			break;
		}
	}

	return 0;

}

int create_output_stream(AVFormatContext * ofmt_ctx, AVFormatContext * ifmt_ctx) {
	//AVOutputFormat *ofmt = NULL;
	int i = 0;
	int ret = 0;
	printf("create_output_stream \n");
	// ofmt = ofmt_ctx->oformat;
	for (i = 0; i < ifmt_ctx->nb_streams; i++) {
		//Create output AVStream according to input AVStream
		AVStream* in_stream = ifmt_ctx->streams[i];
		printf("start allocating output stream codec_type:%d\n", in_stream->codec->codec_type);
		AVStream* out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
		if (!out_stream) {
			printf("Failed allocating output stream\n");
			ret = AVERROR_UNKNOWN;
			return -1;
		}

		//Copy the settings of AVCodecContext
		ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
		ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
		if (ret < 0) {
			printf("Failed to copy context from input to output stream codec context\n");
			return -2;
		}
		out_stream->codec->codec_tag = 0;
		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
			out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	return 0;
}

int main(int argc, char* argv[])
{
	AVFormatContext* fmt_ctx = NULL;
	//AVIOContext *avio_ctx = NULL;
	AVFormatContext* fmt_ctx2 = NULL;
	// AVIOContext *avio_ctx2 = NULL;    
	 //uint8_t *buffer = malloc(4096* 64* 10);
	 //uint8_t *buffer2 = malloc(4096* 64* 10);    
	 //uint8_t* avio_ctx_buffer = NULL;
	 //uint8_t* avio_ctx_buffer2 = NULL;    
	 //size_t buffer_size = 4096* 64* 10;
	 //size_t buffer_size2 = 4096* 64* 10;    
	 //size_t avio_ctx_buffer_size = 4096*4* 10;
	 //size_t avio_ctx_buffer_size2 = 4096*4* 10;    
	char* input_filename = NULL;
	char* input_filename2 = NULL;
	int ret = 0;
	// struct buffer_data bd = { 0 };
	// struct buffer_data bd2 = { 0 };    

 //
	int64_t avio_pos = 0;
	int frame_index = 0;
	int audio_frame_loop = 0;
	int video_frame_loop = 0;
	int loop = 0;
	int i = 0;
	int videoindex = -1;
	int audioindex = -1;
	AVOutputFormat* ofmt = NULL;
	AVFormatContext* ofmt_ctx = NULL;

	const char* out_filename = "tiger.wav";//"rtmp://10.1.9.117:1935/hls/tiger";//  1935 rtmp://10.1.9.117:1935/live/50000_0
	struct timeval video_tm;
	struct timeval audio_tm;
	struct timeval now_tm;
	long int delay = 0;
	AVDictionaryEntry* tag = NULL;

	av_log_set_level(AV_LOG_TRACE);
	printf("log_level:%d\n", av_log_get_level());


	//ffmpeg -re -i test.flv -vcodec  copy -an -f rtp rtp://10.1.9.122:22345 -acodec  copy -vn -f rtp rtp://10.1.9.122:22347
	//ffmpeg -re -i test.flv -vcodec  copy -an -f rtp rtp://192.168.122.18:22345 -acodec  copy -vn -f rtp rtp://192.168.122.18:22347
	//ffplay -nodisp  -protocol_whitelist "file,crypto,rtp,udp" -i local.sdp //tcp,http,https//local.sdp:o=- 0 0 IN IP4 10.1.9.122 ip不要为本地

	if (argc != 3) {
		fprintf(stderr, "usage: %s video_sdp_input_file  audio_sdp_input_file\n"
			"API example program to show how to read from a custom buffer "
			"accessed through AVIOContext.\n", argv[0]);
		//return 1;
		input_filename = "my_video.sdp";
		input_filename2 = "my_audio.sdp";
	}
	else {
		input_filename = argv[1];
		input_filename2 = argv[2];
	}


	const char* portname = "22345";
	my_simple_bind_comm(&g_fd, portname);
	const char* portname2 = "22347";
	my_simple_bind_comm(&g_fd2, portname2);
	//my_simple_bind();
	//my_simple_bind2();      

		/* register codecs and formats and other lavf/lavc components*/
	av_register_all();
	avformat_network_init();
	ret = av_lockmgr_register(ffmpeg_lockmgr_cb);
	if (ret < 0)
	{
		fprintf(stderr, "av_lockmgr_register failed (%d)\n", ret);
		abort();
	}
	gettimeofday(&callback_tm, NULL);
	gettimeofday(&video_tm, NULL);

	printf("open_stream 1\n");
	if (open_stream(&fmt_ctx, input_filename, &videoindex, &get_packet_from_mem, &my_write_packet) < 0) {
		printf("quit 1\n");
		return -1;
	}
	printf("av_dict_get \n");
	while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
		printf("%s=%s\n", tag->key, tag->value);

	gettimeofday(&now_tm, NULL);
	delay = (now_tm.tv_sec  - video_tm.tv_sec) * 1000000 + (now_tm.tv_usec - video_tm.tv_usec);
	printf("open_stream 1 delay:%ld, fmt_ctx:%p, videoindex:%d\n", delay, videoindex, fmt_ctx);

	av_dump_format(fmt_ctx, 0, 0, 0);
	gettimeofday(&audio_tm, NULL);
	printf("open_stream 2\n");
	if (open_stream(&fmt_ctx2, input_filename2, &audioindex, &get_packet_from_mem2, &my_write_packet) < 0) {
		printf("quit 2\n");
		return -2;

	}
	printf("av_dict_get 2\n");
	while ((tag = av_dict_get(fmt_ctx2->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
		printf("%s=%s\n", tag->key, tag->value);

	gettimeofday(&now_tm, NULL);
	delay = (now_tm.tv_sec - audio_tm.tv_sec) * 1000000 + (now_tm.tv_usec - audio_tm.tv_usec);
	printf("open_stream 2 delay:%ld, fmt_ctx2:%p, audioindex:%d\n", delay, audioindex, fmt_ctx2);

	av_dump_format(fmt_ctx2, 0, 0, 0);


	if (fmt_ctx == 0 || fmt_ctx2 == 0) {
		printf("quit 3 fmt_ctx:%p fmt_ctx2:%p\n", fmt_ctx, fmt_ctx2);
		return -3;
	}

	printf("tiger ofmt_ctx avformat_alloc_output_context2:\n");
	avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", out_filename);

	if (!ofmt_ctx) {
		printf("Could not create output context\n");
		ret = AVERROR_UNKNOWN;
		goto end;
	}
//	p* fmt_ctx->streams[0]
//		p* fmt_ctx->streams[0]->codec
////p* fmt_ctx->streams[0]->parser

	create_output_stream(ofmt_ctx, fmt_ctx);
	av_dump_format(ofmt_ctx, 0, out_filename, 1);
#ifndef BAND_INSIDE//#if 0 //
	ofmt_ctx->streams[audioindex]->start_time = 0;
	ofmt_ctx->streams[audioindex]->first_dts = 0;
	ofmt_ctx->streams[audioindex]->cur_dts = 0;	
	ofmt_ctx->streams[audioindex]->info = 0;
	ofmt_ctx->streams[audioindex]->last_IP_pts = 0;
	ofmt_ctx->streams[audioindex]->last_IP_duration = 0;
	ofmt_ctx->streams[audioindex]->codec_info_nb_frames = 1;//?
	ofmt_ctx->streams[audioindex]->pts_wrap_reference = -480000; 
	ofmt_ctx->streams[audioindex]->pts_wrap_behavior = 1;

	ofmt_ctx->streams[audioindex]->codec->time_base.num = 1;
	ofmt_ctx->streams[audioindex]->codec->time_base.den = 8000;

	ofmt_ctx->streams[audioindex]->codec->sample_rate = 8000;//same
	ofmt_ctx->streams[audioindex]->codec->channels = 1;//same
	ofmt_ctx->streams[audioindex]->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	ofmt_ctx->streams[audioindex]->codec->frame_size = 1024;//; 1024;
	ofmt_ctx->streams[audioindex]->codec->frame_number = 1;
	ofmt_ctx->streams[audioindex]->codec->channel_layout = 4;
	ofmt_ctx->streams[audioindex]->codec->profile = 0;
	//ofmt_ctx->streams[audioindex]->parser = AVCodecParserContext
	//ofmt_ctx->streams[audioindex]->codec->codec_descriptor = ""; AVCodecDescriptor
	   
	ofmt_ctx->streams[audioindex]->codec->sw_pix_fmt = AV_PIX_FMT_NONE;//应该不需要


	//fmt_ctx->streams[0]->parser：ff_aac_parser
#endif
	if (1) {
		ofmt_ctx->streams[audioindex]->codecpar->profile = 0;
		ofmt_ctx->streams[audioindex]->codecpar->frame_size = 1024;//; 1024;
		ofmt_ctx->streams[audioindex]->codec->profile = 0;
		ofmt_ctx->streams[audioindex]->codec->frame_size = 1024;//; 1024;
	}
	printf("tiger ofmt_ctx create_output_stream nb_streams:%d, sample_rate:%d\n", ofmt_ctx->nb_streams, ofmt_ctx->streams[audioindex]->codec->sample_rate);
	//ofmt_ctx->streams[audioindex]->codec->sample_rate = 8000;
	//ofmt_ctx->streams[audioindex]->codec->channels = 1;
	create_output_stream(ofmt_ctx, fmt_ctx2);
	printf("tiger ofmt_ctx2 create_output_stream nb_streams:%d, %d\n", ofmt_ctx->nb_streams, ofmt_ctx->flags | AVFMT_NODIMENSIONS);
#ifndef BAND_INSIDE
	ofmt_ctx->streams[videoindex]->start_time = 0;
	ofmt_ctx->streams[videoindex]->avg_frame_rate.den = 1;
	ofmt_ctx->streams[videoindex]->avg_frame_rate.num = 25;

	ofmt_ctx->streams[videoindex]->first_dts = 0;
	ofmt_ctx->streams[videoindex]->cur_dts = 0;
	ofmt_ctx->streams[videoindex]->last_IP_duration = 0;

	ofmt_ctx->streams[videoindex]->r_frame_rate.den = 1;
	ofmt_ctx->streams[videoindex]->r_frame_rate.num = 25;

	ofmt_ctx->streams[videoindex]->pts_wrap_reference = -5400000;
	ofmt_ctx->streams[videoindex]->pts_wrap_behavior = 1;
	
	ofmt_ctx->streams[videoindex]->info = 0;
	//ofmt_ctx->streams[videoindex]->last_IP_pts = 0;

	ofmt_ctx->streams[videoindex]->codec_info_nb_frames = 1;//?
	ofmt_ctx->streams[videoindex]->pts_wrap_reference = -480000;
	ofmt_ctx->streams[videoindex]->pts_wrap_behavior = 1;


	ofmt_ctx->streams[videoindex]->codecpar->width = 704;
	ofmt_ctx->streams[videoindex]->codecpar->height = 576;
	ofmt_ctx->streams[videoindex]->codec->pix_fmt = AV_PIX_FMT_YUV420P;
	ofmt_ctx->streams[videoindex]->codec->chroma_sample_location = AVCHROMA_LOC_LEFT;
	ofmt_ctx->streams[videoindex]->codec->width = 704;//del
	ofmt_ctx->streams[videoindex]->codec->height = 576;//de
	ofmt_ctx->streams[videoindex]->codec->coded_width = 704;//del
	ofmt_ctx->streams[videoindex]->codec->coded_height = 576;//del
	//ofmt_ctx->streams[videoindex]->codec->flags &= AVFMT_NODIMENSIONS;
#endif
	
		if (ofmt_ctx->streams[videoindex]->codecpar->height == 0 || ofmt_ctx->streams[videoindex]->codecpar->width == 0) {
			printf("tiger warning av_dump_format codecpar width:%d, height:%d\n", ofmt_ctx->streams[videoindex]->codecpar->width, ofmt_ctx->streams[videoindex]->codecpar->height);
			ofmt_ctx->streams[videoindex]->codecpar->width = 704;
			ofmt_ctx->streams[videoindex]->codecpar->height = 576;
		}

	if (ofmt_ctx->streams[videoindex]->codec->width == 0) {//please delete this
		printf("tiger warning av_dump_format codec width:%d, height:%d\n", ofmt_ctx->streams[videoindex]->codec->width, ofmt_ctx->streams[videoindex]->codec->height);
		ofmt_ctx->streams[videoindex]->codec->width = 704;
		ofmt_ctx->streams[videoindex]->codec->height = 576;
		printf("tiger warning av_dump_format codec after setting: width:%d, height:%d\n", ofmt_ctx->streams[videoindex]->codec->width, ofmt_ctx->streams[videoindex]->codec->height);
	}
	else {
		printf("tiger av_dump_format width:%d, height:%d\n", ofmt_ctx->streams[videoindex]->codec->width, ofmt_ctx->streams[videoindex]->codec->height);
	}
	//Dump Format------------------
	av_dump_format(ofmt_ctx, 0, out_filename, 1);
	printf("tiger avio_open?\n");

	ofmt = ofmt_ctx->oformat;
	//Open output URL
	if (!(ofmt->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
		if (ret < 0) {
			printf("avio_open Could not open output URL '%s'", out_filename);
			goto end;
		}
	}
	printf("tiger avformat_write_header\n");
	//Write file header
	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		printf("avformat_write_header Error occurred when opening output URL\n");
		goto end;
	}
	printf("tiger fcntl O_NONBLOCK\n");
	if (1) {
		//nonblock will stuck in the  avformat_find_stream_info
		int flags;
		int err;
		flags = fcntl(g_fd, F_GETFL, 0);//TODO
		err = fcntl(g_fd, F_SETFL, flags | O_NONBLOCK);
		fmt_ctx->flags |= AVFMT_FLAG_NONBLOCK;
	}

	if (1) {
		//nonblock will stuck in the  avformat_find_stream_info
		int flags;
		int err;
		flags = fcntl(g_fd2, F_GETFL, 0);//TODO
		err = fcntl(g_fd2, F_SETFL, flags | O_NONBLOCK);
		fmt_ctx2->flags |= AVFMT_FLAG_NONBLOCK;
	}

	printf("tiger av_read_frame\n");
	// start_time=av_gettime();
	while (1) {

		//AVStream *in_stream, *out_stream;
		//Get an AVPacket
		if (frame_index > 300) {
			break;
		}
		if (1) {
			AVPacket pkt;
			int stream_index;
			//goto audio; //仅测试
			ret = av_read_frame(fmt_ctx, &pkt);
			if (frame_index > 100) {
				break;
			}
			//stream_index = pkt.stream_index;//流的index
			//av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u, stream index:%d, codec type:%d\n",
			//	stream_index, stream_index, fmt_ctx->streams[pkt.stream_index]->codecpar->codec_type);

			if (ret == AVERROR(EAGAIN)) {
				//printf( "EAGAIN\n");
				//usleep(10000);
				usleep(300);
				 goto audio; //continue;////continue
			}
			if (ret < 0)
				break;
			frame_index++;
			video_frame_loop++;
			printf("frame_index %d, video_frame_loop:%d\n", frame_index, video_frame_loop);

			pkt.stream_index = videoindex;

			pkt.dts = av_rescale_q_rnd(pkt.dts,
				fmt_ctx->streams[videoindex]->time_base,
				ofmt_ctx->streams[videoindex]->time_base,
				(enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.pts = av_rescale_q_rnd(pkt.pts,
				fmt_ctx->streams[videoindex]->time_base,
				ofmt_ctx->streams[videoindex]->time_base,
				(enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.duration = av_rescale_q_rnd(pkt.duration,
				fmt_ctx->streams[videoindex]->time_base,
				ofmt_ctx->streams[videoindex]->time_base,
				(enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			printf("video dts:%ld,pkt:%ld,duration:%ld,%d %d,size:%d, ret:%d\n", pkt.dts, pkt.pts, pkt.duration,
				fmt_ctx->streams[videoindex]->time_base.den, fmt_ctx->streams[videoindex]->time_base.num,
				ofmt_ctx->streams[videoindex]->time_base.den, ofmt_ctx->streams[videoindex]->time_base.num, pkt.size, ret);
			//ret = av_write_frame(ofmt_ctx, &pkt);

			ret = av_interleaved_write_frame(ofmt_ctx, &pkt);

			if (ret < 0) {
				printf("Error muxing packet, I'm quitting...ret:%d\n",ret);
				//tiger .... here maybe error av_free_packet....
				break;
			}

			av_free_packet(&pkt);
		}
	audio:
		if (1) {
			AVPacket pkt;
			//   printf( "av_read_frame auido 1 \n");        
			ret = av_read_frame(fmt_ctx2, &pkt);
			if (ret == AVERROR(EAGAIN)) {
				//printf( "EAGAIN\n");
				//usleep(100000);
				continue;
			}

			if (ret < 0) {
				printf("av_read_frame auido 2 ret:%d \n", ret);
				break;
			}

			frame_index++;
			audio_frame_loop++;
			printf("frame_index:%d , audio_frame_loop:%d\n", frame_index, audio_frame_loop);
			//printf( "av_read_frame auido 3 frame_index:%d \n", frame_index);        
			pkt.stream_index = audioindex + 1;

			pkt.dts = av_rescale_q_rnd(pkt.dts,
				fmt_ctx2->streams[audioindex]->time_base,
				ofmt_ctx->streams[audioindex + 1]->time_base,
				(enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.pts = av_rescale_q_rnd(pkt.pts,
				fmt_ctx2->streams[audioindex]->time_base,
				ofmt_ctx->streams[audioindex + 1]->time_base,
				(enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.duration = av_rescale_q_rnd(pkt.duration,
				fmt_ctx2->streams[audioindex]->time_base,
				ofmt_ctx->streams[audioindex + 1]->time_base,
				(enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			printf("audio dts:%ld,pkt:%ld,duration:%ld,%d %d, size:%d,ret:%d\n", pkt.dts, pkt.pts, pkt.duration,
				fmt_ctx->streams[videoindex]->time_base.den, fmt_ctx->streams[videoindex]->time_base.num,
				ofmt_ctx->streams[videoindex]->time_base.den, ofmt_ctx->streams[videoindex]->time_base.num, pkt.size, ret);
			//ret = av_write_frame(ofmt_ctx, &pkt);
		   // printf( "av_read_frame auido 4 frame_index:%d \n", frame_index); 
			ret = av_interleaved_write_frame(ofmt_ctx, &pkt);

			if (ret < 0) {
				printf("Error muxing packet2, I'm quitting...ret:%d\n", ret);
				//tiger .... here maybe error av_free_packet....
				break;
			}

			av_free_packet(&pkt);
		}


		//printf( "av_read_frame ret:%d, pkt.stream_index:%d, videoindex:%d, pos:%ld, size:%d, duration:%ld,dts:%ld,pts:%ld, frame_index:%d\n", 
		//    ret, pkt.stream_index, videoindex, pkt.pos, pkt.size, pkt.duration, pkt.dts, pkt.pts, frame_index);
		//printf( "av_read_frame time_base.num:%d, time_base.den:%d\n", 
		//    fmt_ctx->streams[videoindex]->time_base.num, fmt_ctx->streams[videoindex]->time_base.den);

	//avio_pos = avio_tell(avio_ctx);
//printf("av_read_frame avio_pos:%lld, avio_ctx->pos:%lld,bytes_read:%lld, eof_reached:%d , max_packet_size:%d, error:%d\n", 
//    avio_pos, avio_ctx->pos, avio_ctx->bytes_read,  avio_ctx->eof_reached, avio_ctx->max_packet_size, avio_ctx->error);
	//av_hex_dump_log(NULL, AV_LOG_INFO, pkt.data, pkt.size);  //data + 是包含 0001的
/*
	  avio_skip(avio_ctx, avio_pos);
	  //avio_ctx->eof_reached = 0;
	avio_pos = avio_tell(avio_ctx);
printf("av_read_frame avio_pos:%lld, avio_ctx->pos:%lld,bytes_read:%lld, eof_reached:%d , max_packet_size:%d, error:%d\n",
	avio_pos, avio_ctx->pos, avio_ctx->bytes_read,  avio_ctx->eof_reached, avio_ctx->max_packet_size, avio_ctx->error);
*/



// if(loop ++ == 0){  忽略第一个包
//         av_free_packet(&pkt);
//         continue;
// }


 //FIX£ºNo PTS (Example: Raw H.264)
 //Simple Write PTS
/*
		if(pkt.pts==AV_NOPTS_VALUE){
		{
			int64_t duration = 0;
			int64_t pts = 0;
			int64_t dts = 0;

			//Write PTS
			AVRational time_base1=fmt_ctx->streams[videoindex]->time_base;
			//Duration between 2 frames (us)
			int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(fmt_ctx->streams[videoindex]->r_frame_rate);
			//Parameters
			pts=(double)(frame_index++*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
			dts=pkt.pts;
			duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);
			printf("duration:%ld, pts:%ld, dts:%d, frame_index:%d\n", duration, pts, dts, frame_index);
			pkt.pts = pts;
			pkt.dts = pts;
			pkt.duration = duration;
		}

		//Important:Delay
		if(pkt.stream_index==videoindex){
			AVRational time_base=ifmt_ctx->streams[videoindex]->time_base;
			AVRational time_base_q={1,AV_TIME_BASE};
			int64_t pts_time = av_rescale_q(pkt.dts, time_base, time_base_q);
			int64_t now_time = av_gettime() - start_time;
			if (pts_time > now_time)
				av_usleep(pts_time - now_time);

		}
*/
/*
		in_stream  = ifmt_ctx->streams[pkt.stream_index];
		out_stream = ofmt_ctx->streams[pkt.stream_index];
		// copy packet //
		//Convert PTS/DTS
		pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
		pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
		pkt.pos = -1;
		//Print to Screen
		if(pkt.stream_index==videoindex){
			printf("Send %8d video frames to output URL\n",frame_index);
			frame_index++;
		}
*/


	}
	//Write file trailer
	av_write_trailer(ofmt_ctx);

	/*

	{
		int i = 0;
			AVPacket pkt;
		fmt_ctx->pb = avio_ctx;

		for(i =0; i < 50; i ++){//tiger read packets unions, not slices.
				ret = av_read_frame(fmt_ctx, &pkt);
			fprintf(stderr, "read with break ret:%d, stream index:%d, pos:%ld, size:%ld, duration:%ld,dts:%ld,pts:%ld\n", ret, pkt.stream_index, pkt.pos, pkt.size, pkt.duration, pkt.dts, pkt.pts);
			if (ret < 0){
				fprintf(stderr, "read with break\n");
				break;
			}

		}

	}

	*/
	printf(" av_dump_format...... end...\n");
	
end:
	avformat_close_input(&fmt_ctx);
	/* note: the internal buffer could have changed, and be != avio_ctx_buffer */
	//if (avio_ctx) {
	//    av_freep(&avio_ctx->buffer);
	//    avio_context_free(&avio_ctx);
   // }
   // av_file_unmap(buffer, buffer_size);

	/* close output */
	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
		avio_close(ofmt_ctx->pb);

	avformat_free_context(ofmt_ctx);

	if (ret < 0) {
		fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
		return 1;
	}

	return 0;
}


