/*
 * Copyright 2026 Courville Software
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

#ifndef VIDEO_DOVI_RESHAPE_GLSL_H
#define VIDEO_DOVI_RESHAPE_GLSL_H

/* GLES 3.0 fragment shaders: Dolby Vision single-layer reshaping of the
 * decoder output to BT.2020 PQ RGB. Uniforms mirror VIDEO_DOVI_RESHAPE;
 * matrices are uploaded row-major with transpose = GL_TRUE.
 *   u_tex          decoder output buffer
 *   u_code_scale   sampled value to code / 2^bl_bit_depth
 *   u_sample_bias  per-component bias of the sampler, same units
 *   u_sdr_scale    0: BT.2020 PQ output. Otherwise SDR output for displays
 *                  without HDR: linear light times u_sdr_scale (10000 nits /
 *                  reference white), luminance tone mapped up to u_sdr_peak
 *                  (source peak / reference white), BT.709, gamma 2.2
 * video_dovi_reshape_fs samples raw YUV (GL_EXT_YUV_target; P010 gives
 * u_code_scale = 65535 / 64 / 1024). video_dovi_reshape_rgb_fs is for GPUs
 * without it: the driver converts YUV to RGB with the matrix implied by the
 * buffer (BT.601 full range for Profile 5, unclamped on NVIDIA Tegra) and
 * u_rgb_to_ycc undoes it; the result is code / 1024. */
#define VIDEO_DOVI_RESHAPE_FS_HEAD \
	"#version 300 es\n"
#define VIDEO_DOVI_RESHAPE_FS_YUV \
	"#extension GL_EXT_YUV_target : require\n" \
	"precision highp float;\n" \
	"precision highp int;\n" \
	"uniform __samplerExternal2DY2YEXT u_tex;\n" \
	"vec3 dovi_sample(vec2 uv) { return texture(u_tex, uv).xyz; }\n"
#define VIDEO_DOVI_RESHAPE_FS_RGB \
	"#extension GL_OES_EGL_image_external_essl3 : require\n" \
	"precision highp float;\n" \
	"precision highp int;\n" \
	"uniform samplerExternalOES u_tex;\n" \
	"uniform mat3 u_rgb_to_ycc;\n" \
	"vec3 dovi_sample(vec2 uv) {\n" \
	"	return u_rgb_to_ycc * texture(u_tex, uv).rgb + vec3(0.0, 0.5, 0.5);\n" \
	"}\n"
#define VIDEO_DOVI_RESHAPE_FS_BODY \
	"uniform float u_code_scale;\n" \
	"uniform vec3 u_sample_bias;\n" \
	"uniform vec3 u_pivot_lo;\n" \
	"uniform vec3 u_pivot_hi;\n" \
	"uniform float u_pivots[21];\n" \
	"uniform vec4 u_coeffs[24];\n" \
	"uniform vec4 u_mmr[144];\n" \
	"uniform mat3 u_ycc_to_rgb;\n" \
	"uniform vec3 u_ycc_offset;\n" \
	"uniform mat3 u_lms_to_rgb;\n" \
	"uniform float u_sdr_scale;\n" \
	"uniform float u_sdr_peak;\n" \
	"in vec2 v_uv;\n" \
	"out vec4 o_color;\n" \
	"const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;\n" \
	"const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;\n" \
	"vec3 pq_eotf(vec3 e) {\n" \
	"	e = pow(clamp(e, 0.0, 1.0), vec3(1.0 / PQ_M2));\n" \
	"	return pow(max(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e), vec3(1.0 / PQ_M1));\n" \
	"}\n" \
	"vec3 pq_oetf(vec3 l) {\n" \
	"	l = pow(max(l, 0.0), vec3(PQ_M1));\n" \
	"	return pow((PQ_C1 + PQ_C2 * l) / (1.0 + PQ_C3 * l), vec3(PQ_M2));\n" \
	"}\n"
/* Generic reshaping: any piece count, polynomial or MMR of any order, chosen
 * per pixel. sfdec_dovi_gl generates reshape0..2 specialized to the RPU's
 * structure instead, and keeps this version as its fallback. */
#define VIDEO_DOVI_RESHAPE_FS_GENERIC \
	"float reshape(int c, vec3 sig) {\n" \
	"	float s = sig[c];\n" \
	"	int piece = 0;\n" \
	"	for (int i = 0; i < 7; i++)\n" \
	"		if (s >= u_pivots[c * 7 + i]) piece = i + 1;\n" \
	"	vec4 k = u_coeffs[c * 8 + piece];\n" \
	"	if (k.w == 0.0)\n" \
	"		return (k.z * s + k.y) * s + k.x;\n" \
	"	int row = c * 48 + int(k.y);\n" \
	"	vec4 x;\n" \
	"	x.xyz = sig.xxy * sig.yzz;\n" \
	"	x.w = x.x * sig.z;\n" \
	"	float r = k.x + dot(u_mmr[row].xyz, sig) + dot(u_mmr[row + 1], x);\n" \
	"	if (k.w >= 2.0) {\n" \
	"		vec3 s2 = sig * sig;\n" \
	"		vec4 x2 = x * x;\n" \
	"		r += dot(u_mmr[row + 2].xyz, s2) + dot(u_mmr[row + 3], x2);\n" \
	"		if (k.w >= 3.0)\n" \
	"			r += dot(u_mmr[row + 4].xyz, s2 * sig) + dot(u_mmr[row + 5], x2 * x);\n" \
	"	}\n" \
	"	return r;\n" \
	"}\n" \
	"float reshape0(vec3 sig) { return reshape(0, sig); }\n" \
	"float reshape1(vec3 sig) { return reshape(1, sig); }\n" \
	"float reshape2(vec3 sig) { return reshape(2, sig); }\n"
#define VIDEO_DOVI_RESHAPE_FS_MAIN \
	"void main() {\n" \
	"	vec3 sig = dovi_sample(v_uv) * u_code_scale - u_sample_bias;\n" \
	"	sig = clamp(sig, u_pivot_lo, u_pivot_hi);\n" \
	"	vec3 c = clamp(vec3(reshape0(sig), reshape1(sig), reshape2(sig)), 0.0, 1.0);\n" \
	"	vec3 lms = pq_eotf(u_ycc_to_rgb * (c - u_ycc_offset));\n" \
	"	vec3 rgb = u_lms_to_rgb * lms;\n" \
	"	if (u_sdr_scale == 0.0) {\n" \
	"		o_color = vec4(pq_oetf(rgb), 1.0);\n" \
	"		return;\n" \
	"	}\n" \
	"	rgb = max(rgb, 0.0) * u_sdr_scale;\n" \
	"	float y = dot(rgb, vec3(0.2627, 0.6780, 0.0593));\n" \
	"	float m = y * (1.0 + y / (u_sdr_peak * u_sdr_peak)) / (1.0 + y);\n" \
	"	rgb *= y > 0.0 ? m / y : 0.0;\n" \
	"	const mat3 BT2020_TO_BT709 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006,\n" \
	"	                                  -0.0728, -0.0083, 1.1187);\n" \
	"	o_color = vec4(pow(clamp(BT2020_TO_BT709 * rgb, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);\n" \
	"}\n"

static const char video_dovi_reshape_fs[] =
	VIDEO_DOVI_RESHAPE_FS_HEAD VIDEO_DOVI_RESHAPE_FS_YUV VIDEO_DOVI_RESHAPE_FS_BODY
	VIDEO_DOVI_RESHAPE_FS_GENERIC VIDEO_DOVI_RESHAPE_FS_MAIN;
static const char video_dovi_reshape_rgb_fs[] =
	VIDEO_DOVI_RESHAPE_FS_HEAD VIDEO_DOVI_RESHAPE_FS_RGB VIDEO_DOVI_RESHAPE_FS_BODY
	VIDEO_DOVI_RESHAPE_FS_GENERIC VIDEO_DOVI_RESHAPE_FS_MAIN;

#endif
