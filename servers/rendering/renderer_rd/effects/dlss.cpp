/**************************************************************************/
/*  dlss.cpp                                                              */
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

#include "dlss.h"

#ifdef STREAMLINE_ENABLED

#include "core/os/os.h"
#include "drivers/streamline/streamline_context.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

// Texture layout/state constants (avoid including Vulkan/D3D12 headers here).
// These must match the layout the render graph leaves each resource in, which is
// derived from the CallbackResource usage declared in upscale():
// TEXTURE_SAMPLE -> SHADER_READ_ONLY_OPTIMAL, STORAGE_IMAGE_READ_WRITE -> GENERAL.
static constexpr uint64_t DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY = 5;
static constexpr uint64_t DLSS_VK_IMAGE_LAYOUT_GENERAL = 1;
static constexpr uint64_t DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR = 0x40;
static constexpr uint64_t DLSS_D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 0x8;
static constexpr float DLSS_OPTIMAL_MODE_MAX_DISTANCE = 1000000.0f;

namespace RendererRD {

class DLSSContextInner : public DLSSContext {
public:
	sl::ViewportHandle viewport;
	sl::Constants constants;
	sl::DLSSOptions current_dlss_options;
	sl::DLSSOptimalSettings current_optimal_settings;
	sl::DLSSDOptions current_dlss_d_options; // DLSS Ray Reconstruction.

	DLSSContextInner();
	virtual ~DLSSContextInner();

	// Picks the DLSS quality mode whose optimal render resolution is closest to the
	// resolution the viewport is already configured for, so that the 3D scaling set
	// on the viewport stays authoritative.
	sl::DLSSMode find_optimal_mode(uint32_t p_output_width, uint32_t p_output_height, uint32_t p_desired_width, uint32_t p_desired_height, sl::DLSSOptimalSettings &r_optimal_settings) {
		if (StreamlineContext::get().slDLSSGetOptimalSettings == nullptr) {
			return sl::DLSSMode::eOff;
		}

		const sl::DLSSMode modes[] = { sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance };
		constexpr size_t mode_count = sizeof(modes) / sizeof(modes[0]);

		sl::DLSSOptimalSettings settings[mode_count];
		bool valid_settings[mode_count];
		Vector2 distance[mode_count];
		memset(valid_settings, 0, sizeof(valid_settings));

		for (size_t i = 0; i < mode_count; i++) {
			sl::DLSSOptions dlss_options = {};
			dlss_options.outputWidth = p_output_width;
			dlss_options.outputHeight = p_output_height;
			dlss_options.mode = modes[i];

			if (StreamlineContext::get().slDLSSGetOptimalSettings(dlss_options, settings[i]) != sl::Result::eOk) {
				continue;
			}

			const sl::DLSSOptimalSettings &optimal_settings = settings[i];
			if (p_desired_width >= optimal_settings.renderWidthMin &&
					p_desired_width <= optimal_settings.renderWidthMax &&
					p_desired_height >= optimal_settings.renderHeightMin &&
					p_desired_height <= optimal_settings.renderHeightMax) {
				valid_settings[i] = true;
				distance[i] = Vector2(Math::abs((float)optimal_settings.optimalRenderWidth - (float)p_desired_width), Math::abs((float)optimal_settings.optimalRenderHeight - (float)p_desired_height));
			}
		}

		// Now select the closest match.
		Vector2 closest_distance(DLSS_OPTIMAL_MODE_MAX_DISTANCE, DLSS_OPTIMAL_MODE_MAX_DISTANCE);
		int closest_distance_match = -1;
		for (size_t i = 0; i < mode_count; i++) {
			if (valid_settings[i] && distance[i].length_squared() < closest_distance.length_squared()) {
				closest_distance_match = i;
				closest_distance = distance[i];
			}
		}

		if (closest_distance_match != -1) {
			r_optimal_settings = settings[closest_distance_match];
			return modes[closest_distance_match];
		}

		// Every DLSS quality mode only accepts render resolutions inside its own range, and
		// those ranges do not cover arbitrarily small scaling factors. Report the ranges so
		// the usable range is discoverable, and let the caller fall back to another upscaler.
		if (OS::get_singleton()->is_stdout_verbose()) {
			for (size_t i = 0; i < mode_count; i++) {
				print_line(vformat("DLSS mode %d accepts render resolutions from %dx%d to %dx%d.", int(modes[i]),
						settings[i].renderWidthMin, settings[i].renderHeightMin, settings[i].renderWidthMax, settings[i].renderHeightMax));
			}
		}
		WARN_PRINT_ONCE(vformat("No DLSS quality mode covers a render resolution of %dx%d for a %dx%d output; the 3D scaling factor is outside the range DLSS supports. Falling back to FSR2. Run with --verbose to see the supported ranges.",
				p_desired_width, p_desired_height, p_output_width, p_output_height));
		return sl::DLSSMode::eOff;
	}
};

} // namespace RendererRD

static Vector<unsigned int> g_dlss_free_viewport_indices;
static unsigned int g_dlss_viewport_index = 1;

DLSSContextInner::DLSSContextInner() {
	if (g_dlss_free_viewport_indices.is_empty()) {
		g_dlss_free_viewport_indices.push_back(g_dlss_viewport_index++);
	}
	viewport = g_dlss_free_viewport_indices[g_dlss_free_viewport_indices.size() - 1];
	g_dlss_free_viewport_indices.remove_at(g_dlss_free_viewport_indices.size() - 1);
}

DLSSContextInner::~DLSSContextInner() {
	g_dlss_free_viewport_indices.push_back((unsigned int)viewport);
}

DLSSEffect::DLSSEffect() {
	Vector<String> modes;
	modes.push_back("\n");
	shaders.mvec_decode_shader.initialize(modes, "");
	shaders.mvec_decode_version = shaders.mvec_decode_shader.version_create();
	shaders.mvec_decode_pipeline = RD::get_singleton()->compute_pipeline_create(shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0));
}

DLSSEffect::~DLSSEffect() {
	shaders.mvec_decode_shader.version_free(shaders.mvec_decode_version);
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	DLSSContextInner *context = memnew(RendererRD::DLSSContextInner);

	context->current_dlss_options.mode = context->find_optimal_mode(p_target_size.width, p_target_size.height, p_internal_size.width, p_internal_size.height, context->current_optimal_settings);
	context->current_dlss_options.outputWidth = p_target_size.width;
	context->current_dlss_options.outputHeight = p_target_size.height;

	context->is_d3d12 = (RD::get_singleton()->get_device_api_name().to_lower() == "d3d12");

	print_verbose(vformat("DLSS: context %dx%d -> %dx%d, quality mode %d, adapter support %s.",
			p_internal_size.width, p_internal_size.height, p_target_size.width, p_target_size.height,
			int(context->current_dlss_options.mode),
			StreamlineContext::get().streamline_capabilities.dlss_available ? "yes" : "no"));

	return context;
}

static sl::float4x4 sl_make_identity_matrix() {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(1.0f, 0.0f, 0.0f, 0.0f));
	ret.setRow(1, sl::float4(0.0f, 1.0f, 0.0f, 0.0f));
	ret.setRow(2, sl::float4(0.0f, 0.0f, 1.0f, 0.0f));
	ret.setRow(3, sl::float4(0.0f, 0.0f, 0.0f, 1.0f));
	return ret;
}

static sl::float4x4 sl_convert_matrix(const Projection &p_mtx) {
	sl::float4x4 ret;
	ret.setRow(0, sl::float4(p_mtx.columns[0].x, p_mtx.columns[1].x, p_mtx.columns[2].x, p_mtx.columns[3].x));
	ret.setRow(1, sl::float4(p_mtx.columns[0].y, p_mtx.columns[1].y, p_mtx.columns[2].y, p_mtx.columns[3].y));
	ret.setRow(2, sl::float4(p_mtx.columns[0].z, p_mtx.columns[1].z, p_mtx.columns[2].z, p_mtx.columns[3].z));
	ret.setRow(3, sl::float4(p_mtx.columns[0].w, p_mtx.columns[1].w, p_mtx.columns[2].w, p_mtx.columns[3].w));
	return ret;
}

static sl::float3 sl_convert_vector(const Vector3 &p_vec) {
	return sl::float3(p_vec.x, p_vec.y, p_vec.z);
}

void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {
	DLSSContextInner *context = (DLSSContextInner *)p_params.context;
	ERR_FAIL_NULL(context);

	// Warm-up frames. The temporal history is not usable yet.
	if (context->delay > 0) {
		--context->delay;
		return;
	}

	// If DLSS is not loaded, escape early.
	if (StreamlineContext::get().slDLSSSetOptions == nullptr) {
		return;
	}

	// Begin frame if needed.
	if (StreamlineContext::get().last_token == nullptr) {
		StreamlineContext::get().get_new_frame_token();
	}

	context->last_parameters = p_params;
	context->last_effect = this;

	// DLSS requires every texel of the motion vector buffer to be valid, but the
	// engine leaves static geometry marked as invalid. Derive those from depth.
	{
		RD::get_singleton()->draw_command_begin_label("Decode Invalid Motion Vectors");

		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(uniform_set_cache);

		RD::Uniform u_velocity_image(RD::UNIFORM_TYPE_IMAGE, 0, p_params.velocity);
		RD::Uniform u_depth_texture(RD::UNIFORM_TYPE_TEXTURE, 0, p_params.depth);

		RID shader = shaders.mvec_decode_shader.version_get_shader(shaders.mvec_decode_version, 0);
		ERR_FAIL_COND(shader.is_null());

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, shaders.mvec_decode_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_velocity_image), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_depth_texture), 1);

		RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(p_params.velocity);

		float push_constants[20];
		push_constants[0] = texture_format.width;
		push_constants[1] = texture_format.height;
		push_constants[2] = 0.0f;
		push_constants[3] = 0.0f;
		memcpy(push_constants + 4, &p_params.reprojection.columns[0].x, sizeof(float) * 16);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constants, sizeof(push_constants));

		RD::get_singleton()->compute_list_dispatch_threads(compute_list, texture_format.width, texture_format.height, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);

		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
	}

	// Inject DLSS into the render graph. The output is written by DLSS, so it has to be
	// declared read-write: that both makes the graph order the following passes after us
	// and leaves the texture in the general layout DLSS needs to write into.
	RD::CallbackResource res[9];
	int num_resources = 0;
	res[num_resources].rid = p_params.color;
	res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	res[num_resources].rid = p_params.depth;
	res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	res[num_resources].rid = p_params.velocity;
	res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	if (p_params.dlss_rr) {
		for (RID guide : { p_params.dlss_rr_diffuse_albedo, p_params.dlss_rr_specular_albedo, p_params.dlss_rr_normal_roughness, p_params.dlss_rr_specular_hit_dist }) {
			if (guide.is_valid()) {
				res[num_resources].rid = guide;
				res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			}
		}
	}
	res[num_resources].rid = p_params.output;
	res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;

	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::_upscale_internal_graph_callback, p_params.context, VectorView<RD::CallbackResource>(res, num_resources));
}

void DLSSEffect::_upscale_internal(RDD::CommandBufferID p_cmd_buffer, const DLSSContext::Parameters &p_params) {
	DLSSContextInner *context = (DLSSContextInner *)p_params.context;

	void *native_cmdlist = RD::get_singleton()->get_device_driver()->command_buffer_get_native_handle(p_cmd_buffer);

	// Helper for tagging resources.
	auto assign_resource = [context](sl::Resource *r_resources, sl::ResourceTag *r_resource_tags, int &r_num_resources, RID p_texture, sl::BufferType p_buffer_type, sl::ResourceLifecycle p_lifecycle, bool p_writable = false) {
		if (p_texture.is_null()) {
			return;
		}

		RD::TextureFormat texture_format = RD::get_singleton()->texture_get_format(p_texture);
		uint64_t texture_image = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE, p_texture);
		uint64_t texture_view = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture);
		uint64_t texture_device_memory = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, p_texture);
		uint64_t texture_native_format = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture);
		uint64_t texture_usage_flags = RD::get_singleton()->get_driver_resource(RD::DriverResource::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_texture);

		uint64_t texture_state;
		if (context->is_d3d12) {
			texture_state = p_writable ? DLSS_D3D12_RESOURCE_STATE_UNORDERED_ACCESS : DLSS_D3D12_RESOURCE_STATE_NON_PIXEL_SR;
		} else {
			texture_state = p_writable ? DLSS_VK_IMAGE_LAYOUT_GENERAL : DLSS_VK_IMAGE_LAYOUT_SHADER_READ_ONLY;
		}

		sl::Resource &destination_resource = r_resources[r_num_resources];
		if (context->is_d3d12) {
			destination_resource = sl::Resource(sl::ResourceType::eTex2d, (void *)texture_view, texture_state);
		} else {
			destination_resource = sl::Resource(sl::ResourceType::eTex2d, (void *)texture_image, (void *)texture_device_memory, (void *)texture_view, texture_state);
		}
		destination_resource.width = texture_format.width;
		destination_resource.height = texture_format.height;
		destination_resource.nativeFormat = texture_native_format;
		destination_resource.arrayLayers = texture_format.array_layers;
		destination_resource.flags = 0;
		destination_resource.mipLevels = texture_format.mipmaps;
		destination_resource.usage = texture_usage_flags;

		r_resource_tags[r_num_resources] = sl::ResourceTag(r_resources + r_num_resources, p_buffer_type, p_lifecycle, nullptr);
		++r_num_resources;
	};

	// Ray Reconstruction denoises and upscales in one step, replacing plain super
	// resolution. It is only possible when the caller supplied the guide buffers.
	const bool use_dlss_rr = p_params.dlss_rr &&
			StreamlineContext::get().slDLSSDSetOptions != nullptr &&
			StreamlineContext::get().streamline_capabilities.dlss_rr_available;

	if (use_dlss_rr) {
		context->current_dlss_d_options.mode = context->current_dlss_options.mode;
		context->current_dlss_d_options.outputWidth = context->current_dlss_options.outputWidth;
		context->current_dlss_d_options.outputHeight = context->current_dlss_options.outputHeight;
		context->current_dlss_d_options.colorBuffersHDR = sl::Boolean::eTrue;
		context->current_dlss_d_options.alphaUpscalingEnabled = p_params.dlss_rr_alpha_upscaling ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->current_dlss_d_options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked; // Normal XYZ + roughness W.

		const Transform3D view_matrix = p_params.cam_transform.affine_inverse();
		context->current_dlss_d_options.worldToCameraView = sl_convert_matrix(Projection(view_matrix));
		context->current_dlss_d_options.cameraViewToWorld = sl_convert_matrix(Projection(view_matrix).inverse());

		char dlss_preset = p_params.preset;
		if (dlss_preset == '?') {
			dlss_preset = StreamlineContext::get().dlss_rr_default_preset;
		}
		sl::DLSSDPreset preset = sl::DLSSDPreset::eDefault;
		if (dlss_preset != '?') {
			preset = (sl::DLSSDPreset)((int)sl::DLSSDPreset::ePresetD + ((int)dlss_preset - (int)'D'));
		}
		context->current_dlss_d_options.dlaaPreset = preset;
		context->current_dlss_d_options.qualityPreset = preset;
		context->current_dlss_d_options.balancedPreset = preset;
		context->current_dlss_d_options.performancePreset = preset;
		context->current_dlss_d_options.ultraPerformancePreset = preset;

		sl::Result result = StreamlineContext::get().slDLSSDSetOptions(context->viewport, context->current_dlss_d_options);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSDSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
	} else if (StreamlineContext::get().slDLSSSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.dlss_available) {
		// The colour buffer handed to DLSS is raw, un-exposed linear HDR: tone mapping and
		// exposure are applied later in the pipeline. Godot's luminance buffer holds average
		// scene luminance, which is not the exposure multiplier DLSS expects, so rather than
		// tag a buffer with mismatched semantics we always let DLSS derive exposure itself.
		context->current_dlss_options.useAutoExposure = sl::Boolean::eTrue;
		context->current_dlss_options.colorBuffersHDR = sl::Boolean::eTrue;

		char dlss_preset = p_params.preset;
		if (dlss_preset == '?') {
			dlss_preset = StreamlineContext::get().dlss_default_preset;
		}

		sl::DLSSPreset preset = sl::DLSSPreset::eDefault;
		if (dlss_preset != '?') {
			preset = (sl::DLSSPreset)((int)sl::DLSSPreset::ePresetF + ((int)dlss_preset - (int)'F'));
		}
		context->current_dlss_options.dlaaPreset = preset;
		context->current_dlss_options.qualityPreset = preset;
		context->current_dlss_options.balancedPreset = preset;
		context->current_dlss_options.performancePreset = preset;
		context->current_dlss_options.ultraPerformancePreset = preset;

		sl::Result result = StreamlineContext::get().slDLSSSetOptions(context->viewport, context->current_dlss_options);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));
	}

	// Set common constants.
	if (StreamlineContext::get().slSetConstants != nullptr) {
		sl::float4x4 mtx_identity = sl_make_identity_matrix();
		context->constants.cameraViewToClip = sl_convert_matrix(p_params.cam_projection); // Projection matrix (unjittered).
		context->constants.clipToCameraView = sl_convert_matrix(p_params.cam_projection.inverse()); // Projection matrix (unjittered, inverted).
		context->constants.clipToLensClip = mtx_identity; // Keep identity unless some lens distortion is applied.
		context->constants.clipToPrevClip = sl_convert_matrix(p_params.clip_to_prev_clip); // Reprojection matrix.
		context->constants.prevClipToClip = sl_convert_matrix(p_params.clip_to_prev_clip.inverse()); // Inverted reprojection matrix.

		context->constants.cameraPos = sl_convert_vector(p_params.cam_transform.get_origin());
		context->constants.cameraFwd = sl_convert_vector(-p_params.cam_transform.get_basis().rows[2]);
		context->constants.cameraUp = sl_convert_vector(p_params.cam_transform.get_basis().rows[1]);
		context->constants.cameraRight = sl_convert_vector(p_params.cam_transform.get_basis().rows[0]);

		context->constants.cameraNear = p_params.z_near;
		context->constants.cameraFar = p_params.z_far;
		context->constants.cameraFOV = Math::deg_to_rad(p_params.fovy);
		context->constants.cameraMotionIncluded = sl::Boolean::eTrue;
		context->constants.cameraAspectRatio = static_cast<float>(context->current_dlss_options.outputWidth) / static_cast<float>(context->current_dlss_options.outputHeight);
		context->constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
		context->constants.depthInverted = p_params.reverse_depth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		context->constants.motionVectors3D = sl::Boolean::eFalse;
		context->constants.motionVectorsDilated = sl::Boolean::eFalse;
		context->constants.motionVectorsJittered = sl::Boolean::eFalse;
		// Passed through unflipped. Godot applies the jitter after the Y-flip correction, so
		// taa_jitter is already in the same Y-down space DLSS expects; negating Y here was
		// measured to misalign the reconstruction by roughly half a pixel. Note this differs
		// from MetalFX, which does need the flip because Metal's origin is bottom-left.
		context->constants.jitterOffset = sl::float2(p_params.jitter.x, p_params.jitter.y);
		context->constants.mvecScale = sl::float2(1.0f, 1.0f);
		context->constants.orthographicProjection = sl::Boolean::eFalse;
		context->constants.reset = p_params.reset_accumulation ? sl::Boolean::eTrue : sl::Boolean::eFalse;

		sl::Result result = StreamlineContext::get().slSetConstants(context->constants, *StreamlineContext::get().last_token, context->viewport);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slSetConstants. Result: " + String(StreamlineContext::result_to_string(result)));
	}

	// Tag resources.
	if (StreamlineContext::get().slSetTag != nullptr) {
		sl::Resource resources[9];
		sl::ResourceTag resource_tags[9];
		int num_resources = 0;

		assign_resource(resources, resource_tags, num_resources, p_params.color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);
		assign_resource(resources, resource_tags, num_resources, p_params.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, true);
		assign_resource(resources, resource_tags, num_resources, p_params.depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent);
		assign_resource(resources, resource_tags, num_resources, p_params.velocity, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent);

		if (use_dlss_rr) {
			assign_resource(resources, resource_tags, num_resources, p_params.dlss_rr_diffuse_albedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			assign_resource(resources, resource_tags, num_resources, p_params.dlss_rr_specular_albedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
			assign_resource(resources, resource_tags, num_resources, p_params.dlss_rr_normal_roughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent);
			assign_resource(resources, resource_tags, num_resources, p_params.dlss_rr_specular_hit_dist, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent);
		}

		sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resource_tags, num_resources, native_cmdlist);
		ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slSetTag. Result: " + String(StreamlineContext::result_to_string(result)));
	}

	// Toggle DLSS Frame Generation (only enabled in game mode).
	if (StreamlineContext::get().slDLSSGSetOptions != nullptr && StreamlineContext::get().is_game && StreamlineContext::get().streamline_capabilities.dlss_g_available) {
		sl::DLSSGOptions dlss_g_options{};
		bool want_activate_dlssg = p_params.dlss_g;
		bool can_activate_dlssg = StreamlineContext::get().dlssg_delay == 0;

		// Disable previous DLSS-G context if needed.
		if (StreamlineContext::get().dlssg_viewport != sl::ViewportHandle(-1) && ((!want_activate_dlssg && StreamlineContext::get().dlssg_viewport == context->viewport) || (want_activate_dlssg && StreamlineContext::get().dlssg_viewport != context->viewport))) {
			print_verbose("Disabling DLSS Frame Generation on viewport: " + itos((unsigned int)StreamlineContext::get().dlssg_viewport));
			dlss_g_options.mode = sl::DLSSGMode::eOff;
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(StreamlineContext::get().dlssg_viewport, dlss_g_options);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));

			StreamlineContext::get().dlssg_viewport = sl::ViewportHandle(-1);
		}

		// Enable new DLSS-G context if needed.
		if (can_activate_dlssg && want_activate_dlssg && StreamlineContext::get().dlssg_viewport != context->viewport) {
			print_verbose("Enabling DLSS Frame Generation on viewport: " + itos((unsigned int)context->viewport));

			dlss_g_options.mode = sl::DLSSGMode::eOn;
			sl::Result result = StreamlineContext::get().slDLSSGSetOptions(context->viewport, dlss_g_options);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slDLSSGSetOptions. Result: " + String(StreamlineContext::result_to_string(result)));

			StreamlineContext::get().dlssg_viewport = context->viewport;
		}
	}

	// Evaluate DLSS Ray Reconstruction, or plain Super Resolution when no guide buffers exist.
	if (context->current_dlss_options.mode != sl::DLSSMode::eOff) {
		const sl::BaseStructure *inputs[] = { &context->viewport };
		if (use_dlss_rr) {
			sl::Result result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS_RR, *StreamlineContext::get().last_token, inputs, 1, native_cmdlist);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slEvaluateFeature for DLSS Ray Reconstruction. Result: " + String(StreamlineContext::result_to_string(result)));
		} else if (StreamlineContext::get().streamline_capabilities.dlss_available) {
			sl::Result result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureDLSS, *StreamlineContext::get().last_token, inputs, 1, native_cmdlist);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slEvaluateFeature for DLSS Super Resolution. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}

	// Optional post-upscale sharpening through NIS.
	if (p_params.sharpness > 0.0f && StreamlineContext::get().slNISSetOptions != nullptr && StreamlineContext::get().streamline_capabilities.nis_available) {
		{ // Set NIS settings.
			sl::NISOptions options;
			options.hdrMode = sl::NISHDR::eNone;
			options.mode = sl::NISMode::eSharpen;
			options.sharpness = p_params.sharpness;
			StreamlineContext::get().slNISSetOptions(context->viewport, options);
		}

		{ // Tag NIS buffers.
			sl::Resource resources[2];
			sl::ResourceTag resource_tags[2];
			int num_resources = 0;

			// NIS sharpens the upscaled texture in place, which the graph left in the general layout.
			assign_resource(resources, resource_tags, num_resources, p_params.output, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, true);
			assign_resource(resources, resource_tags, num_resources, p_params.output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, true);

			sl::Result result = StreamlineContext::get().slSetTag(context->viewport, resource_tags, num_resources, native_cmdlist);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slSetTag for NIS. Result: " + String(StreamlineContext::result_to_string(result)));
		}

		{ // Evaluate NIS.
			const sl::BaseStructure *inputs[] = { &context->viewport };
			sl::Result result = StreamlineContext::get().slEvaluateFeature(sl::kFeatureNIS, *StreamlineContext::get().last_token, inputs, 1, native_cmdlist);
			ERR_FAIL_COND_MSG(result != sl::Result::eOk, "Failed to call streamline slEvaluateFeature for NIS. Result: " + String(StreamlineContext::result_to_string(result)));
		}
	}
}

void DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	DLSSContextInner *self = (DLSSContextInner *)p_userdata;
	self->last_effect->_upscale_internal(p_command_buffer, self->last_parameters);
}

bool DLSSEffect::is_context_valid(DLSSContext *p_context) {
	DLSSContextInner *context = (DLSSContextInner *)p_context;
	if (context == nullptr) {
		WARN_PRINT_ONCE("DLSS unavailable: no context was created.");
		return false;
	}
	if (context->current_dlss_options.mode == sl::DLSSMode::eOff) {
		return false; // Already reported in detail by find_optimal_mode().
	}
	// The adapter or the Streamline plugin set may not support DLSS at all. Without this
	// check the caller would keep applying temporal jitter that nothing ever resolves.
	if (!StreamlineContext::get().streamline_capabilities.dlss_available) {
		WARN_PRINT_ONCE("DLSS unavailable: Streamline reports the adapter does not support it. Falling back to FSR2.");
		return false;
	}
	if (StreamlineContext::get().slEvaluateFeature == nullptr || StreamlineContext::get().slSetTag == nullptr) {
		WARN_PRINT_ONCE("DLSS unavailable: the Streamline interposer did not provide slEvaluateFeature/slSetTag. Falling back to FSR2.");
		return false;
	}
	return true;
}

bool DLSSEffect::is_ready(DLSSContext *p_context) {
	if (!is_context_valid(p_context)) {
		return false;
	}
	if (((DLSSContextInner *)p_context)->delay > 0) {
		return false; // Still warming up.
	}
	return true;
}

#else // !STREAMLINE_ENABLED

using namespace RendererRD;

DLSSEffect::DLSSEffect() {}
DLSSEffect::~DLSSEffect() {}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	return nullptr;
}

void DLSSEffect::upscale(const DLSSContext::Parameters &p_params) {}

bool DLSSEffect::is_context_valid(DLSSContext *p_context) {
	return false;
}

bool DLSSEffect::is_ready(DLSSContext *p_context) {
	return false;
}

void DLSSEffect::_upscale_internal(RDD::CommandBufferID p_cmd_buffer, const DLSSContext::Parameters &p_params) {}
void DLSSEffect::_upscale_internal_graph_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {}

#endif // STREAMLINE_ENABLED
