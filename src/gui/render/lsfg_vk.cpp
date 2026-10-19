// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// LSFG v3.1 compute chain (see gui/render/lsfg_vk.h).
//
// Pipeline facts (stage order, extents, formats, descriptor multisets,
// dispatch group math, uniform bytes, sampler params, barrier transitions)
// follow lsfg-vk v1.0.0 by PancakeTAS (MIT licensed); the Vulkan code here
// is an original implementation written for DOSBox-XR's CPU-frame in/out
// model. Staged precompiled SPIR-V blobs are addressed as base+49 (fp16)
// / base+98 (fp32) per the v1 id table; every blob is self-checked at load
// (descriptor-class counts and exact bindings vs the stage recipe) and any
// mismatch disables the GPU path loudly instead of dispatching garbage.

#include "gui/render/lsfg_vk.h"

#include "dosbox_config.h"
#include "gui/render/lsfg.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if C_XR_VULKAN

#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "misc/logging.h"

// The Vulkan loader is dynamic (vulkan-1.dll / libvulkan.so.1), like the
// OpenXR Vulkan binding: no link dependency, systems without Vulkan keep
// running on CPU blend.
namespace {

#if defined(_WIN32)
struct VkLib {
	HMODULE h = nullptr;
	bool open()
	{
		h = LoadLibraryA("vulkan-1.dll");
		return h != nullptr;
	}
	void* proc(const char* name)
	{
		return h ? reinterpret_cast<void*>(GetProcAddress(h, name)) : nullptr;
	}
};
#else
struct VkLib {
	void* h = nullptr;
	bool open()
	{
		h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
		return h != nullptr;
	}
	void* proc(const char* name)
	{
		return h ? dlsym(h, name) : nullptr;
	}
};
#endif

// Entry points used by the chain.
struct VkFn {
#define VKFN(what) PFN_##what what = nullptr
	VKFN(vkCreateInstance);
	VKFN(vkDestroyInstance);
	VKFN(vkEnumeratePhysicalDevices);
	VKFN(vkGetPhysicalDeviceProperties);
	VKFN(vkGetPhysicalDeviceQueueFamilyProperties);
	VKFN(vkGetPhysicalDeviceMemoryProperties);
	VKFN(vkGetPhysicalDeviceFormatProperties);
	VKFN(vkGetPhysicalDeviceFeatures2);
	VKFN(vkEnumerateDeviceExtensionProperties);
	VKFN(vkCreateDevice);
	VKFN(vkDestroyDevice);
	VKFN(vkGetDeviceQueue);
	VKFN(vkAllocateMemory);
	VKFN(vkFreeMemory);
	VKFN(vkMapMemory);
	VKFN(vkUnmapMemory);
	VKFN(vkCreateBuffer);
	VKFN(vkDestroyBuffer);
	VKFN(vkGetBufferMemoryRequirements);
	VKFN(vkBindBufferMemory);
	VKFN(vkCreateImage);
	VKFN(vkDestroyImage);
	VKFN(vkGetImageMemoryRequirements);
	VKFN(vkBindImageMemory);
	VKFN(vkCreateImageView);
	VKFN(vkDestroyImageView);
	VKFN(vkCreateSampler);
	VKFN(vkDestroySampler);
	VKFN(vkCreateShaderModule);
	VKFN(vkDestroyShaderModule);
	VKFN(vkCreateDescriptorSetLayout);
	VKFN(vkDestroyDescriptorSetLayout);
	VKFN(vkCreatePipelineLayout);
	VKFN(vkDestroyPipelineLayout);
	VKFN(vkCreateComputePipelines);
	VKFN(vkDestroyPipeline);
	VKFN(vkCreateDescriptorPool);
	VKFN(vkDestroyDescriptorPool);
	VKFN(vkAllocateDescriptorSets);
	VKFN(vkUpdateDescriptorSets);
	VKFN(vkCreateCommandPool);
	VKFN(vkDestroyCommandPool);
	VKFN(vkAllocateCommandBuffers);
	VKFN(vkFreeCommandBuffers);
	VKFN(vkBeginCommandBuffer);
	VKFN(vkEndCommandBuffer);
	VKFN(vkCmdPipelineBarrier2);
	VKFN(vkCmdBindPipeline);
	VKFN(vkCmdBindDescriptorSets);
	VKFN(vkCmdDispatch);
	VKFN(vkCmdCopyBufferToImage);
	VKFN(vkCmdCopyImageToBuffer);
	VKFN(vkCreateFence);
	VKFN(vkDestroyFence);
	VKFN(vkWaitForFences);
	VKFN(vkQueueSubmit);
	VKFN(vkQueueWaitIdle);
#undef VKFN

	bool load(VkLib& lib)
	{
#define LOAD(what)                                                \
	what = reinterpret_cast<PFN_##what>(lib.proc(#what));  \
	if (!what)                                            \
		return false;
		LOAD(vkCreateInstance);
		LOAD(vkDestroyInstance);
		LOAD(vkEnumeratePhysicalDevices);
		LOAD(vkGetPhysicalDeviceProperties);
		LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
		LOAD(vkGetPhysicalDeviceMemoryProperties);
		LOAD(vkGetPhysicalDeviceFormatProperties);
		LOAD(vkGetPhysicalDeviceFeatures2);
		LOAD(vkEnumerateDeviceExtensionProperties);
		LOAD(vkCreateDevice);
		LOAD(vkDestroyDevice);
		LOAD(vkGetDeviceQueue);
		LOAD(vkAllocateMemory);
		LOAD(vkFreeMemory);
		LOAD(vkMapMemory);
		LOAD(vkUnmapMemory);
		LOAD(vkCreateBuffer);
		LOAD(vkDestroyBuffer);
		LOAD(vkGetBufferMemoryRequirements);
		LOAD(vkBindBufferMemory);
		LOAD(vkCreateImage);
		LOAD(vkDestroyImage);
		LOAD(vkGetImageMemoryRequirements);
		LOAD(vkBindImageMemory);
		LOAD(vkCreateImageView);
		LOAD(vkDestroyImageView);
		LOAD(vkCreateSampler);
		LOAD(vkDestroySampler);
		LOAD(vkCreateShaderModule);
		LOAD(vkDestroyShaderModule);
		LOAD(vkCreateDescriptorSetLayout);
		LOAD(vkDestroyDescriptorSetLayout);
		LOAD(vkCreatePipelineLayout);
		LOAD(vkDestroyPipelineLayout);
		LOAD(vkCreateComputePipelines);
		LOAD(vkDestroyPipeline);
		LOAD(vkCreateDescriptorPool);
		LOAD(vkDestroyDescriptorPool);
		LOAD(vkAllocateDescriptorSets);
		LOAD(vkUpdateDescriptorSets);
		LOAD(vkCreateCommandPool);
		LOAD(vkDestroyCommandPool);
		LOAD(vkAllocateCommandBuffers);
		LOAD(vkFreeCommandBuffers);
		LOAD(vkBeginCommandBuffer);
		LOAD(vkEndCommandBuffer);
		LOAD(vkCmdPipelineBarrier2);
		LOAD(vkCmdBindPipeline);
		LOAD(vkCmdBindDescriptorSets);
		LOAD(vkCmdDispatch);
		LOAD(vkCmdCopyBufferToImage);
		LOAD(vkCmdCopyImageToBuffer);
		LOAD(vkCreateFence);
		LOAD(vkDestroyFence);
		LOAD(vkWaitForFences);
		LOAD(vkQueueSubmit);
		LOAD(vkQueueWaitIdle);
#undef LOAD
		return true;
	}
};

// --- SPIR-V introspection -------------------------------------------------
// Two-phase parse: OpVariable storage classes first, then OpDecorate
// set/binding joined by target id. Only UniformConstant (class 0)
// variables are resources. Classes follow the translator's register
// convention: UBO 0.., samplers 16.., sampled images 32.., storage
// images 48...

struct SpirvInfo {
	bool ok = false;
	std::string entry = "main";
	// (set, binding) of every UniformConstant variable.
	std::vector<std::pair<uint32_t, uint32_t>> resources;
};

SpirvInfo parse_spirv(const uint32_t* words, size_t nwords)
{
	SpirvInfo info;
	if (nwords < 6 || words[0] != 0x07230203u) {
		return info;
	}
	std::map<uint32_t, uint32_t> var_class; // id -> storage class
	std::map<uint32_t, std::pair<uint32_t, uint32_t>> deco; // id -> {set,binding}
	size_t i = 5;
	while (i < nwords) {
		const uint32_t head = words[i];
		const uint32_t wc   = head >> 16;
		const uint32_t op   = head & 0xffff;
		if (wc < 1 || i + wc > nwords) {
			break;
		}
		if (op == 15 && wc >= 3) { // OpEntryPoint
			std::string name;
			for (size_t k = i + 3; k < i + wc; ++k) {
				for (int s = 0; s < 4; ++s) {
					const char ch = static_cast<char>(
					        (words[k] >> (8 * s)) & 0xff);
					if (!ch) {
						k = i + wc; // end name
						break;
					}
					name.push_back(ch);
				}
			}
			if (!name.empty()) {
				info.entry = name;
			}
		} else if (op == 59 && wc >= 4) { // OpVariable
			var_class[words[i + 2]] = words[i + 3];
		} else if (op == 71 && wc >= 4) { // OpDecorate
			const uint32_t tgt = words[i + 1];
			const uint32_t dec = words[i + 2];
			const uint32_t lit = words[i + 3];
			if (dec == 33) { // Binding
				deco[tgt].second = lit;
			} else if (dec == 34) { // DescriptorSet
				deco[tgt].first = lit;
			}
		}
		i += wc;
	}
	for (const auto& [id, cls] : var_class) {
		// UniformConstant holds samplers/images; Uniform (Block struct)
		// holds the uniform buffer. Anything else is not a resource.
		if (cls != 0 && cls != 2) {
			continue;
		}
		const auto it = deco.find(id);
		if (it == deco.end()) {
			return info; // resource without bindings: invalid
		}
		info.resources.push_back(it->second);
	}
	info.ok = true;
	return info;
}

// Count resources per translator class: {ubo, sampler, sampled, storage}.
std::array<int, 4> classify(const SpirvInfo& info)
{
	std::array<int, 4> n = {0, 0, 0, 0};
	for (const auto& [set, bind] : info.resources) {
		if (set != 0) {
			n[0] = -1000; // unexpected set: poison the signature
			continue;
		}
		if (bind < 16) {
			n[0]++;
		} else if (bind < 32) {
			n[1]++;
		} else if (bind < 48) {
			n[2]++;
		} else if (bind < 64) {
			n[3]++;
		} else {
			n[0] = -1000;
		}
	}
	return n;
}

// --- bootstrap ------------------------------------------------------------

struct Boot {
	VkLib lib;
	VkFn fn;
	VkInstance inst         = VK_NULL_HANDLE;
	VkPhysicalDevice phys   = VK_NULL_HANDLE;
	VkDevice dev            = VK_NULL_HANDLE;
	uint32_t qfam           = 0;
	VkQueue queue           = VK_NULL_HANDLE;
	bool robust             = false;
	bool ready              = false;
	bool down               = false; // logged failure: stay on blend
};

Boot& boot()
{
	static Boot b;
	return b;
}

bool fmt_ok(VkFormat fmt, VkFormatFeatureFlags need)
{
	auto& b = boot();
	VkFormatProperties p{};
	b.fn.vkGetPhysicalDeviceFormatProperties(b.phys, fmt, &p);
	return (p.optimalTilingFeatures & need) == need;
}

bool boot_init()
{
	auto& b = boot();
	if (b.ready || b.down) {
		return b.ready;
	}
	if (!b.lib.open() || !b.fn.load(b.lib)) {
		b.down = true;
		LOG_MSG("LSFGVK: No Vulkan loader; GPU dispatch off (CPU blend)");
		return false;
	}
	const VkApplicationInfo app{
	    .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	    .pApplicationName   = "dosbox-xr-lsfg",
	    .apiVersion         = VK_API_VERSION_1_3,
	};
	const VkInstanceCreateInfo ici{
	    .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .pApplicationInfo        = &app,
	};
	if (b.fn.vkCreateInstance(&ici, nullptr, &b.inst) != VK_SUCCESS ||
	    !b.inst) {
		b.down = true;
		LOG_MSG("LSFGVK: vkCreateInstance failed; GPU dispatch off");
		return false;
	}
	uint32_t ndev = 0;
	if (b.fn.vkEnumeratePhysicalDevices(b.inst, &ndev, nullptr) !=
	            VK_SUCCESS ||
	    ndev == 0) {
		b.down = true;
		LOG_MSG("LSFGVK: No Vulkan devices; GPU dispatch off");
		return false;
	}
	std::vector<VkPhysicalDevice> devs(ndev);
	b.fn.vkEnumeratePhysicalDevices(b.inst, &ndev, devs.data());
	VkPhysicalDevice chosen   = VK_NULL_HANDLE;
	uint32_t chosen_qfam      = 0;
	VkPhysicalDeviceType best = VK_PHYSICAL_DEVICE_TYPE_OTHER;
	for (auto pd : devs) {
		VkPhysicalDeviceProperties pr{};
		b.fn.vkGetPhysicalDeviceProperties(pd, &pr);
		if (pr.apiVersion < VK_API_VERSION_1_3) {
			continue;
		}
		uint32_t nfam = 0;
		b.fn.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nfam, nullptr);
		std::vector<VkQueueFamilyProperties> fams(nfam);
		b.fn.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nfam, fams.data());
		uint32_t qf = 0;
		bool have_compute = false;
		for (uint32_t f = 0; f < nfam; ++f) {
			if (fams[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
				qf            = f;
				have_compute  = true;
				break;
			}
		}
		if (!have_compute) {
			continue;
		}
		if (!chosen || (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
		                best != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
			chosen      = pd;
			chosen_qfam = qf;
			best        = pr.deviceType;
		}
	}
	if (!chosen) {
		b.down = true;
		LOG_MSG("LSFGVK: No Vulkan 1.3 compute device; GPU dispatch off");
		return false;
	}
	b.phys = chosen;
	b.qfam = chosen_qfam;
	// Required storage-image support for the chain formats.
	if (!fmt_ok(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
	    !fmt_ok(VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
	    !fmt_ok(VK_FORMAT_R16G16B16A16_SFLOAT,
	            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
		b.down = true;
		LOG_MSG("LSFGVK: Device lacks storage-image formats; GPU dispatch off");
		return false;
	}
	// Optional robustness2 (null descriptors for the first gamma/delta
	// passes); otherwise a 1x1 dummy image is bound instead. The
	// translated shaders may also use the Vulkan memory model; enable
	// both only when the driver reports support, require synchronization2.
	uint32_t next = 0;
	b.fn.vkEnumerateDeviceExtensionProperties(b.phys, nullptr, &next, nullptr);
	std::vector<VkExtensionProperties> exts(next);
	b.fn.vkEnumerateDeviceExtensionProperties(b.phys, nullptr, &next, exts.data());
	bool have_robust = false;
	for (const auto& e : exts) {
		if (std::string(e.extensionName) == "VK_EXT_robustness2") {
			have_robust = true;
			break;
		}
	}
	VkPhysicalDeviceRobustness2FeaturesEXT qrobust2{
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
	};
	VkPhysicalDeviceVulkan13Features q13{
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
	    .pNext = &qrobust2,
	};
	VkPhysicalDeviceVulkan12Features q12{
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	    .pNext = &q13,
	};
	VkPhysicalDeviceFeatures2 qf{
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
	    .pNext = &q12,
	};
	b.fn.vkGetPhysicalDeviceFeatures2(b.phys, &qf);
	if (!q13.synchronization2) {
		b.down = true;
		LOG_MSG("LSFGVK: Device lacks synchronization2; GPU dispatch off");
		return false;
	}
	const bool use_robust   = have_robust && qrobust2.nullDescriptor;
	const bool use_memmodel = q12.vulkanMemoryModel != 0;
	VkPhysicalDeviceRobustness2FeaturesEXT robust2{
	    .sType          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
	    .nullDescriptor = use_robust ? VK_TRUE : VK_FALSE,
	};
	VkPhysicalDeviceVulkan12Features f12{
	    .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	    .pNext            = nullptr,
	    .vulkanMemoryModel = use_memmodel ? VK_TRUE : VK_FALSE,
	};
	VkPhysicalDeviceVulkan13Features f13{
	    .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
	    .pNext            = use_robust ? static_cast<void*>(&robust2) : nullptr,
	    .synchronization2 = VK_TRUE,
	};
	f12.pNext = &f13;
	void* pnext = &f12;
	const char* ext_names[1] = {};
	uint32_t next_count      = 0;
	if (use_robust) {
		ext_names[0] = "VK_EXT_robustness2";
		next_count   = 1;
	}
	const float prio = 1.0f;
	const VkDeviceQueueCreateInfo qci{
	    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	    .queueFamilyIndex = b.qfam,
	    .queueCount       = 1,
	    .pQueuePriorities = &prio,
	};
	const VkDeviceCreateInfo dci{
	    .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	    .pNext                   = pnext,
	    .queueCreateInfoCount    = 1,
	    .pQueueCreateInfos       = &qci,
	    .enabledExtensionCount   = next_count,
	    .ppEnabledExtensionNames = next_count ? ext_names : nullptr,
	};
	if (b.fn.vkCreateDevice(b.phys, &dci, nullptr, &b.dev) != VK_SUCCESS ||
	    !b.dev) {
		b.down = true;
		LOG_MSG("LSFGVK: vkCreateDevice failed; GPU dispatch off");
		return false;
	}
	b.fn.vkGetDeviceQueue(b.dev, b.qfam, 0, &b.queue);
	b.robust = use_robust;
	b.ready  = true;
	VkPhysicalDeviceProperties pr{};
	b.fn.vkGetPhysicalDeviceProperties(b.phys, &pr);
	LOG_MSG("LSFGVK: GPU dispatch on '%s' (robustness2 %s, memory-model %s)",
	        pr.deviceName, b.robust ? "yes" : "no, dummy-image fallback",
	        use_memmodel ? "yes" : "no");
	return true;
}

// --- small helpers ----------------------------------------------------------

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want,
                  VkMemoryPropertyFlags fallback_ok)
{
	auto& b = boot();
	VkPhysicalDeviceMemoryProperties mp{};
	b.fn.vkGetPhysicalDeviceMemoryProperties(b.phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((bits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	if (fallback_ok) {
		for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
			if (bits & (1u << i)) {
				return i;
			}
		}
	}
	return 0xffffffffu;
}

struct Img {
	VkImage img       = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view  = VK_NULL_HANDLE;
	uint32_t w = 0, h = 0;
	VkFormat fmt = VK_FORMAT_UNDEFINED;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Buf {
	VkBuffer b        = VK_NULL_HANDLE;
	VkDeviceMemory m  = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
};

bool make_image(Img& im, uint32_t w, uint32_t h, VkFormat fmt,
                VkImageUsageFlags extra)
{
	auto& b = boot();
	if (w == 0 || h == 0) {
		return false;
	}
	im.w = w > 0 ? w : 1;
	im.h = h > 0 ? h : 1;
	im.fmt = fmt;
	im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	const VkImageCreateInfo ci{
	    .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType   = VK_IMAGE_TYPE_2D,
	    .format      = fmt,
	    .extent      = {im.w, im.h, 1},
	    .mipLevels   = 1,
	    .arrayLayers = 1,
	    .samples     = VK_SAMPLE_COUNT_1_BIT,
	    .usage = static_cast<VkImageUsageFlags>(
	        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra),
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (b.fn.vkCreateImage(b.dev, &ci, nullptr, &im.img) != VK_SUCCESS ||
	    !im.img) {
		return false;
	}
	VkMemoryRequirements mr{};
	b.fn.vkGetImageMemoryRequirements(b.dev, im.img, &mr);
	const uint32_t mt = mem_type(mr.memoryTypeBits,
	                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
	if (mt == 0xffffffffu) {
		return false;
	}
	const VkMemoryAllocateInfo ai{
	    .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize  = mr.size,
	    .memoryTypeIndex = mt,
	};
	if (b.fn.vkAllocateMemory(b.dev, &ai, nullptr, &im.mem) != VK_SUCCESS) {
		return false;
	}
	if (b.fn.vkBindImageMemory(b.dev, im.img, im.mem, 0) != VK_SUCCESS) {
		return false;
	}
	const VkImageViewCreateInfo vi{
	    .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image      = im.img,
	    .viewType   = VK_IMAGE_VIEW_TYPE_2D,
	    .format     = fmt,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                         .levelCount = 1,
	                         .layerCount = 1},
	};
	if (b.fn.vkCreateImageView(b.dev, &vi, nullptr, &im.view) != VK_SUCCESS) {
		return false;
	}
	return true;
}

bool make_buffer(Buf& bf, VkDeviceSize size, VkBufferUsageFlags usage)
{
	auto& b = boot();
	bf.size = size;
	const VkBufferCreateInfo ci{
	    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size        = size,
	    .usage       = usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (b.fn.vkCreateBuffer(b.dev, &ci, nullptr, &bf.b) != VK_SUCCESS ||
	    !bf.b) {
		return false;
	}
	VkMemoryRequirements mr{};
	b.fn.vkGetBufferMemoryRequirements(b.dev, bf.b, &mr);
	uint32_t mt = mem_type(mr.memoryTypeBits,
	                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
	                       0);
	if (mt == 0xffffffffu) {
		mt = mem_type(mr.memoryTypeBits,
		              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		              0);
	}
	if (mt == 0xffffffffu) {
		return false;
	}
	const VkMemoryAllocateInfo ai{
	    .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize  = mr.size,
	    .memoryTypeIndex = mt,
	};
	if (b.fn.vkAllocateMemory(b.dev, &ai, nullptr, &bf.m) != VK_SUCCESS) {
		return false;
	}
	return b.fn.vkBindBufferMemory(b.dev, bf.b, bf.m, 0) == VK_SUCCESS;
}

void free_img(Img& im)
{
	auto& b = boot();
	if (im.view) {
		b.fn.vkDestroyImageView(b.dev, im.view, nullptr);
	}
	if (im.img) {
		b.fn.vkDestroyImage(b.dev, im.img, nullptr);
	}
	if (im.mem) {
		b.fn.vkFreeMemory(b.dev, im.mem, nullptr);
	}
	im = Img();
}

void free_buf(Buf& bf)
{
	auto& b = boot();
	if (bf.b) {
		b.fn.vkDestroyBuffer(b.dev, bf.b, nullptr);
	}
	if (bf.m) {
		b.fn.vkFreeMemory(b.dev, bf.m, nullptr);
	}
	bf = Buf();
}

// W2R: image was written, will be read. R2W: was read, will be written.
// Both land in GENERAL, exactly like the reference.
void bar_img(VkCommandBuffer cmd, Img& im, bool was_read)
{
	auto& b = boot();
	const VkImageMemoryBarrier2 bar{
	    .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
	    .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	    .srcAccessMask = was_read ? VK_ACCESS_2_SHADER_READ_BIT
	                              : VK_ACCESS_2_SHADER_WRITE_BIT,
	    .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	    .dstAccessMask = was_read ? VK_ACCESS_2_SHADER_WRITE_BIT
	                              : VK_ACCESS_2_SHADER_READ_BIT,
	    .oldLayout     = im.layout,
	    .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
	    .image         = im.img,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                         .levelCount = 1,
	                         .layerCount = 1},
	};
	const VkDependencyInfo di{
	    .sType                  = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
	    .imageMemoryBarrierCount = 1,
	    .pImageMemoryBarriers   = &bar,
	};
	b.fn.vkCmdPipelineBarrier2(cmd, &di);
	im.layout = VK_IMAGE_LAYOUT_GENERAL;
}

void bar_to_transfer_dst(VkCommandBuffer cmd, Img& im)
{
	auto& b = boot();
	const VkImageMemoryBarrier2 bar{
	    .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
	    .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	    .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
	    .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
	    .oldLayout     = im.layout,
	    .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image         = im.img,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                         .levelCount = 1,
	                         .layerCount = 1},
	};
	const VkDependencyInfo di{
	    .sType                  = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
	    .imageMemoryBarrierCount = 1,
	    .pImageMemoryBarriers   = &bar,
	};
	b.fn.vkCmdPipelineBarrier2(cmd, &di);
	im.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
}

void bar_transfer_to_general(VkCommandBuffer cmd, Img& im, bool was_write)
{
	auto& b = boot();
	const VkImageMemoryBarrier2 bar{
	    .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
	    .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	    .srcAccessMask = was_write ? VK_ACCESS_2_TRANSFER_WRITE_BIT
	                               : VK_ACCESS_2_TRANSFER_READ_BIT,
	    .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	    .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
	    .oldLayout     = im.layout,
	    .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
	    .image         = im.img,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                         .levelCount = 1,
	                         .layerCount = 1},
	};
	const VkDependencyInfo di{
	    .sType                  = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
	    .imageMemoryBarrierCount = 1,
	    .pImageMemoryBarriers   = &bar,
	};
	b.fn.vkCmdPipelineBarrier2(cmd, &di);
	im.layout = VK_IMAGE_LAYOUT_GENERAL;
}

void bar_general_to_transfer_src(VkCommandBuffer cmd, Img& im)
{
	auto& b = boot();
	const VkImageMemoryBarrier2 bar{
	    .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
	    .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	    .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
	    .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
	    .oldLayout     = im.layout,
	    .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .image         = im.img,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                         .levelCount = 1,
	                         .layerCount = 1},
	};
	const VkDependencyInfo di{
	    .sType                  = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
	    .imageMemoryBarrierCount = 1,
	    .pImageMemoryBarriers   = &bar,
	};
	b.fn.vkCmdPipelineBarrier2(cmd, &di);
	im.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

// --- shader recipes ---------------------------------------------------------
// Per-shader descriptor multisets in add order (from the reference
// stage sources), with the v1 base resource id. Classes: 0=UBO, 1=sampler,
// 2=sampled image, 3=storage image. Bindings follow the translator rule:
// UBO 0.., samplers 16.., sampled 32.., storage 48...

struct Recipe {
	const char* name;
	int base_id;
	int n_ubo, n_samp, n_simg, n_stor;
};

constexpr Recipe kRecipes[] = {
    {"mipmaps", 255, 1, 1, 1, 7},
    {"alpha[0]", 267, 0, 1, 1, 2},
    {"alpha[1]", 268, 0, 1, 2, 2},
    {"alpha[2]", 269, 0, 1, 2, 4},
    {"alpha[3]", 270, 0, 1, 4, 4},
    {"beta[0]", 275, 0, 1, 12, 2},
    {"beta[1]", 276, 0, 1, 2, 2},
    {"beta[2]", 277, 0, 1, 2, 2},
    {"beta[3]", 278, 0, 1, 2, 2},
    {"beta[4]", 279, 1, 1, 2, 6},
    {"gamma[0]", 257, 1, 2, 9, 3},
    {"gamma[1]", 259, 0, 1, 3, 4},
    {"gamma[2]", 260, 0, 1, 4, 4},
    {"gamma[3]", 261, 0, 1, 4, 4},
    {"gamma[4]", 262, 1, 2, 6, 1},
    {"delta[0]", 257, 1, 2, 9, 3},
    {"delta[1]", 263, 0, 1, 3, 4},
    {"delta[2]", 264, 0, 1, 4, 4},
    {"delta[3]", 265, 0, 1, 4, 4},
    {"delta[4]", 266, 1, 2, 6, 1},
    {"delta[5]", 258, 1, 2, 10, 2},
    {"delta[6]", 271, 0, 1, 2, 2},
    {"delta[7]", 272, 0, 1, 2, 2},
    {"delta[8]", 273, 0, 1, 2, 2},
    {"delta[9]", 274, 1, 2, 3, 1},
    {"generate", 256, 1, 2, 5, 1},
    // v3.1p performance model (lighter temps/fewer bindings, same topology).
    {"p_mipmaps", 255, 1, 1, 1, 7},
    {"p_alpha[0]", 290, 0, 1, 1, 1},
    {"p_alpha[1]", 291, 0, 1, 1, 1},
    {"p_alpha[2]", 292, 0, 1, 1, 2},
    {"p_alpha[3]", 293, 0, 1, 2, 2},
    {"p_beta[0]", 298, 0, 1, 6, 2},
    {"p_beta[1]", 299, 0, 1, 2, 2},
    {"p_beta[2]", 300, 0, 1, 2, 2},
    {"p_beta[3]", 301, 0, 1, 2, 2},
    {"p_beta[4]", 302, 1, 1, 2, 6},
    {"p_gamma[0]", 280, 1, 2, 5, 3},
    {"p_gamma[1]", 282, 0, 1, 3, 2},
    {"p_gamma[2]", 283, 0, 1, 2, 2},
    {"p_gamma[3]", 284, 0, 1, 2, 2},
    {"p_gamma[4]", 285, 1, 2, 4, 1},
    {"p_delta[0]", 280, 1, 2, 5, 3},
    {"p_delta[1]", 286, 0, 1, 3, 2},
    {"p_delta[2]", 287, 0, 1, 2, 2},
    {"p_delta[3]", 288, 0, 1, 2, 2},
    {"p_delta[4]", 289, 1, 2, 4, 1},
    {"p_delta[5]", 281, 1, 2, 6, 1},
    {"p_delta[6]", 294, 0, 1, 1, 1},
    {"p_delta[7]", 295, 0, 1, 1, 1},
    {"p_delta[8]", 296, 0, 1, 1, 1},
    {"p_delta[9]", 297, 1, 2, 2, 1},
    {"p_generate", 256, 1, 2, 5, 1},
};

const Recipe* find_recipe(const char* name)
{
	for (const auto& r : kRecipes) {
		if (std::string(r.name) == name) {
			return &r;
		}
	}
	return nullptr;
}

struct ShaderPipe {
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkPipelineLayout pl       = VK_NULL_HANDLE;
	VkPipeline pipe            = VK_NULL_HANDLE;
};

// Global per-boot caches: samplers, UBOs (by timestamp bits), shader
// modules+pipelines (by name+precision).
struct Shared {
	VkSampler samp[3] = {};
	bool samp_ok      = false;
	std::map<uint64_t, Buf> ubos;
	struct PipeKey {
		std::string name;
		bool fp32;
		bool operator<(const PipeKey& o) const
		{
			return std::tie(name, fp32) < std::tie(o.name, o.fp32);
		}
	};
	std::map<PipeKey, ShaderPipe> pipes;
	std::string staged_dir;
};

Shared& shared()
{
	static Shared s;
	return s;
}

// 48-byte uniform block, byte-identical to the reference:
// inputOffset{0,0}, firstIter 0, firstIterS 0, colorKind 0 (SDR),
// hdrSupport 0, resolutionInvScale 1.0, timestamp, uiThreshold 0.5, pad.
struct ConstBlock {
	uint32_t input_offset[2];
	uint32_t first_iter;
	uint32_t first_iter_s;
	uint32_t color_kind;
	uint32_t hdr_support;
	float inv_scale;
	float timestamp;
	float ui_threshold;
	uint32_t pad[3];
};

// Flow working-resolution scale: the optical-flow mip pyramid and every
// stage derived from it run at (frame * g_flow_scale); 1.0 is full
// resolution and is the reference's own default is 0.7. The UBO field
// resolutionInvScale is its reciprocal. A change invalidates every chain
// and cached uniform buffer (see vk_set_flow_scale).
static float g_flow_scale = 1.0f;

static uint32_t flow_extent(int e)
{
	const float fs    = g_flow_scale > 0.0f ? g_flow_scale : 1.0f;
	const uint32_t v  = static_cast<uint32_t>(static_cast<float>(e) * fs);
	return v > 0 ? v : 1;
}

bool get_ubo(uint32_t ts_bits, Buf& out)
{
	auto& b = boot();
	auto& s = shared();
	const float inv_scale = g_flow_scale > 0.0f ? 1.0f / g_flow_scale
	                                            : 1.0f;
	uint32_t scale_bits = 0;
	std::memcpy(&scale_bits, &inv_scale, 4);
	const uint64_t key = static_cast<uint64_t>(ts_bits) |
	                     (static_cast<uint64_t>(scale_bits) << 32);
	const auto it = s.ubos.find(key);
	if (it != s.ubos.end()) {
		out = it->second;
		return true;
	}
	ConstBlock cb{};
	cb.inv_scale    = inv_scale;
	cb.ui_threshold = 0.5f;
	static_assert(sizeof(cb) == 48, "ConstBlock must be 48 bytes");
	std::memcpy(&cb.timestamp, &ts_bits, 4);
	if (!make_buffer(out, sizeof(cb),
	                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
		return false;
	}
	void* p = nullptr;
	if (b.fn.vkMapMemory(b.dev, out.m, 0, sizeof(cb), 0, &p) != VK_SUCCESS) {
		return false;
	}
	std::memcpy(p, &cb, sizeof(cb));
	b.fn.vkUnmapMemory(b.dev, out.m);
	s.ubos[key] = out;
	return true;
}

bool get_samplers()
{
	auto& b = boot();
	auto& s = shared();
	if (s.samp_ok) {
		return true;
	}
	struct Spec {
		VkSamplerAddressMode mode;
		VkCompareOp cmp;
		bool white;
	};
	const Spec specs[3] = {
	    {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, false},
	    {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, true},
	    {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false},
	};
	for (int i = 0; i < 3; ++i) {
		const VkSamplerCreateInfo ci{
		    .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		    .magFilter    = VK_FILTER_LINEAR,
		    .minFilter    = VK_FILTER_LINEAR,
		    .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		    .addressModeU = specs[i].mode,
		    .addressModeV = specs[i].mode,
		    .addressModeW = specs[i].mode,
		    .compareOp    = specs[i].cmp,
		    .maxLod       = VK_LOD_CLAMP_NONE,
		    .borderColor  = specs[i].white
		        ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
		        : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		};
		if (b.fn.vkCreateSampler(b.dev, &ci, nullptr, &s.samp[i]) !=
		            VK_SUCCESS ||
		    !s.samp[i]) {
			return false;
		}
	}
	s.samp_ok = true;
	return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		return false;
	}
	const auto n = static_cast<size_t>(f.tellg());
	f.seekg(0);
	out.resize(n);
	return n == 0 ||
	       static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()),
	                                static_cast<std::streamsize>(n)));
}

// Load + self-check one shader: blob bindings must equal the recipe rule
// (UBO 0.., samplers 16.., sampled 32.., storage 48.. in add order).
bool get_shader_pipe(const char* name, bool fp32, ShaderPipe& out)
{
	auto& b = boot();
	auto& s = shared();
	Shared::PipeKey key{name, fp32};
	const auto hit = s.pipes.find(key);
	if (hit != s.pipes.end()) {
		out = hit->second;
		return true;
	}
	const Recipe* r = find_recipe(name);
	if (!r) {
		LOG_ERR("LSFGVK: No recipe for '%s'", name);
		return false;
	}
	const int blob_id = r->base_id + (fp32 ? 98 : 49);
	char fname[64]    = {};
	snprintf(fname, sizeof(fname), "rcdata_%d.bin", blob_id);
	std::vector<uint8_t> code;
	if (!read_file(s.staged_dir + "/" + fname, code) || code.size() < 20 ||
	    code.size() % 4 != 0) {
		LOG_ERR("LSFGVK: Cannot read %s for '%s'", fname, name);
		return false;
	}
	const SpirvInfo si = parse_spirv(
	        reinterpret_cast<const uint32_t*>(code.data()), code.size() / 4);
	if (!si.ok) {
		LOG_ERR("LSFGVK: SPIR-V parse failed for '%s'", name);
		return false;
	}
	// Expected binding set from the recipe rule.
	std::vector<std::pair<uint32_t, uint32_t>> want;
	for (int i = 0; i < r->n_ubo; ++i) {
		want.push_back({0, static_cast<uint32_t>(i)});
	}
	for (int i = 0; i < r->n_samp; ++i) {
		want.push_back({0, static_cast<uint32_t>(16 + i)});
	}
	for (int i = 0; i < r->n_simg; ++i) {
		want.push_back({0, static_cast<uint32_t>(32 + i)});
	}
	for (int i = 0; i < r->n_stor; ++i) {
		want.push_back({0, static_cast<uint32_t>(48 + i)});
	}
	auto have = si.resources;
	std::sort(have.begin(), have.end());
	std::sort(want.begin(), want.end());
	if (have != want) {
		LOG_ERR("LSFGVK: Binding mismatch in '%s' (%s): got %zu resources, "
		        "want %zu (recipe U%d S%d SI%d ST%d)",
		        name, fname, have.size(), want.size(), r->n_ubo, r->n_samp,
		        r->n_simg, r->n_stor);
		return false;
	}
	VkShaderModule mod = VK_NULL_HANDLE;
	const VkShaderModuleCreateInfo mci{
	    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = code.size(),
	    .pCode    = reinterpret_cast<const uint32_t*>(code.data()),
	};
	if (b.fn.vkCreateShaderModule(b.dev, &mci, nullptr, &mod) != VK_SUCCESS ||
	    !mod) {
		LOG_ERR("LSFGVK: vkCreateShaderModule failed for '%s'", name);
		return false;
	}
	// Layout bindings in the same rule order.
	struct Bind {
		uint32_t n;
		VkDescriptorType t;
	};
	std::vector<VkDescriptorSetLayoutBinding> lbs;
	auto push = [&](int count, VkDescriptorType t, uint32_t base) {
		for (int i = 0; i < count; ++i) {
			lbs.push_back({.binding         = base + static_cast<uint32_t>(i),
			               .descriptorType  = t,
			               .descriptorCount = 1,
			               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});
		}
	};
	push(r->n_ubo, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0);
	push(r->n_samp, VK_DESCRIPTOR_TYPE_SAMPLER, 16);
	push(r->n_simg, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32);
	push(r->n_stor, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 48);
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	const VkDescriptorSetLayoutCreateInfo dci{
	    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = static_cast<uint32_t>(lbs.size()),
	    .pBindings    = lbs.data(),
	};
	if (b.fn.vkCreateDescriptorSetLayout(b.dev, &dci, nullptr, &dsl) !=
	            VK_SUCCESS ||
	    !dsl) {
		b.fn.vkDestroyShaderModule(b.dev, mod, nullptr);
		return false;
	}
	VkPipelineLayout pl = VK_NULL_HANDLE;
	const VkPipelineLayoutCreateInfo pli{
	    .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts    = &dsl,
	};
	VkPipeline pipe = VK_NULL_HANDLE;
	if (b.fn.vkCreatePipelineLayout(b.dev, &pli, nullptr, &pl) != VK_SUCCESS ||
	    !pl) {
		b.fn.vkDestroyDescriptorSetLayout(b.dev, dsl, nullptr);
		b.fn.vkDestroyShaderModule(b.dev, mod, nullptr);
		return false;
	}
	// NOTE: the entry string must outlive pipeline creation; it is only
	// read during vkCreateComputePipelines.
	std::string entry_copy = si.entry;
	const VkComputePipelineCreateInfo pci{
	    .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	    .stage  = {.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	               .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
	               .module = mod,
	               .pName  = entry_copy.c_str()},
	    .layout = pl,
	};
	if (b.fn.vkCreateComputePipelines(b.dev, VK_NULL_HANDLE, 1, &pci,
	                                  nullptr, &pipe) != VK_SUCCESS ||
	    !pipe) {
		LOG_ERR("LSFGVK: vkCreateComputePipelines failed for '%s' (%s)",
		        name, fname);
		b.fn.vkDestroyPipelineLayout(b.dev, pl, nullptr);
		b.fn.vkDestroyDescriptorSetLayout(b.dev, dsl, nullptr);
		b.fn.vkDestroyShaderModule(b.dev, mod, nullptr);
		return false;
	}
	b.fn.vkDestroyShaderModule(b.dev, mod, nullptr);
	ShaderPipe sp{dsl, pl, pipe};
	s.pipes[key] = sp;
	out            = sp;
	return true;
}

// --- chain ------------------------------------------------------------------
// v3.1 full model, generationCount = caller frame count, flowScale 1.0,
// SDR. Field-for-field mirror of the reference stage wiring.

struct Writer {
	VkDescriptorSet set = VK_NULL_HANDLE;
	int cu = 0, cs = 0, ci = 0, ct = 0;
	std::vector<VkWriteDescriptorSet> ws;
	std::vector<VkDescriptorImageInfo> ims;
	std::vector<VkDescriptorBufferInfo> bis;

	void reserve()
	{
		ims.reserve(24);
		bis.reserve(4);
		ws.reserve(24);
	}

	void ubo(const Buf& b)
	{
		bis.push_back({.buffer = b.b, .offset = 0, .range = b.size});
		ws.push_back({.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		              .dstSet          = set,
		              .dstBinding      = static_cast<uint32_t>(cu++),
		              .descriptorCount = 1,
		              .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		              .pBufferInfo     = &bis.back()});
	}
	void samp(VkSampler s)
	{
		ims.push_back({.sampler = s});
		ws.push_back({.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		              .dstSet          = set,
		              .dstBinding      = static_cast<uint32_t>(16 + cs++),
		              .descriptorCount = 1,
		              .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
		              .pImageInfo      = &ims.back()});
	}
	void simg(Img& im)
	{
		ims.push_back({.imageView = im.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL});
		ws.push_back({.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		              .dstSet          = set,
		              .dstBinding      = static_cast<uint32_t>(32 + ci++),
		              .descriptorCount = 1,
		              .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		              .pImageInfo      = &ims.back()});
	}
	void stor(Img& im)
	{
		ims.push_back({.imageView = im.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL});
		ws.push_back({.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		              .dstSet          = set,
		              .dstBinding      = static_cast<uint32_t>(48 + ct++),
		              .descriptorCount = 1,
		              .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		              .pImageInfo      = &ims.back()});
	}
	// Optional sampled image (first gamma/delta passes): empty write
	// with robustness2, 1x1 dummy view otherwise.
	void null_simg(Img* dummy)
	{
		auto& b = boot();
		if (b.robust || !dummy || !dummy->view) {
			ims.push_back({});
		} else {
			ims.push_back({.imageView   = dummy->view,
			               .imageLayout = VK_IMAGE_LAYOUT_GENERAL});
		}
		ws.push_back({.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		              .dstSet          = set,
		              .dstBinding      = static_cast<uint32_t>(32 + ci++),
		              .descriptorCount = 1,
		              .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		              .pImageInfo      = &ims.back()});
	}
	void flush()
	{
		auto& b = boot();
		b.fn.vkUpdateDescriptorSets(b.dev, static_cast<uint32_t>(ws.size()),
		                            ws.data(), 0, nullptr);
	}
};

struct Chain {
	int w = 0, h = 0, gen = 0;
	bool fp32      = false;
	bool broken    = false;
	uint64_t fc    = 0;
	VkDescriptorPool pool  = VK_NULL_HANDLE;
	VkCommandPool cpool    = VK_NULL_HANDLE;
	Img in0, in1, dummy;
	Img mip[7];
	VkDescriptorSet mset[2] = {};
	struct AL {
		Img t1[2], t2[2], t3[4], o[3][4];
		VkDescriptorSet s[3] = {}, l[3] = {};
	} al[7];
	struct {
		Img t1[2], t2[2], o[6];
		VkDescriptorSet f[3] = {}, m[4] = {};
	} be;
	struct GP {
		Buf u;
		VkDescriptorSet f[3] = {}, m[4] = {};
	};
	struct GL {
		Img t1[4], t2[4], o;
		std::vector<GP> ps;
	} ga[7];
	struct DP {
		Buf u;
		VkDescriptorSet f[3] = {}, x[3] = {}, m[8] = {};
	};
	struct DL {
		Img t1[4], t2[4], o1, o2;
		std::vector<DP> ps;
	} de[3];
	struct NP {
		Buf u;
		VkDescriptorSet s[2] = {};
	};
	struct {
		std::vector<NP> ps;
		std::vector<Img> outs;
	} gn;
	Buf up, back;
	std::vector<std::vector<uint32_t>> frames_done; // last run outputs
	ShaderPipe pm, pa[4], pb[5], pg[5], pd[10], pn;
};

VkDescriptorSet alloc_set(Chain& c, VkDescriptorSetLayout layout)
{
	auto& b = boot();
	const VkDescriptorSetAllocateInfo ai{
	    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool     = c.pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts        = &layout,
	};
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (b.fn.vkAllocateDescriptorSets(b.dev, &ai, &set) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return set;
}

void destroy_chain(Chain* c)
{
	if (!c) {
		return;
	}
	auto& b = boot();
	if (c->pool) {
		b.fn.vkDestroyDescriptorPool(b.dev, c->pool, nullptr);
	}
	if (c->cpool) {
		b.fn.vkDestroyCommandPool(b.dev, c->cpool, nullptr);
	}
	free_img(c->in0);
	free_img(c->in1);
	free_img(c->dummy);
	for (auto& m : c->mip) {
		free_img(m);
	}
	for (auto& a : c->al) {
		for (auto& t : a.t1) {
			free_img(t);
		}
		for (auto& t : a.t2) {
			free_img(t);
		}
		for (auto& t : a.t3) {
			free_img(t);
		}
		for (auto& r : a.o) {
			for (auto& o : r) {
				free_img(o);
			}
		}
	}
	for (auto& t : c->be.t1) {
		free_img(t);
	}
	for (auto& t : c->be.t2) {
		free_img(t);
	}
	for (auto& o : c->be.o) {
		free_img(o);
	}
	for (auto& g : c->ga) {
		for (auto& t : g.t1) {
			free_img(t);
		}
		for (auto& t : g.t2) {
			free_img(t);
		}
		free_img(g.o);
	}
	for (auto& d : c->de) {
		for (auto& t : d.t1) {
			free_img(t);
		}
		for (auto& t : d.t2) {
			free_img(t);
		}
		free_img(d.o1);
		free_img(d.o2);
	}
	for (auto& o : c->gn.outs) {
		free_img(o);
	}
	free_buf(c->up);
	free_buf(c->back);
	delete c;
}

bool wire_rest(Chain& c);

// --- chain wiring -----------------------------------------------------------
// Timestamps: mipmaps 0.0, beta 0.5, per-pass (p+1)/(gen+1). UBO bytes are
// byte-identical to the reference (zeros, SDR, invScale 1.0, ui 0.5).

uint32_t ts_bits(float ts)
{
	uint32_t b = 0;
	std::memcpy(&b, &ts, 4);
	return b;
}

bool fetch_pipes(Chain& c, bool fp32)
{
	if (!get_shader_pipe("mipmaps", fp32, c.pm)) {
		return false;
	}
	const char* an[4] = {"alpha[0]", "alpha[1]", "alpha[2]", "alpha[3]"};
	const char* bn[5] = {"beta[0]", "beta[1]", "beta[2]", "beta[3]", "beta[4]"};
	const char* gn[5] = {"gamma[0]", "gamma[1]", "gamma[2]", "gamma[3]", "gamma[4]"};
	const char* dn[10] = {"delta[0]", "delta[1]", "delta[2]", "delta[3]", "delta[4]",
	                      "delta[5]", "delta[6]", "delta[7]", "delta[8]", "delta[9]"};
	for (int i = 0; i < 4; ++i) {
		if (!get_shader_pipe(an[i], fp32, c.pa[i])) {
			return false;
		}
	}
	for (int i = 0; i < 5; ++i) {
		if (!get_shader_pipe(bn[i], fp32, c.pb[i]) ||
		    !get_shader_pipe(gn[i], fp32, c.pg[i])) {
			return false;
		}
	}
	for (int i = 0; i < 10; ++i) {
		if (!get_shader_pipe(dn[i], fp32, c.pd[i])) {
			return false;
		}
	}
	return get_shader_pipe("generate", fp32, c.pn);
}

bool wire_sets(Chain& c)
{
	auto& s = shared();
	auto A  = [&](VkDescriptorSetLayout l) { return alloc_set(c, l); };
	// Samplers: 0 default, 1 border-white, 2 edge.
	VkSampler s0 = s.samp[0], s1 = s.samp[1], s2 = s.samp[2];

	// Mipmaps: UBO, S, SI(in parity), ST(mip 0..6).
	Buf ub0;
	uint32_t z = 0;
	if (!get_ubo(z, ub0)) {
		return false;
	}
	for (int fc = 0; fc < 2; ++fc) {
		Writer w;
		w.set = A(c.pm.dsl);
		if (!w.set) {
			return false;
		}
		w.reserve();
		w.ubo(ub0);
		w.samp(s0);
		w.simg(fc == 0 ? c.in0 : c.in1);
		for (auto& m : c.mip) {
			w.stor(m);
		}
		w.flush();
		c.mset[fc] = w.set;
	}
	return wire_rest(c);
}

bool wire_rest(Chain& c)
{
	auto& s = shared();
	auto A  = [&](VkDescriptorSetLayout l) { return alloc_set(c, l); };
	VkSampler s0 = s.samp[0], s1 = s.samp[1], s2 = s.samp[2];
	Img* dum     = (!boot().robust && c.dummy.view) ? &c.dummy : nullptr;
	auto opt_simg = [&](Writer& w, Img* im) {
		if (im) {
			w.simg(*im);
		} else {
			w.null_simg(dum);
		}
	};

	// Alpha levels: 3 sets (passes 0..2) + 3 last sets (pass 3 ring).
	for (int k = 0; k < 7; ++k) {
		auto& a = c.al[k];
		for (int i = 0; i < 3; ++i) {
			a.s[i] = A(c.pa[i].dsl);
			a.l[i] = A(c.pa[3].dsl);
			if (!a.s[i] || !a.l[i]) {
				return false;
			}
		}
		{
			Writer w; // alpha[0]: S, SI(mip), ST(t1)
			w.set = a.s[0];
			w.reserve();
			w.samp(s0);
			w.simg(c.mip[k]);
			w.stor(a.t1[0]);
			w.stor(a.t1[1]);
			w.flush();
		}
		{
			Writer w; // alpha[1]: S, SI(t1), ST(t2)
			w.set = a.s[1];
			w.reserve();
			w.samp(s0);
			w.simg(a.t1[0]);
			w.simg(a.t1[1]);
			w.stor(a.t2[0]);
			w.stor(a.t2[1]);
			w.flush();
		}
		{
			Writer w; // alpha[2]: S, SI(t2), ST(t3)
			w.set = a.s[2];
			w.reserve();
			w.samp(s0);
			w.simg(a.t2[0]);
			w.simg(a.t2[1]);
			for (auto& t : a.t3) {
				w.stor(t);
			}
			w.flush();
		}
		for (int r = 0; r < 3; ++r) {
			Writer w; // alpha[3]: S, SI(t3), ST(o[r])
			w.set = a.l[r];
			w.reserve();
			w.samp(s0);
			for (auto& t : a.t3) {
				w.simg(t);
			}
			for (auto& o : a.o[r]) {
				w.stor(o);
			}
			w.flush();
		}
	}

	// Beta: 3 first + 4 singles. UBO(0.5) on the last.
	{
		for (int i = 0; i < 3; ++i) {
			c.be.f[i] = A(c.pb[0].dsl);
			if (!c.be.f[i]) {
				return false;
			}
			Writer w;
			w.set = c.be.f[i];
			w.reserve();
			w.samp(s1);
			for (auto& o : c.al[0].o[(i + 1) % 3]) {
				w.simg(o);
			}
			for (auto& o : c.al[0].o[(i + 2) % 3]) {
				w.simg(o);
			}
			for (auto& o : c.al[0].o[i % 3]) {
				w.simg(o);
			}
			w.stor(c.be.t1[0]);
			w.stor(c.be.t1[1]);
			w.flush();
		}
		for (int i = 0; i < 4; ++i) {
			c.be.m[i] = A(c.pb[i + 1].dsl);
			if (!c.be.m[i]) {
				return false;
			}
		}
		Img* ping[3] = {c.be.t1, c.be.t2, c.be.t1};
		Img* pong[3] = {c.be.t2, c.be.t1, c.be.t2};
		for (int i = 0; i < 3; ++i) {
			Writer w;
			w.set = c.be.m[i];
			w.reserve();
			w.samp(s0);
			w.simg(ping[i][0]);
			w.simg(ping[i][1]);
			w.stor(pong[i][0]);
			w.stor(pong[i][1]);
			w.flush();
		}
		{
			Buf ub;
			float tsf = 0.5f;
			uint32_t tsb = 0;
			std::memcpy(&tsb, &tsf, 4);
			if (!get_ubo(tsb, ub)) {
				return false;
			}
			Writer w;
			w.set = c.be.m[3];
			w.reserve();
			w.ubo(ub);
			w.samp(s0);
			w.simg(c.be.t2[0]);
			w.simg(c.be.t2[1]);
			for (auto& o : c.be.o) {
				w.stor(o);
			}
			w.flush();
		}
	}

	// Gamma levels x passes.
	for (int gi = 0; gi < 7; ++gi) {
		auto& g     = c.ga[gi];
		auto& rings = c.al[6 - gi].o;
		Img& bin    = c.be.o[(6 - gi) < 5 ? (6 - gi) : 5];
		for (auto& p : g.ps) {
			for (int i = 0; i < 3; ++i) {
				p.f[i] = A(c.pg[0].dsl);
				if (!p.f[i]) {
					return false;
				}
				Writer w;
				w.set = p.f[i];
				w.reserve();
				w.ubo(p.u);
				w.samp(s1);
				w.samp(s2);
				for (auto& o : rings[(i + 2) % 3]) {
					w.simg(o);
				}
				for (auto& o : rings[i % 3]) {
					w.simg(o);
				}
				opt_simg(w, (gi == 0) ? nullptr : &c.ga[gi - 1].o);
				w.stor(g.t1[0]);
				w.stor(g.t1[1]);
				w.stor(g.t1[2]);
				w.flush();
			}
			for (int i = 0; i < 4; ++i) {
				p.m[i] = A(c.pg[i + 1].dsl);
				if (!p.m[i]) {
					return false;
				}
			}
			{
				Writer w; // gamma[1]
				w.set = p.m[0];
				w.reserve();
				w.samp(s0);
				w.simg(g.t1[0]);
				w.simg(g.t1[1]);
				w.simg(g.t1[2]);
				for (auto& t : g.t2) {
					w.stor(t);
				}
				w.flush();
			}
			{
				Writer w; // gamma[2]
				w.set = p.m[1];
				w.reserve();
				w.samp(s0);
				for (auto& t : g.t2) {
					w.simg(t);
				}
				for (auto& t : g.t1) {
					w.stor(t);
				}
				w.flush();
			}
			{
				Writer w; // gamma[3]
				w.set = p.m[2];
				w.reserve();
				w.samp(s0);
				for (auto& t : g.t1) {
					w.simg(t);
				}
				for (auto& t : g.t2) {
					w.stor(t);
				}
				w.flush();
			}
			{
				Writer w; // gamma[4]
				w.set = p.m[3];
				w.reserve();
				w.ubo(p.u);
				w.samp(s0);
				w.samp(s2);
				for (auto& t : g.t2) {
					w.simg(t);
				}
				opt_simg(w, (gi == 0) ? nullptr : &c.ga[gi - 1].o);
				w.simg(bin);
				w.stor(g.o);
				w.flush();
			}
		}
	}

	// Delta blocks x passes (block j mirrors i=j+4).
	for (int dj = 0; dj < 3; ++dj) {
		const int gi = dj + 4;
		auto& d      = c.de[dj];
		auto& rings  = c.al[6 - gi].o;
		Img& bin     = c.be.o[6 - gi];
		Img* og      = (gi == 4) ? nullptr : &c.ga[gi - 1].o;
		Img* od1     = (gi == 4) ? nullptr : &c.de[dj - 1].o1;
		Img* od2     = (gi == 4) ? nullptr : &c.de[dj - 1].o2;
		for (auto& p : d.ps) {
			for (int i = 0; i < 3; ++i) {
				p.f[i] = A(c.pd[0].dsl);
				p.x[i] = A(c.pd[5].dsl);
				if (!p.f[i] || !p.x[i]) {
					return false;
				}
				{
					Writer w; // delta[0]
					w.set = p.f[i];
					w.reserve();
					w.ubo(p.u);
					w.samp(s1);
					w.samp(s2);
					for (auto& o : rings[(i + 2) % 3]) {
						w.simg(o);
					}
					for (auto& o : rings[i % 3]) {
						w.simg(o);
					}
					opt_simg(w, og);
					w.stor(d.t1[0]);
					w.stor(d.t1[1]);
					w.stor(d.t1[2]);
					w.flush();
				}
				{
					Writer w; // delta[5]
					w.set = p.x[i];
					w.reserve();
					w.ubo(p.u);
					w.samp(s1);
					w.samp(s2);
					for (auto& o : rings[(i + 2) % 3]) {
						w.simg(o);
					}
					for (auto& o : rings[i % 3]) {
						w.simg(o);
					}
					opt_simg(w, og);
					opt_simg(w, od1);
					w.stor(d.t2[0]);
					w.stor(d.t2[1]);
					w.flush();
				}
			}
			for (int i = 0; i < 8; ++i) {
				p.m[i] = A(c.pd[i + 1].dsl);
				if (!p.m[i]) {
					return false;
				}
			}
			{
				Writer w; // delta[1]
				w.set = p.m[0];
				w.reserve();
				w.samp(s0);
				w.simg(d.t1[0]);
				w.simg(d.t1[1]);
				w.simg(d.t1[2]);
				for (auto& t : d.t2) {
					w.stor(t);
				}
				w.flush();
			}
			{
				Writer w; // delta[2]
				w.set = p.m[1];
				w.reserve();
				w.samp(s0);
				for (auto& t : d.t2) {
					w.simg(t);
				}
				for (auto& t : d.t1) {
					w.stor(t);
				}
				w.flush();
			}
			{
				Writer w; // delta[3]
				w.set = p.m[2];
				w.reserve();
				w.samp(s0);
				for (auto& t : d.t1) {
					w.simg(t);
				}
				for (auto& t : d.t2) {
					w.stor(t);
				}
				w.flush();
			}
			{
				Writer w; // delta[4]
				w.set = p.m[3];
				w.reserve();
				w.ubo(p.u);
				w.samp(s0);
				w.samp(s2);
				for (auto& t : d.t2) {
					w.simg(t);
				}
				opt_simg(w, og);
				w.simg(bin);
				w.stor(d.o1);
				w.flush();
			}
			{
				Writer w; // delta[6]
				w.set = p.m[4];
				w.reserve();
				w.samp(s0);
				w.simg(d.t2[0]);
				w.simg(d.t2[1]);
				w.stor(d.t1[0]);
				w.stor(d.t1[1]);
				w.flush();
			}
			{
				Writer w; // delta[7]
				w.set = p.m[5];
				w.reserve();
				w.samp(s0);
				w.simg(d.t1[0]);
				w.simg(d.t1[1]);
				w.stor(d.t2[0]);
				w.stor(d.t2[1]);
				w.flush();
			}
			{
				Writer w; // delta[8]
				w.set = p.m[6];
				w.reserve();
				w.samp(s0);
				w.simg(d.t2[0]);
				w.simg(d.t2[1]);
				w.stor(d.t1[0]);
				w.stor(d.t1[1]);
				w.flush();
			}
			{
				Writer w; // delta[9]
				w.set = p.m[7];
				w.reserve();
				w.ubo(p.u);
				w.samp(s0);
				w.samp(s2);
				w.simg(d.t1[0]);
				w.simg(d.t1[1]);
				opt_simg(w, od2);
				w.stor(d.o2);
				w.flush();
			}
		}
	}

	// Generate x passes.
	for (size_t pi = 0; pi < c.gn.ps.size(); ++pi) {
		auto& p = c.gn.ps[pi];
		for (int j = 0; j < 2; ++j) {
			p.s[j] = A(c.pn.dsl);
			if (!p.s[j]) {
				return false;
			}
			Writer w;
			w.set = p.s[j];
			w.reserve();
			w.ubo(p.u);
			w.samp(s0);
			w.samp(s1);
			w.simg(j == 0 ? c.in1 : c.in0);
			w.simg(j == 0 ? c.in0 : c.in1);
			w.simg(c.ga[6].o);
			w.simg(c.de[2].o1);
			w.simg(c.de[2].o2);
			w.stor(c.gn.outs[pi]);
			w.flush();
		}
	}
	return true;
}

// --- chain build ------------------------------------------------------------

Chain* build_chain(int w, int h, int gen, bool fp32)
{
	auto& b = boot();
	Chain* c = new Chain();
	c->w = w;
	c->h = h;
	c->gen = gen;
	c->fp32 = fp32;
	auto fail = [&](const char* what) -> Chain* {
		LOG_ERR("LSFGVK: Chain build failed (%dx%d x%d): %s", w, h, gen, what);
		destroy_chain(c);
		return nullptr;
	};

	const VkDescriptorPoolSize sizes[4] = {
	    {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 4096},
	    {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 4096},
	    {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 4096},
	    {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 4096},
	};
	const VkDescriptorPoolCreateInfo pci{
	    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
	    .maxSets       = 16384,
	    .poolSizeCount = 4,
	    .pPoolSizes    = sizes,
	};
	if (b.fn.vkCreateDescriptorPool(b.dev, &pci, nullptr, &c->pool) != VK_SUCCESS) {
		return fail("descriptor pool");
	}
	const VkCommandPoolCreateInfo cpi{
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = b.qfam,
	};
	if (b.fn.vkCreateCommandPool(b.dev, &cpi, nullptr, &c->cpool) != VK_SUCCESS) {
		return fail("command pool");
	}

	const VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
	const VkFormat r8    = VK_FORMAT_R8_UNORM;
	const VkFormat f16   = VK_FORMAT_R16G16B16A16_SFLOAT;
	auto half_q = [](uint32_t e, uint32_t& half, uint32_t& quarter) {
		half    = ((e + 1) >> 1) > 0 ? ((e + 1) >> 1) : 1;
		quarter = ((half + 1) >> 1) > 0 ? ((half + 1) >> 1) : 1;
	};
	if (!make_image(c->in0, w, h, rgba8, VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
	    !make_image(c->in1, w, h, rgba8, VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
	    !make_image(c->dummy, 1, 1, rgba8, 0)) {
		return fail("inputs");
	}
	for (int i = 0; i < 7; ++i) {
		const uint32_t mw = (flow_extent(w) >> i) > 0
		        ? (flow_extent(w) >> i)
		        : 1;
		const uint32_t mh = (flow_extent(h) >> i) > 0
		        ? (flow_extent(h) >> i)
		        : 1;
		if (!make_image(c->mip[i], mw, mh, r8, 0)) {
			return fail("mipmaps");
		}
		uint32_t hh = 0, qq = 0, hh2 = 0, qq2 = 0;
		half_q(mw, hh, qq);
		half_q(mh, hh2, qq2);
		auto& a = c->al[i];
		for (auto& t : a.t1) {
			if (!make_image(t, hh, hh2, rgba8, 0)) {
				return fail("alpha temps");
			}
		}
		for (auto& t : a.t2) {
			if (!make_image(t, hh, hh2, rgba8, 0)) {
				return fail("alpha temps");
			}
		}
		for (auto& t : a.t3) {
			if (!make_image(t, qq, qq2, rgba8, 0)) {
				return fail("alpha temps");
			}
		}
		for (auto& r : a.o) {
			for (auto& o : r) {
				if (!make_image(o, qq, qq2, rgba8, 0)) {
					return fail("alpha outs");
				}
			}
		}
	}
	{
		auto& e = c->al[0].o[0][0];
		for (auto& t : c->be.t1) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("beta temps");
			}
		}
		for (auto& t : c->be.t2) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("beta temps");
			}
		}
		for (int i = 0; i < 6; ++i) {
			const uint32_t ew = (e.w >> i) > 0 ? (e.w >> i) : 1;
			const uint32_t eh = (e.h >> i) > 0 ? (e.h >> i) : 1;
			if (!make_image(c->be.o[i], ew, eh, r8, 0)) {
				return fail("beta outs");
			}
		}
	}
	for (int gi = 0; gi < 7; ++gi) {
		auto& g = c->ga[gi];
		auto& e = c->al[6 - gi].o[0][0];
		for (auto& t : g.t1) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("gamma temps");
			}
		}
		for (auto& t : g.t2) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("gamma temps");
			}
		}
		if (!make_image(g.o, e.w, e.h, f16, 0)) {
			return fail("gamma out");
		}
		g.ps.resize(gen);
		for (int p = 0; p < gen; ++p) {
			const float tsf = static_cast<float>(p + 1) /
			                  static_cast<float>(gen + 1);
			uint32_t tsb = 0;
			std::memcpy(&tsb, &tsf, 4);
			if (!get_ubo(tsb, g.ps[p].u)) {
				return fail("gamma ubo");
			}
		}
	}
	for (int dj = 0; dj < 3; ++dj) {
		auto& d = c->de[dj];
		auto& e = c->al[2 - dj].o[0][0];
		for (auto& t : d.t1) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("delta temps");
			}
		}
		for (auto& t : d.t2) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("delta temps");
			}
		}
		if (!make_image(d.o1, e.w, e.h, f16, 0) ||
		    !make_image(d.o2, e.w, e.h, f16, 0)) {
			return fail("delta outs");
		}
		d.ps.resize(gen);
		for (int p = 0; p < gen; ++p) {
			const float tsf = static_cast<float>(p + 1) /
			                  static_cast<float>(gen + 1);
			uint32_t tsb = 0;
			std::memcpy(&tsb, &tsf, 4);
			if (!get_ubo(tsb, d.ps[p].u)) {
				return fail("delta ubo");
			}
		}
	}
	c->gn.ps.resize(gen);
	c->gn.outs.resize(gen);
	for (int p = 0; p < gen; ++p) {
		const float tsf = static_cast<float>(p + 1) /
		                  static_cast<float>(gen + 1);
		uint32_t tsb = 0;
		std::memcpy(&tsb, &tsf, 4);
		if (!get_ubo(tsb, c->gn.ps[p].u)) {
			return fail("generate ubo");
		}
		if (!make_image(c->gn.outs[p], w, h, rgba8,
		                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
			return fail("generate outs");
		}
	}
	const VkDeviceSize frame_bytes = static_cast<VkDeviceSize>(w) * h * 4;
	if (!make_buffer(c->up, frame_bytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
	    !make_buffer(c->back, frame_bytes * static_cast<VkDeviceSize>(gen),
	                 VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
		return fail("staging");
	}
	if (!get_samplers()) {
		return fail("samplers");
	}
	if (!fetch_pipes(*c, fp32)) {
		return fail("pipelines");
	}
	if (!wire_sets(*c)) {
		return fail("descriptor sets");
	}
	LOG_MSG("LSFGVK: Chain ready (%dx%d x%d, %s)", w, h, gen,
	        fp32 ? "fp32" : "fp16");
	return c;
}

// --- record ---------------------------------------------------------------
// Single command buffer per Generate call: upload, pass 1
// (mipmaps/alpha/beta), per-pass gamma/delta/generate, readback copies.

void disp(VkCommandBuffer cmd, const ShaderPipe& p, VkDescriptorSet set,
          uint32_t gx, uint32_t gy)
{
	auto& b = boot();
	b.fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
	b.fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0,
	                             1, &set, 0, nullptr);
	b.fn.vkCmdDispatch(cmd, gx, gy, 1);
}

void bar_r(VkCommandBuffer cmd, Img* arr, int n)
{
	for (int i = 0; i < n; ++i) {
		bar_img(cmd, arr[i], false); // was written, will be read
	}
}

void bar_w(VkCommandBuffer cmd, Img* arr, int n)
{
	for (int i = 0; i < n; ++i) {
		bar_img(cmd, arr[i], true); // was read, will be written
	}
}

void copy_buf_to_img(VkCommandBuffer cmd, VkBuffer src, Img& im,
                       VkDeviceSize off)
{
	auto& b = boot();
	const VkBufferImageCopy rc{
	    .bufferOffset      = off,
	    .bufferRowLength   = 0,
	    .bufferImageHeight = 0,
	    .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                          .layerCount = 1},
	    .imageExtent       = {im.w, im.h, 1},
	};
	b.fn.vkCmdCopyBufferToImage(cmd, src, im.img,
	                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
}

void copy_img_to_buf(VkCommandBuffer cmd, Img& im, VkBuffer dst,
                       VkDeviceSize off)
{
	auto& b = boot();
	const VkBufferImageCopy rc{
	    .bufferOffset      = off,
	    .bufferRowLength   = 0,
	    .bufferImageHeight = 0,
	    .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                          .layerCount = 1},
	    .imageExtent       = {im.w, im.h, 1},
	};
	b.fn.vkCmdCopyImageToBuffer(cmd, im.img,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
	                            1, &rc);
}

void copy_to_img(VkCommandBuffer cmd, Chain& c, Img& im, VkDeviceSize off)
{
	auto& b = boot();
	const VkBufferImageCopy rc{
	    .bufferOffset      = off,
	    .bufferRowLength   = 0,
	    .bufferImageHeight = 0,
	    .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                          .layerCount = 1},
	    .imageExtent       = {im.w, im.h, 1},
	};
	b.fn.vkCmdCopyBufferToImage(cmd, c.up.b, im.img,
	                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
}

void copy_to_buf(VkCommandBuffer cmd, Chain& c, Img& im, VkDeviceSize off)
{
	auto& b = boot();
	const VkBufferImageCopy rc{
	    .bufferOffset      = off,
	    .bufferRowLength   = 0,
	    .bufferImageHeight = 0,
	    .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	                          .layerCount = 1},
	    .imageExtent       = {im.w, im.h, 1},
	};
	b.fn.vkCmdCopyImageToBuffer(cmd, im.img,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.back.b,
	                            1, &rc);
}

bool record_all(Chain& c, VkCommandBuffer cmd)
{
	auto& b       = boot();
	const auto fc = c.fc;
	const uint32_t W = static_cast<uint32_t>(c.w);
	const uint32_t H = static_cast<uint32_t>(c.h);
	const VkDeviceSize fb = static_cast<VkDeviceSize>(c.w) * c.h * 4;

	// Upload (staging holds [in0 pixels][in1 pixels], RGBX swizzled).
	bar_to_transfer_dst(cmd, c.in0);
	bar_to_transfer_dst(cmd, c.in1);
	copy_to_img(cmd, c, c.in0, 0);
	copy_to_img(cmd, c, c.in1, fb);
	bar_transfer_to_general(cmd, c.in0, true);
	bar_transfer_to_general(cmd, c.in1, true);

	// Pass 1: mipmaps.
	{
		Img& sel = (fc % 2 == 0) ? c.in0 : c.in1;
		bar_r(cmd, &sel, 1);
		bar_w(cmd, c.mip, 7);
		const uint32_t gx = (c.mip[0].w + 63) >> 6;
		const uint32_t gy = (c.mip[0].h + 63) >> 6;
		disp(cmd, c.pm, c.mset[fc % 2], gx, gy);
	}
	// Pass 1: alphas, coarse to fine (6..0).
	for (int k = 6; k >= 0; --k) {
		auto& a = c.al[k];
		const uint32_t hx = (a.t1[0].w + 7) >> 3;
		const uint32_t hy = (a.t1[0].h + 7) >> 3;
		const uint32_t qx = (a.t3[0].w + 7) >> 3;
		const uint32_t qy = (a.t3[0].h + 7) >> 3;
		bar_r(cmd, &c.mip[k], 1);
		bar_w(cmd, a.t1, 2);
		disp(cmd, c.pa[0], a.s[0], hx, hy);
		bar_r(cmd, a.t1, 2);
		bar_w(cmd, a.t2, 2);
		disp(cmd, c.pa[1], a.s[1], hx, hy);
		bar_r(cmd, a.t2, 2);
		bar_w(cmd, a.t3, 4);
		disp(cmd, c.pa[2], a.s[2], qx, qy);
		bar_r(cmd, a.t3, 4);
		bar_w(cmd, a.o[fc % 3], 4);
		disp(cmd, c.pa[3], a.l[fc % 3], qx, qy);
	}
	// Pass 1: beta.
	{
		auto& be = c.be;
		const uint32_t gx = (be.t1[0].w + 7) >> 3;
		const uint32_t gy = (be.t1[0].h + 7) >> 3;
		for (int r = 0; r < 3; ++r) {
			bar_r(cmd, c.al[0].o[r], 4);
		}
		bar_w(cmd, be.t1, 2);
		disp(cmd, c.pb[0], be.f[fc % 3], gx, gy);
		bar_r(cmd, be.t1, 2);
		bar_w(cmd, be.t2, 2);
		disp(cmd, c.pb[1], be.m[0], gx, gy);
		bar_r(cmd, be.t2, 2);
		bar_w(cmd, be.t1, 2);
		disp(cmd, c.pb[2], be.m[1], gx, gy);
		bar_r(cmd, be.t1, 2);
		bar_w(cmd, be.t2, 2);
		disp(cmd, c.pb[3], be.m[2], gx, gy);
		bar_r(cmd, be.t2, 2);
		bar_w(cmd, be.o, 6);
		const uint32_t bx = (be.t1[0].w + 31) >> 5;
		const uint32_t by = (be.t1[0].h + 31) >> 5;
		disp(cmd, c.pb[4], be.m[3], bx, by);
	}

	// Pass 2 x gen: gamma + delta + generate.
	for (int p = 0; p < c.gen; ++p) {
		for (int gi = 0; gi < 7; ++gi) {
			auto& g = c.ga[gi];
			auto& GP = g.ps[p];
			auto& rings = c.al[6 - gi].o;
			Img& bin    = c.be.o[(6 - gi) < 5 ? (6 - gi) : 5];
			Img* og     = (gi == 0) ? nullptr : &c.ga[gi - 1].o;
			const uint32_t gx = (g.t1[0].w + 7) >> 3;
			const uint32_t gy = (g.t1[0].h + 7) >> 3;
			// gamma[0]
			bar_r(cmd, rings[(fc + 2) % 3], 4);
			bar_r(cmd, rings[fc % 3], 4);
			if (og) {
				bar_r(cmd, og, 1);
			}
			bar_w(cmd, g.t1, 3);
			disp(cmd, c.pg[0], GP.f[fc % 3], gx, gy);
			// gamma[1]
			bar_r(cmd, g.t1, 3);
			bar_w(cmd, g.t2, 4);
			disp(cmd, c.pg[1], GP.m[0], gx, gy);
			// gamma[2]
			bar_r(cmd, g.t2, 4);
			bar_w(cmd, g.t1, 4);
			disp(cmd, c.pg[2], GP.m[1], gx, gy);
			// gamma[3]
			bar_r(cmd, g.t1, 4);
			bar_w(cmd, g.t2, 4);
			disp(cmd, c.pg[3], GP.m[2], gx, gy);
			// gamma[4]
			bar_r(cmd, g.t2, 4);
			if (og) {
				bar_r(cmd, og, 1);
			}
			bar_r(cmd, &bin, 1);
			bar_w(cmd, &g.o, 1);
			disp(cmd, c.pg[4], GP.m[3], gx, gy);
			// delta for gi >= 4
			if (gi >= 4) {
				auto& d  = c.de[gi - 4];
				auto& DP = d.ps[p];
				auto& dr = c.al[6 - gi].o;
				Img& dbin = c.be.o[6 - gi];
				Img* dog  = (gi == 4) ? nullptr : &c.ga[gi - 1].o;
				Img* dod1 = (gi == 4) ? nullptr : &c.de[gi - 5].o1;
				Img* dod2 = (gi == 4) ? nullptr : &c.de[gi - 5].o2;
				// delta[0]
				bar_r(cmd, dr[(fc + 2) % 3], 4);
				bar_r(cmd, dr[fc % 3], 4);
				if (dog) {
					bar_r(cmd, dog, 1);
				}
				bar_w(cmd, d.t1, 3);
				disp(cmd, c.pd[0], DP.f[fc % 3], gx, gy);
				// delta[1]
				bar_r(cmd, d.t1, 3);
				bar_w(cmd, d.t2, 4);
				disp(cmd, c.pd[1], DP.m[0], gx, gy);
				// delta[2]
				bar_r(cmd, d.t2, 4);
				bar_w(cmd, d.t1, 4);
				disp(cmd, c.pd[2], DP.m[1], gx, gy);
				// delta[3]
				bar_r(cmd, d.t1, 4);
				bar_w(cmd, d.t2, 4);
				disp(cmd, c.pd[3], DP.m[2], gx, gy);
				// delta[4]
				bar_r(cmd, d.t2, 4);
				if (dog) {
					bar_r(cmd, dog, 1);
				}
				bar_r(cmd, &dbin, 1);
				bar_w(cmd, &d.o1, 1);
				disp(cmd, c.pd[4], DP.m[3], gx, gy);
				// delta[5]
				bar_r(cmd, dr[(fc + 2) % 3], 4);
				bar_r(cmd, dr[fc % 3], 4);
				if (dog) {
					bar_r(cmd, dog, 1);
				}
				if (dod1) {
					bar_r(cmd, dod1, 1);
				}
				bar_w(cmd, d.t2, 2);
				disp(cmd, c.pd[5], DP.x[fc % 3], gx, gy);
				// delta[6]
				bar_r(cmd, d.t2, 2);
				bar_w(cmd, d.t1, 2);
				disp(cmd, c.pd[6], DP.m[4], gx, gy);
				// delta[7]
				bar_r(cmd, d.t1, 2);
				bar_w(cmd, d.t2, 2);
				disp(cmd, c.pd[7], DP.m[5], gx, gy);
				// delta[8]
				bar_r(cmd, d.t2, 2);
				bar_w(cmd, d.t1, 2);
				disp(cmd, c.pd[8], DP.m[6], gx, gy);
				// delta[9]
				bar_r(cmd, d.t1, 2);
				if (dod2) {
					bar_r(cmd, dod2, 1);
				}
				bar_w(cmd, &d.o2, 1);
				disp(cmd, c.pd[9], DP.m[7], gx, gy);
			}
		}
		// generate
		{
			auto& NP = c.gn.ps[p];
			bar_r(cmd, &c.in0, 1);
			bar_r(cmd, &c.in1, 1);
			bar_r(cmd, &c.ga[6].o, 1);
			bar_r(cmd, &c.de[2].o1, 1);
			bar_r(cmd, &c.de[2].o2, 1);
			bar_w(cmd, &c.gn.outs[p], 1);
			const uint32_t gx = (W + 15) >> 4;
			const uint32_t gy = (H + 15) >> 4;
			disp(cmd, c.pn, NP.s[fc % 2], gx, gy);
			bar_general_to_transfer_src(cmd, c.gn.outs[p]);
			copy_to_buf(cmd, c, c.gn.outs[p],
			            static_cast<VkDeviceSize>(p) * fb);
			bar_transfer_to_general(cmd, c.gn.outs[p], false);
		}
	}
	return true;
}

// --- execute ---------------------------------------------------------------

struct ChainKey {
	int w, h, gen;
	bool fp32;
	bool operator<(const ChainKey& o) const
	{
		return std::tie(w, h, gen, fp32) < std::tie(o.w, o.h, o.gen, o.fp32);
	}
};

struct State {
	bool prefer_fp32 = false;
	bool perf_model  = false;
	std::string staged_dir;
	std::map<ChainKey, Chain*> chains;
	// Keys that failed deterministically (build or run). Never retried
	// until the DLL, size, count, or precision changes (which makes a
	// new key), so a broken setup costs one log, not one per present.
	std::set<ChainKey> dead;
	// Performance-model chains live in their own map (different images
	// and sets, same helpers and shader/UBO caches).
	std::map<ChainKey, struct PChain*> pchains;
	std::set<ChainKey> pdead;
};

State& state()
{
	static State s;
	return s;
}

bool run_chain(Chain& c, const uint32_t* prev_px, const uint32_t* curr_px)
{
	auto& b       = boot();
	const int w = c.w, h = c.h;
	const VkDeviceSize fb = static_cast<VkDeviceSize>(w) * h * 4;
	c.frames_done.clear();

	// Stage upload (RGBX swizzled): [in0 pixels][in1 pixels]. in0 holds
	// the newest frame on even calls (mirrors the reference alternation).
	const uint32_t* slot0 = (c.fc % 2 == 0) ? curr_px : prev_px;
	const uint32_t* slot1 = (c.fc % 2 == 0) ? prev_px : curr_px;
	void* mapp            = nullptr;
	if (b.fn.vkMapMemory(b.dev, c.up.m, 0, fb * 2, 0, &mapp) != VK_SUCCESS) {
		return false;
	}
	auto* dst = static_cast<uint8_t*>(mapp);
	for (int s = 0; s < 2; ++s) {
		const uint32_t* src = s == 0 ? slot0 : slot1;
		uint8_t* d          = dst + static_cast<size_t>(s) * fb;
		for (int i = 0; i < w * h; ++i) {
			const uint32_t p = src[i];
			d[4 * i + 0]     = static_cast<uint8_t>((p >> 16) & 0xff);
			d[4 * i + 1]     = static_cast<uint8_t>((p >> 8) & 0xff);
			d[4 * i + 2]     = static_cast<uint8_t>(p & 0xff);
			d[4 * i + 3]     = 0xff;
		}
	}
	b.fn.vkUnmapMemory(b.dev, c.up.m);

	const VkCommandPoolCreateInfo cpi{
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
	    .queueFamilyIndex = b.qfam,
	};
	VkCommandPool tpool = VK_NULL_HANDLE;
	if (b.fn.vkCreateCommandPool(b.dev, &cpi, nullptr, &tpool) != VK_SUCCESS) {
		return false;
	}
	const VkCommandBufferAllocateInfo cai{
	    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool        = tpool,
	    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	bool ok             = b.fn.vkAllocateCommandBuffers(b.dev, &cai, &cmd) ==
	            VK_SUCCESS;
	const VkCommandBufferBeginInfo bbi{
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (ok) {
		ok = b.fn.vkBeginCommandBuffer(cmd, &bbi) == VK_SUCCESS;
	}
	if (ok) {
		record_all(c, cmd);
		ok = b.fn.vkEndCommandBuffer(cmd) == VK_SUCCESS;
	}
	VkFence fence = VK_NULL_HANDLE;
	const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (ok) {
		ok = b.fn.vkCreateFence(b.dev, &fci, nullptr, &fence) == VK_SUCCESS;
	}
	if (ok) {
		const VkSubmitInfo si{
		    .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers    = &cmd,
		};
		ok = b.fn.vkQueueSubmit(b.queue, 1, &si, fence) == VK_SUCCESS;
	}
	if (ok) {
		ok = b.fn.vkWaitForFences(b.dev, 1, &fence, VK_TRUE,
		                          5ull * 1000 * 1000 * 1000) == VK_SUCCESS;
	}
	if (fence) {
		b.fn.vkDestroyFence(b.dev, fence, nullptr);
	}
	b.fn.vkFreeCommandBuffers(b.dev, tpool, 1, &cmd);
	b.fn.vkDestroyCommandPool(b.dev, tpool, nullptr);
	if (!ok) {
		return false;
	}

	// Read back + unswizzle (RGBX -> BGRX, forced opaque).
	if (b.fn.vkMapMemory(b.dev, c.back.m, 0, fb * c.gen, 0, &mapp) !=
	    VK_SUCCESS) {
		return false;
	}
	auto* src = static_cast<const uint8_t*>(mapp);
	for (int g = 0; g < c.gen; ++g) {
		std::vector<uint32_t> frame(static_cast<size_t>(w) * h);
		for (int i = 0; i < w * h; ++i) {
			const uint8_t* px = src + static_cast<size_t>(g) * fb + 4 * i;
			frame[i] = static_cast<uint32_t>(px[2]) |
			           (static_cast<uint32_t>(px[1]) << 8) |
			           (static_cast<uint32_t>(px[0]) << 16) | (0xffu << 24);
		}
		c.frames_done.push_back(std::move(frame));
	}
	b.fn.vkUnmapMemory(b.dev, c.back.m);
	c.fc++;
	return true;
}

// --- performance model (v3.1p) --------------------------------------------------
// Lighter-weight mirror of the full chain: single/double temps instead of
// quads, fewer bindings per shader, identical topology/rings/parity/UBO.

struct PChain {
	int w = 0, h = 0, gen = 0;
	bool fp32 = false, broken = false;
	uint64_t fc = 0;
	VkDescriptorPool pool  = VK_NULL_HANDLE;
	VkCommandPool cpool    = VK_NULL_HANDLE;
	Img in0, in1, dummy, mip[7];
	VkDescriptorSet mset[2] = {};
	struct PAL {
		Img t1, t2, t3[2], o[3][2];
		VkDescriptorSet s[3] = {}, l[3] = {};
	} pal[7];
	struct {
		Img t1[2], t2[2], o[6];
		VkDescriptorSet f[3] = {}, m[4] = {};
	} pbe;
	struct PGP {
		Buf u;
		VkDescriptorSet f[3] = {}, m[4] = {};
	};
	struct PGL {
		Img t1[3], t2[2], o;
		std::vector<PGP> ps;
	} pga[7];
	struct PDP {
		Buf u;
		VkDescriptorSet f[3] = {}, x[3] = {}, m[8] = {};
	};
	struct PDL {
		Img t1[3], t2[2], o1, o2;
		std::vector<PDP> ps;
	} pde[3];
	struct PNP {
		Buf u;
		VkDescriptorSet s[2] = {};
	};
	struct {
		std::vector<PNP> ps;
		std::vector<Img> outs;
	} pgn;
	Buf up, back;
	std::vector<std::vector<uint32_t>> frames_done;
	ShaderPipe pm, pa[4], pb[5], pg[5], pd[10], pn;
};

bool p_fetch_pipes(PChain& c, bool fp32)
{
	const char* names[26] = {
	    "p_mipmaps", "p_alpha[0]", "p_alpha[1]", "p_alpha[2]", "p_alpha[3]",
	    "p_beta[0]", "p_beta[1]", "p_beta[2]", "p_beta[3]", "p_beta[4]",
	    "p_gamma[0]", "p_gamma[1]", "p_gamma[2]", "p_gamma[3]", "p_gamma[4]",
	    "p_delta[0]", "p_delta[1]", "p_delta[2]", "p_delta[3]", "p_delta[4]",
	    "p_delta[5]", "p_delta[6]", "p_delta[7]", "p_delta[8]", "p_delta[9]",
	    "p_generate",
	};
	ShaderPipe* outs[26] = {
	    &c.pm, &c.pa[0], &c.pa[1], &c.pa[2], &c.pa[3], &c.pb[0], &c.pb[1],
	    &c.pb[2], &c.pb[3], &c.pb[4], &c.pg[0], &c.pg[1], &c.pg[2], &c.pg[3],
	    &c.pg[4], &c.pd[0], &c.pd[1], &c.pd[2], &c.pd[3], &c.pd[4], &c.pd[5],
	    &c.pd[6], &c.pd[7], &c.pd[8], &c.pd[9], &c.pn,
	};
	for (int i = 0; i < 26; ++i) {
		if (!get_shader_pipe(names[i], fp32, *outs[i])) {
			return false;
		}
	}
	return true;
}

VkDescriptorSet alloc_pset(PChain& c, VkDescriptorSetLayout layout);

bool wire_psets(PChain& c)
{
	auto& s = shared();
	auto A  = [&](VkDescriptorSetLayout l) { return alloc_pset(c, l); };
	VkSampler s0 = s.samp[0], s1 = s.samp[1], s2 = s.samp[2];
	Img* dum     = (!boot().robust && c.dummy.view) ? &c.dummy : nullptr;
	auto opt_simg = [&](Writer& w, Img* im) {
		if (im) {
			w.simg(*im);
		} else {
			w.null_simg(dum);
		}
	};

	// Mipmaps x2 parity (same shader shape as full).
	{
		Buf ub0;
		uint32_t z = 0;
		if (!get_ubo(z, ub0)) {
			return false;
		}
		for (int fc = 0; fc < 2; ++fc) {
			Writer w;
			w.set = A(c.pm.dsl);
			if (!w.set) {
				return false;
			}
			w.reserve();
			w.ubo(ub0);
			w.samp(s0);
			w.simg(fc == 0 ? c.in0 : c.in1);
			for (auto& m : c.mip) {
				w.stor(m);
			}
			w.flush();
			c.mset[fc] = w.set;
		}
	}

	// Alpha levels (single/double temps).
	for (int k = 0; k < 7; ++k) {
		auto& a = c.pal[k];
		for (int i = 0; i < 3; ++i) {
			a.s[i] = A(c.pa[i].dsl);
			a.l[i] = A(c.pa[3].dsl);
			if (!a.s[i] || !a.l[i]) {
				return false;
			}
		}
		{
			Writer w; // p_alpha[0]: S, SI(mip), ST(t1)
			w.set = a.s[0];
			w.reserve();
			w.samp(s0);
			w.simg(c.mip[k]);
			w.stor(a.t1);
			w.flush();
		}
		{
			Writer w; // p_alpha[1]: S, SI(t1), ST(t2)
			w.set = a.s[1];
			w.reserve();
			w.samp(s0);
			w.simg(a.t1);
			w.stor(a.t2);
			w.flush();
		}
		{
			Writer w; // p_alpha[2]: S, SI(t2), ST(t3)
			w.set = a.s[2];
			w.reserve();
			w.samp(s0);
			w.simg(a.t2);
			w.stor(a.t3[0]);
			w.stor(a.t3[1]);
			w.flush();
		}
		for (int r = 0; r < 3; ++r) {
			Writer w; // p_alpha[3]: S, SI(t3), ST(o[r])
			w.set = a.l[r];
			w.reserve();
			w.samp(s0);
			w.simg(a.t3[0]);
			w.simg(a.t3[1]);
			w.stor(a.o[r][0]);
			w.stor(a.o[r][1]);
			w.flush();
		}
	}

	// Beta (rings of 2).
	{
		for (int i = 0; i < 3; ++i) {
			c.pbe.f[i] = A(c.pb[0].dsl);
			if (!c.pbe.f[i]) {
				return false;
			}
			Writer w;
			w.set = c.pbe.f[i];
			w.reserve();
			w.samp(s1);
			for (auto& o : c.pal[0].o[(i + 1) % 3]) {
				w.simg(o);
			}
			for (auto& o : c.pal[0].o[(i + 2) % 3]) {
				w.simg(o);
			}
			for (auto& o : c.pal[0].o[i % 3]) {
				w.simg(o);
			}
			w.stor(c.pbe.t1[0]);
			w.stor(c.pbe.t1[1]);
			w.flush();
		}
		for (int i = 0; i < 4; ++i) {
			c.pbe.m[i] = A(c.pb[i + 1].dsl);
			if (!c.pbe.m[i]) {
				return false;
			}
		}
		Img* ping[3] = {c.pbe.t1, c.pbe.t2, c.pbe.t1};
		Img* pong[3] = {c.pbe.t2, c.pbe.t1, c.pbe.t2};
		for (int i = 0; i < 3; ++i) {
			Writer w;
			w.set = c.pbe.m[i];
			w.reserve();
			w.samp(s0);
			w.simg(ping[i][0]);
			w.simg(ping[i][1]);
			w.stor(pong[i][0]);
			w.stor(pong[i][1]);
			w.flush();
		}
		{
			Buf ub;
			float tsf = 0.5f;
			uint32_t tsb = 0;
			std::memcpy(&tsb, &tsf, 4);
			if (!get_ubo(tsb, ub)) {
				return false;
			}
			Writer w;
			w.set = c.pbe.m[3];
			w.reserve();
			w.ubo(ub);
			w.samp(s0);
			w.simg(c.pbe.t2[0]);
			w.simg(c.pbe.t2[1]);
			for (auto& o : c.pbe.o) {
				w.stor(o);
			}
			w.flush();
		}
	}

	// Gamma levels x passes.
	for (int gi = 0; gi < 7; ++gi) {
		auto& g     = c.pga[gi];
		auto& rings = c.pal[6 - gi].o;
		Img& bin    = c.pbe.o[(6 - gi) < 5 ? (6 - gi) : 5];
		for (auto& p : g.ps) {
			for (int i = 0; i < 3; ++i) {
				p.f[i] = A(c.pg[0].dsl);
				if (!p.f[i]) {
					return false;
				}
				Writer w;
				w.set = p.f[i];
				w.reserve();
				w.ubo(p.u);
				w.samp(s1);
				w.samp(s2);
				for (auto& o : rings[(i + 2) % 3]) {
					w.simg(o);
				}
				for (auto& o : rings[i % 3]) {
					w.simg(o);
				}
				opt_simg(w, (gi == 0) ? nullptr : &c.pga[gi - 1].o);
				w.stor(g.t1[0]);
				w.stor(g.t1[1]);
				w.stor(g.t1[2]);
				w.flush();
			}
			for (int i = 0; i < 4; ++i) {
				p.m[i] = A(c.pg[i + 1].dsl);
				if (!p.m[i]) {
					return false;
				}
			}
			{
				Writer w; // p_gamma[1]: S, SI(t1 all 3), ST(t2)
				w.set = p.m[0];
				w.reserve();
				w.samp(s0);
				w.simg(g.t1[0]);
				w.simg(g.t1[1]);
				w.simg(g.t1[2]);
				w.stor(g.t2[0]);
				w.stor(g.t2[1]);
				w.flush();
			}
			{
				Writer w; // p_gamma[2]: S, SI(t2), ST(t1[0..1])
				w.set = p.m[1];
				w.reserve();
				w.samp(s0);
				w.simg(g.t2[0]);
				w.simg(g.t2[1]);
				w.stor(g.t1[0]);
				w.stor(g.t1[1]);
				w.flush();
			}
			{
				Writer w; // p_gamma[3]: S, SI(t1[0..1]), ST(t2)
				w.set = p.m[2];
				w.reserve();
				w.samp(s0);
				w.simg(g.t1[0]);
				w.simg(g.t1[1]);
				w.stor(g.t2[0]);
				w.stor(g.t2[1]);
				w.flush();
			}
			{
				Writer w; // p_gamma[4]: UBO, S, S, SI(t2+opt+in2), ST(out)
				w.set = p.m[3];
				w.reserve();
				w.ubo(p.u);
				w.samp(s0);
				w.samp(s2);
				w.simg(g.t2[0]);
				w.simg(g.t2[1]);
				opt_simg(w, (gi == 0) ? nullptr : &c.pga[gi - 1].o);
				w.simg(bin);
				w.stor(g.o);
				w.flush();
			}
		}
	}

	// Delta blocks x passes (block j mirrors i=j+4).
	for (int dj = 0; dj < 3; ++dj) {
		const int gi = dj + 4;
		auto& d      = c.pde[dj];
		auto& rings  = c.pal[6 - gi].o;
		Img& bin     = c.pbe.o[6 - gi];
		Img* og      = (gi == 4) ? nullptr : &c.pga[gi - 1].o;
		Img* od1     = (gi == 4) ? nullptr : &c.pde[dj - 1].o1;
		Img* od2     = (gi == 4) ? nullptr : &c.pde[dj - 1].o2;
		for (auto& p : d.ps) {
			for (int i = 0; i < 3; ++i) {
				p.f[i] = A(c.pd[0].dsl);
				p.x[i] = A(c.pd[5].dsl);
				if (!p.f[i] || !p.x[i]) {
					return false;
				}
				{
					Writer w; // p_delta[0]
					w.set = p.f[i];
					w.reserve();
					w.ubo(p.u);
					w.samp(s1);
					w.samp(s2);
					for (auto& o : rings[(i + 2) % 3]) {
						w.simg(o);
					}
					for (auto& o : rings[i % 3]) {
						w.simg(o);
					}
					opt_simg(w, og);
					w.stor(d.t1[0]);
					w.stor(d.t1[1]);
					w.stor(d.t1[2]);
					w.flush();
				}
				{
					Writer w; // p_delta[5]
					w.set = p.x[i];
					w.reserve();
					w.ubo(p.u);
					w.samp(s1);
					w.samp(s2);
					for (auto& o : rings[(i + 2) % 3]) {
						w.simg(o);
					}
					for (auto& o : rings[i % 3]) {
						w.simg(o);
					}
					opt_simg(w, og);
					opt_simg(w, od1);
					w.stor(d.t2[0]);
					w.flush();
				}
			}
			for (int i = 0; i < 8; ++i) {
				p.m[i] = A(c.pd[i + 1].dsl);
				if (!p.m[i]) {
					return false;
				}
			}
			{
				Writer w; // p_delta[1]: S, SI(t1 all 3), ST(t2)
				w.set = p.m[0];
				w.reserve();
				w.samp(s0);
				w.simg(d.t1[0]);
				w.simg(d.t1[1]);
				w.simg(d.t1[2]);
				w.stor(d.t2[0]);
				w.stor(d.t2[1]);
				w.flush();
			}
			{
				Writer w; // p_delta[2]: S, SI(t2), ST(t1[0..1])
				w.set = p.m[1];
				w.reserve();
				w.samp(s0);
				w.simg(d.t2[0]);
				w.simg(d.t2[1]);
				w.stor(d.t1[0]);
				w.stor(d.t1[1]);
				w.flush();
			}
			{
				Writer w; // p_delta[3]: S, SI(t1[0..1]), ST(t2)
				w.set = p.m[2];
				w.reserve();
				w.samp(s0);
				w.simg(d.t1[0]);
				w.simg(d.t1[1]);
				w.stor(d.t2[0]);
				w.stor(d.t2[1]);
				w.flush();
			}
			{
				Writer w; // p_delta[4]: UBO, S, S, SI(t2+opt+in2), ST(out1)
				w.set = p.m[3];
				w.reserve();
				w.ubo(p.u);
				w.samp(s0);
				w.samp(s2);
				w.simg(d.t2[0]);
				w.simg(d.t2[1]);
				opt_simg(w, og);
				w.simg(bin);
				w.stor(d.o1);
				w.flush();
			}
			{
				Writer w; // p_delta[6]: S, SI(t2[0]), ST(t1[0])
				w.set = p.m[4];
				w.reserve();
				w.samp(s0);
				w.simg(d.t2[0]);
				w.stor(d.t1[0]);
				w.flush();
			}
			{
				Writer w; // p_delta[7]: S, SI(t1[0]), ST(t2[0])
				w.set = p.m[5];
				w.reserve();
				w.samp(s0);
				w.simg(d.t1[0]);
				w.stor(d.t2[0]);
				w.flush();
			}
			{
				Writer w; // p_delta[8]: S, SI(t2[0]), ST(t1[0])
				w.set = p.m[6];
				w.reserve();
				w.samp(s0);
				w.simg(d.t2[0]);
				w.stor(d.t1[0]);
				w.flush();
			}
			{
				Writer w; // p_delta[9]: UBO, S, S, SI(t1[0]+opt3), ST(out2)
				w.set = p.m[7];
				w.reserve();
				w.ubo(p.u);
				w.samp(s0);
				w.samp(s2);
				w.simg(d.t1[0]);
				opt_simg(w, od2);
				w.stor(d.o2);
				w.flush();
			}
		}
	}

	// Generate x passes (same wiring as full).
	for (size_t pi = 0; pi < c.pgn.ps.size(); ++pi) {
		auto& p = c.pgn.ps[pi];
		for (int j = 0; j < 2; ++j) {
			p.s[j] = A(c.pn.dsl);
			if (!p.s[j]) {
				return false;
			}
			Writer w;
			w.set = p.s[j];
			w.reserve();
			w.ubo(p.u);
			w.samp(s0);
			w.samp(s1);
			w.simg(j == 0 ? c.in1 : c.in0);
			w.simg(j == 0 ? c.in0 : c.in1);
			w.simg(c.pga[6].o);
			w.simg(c.pde[2].o1);
			w.simg(c.pde[2].o2);
			w.stor(c.pgn.outs[pi]);
			w.flush();
		}
	}
	return true;
}

VkDescriptorSet alloc_pset(PChain& c, VkDescriptorSetLayout layout)
{
	auto& b = boot();
	const VkDescriptorSetAllocateInfo ai{
	    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool     = c.pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts        = &layout,
	};
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (b.fn.vkAllocateDescriptorSets(b.dev, &ai, &set) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return set;
}

void destroy_pchain(PChain* c)
{
	if (!c) {
		return;
	}
	auto& b = boot();
	if (c->pool) {
		b.fn.vkDestroyDescriptorPool(b.dev, c->pool, nullptr);
	}
	if (c->cpool) {
		b.fn.vkDestroyCommandPool(b.dev, c->cpool, nullptr);
	}
	free_img(c->in0);
	free_img(c->in1);
	free_img(c->dummy);
	for (auto& m : c->mip) {
		free_img(m);
	}
	for (auto& a : c->pal) {
		free_img(a.t1);
		free_img(a.t2);
		for (auto& t : a.t3) {
			free_img(t);
		}
		for (auto& r : a.o) {
			for (auto& o : r) {
				free_img(o);
			}
		}
	}
	for (auto& t : c->pbe.t1) {
		free_img(t);
	}
	for (auto& t : c->pbe.t2) {
		free_img(t);
	}
	for (auto& o : c->pbe.o) {
		free_img(o);
	}
	for (auto& g : c->pga) {
		for (auto& t : g.t1) {
			free_img(t);
		}
		for (auto& t : g.t2) {
			free_img(t);
		}
		free_img(g.o);
	}
	for (auto& d : c->pde) {
		for (auto& t : d.t1) {
			free_img(t);
		}
		for (auto& t : d.t2) {
			free_img(t);
		}
		free_img(d.o1);
		free_img(d.o2);
	}
	for (auto& o : c->pgn.outs) {
		free_img(o);
	}
	free_buf(c->up);
	free_buf(c->back);
	delete c;
}

PChain* build_pchain(int w, int h, int gen, bool fp32)
{
	auto& b = boot();
	PChain* c = new PChain();
	c->w = w;
	c->h = h;
	c->gen = gen;
	c->fp32 = fp32;
	auto fail = [&](const char* what) -> PChain* {
		LOG_ERR("LSFGVK: Perf chain build failed (%dx%d x%d): %s", w, h,
		        gen, what);
		destroy_pchain(c);
		return nullptr;
	};

	const VkDescriptorPoolSize sizes[4] = {
	    {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 4096},
	    {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 4096},
	    {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 4096},
	    {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 4096},
	};
	const VkDescriptorPoolCreateInfo pci{
	    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
	    .maxSets       = 16384,
	    .poolSizeCount = 4,
	    .pPoolSizes    = sizes,
	};
	if (b.fn.vkCreateDescriptorPool(b.dev, &pci, nullptr, &c->pool) != VK_SUCCESS) {
		return fail("descriptor pool");
	}
	const VkCommandPoolCreateInfo cpi{
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = b.qfam,
	};
	if (b.fn.vkCreateCommandPool(b.dev, &cpi, nullptr, &c->cpool) != VK_SUCCESS) {
		return fail("command pool");
	}

	const VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
	const VkFormat r8    = VK_FORMAT_R8_UNORM;
	const VkFormat f16   = VK_FORMAT_R16G16B16A16_SFLOAT;
	auto half_q = [](uint32_t e, uint32_t& half, uint32_t& quarter) {
		half    = ((e + 1) >> 1) > 0 ? ((e + 1) >> 1) : 1;
		quarter = ((half + 1) >> 1) > 0 ? ((half + 1) >> 1) : 1;
	};
	if (!make_image(c->in0, w, h, rgba8, VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
	    !make_image(c->in1, w, h, rgba8, VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
	    !make_image(c->dummy, 1, 1, rgba8, 0)) {
		return fail("inputs");
	}
	for (int i = 0; i < 7; ++i) {
		const uint32_t mw = (flow_extent(w) >> i) > 0
		        ? (flow_extent(w) >> i)
		        : 1;
		const uint32_t mh = (flow_extent(h) >> i) > 0
		        ? (flow_extent(h) >> i)
		        : 1;
		if (!make_image(c->mip[i], mw, mh, r8, 0)) {
			return fail("mipmaps");
		}
		uint32_t hh = 0, qq = 0, hh2 = 0, qq2 = 0;
		half_q(mw, hh, qq);
		half_q(mh, hh2, qq2);
		auto& a = c->pal[i];
		if (!make_image(a.t1, hh, hh2, rgba8, 0) ||
		    !make_image(a.t2, hh, hh2, rgba8, 0)) {
			return fail("alpha temps");
		}
		for (auto& t : a.t3) {
			if (!make_image(t, qq, qq2, rgba8, 0)) {
				return fail("alpha temps");
			}
		}
		for (auto& r : a.o) {
			for (auto& o : r) {
				if (!make_image(o, qq, qq2, rgba8, 0)) {
					return fail("alpha outs");
				}
			}
		}
	}
	{
		auto& e = c->pal[0].o[0][0];
		for (auto& t : c->pbe.t1) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("beta temps");
			}
		}
		for (auto& t : c->pbe.t2) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("beta temps");
			}
		}
		for (int i = 0; i < 6; ++i) {
			const uint32_t ew = (e.w >> i) > 0 ? (e.w >> i) : 1;
			const uint32_t eh = (e.h >> i) > 0 ? (e.h >> i) : 1;
			if (!make_image(c->pbe.o[i], ew, eh, r8, 0)) {
				return fail("beta outs");
			}
		}
	}
	for (int gi = 0; gi < 7; ++gi) {
		auto& g = c->pga[gi];
		auto& e = c->pal[6 - gi].o[0][0];
		for (auto& t : g.t1) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("gamma temps");
			}
		}
		for (auto& t : g.t2) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("gamma temps");
			}
		}
		if (!make_image(g.o, e.w, e.h, f16, 0)) {
			return fail("gamma out");
		}
		g.ps.resize(gen);
		for (int p = 0; p < gen; ++p) {
			const float tsf = static_cast<float>(p + 1) /
			                  static_cast<float>(gen + 1);
			uint32_t tsb = 0;
			std::memcpy(&tsb, &tsf, 4);
			if (!get_ubo(tsb, g.ps[p].u)) {
				return fail("gamma ubo");
			}
		}
	}
	for (int dj = 0; dj < 3; ++dj) {
		auto& d = c->pde[dj];
		auto& e = c->pal[2 - dj].o[0][0];
		for (auto& t : d.t1) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("delta temps");
			}
		}
		for (auto& t : d.t2) {
			if (!make_image(t, e.w, e.h, rgba8, 0)) {
				return fail("delta temps");
			}
		}
		if (!make_image(d.o1, e.w, e.h, f16, 0) ||
		    !make_image(d.o2, e.w, e.h, f16, 0)) {
			return fail("delta outs");
		}
		d.ps.resize(gen);
		for (int p = 0; p < gen; ++p) {
			const float tsf = static_cast<float>(p + 1) /
			                  static_cast<float>(gen + 1);
			uint32_t tsb = 0;
			std::memcpy(&tsb, &tsf, 4);
			if (!get_ubo(tsb, d.ps[p].u)) {
				return fail("delta ubo");
			}
		}
	}
	c->pgn.ps.resize(gen);
	c->pgn.outs.resize(gen);
	for (int p = 0; p < gen; ++p) {
		const float tsf = static_cast<float>(p + 1) /
		                  static_cast<float>(gen + 1);
		uint32_t tsb = 0;
		std::memcpy(&tsb, &tsf, 4);
		if (!get_ubo(tsb, c->pgn.ps[p].u)) {
			return fail("generate ubo");
		}
		if (!make_image(c->pgn.outs[p], w, h, rgba8,
		                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
			return fail("generate outs");
		}
	}
	const VkDeviceSize frame_bytes = static_cast<VkDeviceSize>(w) * h * 4;
	if (!make_buffer(c->up, frame_bytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
	    !make_buffer(c->back, frame_bytes * static_cast<VkDeviceSize>(gen),
	                 VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
		return fail("staging");
	}
	if (!get_samplers()) {
		return fail("samplers");
	}
	if (!p_fetch_pipes(*c, fp32)) {
		return fail("pipelines");
	}
	if (!wire_psets(*c)) {
		return fail("descriptor sets");
	}
	LOG_MSG("LSFGVK: Perf chain ready (%dx%d x%d, %s)", w, h, gen,
	        fp32 ? "fp32" : "fp16");
	return c;
}

bool record_pchain(PChain& c, VkCommandBuffer cmd)
{
	auto& b       = boot();
	const auto fc = c.fc;
	const uint32_t W = static_cast<uint32_t>(c.w);
	const uint32_t H = static_cast<uint32_t>(c.h);
	const VkDeviceSize fb = static_cast<VkDeviceSize>(c.w) * c.h * 4;

	bar_to_transfer_dst(cmd, c.in0);
	bar_to_transfer_dst(cmd, c.in1);
	copy_buf_to_img(cmd, c.up.b, c.in0, 0);
	copy_buf_to_img(cmd, c.up.b, c.in1, fb);
	bar_transfer_to_general(cmd, c.in0, true);
	bar_transfer_to_general(cmd, c.in1, true);

	// Pass 1: mipmaps.
	{
		Img& sel = (fc % 2 == 0) ? c.in0 : c.in1;
		bar_r(cmd, &sel, 1);
		bar_w(cmd, c.mip, 7);
		disp(cmd, c.pm, c.mset[fc % 2], (c.mip[0].w + 63) >> 6,
		     (c.mip[0].h + 63) >> 6);
	}
	// Pass 1: alphas, coarse to fine (6..0).
	for (int k = 6; k >= 0; --k) {
		auto& a = c.pal[k];
		const uint32_t hx = (a.t1.w + 7) >> 3;
		const uint32_t hy = (a.t1.h + 7) >> 3;
		const uint32_t qx = (a.t3[0].w + 7) >> 3;
		const uint32_t qy = (a.t3[0].h + 7) >> 3;
		bar_r(cmd, &c.mip[k], 1);
		bar_w(cmd, &a.t1, 1);
		disp(cmd, c.pa[0], a.s[0], hx, hy);
		bar_r(cmd, &a.t1, 1);
		bar_w(cmd, &a.t2, 1);
		disp(cmd, c.pa[1], a.s[1], hx, hy);
		bar_r(cmd, &a.t2, 1);
		bar_w(cmd, a.t3, 2);
		disp(cmd, c.pa[2], a.s[2], qx, qy);
		bar_r(cmd, a.t3, 2);
		bar_w(cmd, a.o[fc % 3], 2);
		disp(cmd, c.pa[3], a.l[fc % 3], qx, qy);
	}
	// Pass 1: beta.
	{
		auto& be = c.pbe;
		const uint32_t gx = (be.t1[0].w + 7) >> 3;
		const uint32_t gy = (be.t1[0].h + 7) >> 3;
		for (int r = 0; r < 3; ++r) {
			bar_r(cmd, c.pal[0].o[r], 2);
		}
		bar_w(cmd, be.t1, 2);
		disp(cmd, c.pb[0], be.f[fc % 3], gx, gy);
		bar_r(cmd, be.t1, 2);
		bar_w(cmd, be.t2, 2);
		disp(cmd, c.pb[1], be.m[0], gx, gy);
		bar_r(cmd, be.t2, 2);
		bar_w(cmd, be.t1, 2);
		disp(cmd, c.pb[2], be.m[1], gx, gy);
		bar_r(cmd, be.t1, 2);
		bar_w(cmd, be.t2, 2);
		disp(cmd, c.pb[3], be.m[2], gx, gy);
		bar_r(cmd, be.t2, 2);
		bar_w(cmd, be.o, 6);
		disp(cmd, c.pb[4], be.m[3], (be.t1[0].w + 31) >> 5,
		     (be.t1[0].h + 31) >> 5);
	}

	// Pass 2 x gen.
	for (int p = 0; p < c.gen; ++p) {
		for (int gi = 0; gi < 7; ++gi) {
			auto& g = c.pga[gi];
			auto& GP = g.ps[p];
			auto& rings = c.pal[6 - gi].o;
			Img& bin    = c.pbe.o[(6 - gi) < 5 ? (6 - gi) : 5];
			Img* og     = (gi == 0) ? nullptr : &c.pga[gi - 1].o;
			const uint32_t gx = (g.t1[0].w + 7) >> 3;
			const uint32_t gy = (g.t1[0].h + 7) >> 3;
			bar_r(cmd, rings[(fc + 2) % 3], 2);
			bar_r(cmd, rings[fc % 3], 2);
			if (og) {
				bar_r(cmd, og, 1);
			}
			bar_w(cmd, g.t1, 3);
			disp(cmd, c.pg[0], GP.f[fc % 3], gx, gy);
			bar_r(cmd, g.t1, 3);
			bar_w(cmd, g.t2, 2);
			disp(cmd, c.pg[1], GP.m[0], gx, gy);
			bar_r(cmd, g.t2, 2);
			bar_w(cmd, &g.t1[0], 1);
			bar_w(cmd, &g.t1[1], 1);
			disp(cmd, c.pg[2], GP.m[1], gx, gy);
			bar_r(cmd, &g.t1[0], 1);
			bar_r(cmd, &g.t1[1], 1);
			bar_w(cmd, g.t2, 2);
			disp(cmd, c.pg[3], GP.m[2], gx, gy);
			bar_r(cmd, g.t2, 2);
			if (og) {
				bar_r(cmd, og, 1);
			}
			bar_r(cmd, &bin, 1);
			bar_w(cmd, &g.o, 1);
			disp(cmd, c.pg[4], GP.m[3], gx, gy);
			if (gi >= 4) {
				auto& d  = c.pde[gi - 4];
				auto& DP = d.ps[p];
				auto& dr = c.pal[6 - gi].o;
				Img& dbin = c.pbe.o[6 - gi];
				Img* dog  = (gi == 4) ? nullptr : &c.pga[gi - 1].o;
				Img* dod1 = (gi == 4) ? nullptr : &c.pde[gi - 5].o1;
				Img* dod2 = (gi == 4) ? nullptr : &c.pde[gi - 5].o2;
				bar_r(cmd, dr[(fc + 2) % 3], 2);
				bar_r(cmd, dr[fc % 3], 2);
				if (dog) {
					bar_r(cmd, dog, 1);
				}
				bar_w(cmd, d.t1, 3);
				disp(cmd, c.pd[0], DP.f[fc % 3], gx, gy);
				bar_r(cmd, d.t1, 3);
				bar_w(cmd, d.t2, 2);
				disp(cmd, c.pd[1], DP.m[0], gx, gy);
				bar_r(cmd, d.t2, 2);
				bar_w(cmd, &d.t1[0], 1);
				bar_w(cmd, &d.t1[1], 1);
				disp(cmd, c.pd[2], DP.m[1], gx, gy);
				bar_r(cmd, &d.t1[0], 1);
				bar_r(cmd, &d.t1[1], 1);
				bar_w(cmd, d.t2, 2);
				disp(cmd, c.pd[3], DP.m[2], gx, gy);
				bar_r(cmd, d.t2, 2);
				if (dog) {
					bar_r(cmd, dog, 1);
				}
				bar_r(cmd, &dbin, 1);
				bar_w(cmd, &d.o1, 1);
				disp(cmd, c.pd[4], DP.m[3], gx, gy);
				bar_r(cmd, dr[(fc + 2) % 3], 2);
				bar_r(cmd, dr[fc % 3], 2);
				if (dog) {
					bar_r(cmd, dog, 1);
				}
				if (dod1) {
					bar_r(cmd, dod1, 1);
				}
				bar_w(cmd, d.t2, 2);
				disp(cmd, c.pd[5], DP.x[fc % 3], gx, gy);
				bar_r(cmd, d.t2, 2);
				bar_w(cmd, &d.t1[0], 1);
				bar_w(cmd, &d.t1[1], 1);
				disp(cmd, c.pd[6], DP.m[4], gx, gy);
				bar_r(cmd, &d.t1[0], 1);
				bar_r(cmd, &d.t1[1], 1);
				bar_w(cmd, d.t2, 2);
				disp(cmd, c.pd[7], DP.m[5], gx, gy);
				bar_r(cmd, d.t2, 2);
				bar_w(cmd, &d.t1[0], 1);
				bar_w(cmd, &d.t1[1], 1);
				disp(cmd, c.pd[8], DP.m[6], gx, gy);
				bar_r(cmd, &d.t1[0], 1);
				bar_r(cmd, &d.t1[1], 1);
				if (dod2) {
					bar_r(cmd, dod2, 1);
				}
				bar_w(cmd, &d.o2, 1);
				disp(cmd, c.pd[9], DP.m[7], gx, gy);
			}
		}
		{
			auto& NP = c.pgn.ps[p];
			bar_r(cmd, &c.in0, 1);
			bar_r(cmd, &c.in1, 1);
			bar_r(cmd, &c.pga[6].o, 1);
			bar_r(cmd, &c.pde[2].o1, 1);
			bar_r(cmd, &c.pde[2].o2, 1);
			bar_w(cmd, &c.pgn.outs[p], 1);
			disp(cmd, c.pn, NP.s[fc % 2], (W + 15) >> 4, (H + 15) >> 4);
			bar_general_to_transfer_src(cmd, c.pgn.outs[p]);
			copy_img_to_buf(cmd, c.pgn.outs[p], c.back.b,
			                static_cast<VkDeviceSize>(p) * fb);
			bar_transfer_to_general(cmd, c.pgn.outs[p], false);
		}
	}
	return true;
}

bool run_pchain(PChain& c, const uint32_t* prev_px, const uint32_t* curr_px)
{
	auto& b       = boot();
	const int w = c.w, h = c.h;
	const VkDeviceSize fb = static_cast<VkDeviceSize>(w) * h * 4;
	c.frames_done.clear();

	const uint32_t* slot0 = (c.fc % 2 == 0) ? curr_px : prev_px;
	const uint32_t* slot1 = (c.fc % 2 == 0) ? prev_px : curr_px;
	void* mapp            = nullptr;
	if (b.fn.vkMapMemory(b.dev, c.up.m, 0, fb * 2, 0, &mapp) != VK_SUCCESS) {
		return false;
	}
	auto* dst = static_cast<uint8_t*>(mapp);
	for (int s = 0; s < 2; ++s) {
		const uint32_t* src = s == 0 ? slot0 : slot1;
		uint8_t* d          = dst + static_cast<size_t>(s) * fb;
		for (int i = 0; i < w * h; ++i) {
			const uint32_t p = src[i];
			d[4 * i + 0]     = static_cast<uint8_t>((p >> 16) & 0xff);
			d[4 * i + 1]     = static_cast<uint8_t>((p >> 8) & 0xff);
			d[4 * i + 2]     = static_cast<uint8_t>(p & 0xff);
			d[4 * i + 3]     = 0xff;
		}
	}
	b.fn.vkUnmapMemory(b.dev, c.up.m);

	const VkCommandPoolCreateInfo cpi{
	    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
	    .queueFamilyIndex = b.qfam,
	};
	VkCommandPool tpool = VK_NULL_HANDLE;
	if (b.fn.vkCreateCommandPool(b.dev, &cpi, nullptr, &tpool) != VK_SUCCESS) {
		return false;
	}
	const VkCommandBufferAllocateInfo cai{
	    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool        = tpool,
	    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	bool ok             = b.fn.vkAllocateCommandBuffers(b.dev, &cai, &cmd) ==
	            VK_SUCCESS;
	const VkCommandBufferBeginInfo bbi{
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (ok) {
		ok = b.fn.vkBeginCommandBuffer(cmd, &bbi) == VK_SUCCESS;
	}
	if (ok) {
		record_pchain(c, cmd);
		ok = b.fn.vkEndCommandBuffer(cmd) == VK_SUCCESS;
	}
	VkFence fence = VK_NULL_HANDLE;
	const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (ok) {
		ok = b.fn.vkCreateFence(b.dev, &fci, nullptr, &fence) == VK_SUCCESS;
	}
	if (ok) {
		const VkSubmitInfo si{
		    .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers    = &cmd,
		};
		ok = b.fn.vkQueueSubmit(b.queue, 1, &si, fence) == VK_SUCCESS;
	}
	if (ok) {
		ok = b.fn.vkWaitForFences(b.dev, 1, &fence, VK_TRUE,
		                          5ull * 1000 * 1000 * 1000) == VK_SUCCESS;
	}
	if (fence) {
		b.fn.vkDestroyFence(b.dev, fence, nullptr);
	}
	b.fn.vkFreeCommandBuffers(b.dev, tpool, 1, &cmd);
	b.fn.vkDestroyCommandPool(b.dev, tpool, nullptr);
	if (!ok) {
		return false;
	}
	if (b.fn.vkMapMemory(b.dev, c.back.m, 0, fb * c.gen, 0, &mapp) !=
	    VK_SUCCESS) {
		return false;
	}
	auto* src = static_cast<const uint8_t*>(mapp);
	for (int g = 0; g < c.gen; ++g) {
		std::vector<uint32_t> frame(static_cast<size_t>(w) * h);
		for (int i = 0; i < w * h; ++i) {
			const uint8_t* px = src + static_cast<size_t>(g) * fb + 4 * i;
			frame[i] = static_cast<uint32_t>(px[2]) |
			           (static_cast<uint32_t>(px[1]) << 8) |
			           (static_cast<uint32_t>(px[0]) << 16) | (0xffu << 24);
		}
		c.frames_done.push_back(std::move(frame));
	}
	b.fn.vkUnmapMemory(b.dev, c.back.m);
	c.fc++;
	return true;
}

// Drop every DLL-derived cache (both models + shared pipes/UBOs).
// Chains first (their sets/pools go with them), shared pipelines and
// UBOs after (pipelines before layouts). Samplers are DLL-independent
// and survive.
void drop_all_for_new_dll(const std::string& dir)
{
	auto& st = state();
	for (auto& [k, c] : st.chains) {
		destroy_chain(c);
	}
	for (auto& [k, c] : st.pchains) {
		destroy_pchain(c);
	}
	st.chains.clear();
	st.dead.clear();
	st.pchains.clear();
	st.pdead.clear();
	auto& b  = boot();
	auto& sh = shared();
	for (auto& [k, sp] : sh.pipes) {
		if (sp.pipe) {
			b.fn.vkDestroyPipeline(b.dev, sp.pipe, nullptr);
		}
		if (sp.pl) {
			b.fn.vkDestroyPipelineLayout(b.dev, sp.pl, nullptr);
		}
		if (sp.dsl) {
			b.fn.vkDestroyDescriptorSetLayout(b.dev, sp.dsl, nullptr);
		}
	}
	for (auto& [k, bf] : sh.ubos) {
		free_buf(bf);
	}
	sh.pipes.clear();
	sh.ubos.clear();
	sh.staged_dir = dir;
	st.staged_dir = dir;
}

PChain* find_or_build_p(int w, int h, int gen, bool fp32)
{
	auto& st = state();
	const std::string dir = LSFG_StagedDir();
	if (dir.empty()) {
		return nullptr;
	}
	if (st.staged_dir != dir) {
		drop_all_for_new_dll(dir);
	}
	const ChainKey key{w, h, gen, fp32};
	if (st.pdead.find(key) != st.pdead.end()) {
		return nullptr;
	}
	const auto hit = st.pchains.find(key);
	if (hit != st.pchains.end()) {
		return hit->second->broken ? nullptr : hit->second;
	}
	if (st.pchains.size() >= 6) {
		for (auto& [k, c] : st.pchains) {
			destroy_pchain(c);
		}
		st.pchains.clear();
		st.pdead.clear();
	}
	PChain* c = build_pchain(w, h, gen, fp32);
	if (c) {
		st.pchains[key] = c;
	} else {
		st.pdead.insert(key);
	}
	return c;
}

// __PCHAIN_API__

Chain* find_or_build(int w, int h, int gen, bool fp32)
{
	auto& st = state();
	const std::string dir = LSFG_StagedDir();
	if (dir.empty()) {
		return nullptr;
	}
	if (st.staged_dir != dir) {
		// New DLL generation: drop everything cached under the old one.
		drop_all_for_new_dll(dir);
	}
	const ChainKey key{w, h, gen, fp32};
	if (st.dead.find(key) != st.dead.end()) {
		return nullptr;
	}
	const auto hit = st.chains.find(key);
	if (hit != st.chains.end()) {
		return hit->second->broken ? nullptr : hit->second;
	}
	// Up to 6 cached sizes: boot mode churn must not evict the steady
	// state (each chain build compiles 26 pipelines once).
	if (st.chains.size() >= 6) {
		for (auto& [k, c] : st.chains) {
			destroy_chain(c);
		}
		st.chains.clear();
		st.dead.clear();
	}
	Chain* c = build_chain(w, h, gen, fp32);
	if (c) {
		st.chains[key] = c;
	} else {
		st.dead.insert(key);
	}
	return c;
}

bool vk_generate(const uint32_t* prev_px, const uint32_t* curr_px, int w_px,
                 int h_px, int gen_count,
                 std::vector<std::vector<uint32_t>>& frames_out)
{
	if (!prev_px || !curr_px || w_px <= 0 || h_px <= 0 || gen_count < 1 ||
	    gen_count > 7) {
		return false;
	}
	if (!boot_init()) {
		return false;
	}
	auto& st = state();
	if (st.perf_model) {
		PChain* c = find_or_build_p(w_px, h_px, gen_count, st.prefer_fp32);
		if (!c) {
			return false;
		}
		if (!run_pchain(*c, prev_px, curr_px)) {
			LOG_ERR("LSFGVK: GPU perf run failed (%dx%d x%d); not "
			        "retrying until settings change, CPU blend meanwhile",
			        w_px, h_px, gen_count);
			st.pdead.insert(
			        ChainKey{w_px, h_px, gen_count, st.prefer_fp32});
			for (auto it = st.pchains.begin(); it != st.pchains.end();
			     ++it) {
				if (it->second == c) {
					st.pchains.erase(it);
					break;
				}
			}
			destroy_pchain(c);
			return false;
		}
		frames_out = std::move(c->frames_done);
		return frames_out.size() == static_cast<size_t>(gen_count);
	}
	Chain* c = find_or_build(w_px, h_px, gen_count, st.prefer_fp32);
	if (!c) {
		return false;
	}
	if (!run_chain(*c, prev_px, curr_px)) {
		LOG_ERR("LSFGVK: GPU run failed (%dx%d x%d); not retrying until "
		        "settings change, CPU blend meanwhile",
		        w_px, h_px, gen_count);
		st.dead.insert(ChainKey{w_px, h_px, gen_count, st.prefer_fp32});
		for (auto it = st.chains.begin(); it != st.chains.end(); ++it) {
			if (it->second == c) {
				st.chains.erase(it);
				break;
			}
		}
		destroy_chain(c);
		return false;
	}
	frames_out = std::move(c->frames_done);
	return frames_out.size() == static_cast<size_t>(gen_count);
}

bool vk_is_up()
{
	return boot_init() && boot().ready && !boot().down;
}

void vk_set_fp32(bool prefer)
{
	state().prefer_fp32 = prefer;
}

void vk_set_perf(bool perf)
{
	state().perf_model = perf;
}

void vk_set_flow_scale(float scale)
{
	if (!(scale > 0.0f)) {
		scale = 1.0f;
	}
	if (scale == g_flow_scale) {
		return;
	}
	g_flow_scale = scale;
	// The flow extent and the UBO resolutionInvScale are baked into the
	// chains and into the cached uniform buffers, so drop both; they
	// rebuild lazily at the new scale on the next generate.
	auto& st = state();
	for (auto& [k, c] : st.chains) {
		destroy_chain(c);
	}
	st.chains.clear();
	st.dead.clear();
	for (auto& [k, c] : st.pchains) {
		destroy_pchain(c);
	}
	st.pchains.clear();
	st.pdead.clear();
	auto& sh = shared();
	for (auto& [k, bf] : sh.ubos) {
		free_buf(bf);
	}
	sh.ubos.clear();
}

// __CHAIN_API__

} // namespace

#endif // C_XR_VULKAN

bool LSFGVK_Generate(const uint32_t* prev_px, const uint32_t* curr_px,
                     int w_px, int h_px, int gen_count,
                     std::vector<std::vector<uint32_t>>& frames_out)
{
#if !C_XR_VULKAN
	(void)prev_px;
	(void)curr_px;
	(void)w_px;
	(void)h_px;
	(void)gen_count;
	(void)frames_out;
	return false;
#else
	return vk_generate(prev_px, curr_px, w_px, h_px, gen_count, frames_out);
#endif
}

bool LSFGVK_IsUp()
{
#if !C_XR_VULKAN
	return false;
#else
	return vk_is_up();
#endif
}

void LSFGVK_SetPreferFp32(bool prefer)
{
#if C_XR_VULKAN
	vk_set_fp32(prefer);
#else
	(void)prefer;
#endif
}

void LSFGVK_SetPerfModel(bool perf)
{
#if C_XR_VULKAN
	vk_set_perf(perf);
#else
	(void)perf;
#endif
}

void LSFGVK_SetFlowScale(int percent)
{
#if C_XR_VULKAN
	const int pct = percent < 25 ? 25 : (percent > 100 ? 100 : percent);
	vk_set_flow_scale(static_cast<float>(pct) / 100.0f);
#else
	(void)percent;
#endif
}
