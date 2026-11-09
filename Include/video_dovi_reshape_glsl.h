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

/* GLES 3.0 fragment shader: Dolby Vision single-layer reshaping of raw YUV
 * (GL_EXT_YUV_target) to BT.2020 PQ RGB. Uniforms mirror VIDEO_DOVI_RESHAPE;
 * matrices are uploaded row-major with transpose = GL_TRUE.
 *   u_tex          decoder output buffer, sampled without color conversion
 *   u_code_scale   sampled value to code / 2^bl_bit_depth
 *                  (P010 through GL_EXT_YUV_target: 65535 / 64 / 1024)
 *   u_sample_bias  per-component bias of the raw sampler, same units */
static const char video_dovi_reshape_fs[] =
	"#version 300 es\n"
	"#extension GL_EXT_YUV_target : require\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform __samplerExternal2DY2YEXT u_tex;\n"
	"uniform float u_code_scale;\n"
	"uniform vec3 u_sample_bias;\n"
	"uniform vec3 u_pivot_lo;\n"
	"uniform vec3 u_pivot_hi;\n"
	"uniform float u_pivots[21];\n"
	"uniform vec4 u_coeffs[24];\n"
	"uniform vec4 u_mmr[144];\n"
	"uniform mat3 u_ycc_to_rgb;\n"
	"uniform vec3 u_ycc_offset;\n"
	"uniform mat3 u_lms_to_rgb;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_color;\n"
	"const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;\n"
	"const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;\n"
	"vec3 pq_eotf(vec3 e) {\n"
	"	e = pow(clamp(e, 0.0, 1.0), vec3(1.0 / PQ_M2));\n"
	"	return pow(max(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e), vec3(1.0 / PQ_M1));\n"
	"}\n"
	"vec3 pq_oetf(vec3 l) {\n"
	"	l = pow(max(l, 0.0), vec3(PQ_M1));\n"
	"	return pow((PQ_C1 + PQ_C2 * l) / (1.0 + PQ_C3 * l), vec3(PQ_M2));\n"
	"}\n"
	"float reshape(int c, vec3 sig) {\n"
	"	float s = sig[c];\n"
	"	int piece = 0;\n"
	"	for (int i = 0; i < 7; i++)\n"
	"		if (s >= u_pivots[c * 7 + i]) piece = i + 1;\n"
	"	vec4 k = u_coeffs[c * 8 + piece];\n"
	"	if (k.w == 0.0)\n"
	"		return (k.z * s + k.y) * s + k.x;\n"
	"	int row = c * 48 + int(k.y);\n"
	"	vec4 x;\n"
	"	x.xyz = sig.xxy * sig.yzz;\n"
	"	x.w = x.x * sig.z;\n"
	"	float r = k.x + dot(u_mmr[row].xyz, sig) + dot(u_mmr[row + 1], x);\n"
	"	if (k.w >= 2.0) {\n"
	"		vec3 s2 = sig * sig;\n"
	"		vec4 x2 = x * x;\n"
	"		r += dot(u_mmr[row + 2].xyz, s2) + dot(u_mmr[row + 3], x2);\n"
	"		if (k.w >= 3.0)\n"
	"			r += dot(u_mmr[row + 4].xyz, s2 * sig) + dot(u_mmr[row + 5], x2 * x);\n"
	"	}\n"
	"	return r;\n"
	"}\n"
	"void main() {\n"
	"	vec3 sig = texture(u_tex, v_uv).xyz * u_code_scale - u_sample_bias;\n"
	"	sig = clamp(sig, u_pivot_lo, u_pivot_hi);\n"
	"	vec3 c = clamp(vec3(reshape(0, sig), reshape(1, sig), reshape(2, sig)), 0.0, 1.0);\n"
	"	vec3 lms = pq_eotf(u_ycc_to_rgb * (c - u_ycc_offset));\n"
	"	o_color = vec4(pq_oetf(u_lms_to_rgb * lms), 1.0);\n"
	"}\n";

#endif
