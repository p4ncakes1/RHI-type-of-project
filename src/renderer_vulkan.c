#include "renderer_internal.h"
#include "fatal_error.h"

#include <vulkan/vulkan.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#define VK_PLATFORM_SURFACE_EXT VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#elif defined(__linux__)
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#define VK_PLATFORM_SURFACE_EXT VK_KHR_XCB_SURFACE_EXTENSION_NAME
#else
#error "renderer_vulkan: unsupported platform (Windows and Linux only)"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define VK_FRAMES_IN_FLIGHT 2u
#define VK_MAX_RENDER_PASSES 64u
#define VK_MAX_SWAPCHAIN_IMAGES 8u

typedef struct
{
    VkRenderPass vk_render_pass;
    VkFormat color_format;
    VkFormat depth_format;
    int has_depth;
    int has_stencil;
    int sample_count;
    int is_swapchain;
} vk_rp_data;

typedef struct
{
    VkPipeline pipeline;
    VkPipelineLayout layout;
    uint32_t push_constant_size;
} vk_pipeline_data;

typedef struct
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t size;
} vk_buffer_data;

typedef struct
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
    int is_depth;
    int owns_image;
} vk_texture_data;
typedef struct
{
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
} vk_swapchain_frame;
typedef struct
{
    VkCommandBuffer cmd;
    VkFence fence;
} vk_frame_sync;

typedef struct
{
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    uint32_t graphics_family;

    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    uint32_t swapchain_image_count;
    vk_swapchain_frame *swapchain_frames;
    VkImage msaa_image;
    VkDeviceMemory msaa_memory;
    VkImageView msaa_view;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkFormat depth_format;
    renderer_render_pass_t swapchain_rp;
    vk_rp_data swapchain_rp_data;
    VkCommandPool cmd_pool;
    vk_frame_sync frames[VK_FRAMES_IN_FLIGHT];
    uint32_t current_frame;
    uint32_t swapchain_index;

    int vsync;
    int sample_count;

    VkPhysicalDeviceMemoryProperties mem_props;

    VkSemaphore              image_avail[VK_MAX_SWAPCHAIN_IMAGES];
    VkSemaphore              pending_acquire_sem;
    VkSemaphore              render_done[VK_MAX_SWAPCHAIN_IMAGES];
    VkSemaphore              pending_render_sem;

#if defined(_DEBUG) || defined(RENDERER_VK_VALIDATION)
    VkDebugUtilsMessengerEXT debug_messenger;
#endif
} vk_renderer_data;
typedef struct
{
    vk_renderer_data *rd;
    VkCommandBuffer vk_cmd;
    vk_pipeline_data *bound_pipeline;
    VkFramebuffer active_framebuffer;
    VkRenderPass active_render_pass;
} vk_cmd_data;

#define VK_RD(r) ((vk_renderer_data *)(r)->backend_data)
#define VK_RP(rp) ((vk_rp_data *)(rp)->backend_data)
#define VK_PL(pl) ((vk_pipeline_data *)(pl)->backend_data)
#define VK_BUF(b) ((vk_buffer_data *)(b)->backend_data)
#define VK_TEX(t) ((vk_texture_data *)(t)->backend_data)
#define VK_CMD(c) ((vk_cmd_data *)(c)->backend_data)

static VkFormat tex_fmt_to_vk(renderer_texture_format f)
{
    switch (f)
    {
    case RENDERER_TEXTURE_FORMAT_RGBA8:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case RENDERER_TEXTURE_FORMAT_RGB8:
        return VK_FORMAT_R8G8B8_UNORM;
    case RENDERER_TEXTURE_FORMAT_RG8:
        return VK_FORMAT_R8G8_UNORM;
    case RENDERER_TEXTURE_FORMAT_R8:
        return VK_FORMAT_R8_UNORM;
    case RENDERER_TEXTURE_FORMAT_RGBA16F:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RENDERER_TEXTURE_FORMAT_RG16F:
        return VK_FORMAT_R16G16_SFLOAT;
    case RENDERER_TEXTURE_FORMAT_R16F:
        return VK_FORMAT_R16_SFLOAT;
    case RENDERER_TEXTURE_FORMAT_RGBA32F:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RENDERER_TEXTURE_FORMAT_RG32F:
        return VK_FORMAT_R32G32_SFLOAT;
    case RENDERER_TEXTURE_FORMAT_R32F:
        return VK_FORMAT_R32_SFLOAT;
    case RENDERER_TEXTURE_FORMAT_RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case RENDERER_TEXTURE_FORMAT_BC1_UNORM:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case RENDERER_TEXTURE_FORMAT_BC1_SRGB:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case RENDERER_TEXTURE_FORMAT_BC3_UNORM:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case RENDERER_TEXTURE_FORMAT_BC3_SRGB:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case RENDERER_TEXTURE_FORMAT_BC4_UNORM:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case RENDERER_TEXTURE_FORMAT_BC5_UNORM:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case RENDERER_TEXTURE_FORMAT_DEPTH16:
        return VK_FORMAT_D16_UNORM;
    case RENDERER_TEXTURE_FORMAT_DEPTH32F:
        return VK_FORMAT_D32_SFLOAT;
    case RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case RENDERER_TEXTURE_FORMAT_DEPTH32F_STENCIL8:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

static VkFormat attrib_fmt_to_vk(renderer_attrib_format f)
{
    switch (f)
    {
    case RENDERER_ATTRIB_FLOAT1:
        return VK_FORMAT_R32_SFLOAT;
    case RENDERER_ATTRIB_FLOAT2:
        return VK_FORMAT_R32G32_SFLOAT;
    case RENDERER_ATTRIB_FLOAT3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case RENDERER_ATTRIB_FLOAT4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
        return VK_FORMAT_R32G32B32_SFLOAT;
    }
}

static VkPrimitiveTopology primitive_to_vk(renderer_primitive p)
{
    switch (p)
    {
    case RENDERER_PRIMITIVE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case RENDERER_PRIMITIVE_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case RENDERER_PRIMITIVE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case RENDERER_PRIMITIVE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    default:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static VkCompareOp compare_to_vk(renderer_compare_op op)
{
    switch (op)
    {
    case RENDERER_COMPARE_NEVER:
        return VK_COMPARE_OP_NEVER;
    case RENDERER_COMPARE_LESS:
        return VK_COMPARE_OP_LESS;
    case RENDERER_COMPARE_EQUAL:
        return VK_COMPARE_OP_EQUAL;
    case RENDERER_COMPARE_LEQUAL:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case RENDERER_COMPARE_GREATER:
        return VK_COMPARE_OP_GREATER;
    case RENDERER_COMPARE_NOTEQUAL:
        return VK_COMPARE_OP_NOT_EQUAL;
    case RENDERER_COMPARE_GEQUAL:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case RENDERER_COMPARE_ALWAYS:
        return VK_COMPARE_OP_ALWAYS;
    default:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    }
}

static VkStencilOp stencil_op_to_vk(renderer_stencil_op op)
{
    switch (op)
    {
    case RENDERER_STENCIL_OP_KEEP:
        return VK_STENCIL_OP_KEEP;
    case RENDERER_STENCIL_OP_ZERO:
        return VK_STENCIL_OP_ZERO;
    case RENDERER_STENCIL_OP_REPLACE:
        return VK_STENCIL_OP_REPLACE;
    case RENDERER_STENCIL_OP_INCREMENT_CLAMP:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case RENDERER_STENCIL_OP_DECREMENT_CLAMP:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case RENDERER_STENCIL_OP_INVERT:
        return VK_STENCIL_OP_INVERT;
    case RENDERER_STENCIL_OP_INCREMENT_WRAP:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case RENDERER_STENCIL_OP_DECREMENT_WRAP:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:
        return VK_STENCIL_OP_KEEP;
    }
}

static VkBlendFactor blend_factor_to_vk(renderer_blend_factor f)
{
    switch (f)
    {
    case RENDERER_BLEND_FACTOR_ZERO:
        return VK_BLEND_FACTOR_ZERO;
    case RENDERER_BLEND_FACTOR_ONE:
        return VK_BLEND_FACTOR_ONE;
    case RENDERER_BLEND_FACTOR_SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case RENDERER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case RENDERER_BLEND_FACTOR_DST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case RENDERER_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:
        return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op_to_vk(renderer_blend_op op)
{
    switch (op)
    {
    case RENDERER_BLEND_OP_ADD:
        return VK_BLEND_OP_ADD;
    case RENDERER_BLEND_OP_SUBTRACT:
        return VK_BLEND_OP_SUBTRACT;
    case RENDERER_BLEND_OP_REV_SUBTRACT:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case RENDERER_BLEND_OP_MIN:
        return VK_BLEND_OP_MIN;
    case RENDERER_BLEND_OP_MAX:
        return VK_BLEND_OP_MAX;
    default:
        return VK_BLEND_OP_ADD;
    }
}

static int fmt_is_depth(VkFormat f)
{
    return (f == VK_FORMAT_D16_UNORM ||
            f == VK_FORMAT_D32_SFLOAT ||
            f == VK_FORMAT_D24_UNORM_S8_UINT ||
            f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
            f == VK_FORMAT_X8_D24_UNORM_PACK32);
}

static int fmt_has_stencil(VkFormat f)
{
    return (f == VK_FORMAT_D24_UNORM_S8_UINT ||
            f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
            f == VK_FORMAT_S8_UINT);
}

static int find_memory_type(const VkPhysicalDeviceMemoryProperties *mp,
                            uint32_t type_bits, VkMemoryPropertyFlags props)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; ++i)
    {
        if ((type_bits & (1u << i)) &&
            (mp->memoryTypes[i].propertyFlags & props) == props)
            return (int)i;
    }
    return -1;
}

static int alloc_image_memory(VkDevice dev,
                              const VkPhysicalDeviceMemoryProperties *mp,
                              VkImage image,
                              VkMemoryPropertyFlags props,
                              VkDeviceMemory *out_mem)
{
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image, &req);
    int type = find_memory_type(mp, req.memoryTypeBits, props);
    if (type < 0)
        return -1;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    if (vkAllocateMemory(dev, &ai, NULL, out_mem) != VK_SUCCESS)
        return -1;
    vkBindImageMemory(dev, image, *out_mem, 0);
    return 0;
}

static int alloc_buffer_memory(VkDevice dev,
                               const VkPhysicalDeviceMemoryProperties *mp,
                               VkBuffer buf,
                               VkMemoryPropertyFlags props,
                               VkDeviceMemory *out_mem)
{
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    int type = find_memory_type(mp, req.memoryTypeBits, props);
    if (type < 0)
        return -1;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    if (vkAllocateMemory(dev, &ai, NULL, out_mem) != VK_SUCCESS)
        return -1;
    vkBindBufferMemory(dev, buf, *out_mem, 0);
    return 0;
}

static VkCommandBuffer begin_one_shot(VkDevice dev, VkCommandPool pool)
{
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void end_one_shot(VkDevice dev, VkCommandPool pool,
                         VkQueue queue, VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static void image_barrier(VkCommandBuffer cb,
                          VkImage image,
                          VkImageAspectFlags aspects,
                          VkImageLayout old_layout,
                          VkImageLayout new_layout,
                          VkAccessFlags src_access,
                          VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspects,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &b);
}

#if defined(_DEBUG) || defined(RENDERER_VK_VALIDATION)
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *userdata)
{
    (void)type;
    (void)userdata;
    const char *prefix = (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARNING"
                                                                                                                                                               : "INFO";
    fprintf(stderr, "[Vulkan %s] %s\n", prefix, data->pMessage);
    return VK_FALSE;
}
#endif

static int create_instance(vk_renderer_data *rd)
{
    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_PLATFORM_SURFACE_EXT,
#if defined(_DEBUG) || defined(RENDERER_VK_VALIDATION)
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
    };
    uint32_t ext_count = sizeof(extensions) / sizeof(extensions[0]);

    const char *layers[] = {
#if defined(_DEBUG) || defined(RENDERER_VK_VALIDATION)
        "VK_LAYER_KHRONOS_validation",
#endif
    };
    uint32_t layer_count = sizeof(layers) / sizeof(layers[0]);

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "renderer",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "renderer",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = extensions,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layer_count ? layers : NULL,
    };
    if (vkCreateInstance(&ci, NULL, &rd->instance) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateInstance failed\n");
        return -1;
    }

#if defined(_DEBUG) || defined(RENDERER_VK_VALIDATION)
    PFN_vkCreateDebugUtilsMessengerEXT fn =
        (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(rd->instance, "vkCreateDebugUtilsMessengerEXT");
    if (fn)
    {
        VkDebugUtilsMessengerCreateInfoEXT dci = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = vk_debug_callback,
        };
        fn(rd->instance, &dci, NULL, &rd->debug_messenger);
    }
#endif
    return 0;
}

static int create_surface(vk_renderer_data *rd, const renderer_create_desc *desc)
{
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hwnd = (HWND)desc->native_window_handle,
        .hinstance = GetModuleHandleA(NULL),
    };
    if (vkCreateWin32SurfaceKHR(rd->instance, &sci, NULL, &rd->surface) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateWin32SurfaceKHR failed\n");
        return -1;
    }
#elif defined(__linux__)
    VkXcbSurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = (xcb_connection_t *)desc->native_display_handle,
        .window = (xcb_window_t)(uintptr_t)desc->native_window_handle,
    };
    if (vkCreateXcbSurfaceKHR(rd->instance, &sci, NULL, &rd->surface) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateXcbSurfaceKHR failed\n");
        return -1;
    }
#endif
    return 0;
}

static int pick_physical_device(vk_renderer_data *rd)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(rd->instance, &count, NULL);
    if (count == 0)
    {
        fprintf(stderr, "renderer_vulkan: no Vulkan-capable GPU found\n");
        return -1;
    }
    VkPhysicalDevice *devs = calloc(count, sizeof(VkPhysicalDevice));
    if (!devs)
        return -1;
    vkEnumeratePhysicalDevices(rd->instance, &count, devs);

    /* Prefer a discrete GPU, fall back to the first device that has a
     * graphics + present queue on our surface. */
    rd->physical_device = VK_NULL_HANDLE;
    rd->graphics_family = UINT32_MAX;

    for (uint32_t i = 0; i < count && rd->physical_device == VK_NULL_HANDLE; ++i)
    {
        uint32_t qfcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qfcount, NULL);
        VkQueueFamilyProperties *qf = calloc(qfcount, sizeof(VkQueueFamilyProperties));
        if (!qf)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qfcount, qf);

        for (uint32_t j = 0; j < qfcount; ++j)
        {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], j, rd->surface, &present_support);
            if ((qf[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support)
            {
                rd->physical_device = devs[i];
                rd->graphics_family = j;
                break;
            }
        }
        free(qf);
    }
    free(devs);

    if (rd->physical_device == VK_NULL_HANDLE)
    {
        fprintf(stderr, "renderer_vulkan: no suitable GPU found\n");
        return -1;
    }

    vkGetPhysicalDeviceMemoryProperties(rd->physical_device, &rd->mem_props);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(rd->physical_device, &props);
    fprintf(stderr, "renderer_vulkan: GPU = %s (Vulkan %d.%d)\n",
            props.deviceName,
            (int)VK_VERSION_MAJOR(props.apiVersion),
            (int)VK_VERSION_MINOR(props.apiVersion));
    return 0;
}

static int create_logical_device(vk_renderer_data *rd)
{
    float priority = 1.f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = rd->graphics_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features = {0};
    features.fillModeNonSolid = VK_TRUE; /* needed for wireframe         */
    features.samplerAnisotropy = VK_TRUE;
    features.depthClamp = VK_TRUE;

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_exts,
        .pEnabledFeatures = &features,
    };
    if (vkCreateDevice(rd->physical_device, &dci, NULL, &rd->device) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateDevice failed\n");
        return -1;
    }
    vkGetDeviceQueue(rd->device, rd->graphics_family, 0, &rd->graphics_queue);
    return 0;
}

static VkFormat choose_surface_format(vk_renderer_data *rd)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(rd->physical_device, rd->surface, &count, NULL);
    VkSurfaceFormatKHR *fmts = calloc(count, sizeof(VkSurfaceFormatKHR));
    if (!fmts)
        return VK_FORMAT_B8G8R8A8_UNORM;
    vkGetPhysicalDeviceSurfaceFormatsKHR(rd->physical_device, rd->surface, &count, fmts);
    VkFormat chosen = VK_FORMAT_B8G8R8A8_UNORM;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = fmts[i].format;
            break;
        }
    }
    free(fmts);
    return chosen;
}

static VkPresentModeKHR choose_present_mode(vk_renderer_data *rd, int vsync)
{
    if (vsync)
        return VK_PRESENT_MODE_FIFO_KHR;
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(rd->physical_device, rd->surface, &count, NULL);
    VkPresentModeKHR *modes = calloc(count, sizeof(VkPresentModeKHR));
    if (!modes)
        return VK_PRESENT_MODE_FIFO_KHR;
    vkGetPhysicalDeviceSurfacePresentModesKHR(rd->physical_device, rd->surface, &count, modes);
    /* Prefer IMMEDIATE (no frame-rate cap, may tear) over MAILBOX (still
       targets refresh rate, no tearing) when vsync is explicitly disabled. */
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
        {
            chosen = VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        }
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            chosen = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    free(modes);
    return chosen;
}

static VkFormat choose_depth_format(vk_renderer_data *rd)
{
    /* Prefer D24S8, fall back to D32F_S8, then D32F */
    static const VkFormat candidates[] = {
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT,
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(rd->physical_device, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return candidates[i];
    }
    return VK_FORMAT_D32_SFLOAT;
}

/* Build / rebuild the VkRenderPass used for swapchain rendering. */
static int build_swapchain_render_pass(vk_renderer_data *rd)
{
    VkSampleCountFlagBits samples = (rd->sample_count > 1)
                                        ? (VkSampleCountFlagBits)rd->sample_count
                                        : VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription attachments[3];
    uint32_t attachment_count = 0;
    uint32_t color_idx = 0, depth_idx = 1, resolve_idx = 2;

    /* Color attachment (MSAA or single-sample) */
    attachments[color_idx] = (VkAttachmentDescription){
        .format = rd->swapchain_format,
        .samples = samples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = (rd->sample_count > 1)
                       ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                       : VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = (rd->sample_count > 1)
                           ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                           : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    attachment_count++;

    /* Depth attachment */
    int has_stencil = fmt_has_stencil(rd->depth_format);
    attachments[depth_idx] = (VkAttachmentDescription){
        .format = rd->depth_format,
        .samples = samples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = has_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                     : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    attachment_count++;

    /* Resolve attachment (only if MSAA) */
    if (rd->sample_count > 1)
    {
        attachments[resolve_idx] = (VkAttachmentDescription){
            .format = rd->swapchain_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        };
        attachment_count++;
    }

    VkAttachmentReference color_ref = {color_idx, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {depth_idx, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve_ref = {resolve_idx, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref,
        .pResolveAttachments = (rd->sample_count > 1) ? &resolve_ref : NULL,
    };

    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo rp_ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    if (rd->swapchain_rp_data.vk_render_pass)
        vkDestroyRenderPass(rd->device, rd->swapchain_rp_data.vk_render_pass, NULL);

    if (vkCreateRenderPass(rd->device, &rp_ci, NULL, &rd->swapchain_rp_data.vk_render_pass) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateRenderPass (swapchain) failed\n");
        return -1;
    }
    rd->swapchain_rp_data.color_format = rd->swapchain_format;
    rd->swapchain_rp_data.depth_format = rd->depth_format;
    rd->swapchain_rp_data.has_depth = 1;
    rd->swapchain_rp_data.has_stencil = has_stencil;
    rd->swapchain_rp_data.sample_count = rd->sample_count;
    rd->swapchain_rp_data.is_swapchain = 1;
    rd->swapchain_rp.backend_data = &rd->swapchain_rp_data;
    return 0;
}

/* Create MSAA color image + depth image, then framebuffers for each
 * swapchain image.  Safe to call on resize (frees old resources first). */
static int build_swapchain_resources(vk_renderer_data *rd)
{
    if (rd->msaa_view)
    {
        vkDestroyImageView(rd->device, rd->msaa_view, NULL);
        rd->msaa_view = VK_NULL_HANDLE;
    }
    if (rd->msaa_image)
    {
        vkDestroyImage(rd->device, rd->msaa_image, NULL);
        rd->msaa_image = VK_NULL_HANDLE;
    }
    if (rd->msaa_memory)
    {
        vkFreeMemory(rd->device, rd->msaa_memory, NULL);
        rd->msaa_memory = VK_NULL_HANDLE;
    }
    if (rd->depth_view)
    {
        vkDestroyImageView(rd->device, rd->depth_view, NULL);
        rd->depth_view = VK_NULL_HANDLE;
    }
    if (rd->depth_image)
    {
        vkDestroyImage(rd->device, rd->depth_image, NULL);
        rd->depth_image = VK_NULL_HANDLE;
    }
    if (rd->depth_memory)
    {
        vkFreeMemory(rd->device, rd->depth_memory, NULL);
        rd->depth_memory = VK_NULL_HANDLE;
    }
    if (rd->swapchain_frames)
    {
        for (uint32_t i = 0; i < rd->swapchain_image_count; ++i)
        {
            if (rd->swapchain_frames[i].framebuffer)
                vkDestroyFramebuffer(rd->device, rd->swapchain_frames[i].framebuffer, NULL);
            if (rd->swapchain_frames[i].view)
                vkDestroyImageView(rd->device, rd->swapchain_frames[i].view, NULL);
        }
        free(rd->swapchain_frames);
        rd->swapchain_frames = NULL;
    }

    VkSampleCountFlagBits samples = (rd->sample_count > 1)
                                        ? (VkSampleCountFlagBits)rd->sample_count
                                        : VK_SAMPLE_COUNT_1_BIT;
    uint32_t w = rd->swapchain_extent.width;
    uint32_t h = rd->swapchain_extent.height;
    if (rd->sample_count > 1)
    {
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = rd->swapchain_format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = samples,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(rd->device, &ici, NULL, &rd->msaa_image) != VK_SUCCESS)
            return -1;
        if (alloc_image_memory(rd->device, &rd->mem_props, rd->msaa_image,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &rd->msaa_memory) < 0)
            return -1;
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = rd->msaa_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = rd->swapchain_format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(rd->device, &vci, NULL, &rd->msaa_view) != VK_SUCCESS)
            return -1;
    }
    VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (fmt_has_stencil(rd->depth_format))
        depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    {
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = rd->depth_format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = samples,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(rd->device, &ici, NULL, &rd->depth_image) != VK_SUCCESS)
            return -1;
        if (alloc_image_memory(rd->device, &rd->mem_props, rd->depth_image,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &rd->depth_memory) < 0)
            return -1;
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = rd->depth_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = rd->depth_format,
            .subresourceRange = {depth_aspect, 0, 1, 0, 1},
        };
        if (vkCreateImageView(rd->device, &vci, NULL, &rd->depth_view) != VK_SUCCESS)
            return -1;
    }
    rd->swapchain_frames = calloc(rd->swapchain_image_count, sizeof(vk_swapchain_frame));
    if (!rd->swapchain_frames)
        return -1;

    VkImage *images = calloc(rd->swapchain_image_count, sizeof(VkImage));
    if (!images)
        return -1;
    uint32_t cnt = rd->swapchain_image_count;
    vkGetSwapchainImagesKHR(rd->device, rd->swapchain, &cnt, images);

    for (uint32_t i = 0; i < rd->swapchain_image_count; ++i)
    {
        rd->swapchain_frames[i].image = images[i];

        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = rd->swapchain_format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(rd->device, &vci, NULL, &rd->swapchain_frames[i].view) != VK_SUCCESS)
        {
            free(images);
            return -1;
        }
        VkImageView fb_attachments[3];
        uint32_t fb_count = 0;
        if (rd->sample_count > 1)
        {
            fb_attachments[fb_count++] = rd->msaa_view;
            fb_attachments[fb_count++] = rd->depth_view;
            fb_attachments[fb_count++] = rd->swapchain_frames[i].view;
        }
        else
        {
            fb_attachments[fb_count++] = rd->swapchain_frames[i].view;
            fb_attachments[fb_count++] = rd->depth_view;
        }

        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rd->swapchain_rp_data.vk_render_pass,
            .attachmentCount = fb_count,
            .pAttachments = fb_attachments,
            .width = w,
            .height = h,
            .layers = 1,
        };
        if (vkCreateFramebuffer(rd->device, &fci, NULL, &rd->swapchain_frames[i].framebuffer) != VK_SUCCESS)
        {
            free(images);
            return -1;
        }
    }
    free(images);
    return 0;
}

static int create_swapchain(vk_renderer_data *rd, int w, int h)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(rd->physical_device, rd->surface, &caps);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        extent.width = (uint32_t)w;
        extent.height = (uint32_t)h;
    }
    rd->swapchain_extent = extent;

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = rd->surface,
        .minImageCount = image_count,
        .imageFormat = rd->swapchain_format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = choose_present_mode(rd, rd->vsync),
        .clipped = VK_TRUE,
        .oldSwapchain = rd->swapchain,
    };
    VkSwapchainKHR new_sc;
    if (vkCreateSwapchainKHR(rd->device, &sci, NULL, &new_sc) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateSwapchainKHR failed\n");
        return -1;
    }
    if (rd->swapchain)
        vkDestroySwapchainKHR(rd->device, rd->swapchain, NULL);
    rd->swapchain = new_sc;

    vkGetSwapchainImagesKHR(rd->device, rd->swapchain, &rd->swapchain_image_count, NULL);

    if (build_swapchain_render_pass(rd) != 0)
        return -1;
    if (build_swapchain_resources(rd) != 0)
        return -1;
    return 0;
}

static int create_frame_objects(vk_renderer_data *rd)
{
    VkCommandPoolCreateInfo cp_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = rd->graphics_family,
    };
    if (vkCreateCommandPool(rd->device, &cp_ci, NULL, &rd->cmd_pool) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateCommandPool failed\n");
        return -1;
    }

    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = rd->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VK_FRAMES_IN_FLIGHT,
    };
    VkCommandBuffer cbs[VK_FRAMES_IN_FLIGHT];
    if (vkAllocateCommandBuffers(rd->device, &cb_ai, cbs) != VK_SUCCESS)
        return -1;

    VkFenceCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; ++i)
    {
        rd->frames[i].cmd = cbs[i];
        if (vkCreateFence(rd->device, &fi, NULL, &rd->frames[i].fence) != VK_SUCCESS)
            return -1;
    }
    for (uint32_t i = 0; i < rd->swapchain_image_count && i < VK_MAX_SWAPCHAIN_IMAGES; ++i)
        if (vkCreateSemaphore(rd->device, &si, NULL, &rd->image_avail[i]) != VK_SUCCESS)
            return -1;
    if (vkCreateSemaphore(rd->device, &si, NULL, &rd->pending_acquire_sem) != VK_SUCCESS)
        return -1;
    for (uint32_t i = 0; i < rd->swapchain_image_count && i < VK_MAX_SWAPCHAIN_IMAGES; ++i)
        if (vkCreateSemaphore(rd->device, &si, NULL, &rd->render_done[i]) != VK_SUCCESS)
            return -1;
    if (vkCreateSemaphore(rd->device, &si, NULL, &rd->pending_render_sem) != VK_SUCCESS)
        return -1;
    return 0;
}

static int vulkan_init(renderer_t *r, const renderer_create_desc *desc)
{
    vk_renderer_data *rd = calloc(1, sizeof(vk_renderer_data));
    if (!rd)
        return -1;
    r->backend_data = rd;
    rd->vsync = desc->vsync;
    rd->sample_count = (desc->sample_count > 1) ? desc->sample_count : 1;

    if (create_instance(rd) != 0)
        goto fail;
    if (create_surface(rd, desc) != 0)
        goto fail;
    if (pick_physical_device(rd) != 0)
        goto fail;
    if (create_logical_device(rd) != 0)
        goto fail;

    rd->swapchain_format = choose_surface_format(rd);
    rd->depth_format = choose_depth_format(rd);

    if (create_swapchain(rd, desc->width, desc->height) != 0)
        goto fail;
    if (create_frame_objects(rd) != 0)
        goto fail;
    return 0;

fail:
    return -1;
}

static void vulkan_shutdown(renderer_t *r)
{
    vk_renderer_data *rd = VK_RD(r);
    if (!rd)
        return;
    if (rd->device)
        vkDeviceWaitIdle(rd->device);
    for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; ++i)
    {
        if (rd->frames[i].fence)
            vkDestroyFence(rd->device, rd->frames[i].fence, NULL);
    }
    for (uint32_t i = 0; i < rd->swapchain_image_count && i < VK_MAX_SWAPCHAIN_IMAGES; ++i)
        if (rd->image_avail[i])
            vkDestroySemaphore(rd->device, rd->image_avail[i], NULL);
    if (rd->pending_acquire_sem)
        vkDestroySemaphore(rd->device, rd->pending_acquire_sem, NULL);
    for (uint32_t i = 0; i < rd->swapchain_image_count && i < VK_MAX_SWAPCHAIN_IMAGES; ++i)
        if (rd->render_done[i])
            vkDestroySemaphore(rd->device, rd->render_done[i], NULL);
    if (rd->pending_render_sem)
        vkDestroySemaphore(rd->device, rd->pending_render_sem, NULL);
    if (rd->cmd_pool)
        vkDestroyCommandPool(rd->device, rd->cmd_pool, NULL);
    if (rd->swapchain_frames)
    {
        for (uint32_t i = 0; i < rd->swapchain_image_count; ++i)
        {
            if (rd->swapchain_frames[i].framebuffer)
                vkDestroyFramebuffer(rd->device, rd->swapchain_frames[i].framebuffer, NULL);
            if (rd->swapchain_frames[i].view)
                vkDestroyImageView(rd->device, rd->swapchain_frames[i].view, NULL);
        }
        free(rd->swapchain_frames);
    }
    if (rd->msaa_view)
        vkDestroyImageView(rd->device, rd->msaa_view, NULL);
    if (rd->msaa_image)
        vkDestroyImage(rd->device, rd->msaa_image, NULL);
    if (rd->msaa_memory)
        vkFreeMemory(rd->device, rd->msaa_memory, NULL);
    if (rd->depth_view)
        vkDestroyImageView(rd->device, rd->depth_view, NULL);
    if (rd->depth_image)
        vkDestroyImage(rd->device, rd->depth_image, NULL);
    if (rd->depth_memory)
        vkFreeMemory(rd->device, rd->depth_memory, NULL);
    if (rd->swapchain_rp_data.vk_render_pass)
        vkDestroyRenderPass(rd->device, rd->swapchain_rp_data.vk_render_pass, NULL);
    if (rd->swapchain)
        vkDestroySwapchainKHR(rd->device, rd->swapchain, NULL);

    if (rd->device)
        vkDestroyDevice(rd->device, NULL);
    if (rd->surface)
        vkDestroySurfaceKHR(rd->instance, rd->surface, NULL);

#if defined(_DEBUG) || defined(RENDERER_VK_VALIDATION)
    if (rd->debug_messenger)
    {
        PFN_vkDestroyDebugUtilsMessengerEXT fn =
            (PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(rd->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn)
            fn(rd->instance, rd->debug_messenger, NULL);
    }
#endif
    if (rd->instance)
        vkDestroyInstance(rd->instance, NULL);
    free(rd);
    r->backend_data = NULL;
}

static void vulkan_begin_frame(renderer_t *r)
{
    vk_renderer_data *rd = VK_RD(r);
    vk_frame_sync *f = &rd->frames[rd->current_frame];

    vkWaitForFences(rd->device, 1, &f->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(rd->device, 1, &f->fence);

    VkResult res = vkAcquireNextImageKHR(rd->device, rd->swapchain, UINT64_MAX,
                                         rd->pending_acquire_sem, VK_NULL_HANDLE,
                                         &rd->swapchain_index);
    /* Swap: the semaphore we just signalled becomes the per-image slot's new
       "ready" semaphore; the old per-image semaphore (presentation finished with
       it) becomes the next pending semaphore to hand to the next acquire. */
    VkSemaphore just_acquired          = rd->pending_acquire_sem;
    rd->pending_acquire_sem            = rd->image_avail[rd->swapchain_index];
    rd->image_avail[rd->swapchain_index] = just_acquired;
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
    {
    }
}

static void vulkan_end_frame(renderer_t *r)
{
    (void)r;
}

static void vulkan_present(renderer_t *r)
{
    vk_renderer_data *rd = VK_RD(r);

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rd->pending_render_sem,
        .swapchainCount = 1,
        .pSwapchains = &rd->swapchain,
        .pImageIndices = &rd->swapchain_index,
    };
    VkResult res = vkQueuePresentKHR(rd->graphics_queue, &pi);
    /* Swap: pending_render_sem was just handed to the presentation engine.
       Recycle the per-image slot (presentation engine finished with it when
       this image was last presented) as the new free semaphore. */
    VkSemaphore just_presented                 = rd->pending_render_sem;
    rd->pending_render_sem                     = rd->render_done[rd->swapchain_index];
    rd->render_done[rd->swapchain_index]       = just_presented;

    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
    {
        vkDeviceWaitIdle(rd->device);
        create_swapchain(rd, (int)rd->swapchain_extent.width, (int)rd->swapchain_extent.height);
    }
    rd->current_frame = (rd->current_frame + 1) % VK_FRAMES_IN_FLIGHT;
}

static void vulkan_resize(renderer_t *r, int w, int h)
{
    vk_renderer_data *rd = VK_RD(r);
    vkDeviceWaitIdle(rd->device);
    create_swapchain(rd, w, h);
}

static renderer_render_pass_t *vulkan_get_swapchain_render_pass(renderer_t *r)
{
    return &VK_RD(r)->swapchain_rp;
}

static renderer_render_pass_t *vulkan_render_pass_create(
    renderer_t *r, const renderer_render_pass_desc *desc)
{
    vk_renderer_data *rd = VK_RD(r);

    VkFormat color_fmt = tex_fmt_to_vk(desc->color_format);
    VkFormat depth_fmt = tex_fmt_to_vk(desc->depth_format);
    VkSampleCountFlagBits samples = (desc->sample_count > 1)
                                        ? (VkSampleCountFlagBits)desc->sample_count
                                        : VK_SAMPLE_COUNT_1_BIT;

    VkAttachmentLoadOp auto_load_op_to_vk = VK_ATTACHMENT_LOAD_OP_LOAD;
    (void)auto_load_op_to_vk;

    VkAttachmentLoadOp color_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp color_store = VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentLoadOp depth_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp depth_store = VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentLoadOp stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp stencil_store = VK_ATTACHMENT_STORE_OP_STORE;

    switch (desc->color_load_op)
    {
    case RENDERER_LOAD_OP_LOAD:
        color_load = VK_ATTACHMENT_LOAD_OP_LOAD;
        break;
    case RENDERER_LOAD_OP_CLEAR:
        color_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        break;
    case RENDERER_LOAD_OP_DONT_CARE:
        color_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        break;
    }
    color_store = (desc->color_store_op == RENDERER_STORE_OP_DONT_CARE)
                      ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                      : VK_ATTACHMENT_STORE_OP_STORE;
    switch (desc->depth_load_op)
    {
    case RENDERER_LOAD_OP_LOAD:
        depth_load = VK_ATTACHMENT_LOAD_OP_LOAD;
        break;
    case RENDERER_LOAD_OP_CLEAR:
        depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        break;
    case RENDERER_LOAD_OP_DONT_CARE:
        depth_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        break;
    }
    depth_store = (desc->depth_store_op == RENDERER_STORE_OP_DONT_CARE)
                      ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                      : VK_ATTACHMENT_STORE_OP_STORE;
    switch (desc->stencil_load_op)
    {
    case RENDERER_LOAD_OP_LOAD:
        stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
        break;
    case RENDERER_LOAD_OP_CLEAR:
        stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        break;
    case RENDERER_LOAD_OP_DONT_CARE:
        stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        break;
    }
    stencil_store = (desc->stencil_store_op == RENDERER_STORE_OP_DONT_CARE)
                        ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                        : VK_ATTACHMENT_STORE_OP_STORE;

    VkAttachmentDescription attachments[2];
    uint32_t attachment_count = 0;
    attachments[0] = (VkAttachmentDescription){
        .format = color_fmt,
        .samples = samples,
        .loadOp = color_load,
        .storeOp = color_store,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    attachment_count++;

    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };

    if (desc->has_depth)
    {
        attachments[1] = (VkAttachmentDescription){
            .format = depth_fmt,
            .samples = samples,
            .loadOp = depth_load,
            .storeOp = depth_store,
            .stencilLoadOp = stencil_load,
            .stencilStoreOp = stencil_store,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        attachment_count++;
        subpass.pDepthStencilAttachment = &depth_ref;
    }

    VkRenderPassCreateInfo rp_ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };

    vk_rp_data *data = calloc(1, sizeof(vk_rp_data));
    if (!data)
        return NULL;
    if (vkCreateRenderPass(rd->device, &rp_ci, NULL, &data->vk_render_pass) != VK_SUCCESS)
    {
        free(data);
        return NULL;
    }
    data->color_format = color_fmt;
    data->depth_format = depth_fmt;
    data->has_depth = desc->has_depth;
    data->has_stencil = desc->has_stencil;
    data->sample_count = desc->sample_count > 1 ? desc->sample_count : 1;
    data->is_swapchain = 0;

    renderer_render_pass_t *rp = calloc(1, sizeof(renderer_render_pass_t));
    if (!rp)
    {
        vkDestroyRenderPass(rd->device, data->vk_render_pass, NULL);
        free(data);
        return NULL;
    }
    rp->backend_data = data;
    return rp;
}

static void vulkan_render_pass_destroy(renderer_t *r, renderer_render_pass_t *rp)
{
    vk_renderer_data *rd = VK_RD(r);
    vk_rp_data *data = VK_RP(rp);
    vkDestroyRenderPass(rd->device, data->vk_render_pass, NULL);
    free(data);
    free(rp);
}

static renderer_pipeline_t *vulkan_pipeline_create(
    renderer_t *r, const renderer_pipeline_desc *desc)
{
    vk_renderer_data *rd = VK_RD(r);
    if (!desc->vert_src || !desc->frag_src || !desc->render_pass)
        return NULL;

    /* In a production Vulkan backend, vert_src/frag_src would be pre-compiled
     * SPIR-V blobs passed as (const uint32_t*, size_t) pairs.  For convenience
     * and compatibility with the existing main.c that passes GLSL strings we
     * expect the caller to pass SPIR-V bytecode cast to const char*, with the
     * size encoded as a uint32_t in desc->push_constant_size when the msb is
     * set — OR the user links shaderc/glslang and compiles at runtime.
     *
     * To keep this backend self-contained without a runtime compiler dependency,
     * we detect SPIR-V by the magic number 0x07230203 in the first 4 bytes and
     * interpret the pointer as (const uint32_t*) with the byte-length stored in
     * the secondary fields below.  GLSL strings are not accepted here; callers
     * must pre-compile their shaders with glslangValidator or shaderc.
     *
     * Convenience macros for the caller:
     *   RENDERER_VK_SPIRV(ptr, byte_len)  — used in renderer_pipeline_desc
     *     .vert_src = (const char*)(spirv_vert_data)
     *     .frag_src = (const char*)(spirv_frag_data)
     * The byte lengths are passed via the new fields:
     *   .vert_spirv_size  (uint32_t)
     *   .frag_spirv_size  (uint32_t)
     * These fields do not exist in the current renderer_pipeline_desc; see
     * the note at the bottom of renderer.h about adding them for Vulkan.
     *
     * For now, we use the common convention that SPIR-V data is terminated by a
     * sentinel 0x00000000 word so we can compute the length ourselves.
     */
    const uint32_t *vert_spirv = (const uint32_t *)desc->vert_src;
    const uint32_t *frag_spirv = (const uint32_t *)desc->frag_src;

    if (vert_spirv[0] != 0x07230203u)
    {
        fprintf(stderr, "renderer_vulkan: vert_src is not SPIR-V "
                        "(magic 0x%08X, expected 0x07230203)\n",
                vert_spirv[0]);
        return NULL;
    }
    if (frag_spirv[0] != 0x07230203u)
    {
        fprintf(stderr, "renderer_vulkan: frag_src is not SPIR-V "
                        "(magic 0x%08X, expected 0x07230203)\n",
                frag_spirv[0]);
        return NULL;
    }

    /* Compute SPIR-V sizes by finding the word count from the SPIR-V header.
     * Word[2] in the SPIR-V header is the bound (IDs), which is not the size.
     * The actual size is encoded in the bound field of the binary header.
     * We use the SPIR-V header word[0]=magic, word[1]=version, word[2]=generator,
     * word[3]=bound, word[4]=schema(0).  The total word count is not in the
     * header itself, so we require zero-word termination OR we read the file
     * size passed by the caller.
     *
     * For simplicity: we scan for a sentinel 0x00000000 end-marker appended
     * by the build system.  If none is found within 256 KiB we give up.
     */
    size_t vert_words = (desc->vert_spirv_size > 0)
                            ? desc->vert_spirv_size / 4
                            : ({ size_t n = 5; while (n < 256*1024/4 && vert_spirv[n]) ++n; n; });
    size_t frag_words = (desc->frag_spirv_size > 0)
                            ? desc->frag_spirv_size / 4
                            : ({ size_t n = 5; while (n < 256*1024/4 && frag_spirv[n]) ++n; n; });

    VkShaderModuleCreateInfo vsci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vert_words * 4,
        .pCode = vert_spirv,
    };
    VkShaderModuleCreateInfo fsci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = frag_words * 4,
        .pCode = frag_spirv,
    };

    VkShaderModule vert_mod = VK_NULL_HANDLE, frag_mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(rd->device, &vsci, NULL, &vert_mod) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateShaderModule (vert) failed\n");
        return NULL;
    }
    if (vkCreateShaderModule(rd->device, &fsci, NULL, &frag_mod) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateShaderModule (frag) failed\n");
        vkDestroyShaderModule(rd->device, vert_mod, NULL);
        return NULL;
    }
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = desc->push_constant_size ? desc->push_constant_size
                                         : RENDERER_PUSH_CONSTANT_MAX_SIZE,
    };
    VkPipelineLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = desc->push_constant_size ? 1 : 0,
        .pPushConstantRanges = desc->push_constant_size ? &pc_range : NULL,
    };
    vk_pipeline_data *data = calloc(1, sizeof(vk_pipeline_data));
    if (!data)
        goto fail_modules;
    if (vkCreatePipelineLayout(rd->device, &lci, NULL, &data->layout) != VK_SUCCESS)
        goto fail_data;
    data->push_constant_size = desc->push_constant_size;
    VkVertexInputBindingDescription vib = {
        .binding = 0,
        .stride = desc->vertex_stride,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attribs[RENDERER_MAX_VERTEX_ATTRIBS];
    uint32_t attrib_count = desc->attrib_count < RENDERER_MAX_VERTEX_ATTRIBS
                                ? desc->attrib_count
                                : RENDERER_MAX_VERTEX_ATTRIBS;
    for (uint32_t i = 0; i < attrib_count; ++i)
    {
        attribs[i].binding = 0;
        attribs[i].location = desc->attribs[i].location;
        attribs[i].format = attrib_fmt_to_vk(desc->attribs[i].format);
        attribs[i].offset = desc->attribs[i].offset;
    }
    VkPipelineVertexInputStateCreateInfo vi_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = desc->vertex_stride > 0 ? 1 : 0,
        .pVertexBindingDescriptions = &vib,
        .vertexAttributeDescriptionCount = attrib_count,
        .pVertexAttributeDescriptions = attribs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = primitive_to_vk(desc->primitive),
        .primitiveRestartEnable = VK_FALSE,
    };
    VkPipelineViewportStateCreateInfo vp_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkCullModeFlags cull;
    switch (desc->cull_mode)
    {
    case RENDERER_CULL_FRONT:
        cull = VK_CULL_MODE_FRONT_BIT;
        break;
    case RENDERER_CULL_BACK:
        cull = VK_CULL_MODE_BACK_BIT;
        break;
    default:
        cull = VK_CULL_MODE_NONE;
        break;
    }
    VkPipelineRasterizationStateCreateInfo rs_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = desc->depth_clamp_enable ? VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = (desc->fill_mode == RENDERER_FILL_WIREFRAME)
                           ? VK_POLYGON_MODE_LINE
                           : VK_POLYGON_MODE_FILL,
        .cullMode = cull,
        .frontFace = (desc->front_face == RENDERER_FRONT_FACE_CW)
                         ? VK_FRONT_FACE_CLOCKWISE
                         : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = (desc->depth_bias_constant_factor != 0.f ||
                            desc->depth_bias_slope_factor != 0.f)
                               ? VK_TRUE
                               : VK_FALSE,
        .depthBiasConstantFactor = desc->depth_bias_constant_factor,
        .depthBiasClamp = desc->depth_bias_clamp,
        .depthBiasSlopeFactor = desc->depth_bias_slope_factor,
        .lineWidth = 1.f,
    };
    VkPipelineMultisampleStateCreateInfo ms_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (desc->sample_count > 1)
                                    ? (VkSampleCountFlagBits)desc->sample_count
                                    : VK_SAMPLE_COUNT_1_BIT,
        .alphaToCoverageEnable = desc->alpha_to_coverage_enable ? VK_TRUE : VK_FALSE,
    };
#define MAKE_STENCIL(s) (VkStencilOpState){                       \
    .failOp = stencil_op_to_vk((s)->fail_op),                     \
    .passOp = stencil_op_to_vk((s)->pass_op),                     \
    .depthFailOp = stencil_op_to_vk((s)->depth_fail_op),          \
    .compareOp = compare_to_vk((s)->compare_op),                  \
    .compareMask = (s)->compare_mask ? (s)->compare_mask : 0xFFu, \
    .writeMask = (s)->write_mask ? (s)->write_mask : 0xFFu,       \
    .reference = (s)->reference,                                  \
}
    VkPipelineDepthStencilStateCreateInfo ds_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc->depth_test_enable ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc->depth_write_enable ? VK_TRUE : VK_FALSE,
        .depthCompareOp = compare_to_vk(desc->depth_compare),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = desc->stencil_test_enable ? VK_TRUE : VK_FALSE,
        .front = MAKE_STENCIL(&desc->stencil_front),
        .back = MAKE_STENCIL(&desc->stencil_back),
    };
#undef MAKE_STENCIL
    uint32_t cwm = desc->color_write_mask ? (uint32_t)desc->color_write_mask
                                          : (uint32_t)RENDERER_COLOR_WRITE_ALL;
    VkColorComponentFlags vk_cwm = 0;
    if (cwm & RENDERER_COLOR_WRITE_R)
        vk_cwm |= VK_COLOR_COMPONENT_R_BIT;
    if (cwm & RENDERER_COLOR_WRITE_G)
        vk_cwm |= VK_COLOR_COMPONENT_G_BIT;
    if (cwm & RENDERER_COLOR_WRITE_B)
        vk_cwm |= VK_COLOR_COMPONENT_B_BIT;
    if (cwm & RENDERER_COLOR_WRITE_A)
        vk_cwm |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable = desc->blend_enable ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = blend_factor_to_vk(desc->blend_src_color),
        .dstColorBlendFactor = blend_factor_to_vk(desc->blend_dst_color),
        .colorBlendOp = blend_op_to_vk(desc->blend_op_color),
        .srcAlphaBlendFactor = blend_factor_to_vk(desc->blend_src_alpha),
        .dstAlphaBlendFactor = blend_factor_to_vk(desc->blend_dst_alpha),
        .alphaBlendOp = blend_op_to_vk(desc->blend_op_alpha),
        .colorWriteMask = vk_cwm,
    };
    VkPipelineColorBlendStateCreateInfo cb_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_att,
    };
    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_mod,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_mod,
            .pName = "main",
        },
    };

    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi_state,
        .pInputAssemblyState = &ia_state,
        .pViewportState = &vp_state,
        .pRasterizationState = &rs_state,
        .pMultisampleState = &ms_state,
        .pDepthStencilState = &ds_state,
        .pColorBlendState = &cb_state,
        .pDynamicState = &dyn_state,
        .layout = data->layout,
        .renderPass = VK_RP(desc->render_pass)->vk_render_pass,
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(rd->device, VK_NULL_HANDLE, 1, &gp_ci, NULL,
                                  &data->pipeline) != VK_SUCCESS)
    {
        fprintf(stderr, "renderer_vulkan: vkCreateGraphicsPipelines failed\n");
        goto fail_data;
    }

    vkDestroyShaderModule(rd->device, vert_mod, NULL);
    vkDestroyShaderModule(rd->device, frag_mod, NULL);

    renderer_pipeline_t *pl = calloc(1, sizeof(renderer_pipeline_t));
    if (!pl)
    {
        vkDestroyPipeline(rd->device, data->pipeline, NULL);
        goto fail_data;
    }
    pl->backend_data = data;
    return pl;

fail_data:
    if (data->layout)
        vkDestroyPipelineLayout(rd->device, data->layout, NULL);
    free(data);
fail_modules:
    if (vert_mod)
        vkDestroyShaderModule(rd->device, vert_mod, NULL);
    if (frag_mod)
        vkDestroyShaderModule(rd->device, frag_mod, NULL);
    return NULL;
}

static void vulkan_pipeline_destroy(renderer_t *r, renderer_pipeline_t *pl)
{
    vk_renderer_data *rd = VK_RD(r);
    vkDeviceWaitIdle(rd->device);
    vk_pipeline_data *data = VK_PL(pl);
    vkDestroyPipeline(rd->device, data->pipeline, NULL);
    vkDestroyPipelineLayout(rd->device, data->layout, NULL);
    free(data);
    free(pl);
}

static renderer_buffer_t *vulkan_buffer_create(
    renderer_t *r, const renderer_buffer_desc *desc)
{
    vk_renderer_data *rd = VK_RD(r);

    VkBufferUsageFlags usage;
    switch (desc->type)
    {
    case RENDERER_BUFFER_VERTEX:
        usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        break;
    case RENDERER_BUFFER_INDEX:
        usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        break;
    case RENDERER_BUFFER_UNIFORM:
        usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        break;
    default:
        usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        break;
    }
    /* For static buffers we ideally use a staging path, but for simplicity we
     * use host-visible memory directly (appropriate for small geometry). */
    VkMemoryPropertyFlags mem_props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc->size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    vk_buffer_data *data = calloc(1, sizeof(vk_buffer_data));
    if (!data)
        return NULL;
    data->size = desc->size;

    if (vkCreateBuffer(rd->device, &bci, NULL, &data->buffer) != VK_SUCCESS)
    {
        free(data);
        return NULL;
    }
    if (alloc_buffer_memory(rd->device, &rd->mem_props, data->buffer, mem_props, &data->memory) < 0)
    {
        vkDestroyBuffer(rd->device, data->buffer, NULL);
        free(data);
        return NULL;
    }
    if (desc->data)
    {
        void *mapped;
        vkMapMemory(rd->device, data->memory, 0, desc->size, 0, &mapped);
        memcpy(mapped, desc->data, desc->size);
        vkUnmapMemory(rd->device, data->memory);
    }
    renderer_buffer_t *buf = calloc(1, sizeof(renderer_buffer_t));
    if (!buf)
    {
        vkDestroyBuffer(rd->device, data->buffer, NULL);
        vkFreeMemory(rd->device, data->memory, NULL);
        free(data);
        return NULL;
    }
    buf->backend_data = data;
    return buf;
}

static void vulkan_buffer_destroy(renderer_t *r, renderer_buffer_t *b)
{
    vk_renderer_data *rd = VK_RD(r);
    vkDeviceWaitIdle(rd->device);
    vk_buffer_data *data = VK_BUF(b);
    vkDestroyBuffer(rd->device, data->buffer, NULL);
    vkFreeMemory(rd->device, data->memory, NULL);
    free(data);
    free(b);
}

static void vulkan_buffer_update(renderer_t *r, renderer_buffer_t *b,
                                 const void *src, uint32_t size)
{
    vk_renderer_data *rd = VK_RD(r);
    vk_buffer_data *bd = VK_BUF(b);
    void *mapped;
    if (vkMapMemory(rd->device, bd->memory, 0, size, 0, &mapped) == VK_SUCCESS)
    {
        memcpy(mapped, src, size);
        vkUnmapMemory(rd->device, bd->memory);
    }
}

static renderer_texture_t *vulkan_texture_create(
    renderer_t *r, const renderer_texture_desc *desc)
{
    vk_renderer_data *rd = VK_RD(r);
    VkFormat fmt = tex_fmt_to_vk(desc->format);
    int is_depth = fmt_is_depth(fmt);
    int array_layers = (desc->array_layers > 1) ? desc->array_layers : 1;
    int sample_count = (desc->sample_count > 1) ? desc->sample_count : 1;
    uint32_t mip_levels = (desc->mip_levels > 0) ? (uint32_t)desc->mip_levels : 1;
    if (desc->generate_mipmaps && mip_levels == 1)
    {
        uint32_t m = desc->width > desc->height ? desc->width : desc->height;
        mip_levels = 1;
        while (m > 1)
        {
            m >>= 1;
            ++mip_levels;
        }
    }

    VkImageUsageFlags usage = 0;
    uint32_t uf = desc->usage;
    if (!uf)
        uf = is_depth ? RENDERER_TEXTURE_USAGE_DEPTH_STENCIL
                      : (RENDERER_TEXTURE_USAGE_SAMPLED | RENDERER_TEXTURE_USAGE_RENDER_TARGET);
    if (uf & RENDERER_TEXTURE_USAGE_SAMPLED)
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (uf & RENDERER_TEXTURE_USAGE_RENDER_TARGET)
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (uf & RENDERER_TEXTURE_USAGE_DEPTH_STENCIL)
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (uf & RENDERER_TEXTURE_USAGE_STORAGE)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (uf & RENDERER_TEXTURE_USAGE_COPY_SRC)
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (uf & RENDERER_TEXTURE_USAGE_COPY_DST)
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc->pixels)
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {(uint32_t)desc->width, (uint32_t)desc->height, 1},
        .mipLevels = mip_levels,
        .arrayLayers = (uint32_t)array_layers,
        .samples = (VkSampleCountFlagBits)sample_count,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vk_texture_data *data = calloc(1, sizeof(vk_texture_data));
    if (!data)
        return NULL;
    data->format = fmt;
    data->is_depth = is_depth;
    data->owns_image = 1;

    if (vkCreateImage(rd->device, &ici, NULL, &data->image) != VK_SUCCESS)
    {
        free(data);
        return NULL;
    }
    if (alloc_image_memory(rd->device, &rd->mem_props, data->image,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &data->memory) < 0)
    {
        vkDestroyImage(rd->device, data->image, NULL);
        free(data);
        return NULL;
    }

    VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (is_depth && fmt_has_stencil(fmt))
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = data->image,
        .viewType = (array_layers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {aspect, 0, mip_levels, 0, (uint32_t)array_layers},
    };
    if (vkCreateImageView(rd->device, &vci, NULL, &data->view) != VK_SUCCESS)
        goto fail;
    if (!is_depth)
    {
        VkFilter min_f = (desc->min_filter == RENDERER_FILTER_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        VkFilter mag_f = (desc->mag_filter == RENDERER_FILTER_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        VkSamplerMipmapMode mip_mode = (desc->min_filter == RENDERER_FILTER_NEAREST)
                                           ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                           : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode wrap_u, wrap_v, wrap_w;
        wrap_u = (desc->wrap_u == RENDERER_WRAP_CLAMP_TO_EDGE)     ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : (desc->wrap_u == RENDERER_WRAP_MIRRORED_REPEAT) ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                 : (desc->wrap_u == RENDERER_WRAP_CLAMP_TO_BORDER) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                                                                   : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        wrap_v = (desc->wrap_v == RENDERER_WRAP_CLAMP_TO_EDGE)     ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : (desc->wrap_v == RENDERER_WRAP_MIRRORED_REPEAT) ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                 : (desc->wrap_v == RENDERER_WRAP_CLAMP_TO_BORDER) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                                                                   : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        wrap_w = (desc->wrap_w == RENDERER_WRAP_CLAMP_TO_EDGE)     ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : (desc->wrap_w == RENDERER_WRAP_MIRRORED_REPEAT) ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                 : (desc->wrap_w == RENDERER_WRAP_CLAMP_TO_BORDER) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                                                                   : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = mag_f,
            .minFilter = min_f,
            .mipmapMode = mip_mode,
            .addressModeU = wrap_u,
            .addressModeV = wrap_v,
            .addressModeW = wrap_w,
            .mipLodBias = 0.f,
            .anisotropyEnable = (desc->max_anisotropy > 1.f) ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = desc->max_anisotropy,
            .compareEnable = VK_FALSE,
            .minLod = 0.f,
            .maxLod = (float)mip_levels,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
        if (vkCreateSampler(rd->device, &sci, NULL, &data->sampler) != VK_SUCCESS)
            goto fail;
    }

    /* Upload pixel data via a staging buffer */
    if (desc->pixels)
    {
        uint32_t pixel_size;
        switch (desc->format)
        {
        case RENDERER_TEXTURE_FORMAT_R8:
            pixel_size = 1;
            break;
        case RENDERER_TEXTURE_FORMAT_RG8:
            pixel_size = 2;
            break;
        case RENDERER_TEXTURE_FORMAT_RGBA8:
        case RENDERER_TEXTURE_FORMAT_RGBA8_SRGB:
            pixel_size = 4;
            break;
        case RENDERER_TEXTURE_FORMAT_RGBA16F:
            pixel_size = 8;
            break;
        case RENDERER_TEXTURE_FORMAT_RGBA32F:
            pixel_size = 16;
            break;
        default:
            pixel_size = 4;
            break;
        }
        uint32_t upload_size = (uint32_t)desc->width * (uint32_t)desc->height * pixel_size;

        VkBufferCreateInfo sbci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = upload_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBuffer staging_buf = VK_NULL_HANDLE;
        VkDeviceMemory staging_mem = VK_NULL_HANDLE;
        vkCreateBuffer(rd->device, &sbci, NULL, &staging_buf);
        alloc_buffer_memory(rd->device, &rd->mem_props, staging_buf,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &staging_mem);
        void *mapped;
        vkMapMemory(rd->device, staging_mem, 0, upload_size, 0, &mapped);
        memcpy(mapped, desc->pixels, upload_size);
        vkUnmapMemory(rd->device, staging_mem);

        VkCommandBuffer cb = begin_one_shot(rd->device, rd->cmd_pool);
        image_barrier(cb, data->image, aspect,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {aspect, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {(uint32_t)desc->width, (uint32_t)desc->height, 1},
        };
        vkCmdCopyBufferToImage(cb, staging_buf, data->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageLayout final_layout = (uf & RENDERER_TEXTURE_USAGE_SAMPLED)
                                         ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                         : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_barrier(cb, data->image, aspect,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        end_one_shot(rd->device, rd->cmd_pool, rd->graphics_queue, cb);
        vkDestroyBuffer(rd->device, staging_buf, NULL);
        vkFreeMemory(rd->device, staging_mem, NULL);
    }
    else
    {
        /* Transition to the expected initial layout */
        VkImageLayout target_layout = is_depth
                                          ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkCommandBuffer cb = begin_one_shot(rd->device, rd->cmd_pool);
        image_barrier(cb, data->image, aspect,
                      VK_IMAGE_LAYOUT_UNDEFINED, target_layout,
                      0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        end_one_shot(rd->device, rd->cmd_pool, rd->graphics_queue, cb);
    }

    renderer_texture_t *tex = calloc(1, sizeof(renderer_texture_t));
    if (!tex)
        goto fail;
    tex->backend_data = data;
    return tex;

fail:
    if (data->sampler)
        vkDestroySampler(rd->device, data->sampler, NULL);
    if (data->view)
        vkDestroyImageView(rd->device, data->view, NULL);
    if (data->image)
        vkDestroyImage(rd->device, data->image, NULL);
    if (data->memory)
        vkFreeMemory(rd->device, data->memory, NULL);
    free(data);
    return NULL;
}

static void vulkan_texture_destroy(renderer_t *r, renderer_texture_t *t)
{
    vk_renderer_data *rd = VK_RD(r);
    vk_texture_data *data = VK_TEX(t);
    if (data->sampler)
        vkDestroySampler(rd->device, data->sampler, NULL);
    if (data->view)
        vkDestroyImageView(rd->device, data->view, NULL);
    if (data->owns_image)
    {
        if (data->image)
            vkDestroyImage(rd->device, data->image, NULL);
        if (data->memory)
            vkFreeMemory(rd->device, data->memory, NULL);
    }
    free(data);
    free(t);
}

static renderer_cmd_t *vulkan_cmd_begin(renderer_t *r)
{
    vk_renderer_data *rd = VK_RD(r);
    vk_frame_sync *f = &rd->frames[rd->current_frame];

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkResetCommandBuffer(f->cmd, 0);
    vkBeginCommandBuffer(f->cmd, &bi);

    vk_cmd_data *data = calloc(1, sizeof(vk_cmd_data));
    if (!data)
        return NULL;
    data->rd = rd;
    data->vk_cmd = f->cmd;

    renderer_cmd_t *cmd = calloc(1, sizeof(renderer_cmd_t));
    if (!cmd)
    {
        free(data);
        return NULL;
    }
    cmd->backend_data = data;
    cmd->vtable = r->vtable;
    return cmd;
}

static void vulkan_cmd_submit(renderer_t *r, renderer_cmd_t *cmd)
{
    vk_renderer_data *rd = VK_RD(r);
    vk_cmd_data *cd = VK_CMD(cmd);
    vk_frame_sync *f = &rd->frames[rd->current_frame];

    vkEndCommandBuffer(cd->vk_cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rd->image_avail[rd->swapchain_index],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cd->vk_cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &rd->pending_render_sem,
    };
    vkQueueSubmit(rd->graphics_queue, 1, &si, f->fence);
    free(cd);
    free(cmd);
}

static void vulkan_cmd_begin_render_pass(renderer_cmd_t *cmd,
                                         renderer_render_pass_t *rp,
                                         renderer_texture_t *color_tex,
                                         renderer_texture_t *depth_tex,
                                         const renderer_clear_value *clear)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    vk_rp_data *rpd = VK_RP(rp);
    vk_renderer_data *rd = cd->rd;
    VkFramebuffer fb;
    if (rpd->is_swapchain)
    {
        fb = rd->swapchain_frames[rd->swapchain_index].framebuffer;
    }
    else
    {
        VkImageView att[2];
        uint32_t att_count = 0;
        if (color_tex)
            att[att_count++] = VK_TEX(color_tex)->view;
        if (depth_tex)
            att[att_count++] = VK_TEX(depth_tex)->view;
        int w = rd->swapchain_extent.width, h = rd->swapchain_extent.height;

        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rpd->vk_render_pass,
            .attachmentCount = att_count,
            .pAttachments = att,
            .width = (uint32_t)w,
            .height = (uint32_t)h,
            .layers = 1,
        };
        vkCreateFramebuffer(rd->device, &fci, NULL, &fb);
        cd->active_framebuffer = fb;
    }
    VkClearValue clears[3];
    uint32_t clear_count = 0;
    if (clear)
    {
        clears[clear_count++].color = (VkClearColorValue){{clear->r, clear->g, clear->b, clear->a}};
        clears[clear_count++].depthStencil = (VkClearDepthStencilValue){clear->depth, clear->stencil};
        if (rpd->sample_count > 1) /* resolve attachment has no clear value */
            clears[clear_count++].color = (VkClearColorValue){{0.f, 0.f, 0.f, 1.f}};
    }

    VkRenderPassBeginInfo rbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rpd->vk_render_pass,
        .framebuffer = fb,
        .renderArea = {{0, 0}, rd->swapchain_extent},
        .clearValueCount = clear_count,
        .pClearValues = clear ? clears : NULL,
    };
    vkCmdBeginRenderPass(cd->vk_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    cd->active_render_pass = rpd->vk_render_pass;
}

static void vulkan_cmd_end_render_pass(renderer_cmd_t *cmd)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    vkCmdEndRenderPass(cd->vk_cmd);
    /* Destroy the temporary offscreen framebuffer if we created one */
    if (cd->active_framebuffer)
    {
        vkDestroyFramebuffer(cd->rd->device, cd->active_framebuffer, NULL);
        cd->active_framebuffer = VK_NULL_HANDLE;
    }
}

static void vulkan_cmd_bind_pipeline(renderer_cmd_t *cmd, renderer_pipeline_t *pl)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    vk_pipeline_data *data = VK_PL(pl);
    cd->bound_pipeline = data;
    vkCmdBindPipeline(cd->vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, data->pipeline);
}

static void vulkan_cmd_bind_vertex_buffer(renderer_cmd_t *cmd, renderer_buffer_t *buf,
                                          uint32_t slot, uint32_t byte_offset)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    VkBuffer vb = VK_BUF(buf)->buffer;
    VkDeviceSize offset = byte_offset;
    vkCmdBindVertexBuffers(cd->vk_cmd, slot, 1, &vb, &offset);
}

static void vulkan_cmd_bind_index_buffer(renderer_cmd_t *cmd, renderer_buffer_t *buf,
                                         uint32_t byte_offset)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    vkCmdBindIndexBuffer(cd->vk_cmd, VK_BUF(buf)->buffer,
                         byte_offset, VK_INDEX_TYPE_UINT32);
}

static void vulkan_cmd_bind_texture(renderer_cmd_t *cmd, renderer_texture_t *tex,
                                    uint32_t slot)
{
    /* Textures in Vulkan are bound via descriptor sets, not inline commands.
     * A production backend would maintain a descriptor pool + set per pipeline.
     * This stub stores the binding intent; the actual VkDescriptorSet update
     * would happen in cmd_draw after all bindings are collected.
     *
     * For the scope of this abstraction layer (no descriptor set API in
     * renderer.h), we leave this as a TODO stub that compiles cleanly.
     */
    (void)cmd;
    (void)tex;
    (void)slot;
    /* TODO: update descriptor set at slot with tex->view + tex->sampler */
}

static void vulkan_cmd_push_constants(renderer_cmd_t *cmd, renderer_pipeline_t *pl,
                                      const void *data, uint32_t size)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    vk_pipeline_data *pd = VK_PL(pl);
    if (!pd->push_constant_size)
        return;
    vkCmdPushConstants(cd->vk_cmd, pd->layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, size, data);
}

static void vulkan_cmd_set_viewport(renderer_cmd_t *cmd,
                                    float x, float y, float w, float h,
                                    float min_depth, float max_depth)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    /* Vulkan NDC has Y pointing down; flip Y and height to match OpenGL/HLSL
     * convention when the caller uses the same coordinate system. */
    VkViewport vp = {x, y + h, w, -h, min_depth, max_depth};
    vkCmdSetViewport(cd->vk_cmd, 0, 1, &vp);
}

static void vulkan_cmd_set_scissor(renderer_cmd_t *cmd, int x, int y, int w, int h)
{
    vk_cmd_data *cd = VK_CMD(cmd);
    VkRect2D rect = {{x, y}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetScissor(cd->vk_cmd, 0, 1, &rect);
}

static void vulkan_cmd_draw(renderer_cmd_t *cmd,
                            uint32_t vertex_count, uint32_t instance_count,
                            uint32_t first_vertex, uint32_t first_instance)
{
    vkCmdDraw(VK_CMD(cmd)->vk_cmd, vertex_count, instance_count,
              first_vertex, first_instance);
}

static void vulkan_cmd_draw_indexed(renderer_cmd_t *cmd,
                                    uint32_t index_count, uint32_t instance_count,
                                    uint32_t first_index, int32_t vertex_offset,
                                    uint32_t first_instance)
{
    vkCmdDrawIndexed(VK_CMD(cmd)->vk_cmd, index_count, instance_count,
                     first_index, vertex_offset, first_instance);
}

static const renderer_backend_vtable s_vulkan_vtable = {
    .init = vulkan_init,
    .shutdown = vulkan_shutdown,
    .begin_frame = vulkan_begin_frame,
    .end_frame = vulkan_end_frame,
    .present = vulkan_present,
    .resize = vulkan_resize,
    .get_swapchain_render_pass = vulkan_get_swapchain_render_pass,
    .render_pass_create = vulkan_render_pass_create,
    .render_pass_destroy = vulkan_render_pass_destroy,
    .pipeline_create = vulkan_pipeline_create,
    .pipeline_destroy = vulkan_pipeline_destroy,
    .buffer_create = vulkan_buffer_create,
    .buffer_destroy = vulkan_buffer_destroy,
    .buffer_update = vulkan_buffer_update,
    .texture_create = vulkan_texture_create,
    .texture_destroy = vulkan_texture_destroy,
    .cmd_begin = vulkan_cmd_begin,
    .cmd_submit = vulkan_cmd_submit,
    .cmd_begin_render_pass = vulkan_cmd_begin_render_pass,
    .cmd_end_render_pass = vulkan_cmd_end_render_pass,
    .cmd_bind_pipeline = vulkan_cmd_bind_pipeline,
    .cmd_bind_vertex_buffer = vulkan_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer = vulkan_cmd_bind_index_buffer,
    .cmd_bind_texture = vulkan_cmd_bind_texture,
    .cmd_push_constants = vulkan_cmd_push_constants,
    .cmd_set_viewport = vulkan_cmd_set_viewport,
    .cmd_set_scissor = vulkan_cmd_set_scissor,
    .cmd_draw = vulkan_cmd_draw,
    .cmd_draw_indexed = vulkan_cmd_draw_indexed,
};

const renderer_backend_vtable *renderer_backend_vulkan_vtable(void)
{
    return &s_vulkan_vtable;
}