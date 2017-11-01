// Copyright (c) 2014- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "base/logging.h"

#include "ext/native/thin3d/thin3d.h"
#include "ext/native/thin3d/VulkanRenderManager.h"
#include "Core/Reporting.h"
#include "GPU/Common/StencilCommon.h"
#include "GPU/Vulkan/FramebufferVulkan.h"
#include "GPU/Vulkan/FragmentShaderGeneratorVulkan.h"
#include "GPU/Vulkan/ShaderManagerVulkan.h"
#include "GPU/Vulkan/TextureCacheVulkan.h"
#include "GPU/Vulkan/VulkanUtil.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

struct StencilValueUB {
	uint32_t u_stencilValue[4];
};

static const char *stencil_fs = R"(#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
layout (binding = 0) uniform sampler2D tex;
layout(push_constant) uniform params {
  int u_stencilValue;
};
layout (location = 0) in vec2 v_texcoord0;
layout (location = 0) out vec4 fragColor0;

void main() {
  vec4 index = texture(tex, v_texcoord0);
	int indexBits = int(floor(index.a * 255.99)) & 0xFF;
	if ((indexBits & u_stencilValue) == 0)
		discard;
  fragColor0 = index.aaaa;
}
)";

static const char stencil_vs[] = R"(#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
layout (location = 0) out vec2 v_texcoord0;
out gl_PerVertex { vec4 gl_Position; };
void main() {
	int id = gl_VertexIndex;
  v_texcoord0.x = (id == 2) ? 2.0 : 0.0;
  v_texcoord0.y = (id == 1) ? 2.0 : 0.0;
  gl_Position = vec4(v_texcoord0 * vec2(2.0, 2.0) + vec2(-1.0, -1.0), 0.0, 1.0);
}
)";

// In Vulkan we should be able to simply copy the stencil data directly to a stencil buffer without
// messing about with bitplane textures and the like. Or actually, maybe not... Let's start with
// the traditional approach.
bool FramebufferManagerVulkan::NotifyStencilUpload(u32 addr, int size, bool skipZero) {
	if (!MayIntersectFramebuffer(addr)) {
		return false;
	}

	VirtualFramebuffer *dstBuffer = 0;
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];
		if (MaskedEqual(vfb->fb_address, addr)) {
			dstBuffer = vfb;
		}
	}
	if (!dstBuffer) {
		return false;
	}

	int values = 0;
	u8 usedBits = 0;

	const u8 *src = Memory::GetPointer(addr);
	if (!src) {
		return false;
	}

	switch (dstBuffer->format) {
	case GE_FORMAT_565:
		// Well, this doesn't make much sense.
		return false;
	case GE_FORMAT_5551:
		usedBits = StencilBits5551(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 2;
		break;
	case GE_FORMAT_4444:
		usedBits = StencilBits4444(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 16;
		break;
	case GE_FORMAT_8888:
		usedBits = StencilBits8888(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 256;
		break;
	case GE_FORMAT_INVALID:
		// Impossible.
		break;
	}

	std::string error;
	if (!stencilVs_) {
		stencilVs_ = CompileShaderModule(vulkan_, VK_SHADER_STAGE_VERTEX_BIT, stencil_vs, &error);
		stencilFs_ = CompileShaderModule(vulkan_, VK_SHADER_STAGE_FRAGMENT_BIT, stencil_fs, &error);
	}
	VkRenderPass rp = (VkRenderPass)draw_->GetNativeObject(Draw::NativeObject::FRAMEBUFFER_RENDERPASS);

	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);

	if (usedBits == 0) {
		if (skipZero) {
			// Common when creating buffers, it's already 0.  We're done.
			return false;
		}

		// TODO: Find a nice way to clear alpha here too.
		draw_->BindFramebufferAsRenderTarget(dstBuffer->fbo, { Draw::RPAction::KEEP, Draw::RPAction::CLEAR });
		gstate_c.Dirty(DIRTY_BLEND_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);
		return true;
	}

	shaderManagerVulkan_->DirtyLastShader();
	textureCacheVulkan_->ForgetLastTexture();

	u16 w = dstBuffer->renderWidth;
	u16 h = dstBuffer->renderHeight;
	float u1 = 1.0f;
	float v1 = 1.0f;
	MakePixelTexture(src, dstBuffer->format, dstBuffer->fb_stride, dstBuffer->bufferWidth, dstBuffer->bufferHeight, u1, v1);
	if (dstBuffer->fbo) {
		draw_->BindFramebufferAsRenderTarget(dstBuffer->fbo, { Draw::RPAction::KEEP, Draw::RPAction::CLEAR });
	} else {
		// something is wrong...
	}

	VkPipeline pipeline = vulkan2D_->GetPipeline(rp, stencilVs_, stencilFs_, false, Vulkan2D::VK2DDepthStencilMode::STENCIL_REPLACE_ALWAYS);
	renderManager->BindPipeline(pipeline);
	renderManager->SetViewport({ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f });
	renderManager->SetScissor({ { 0, 0, },{ (uint32_t)w, (uint32_t)h } });
	gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_BLEND_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE);

	VkDescriptorSet descSet = vulkan2D_->GetDescriptorSet(overrideImageView_, nearestSampler_, VK_NULL_HANDLE, VK_NULL_HANDLE);

	for (int i = 1; i < values; i += i) {
		if (!(usedBits & i)) {
			// It's already zero, let's skip it.
			continue;
		}
		// These feel a little backwards : Mask is the bits that are going to be written, while value
		// is the "mask" that will be tested against.
		uint8_t mask = 0;
		uint32_t value = 0;
		if (dstBuffer->format == GE_FORMAT_4444) {
			mask = i | (i << 4);
			value = i * 16;
		} else if (dstBuffer->format == GE_FORMAT_5551) {
			mask = 0xFF;
			value = i * 128;
		} else {
			mask = i;
			value = i;
		}
		renderManager->SetStencilParams(mask, 0xFF, 0xFF);
		renderManager->PushConstants(vulkan2D_->GetPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &value);
		renderManager->Draw(vulkan2D_->GetPipelineLayout(), descSet, 0, nullptr, VK_NULL_HANDLE, 0, 3);  // full screen triangle
	}

	overrideImageView_ = VK_NULL_HANDLE;
	RebindFramebuffer();
	return true;
}