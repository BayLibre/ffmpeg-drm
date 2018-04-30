/*
 * FFMPEG DRM/KMS example application
 * Jorge Ramirez-Ortiz <jramirez@baylibre.com>
 *
 * Main file of the application
 *      Based on code from:
 *      	2001 Fabrice Bellard (FFMPEG/doc/examples/decode_video.c
 *      	2018 Stanimir Varbanov (v4l2-decode/src/drm.c)
 *
 * This code has been tested on Linaro's Dragonboard 820c
 *      kernel v4.14.15, venus decoder
 *      ffmpeg 4.0 + lrusacks ffmpeg/DRM support + review
 *      	https://github.com/ldts/ffmpeg  branch lrusak/v4l2-drmprime
 *
 * Copyright (c) 2018 Baylibre
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <sys/time.h>
#include <getopt.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>

#define ALIGN(x, a)		((x) + (a - 1)) & (~(a - 1))
#define DRM_ALIGN(val, align)	((val + (align - 1)) & ~(align - 1))

#define INBUF_SIZE 4096

struct drm_buffer {
	unsigned int fourcc;
	unsigned int bo_handle;
	unsigned int fb_handle;
	int dbuf_fd;
	void *mmap_buf;
};

struct drm_dev {
	int fd;
	uint32_t conn_id, enc_id, crtc_id, fb_id, plane_id;
	uint32_t width, height;
	uint32_t pitch, size, handle;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	struct drm_dev *next;
};

static struct drm_dev *pdev;
static unsigned int drm_format;

#define DBG_TAG "  ffmpeg-drm"

#define print(msg, ...)							\
	do {								\
			struct timeval tv;				\
			gettimeofday(&tv, NULL);			\
			fprintf(stderr, "%08u:%08u :" msg,		\
				(uint32_t)tv.tv_sec,			\
				(uint32_t)tv.tv_usec, ##__VA_ARGS__);	\
	} while (0)

#define err(msg, ...)  print("error: " msg "\n", ##__VA_ARGS__)
#define info(msg, ...) print(msg "\n", ##__VA_ARGS__)
#define dbg(msg, ...)  print(DBG_TAG ": " msg "\n", ##__VA_ARGS__)

int drm_dmabuf_set_plane(struct drm_buffer *buf, uint32_t width,
			 uint32_t height, int fullscreen)
{
	uint32_t crtc_w, crtc_h;

	crtc_w = width;
	crtc_h = height;

	if (fullscreen) {
		crtc_w = pdev->width;
		crtc_h = pdev->height;
	}

	return drmModeSetPlane(pdev->fd, pdev->plane_id, pdev->crtc_id,
		      buf->fb_handle, 0,
		      0, 0, crtc_w, crtc_h,
		      0, 0, width << 16, height << 16);
}

static int drm_dmabuf_import(struct drm_buffer *buf, unsigned int width,
		      unsigned int height)
{
	return drmPrimeFDToHandle(pdev->fd, buf->dbuf_fd, &buf->bo_handle);
}

static int drm_dmabuf_addfb(struct drm_buffer *buf, uint32_t width, uint32_t height)
{
	int ret;

	if (width > pdev->width)
		width = pdev->width;
	if (height > pdev->height)
		height = pdev->height;

	width = ALIGN(width, 8);

	uint32_t stride = DRM_ALIGN(width, 128);
	uint32_t y_scanlines = DRM_ALIGN(height, 32);

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t handles[4] = { 0 };

	offsets[0] = 0;
	handles[0] = buf->bo_handle;
	pitches[0] = stride;

	offsets[1] = stride * y_scanlines;
	handles[1] = buf->bo_handle;
	pitches[1] = pitches[0];

	ret = drmModeAddFB2(pdev->fd, width, height, buf->fourcc, handles,
			    pitches, offsets, &buf->fb_handle, 0);
	if (ret) {
		err("drmModeAddFB2 failed: %d (%s)\n", ret, strerror(errno));
		return ret;
	}

	return 0;
}

static int find_plane(int fd, unsigned int fourcc, uint32_t *plane_id,
			uint32_t crtc_id)
{
	drmModePlaneResPtr planes;
	drmModePlanePtr plane;
	unsigned int i;
	unsigned int j;
	int ret = 0;
	unsigned int format = fourcc;

	planes = drmModeGetPlaneResources(fd);
	if (!planes) {
		err("drmModeGetPlaneResources failed\n");
		return -1;
	}

	info("drm: found planes %u", planes->count_planes);

	for (i = 0; i < planes->count_planes; ++i) {
		plane = drmModeGetPlane(fd, planes->planes[i]);
		if (!plane) {
			err("drmModeGetPlane failed: %s\n", strerror(errno));
			break;
		}

	for (j = 0; j < plane->count_formats; ++j) {
			if (plane->formats[j] == format)
				break;
		}

		if (j == plane->count_formats) {
			drmModeFreePlane(plane);
			continue;
		}

		*plane_id = plane->plane_id;
		drmModeFreePlane(plane);
		break;
	}

	if (i == planes->count_planes)
		ret = -1;

	drmModeFreePlaneResources(planes);

	return ret;
}

static struct drm_dev *drm_find_dev(int fd)
{
	int i;
	struct drm_dev *dev = NULL, *dev_head = NULL;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;

	if ((res = drmModeGetResources(fd)) == NULL) {
		err("drmModeGetResources() failed");
		return NULL;
	}

	if (res->count_crtcs <= 0) {
		err("no Crtcs");
		goto free_res;
	}

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn) {
			if (conn->connection == DRM_MODE_CONNECTED) {
				dbg("drm: connector: connected");
			} else if (conn->connection == DRM_MODE_DISCONNECTED) {
				dbg("drm: connector: disconnected");
			} else if (conn->connection == DRM_MODE_UNKNOWNCONNECTION) {
				dbg("drm: connector: unknownconnection");
			} else {
				dbg("drm: connector: unknown");
			}
		}

		if (conn != NULL && conn->connection == DRM_MODE_CONNECTED
		    && conn->count_modes > 0) {
			dev = (struct drm_dev *) malloc(sizeof(struct drm_dev));
			memset(dev, 0, sizeof(struct drm_dev));

			dev->conn_id = conn->connector_id;
			dev->enc_id = conn->encoder_id;
			dev->next = NULL;

			memcpy(&dev->mode, &conn->modes[0], sizeof(drmModeModeInfo));
			dev->width = conn->modes[0].hdisplay;
			dev->height = conn->modes[0].vdisplay;

			/* FIXME: use default encoder/crtc pair */
			if ((enc = drmModeGetEncoder(fd, dev->enc_id)) == NULL) {
				err("drmModeGetEncoder() faild");
				goto free_res;
			}

			dev->crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);

			dev->saved_crtc = NULL;

			/* create dev list */
			dev->next = dev_head;
			dev_head = dev;
		}
		drmModeFreeConnector(conn);
	}

free_res:
	drmModeFreeResources(res);

	return dev_head;
}

static int drm_open(const char *path)
{
	int fd, flags;
	uint64_t has_dumb;
	int ret;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		err("cannot open \"%s\"\n", path);
		return -1;
	}

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0 ||
	     fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		err("fcntl FD_CLOEXEC failed\n");
		goto err;
	}

	/* check capability */
	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
	if (ret < 0 || has_dumb == 0) {
		err("drmGetCap DRM_CAP_DUMB_BUFFER failed or doesn't have dumb "
		    "buffer\n");
		goto err;
	}

	return fd;
err:
	close(fd);
	return -1;
}

static int drm_init(unsigned int fourcc)
{
	struct drm_dev *dev_head, *dev;
	int fd;
	int ret;

	fd = drm_open("/dev/dri/card0");
	if (fd < 0)
		return -1;

	dev_head = drm_find_dev(fd);
	if (dev_head == NULL) {
		err("available drm devices not found\n");
		goto err;
	}

	dbg("available connector(s)");

	for (dev = dev_head; dev != NULL; dev = dev->next) {
		dbg("connector id:%d", dev->conn_id);
		dbg("\tencoder id:%d crtc id:%d fb id:%d", dev->enc_id,
		    dev->crtc_id, dev->fb_id);
		dbg("\twidth:%d height:%d", dev->width, dev->height);
	}

	/* FIXME: use first drm_dev */
	dev = dev_head;
	dev->fd = fd;
	pdev = dev;

	ret = find_plane(fd, fourcc, &dev->plane_id, dev->crtc_id);
	if (ret) {
		err("Cannot find plane\n");
		goto err;
	}

	info("drm: Found NV12 plane_id: %x", dev->plane_id);

	return 0;

err:
	close(fd);
	pdev = NULL;
	return -1;
}

static int display(struct drm_buffer *drm_buf, int width, int height)
{
	int ret;

	ret = drm_dmabuf_import(drm_buf, width, height);
	if (ret) {
		err("cannot import dmabuf %d, fd=%d\n", ret, drm_buf->dbuf_fd);
		return -EFAULT;
	}

	ret = drm_dmabuf_addfb(drm_buf, width, height);
	if (ret) {
		err("cannot add framebuffer %d\n", ret);
		return -EFAULT;
	}

	drm_dmabuf_set_plane(drm_buf, width, height, 1);

	return 0;
}

static void decode_and_display(AVCodecContext *dec_ctx, AVFrame *frame,
			AVPacket *pkt)
{
	AVDRMFrameDescriptor *desc = NULL;
	struct drm_buffer drm_buf;
	int ret;

	ret = avcodec_send_packet(dec_ctx, pkt);
	if (ret < 0) {
		err("Error sending a packet for decoding\n");
		exit(1);
	}

	while (ret >= 0) {
		ret = avcodec_receive_frame(dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return;
		else if (ret < 0) {
			err("Error during decoding\n");
			exit(1);
		}

		desc = (AVDRMFrameDescriptor *) frame->data[0];
		drm_buf.dbuf_fd = desc->objects[0].fd;

                if (!pdev) {
                    /* initialize DRM with the format returned in the frame */
                    ret = drm_init(desc->layers[0].format);
                    if (ret) {
                        err("Error initializing drm\n");
                        exit(1);
                    }

                    /* remember the format */
                    drm_format = desc->layers[0].format;
                }

                /* pass the format in the buffer */
                drm_buf.fourcc = drm_format;
		ret = display(&drm_buf, frame->width, frame->height);
		if (ret < 0)
			return;
    }
}

static const struct option options[] = {
	{
#define help_opt	0
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
	},
	{
#define image_opt	1
		.name = "image",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define codec_opt	2
		.name = "codec",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define height_opt	3
		.name = "height",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define width_opt	4
		.name = "width",
		.has_arg = 1,
		.flag = NULL,
	},
	{
		.name = NULL,
	},
};

static void usage(void)
{
	fprintf(stderr, "usage: ffmpeg-drm <options>, with:\n");
	fprintf(stderr, "--help             display this menu\n");
	fprintf(stderr, "--image=<name>    image to display\n");
	fprintf(stderr, "--codec=<name>    ffmpeg codec: ie h264_v4l2m2m\n");
	fprintf(stderr, "--width=<value>   image width\n");
	fprintf(stderr, "--height=<value>  image height\n");
	fprintf(stderr, "\n");
}

int main(int argc, char *argv[])
{
	uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
	AVCodecParserContext *parser;
	AVCodecContext *c = NULL;
	const AVCodec *codec;
	AVFrame *frame;
	AVPacket *pkt;
	size_t data_size;
	uint8_t *data;
	FILE *f;
	int ret;
	int lindex, opt;
	unsigned int image_width = 0, image_height = 0;
	char *codec_name = NULL, *image_name = NULL;

	for (;;) {
		lindex = -1;

		opt = getopt_long_only(argc, argv, "", options, &lindex);
		if (opt == EOF)
			break;

		switch (lindex) {
		case help_opt:
			usage();
			exit(0);
		case image_opt:
			image_name = optarg;
			break;
		case codec_opt:
			codec_name = optarg;
			break;
		case width_opt:
			image_width = atoi(optarg);
			break;
		case height_opt:
			image_height = atoi(optarg);
			break;
		default:
			usage();
			exit(1);
		}
	}

	if (!image_width || !image_height || !codec_name || !image_name) {
		usage();
		exit(0);
	}

	pkt = av_packet_alloc();
	if (!pkt) {
		err("Error allocating packet\n");
		exit(1);
	}

	/* set end of buffer to 0 (this ensures that no overreading happens for
	   damaged MPEG streams) */
	memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

	/* find the video decoder: ie: h264_v4l2m2m */
	codec = avcodec_find_decoder_by_name(codec_name);
	if (!codec) {
		err("Codec not found\n");
		exit(1);
	}

	parser = av_parser_init(codec->id);
	if (!parser) {
		err("parser not found\n");
		exit(1);
	}

	c = avcodec_alloc_context3(codec);
	if (!c) {
		err("Could not allocate video codec context\n");
		exit(1);
	}

	/* For some codecs, such as msmpeg4 and mpeg4, width and height
	   MUST be initialized before opening the ffmpeg codec (ie, before
	   calling avcodec_open2) because this information is not available in
	   the bitstream). */
	c->pix_fmt = AV_PIX_FMT_DRM_PRIME;   /* request a DRM frame */
        c->coded_height = image_height;
	c->coded_width = image_width;

	/* open it */
	if (avcodec_open2(c, codec, NULL) < 0) {
		err("Could not open codec\n");
		exit(1);
	}

	f = fopen(image_name, "rb");
	if (!f) {
		err("Could not open %s\n", image_name);
		exit(1);
	}

	frame = av_frame_alloc();
	if (!frame) {
		err("Could not allocate video frame\n");
		exit(1);
	}

	while (!feof(f)) {
		/* read raw data from the input file */
		data_size = fread(inbuf, 1, INBUF_SIZE, f);
		if (!data_size)
			break;

		/* use the parser to split the data into frames */
		data = inbuf;
		while (data_size > 0) {
			ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
				data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			if (ret < 0) {
				err("Error while parsing\n");
				exit(1);
			}

			data += ret;
			data_size -= ret;

			if (pkt->size)
				decode_and_display(c, frame, pkt);
		}
	}

	/* flush the decoder */
	decode_and_display(c, frame, NULL);

	fclose(f);

	av_parser_close(parser);
	avcodec_free_context(&c);
	av_frame_free(&frame);
	av_packet_free(&pkt);

	return 0;
}
