#ifdef __aarch64__
/*
 * Copyright 2017 phh
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

#include <onnxruntime/onnxruntime_c_api.h>
#include <fftw3.h>
#include "global.h"
#include "stream_filter_audio.h"
#include "debug.h"
#include "atime.h"
#include "compress.h"
#include "astdlib.h"
#include "util.h"
#include <math.h>
#include <string.h>

// Models parameters
const int fft_size = 2048;
const int chunk_size = 1024;
const int fft_n_entries = 1 + fft_size / 2; // Symetric + DC
const int fft_n_entries_sym = fft_size / 2 - 1;
const int n_chunks = fft_size / chunk_size;
const int n_channels = 2;

static const OrtApi* g_ort;
// Originally ORT_ABORT_ON_ERROR
#define O(expr)                             \
  do {                                                       \
    OrtStatus* onnx_status = (expr);                         \
    if (onnx_status != NULL) {                               \
      const char* msg = g_ort->GetErrorMessage(onnx_status); \
      fprintf(stderr, "%s\n", msg);                          \
      g_ort->ReleaseStatus(onnx_status);                     \
      abort();                                               \
    }                                                        \
  } while (0);


struct ctx {
	OrtEnv *ortEnv;
	OrtSession *ortSession;
	OrtSessionOptions *ortSessionOptions;
	fftw_complex *fft_time;
	fftw_complex *fft_freq;
	fftw_plan fft_plan_fwd;
	fftw_plan fft_plan_inv;
	int level;
	int nightmode;
	OrtValue *state_0;
	OrtValue *state_out_0;
	OrtValue *x_0;
	OrtValue *y_0;
	OrtIoBinding *io_binding;

	float *fft_window;
	float *current_chunk;
	float *overlap;
	float *in_buffer;
	int in_buffer_pos;
	float *out_buffer;
	int out_buffer_pos;
};

static void process_chunk(STREAM_FILTER_AUDIO *f);

static int _delete( STREAM_FILTER_AUDIO *f )
{
serprintf("facomp: delete\n" );
	if( f && f->priv ) {
		struct ctx *ctx = f->priv;

serprintf("%s %d\n", __FILE__, __LINE__);
		fftw_free(ctx->fft_time);
serprintf("%s %d\n", __FILE__, __LINE__);
		fftw_free(ctx->fft_freq);
serprintf("%s %d\n", __FILE__, __LINE__);
		fftw_destroy_plan(ctx->fft_plan_fwd);
serprintf("%s %d\n", __FILE__, __LINE__);
		fftw_destroy_plan(ctx->fft_plan_inv);
serprintf("%s %d\n", __FILE__, __LINE__);
		afree(ctx->fft_window);
serprintf("%s %d\n", __FILE__, __LINE__);
		afree(ctx->current_chunk);
serprintf("%s %d\n", __FILE__, __LINE__);
		afree(ctx->overlap);
serprintf("%s %d\n", __FILE__, __LINE__);
		afree(ctx->in_buffer);
serprintf("%s %d\n", __FILE__, __LINE__);
		afree(ctx->out_buffer);
serprintf("%s %d\n", __FILE__, __LINE__);

serprintf("%s %d\n", __FILE__, __LINE__);
		if(ctx->io_binding) g_ort->ReleaseIoBinding(ctx->io_binding);
serprintf("%s %d\n", __FILE__, __LINE__);
		g_ort->ReleaseSessionOptions(ctx->ortSessionOptions);
serprintf("%s %d\n", __FILE__, __LINE__);
		g_ort->ReleaseSession(ctx->ortSession);
serprintf("%s %d\n", __FILE__, __LINE__);
		g_ort->ReleaseEnv(ctx->ortEnv);
serprintf("%s %d\n", __FILE__, __LINE__);
		if(ctx->state_0) g_ort->ReleaseValue(ctx->state_0);
serprintf("%s %d\n", __FILE__, __LINE__);
		if(ctx->state_out_0) g_ort->ReleaseValue(ctx->state_out_0);
serprintf("%s %d\n", __FILE__, __LINE__);
		if(ctx->x_0) g_ort->ReleaseValue(ctx->x_0);
serprintf("%s %d\n", __FILE__, __LINE__);
		if(ctx->y_0) g_ort->ReleaseValue(ctx->y_0);
serprintf("%s %d\n", __FILE__, __LINE__);
	}
	afree(f);
	return 0;
}

static int _open( STREAM_FILTER_AUDIO *f, AUDIO_PROPERTIES *audio )
{
serprintf("facomp: open\n" );
	struct ctx *ctx = acalloc( 1, sizeof( struct ctx ) );

	ctx->fft_time = fftw_malloc(sizeof(fftw_complex) * fft_size);
	ctx->fft_freq = fftw_malloc(sizeof(fftw_complex) * fft_size);
	ctx->fft_plan_fwd = fftw_plan_dft_1d(fft_size, ctx->fft_time, ctx->fft_freq, FFTW_FORWARD, FFTW_ESTIMATE);
	ctx->fft_plan_inv = fftw_plan_dft_1d(fft_size, ctx->fft_freq, ctx->fft_time, FFTW_BACKWARD, FFTW_ESTIMATE);

	ctx->fft_window = amalloc(sizeof(float) * fft_size);
	for (int i = 0; i < fft_size; i++) {
		ctx->fft_window[i] = 0.5 * (1 - cos(2 * M_PI * i / fft_size));
	}

	ctx->current_chunk = acalloc(n_channels * fft_size, sizeof(float));
	ctx->overlap = acalloc(n_channels * n_chunks * fft_size, sizeof(float));
	ctx->in_buffer = acalloc(n_channels * chunk_size, sizeof(float));
	ctx->in_buffer_pos = 0;
	ctx->out_buffer = acalloc(n_channels * chunk_size, sizeof(float));
	ctx->out_buffer_pos = 0;


	O(g_ort->CreateEnv(ORT_LOGGING_LEVEL_INFO, "test", &ctx->ortEnv));
	O(g_ort->CreateSessionOptions(&ctx->ortSessionOptions));
	O(g_ort->SetIntraOpNumThreads(ctx->ortSessionOptions, 1));
	O(g_ort->AddSessionConfigEntry(ctx->ortSessionOptions, "session.allow_spinning", "0"));
	O(g_ort->CreateSession(ctx->ortEnv, "/sdcard/Android/data/org.courville.nova/model.onnx", ctx->ortSessionOptions, &ctx->ortSession));
	OrtMemoryInfo* memory_info;
	O(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info));
	OrtAllocator *allocator;
	O(g_ort->CreateAllocator(ctx->ortSession, memory_info, &allocator));

	size_t inputCount;
	O(g_ort->SessionGetInputCount(ctx->ortSession, &inputCount));
	for(int i=0; i< inputCount; i++) {
		char *name = NULL;
		OrtTypeInfo *type_info = NULL;
		enum ONNXType type;

		O(g_ort->SessionGetInputName(ctx->ortSession, i, allocator, &name));
		serprintf("input %d name %s\n", i, name);
		O(g_ort->SessionGetInputTypeInfo(ctx->ortSession, i, &type_info));
		O(g_ort->GetOnnxTypeFromTypeInfo(type_info, &type));
		if (type == ONNX_TYPE_TENSOR) {
			const OrtTensorTypeAndShapeInfo *shape_info = NULL;
			O(g_ort->CastTypeInfoToTensorInfo(type_info, &shape_info));
			size_t ndims = 0;
			O(g_ort->GetDimensionsCount(shape_info, &ndims));
			int64_t dims[ndims];
			O(g_ort->GetDimensions(shape_info, dims, ndims));

			OrtValue **val = NULL;
			if(strcmp(name, "state.0") == 0) val = &ctx->state_0;
			if(strcmp(name, "x.0") == 0) val = &ctx->x_0;

			if(!val) continue;

			O(g_ort->CreateTensorAsOrtValue(allocator, dims, ndims, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, val));
			void* tensor_data = NULL;
			O(g_ort->GetTensorMutableData(*val, &tensor_data));
			size_t tensor_size = 1;
			for(size_t j=0; j<ndims; j++) {
				tensor_size *= dims[j];
			}
			if(tensor_data)
				memset(tensor_data, 0, tensor_size * sizeof(float));
		}
		g_ort->ReleaseTypeInfo(type_info);
	}

	size_t outputCount;
	O(g_ort->SessionGetOutputCount(ctx->ortSession, &outputCount));
	for(int i=0; i< outputCount; i++) {
		char *name = NULL;
		OrtTypeInfo *type_info = NULL;
		enum ONNXType type;

		O(g_ort->SessionGetOutputName(ctx->ortSession, i, allocator, &name));
		serprintf("output %d name %s\n", i, name);
		O(g_ort->SessionGetOutputTypeInfo(ctx->ortSession, i, &type_info));
		O(g_ort->GetOnnxTypeFromTypeInfo(type_info, &type));
		if (type == ONNX_TYPE_TENSOR) {
			const OrtTensorTypeAndShapeInfo *shape_info = NULL;
			O(g_ort->CastTypeInfoToTensorInfo(type_info, &shape_info));
			size_t ndims = 0;
			O(g_ort->GetDimensionsCount(shape_info, &ndims));
			int64_t dims[ndims];
			O(g_ort->GetDimensions(shape_info, dims, ndims));

			OrtValue **val = NULL;
			if(strcmp(name, "new_state.0") == 0) val = &ctx->state_out_0;
			if(strcmp(name, "y.0") == 0) val = &ctx->y_0;

			if(!val) continue;

			O(g_ort->CreateTensorAsOrtValue(allocator, dims, ndims, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, val));
			void* tensor_data = NULL;
			O(g_ort->GetTensorMutableData(*val, &tensor_data));
			size_t tensor_size = 1;
			for(size_t j=0; j<ndims; j++) {
				tensor_size *= dims[j];
			}
			if(tensor_data)
				memset(tensor_data, 0, tensor_size * sizeof(float));

		}
		g_ort->ReleaseTypeInfo(type_info);
	}
	g_ort->ReleaseAllocator(allocator);
	g_ort->ReleaseMemoryInfo(memory_info);

	O(g_ort->CreateIoBinding(ctx->ortSession, &ctx->io_binding));
	O(g_ort->BindInput(ctx->io_binding, "x.0", ctx->x_0));
	O(g_ort->BindInput(ctx->io_binding, "state.0", ctx->state_0));
	O(g_ort->BindOutput(ctx->io_binding, "y.0", ctx->y_0));
	O(g_ort->BindOutput(ctx->io_binding, "new_state.0", ctx->state_out_0));

	f->priv = ctx;

	return 0;
}

static int _close( STREAM_FILTER_AUDIO *f ) 
{
serprintf("facomp: close\n" );
	return 0;
}

static void process_chunk(STREAM_FILTER_AUDIO *f) {
    struct ctx *ctx = f->priv;

    // 1. Update current_chunk
    for(int c=0; c<n_channels; c++) {
	// Move data to older portion of current_chunk
        memmove(ctx->current_chunk + c * fft_size, ctx->current_chunk + c * fft_size + chunk_size, sizeof(float) * (fft_size - chunk_size));
    }

    for(int c=0; c<n_channels; c++) {
	// Copy input to end of current_chunk
        memmove(ctx->current_chunk + c * fft_size + (fft_size - chunk_size), ctx->in_buffer + c * chunk_size, sizeof(float) * chunk_size);
    }
    // 2. FFT for each channel
    float *tensor_in_data;
    O(g_ort->GetTensorMutableData(ctx->x_0, (void**)&tensor_in_data));
    for(int c=0; c<n_channels; c++) {
        for (int i = 0; i < fft_size; i++) {
            ctx->fft_time[i][0] = ctx->current_chunk[c * fft_size + i] * ctx->fft_window[i];
            ctx->fft_time[i][1] = 0;
        }
        fftw_execute(ctx->fft_plan_fwd);

        for (int i = 0; i < fft_n_entries; i++) {
            tensor_in_data[c * fft_n_entries * 2 + i * 2    ] = ctx->fft_freq[i][0];
            tensor_in_data[c * fft_n_entries * 2 + i * 2 + 1] = ctx->fft_freq[i][1];
        }
    }

    // 3. ONNX Inference
    O(g_ort->RunWithBinding(ctx->ortSession, NULL, ctx->io_binding));

    // 4. IFFT for each channel
    float *tensor_out_data;
    O(g_ort->GetTensorMutableData(ctx->y_0, (void**)&tensor_out_data));
    for(int c=0; c<n_channels; c++) {
        float mix = 0.0;//ctx->level / 100.0f;
	if (ctx->level == 3) {
		if (ctx->nightmode)
			mix = 0.8;
		else
			mix = 1.0;
	} else if(ctx->nightmode) {
		mix = -1.0;
	}

	if (mix > 0) {
		for (int i = 0; i < fft_n_entries; i++) {
		    ctx->fft_freq[i][0] = mix * tensor_out_data[c * fft_n_entries * 2 + i * 2    ] + (1.0 - mix) * tensor_in_data[c * fft_n_entries * 2 + i * 2    ];
		    ctx->fft_freq[i][1] = mix * tensor_out_data[c * fft_n_entries * 2 + i * 2 + 1] + (1.0 - mix) * tensor_in_data[c * fft_n_entries * 2 + i * 2 + 1];
		}
	} else {
		for (int i = 0; i < fft_n_entries; i++) {
		    ctx->fft_freq[i][0] = mix * tensor_out_data[c * fft_n_entries * 2 + i * 2    ] + tensor_in_data[c * fft_n_entries * 2 + i * 2    ];
		    ctx->fft_freq[i][1] = mix * tensor_out_data[c * fft_n_entries * 2 + i * 2 + 1] + tensor_in_data[c * fft_n_entries * 2 + i * 2 + 1];
		}
	}

        for (int i = 0; i < fft_n_entries_sym; i++) {
            ctx->fft_freq[fft_n_entries + i][0] = ctx->fft_freq[fft_n_entries_sym - i][0];
            ctx->fft_freq[fft_n_entries + i][1] = -ctx->fft_freq[fft_n_entries_sym - i][1];
        }

        fftw_execute(ctx->fft_plan_inv);

        // Shift previous overlap data
        float *channel_overlap_base = &ctx->overlap[c * n_chunks * fft_size];
        memmove(channel_overlap_base, channel_overlap_base + fft_size, (n_chunks - 1) * fft_size * sizeof(float));

        // Add new IFFT result to the last overlap slot
        float *new_overlap_slot = channel_overlap_base + (n_chunks - 1) * fft_size;
        for (int i = 0; i < fft_size; i++) {
            new_overlap_slot[i] = ctx->fft_time[i][0] / fft_size;
        }

        // Perform overlap-add
        for (int i = 0; i < chunk_size; i++) {
            float sum = 0;
            float window_sum = 0;
            for (int j = 0; j < n_chunks; j++) {
                int pos = i + (n_chunks - 1 - j) * chunk_size;
                sum += channel_overlap_base[j * fft_size + pos];
                window_sum += ctx->fft_window[pos];
            }
            if(window_sum > 1e-4)
                ctx->out_buffer[c * chunk_size + i] = sum / window_sum;
            else
                ctx->out_buffer[c * chunk_size + i] = 0;
        }
    }

    // 5. Update state
    void *state_in_data, *state_out_data;
    O(g_ort->GetTensorMutableData(ctx->state_0, &state_in_data));
    O(g_ort->GetTensorMutableData(ctx->state_out_0, &state_out_data));

    OrtTensorTypeAndShapeInfo *info;
    O(g_ort->GetTensorTypeAndShape(ctx->state_0, &info));
    size_t size;
    O(g_ort->GetTensorShapeElementCount(info, &size));
    g_ort->ReleaseTensorTypeAndShapeInfo(info);

    memcpy(state_in_data, state_out_data, size * sizeof(float));

    ctx->in_buffer_pos = 0;
    ctx->out_buffer_pos = 0;
}

static int _filter( STREAM_FILTER_AUDIO *f, AUDIO_FRAME *frame )
{
	struct ctx *ctx = f->priv;
	short *data = (short*)frame->data;
	int samples = frame->size / 4;

	int current_sample = 0;
	while(current_sample < samples) {
		int out_available = chunk_size - ctx->out_buffer_pos;
		int to_copy = min(out_available, samples - current_sample);

		// De-interleave and convert to float
		for(int i=0; i<to_copy; i++) {
			ctx->in_buffer[             ctx->in_buffer_pos + i] = data[(current_sample + i) * 2    ] / 32768.0f;
			ctx->in_buffer[chunk_size + ctx->in_buffer_pos + i] = data[(current_sample + i) * 2 + 1] / 32768.0f;
		}
		ctx->in_buffer_pos += to_copy;

		//memcpy(ctx->out_buffer,                              ctx->in_buffer, n_channels * chunk_size * sizeof(float));
		//memcpy(ctx->out_buffer,                              ctx->in_buffer,              chunk_size * sizeof(float));
		//memcpy(ctx->out_buffer + chunk_size, ctx->in_buffer,              chunk_size * sizeof(float));

		//Copy 1st channel to 2nd
		memcpy(ctx->out_buffer + chunk_size, ctx->out_buffer,              chunk_size * sizeof(float));

		// Interleave and convert to short
		for(int i=0; i<to_copy; i++) {
			float left = ctx->out_buffer[ctx->out_buffer_pos + i];
			float right = ctx->out_buffer[chunk_size + ctx->out_buffer_pos + i];
			data[(current_sample + i) * 2    ] = (short)(fmaxf(-32767.0f, fminf(32767.0f, left * 32768.0f)));
			data[(current_sample + i) * 2 + 1] = (short)(fmaxf(-32767.0f, fminf(32767.0f, right * 32768.0f)));
		}
		ctx->out_buffer_pos += to_copy;
		current_sample += to_copy;

		if(ctx->out_buffer_pos != ctx->in_buffer_pos) {
			serprintf("Uhhh, buffer fail %d %d\n", ctx->out_buffer_pos, ctx->in_buffer_pos);
		}

		if(ctx->in_buffer_pos == chunk_size) {
			process_chunk(f);
		}
	}
	return 0;
}

static int _flush( STREAM_FILTER_AUDIO *f )
{
//serprintf("facomp: flush\n" );
	return 0;
}

static int _set_param( STREAM_FILTER_AUDIO *f, void *params, void *night_on )
{
	int *level = params;
	int *nightmode = night_on;
	struct ctx *ctx = f->priv;
serprintf("facomp: level %d\n", *level );
	if( (ctx->level != *level ) || (ctx->nightmode != *nightmode)) {
		ctx->level = *level;
		ctx->nightmode = *nightmode;
	}
	return 0;
}

static int _delay( STREAM_FILTER_AUDIO *f )
{
	return chunk_size;
}

STREAM_FILTER_AUDIO *stream_filter_audio_onnx_new( void ) 
{
	STREAM_FILTER_AUDIO *f = acalloc( 1, sizeof( STREAM_FILTER_AUDIO ) );
	
	if( !f )
		return NULL;

	static char name[] = "compress";
	f->name    = name;
	f->delete  = _delete;
	f->open    = _open;
	f->close   = _close;
	f->filter  = _filter;
	f->flush   = _flush;
	f->set_param = _set_param;
	f->delay   = _delay;

	if (!g_ort)
		g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);

	return f;
}

#endif
