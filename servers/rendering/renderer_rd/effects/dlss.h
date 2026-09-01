/**************************************************************************/
/*  dlss.h                                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/shaders/effects/motion_vector_decode.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

class DLSSEffect;

// Per-viewport DLSS state. The concrete type is private to dlss.cpp, as it holds
// Streamline types that must not leak into the rest of the renderer.
class DLSSContext {
public:
	struct Parameters {
		DLSSContext *context = nullptr;
		Size2i internal_size;
		RID color;
		RID depth;
		RID velocity;
		RID reactive;
		RID exposure;
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		bool reverse_depth = true;
		Vector2 jitter;
		float delta_time = 0.0f;
		float sharpness = 0.0f;
		char preset = '?';
		bool reset_accumulation = false;
		// Reprojection in the [-1,1] depth convention, used to derive motion vectors for
		// pixels the engine left marked invalid. Matches what FSR2 is given.
		Projection reprojection;
		// The two matrices below live in the clip space the scene was actually rendered in
		// (Y flipped, reverse Z, depth remapped to [0,1]), so that they agree with the depth
		// buffer handed to DLSS and with the depthInverted flag. Both are jitter-free.
		Projection cam_projection;
		Projection clip_to_prev_clip;
		Transform3D cam_transform;
		bool dlss_g = false;
	} last_parameters;

	DLSSEffect *last_effect = nullptr;
	bool is_d3d12 = false;
	int delay = 4; // Warmup frames before DLSS evaluates (Vulkan stability workaround).

	virtual ~DLSSContext() {}
};

class DLSSEffect {
public:
	struct Shaders {
		MotionVectorDecodeShaderRD mvec_decode_shader;
		RID mvec_decode_version;
		RID mvec_decode_pipeline;
	} shaders;

	DLSSEffect();
	~DLSSEffect();

	DLSSContext *create_context(Size2i p_internal_size, Size2i p_target_size);
	void upscale(const DLSSContext::Parameters &p_params);

	// False when DLSS can never run for this configuration: no quality mode covers the
	// requested render resolution, or the adapter/plugins do not support DLSS. Callers
	// must fall back to another upscaler rather than leave the jitter unresolved.
	bool is_context_valid(DLSSContext *p_context);

	// False while the context is still warming up or DLSS is unavailable; the
	// upscaled texture must not be consumed until this returns true.
	bool is_ready(DLSSContext *p_context);

private:
	void _upscale_internal(RDD::CommandBufferID p_cmd_buffer, const DLSSContext::Parameters &p_params);
	static void _upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
};

} // namespace RendererRD
