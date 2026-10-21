// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Raw Vulkan device for the OpenXR Vulkan graphics binding plus a
// transfer-only compositor. See xrvulkan.h for the contract.

#include "xrvulkan.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "dosbox.h"

#if C_XR_VULKAN

#ifndef _WIN32
#include <dlfcn.h>
#endif

// ---------------------------------------------------------------------------
// Dynamic loader + function table (Vulkan 1.0 core only, no WSI).
// ---------------------------------------------------------------------------

#ifdef _WIN32
static HMODULE vk_lib = nullptr;
#else
static void* vk_lib = nullptr;
#endif

struct VkFns {
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr           = nullptr;
	PFN_vkCreateInstance CreateInstance                     = nullptr;
	PFN_vkDestroyInstance DestroyInstance                   = nullptr;
	PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties
	        GetPhysicalDeviceQueueFamilyProperties          = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties
	        GetPhysicalDeviceMemoryProperties               = nullptr;
	PFN_vkGetPhysicalDeviceFormatProperties
	        GetPhysicalDeviceFormatProperties               = nullptr;
	PFN_vkCreateDevice CreateDevice                         = nullptr;
	PFN_vkDestroyDevice DestroyDevice                       = nullptr;
	PFN_vkGetDeviceQueue GetDeviceQueue                     = nullptr;
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr               = nullptr;
	// Device-level (loaded after vkCreateDevice).
	PFN_vkCreateBuffer CreateBuffer                         = nullptr;
	PFN_vkDestroyBuffer DestroyBuffer                       = nullptr;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
	PFN_vkAllocateMemory AllocateMemory                     = nullptr;
	PFN_vkFreeMemory FreeMemory                             = nullptr;
	PFN_vkBindBufferMemory BindBufferMemory                 = nullptr;
	PFN_vkBindImageMemory BindImageMemory                   = nullptr;
	PFN_vkMapMemory MapMemory                               = nullptr;
	PFN_vkUnmapMemory UnmapMemory                           = nullptr;
	PFN_vkCreateImage CreateImage                           = nullptr;
	PFN_vkDestroyImage DestroyImage                         = nullptr;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
	PFN_vkCreateCommandPool CreateCommandPool               = nullptr;
	PFN_vkDestroyCommandPool DestroyCommandPool             = nullptr;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers     = nullptr;
	PFN_vkBeginCommandBuffer BeginCommandBuffer             = nullptr;
	PFN_vkEndCommandBuffer EndCommandBuffer                 = nullptr;
	PFN_vkResetCommandBuffer ResetCommandBuffer             = nullptr;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier             = nullptr;
	PFN_vkCmdClearColorImage CmdClearColorImage             = nullptr;
	PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage         = nullptr;
	PFN_vkCmdBlitImage CmdBlitImage                         = nullptr;
	PFN_vkCreateFence CreateFence                           = nullptr;
	PFN_vkDestroyFence DestroyFence                         = nullptr;
	PFN_vkResetFences ResetFences                           = nullptr;
	PFN_vkWaitForFences WaitForFences                       = nullptr;
	PFN_vkQueueSubmit QueueSubmit                           = nullptr;
};

static VkFns vk = {};

// Warning-free loader lookup (FARPROC/void* to function pointer via
// memcpy; plain reinterpret_cast trips -Wcast-function-type on
// clang-cl for the Win32 FARPROC case).
template <typename Fn, typename Lib>
static Fn load_proc(Lib lib, const char* name)
{
	Fn fn = nullptr;
#ifdef _WIN32
	FARPROC p = GetProcAddress(lib, name);
#else
	void* p = dlsym(lib, name);
	(void)lib;
#endif
	static_assert(sizeof(fn) == sizeof(p));
	if (p) {
		std::memcpy(&fn, &p, sizeof(fn));
	}
	return fn;
}

static bool xrvk_load_bootstraps()
{
	if (vk.GetInstanceProcAddr && vk.CreateInstance) {
		return true;
	}
#ifdef _WIN32
	if (!vk_lib) {
		vk_lib = LoadLibraryA("vulkan-1.dll");
	}
	if (!vk_lib) {
		return false;
	}
#else
	if (!vk_lib) {
		vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	}
	if (!vk_lib) {
		return false;
	}
#endif
	vk.GetInstanceProcAddr = load_proc<PFN_vkGetInstanceProcAddr>(
	        vk_lib, "vkGetInstanceProcAddr");
	if (!vk.GetInstanceProcAddr) {
		return false;
	}
	vk.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
	        vk.GetInstanceProcAddr(nullptr, "vkCreateInstance"));
	return vk.CreateInstance != nullptr;
}

static bool loader_probed = false;
static bool loader_present = false;

bool xrvk_have_loader()
{
	if (!loader_probed) {
		loader_probed  = true;
		loader_present = xrvk_load_bootstraps();
	}
	return loader_present;
}

bool xrvk_available()
{
	if (!xrvk_have_loader()) {
		LOG_MSG("OpenXR: No Vulkan loader found; headset needs a Vulkan "
		        "driver install (output=opengl still works)");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Device state.
// ---------------------------------------------------------------------------

static VkInstance       vk_instance  = VK_NULL_HANDLE;
static VkPhysicalDevice vk_phys      = VK_NULL_HANDLE;
static VkDevice         vk_device    = VK_NULL_HANDLE;
static VkQueue          vk_queue     = VK_NULL_HANDLE;
static uint32_t         vk_queue_family = 0;

static VkCommandPool   vk_cmd_pool = VK_NULL_HANDLE;
static VkCommandBuffer vk_cmd      = VK_NULL_HANDLE;
static VkFence         vk_fence    = VK_NULL_HANDLE;

// Growable host-visible staging buffer (transfer source).
static VkBuffer       vk_stage_buf  = VK_NULL_HANDLE;
static VkDeviceMemory vk_stage_mem  = VK_NULL_HANDLE;
static VkDeviceSize   vk_stage_size = 0;

// Reusable staging image (blit source; RGBA8 or BGRA8 to match the
// swapchain component order, which vkCmdBlitImage requires).
static VkImage        vk_stage_img     = VK_NULL_HANDLE;
static VkDeviceMemory vk_stage_img_mem = VK_NULL_HANDLE;
static int            vk_stage_w       = 0;
static int            vk_stage_h       = 0;
static bool           vk_stage_is_bgra = false;
static VkImageLayout  vk_stage_layout  = VK_IMAGE_LAYOUT_UNDEFINED;

static VkFormat vk_swap_format = VK_FORMAT_UNDEFINED;
static bool     vk_swap_is_bgra = false;
static bool     vk_swap_blit_ok = false;

// Current layout of each swapchain image we touched (starts
// UNDEFINED; we always leave COLOR_ATTACHMENT_OPTIMAL).
static std::unordered_map<uint64_t, VkImageLayout> vk_img_layouts;

bool xrvk_is_ready()
{
	return vk_device != VK_NULL_HANDLE;
}

static bool find_memory_type(const uint32_t bits,
                             const VkMemoryPropertyFlags want,
                             uint32_t& idx_out)
{
	VkPhysicalDeviceMemoryProperties props = {};
	vk.GetPhysicalDeviceMemoryProperties(vk_phys, &props);
	// Exact match first, then any superset.
	for (uint32_t pass = 0; pass < 2; ++pass) {
		for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
			if (!(bits & (1u << i))) {
				continue;
			}
			const auto have = props.memoryTypes[i].propertyFlags;
			if ((have & want) != want) {
				continue;
			}
			if (pass == 0 && have != want) {
				continue;
			}
			idx_out = i;
			return true;
		}
	}
	return false;
}

static void xrvk_destroy_device_objects()
{
	if (vk_device == VK_NULL_HANDLE) {
		return;
	}
	if (vk_stage_img != VK_NULL_HANDLE) {
		vk.DestroyImage(vk_device, vk_stage_img, nullptr);
		vk_stage_img = VK_NULL_HANDLE;
	}
	if (vk_stage_img_mem != VK_NULL_HANDLE) {
		vk.FreeMemory(vk_device, vk_stage_img_mem, nullptr);
		vk_stage_img_mem = VK_NULL_HANDLE;
	}
	vk_stage_w = vk_stage_h = 0;
	vk_stage_is_bgra         = false;
	vk_stage_layout          = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vk_stage_buf != VK_NULL_HANDLE) {
		vk.DestroyBuffer(vk_device, vk_stage_buf, nullptr);
		vk_stage_buf = VK_NULL_HANDLE;
	}
	if (vk_stage_mem != VK_NULL_HANDLE) {
		vk.FreeMemory(vk_device, vk_stage_mem, nullptr);
		vk_stage_mem = VK_NULL_HANDLE;
	}
	vk_stage_size = 0;
	if (vk_fence != VK_NULL_HANDLE) {
		vk.DestroyFence(vk_device, vk_fence, nullptr);
		vk_fence = VK_NULL_HANDLE;
	}
	if (vk_cmd_pool != VK_NULL_HANDLE) {
		vk.DestroyCommandPool(vk_device, vk_cmd_pool, nullptr);
		vk_cmd_pool = VK_NULL_HANDLE;
		vk_cmd      = VK_NULL_HANDLE;
	}
	vk.DestroyDevice(vk_device, nullptr);
	vk_device = VK_NULL_HANDLE;
	vk_queue  = VK_NULL_HANDLE;
}

void xrvk_shutdown()
{
	xrvk_destroy_device_objects();
	if (vk_instance != VK_NULL_HANDLE) {
		vk.DestroyInstance(vk_instance, nullptr);
		vk_instance = VK_NULL_HANDLE;
	}
	vk_phys = VK_NULL_HANDLE;
	vk_swap_format  = VK_FORMAT_UNDEFINED;
	vk_swap_is_bgra = false;
	vk_swap_blit_ok = false;
	vk_img_layouts.clear();
}

// Split a space-separated extension string (as the XR Vulkan queries
// return) into C string pointers (storage owned by the input).
static std::vector<const char*> split_extensions(std::string& list)
{
	std::vector<const char*> out;
	size_t pos = 0;
	while (pos < list.size() && list[pos] != '\0') {
		while (pos < list.size() && list[pos] == ' ') {
			++pos;
		}
		if (pos >= list.size() || list[pos] == '\0') {
			break;
		}
		size_t end = pos;
		while (end < list.size() && list[end] != ' ' &&
		       list[end] != '\0') {
			++end;
		}
		if (end < list.size()) {
			list[end] = '\0';
		}
		out.push_back(list.data() + pos);
		pos = end + 1;
	}
	return out;
}

bool xrvk_create(XrInstance xr_instance, XrSystemId xr_system,
                 XrGraphicsBindingVulkanKHR* binding_out)
{
	if (xrvk_is_ready()) {
		return true;
	}
	if (!binding_out || xr_instance == XR_NULL_HANDLE ||
	    xr_system == XR_NULL_SYSTEM_ID) {
		return false;
	}
	if (!xrvk_available()) {
		return false;
	}

	// XR Vulkan entry points (same instance-proc-addr pattern as the
	// OpenGL requirements query).
	PFN_xrGetVulkanGraphicsRequirementsKHR pfnReqs = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR pfnInstExts = nullptr;
	PFN_xrGetVulkanGraphicsDeviceKHR pfnGfxDev       = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR pfnDevExts   = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	        xr_instance,
	        "xrGetVulkanGraphicsRequirementsKHR",
	        reinterpret_cast<PFN_xrVoidFunction*>(&pfnReqs));
	if (res != XR_SUCCESS || !pfnReqs) {
		LOG_ERR("OpenXR: xrGetVulkanGraphicsRequirementsKHR unavailable: %d",
		        res);
		return false;
	}
	res = xrGetInstanceProcAddr(
	        xr_instance,
	        "xrGetVulkanInstanceExtensionsKHR",
	        reinterpret_cast<PFN_xrVoidFunction*>(&pfnInstExts));
	if (res != XR_SUCCESS || !pfnInstExts) {
		LOG_ERR("OpenXR: xrGetVulkanInstanceExtensionsKHR unavailable: %d",
		        res);
		return false;
	}
	res = xrGetInstanceProcAddr(
	        xr_instance,
	        "xrGetVulkanGraphicsDeviceKHR",
	        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGfxDev));
	if (res != XR_SUCCESS || !pfnGfxDev) {
		LOG_ERR("OpenXR: xrGetVulkanGraphicsDeviceKHR unavailable: %d",
		        res);
		return false;
	}
	res = xrGetInstanceProcAddr(
	        xr_instance,
	        "xrGetVulkanDeviceExtensionsKHR",
	        reinterpret_cast<PFN_xrVoidFunction*>(&pfnDevExts));
	if (res != XR_SUCCESS || !pfnDevExts) {
		LOG_ERR("OpenXR: xrGetVulkanDeviceExtensionsKHR unavailable: %d",
		        res);
		return false;
	}

	// API version window mandated by the runtime.
	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = pfnReqs(xr_instance, xr_system, &reqs);
	if (res != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetVulkanGraphicsRequirementsKHR failed: %d",
		        res);
		return false;
	}
	LOG_MSG("OpenXR: Vulkan requirements min %u.%u, max %u.%u",
	        XR_VERSION_MAJOR(reqs.minApiVersionSupported),
	        XR_VERSION_MINOR(reqs.minApiVersionSupported),
	        XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
	        XR_VERSION_MINOR(reqs.maxApiVersionSupported));

	// Loader API version (vkEnumerateInstanceVersion is 1.1+).
	auto pfnEnumVer = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
	        vk.GetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
	uint32_t loader_ver = VK_API_VERSION_1_0;
	if (pfnEnumVer && pfnEnumVer(&loader_ver) != VK_SUCCESS) {
		loader_ver = VK_API_VERSION_1_0;
	}
	// The requirements are XrVersion-encoded (major << 48 |
	// minor << 32 | patch); convert them to Vulkan's packed
	// VK_MAKE_VERSION space before comparing with the loader.
	const auto to_vk_version = [](const XrVersion v) {
		return static_cast<uint64_t>(VK_MAKE_VERSION(
		        XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v),
		        XR_VERSION_PATCH(v)));
	};
	const auto max_ver = to_vk_version(reqs.maxApiVersionSupported);
	const auto min_ver = to_vk_version(reqs.minApiVersionSupported);
	uint32_t want_ver = max_ver > loader_ver
	        ? loader_ver
	        : static_cast<uint32_t>(max_ver);
	if (static_cast<uint64_t>(want_ver) < min_ver) {
		LOG_ERR("OpenXR: Vulkan loader too old for the runtime");
		return false;
	}

	// Instance extensions mandated by the runtime (no surface/WSI:
	// swapchain images come from OpenXR, not from us).
	uint32_t inst_len = 0;
	res = pfnInstExts(xr_instance, xr_system, 0, &inst_len, nullptr);
	if (res != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetVulkanInstanceExtensionsKHR count failed: %d",
		        res);
		return false;
	}
	std::string inst_list(inst_len, '\0');
	res = pfnInstExts(xr_instance, xr_system, inst_len, &inst_len,
	                  inst_list.data());
	if (res != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetVulkanInstanceExtensionsKHR failed: %d",
		        res);
		return false;
	}
	auto inst_exts = split_extensions(inst_list);

	VkApplicationInfo app = {};
	app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName   = "DOSBox Staging";
	app.applicationVersion = VK_MAKE_VERSION(0, 84, 0);
	app.pEngineName        = "DOSBox Staging";
	app.engineVersion      = VK_MAKE_VERSION(0, 84, 0);
	app.apiVersion         = want_ver;
	VkInstanceCreateInfo ici = {};
	ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo        = &app;
	ici.enabledExtensionCount   = static_cast<uint32_t>(inst_exts.size());
	ici.ppEnabledExtensionNames = inst_exts.data();
	if (vk.CreateInstance(&ici, nullptr, &vk_instance) != VK_SUCCESS) {
		LOG_ERR("OpenXR: vkCreateInstance failed");
		vk_instance = VK_NULL_HANDLE;
		return false;
	}

	// Remaining instance-level entry points.
#define XRVK_LOAD_INST(name)                                                   \
	vk.name = reinterpret_cast<PFN_vk##name>(                              \
	        vk.GetInstanceProcAddr(vk_instance, "vk" #name));               \
	if (!vk.name) {                                                        \
		LOG_ERR("OpenXR: vk" #name " missing");                        \
		xrvk_shutdown();                                               \
		return false;                                                  \
	}
	XRVK_LOAD_INST(DestroyInstance)
	XRVK_LOAD_INST(EnumeratePhysicalDevices)
	XRVK_LOAD_INST(GetPhysicalDeviceProperties)
	XRVK_LOAD_INST(GetPhysicalDeviceQueueFamilyProperties)
	XRVK_LOAD_INST(GetPhysicalDeviceMemoryProperties)
	XRVK_LOAD_INST(GetPhysicalDeviceFormatProperties)
	XRVK_LOAD_INST(CreateDevice)
	XRVK_LOAD_INST(DestroyDevice)
	XRVK_LOAD_INST(GetDeviceQueue)
	XRVK_LOAD_INST(GetDeviceProcAddr)
#undef XRVK_LOAD_INST

	// The runtime picks the physical device (multi-GPU safe).
	res = pfnGfxDev(xr_instance, xr_system, vk_instance, &vk_phys);
	if (res != XR_SUCCESS || vk_phys == VK_NULL_HANDLE) {
		LOG_ERR("OpenXR: xrGetVulkanGraphicsDeviceKHR failed: %d", res);
		xrvk_shutdown();
		return false;
	}
	VkPhysicalDeviceProperties props = {};
	vk.GetPhysicalDeviceProperties(vk_phys, &props);
	LOG_MSG("OpenXR: Vulkan device '%s' (Vulkan %u.%u)", props.deviceName,
	        VK_VERSION_MAJOR(props.apiVersion),
	        VK_VERSION_MINOR(props.apiVersion));

	// First graphics-capable queue family carries everything (graphics
	// queues always support transfer).
	uint32_t fam_count = 0;
	vk.GetPhysicalDeviceQueueFamilyProperties(vk_phys, &fam_count, nullptr);
	std::vector<VkQueueFamilyProperties> fams(fam_count);
	vk.GetPhysicalDeviceQueueFamilyProperties(vk_phys, &fam_count,
	                                          fams.data());
	bool have_gfx = false;
	for (uint32_t i = 0; i < fam_count; ++i) {
		if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			vk_queue_family = i;
			have_gfx        = true;
			break;
		}
	}
	if (!have_gfx) {
		LOG_ERR("OpenXR: Vulkan device has no graphics queue");
		xrvk_shutdown();
		return false;
	}

	uint32_t dev_len = 0;
	res = pfnDevExts(xr_instance, xr_system, 0, &dev_len, nullptr);
	if (res != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetVulkanDeviceExtensionsKHR count failed: %d",
		        res);
		xrvk_shutdown();
		return false;
	}
	std::string dev_list(dev_len, '\0');
	res = pfnDevExts(xr_instance, xr_system, dev_len, &dev_len,
	                 dev_list.data());
	if (res != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetVulkanDeviceExtensionsKHR failed: %d",
		        res);
		xrvk_shutdown();
		return false;
	}
	auto dev_exts = split_extensions(dev_list);

	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = vk_queue_family;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {};
	dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount    = 1;
	dci.pQueueCreateInfos       = &qci;
	dci.enabledExtensionCount   = static_cast<uint32_t>(dev_exts.size());
	dci.ppEnabledExtensionNames = dev_exts.data();
	if (vk.CreateDevice(vk_phys, &dci, nullptr, &vk_device) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: vkCreateDevice failed");
		vk_device = VK_NULL_HANDLE;
		xrvk_shutdown();
		return false;
	}
	vk.GetDeviceQueue(vk_device, vk_queue_family, 0, &vk_queue);

// Device-level entry points.
#define XRKV_LOAD_DEV(name)                                                    \
	vk.name = reinterpret_cast<PFN_vk##name>(                              \
	        vk.GetDeviceProcAddr(vk_device, "vk" #name));                   \
	if (!vk.name) {                                                        \
		LOG_ERR("OpenXR: vk" #name " missing");                        \
		xrvk_shutdown();                                               \
		return false;                                                  \
	}
	XRKV_LOAD_DEV(CreateBuffer)
	XRKV_LOAD_DEV(DestroyBuffer)
	XRKV_LOAD_DEV(GetBufferMemoryRequirements)
	XRKV_LOAD_DEV(AllocateMemory)
	XRKV_LOAD_DEV(FreeMemory)
	XRKV_LOAD_DEV(BindBufferMemory)
	XRKV_LOAD_DEV(BindImageMemory)
	XRKV_LOAD_DEV(MapMemory)
	XRKV_LOAD_DEV(UnmapMemory)
	XRKV_LOAD_DEV(CreateImage)
	XRKV_LOAD_DEV(DestroyImage)
	XRKV_LOAD_DEV(GetImageMemoryRequirements)
	XRKV_LOAD_DEV(CreateCommandPool)
	XRKV_LOAD_DEV(DestroyCommandPool)
	XRKV_LOAD_DEV(AllocateCommandBuffers)
	XRKV_LOAD_DEV(BeginCommandBuffer)
	XRKV_LOAD_DEV(EndCommandBuffer)
	XRKV_LOAD_DEV(ResetCommandBuffer)
	XRKV_LOAD_DEV(CmdPipelineBarrier)
	XRKV_LOAD_DEV(CmdClearColorImage)
	XRKV_LOAD_DEV(CmdCopyBufferToImage)
	XRKV_LOAD_DEV(CmdBlitImage)
	XRKV_LOAD_DEV(CreateFence)
	XRKV_LOAD_DEV(DestroyFence)
	XRKV_LOAD_DEV(ResetFences)
	XRKV_LOAD_DEV(WaitForFences)
	XRKV_LOAD_DEV(QueueSubmit)
#undef XRKV_LOAD_DEV

	VkCommandPoolCreateInfo pci = {};
	pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = vk_queue_family;
	if (vk.CreateCommandPool(vk_device, &pci, nullptr, &vk_cmd_pool) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: vkCreateCommandPool failed");
		xrvk_shutdown();
		return false;
	}
	VkCommandBufferAllocateInfo cai = {};
	cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cai.commandPool        = vk_cmd_pool;
	cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	if (vk.AllocateCommandBuffers(vk_device, &cai, &vk_cmd) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: vkAllocateCommandBuffers failed");
		xrvk_shutdown();
		return false;
	}
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vk.CreateFence(vk_device, &fci, nullptr, &vk_fence) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: vkCreateFence failed");
		xrvk_shutdown();
		return false;
	}

	binding_out->type             = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	binding_out->next             = nullptr;
	binding_out->instance         = vk_instance;
	binding_out->physicalDevice   = vk_phys;
	binding_out->device           = vk_device;
	binding_out->queueFamilyIndex = vk_queue_family;
	binding_out->queueIndex       = 0;
	LOG_MSG("OpenXR: Vulkan binding ready (queue family %u)",
	        vk_queue_family);
	return true;
}

int64_t xrvk_pick_format(const int64_t* formats, const uint32_t count)
{
	vk_swap_format  = VK_FORMAT_UNDEFINED;
	vk_swap_is_bgra = false;
	vk_swap_blit_ok = false;
	if (!formats || count == 0) {
		return 0;
	}
	// Prefer sRGB RGBA8 (matches the GL path's SRGB8_ALPHA8 pick);
	// BGR variants only need an R/B lane swap on upload.
	static constexpr int64_t prefs[] = {
	        static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB),
	        static_cast<int64_t>(VK_FORMAT_B8G8R8A8_SRGB),
	        static_cast<int64_t>(VK_FORMAT_R8G8B8A8_UNORM),
	        static_cast<int64_t>(VK_FORMAT_B8G8R8A8_UNORM)};
	int64_t picked = formats[0];
	for (const auto want : prefs) {
		for (uint32_t i = 0; i < count; ++i) {
			if (formats[i] == want) {
				picked = want;
				break;
			}
		}
		if (picked == want) {
			break;
		}
	}
	vk_swap_format = static_cast<VkFormat>(picked);
	vk_swap_is_bgra =
	        (vk_swap_format == VK_FORMAT_B8G8R8A8_SRGB ||
	         vk_swap_format == VK_FORMAT_B8G8R8A8_UNORM);
	if (xrvk_is_ready()) {
		VkFormatProperties fp = {};
		vk.GetPhysicalDeviceFormatProperties(vk_phys, vk_swap_format,
		                                     &fp);
		vk_swap_blit_ok =
		        (fp.optimalTilingFeatures &
		         VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
	}
	LOG_MSG("OpenXR: Vulkan swapchain format %lld%s%s",
	        static_cast<long long>(picked),
	        vk_swap_is_bgra ? " (BGRA, R/B swapped on upload)" : "",
	        vk_swap_blit_ok ? "" : " (no blit-dst: CPU scaling)");
	return picked;
}

bool xrvk_enumerate(XrSwapchain swapchain, const char* label,
                    std::vector<XrGfxImage>& out)
{
	out.clear();
	if (!xrvk_is_ready() || swapchain == XR_NULL_HANDLE) {
		return false;
	}
	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr) !=
	            XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate %s swapchain images", label);
		return false;
	}
	std::vector<XrSwapchainImageVulkanKHR> tmp(image_count);
	for (auto& img : tmp) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
		img.next = nullptr;
	}
	if (xrEnumerateSwapchainImages(
	            swapchain,
	            image_count,
	            &image_count,
	            reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                    tmp.data())) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate %s swapchain images", label);
		return false;
	}
	out.resize(image_count);
	for (uint32_t i = 0; i < image_count; ++i) {
		out[i].type   = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
		out[i].handle = reinterpret_cast<uint64_t>(tmp[i].image);
		vk_img_layouts[out[i].handle] = VK_IMAGE_LAYOUT_UNDEFINED;
	}
	LOG_MSG("OpenXR: %s swapchain ready (%u VkImages)", label, image_count);
	return true;
}

// ---------------------------------------------------------------------------
// Staging + command submission.
// ---------------------------------------------------------------------------

static bool ensure_staging_buffer(const VkDeviceSize bytes)
{
	if (bytes <= vk_stage_size) {
		return true;
	}
	if (vk_stage_buf != VK_NULL_HANDLE) {
		vk.DestroyBuffer(vk_device, vk_stage_buf, nullptr);
		vk_stage_buf = VK_NULL_HANDLE;
	}
	if (vk_stage_mem != VK_NULL_HANDLE) {
		vk.FreeMemory(vk_device, vk_stage_mem, nullptr);
		vk_stage_mem = VK_NULL_HANDLE;
	}
	vk_stage_size = 0;
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size  = bytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	if (vk.CreateBuffer(vk_device, &bci, nullptr, &vk_stage_buf) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging buffer create failed");
		return false;
	}
	VkMemoryRequirements mr = {};
	vk.GetBufferMemoryRequirements(vk_device, vk_stage_buf, &mr);
	uint32_t idx = 0;
	if (!find_memory_type(mr.memoryTypeBits,
	                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                      idx)) {
		LOG_ERR("OpenXR: No host-visible Vulkan memory");
		return false;
	}
	VkMemoryAllocateInfo mai = {};
	mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize  = mr.size;
	mai.memoryTypeIndex = idx;
	if (vk.AllocateMemory(vk_device, &mai, nullptr, &vk_stage_mem) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging memory alloc failed");
		return false;
	}
	if (vk.BindBufferMemory(vk_device, vk_stage_buf, vk_stage_mem, 0) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging buffer bind failed");
		return false;
	}
	vk_stage_size = mr.size;
	return true;
}

static bool ensure_staging_image(const int w_px, const int h_px)
{
	if (w_px == vk_stage_w && h_px == vk_stage_h &&
	    vk_stage_is_bgra == vk_swap_is_bgra &&
	    vk_stage_img != VK_NULL_HANDLE) {
		return true;
	}
	if (vk_stage_img != VK_NULL_HANDLE) {
		vk.DestroyImage(vk_device, vk_stage_img, nullptr);
		vk_stage_img = VK_NULL_HANDLE;
	}
	if (vk_stage_img_mem != VK_NULL_HANDLE) {
		vk.FreeMemory(vk_device, vk_stage_img_mem, nullptr);
		vk_stage_img_mem = VK_NULL_HANDLE;
	}
	vk_stage_w = vk_stage_h = 0;
	VkImageCreateInfo ici = {};
	ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType     = VK_IMAGE_TYPE_2D;
	ici.format        = vk_swap_is_bgra ? VK_FORMAT_B8G8R8A8_UNORM
	                                    : VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent        = {static_cast<uint32_t>(w_px),
	                     static_cast<uint32_t>(h_px), 1};
	ici.mipLevels     = 1;
	ici.arrayLayers   = 1;
	ici.samples       = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
	ici.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vk.CreateImage(vk_device, &ici, nullptr, &vk_stage_img) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging image create failed");
		return false;
	}
	VkMemoryRequirements mr = {};
	vk.GetImageMemoryRequirements(vk_device, vk_stage_img, &mr);
	uint32_t idx = 0;
	if (!find_memory_type(mr.memoryTypeBits,
	                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                      idx) &&
	    !find_memory_type(mr.memoryTypeBits, 0, idx)) {
		LOG_ERR("OpenXR: No Vulkan memory for staging image");
		return false;
	}
	VkMemoryAllocateInfo mai = {};
	mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize  = mr.size;
	mai.memoryTypeIndex = idx;
	if (vk.AllocateMemory(vk_device, &mai, nullptr,
	                      &vk_stage_img_mem) != VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging image memory alloc failed");
		return false;
	}
	if (vk.BindImageMemory(vk_device, vk_stage_img, vk_stage_img_mem,
	                       0) != VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging image bind failed");
		return false;
	}
	vk_stage_w       = w_px;
	vk_stage_h       = h_px;
	vk_stage_is_bgra = vk_swap_is_bgra;
	vk_stage_layout  = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

static void vk_barrier(VkImage img, const int layer,
                       const VkImageLayout old_layout,
                       const VkImageLayout new_layout,
                       const VkAccessFlags src_access,
                       const VkAccessFlags dst_access,
                       const VkPipelineStageFlags src_stage,
                       const VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {};
	b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask       = src_access;
	b.dstAccessMask       = dst_access;
	b.oldLayout           = old_layout;
	b.newLayout           = new_layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image               = img;
	b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
	                         static_cast<uint32_t>(layer), 1};
	vk.CmdPipelineBarrier(vk_cmd, src_stage, dst_stage, 0, 0, nullptr, 0,
	                      nullptr, 1, &b);
}

static VkImageLayout tracked_layout(const uint64_t handle)
{
	const auto it = vk_img_layouts.find(handle);
	return (it != vk_img_layouts.end()) ? it->second
	                                    : VK_IMAGE_LAYOUT_UNDEFINED;
}

// Begin a one-shot command buffer (fence is idle on entry: every
// submit below waits before returning).
static bool begin_cmds()
{
	if (vk.ResetCommandBuffer(vk_cmd, 0) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	return vk.BeginCommandBuffer(vk_cmd, &bi) == VK_SUCCESS;
}

// Submit, then fence-wait so the image is safe to release (the
// runtime requires our queue work finished at release). Returns the
// submit status.
static bool submit_cmds()
{
	if (vk.EndCommandBuffer(vk_cmd) != VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan command end failed");
		return false;
	}
	VkSubmitInfo si  = {};
	si.sType         = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers    = &vk_cmd;
	if (vk.QueueSubmit(vk_queue, 1, &si, vk_fence) != VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan queue submit failed");
		return false;
	}
  static constexpr uint64_t FenceWaitNs = 100'000'000; // 100 ms
	if (vk.WaitForFences(vk_device, 1, &vk_fence, VK_TRUE, FenceWaitNs) !=
	    VK_SUCCESS) {
		// Wedged GPU: drop the whole device so bring-up recreates it
		// cleanly instead of reusing objects with pending work.
		LOG_ERR("OpenXR: Vulkan fence wait timed out");
		xrvk_shutdown();
		return false;
	}
	vk.ResetFences(vk_device, 1, &vk_fence);
	return true;
}

// Copy/convert bottom-up source rows into the mapped staging buffer
// as top-down RGBA (swap R/B when the source is BGRA-ordered or the
// swapchain wants BGRA).
static void fill_staging(uint8_t* dst, const uint32_t* src_bottom_up,
                         const int w_px, const int h_px,
                         const bool src_is_bgra, const bool dst_is_bgra)
{
	const bool swap = (src_is_bgra != dst_is_bgra);
	for (int y = 0; y < h_px; ++y) {
		const uint32_t* srow =
		        src_bottom_up +
		        static_cast<size_t>(h_px - 1 - y) * w_px;
		uint8_t* drow = dst + static_cast<size_t>(y) * w_px * 4;
		if (!swap) {
			std::memcpy(drow, srow,
			            static_cast<size_t>(w_px) * sizeof(uint32_t));
		} else {
			for (int x = 0; x < w_px; ++x) {
				const uint32_t p = srow[x];
				drow[4 * x + 0]   = static_cast<uint8_t>(p >> 16);
				drow[4 * x + 1]   = static_cast<uint8_t>(p >> 8);
				drow[4 * x + 2]   = static_cast<uint8_t>(p);
				drow[4 * x + 3]   = static_cast<uint8_t>(p >> 24);
			}
		}
	}
}

// Bilinear upscale of bottom-up BGRA source into a top-down dst
// buffer in dst byte order. Fallback for runtimes whose swapchain
// format lacks blit-dst support (effectively never on desktop GPUs).
static void bilinear_scale(std::vector<uint32_t>& dst, const int dw_px,
                           const int dh_px, const uint32_t* src_bottom_up,
                           const int sw_px, const int sh_px,
                           const bool dst_is_bgra)
{
	if (sw_px <= 0 || sh_px <= 0 || dw_px <= 0 || dh_px <= 0) {
		return;
	}
	dst.assign(static_cast<size_t>(dw_px) * dh_px, 0xFF000000u);
	const float sx = static_cast<float>(sw_px) / dw_px;
	const float sy = static_cast<float>(sh_px) / dh_px;
	for (int y = 0; y < dh_px; ++y) {
		// Source is bottom-up: sample mirrored so dst row 0 (top)
		// shows the source top.
		const float v  = (y + 0.5f) * sy - 0.5f;
		const float vf = (sh_px - 1.0f) - v;
		int y0         = static_cast<int>(vf);
		float fy       = vf - y0;
		if (y0 < 0) {
			y0 = 0;
			fy = 0.0f;
		}
		if (y0 >= sh_px - 1) {
			y0 = sh_px - 1;
			fy = 0.0f;
		}
		const int y1 = (y0 + 1 < sh_px) ? y0 + 1 : y0;
		for (int x = 0; x < dw_px; ++x) {
			const float u  = (x + 0.5f) * sx - 0.5f;
			int x0         = static_cast<int>(u);
			float fx       = u - x0;
			if (x0 < 0) {
				x0 = 0;
				fx = 0.0f;
			}
			if (x0 >= sw_px - 1) {
				x0 = sw_px - 1;
				fx = 0.0f;
			}
			const int x1 = (x0 + 1 < sw_px) ? x0 + 1 : x0;
			const uint32_t p00 = src_bottom_up[y0 * sw_px + x0];
			const uint32_t p10 = src_bottom_up[y0 * sw_px + x1];
			const uint32_t p01 = src_bottom_up[y1 * sw_px + x0];
			const uint32_t p11 = src_bottom_up[y1 * sw_px + x1];
			float r = 0.0f, g = 0.0f, b = 0.0f;
			const float w00 = (1 - fx) * (1 - fy);
			const float w10 = fx * (1 - fy);
			const float w01 = (1 - fx) * fy;
			const float w11 = fx * fy;
			// Source bytes are BGRA-ordered (B,G,R,A in memory).
			b += ((p00) & 0xFFu) * w00;
			g += ((p00 >> 8) & 0xFFu) * w00;
			r += ((p00 >> 16) & 0xFFu) * w00;
			b += ((p10) & 0xFFu) * w10;
			g += ((p10 >> 8) & 0xFFu) * w10;
			r += ((p10 >> 16) & 0xFFu) * w10;
			b += ((p01) & 0xFFu) * w01;
			g += ((p01 >> 8) & 0xFFu) * w01;
			r += ((p01 >> 16) & 0xFFu) * w01;
			b += ((p11) & 0xFFu) * w11;
			g += ((p11 >> 8) & 0xFFu) * w11;
			r += ((p11 >> 16) & 0xFFu) * w11;
			const auto R = static_cast<uint32_t>(r + 0.5f);
			const auto G = static_cast<uint32_t>(g + 0.5f);
			const auto B = static_cast<uint32_t>(b + 0.5f);
			dst[static_cast<size_t>(y) * dw_px + x] =
			        dst_is_bgra ? (0xFF000000u | (R << 16) | (G << 8) | B)
			                    : (0xFF000000u | (B << 16) | (G << 8) | R);
		}
	}
}

bool xrvk_upload_full(const uint64_t dst_handle, const int dst_layer,
                      const int w_px, const int h_px,
                      const uint32_t* rgba_bottom_up)
{
	if (!xrvk_is_ready() || !dst_handle || !rgba_bottom_up || w_px <= 0 ||
	    h_px <= 0 || vk_swap_format == VK_FORMAT_UNDEFINED) {
		return false;
	}
	const auto bytes = static_cast<VkDeviceSize>(w_px) * h_px * 4;
	if (!ensure_staging_buffer(bytes) || !begin_cmds()) {
		return false;
	}
	void* mapped = nullptr;
	if (vk.MapMemory(vk_device, vk_stage_mem, 0, bytes, 0, &mapped) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging map failed");
		return false;
	}
	// Rasters are RGBA-ordered; swap only for BGRA swapchains.
	fill_staging(static_cast<uint8_t*>(mapped), rgba_bottom_up, w_px,
	             h_px, false, vk_swap_is_bgra);
	vk.UnmapMemory(vk_device, vk_stage_mem);

	VkImage dst = reinterpret_cast<VkImage>(
	        static_cast<uintptr_t>(dst_handle));
	const auto cur = tracked_layout(dst_handle);
	if (cur == VK_IMAGE_LAYOUT_UNDEFINED) {
		vk_barrier(dst, dst_layer, cur,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else {
		vk_barrier(dst, dst_layer, cur,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	VkBufferImageCopy region = {};
	region.bufferOffset      = 0;
	region.bufferRowLength   = 0; // tightly packed
	region.bufferImageHeight = 0;
	region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
	                            static_cast<uint32_t>(dst_layer), 1};
	region.imageOffset       = {0, 0, 0};
	region.imageExtent       = {static_cast<uint32_t>(w_px),
	                            static_cast<uint32_t>(h_px), 1};
	vk.CmdCopyBufferToImage(vk_cmd, vk_stage_buf, dst,
	                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                        &region);
	vk_barrier(dst, dst_layer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	           VK_ACCESS_TRANSFER_WRITE_BIT, 0,
	           VK_PIPELINE_STAGE_TRANSFER_BIT,
	           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	if (!submit_cmds()) {
		return false;
	}
	vk_img_layouts[dst_handle] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	return true;
}

bool xrvk_upload_fitted(const uint64_t dst_handle, const int dst_layer,
                        const int eye_w_px, const int eye_h_px,
                        const int dx_px, const int dy_px_top,
                        const int dw_px, const int dh_px,
                        const uint32_t* bgra_bottom_up,
                        const int src_w_px, const int src_h_px)
{
	if (!xrvk_is_ready() || !dst_handle || !bgra_bottom_up ||
	    eye_w_px <= 0 || eye_h_px <= 0 || src_w_px <= 0 ||
	    src_h_px <= 0 || vk_swap_format == VK_FORMAT_UNDEFINED) {
		return false;
	}
	VkImage dst = reinterpret_cast<VkImage>(
	        static_cast<uintptr_t>(dst_handle));

	// No-blit fallback: CPU-scale to a full-eye buffer and copy it.
	if (!vk_swap_blit_ok) {
		std::vector<uint32_t> scaled;
		bilinear_scale(scaled, eye_w_px, eye_h_px, bgra_bottom_up,
		               src_w_px, src_h_px, vk_swap_is_bgra);
		if (!begin_cmds()) {
			return false;
		}
		const auto bytes =
		        static_cast<VkDeviceSize>(eye_w_px) * eye_h_px * 4;
		if (!ensure_staging_buffer(bytes)) {
			return false;
		}
		void* mapped = nullptr;
		if (vk.MapMemory(vk_device, vk_stage_mem, 0, bytes, 0,
		                 &mapped) != VK_SUCCESS) {
			return false;
		}
		std::memcpy(mapped, scaled.data(), static_cast<size_t>(bytes));
		vk.UnmapMemory(vk_device, vk_stage_mem);
		const auto cur = tracked_layout(dst_handle);
		vk_barrier(dst, dst_layer,
		           cur == VK_IMAGE_LAYOUT_UNDEFINED
		                   ? VK_IMAGE_LAYOUT_UNDEFINED
		                   : cur,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		           cur == VK_IMAGE_LAYOUT_UNDEFINED
		                   ? VkAccessFlags(0)
		                   : VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           cur == VK_IMAGE_LAYOUT_UNDEFINED
		                   ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
		                   : VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region = {};
		region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
		                            static_cast<uint32_t>(dst_layer),
		                            1};
		region.imageExtent       = {static_cast<uint32_t>(eye_w_px),
		                            static_cast<uint32_t>(eye_h_px), 1};
		vk.CmdCopyBufferToImage(vk_cmd, vk_stage_buf, dst,
		                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                        1, &region);
		vk_barrier(dst, dst_layer,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		           VK_ACCESS_TRANSFER_WRITE_BIT, 0,
		           VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		if (!submit_cmds()) {
			return false;
		}
		vk_img_layouts[dst_handle] =
		        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		return true;
	}

	const auto src_bytes =
	        static_cast<VkDeviceSize>(src_w_px) * src_h_px * 4;
	if (!ensure_staging_buffer(src_bytes) ||
	    !ensure_staging_image(src_w_px, src_h_px) || !begin_cmds()) {
		return false;
	}
	void* mapped = nullptr;
	if (vk.MapMemory(vk_device, vk_stage_mem, 0, src_bytes, 0, &mapped) !=
	    VK_SUCCESS) {
		LOG_ERR("OpenXR: Vulkan staging map failed");
		return false;
	}
	// Staging image matches the swapchain's component order.
	fill_staging(static_cast<uint8_t*>(mapped), bgra_bottom_up, src_w_px,
	             src_h_px, true, vk_swap_is_bgra);
	vk.UnmapMemory(vk_device, vk_stage_mem);

	// Staging image -> TRANSFER_DST, upload, then TRANSFER_SRC for blit.
	if (vk_stage_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
		vk_barrier(vk_stage_img, 0, vk_stage_layout,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else {
		vk_barrier(vk_stage_img, 0, vk_stage_layout,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		           VK_ACCESS_TRANSFER_READ_BIT,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	VkBufferImageCopy to_stage = {};
	to_stage.imageSubresource   = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	to_stage.imageExtent        = {static_cast<uint32_t>(src_w_px),
	                               static_cast<uint32_t>(src_h_px), 1};
	vk.CmdCopyBufferToImage(vk_cmd, vk_stage_buf, vk_stage_img,
	                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                        &to_stage);
	vk_barrier(vk_stage_img, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	           VK_PIPELINE_STAGE_TRANSFER_BIT,
	           VK_PIPELINE_STAGE_TRANSFER_BIT);
	vk_stage_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	// Destination: clear to black, then blit the picture in.
	const auto cur = tracked_layout(dst_handle);
	if (cur == VK_IMAGE_LAYOUT_UNDEFINED) {
		vk_barrier(dst, dst_layer, cur,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else {
		vk_barrier(dst, dst_layer, cur,
		           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_ACCESS_TRANSFER_WRITE_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	const VkClearColorValue black = {};
	VkImageSubresourceRange clear_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
	                                       1,
	                                       static_cast<uint32_t>(dst_layer),
	                                       1};
	vk.CmdClearColorImage(vk_cmd, dst,
	                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
	                      &clear_range);
	// Clip the blit rect to the image (the disparity shift can push it
	// past the edge); Vulkan blits do not clip implicitly. The source
	// shrinks proportionally.
	bool do_blit = (dw_px > 0 && dh_px > 0);
	int sx0 = 0, sy0 = 0, sx1 = src_w_px, sy1 = src_h_px;
	int ddx0 = dx_px, ddy0 = dy_px_top;
	int ddx1 = dx_px + dw_px, ddy1 = dy_px_top + dh_px;
	if (do_blit) {
		if (ddx0 < 0) {
			sx0  = -ddx0 * src_w_px / dw_px;
			ddx0 = 0;
		}
		if (ddy0 < 0) {
			sy0  = -ddy0 * src_h_px / dh_px;
			ddy0 = 0;
		}
		if (ddx1 > eye_w_px) {
			sx1  -= (ddx1 - eye_w_px) * src_w_px / dw_px;
			ddx1 = eye_w_px;
		}
		if (ddy1 > eye_h_px) {
			sy1  -= (ddy1 - eye_h_px) * src_h_px / dh_px;
			ddy1 = eye_h_px;
		}
		if (sx0 < 0) {
			sx0 = 0;
		}
		if (sy0 < 0) {
			sy0 = 0;
		}
		if (sx1 > src_w_px) {
			sx1 = src_w_px;
		}
		if (sy1 > src_h_px) {
			sy1 = src_h_px;
		}
		do_blit = (ddx1 > ddx0 && ddy1 > ddy0 && sx1 > sx0 &&
		           sy1 > sy0);
	}
	if (do_blit) {
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0]  = {sx0, sy0, 0};
		blit.srcOffsets[1]  = {sx1, sy1, 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
		                       static_cast<uint32_t>(dst_layer), 1};
		blit.dstOffsets[0]  = {ddx0, ddy0, 0};
		blit.dstOffsets[1]  = {ddx1, ddy1, 1};
		vk.CmdBlitImage(vk_cmd, vk_stage_img,
		                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
		                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
		                VK_FILTER_LINEAR);
	}
	vk_barrier(dst, dst_layer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	           VK_ACCESS_TRANSFER_WRITE_BIT, 0,
	           VK_PIPELINE_STAGE_TRANSFER_BIT,
	           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	if (!submit_cmds()) {
		return false;
	}
	vk_img_layouts[dst_handle] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	return true;
}

#endif // C_XR_VULKAN
