#include "rtv/NsightGraphicsRuntime.h"

#include <iostream>

#if defined(RTV_HAS_NSIGHT_GRAPHICS_SDK)
#include <NGFX_GPUTrace_Vulkan.h>
#include <NGFX_GraphicsCapture_Vulkan.h>
#include <NGFX_SystemProfiling_Vulkan.h>
#include <NGFX_Vulkan.h>
#endif

namespace rtv {
namespace {
bool g_active = false;
}

bool initializeNsightGraphicsRuntime() {
#if defined(RTV_HAS_NSIGHT_GRAPHICS_SDK)
    struct Candidate {
        NGFX_ActivityType type;
    };
    constexpr Candidate candidates[] = {
        {NGFX_ActivityType_GraphicsCapture},
        {NGFX_ActivityType_GPUTrace},
        {NGFX_ActivityType_SystemProfiling},
    };
    for (const Candidate candidate : candidates) {
        bool injected = false;
        if (NGFX_IsActivityInjected(candidate.type, &injected) != NGFX_Result_Success || !injected) {
            continue;
        }
        NGFX_Result result = NGFX_Result_InvalidParameter;
        if (candidate.type == NGFX_ActivityType_GraphicsCapture) {
            NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params params{};
            params.version = NGFX_GraphicsCapture_InitializeActivity_Vulkan_Params_VER;
            result = NGFX_GraphicsCapture_InitializeActivity_Vulkan(&params);
        } else if (candidate.type == NGFX_ActivityType_GPUTrace) {
            NGFX_GPUTrace_InitializeActivity_Vulkan_Params params{};
            params.version = NGFX_GPUTrace_InitializeActivity_Vulkan_Params_VER;
            result = NGFX_GPUTrace_InitializeActivity_Vulkan(&params);
        } else {
            NGFX_SystemProfiling_InitializeActivity_Vulkan_Params params{};
            params.version = NGFX_SystemProfiling_InitializeActivity_Vulkan_Params_VER;
            result = NGFX_SystemProfiling_InitializeActivity_Vulkan(&params);
        }
        g_active = result == NGFX_Result_Success;
        if (!g_active) {
            std::cerr << "Warning: Nsight Graphics SDK activity initialization failed with result "
                      << static_cast<int>(result) << '\n';
        }
        return g_active;
    }
#endif
    return false;
}

void emitNsightFrameBoundary(VkQueue queue, VkImage outputImage) {
#if defined(RTV_HAS_NSIGHT_GRAPHICS_SDK)
    if (!g_active || queue == VK_NULL_HANDLE) {
        return;
    }
    NGFX_ResourceDescription_Vulkan resource{};
    resource.version = NGFX_ResourceDescription_Vulkan_VER;
    resource.type = NGFX_ResourceType_Vulkan_VkImage;
    resource.image = outputImage;

    NGFX_FrameBoundary_Vulkan_Params params{};
    params.version = NGFX_FrameBoundary_Vulkan_Params_VER;
    params.queue = queue;
    params.outputResources = outputImage != VK_NULL_HANDLE ? &resource : nullptr;
    params.numOutputResources = outputImage != VK_NULL_HANDLE ? 1 : 0;
    (void)NGFX_FrameBoundary_Vulkan(&params);
#else
    (void)queue;
    (void)outputImage;
#endif
}

bool nsightGraphicsRuntimeActive() {
    return g_active;
}

} // namespace rtv
