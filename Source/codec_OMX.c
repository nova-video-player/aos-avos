/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "global.h"
#include "types.h"
#include "debug.h"
#include "util.h"
#include "stream.h"
#include "astdlib.h"
#include "athread.h"
#include "rc_clocks.h"
#include "mpg4.h"
#include "mpeg2.h"
#include "h264.h"
#include "wmv.h"
#include "OMX_utils.h"
#include "android_config.h"
#include "device_config.h"
#include "xdm_utils.h"
#include "pts_reorder.h"

#include <sys/queue.h>

#ifdef CONFIG_STREAM

static int omx_flush = 0;

#ifdef DEBUG_MSG
DECLARE_DEBUG_TOGGLE( "omxf", omx_flush );
#endif

#define DBGS 	if(0||Debug[DBG_STREAM])
#define DBGCV 	if(0||Debug[DBG_CV])
#define DBGCV2 	if(0||Debug[DBG_CV] > 1 )

#define MAXW VIDEO_MAX_WIDTH
#define MAXH VIDEO_MAX_HEIGHT

#define DUCATI_PADX_H264 32
#define DUCATI_PADY_H264 24

#define DUCATI_PADX_MPEG4 32
#define DUCATI_PADY_MPEG4 32

// #define OMX_FORCE_COMP "OMX.google.mpeg4.decoder"
// #define OMX_FORCE_ROLE "video_decoder.mpeg4"
// #define OMX_FORCE_COMP "OMX.google.avc.decoder"
// #define OMX_FORCE_ROLE "video_decoder.avc"

#define CLOG(fmt, ...) serprintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__)
#define CERR(omx_err, fmt, ...) \
	do { \
		err = omx_err; \
		serprintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); \
		goto error; \
	} while (0)


#define CHECK_ERR(fmt) \
	do { \
		if (err != OMX_ErrorNone) { \
			serprintf("%s: " fmt " failed: %s\n", __FUNCTION__, omx_get_error_name(err)); \
			goto error; \
		} \
	} while(0)

#define CHECK_ERR_LOCKED(fmt) \
	do { \
		if (err != OMX_ErrorNone) { \
			serprintf("%s: " fmt " failed: %s\n", __FUNCTION__, omx_get_error_name(err)); \
			pthread_mutex_unlock(&port->mtx); \
			goto error; \
		} \
	} while(0)

#define ERROR_LOG(fmt, ...) serprintf(" "fmt "\n", ##__VA_ARGS__)
#define EVENT_LOG(fmt, ...) DBGCV serprintf(" "fmt "\n", ##__VA_ARGS__)
#define CMD_LOG(fmt, ...) DBGCV serprintf("\tcmd "fmt "\n", ##__VA_ARGS__)
#define BUF_LOG(fmt, ...) DBGCV2 serprintf("\tbuf "fmt "\n", ##__VA_ARGS__)

enum {
	EVENT_RECONFIGURE	= 0x0001,
	EVENT_CROP		= 0x0002,
	EVENT_ILACE		= 0x0004,
};

typedef enum omx_buffer_state {
	BUF_STATE_INIT,
	BUF_STATE_READY,
	BUF_STATE_OMX,
	BUF_STATE_OMX_DONE,
	BUF_STATE_OUT,
} omx_buffer_state_t;

typedef struct omx_buffer {
	int index;
	omx_buffer_state_t state;

	OMX_BUFFERHEADERTYPE *header;
	OMX_DIRTYPE dir;

	OMX_PTR android_handle;
	VIDEO_FRAME *frame;
	int user_ID;

	TAILQ_ENTRY(omx_buffer) next;
} omx_buffer_t;

typedef TAILQ_HEAD(, omx_buffer) BUFFER_TAILQ;

enum omx_port_flag {
	OMX_PORT_FLAG_USE_ANDROID_NATIVE_BUFFERS = 0x01,
	OMX_PORT_FLAG_SET_SIZE = 0x02,
};

typedef struct omx_port {
	int valid;
	OMX_U32 index;
	OMX_U32 flags;

	OMX_HANDLETYPE omx_handle;
	OMX_PARAM_PORTDEFINITIONTYPE omx_def;
	OMX_COLOR_FORMATTYPE omx_format;

	omx_buffer_t **buffers;
	int nb_buffers_max;
	int nb_omx_buffers;
	int nb_omx_buffers_max;
	int flush;
	
	pthread_mutex_t mtx;
	pthread_cond_t cond;

	BUFFER_TAILQ fifo;
	int event;
} omx_port_t;

typedef struct priv {
	OMX_HANDLETYPE omx_handle;
	int format;

	char *comp;
	const char *role;
	omx_port_t port_in;
	omx_port_t port_out;
	omx_port_t *ports[3];
	int port_nb;
	int allocating;

	int buf_type;
	int hw_usage;

	int realloc;

	int do_header;
	int width;
	int height;
	int colorspace;
	int crop_x;
	int crop_y;
	int crop_w;
	int crop_h;
	int interlaced;
	int rk30_hack_size_nok;
	int out_buffer_size;
	int use_shadow;

	struct {
		pthread_mutex_t mtx;
		pthread_cond_t cond;
		OMX_COMMANDTYPE cmd;
		OMX_ERRORTYPE error;
	} wait;

	int reorder_pts;
	struct XDM_ctx XDM_ctx;
	
	VIDEO_FRAME shadow[STREAM_MAX_FRAMES];
	
} priv_t;

#ifdef DEBUG_MSG
#define OMX_TI_IndexParamVideoDeblockingQP  0x7F0000A8

typedef struct OMX_TI_VIDEO_PARAM_DEBLOCKINGQP {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_U32         nDeblockingQP;
} OMX_TI_VIDEO_PARAM_DEBLOCKINGQP;

int deblocking_value = -1;
static void _set_deblocking_value( int argc, char *argv[] )
{
	if( argc > 1 ) {
		deblocking_value = atoi( argv[1] );
		serprintf("%s: %d\r\n", __FUNCTION__, deblocking_value );
	}
}

static void force_deblocking_value(priv_t *p)
{
	OMX_ERRORTYPE err = OMX_ErrorNone;
	OMX_TI_VIDEO_PARAM_DEBLOCKINGQP omx_deblock;

	if (deblocking_value < 0) return;

	memset(&omx_deblock, 0, sizeof(omx_deblock));
	OMX_INIT_STRUCTURE(omx_deblock);
	serprintf("FORCE DEBLOCKING to level %d\n", deblocking_value);
	omx_deblock.nPortIndex = p->port_out.index;
	err = OMX_GetParameter(p->port_out.omx_handle, OMX_TI_IndexParamVideoDeblockingQP, &omx_deblock);
	CHECK_ERR("OMX_GetParameter(OMX_TI_IndexParamVideoDeblockingQP)");
	omx_deblock.nDeblockingQP = deblocking_value;
	err = OMX_SetParameter(p->port_out.omx_handle, OMX_TI_IndexParamVideoDeblockingQP, &omx_deblock);
	CHECK_ERR("OMX_SetParameter(OMX_TI_IndexParamVideoDeblockingQP)");
error:
	return;
}

DECLARE_DEBUG_COMMAND( "dblk",  _set_deblocking_value );
#endif

static int omx_port_set_crop(priv_t *p, omx_port_t *port);
static int omx_event_push_cmd(priv_t *p, OMX_COMMANDTYPE cmd);
static int omx_event_push_error(priv_t *p, OMX_ERRORTYPE err);

/*
 * Hack: Galaxy S II / III (OMX.SEC.*):
 * decoder doesn't send OMX_IndexConfigCommonOutputCrop events and
 * the crop size seems to be the buffer size.
 */
static inline void hack_omx_sec_dec_crop(priv_t *p)
{
	if (strncmp(p->comp, "OMX.SEC.", strlen("OMX.SEC.")) == 0) {
		omx_port_set_crop(p, &p->port_out);
		p->width = p->crop_w;
		p->height = p->crop_h;
	}
}

static void omx_port_add_event(omx_port_t *port, int state)
{
	pthread_mutex_lock(&port->mtx);
	port->event |= state;
	pthread_mutex_unlock(&port->mtx);
}

static int omx_port_get_event(omx_port_t *port)
{
	int event;

	pthread_mutex_lock(&port->mtx);
	event = port->event;
	port->event = 0;
	pthread_mutex_unlock(&port->mtx);
	return event;
}

static omx_buffer_t *omx_port_get_buffer_from_frame(omx_port_t *port, VIDEO_FRAME *f)
{
	int i;

	for (i = 0; i < port->nb_buffers_max; ++i) {
		if (port->buffers[i]->frame == f)
			return port->buffers[i];
	}
	return NULL;
}

static OMX_ERRORTYPE omx_buffer_send_locked(omx_port_t *port, omx_buffer_t *buffer)
{
	OMX_ERRORTYPE err = OMX_ErrorNone;

	if (port->nb_omx_buffers < port->nb_omx_buffers_max) {
		if (buffer->dir == OMX_DirInput) {
			BUF_LOG("send   in (%2d) %2d/%2d  size %6d  time %8lld", buffer->index, port->nb_omx_buffers + 1, port->nb_omx_buffers_max, buffer->header->nFilledLen, buffer->header->nTimeStamp);
			err = OMX_EmptyThisBuffer(port->omx_handle, buffer->header);
			CHECK_ERR("OMX_EmptyThisBuffer");
		} else {
			BUF_LOG("send   out(%2d) %2d/%2d", buffer->index, port->nb_omx_buffers + 1, port->nb_omx_buffers_max);
			err = OMX_FillThisBuffer(port->omx_handle, buffer->header);
			CHECK_ERR("OMX_FillThisBuffer");
		}
		port->nb_omx_buffers++;
		buffer->state = BUF_STATE_OMX;
	}
	return err;
error:
	return err;
}

static inline omx_buffer_t *omx_buffer_get_done_locked(omx_port_t *port, int wait)
{
	omx_buffer_t *buffer = NULL;

	while ((buffer = TAILQ_FIRST(&port->fifo)) == NULL && wait)
		pthread_cond_wait(&port->cond, &port->mtx);
	if (buffer) {
		TAILQ_REMOVE(&port->fifo, buffer, next);
		buffer->state = BUF_STATE_OUT;
	}
	return buffer;
}

static omx_buffer_t *omx_buffer_get_done(omx_port_t *port, int wait)
{
	omx_buffer_t *buffer = NULL;

	pthread_mutex_lock(&port->mtx);
	buffer = omx_buffer_get_done_locked(port, wait);
	pthread_mutex_unlock(&port->mtx);
	return buffer;
}

static omx_buffer_t *omx_buffer_get_free_locked(omx_port_t *port, int wait)
{
	int i;
	omx_buffer_t *buffer = NULL;

	do {
		for (i = 0; i < port->nb_buffers_max && buffer == NULL; ++i) {
			if (port->buffers[i]->state == BUF_STATE_READY ||
			    port->buffers[i]->state == BUF_STATE_INIT)
				buffer = port->buffers[i];
		}
		if (!buffer && wait)
			pthread_cond_wait(&port->cond, &port->mtx);
	} while (!buffer && wait);
	return buffer;
}

static omx_buffer_t *omx_buffer_get_free(omx_port_t *port, int wait)
{
	omx_buffer_t *buffer = NULL;

	pthread_mutex_lock(&port->mtx);
	buffer = omx_buffer_get_free_locked(port, wait);
	pthread_mutex_unlock(&port->mtx);
	return buffer;
}

static void omx_buffer_wait_all_done(omx_port_t *port)
{
	omx_buffer_t *buffer, *buffer_next;

	pthread_mutex_lock(&port->mtx);
	while (port->nb_omx_buffers != 0)
		pthread_cond_wait(&port->cond, &port->mtx);
	for (buffer = TAILQ_FIRST(&port->fifo); buffer != NULL; buffer = buffer_next) {
		buffer_next = TAILQ_NEXT(buffer, next);
		TAILQ_REMOVE(&port->fifo, buffer, next);
	}
	pthread_mutex_unlock(&port->mtx);
}

static OMX_ERRORTYPE omx_event_handler(OMX_HANDLETYPE omx_handle,
    OMX_PTR app_data, OMX_EVENTTYPE event, OMX_U32 data_1,
    OMX_U32 data_2, OMX_PTR event_data)
{
	priv_t *p = (priv_t*)app_data;

	switch (event) {
		case OMX_EventCmdComplete:
			if (data_1 == OMX_CommandStateSet) {
				CMD_LOG("< %s: %s",
				    omx_get_command_name(data_1),
				    omx_get_state_name(data_2));
			} else {
				CMD_LOG("< %s: 0x%X",
				    omx_get_command_name(data_1),
				    data_2);
			}
			omx_event_push_cmd(p, data_1);
			break;
		case OMX_EventError:
			ERROR_LOG("%s: %s: 0x%X",
			    omx_get_event_name(event),
			    omx_get_error_name(data_1),
			    data_2);
			omx_event_push_error(p, data_1);
			break;
		case OMX_EventMark:
			EVENT_LOG("%s: 0x%X: 0x%X", omx_get_event_name(event), data_1, data_2);
			break;
		case OMX_EventPortSettingsChanged:
			if (data_2 == 0 || data_2 == OMX_IndexParamPortDefinition) {
				omx_port_t *port = data_1 == OMX_DirInput ? &p->port_in : &p->port_out;

				omx_port_add_event(port, EVENT_RECONFIGURE);
				EVENT_LOG("%s: 0x%X: IndexParamPortDefinition", omx_get_event_name(event), data_1);
			} else if (data_2 == OMX_IndexConfigCommonOutputCrop) {
				omx_port_t *port = data_1 == OMX_DirInput ? &p->port_in : &p->port_out;

				omx_port_add_event(port, EVENT_CROP);
				EVENT_LOG("%s: 0x%X: IndexConfigCommonOutputCrop", omx_get_event_name(event), data_1);
			} else if (data_2 == OMX_TI_IndexConfigStreamInterlaceFormats) {
				omx_port_t *port = data_1 == OMX_DirInput ? &p->port_in : &p->port_out;

				omx_port_add_event(port, EVENT_ILACE);
				EVENT_LOG("%s: 0x%X: OMX_TI_IndexConfigStreamInterlaceFormats", omx_get_event_name(event), data_1);
			} else {
				EVENT_LOG("%s: 0x%X: 0x%X NOT HANDLED!", omx_get_event_name(event), data_1, data_2);
			}
			break;
		case OMX_EventBufferFlag:
			EVENT_LOG("%s: 0x%X: 0x%X NOT HANDLED!", omx_get_event_name(event), data_1, data_2);
			break;
		case OMX_EventResourcesAcquired:
			EVENT_LOG("%s: 0x%X: 0x%X NOT HANDLED!", omx_get_event_name(event), data_1, data_2);
			break;
		case OMX_EventComponentResumed:
			EVENT_LOG("%s: 0x%X: 0x%X NOT HANDLED!", omx_get_event_name(event), data_1, data_2);
			break;
		case OMX_EventDynamicResourcesAvailable:
			EVENT_LOG("%s: 0x%X: 0x%X NOT HANDLED!", omx_get_event_name(event), data_1, data_2);
			break;
		case OMX_EventPortFormatDetected:
			EVENT_LOG("%s: 0x%X: 0x%X NOT HANDLED!", omx_get_event_name(event), data_1, data_2);
			break;
		default:
			EVENT_LOG("%s: 0x%X: 0x%X NOT HANDLED!", omx_get_event_name(event), data_1, data_2);
			break;
	}
	return 0;
}

static OMX_ERRORTYPE omx_empty_buffer_done(OMX_HANDLETYPE omx_handle,
    OMX_PTR app_data, OMX_BUFFERHEADERTYPE *omx_header)
{
	priv_t *p = (priv_t*) app_data;
	omx_port_t *port;
	omx_buffer_t *buffer = omx_header->pAppPrivate;

	if (!buffer)
		return OMX_ErrorUndefined;
	
	port = buffer->dir == OMX_DirInput ? &p->port_in : &p->port_out;

	pthread_mutex_lock(&port->mtx);
	port->nb_omx_buffers--;
	buffer->state = BUF_STATE_READY;
	pthread_cond_signal(&port->cond);
	pthread_mutex_unlock(&port->mtx);

	if (buffer->dir == OMX_DirInput) {
		BUF_LOG("empty  in (%2d) %2d/%2d",           buffer->index, port->nb_omx_buffers, port->nb_omx_buffers_max);
	} else {
//		BUF_LOG("empty  out(%2d) %2d/%2d  time %8lld", buffer->index, port->nb_omx_buffers, port->nb_omx_buffers_max, buffer->header->nTimeStamp);
	}

	return OMX_ErrorNone;
}

static OMX_ERRORTYPE omx_fill_buffer_done(OMX_HANDLETYPE omx_handle,
    OMX_PTR app_data, OMX_BUFFERHEADERTYPE *omx_header)
{
	priv_t *p = (priv_t*) app_data;
	omx_port_t *port;
	omx_buffer_t *buffer = omx_header->pAppPrivate;

	if (!buffer)
		return OMX_ErrorUndefined;
	
	port = buffer->dir == OMX_DirInput ? &p->port_in : &p->port_out;

	pthread_mutex_lock(&port->mtx);
	port->nb_omx_buffers--;
	buffer->state = BUF_STATE_OMX_DONE;
	TAILQ_INSERT_TAIL(&port->fifo, buffer, next);
	pthread_cond_signal(&port->cond);
	pthread_mutex_unlock(&port->mtx);

	if (buffer->dir == OMX_DirInput) {
//		BUF_LOG("filled in (%2d) %2d/%2d",           buffer->index, port->nb_omx_buffers, port->nb_omx_buffers_max);
	} else {
		BUF_LOG("filled out(%2d) %2d/%2d\t\t\t\ttime %8lld", buffer->index, port->nb_omx_buffers, port->nb_omx_buffers_max, buffer->header->nTimeStamp);
	}

	return OMX_ErrorNone;
}

static OMX_CALLBACKTYPE omx_callbacks = {
	omx_event_handler,
	omx_empty_buffer_done,
	omx_fill_buffer_done
};

static void omx_print_port(priv_t *p, omx_port_t *port)
{
	OMX_PARAM_PORTDEFINITIONTYPE *def = &port->omx_def;

	serprintf("%s PORT: DEFINITIONTYPE: nSize: %u / nVersion: %u %u %u %u\n"
	    "\t%ux%u / stride: %ux%u / size: %u|%u\n"
	    "\tenabled: %d / populated: %d / contiguous: %d / alignement: %u\n"
	    "\tnb_buf: %u / nb_min_buf: %u / bitrate: %u / framerate: %u\n"
	    "\tcodec: %s / format: %s\n\n",
	    (def->eDir == OMX_DirInput) ? "INPUT" : "OUTPUT",
	    def->nSize, def->nVersion.s.nVersionMajor, def->nVersion.s.nVersionMinor,
	    def->nVersion.s.nRevision, def->nVersion.s.nStep,
	    def->format.video.nFrameWidth, def->format.video.nFrameHeight,
	    def->format.video.nStride, def->format.video.nSliceHeight,
	    def->nBufferSize, p->out_buffer_size,
	    def->bEnabled, def->bPopulated, def->bBuffersContiguous, def->nBufferAlignment,
	    def->nBufferCountActual, def->nBufferCountMin,
	    def->format.video.nBitrate, def->format.video.xFramerate,
	    p->role,
	    omx_get_format_name(port->omx_format));
}

static int omx_event_push_cmd(priv_t *p, OMX_COMMANDTYPE cmd)
{
	pthread_mutex_lock(&p->wait.mtx);
	p->wait.cmd = cmd;
	pthread_cond_broadcast(&p->wait.cond);
	pthread_mutex_unlock(&p->wait.mtx);
	return 0;
}

static int omx_event_push_error(priv_t *p, OMX_ERRORTYPE err)
{
	pthread_mutex_lock(&p->wait.mtx);
	p->wait.error = err;
	pthread_cond_broadcast(&p->wait.cond);
	pthread_mutex_unlock(&p->wait.mtx);
	return 0;
}

static OMX_ERRORTYPE omx_event_get_error(priv_t *p)
{
	OMX_ERRORTYPE err;

	pthread_mutex_lock(&p->wait.mtx);
	err = p->wait.error;
	pthread_mutex_unlock(&p->wait.mtx);
	return err;
}

static OMX_ERRORTYPE omx_event_wait_cmd_locked(priv_t *p, OMX_COMMANDTYPE cmd)
{
	while (p->wait.cmd != cmd && p->wait.error == OMX_ErrorNone)
		pthread_cond_wait(&p->wait.cond, &p->wait.mtx);
	p->wait.cmd = OMX_CommandMax;
	return p->wait.error;
}

static OMX_ERRORTYPE omx_event_wait_cmd(priv_t *p, OMX_COMMANDTYPE cmd)
{
	OMX_ERRORTYPE err;

	pthread_mutex_lock(&p->wait.mtx);
	err = omx_event_wait_cmd_locked(p, cmd);
	pthread_mutex_unlock(&p->wait.mtx);
	return err;
}

static OMX_ERRORTYPE omx_send_cmd_async(priv_t *p, OMX_COMMANDTYPE cmd, OMX_U32 param, OMX_PTR data)
{
	if (cmd == OMX_CommandStateSet) {
		CMD_LOG("> %s: %s", omx_get_command_name(cmd), omx_get_state_name(param));
	} else {
		CMD_LOG("> %s: 0x%X", omx_get_command_name(cmd), param);
	}
	return OMX_SendCommand(p->omx_handle, cmd, param, data);
}

static OMX_ERRORTYPE omx_send_cmd_sync_locked(priv_t *p, OMX_COMMANDTYPE cmd, OMX_U32 param, OMX_PTR data)
{
	OMX_ERRORTYPE err;

	err = omx_send_cmd_async(p, cmd, param, data);
	if (err != OMX_ErrorNone)
		return err;
	err = omx_event_wait_cmd_locked(p, cmd);
	return err;
}

static OMX_ERRORTYPE omx_send_cmd_sync(priv_t *p, OMX_COMMANDTYPE cmd, OMX_U32 param, OMX_PTR data)
{
	OMX_ERRORTYPE err;

	err = omx_send_cmd_async(p, cmd, param, data);
	if (err != OMX_ErrorNone)
		return err;
	err = omx_event_wait_cmd(p, cmd);
	return err;
}

static void omx_port_set_def_size(priv_t *p, omx_port_t *port)
{
	OMX_PARAM_PORTDEFINITIONTYPE *def = &port->omx_def;

	if (def->eDir == OMX_DirInput) {
		def->nBufferSize          = def->format.video.nFrameWidth * def->format.video.nFrameHeight * 2;
		def->format.video.nStride = def->format.video.nFrameWidth;
	} else {
		omx_get_format_sizes(def->format.video.eColorFormat, def->format.video.nFrameWidth, def->format.video.nFrameHeight, &def->nBufferSize, &def->format.video.nStride);
		p->out_buffer_size = def->nBufferSize;
	}
}

static OMX_ERRORTYPE omx_port_free_buffers(omx_port_t *port, int wait)
{
	if( !port->buffers )
		return 0;
	
	int i = 0;
	OMX_ERRORTYPE err = OMX_ErrorNone;
	omx_buffer_t *buffer;

	if (wait)
		omx_buffer_wait_all_done(port);

	while ((buffer = port->buffers[i++]) != NULL) {
		if (buffer->header && buffer->state != BUF_STATE_OMX) {
			CLOG("OMX_FreeBuffer(%d)", buffer->index);
			err = OMX_FreeBuffer(port->omx_handle, port->index, buffer->header);
			buffer->header = NULL;
			CHECK_ERR("OMX_FreeBuffer");
		}
	}
error:
	return err;
}

static void set_video_props(priv_t *p, VIDEO_PROPERTIES *video)
{
	OMX_PARAM_PORTDEFINITIONTYPE *def = &p->port_out.omx_def;

	/*
	 * Hack: qcom ics use stride/slice for buffer size
	 */
	if (device_get_android_version() == ANDROID_VERSION_ICS &&
	    strncasecmp(p->comp, "OMX.qcom.", strlen("OMX.qcom.")) == 0) {
		p->width = def->format.video.nStride;
		p->height = def->format.video.nSliceHeight;
	} else {
		/*
		 * Hack: OMX.SEC.*.Decoder wants buffer size to be 32 aligned
		 */
		if (strncasecmp(p->comp, "OMX.SEC.", strlen("OMX.SEC.")) == 0 &&
		    strstr(p->comp, ".Decoder") && def->format.video.eColorFormat == OMX_COLOR_FormatYUV420Planar) {
			p->width = ALIGN(def->format.video.nFrameWidth, 32);
			p->height = ALIGN(def->format.video.nFrameHeight, 32);
		} else {
			p->width = def->format.video.nFrameWidth;
			p->height = def->format.video.nFrameHeight;
		}
	}

	video->padded_width  = p->width;
	video->padded_height = p->height;
	video->colorspace    = p->colorspace;
}

static OMX_ERRORTYPE omx_port_reconfigure(priv_t *p, VIDEO_PROPERTIES *video, omx_port_t *port, int inbuffer_size)
{
	int i, nb_sink_buffers = 0;
	OMX_ERRORTYPE err;
	OMX_PARAM_PORTDEFINITIONTYPE *def = &port->omx_def;
	omx_buffer_t *buffer;

	err = omx_send_cmd_async(p, OMX_CommandPortDisable, port->index, NULL);
	CHECK_ERR("omx_send_cmd_async(OMX_CommandPortDisable)");

	err = omx_port_free_buffers(port, 1);
	CHECK_ERR("omx_port_free_buffers");

	err = omx_event_wait_cmd(p, OMX_CommandPortDisable);
	CHECK_ERR("omx_event_wait_cmd(OMX_CommandPortDisable)");

	err = OMX_GetParameter(p->omx_handle, OMX_IndexParamPortDefinition, def);
	CHECK_ERR("OMX_GetParameter");
	OMX_INIT_SIZE(*def); // OMX_QCOM override the size with a wrong one

	CLOG("definition.nBufferCountActual: %d - port->nb_buffers_max: %d - port->nb_omx_buffers_max: %d", def->nBufferCountActual, port->nb_buffers_max, port->nb_omx_buffers_max);
	CLOG("definition.WxH %dx%d  videoWxH %dx%d", def->format.video.nFrameWidth, def->format.video.nFrameHeight, video->padded_width, video->padded_height );

	if (port->omx_def.eDir == OMX_DirOutput) {
		p->realloc = (def->nBufferCountActual > port->nb_omx_buffers_max ||
			 def->format.video.nFrameWidth > p->width ||
			 def->format.video.nFrameHeight > p->height);

		nb_sink_buffers = port->nb_buffers_max - port->nb_omx_buffers_max;
	} else {
		if (inbuffer_size)
			def->nBufferSize = inbuffer_size;
	}

	// if omx wants less buffers: force previous value (in order to don't realloc)
	if (port->nb_omx_buffers_max > def->nBufferCountActual) {
		def->nBufferCountActual = port->nb_omx_buffers_max;
	}

	port->nb_omx_buffers_max = def->nBufferCountActual;
	def->nBufferCountActual += nb_sink_buffers;

	if (port->flags & OMX_PORT_FLAG_SET_SIZE)
		omx_port_set_def_size(p, port);

	if (p->realloc) {
		CLOG("need sink realloc");
		// need realloc
		for (i = 0; i < port->nb_buffers_max; ++i)
			free(port->buffers[i]);
		free(port->buffers);
		port->buffers = NULL;
		port->nb_buffers_max = 0;
		set_video_props(p, video);
		return 0;
	}

	err = OMX_SetParameter(p->omx_handle, OMX_IndexParamPortDefinition, def);
	CHECK_ERR("OMX_SetParameter(OMX_IndexParamPortDefinition)");

	p->allocating = 1;
	err = omx_send_cmd_async(p, OMX_CommandPortEnable, port->index, NULL);
	CHECK_ERR("omx_send_cmd_async(OMX_CommandPortEnable)");

	for (i = 0; i < port->nb_buffers_max; ++i) {
		omx_buffer_t *buffer = port->buffers[i];

		if (port->flags & OMX_PORT_FLAG_USE_ANDROID_NATIVE_BUFFERS) {
			CLOG("OMX_UseBuffer(handle, %d, %d, 0, %u, %p)", i, port->index, port->omx_def.nBufferSize, buffer->android_handle);
			err = OMX_UseBuffer(p->omx_handle, &port->buffers[i]->header, port->index, 0, port->omx_def.nBufferSize, buffer->android_handle);
			CHECK_ERR("OMX_UseBuffer");
		} else {
			CLOG("OMX_AllocateBuffer(handle, %d, %d, 0, %u)", i, port->index, port->omx_def.nBufferSize);
			err = OMX_AllocateBuffer(p->omx_handle, &port->buffers[i]->header, port->index, 0, port->omx_def.nBufferSize);
			CHECK_ERR("OMX_AllocateBuffer");
		}
		buffer->header->pAppPrivate = port->buffers[i];
	}

	err = omx_event_wait_cmd(p, OMX_CommandPortEnable);
	CHECK_ERR("omx_event_wait_cmd(OMX_CommandPortEnable)");
	p->allocating = 0;

	err = OMX_GetParameter(p->omx_handle, OMX_IndexParamPortDefinition, def);
	CHECK_ERR("OMX_GetParameter");
	OMX_INIT_SIZE(*def); // OMX_QCOM override the size with a wrong one

	omx_print_port(p, port);

	if (port->omx_def.eDir == OMX_DirOutput) {
		set_video_props(p, video);
	}

	pthread_mutex_lock(&port->mtx);
	for (i = 0; i < port->nb_buffers_max; ++i) {
		buffer = port->buffers[i];
		if (buffer->state == BUF_STATE_OMX_DONE) {
			err = omx_buffer_send_locked(port, buffer);
			CHECK_ERR_LOCKED("omx_buffer_send_locked");
		}
	}
	pthread_mutex_unlock(&port->mtx);
	return OMX_ErrorNone;
error:
	return err;
}

static void omx_comp_close(priv_t *p)
{
	int i;
	OMX_ERRORTYPE err;
	OMX_STATETYPE state;

	CLOG("%s", p->comp);

	if (p->allocating) {
		/*
		 * Fail during allocation: we are screwed, free what we can and
		 * and abandon the ship.
		 *
		 * OMX is stuck: no way to go to Idle or Loaded state and free
		 * buffers owned by OMX. The only hope is that OMX_FreeHandle
		 * will clean everything but it may depends on OMX
		 * implementation.
		 */
		for (i = 0; i < p->port_nb; ++i)
			omx_port_free_buffers(p->ports[i], 0);
		goto error;
	}

	// cancel all errors when closing omx
	omx_event_push_error(p, OMX_ErrorNone);

	err = OMX_GetState(p->omx_handle, &state);
	CHECK_ERR("OMX_GetState");

	if (state == OMX_StateExecuting) {
		err = omx_send_cmd_sync(p, OMX_CommandStateSet, OMX_StateIdle, NULL);
		CHECK_ERR("omx_send_cmd_sync(OMX_CommandStateIdle)");
	}

	err = OMX_GetState(p->omx_handle, &state);
	CHECK_ERR("OMX_GetState");

	if (state == OMX_StateIdle) {
		err = omx_send_cmd_async(p, OMX_CommandStateSet, OMX_StateLoaded, NULL);
		CHECK_ERR("omx_send_cmd_async(OMX_CommandStateLoaded)");

		for (i = 0; i < p->port_nb; ++i) {
			err = omx_port_free_buffers(p->ports[i], 1);
			CHECK_ERR("omx_port_free_buffers");
		}
		err = omx_event_wait_cmd(p, OMX_CommandStateSet);
		CHECK_ERR("omx_event_wait_cmd(OMX_CommandStateSet)");
	}

error:
	for (i = 0; i < p->port_nb; ++i) {
		int j;
		omx_port_t *port = p->ports[i];
		p->ports[i] = NULL;

		if (port->buffers) {
			for (j = 0; j < port->nb_buffers_max; ++j) {
				if (port->buffers[j])
					free(port->buffers[j]);
			}
			free(port->buffers);
		}

		pthread_cond_destroy(&port->cond);
		pthread_mutex_destroy(&port->mtx);
		memset(port, 0, sizeof(omx_port_t));
	}
	p->port_nb = 0;
	if (p->omx_handle) {
		OMX_FreeHandle(p->omx_handle);
		p->omx_handle = NULL;
	}

	if (p->comp)
		free(p->comp);
}

static void test_colors(priv_t *p)
{
	OMX_ERRORTYPE err;
	OMX_VIDEO_PARAM_PORTFORMATTYPE format;

	OMX_INIT_STRUCTURE(format);
	format.nPortIndex = p->port_out.omx_def.nPortIndex;
	format.nIndex = 0;
	for (;;) {
		err = OMX_GetParameter(p->omx_handle, OMX_IndexParamVideoPortFormat, &format);
		if (err != OMX_ErrorNone)
			break;
		CLOG("index(%d): eCompressionFormat: 0x%X, eColorFormat: 0x%X, xFramerate: %u", format.nIndex, format.eCompressionFormat, format.eColorFormat, format.xFramerate);
		format.nIndex++;
	}

}

static int omx_comp_setup_output(priv_t *p, omx_port_t *port, VIDEO_PROPERTIES *video)
{
	OMX_ERRORTYPE err;
	int graphic_buffers_enabled = 0;
	OMX_PARAM_PORTDEFINITIONTYPE *def = &port->omx_def;

	p->hw_usage = 0;
	port->omx_format = def->format.video.eColorFormat;

	if (p->buf_type == BUFFER_TYPE_HW) {
		OMX_PARAM_PORTDEFINITIONTYPE omx_def = *def;
		int new_hal_format = 0;

		err = OMXAndroid_EnableGraphicBuffers(port->omx_handle, port->index, OMX_TRUE);
		CHECK_ERR("OMXAndroid_EnableGraphicBuffers");
		graphic_buffers_enabled = 1;
		err = OMX_GetParameter(p->omx_handle, OMX_IndexParamPortDefinition, &omx_def);
		OMX_INIT_SIZE(omx_def);
		CHECK_ERR("OMX_GetParameter(OMX_IndexParamPortDefinition) after enabling Android HW buffers");

		/*
		 * Hack: Galaxy S II / III (OMX.SEC.*):
		 * Some decoders version don't return a good eColorFormat.
		 * Always assume eColorFormat is OMX_COLOR_FormatYUV420SemiPlanar
		 */
		if (omx_def.format.video.eColorFormat == OMX_COLOR_FormatYUV420Planar &&
		    strncmp(p->comp, "OMX.SEC.", strlen("OMX.SEC.")) == 0)
			omx_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;

		p->colorspace = omx_get_colorspace(omx_def.format.video.eColorFormat);

serprintf("out: omx sw format : %08X Android Native format : %08X -> cs %d\n", port->omx_format, omx_def.format.video.eColorFormat, p->colorspace );
		if (p->colorspace == -1)
			goto error;

		if ((new_hal_format = android_get_hal_format(p->colorspace, BUFFER_TYPE_HW)) != 0) {
serprintf("out: new 0x%08X\n", new_hal_format );
			port->omx_def = omx_def;
			CLOG("Force OMX_format to 0x%x\n", def->format.video.eColorFormat);
			port->omx_format = def->format.video.eColorFormat;
			port->flags |= OMX_PORT_FLAG_USE_ANDROID_NATIVE_BUFFERS;
			CLOG("Using Android Graphic Buffers: omx_format: 0x%X - av_color: %d - hal_format: 0x%X", port->omx_format, p->colorspace, new_hal_format);
			p->use_shadow = 0;
			OMXAndroid_GetGraphicBufferUsage(port->omx_handle, port->index, (OMX_U32*)&p->hw_usage);
		} else {
			/* We are on SW Buffers*/
			goto error;
		}
	} else {
		p->use_shadow = 1;
		p->colorspace = omx_get_colorspace(def->format.video.eColorFormat);
serprintf("out: omx sw format : %08X\n", port->omx_format);
	}
	return 0;
error:
	if (graphic_buffers_enabled)
		OMXAndroid_EnableGraphicBuffers(port->omx_handle, port->index, OMX_FALSE);
	return -1;
}

static int omx_comp_open(priv_t *p, VIDEO_PROPERTIES *video, const char *comp, const char *role)
{
	int i;
	OMX_ERRORTYPE err;
	OMX_PARAM_COMPONENTROLETYPE omx_role;
	OMX_PORT_PARAM_TYPE omx_param;
	OMX_PARAM_PORTDEFINITIONTYPE omx_def, *def;


	if (strstr(comp, ".secure"))
		return 0; // we don't do secure things
	if (strncmp(comp, "OMX.google.", strlen("OMX.google.")) == 0)
		return 0; // don't try stagefright sw codecs
	if (strncmp(comp, "OMX.ARICENT.", strlen("OMX.ARICENT.")) == 0)
		return 0; // doesn't work on HTC One
	if ((device_get_hw_type() == HW_TYPE_RK30 || device_get_hw_type() == HW_TYPE_RK29) &&
	    strcmp(role, "video_decoder.m2v") == 0)
		return 0; // MPEG2 on rk doesn't work TODO: why ?

	p->comp = strdup(comp);
	p->role = role;

	CLOG("%s / %s", p->comp, role);

	err = OMX_GetHandle(&p->omx_handle, (OMX_STRING) p->comp, p, &omx_callbacks);
	CHECK_ERR("OMX_GetHandle");

	/* set component role */
	OMX_INIT_STRUCTURE(omx_role);
	strcpy((char*)omx_role.cRole, role);
	err = OMX_SetParameter(p->omx_handle, OMX_IndexParamStandardComponentRole, &omx_role);
	CHECK_ERR("OMX_SetParameter(OMX_IndexParamStandardComponentRole)");

	/* Find the input / output ports */
	OMX_INIT_STRUCTURE(omx_param);
	err = OMX_GetParameter(p->omx_handle, OMX_IndexParamVideoInit, &omx_param);
	if (err != OMX_ErrorNone)
		omx_param.nPorts = 0;

	for (i = 0; i < omx_param.nPorts; ++i) {
		omx_port_t *port;

		OMX_INIT_STRUCTURE(omx_def);
		omx_def.nPortIndex = omx_param.nStartPortNumber + i;

		err = OMX_GetParameter(p->omx_handle, OMX_IndexParamPortDefinition, &omx_def);
		CHECK_ERR("OMX_GetParameter(OMX_IndexParamPortDefinition)");
		OMX_INIT_SIZE(omx_def); // OMX_QCOM override the size with a wrong one

		if (omx_def.eDir == OMX_DirInput) {
			port = &p->port_in;
		} else {
			port = &p->port_out;
		}
		port->omx_handle = p->omx_handle;
		port->index      = omx_def.nPortIndex;
		port->omx_def    = omx_def;
		pthread_mutex_init(&port->mtx, NULL);
		pthread_cond_init(&port->cond, NULL);
		TAILQ_INIT(&port->fifo);

		/* set port definition */

		def = &port->omx_def;
		def->format.video.nFrameWidth  = video->width;
		def->format.video.nFrameHeight = video->height;

		if (def->eDir == OMX_DirInput) {
			/* input port */
			port->omx_format = def->format.video.eColorFormat;
			if (def->format.video.eCompressionFormat == OMX_VIDEO_CodingUnused)
				CERR(OMX_ErrorUndefined, "eCompressionFormat == OMX_VIDEO_CodingUnused");
		} else {
			/* output port */
			if (omx_comp_setup_output(p, port, video))
				CERR(OMX_ErrorUndefined, "omx_comp_setup_output failed");
		}

		err = OMX_SetParameter(port->omx_handle, OMX_IndexParamPortDefinition, def);
		CHECK_ERR("OMX_SetParameter(OMX_IndexParamPortDefinition)");

		port->nb_buffers_max = port->nb_omx_buffers_max = def->nBufferCountActual;
		port->valid = 1;
	}
	if (!p->port_in.valid || !p->port_out.valid)
		CERR(OMX_ErrorUndefined, "input and output port not found");
	p->ports[0] = &p->port_in;
	p->ports[1] = &p->port_out;
	p->ports[2] = NULL;
	p->port_nb = 2;

	// XXX: do we need that ?
#if 0
	if (strncmp(comp, "OMX.SEC.", strlen("OMX.SEC.")) == 0) {
		OMX_INDEXTYPE index;
		err = OMX_GetExtensionIndex(p->omx_handle, (OMX_STRING) "OMX.SEC.index.ThumbnailMode", &index);
		if(err == OMX_ErrorNone) {
			OMX_BOOL enable = OMX_TRUE;
			err = OMX_SetConfig(p->omx_handle, index, &enable);
			if (err == OMX_ErrorNone)
				CLOG("Set OMX.SEC.index.ThumbnailMode");
			else
				CLOG("Unable to set ThumbnailMode");
		} else {
#define OMX_IndexVendorSetYUV420pMode 0x7f000003
			OMX_BOOL enable = OMX_TRUE;
			/* Needed on Samsung Galaxy S II */
			err = OMX_SetConfig(p->omx_handle, OMX_IndexVendorSetYUV420pMode, &enable);
			if (err == OMX_ErrorNone)
				CLOG("Set OMX_IndexVendorSetYUV420pMode successfully");
			else
				CLOG("Unable to set OMX_IndexVendorSetYUV420pMode");
		}
		err = OMX_ErrorNone;
	}
#endif

	switch (video->format) {
	case VIDEO_FORMAT_WMV3:
		p->do_header = 1;
		break;
	case VIDEO_FORMAT_H264:
	case VIDEO_FORMAT_MPG4:
	case VIDEO_FORMAT_H263:
	default:
		p->do_header = 0;
	}

	return 1;
error:
	omx_comp_close(p);
	return 0;
}

static int video_dec_open(STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, void *ctx, int *_need_flush, int *_need_reorder )
{
	priv_t *p = (priv_t*)dec->priv;
	OMX_ERRORTYPE err;
	int comp_opened = 0;
	char **comp_list, **comp_it, *comp;
	const char *role;

	int need_flush    = 1;
	int need_reorder  = 0;
	
	int format;
	
	if (video->format == VIDEO_FORMAT_H264 && video->sps.valid && video->profile >= H264_PROFILE_HIGH10) {
		CLOG("OMX can't do Hi10P, abort");
		return 1;
	}
	if (p->buf_type == BUFFER_TYPE_HW &&
	    android_get_av_color(p->buf_type) == -1) {
		CLOG("no color configured for %s on %s device", dec->name, device_get_hw_type_name());
		return 1;
	}
	CLOG();

	switch (video->format) {
	case VIDEO_FORMAT_WMV1:
	case VIDEO_FORMAT_WMV2:
	case VIDEO_FORMAT_WMV3:
	case VIDEO_FORMAT_VC1:
	case VIDEO_FORMAT_MPEG:
	case VIDEO_FORMAT_MPG4:
	case VIDEO_FORMAT_H263:
		need_flush = 1;
		break;
	case VIDEO_FORMAT_H264:
	case VIDEO_FORMAT_HEVC:
		need_flush = 0;
		break;
	default:
serprintf("OMX: unknown video format %i\r\n", video->format);
		return 1;
	}
	
	// Initialize the init time params
	dec->ctx = ctx;
	
	p->reorder_pts = video->reorder_pts;

	if( _need_flush )
		*_need_flush = need_flush;
	if( _need_reorder )
		*_need_reorder = need_reorder;

	if (OMX_Init() != OMX_ErrorNone) {
		CLOG("OMX_Init() failed");
		return 1;
	}

	pthread_mutex_init(&p->wait.mtx, NULL);
	pthread_cond_init(&p->wait.cond, NULL);
	pthread_mutex_lock(&p->wait.mtx);
	p->wait.cmd = OMX_CommandMax;
	pthread_mutex_unlock(&p->wait.mtx);

#ifdef OMX_FORCE_COMP
	comp_opened = omx_comp_open(p, video, OMX_FORCE_COMP, OMX_FORCE_ROLE);
#else
	comp_it = comp_list = omx_get_components(video->format, 1, &role);
	if (!comp_list) {
		CLOG("no component found for format: %d", video->format);
		return 1;
	}
	while ((comp = *(comp_it++)) != NULL) {
		if (!comp_opened)
			comp_opened = omx_comp_open(p, video, comp, role);
		free(comp);
	}
	free(comp_list);
#endif
	if (comp_opened) {
		set_video_props(p, video);
#if 0
		if (device_get_hw_type() == HW_TYPE_OMAP4 &&
		    strcmp(p->comp, "OMX.TI.DUCATI1.VIDEO.DECODER") == 0) {
			/*
			 * The true video size is returned after the first
			 * decode.  Apply ducati paddings to video size in
			 * order to avoid a full realloc when the component
			 * return the good video size.
			 */
			switch (video->format) {
			case VIDEO_FORMAT_MPG4:
			case VIDEO_FORMAT_H263:
				video->padded_width  = ALIGN(video->padded_width + 2 * DUCATI_PADX_MPEG4, 128);
				video->padded_height = ALIGN(video->padded_height + 4 * DUCATI_PADY_MPEG4, 16);
#ifdef DEBUG_MSG
				force_deblocking_value(p);
#endif
				break;
			case VIDEO_FORMAT_H264:
				video->padded_width  = ALIGN(video->padded_width + 2 * DUCATI_PADX_H264, 128);
				video->padded_height = ALIGN(video->padded_height + 4 * DUCATI_PADY_H264, 16);
				break;
			default:
				break;
			}
		} else
#endif
		if (device_get_hw_type() == HW_TYPE_RK30 && strcmp(p->comp, "OMX.rk.video_decoder") == 0) {
			p->rk30_hack_size_nok = 1;
		}
	}

	if (video->reorder_pts && (!strncmp(p->comp, "OMX.SEC.avc", strlen("OMX.SEC.avc")) || pts_force_reorder)) {
		STREAM *s = dec->ctx;
		if( s && s->ro_ctx) {
			pts_ro_run(s->ro_ctx);
			p->reorder_pts = 0;
		}
	}

	p->width = p->crop_w = video->width;
	p->height = p->crop_h = video->height;

	dec->video = &dec->_video;
	memcpy(dec->video, video, sizeof(VIDEO_PROPERTIES));
	dec->is_open = comp_opened;

	XDM_id_flush( &p->XDM_ctx );
	XDM_ts_flush( &p->XDM_ctx );

	return dec->is_open ? 0 : 1;
error:
	return 1;
}

static int video_dec_prepare(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **frames, int num_frames)
{
	int i, j, wait_idle = 0, wait_enabled[2] = {0, 0};
	priv_t *p = (priv_t*)dec->priv;
	OMX_ERRORTYPE err;
	OMX_STATETYPE state;
	omx_port_t *port;
	
	CLOG("num_frames: %d, port_out.omx_def.nBufferCountActual: %d", num_frames, p->port_out.omx_def.nBufferCountActual);

	for (i = 0; i < p->port_nb; ++i) {
		port = p->ports[i];

		err = OMX_GetParameter(port->omx_handle, OMX_IndexParamPortDefinition, &port->omx_def);
		CHECK_ERR("OMX_GetParameter(OMX_IndexParamPortDefinition)");
		OMX_INIT_SIZE(port->omx_def); // OMX_QCOM override the size with a wrong one

		if (port->omx_def.eDir == OMX_DirOutput) {
			port->nb_buffers_max = port->omx_def.nBufferCountActual = num_frames;

			err = OMX_SetParameter(p->omx_handle, OMX_IndexParamPortDefinition, &port->omx_def);
			CHECK_ERR("OMX_SetParameter(OMX_IndexParamPortDefinition)");
		}
	}

	err = OMX_GetState(p->omx_handle, &state);
	CHECK_ERR("OMX_GetState");

	p->allocating = 1;
	if (state == OMX_StateLoaded) {
		CLOG("OMX_StateLoaded");
		/*
		 * first call of video_dec_prepare, ports are already enabled
		 * we need to alloc input and output buffer during the OMX_StateIdle transition
		 */
		err = omx_send_cmd_async(p, OMX_CommandStateSet, OMX_StateIdle, NULL);
		CHECK_ERR("omx_send_cmd_async(OMX_CommandStateSet)");
		wait_idle = 1;
	} else if (state == OMX_StateExecuting) {
		CLOG("OMX_StateExecuting");
		/*
		 * not first call of video_dec_prepare, component is already executing
		 * ports that need a realloc should be disabled, alloc port buffers during the 
		 * OMX_CommandPortEnable transition
		 */
		for (i = 0; i < p->port_nb; ++i) {
			port = p->ports[i];
			if (!port->omx_def.bEnabled) {
				err = omx_send_cmd_async(p, OMX_CommandPortEnable, port->index, NULL);
				CHECK_ERR("omx_send_cmd_sync(OMX_CommandPortEnable)");
				wait_enabled[i] = 1;
			}
		}
	} else {
		CERR(OMX_ErrorUndefined, "invalid state");
	}

	for (i = 0; i < p->port_nb; ++i) {
		port = p->ports[i];

		if (port->buffers)
			continue;

		OMX_PARAM_PORTDEFINITIONTYPE *def = &port->omx_def;
		CLOG("%s", def->eDir == OMX_DirOutput ? "OUTPUT:" : "INPUT:");

		port->buffers = calloc(1, (port->nb_buffers_max + 1)* sizeof(omx_buffer_t*));
		if (!port->buffers)
			CERR(OMX_ErrorInsufficientResources, "calloc failed");

		for (j = 0; j < port->nb_buffers_max; ++j) {
			VIDEO_FRAME *f = NULL;

			port->buffers[j] = calloc(1, sizeof(omx_buffer_t));
			if (!port->buffers[j])
				CERR(OMX_ErrorInsufficientResources, "malloc failed");

			if (port->flags & OMX_PORT_FLAG_USE_ANDROID_NATIVE_BUFFERS) {
				OMX_PTR android_handle;
				f = frames[j];
				android_handle = (OMX_PTR) f->android_handle;
				if (!android_handle)
					CERR(OMX_ErrorInsufficientResources, "frame has no android_handle");

				CLOG("OMX_UseBuffer(handle, %d, %d, 0, %u, %p)", j, port->index, port->omx_def.nBufferSize, android_handle);
				err = OMX_UseBuffer(p->omx_handle, &port->buffers[j]->header, port->index, 0, port->omx_def.nBufferSize, android_handle);
				CHECK_ERR("OMX_UseBuffer");
				port->buffers[j]->android_handle = android_handle;
			} else {
				if (def->eDir == OMX_DirOutput) {
					// assign to output frame
					f = frames[j];
					if( p->out_buffer_size > port->omx_def.nBufferSize ) {
serprintf("override!\n");
						def->nBufferSize = p->out_buffer_size;
					}
				}
				CLOG("OMX_AllocateBuffer(handle, %d, %d, 0, %u)", j, port->index, port->omx_def.nBufferSize);
				err = OMX_AllocateBuffer(p->omx_handle, &port->buffers[j]->header, port->index, 0, port->omx_def.nBufferSize);
				CHECK_ERR("OMX_AllocateBuffer");
				port->buffers[j]->android_handle = NULL;
			}

			port->buffers[j]->header->pAppPrivate = port->buffers[j];

			port->buffers[j]->frame = f;
			port->buffers[j]->index = j;
			port->buffers[j]->dir   = port->omx_def.eDir;
			port->buffers[j]->state = BUF_STATE_INIT;
		}
	}

	if (wait_idle) {
		err = omx_event_wait_cmd(p, OMX_CommandStateSet);
		CHECK_ERR("omx_event_wait_cmd(OMX_CommandStateSet)");
		err = omx_send_cmd_sync(p, OMX_CommandStateSet, OMX_StateExecuting, NULL);
		CHECK_ERR("omx_send_cmd_sync(OMX_CommandStateSet)");
	} else {
		for (i = 0; i < p->port_nb; ++i) {
			port = p->ports[i];
			if (wait_enabled[i]) {
				err = omx_event_wait_cmd(p, OMX_CommandPortEnable);
				CHECK_ERR("omx_event_wait_cmd(OMX_CommandPortEnable)");
				err = OMX_GetParameter(port->omx_handle, OMX_IndexParamPortDefinition, &port->omx_def);
				CHECK_ERR("OMX_GetParameter(OMX_IndexParamPortDefinition)");
			}
		}
	}
	p->allocating = 0;

	for (i = 0; i < p->port_nb; ++i)
		omx_print_port(p, p->ports[i]);

	set_video_props(p, dec->video);
	hack_omx_sec_dec_crop(p);

	p->realloc = 0;

	return 0;
error:
	return 1;
}

static int video_dec_close( STREAM_DEC_VIDEO *dec )
{
	if (!dec->is_open)
		return 1;
	CLOG();

	priv_t *p = (priv_t*)dec->priv;

	STREAM *s = dec->ctx;
	if( s && s->ro_ctx) {
		pts_ro_stop(s->ro_ctx);
	}

	omx_comp_close(p);

	pthread_cond_destroy(&p->wait.cond);
	pthread_mutex_destroy(&p->wait.mtx);
	
	dec->is_open = 0;
 	return 0;
}

static int video_dec_in( STREAM_DEC_VIDEO *dec, VIDEO_FRAME **data_frame, int *pdecoded, int *ptime )
{
	priv_t *p = (priv_t*)dec->priv;
	int decoded = 0, inbuffer_size = 0;
	int i, event, wait;
	OMX_ERRORTYPE err;
	omx_port_t *port = NULL;
	omx_buffer_t *buffer;
	VIDEO_FRAME *d = *data_frame;

	//CLOG("data: %p, data_size: %d, time: %d", d->data[0], d->size, d->time);

	if (pdecoded)
		*pdecoded = 0;

	if (ptime)
		*ptime = 0;

	port = &p->port_in;

	err = omx_event_get_error(p);
	if (err != OMX_ErrorNone) {
		STREAM *s = dec->ctx;
		s->video_error = VE_ERROR;
		return 1;
	}
	event = omx_port_get_event(port);
	if (d->size > port->omx_def.nBufferSize) {
		CLOG("input buffer too small: reallocating...");
		event |= EVENT_RECONFIGURE;
		inbuffer_size = d->size * 2;
	}
	if (event & EVENT_RECONFIGURE) {
		CLOG("reconfiguring input port");
		err = omx_port_reconfigure(p, dec->video, port, inbuffer_size);
		CHECK_ERR("omx_port_reconfigure");
	}

	pthread_mutex_lock(&port->mtx);
	while (decoded < d->size && (buffer = omx_buffer_get_free_locked(port, 0)) != NULL) {
		OMX_BUFFERHEADERTYPE *header = buffer->header;
		UCHAR *in_data = NULL;

		header->nOffset = 0;
		if (p->do_header > 0) {
			if (dec->video->format == VIDEO_FORMAT_WMV3) {
				int rcv_size = 0;

				in_data = WMV_get_rcv_header(dec->video, &rcv_size);
				header->nFilledLen = rcv_size;
				p->do_header--;
			} else {
				CERR(OMX_ErrorUndefined, "do_header: unknow video format");
			}

			header->nTimeStamp = 0;
			header->nFlags     = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_CODECCONFIG;
		} else {
			header->nTimeStamp = d->time;
			header->nFilledLen = d->size - decoded;
			header->nFlags     = OMX_BUFFERFLAG_ENDOFFRAME;

			in_data = d->data[0] + decoded;
			decoded += header->nFilledLen;

			XDM_id_put( &p->XDM_ctx, d->time, 0, d->user_ID );
			if( !p->reorder_pts ) {
				XDM_ts_put( &p->XDM_ctx, d->time );
			}

		}

		memcpy(header->pBuffer, in_data, header->nFilledLen);

		//Dump(header->pBuffer, MIN(header->nFilledLen, 128));
		err = omx_buffer_send_locked(port, buffer);
		CHECK_ERR_LOCKED("omx_buffer_send");
	}
	pthread_mutex_unlock(&port->mtx);

	if (pdecoded)
		*pdecoded = decoded;

//	CLOG("end: decoded: %d, time: %d", *pdecoded, *ptime);

	return 0;
error:
	return 1;
}

static int omx_port_set_crop(priv_t *p, omx_port_t *port)
{
	OMX_ERRORTYPE err;
	OMX_CONFIG_RECTTYPE crop_rect;

	OMX_INIT_STRUCTURE(crop_rect);
	crop_rect.nPortIndex = port->omx_def.nPortIndex;
	err = OMX_GetConfig(port->omx_handle, OMX_IndexConfigCommonOutputCrop, &crop_rect);
	CHECK_ERR("OMX_GetConfig(OMX_IndexConfigCommonOutputCrop)");
	p->crop_x  = crop_rect.nLeft;
	p->crop_y  = crop_rect.nTop;
	p->crop_w  = crop_rect.nWidth;
	p->crop_h = crop_rect.nHeight;
	CLOG("crop updated: %d/%d - %d/%d", crop_rect.nLeft, crop_rect.nTop, crop_rect.nWidth, crop_rect.nHeight);

	return 0;
error:
	return 1;
}

static int video_put_out(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pin_frame)
{
	priv_t *p = (priv_t*)dec->priv;
	omx_buffer_t *buffer;
	OMX_ERRORTYPE err;
	int event;
	VIDEO_FRAME *in_frame = pin_frame ? *pin_frame : NULL;

	omx_port_t *port = &p->port_out;

	err = omx_event_get_error(p);
	if (err != OMX_ErrorNone) {
		STREAM *s = dec->ctx;
		s->video_error = VE_ERROR;
		return 1;
	}
	event = omx_port_get_event(port);
	if (event & EVENT_RECONFIGURE) {
		err = omx_port_reconfigure(p, dec->video, port, 0);
		CHECK_ERR("omx_port_reconfigure");
		CLOG("output port reconfigured");
		hack_omx_sec_dec_crop(p);
	}

	if (event & EVENT_CROP)
		omx_port_set_crop(p, port);

	if (event & EVENT_ILACE) {
		OMX_STREAMINTERLACEFORMAT buff_layout;

		OMX_INIT_STRUCTURE(buff_layout);
		buff_layout.nPortIndex = port->omx_def.nPortIndex;
		err = OMX_GetConfig(port->omx_handle, OMX_TI_IndexConfigStreamInterlaceFormats, &buff_layout);
		CHECK_ERR("OMX_GetConfig");

		p->interlaced = buff_layout.bInterlaceFormat ? VIDEO_INTERLACED_ONE_FIELD : VIDEO_PROGRESSIVE;
	}
	if (p->realloc)
		return 0;

	// send in_frame to omx component
	if (in_frame) {
		pthread_mutex_lock(&port->mtx);
		buffer = omx_port_get_buffer_from_frame(port, in_frame);
		if (!buffer) {
			pthread_mutex_unlock(&port->mtx);
			CERR(OMX_ErrorUndefined, "unknown frame: %08X/%d", in_frame, in_frame->index);
		}
		buffer->state = BUF_STATE_READY;
		*pin_frame = NULL;
		int i;
		for (i = 0; i < port->nb_buffers_max && port->nb_omx_buffers < port->nb_omx_buffers_max; ++i) {
			buffer = port->buffers[i];
			if (buffer->state == BUF_STATE_READY) {
				err = omx_buffer_send_locked(port, buffer);
				CHECK_ERR_LOCKED("omx_buffer_send_locked");
			}
		}
		STREAM *s = dec->ctx;
		frame_q_put( &s->codec_q, in_frame );
		pthread_mutex_unlock(&port->mtx);
	}
	return 0;
error:
	return 1;
}

static inline int align(int x, int y)
{
    return (x + y - 1) & ~(y - 1);
}

static int video_get_out(STREAM_DEC_VIDEO *dec, VIDEO_FRAME **pout_frame)
{
	priv_t *p = (priv_t*)dec->priv;
	omx_port_t *port = NULL;
	omx_buffer_t *buffer;

	port = &p->port_out;

	pthread_mutex_lock(&port->mtx);
	buffer = omx_buffer_get_done_locked(port, 0);
	if (buffer) {
		int out_time;
		int out_type;
		int out_ID;
		if( p->reorder_pts ) {
			// get reordered TS 
			out_time = buffer->header->nTimeStamp;
		} else {
			// no reordering, just use the ts_ queue
			out_time = XDM_ts_get( &p->XDM_ctx );
		}
		
		XDM_id_get( &p->XDM_ctx, buffer->header->nTimeStamp, &out_type, &out_ID );

		if (p->rk30_hack_size_nok) {
			OMX_ERRORTYPE err;
			OMX_PARAM_PORTDEFINITIONTYPE *def = &port->omx_def;

			err = OMX_GetParameter(p->omx_handle, OMX_IndexParamPortDefinition, def);
			p->crop_w  = def->format.video.nFrameWidth;
			p->crop_h = def->format.video.nFrameHeight;
			p->rk30_hack_size_nok = 0;
			CLOG("rk30: size changed: %dx%d", p->crop_w, p->crop_h);
		}

		VIDEO_FRAME *frame = buffer->frame;
		if( p->use_shadow ) {
			OMX_PARAM_PORTDEFINITIONTYPE *def = &port->omx_def;

			// hide the OMX frame in a shadow struct
			VIDEO_FRAME *shadow = p->shadow + frame->index;
			frame->priv = shadow;
			
			shadow->width       = p->width;
			shadow->height      = p->height;
			shadow->data[0]     = buffer->header->pBuffer;
			shadow->linestep[0] = def->format.video.nStride;
			shadow->data_size[0] = shadow->linestep[0] * shadow->height;
			shadow->colorspace  = dec->video->colorspace;

			switch (shadow->colorspace) {
			case AV_IMAGE_NV12:
				shadow->linestep[1]  = def->format.video.nStride;
				shadow->data_size[1] = shadow->linestep[1] * shadow->height / 2;
				shadow->data[1]      = shadow->data[0] + shadow->data_size[0];
				break;
			case AV_IMAGE_YV12:
			default:
				shadow->linestep[1]  = shadow->linestep[2]  = def->format.video.nStride / 2;
				shadow->data_size[1] = shadow->data_size[2] = shadow->linestep[1] * shadow->height / 2;
				shadow->data[1]      = shadow->data[0] + shadow->data_size[0];
				shadow->data[2]      = shadow->data[1] + shadow->data_size[1];
				break;
			}
		}
		frame->time       = out_time;
		frame->user_ID    = out_ID;
		frame->valid      = 1;
		frame->ofs_x      = p->crop_x;
		frame->ofs_y      = p->crop_y;
		frame->width      = p->crop_w;
		frame->height     = p->crop_h;
		frame->interlaced = p->interlaced;
		frame->aspect_n   = dec->video->aspect_n;
		frame->aspect_d   = dec->video->aspect_d;
		*pout_frame = frame;

		STREAM *s = dec->ctx;
		VIDEO_FRAME *dummy = frame_q_get_index( &s->codec_q, frame->index );
	} else {
		*pout_frame = NULL;
	}
	pthread_mutex_unlock(&port->mtx);

	return 0;
}

static int video_dec_flush( STREAM_DEC_VIDEO *dec )
{	
	priv_t *p = (priv_t*)dec->priv;

	CLOG();

	if( omx_flush ) {
serprintf("OMX flush in:\n");
		omx_port_t *port = &p->port_in;

		OMX_ERRORTYPE err = omx_send_cmd_sync(p, OMX_CommandFlush, port->index, NULL);
		CHECK_ERR("OMX_CommandFlush");
	}	

	switch (dec->video->format) {
	case VIDEO_FORMAT_MPG4:
	case VIDEO_FORMAT_H263:
		break;
	
	case VIDEO_FORMAT_H264:
		break;
	}

	return 0;
error:
	return 1;
} 

#define FULL_HD_PIXELS (1900 * 1000)
static int video_dec_get_rc( STREAM_DEC_VIDEO *dec, STREAM_RC *rc)
{
	priv_t *p = (priv_t*)dec->priv;

	if( !rc )
		return 1;

	memset( rc, 0, sizeof( STREAM_RC ) );
	
	rc->num_frames = p->port_out.nb_omx_buffers_max;

	CLOG("num_frames: %d", rc->num_frames);

	rc->min_clock  = RC_SYSCLK_VIDDEC_VERY_HIGH;
//	rc->mem_type   = STREAM_MEM_TILER;
	rc->cpu_type   = p->buf_type == BUFFER_TYPE_HW ? OMXHW : OMXSW;
	rc->hw_usage   = p->hw_usage;
	rc->min_available_buffer = p->port_out.omx_def.nBufferCountMin;
	return 0;
}

static int video_dec_need_realloc( STREAM_DEC_VIDEO *dec, VIDEO_PROPERTIES *video, int *num_frames)
{
	priv_t *p = (priv_t*)dec->priv;

	video->padded_width  = dec->video->padded_width;
	video->padded_height = dec->video->padded_height;
	if (num_frames)
		*num_frames = p->port_out.nb_omx_buffers_max;

	return p->realloc;
}

static int video_dec_destroy( STREAM_DEC_VIDEO *dec )
{
	if (dec) {
		afree(dec->priv);
		afree(dec);
	}
	return 0;
} 

static STREAM_DEC_VIDEO *new_dec_common( int buf_type )
{ 
	STREAM_DEC_VIDEO *dec = (STREAM_DEC_VIDEO *)amalloc( sizeof( STREAM_DEC_VIDEO ) );
	
	if( !dec )
		return NULL;
	memset( dec, 0, sizeof( STREAM_DEC_VIDEO ) );

	static char name[] = "OMXID(hw)";
	static char name_sw[] = "OMXID(sw)";
	dec->name    = buf_type == BUFFER_TYPE_HW ? name : name_sw;
	dec->destroy = video_dec_destroy;
	dec->open    = video_dec_open;
	dec->prepare = video_dec_prepare;
	dec->close   = video_dec_close;
	dec->decode  = NULL;
	dec->decode2 = NULL;
	dec->dec_in  = video_dec_in;
	dec->put_out = video_put_out;
	dec->get_out = video_get_out;
	dec->flush   = video_dec_flush;
	dec->get_rc  = video_dec_get_rc;
	dec->need_realloc  = video_dec_need_realloc;
	dec->async = 1;

	if( !(dec->priv = acalloc( 1, sizeof( priv_t ) ) ) ) {
serprintf("OMX: cannot alloc context\n");
		afree(dec);
		return NULL;
	} else {
		priv_t *p = (priv_t*)dec->priv;
		p->buf_type = buf_type;
	}

	return dec;
}

static STREAM_DEC_VIDEO *new_dec( void )
{
	return new_dec_common( BUFFER_TYPE_HW );
}

static STREAM_DEC_VIDEO *new_dec_sw( void )
{
	return new_dec_common( BUFFER_TYPE_SW );
}

// if SFDEC is used, don't even try OMX with sw buffers
#define OMX_REGISTER( format, mangler ) \
STREAM_REGISTER_DEC_VIDEO( format, 0, MAXW, MAXH, OMXHW, new_dec, "OMX(hw)", mangler );
//STREAM_REGISTER_DEC_VIDEO( format, 0, MAXW, MAXH, OMXSW, new_dec_sw, "OMX(sw)", mangler );

#ifdef CONFIG_OMX_MPEG2
OMX_REGISTER( VIDEO_FORMAT_MPEG, &stream_video_mangler_MPEG2 );
#endif
#ifdef CONFIG_OMX_MPEG4
OMX_REGISTER( VIDEO_FORMAT_MPG4, NULL );
#endif
#ifdef CONFIG_OMX_H264
OMX_REGISTER( VIDEO_FORMAT_H264, &stream_video_mangler_H264 );
#endif
#ifdef CONFIG_OMX_HEVC
OMX_REGISTER( VIDEO_FORMAT_HEVC, NULL );
#endif
#ifdef CONFIG_OMX_WMV
OMX_REGISTER( VIDEO_FORMAT_WMV3, NULL );
OMX_REGISTER( VIDEO_FORMAT_VC1, NULL );
#endif
#ifdef CONFIG_OMX_RV3040
OMX_REGISTER( VIDEO_FORMAT_RV40, NULL );
OMX_REGISTER( VIDEO_FORMAT_RV30, NULL );
#endif
#ifdef CONFIG_OMX_VP6
OMX_REGISTER( VIDEO_FORMAT_VP6, NULL );
#endif
#ifdef CONFIG_OMX_VP7
OMX_REGISTER( VIDEO_FORMAT_VP7, NULL );
#endif

#ifdef DEBUG_MSG
static STREAM_REG_DEC_VIDEO reg_mpeg2_hw = { VIDEO_FORMAT_MPEG, 0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", &stream_video_mangler_MPEG2 };
static STREAM_REG_DEC_VIDEO reg_mpeg4_hw = { VIDEO_FORMAT_MPG4, 0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", NULL };
static STREAM_REG_DEC_VIDEO reg_h264_hw  = { VIDEO_FORMAT_H264, 0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", &stream_video_mangler_H264 };
static STREAM_REG_DEC_VIDEO reg_hevc_hw  = { VIDEO_FORMAT_HEVC, 0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", NULL };
static STREAM_REG_DEC_VIDEO reg_wmv3_hw  = { VIDEO_FORMAT_WMV3, 0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", NULL };
static STREAM_REG_DEC_VIDEO reg_vc1_hw   = { VIDEO_FORMAT_VC1,  0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", NULL };
static STREAM_REG_DEC_VIDEO reg_rv4_hw   = { VIDEO_FORMAT_RV40, 0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", NULL };
static STREAM_REG_DEC_VIDEO reg_rv3_hw   = { VIDEO_FORMAT_RV30, 0, MAXW, MAXH, 0, DSP3, new_dec, "OMX(hw)", NULL };

static STREAM_REG_DEC_VIDEO reg_mpeg2_sw = { VIDEO_FORMAT_MPEG, 0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", &stream_video_mangler_MPEG2 };
static STREAM_REG_DEC_VIDEO reg_mpeg4_sw = { VIDEO_FORMAT_MPG4, 0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", NULL };
static STREAM_REG_DEC_VIDEO reg_h264_sw  = { VIDEO_FORMAT_H264, 0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", &stream_video_mangler_H264 };
static STREAM_REG_DEC_VIDEO reg_hevc_sw  = { VIDEO_FORMAT_HEVC, 0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", NULL };
static STREAM_REG_DEC_VIDEO reg_wmv3_sw  = { VIDEO_FORMAT_WMV3, 0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", NULL };
static STREAM_REG_DEC_VIDEO reg_vc1_sw   = { VIDEO_FORMAT_VC1,  0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", NULL };
static STREAM_REG_DEC_VIDEO reg_rv4_sw   = { VIDEO_FORMAT_RV40, 0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", NULL };
static STREAM_REG_DEC_VIDEO reg_rv3_sw   = { VIDEO_FORMAT_RV30, 0, MAXW, MAXH, 0, DSP2, new_dec_sw, "OMX(sw)", NULL };

static void _reg_omx( void ) 
{
serprintf("register OMX for VIDEO_FORMAT_MPEG\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MPEG );
	stream_register_dec_video( &reg_mpeg2_hw );
	stream_register_dec_video( &reg_mpeg2_sw );
serprintf("register OMX for VIDEO_FORMAT_MPG4\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_MPG4 );
	stream_register_dec_video( &reg_mpeg4_hw );
	stream_register_dec_video( &reg_mpeg4_sw );
serprintf("register OMX for VIDEO_FORMAT_H264\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_H264 );
	stream_register_dec_video( &reg_h264_hw );
	stream_register_dec_video( &reg_h264_sw );
serprintf("register OMX for VIDEO_FORMAT_HEVC\r\n");
        stream_unregister_dec_video( VIDEO_FORMAT_HEVC );
        stream_register_dec_video( &reg_hevc_hw );
        stream_register_dec_video( &reg_hevc_sw );
serprintf("register OMX for VIDEO_FORMAT_WMV3\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_WMV3 );
	stream_register_dec_video( &reg_wmv3_hw );
	stream_register_dec_video( &reg_wmv3_sw );
serprintf("register OMX for VIDEO_FORMAT_VC1\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_VC1 );
	stream_register_dec_video( &reg_vc1_hw );
	stream_register_dec_video( &reg_vc1_sw );
serprintf("register OMX for VIDEO_FORMAT_RV40\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_RV40 );
	stream_register_dec_video( &reg_rv4_hw );
	stream_register_dec_video( &reg_rv4_sw );
serprintf("register OMX for VIDEO_FORMAT_RV30\r\n");
	stream_unregister_dec_video( VIDEO_FORMAT_RV30 );
	stream_register_dec_video( &reg_rv3_hw );
	stream_register_dec_video( &reg_rv3_sw );
}

DECLARE_DEBUG_COMMAND_VOID( "regomx", 	_reg_omx );
#endif

#endif
