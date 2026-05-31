#include "rtv/UiOverlay.h"

#include "rtv/Check.h"
#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/NotificationManager.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/RendererDebug.h"
#include "rtv/Swapchain.h"
#include "rtv/VulkanContext.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace rtv {

namespace {

struct ImGuiVulkanLoaderData {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
};

PFN_vkVoidFunction imguiVulkanFunctionLoader(const char* functionName, void* userData) {
    const auto* loader = static_cast<const ImGuiVulkanLoaderData*>(userData);
    if (loader != nullptr && loader->instance != VK_NULL_HANDLE) {
        if (PFN_vkVoidFunction function = vkGetInstanceProcAddr(loader->instance, functionName)) {
            return function;
        }
    }
    if (loader != nullptr && loader->device != VK_NULL_HANDLE) {
        if (PFN_vkVoidFunction function = vkGetDeviceProcAddr(loader->device, functionName)) {
            return function;
        }
    }
    return nullptr;
}

} // namespace

UiOverlay::UiOverlay(GLFWwindow* window, const VulkanContext& context, const Swapchain& swapchain)
    : window_(window),
      context_(context) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "rtv_editor.ini";

    applyDarkStyle();
    ImGui_ImplGlfw_InitForVulkan(window_, true);
    ImGuiVulkanLoaderData loaderData{
        .instance = context_.instance(),
        .device = context_.device(),
    };
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, &imguiVulkanFunctionLoader, &loaderData)) {
        throw std::runtime_error("ImGui_ImplVulkan_LoadFunctions failed");
    }

    const std::array<VkDescriptorPoolSize, 3> poolSizes = {{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
    }};
    descriptorPoolStats_ = DescriptorPoolStats{
        .present = true,
        .maxSets = 256,
        .combinedImageSamplerDescriptors = 256,
        .sampledImageDescriptors = 256,
        .samplerDescriptors = 64,
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 256;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    checkVk(vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool(imgui)");

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    colorAttachmentFormat_ = swapchain.format();
    renderingInfo.pColorAttachmentFormats = &colorAttachmentFormat_;

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = context_.instance();
    initInfo.PhysicalDevice = context_.physicalDevice();
    initInfo.Device = context_.device();
    initInfo.QueueFamily = context_.queueFamilies().graphics.value();
    initInfo.Queue = context_.graphicsQueue();
    initInfo.DescriptorPool = descriptorPool_;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = swapchain.imageCount();
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
    initInfo.CheckVkResultFn = &UiOverlay::checkVkResult;
    initInfo.MinAllocationSize = 1024 * 1024;
    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    }
}

UiOverlay::~UiOverlay() {
    if (context_.device() != VK_NULL_HANDLE) {
        std::cerr << "Device idle wait: UiOverlay teardown\n";
        vkDeviceWaitIdle(context_.device());
    }
    invalidateViewportTexture();
    ImGui_ImplVulkan_Shutdown();
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UiOverlay::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    frameBegun_ = true;
}

EditorRequests UiOverlay::build(
    PathTracerRenderer& renderer,
    VkExtent2D extent,
    const SceneAsset* importedScene,
    SceneDocument* sceneDocument,
    const AssetManager* assets,
    const std::optional<std::filesystem::path>& gltfPath,
    const std::optional<std::filesystem::path>& hdrPath,
    const std::optional<std::filesystem::path>& scenePath,
    const ProjectContext* project,
        const AssetRegistry* assetRegistry,
        bool sceneDirty,
        const std::vector<EntityId>* instanceEntities,
        const std::string& sceneLoadingStatus,
        bool sceneLoadRunning,
        float sceneLoadProgress,
        const CameraController* camera,
        const UndoStack* undoStack,
        float cpuFrameMs,
        NotificationManager* notifications,
        bool externalMouseCapture) {
    EditorRequests requests;
    if (!frameBegun_) {
        return requests;
    }

    const VkExtent2D renderExtent = renderer.renderExtent();
    const VkExtent2D displayExtent = renderer.displayExtent();
    const VkExtent2D targetExtent = editor_.desiredRenderExtent(extent);
    const float renderScale = renderer.settings().renderResolutionScale;
    VkExtent2D targetRenderExtent = targetExtent;
    targetRenderExtent.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(targetRenderExtent.width) * renderScale));
    targetRenderExtent.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(targetRenderExtent.height) * renderScale));
    const bool outputMatchesViewport =
        displayExtent.width == targetExtent.width &&
        displayExtent.height == targetExtent.height &&
        renderExtent.width == targetRenderExtent.width &&
        renderExtent.height == targetRenderExtent.height;

    const VkDescriptorImageInfo descriptor = outputMatchesViewport ? renderer.viewportImageDescriptor() : VkDescriptorImageInfo{};
    if (descriptor.imageView != VK_NULL_HANDLE && descriptor.imageView != viewportImageView_) {
        invalidateViewportTexture();
        viewportTexture_ = ImGui_ImplVulkan_AddTexture(descriptor.imageView, descriptor.imageLayout);
        descriptorPoolStats_.viewportDescriptorAllocated = viewportTexture_ != VK_NULL_HANDLE ? 1u : 0u;
        viewportImageView_ = descriptor.imageView;
        viewportTextureExtent_ = displayExtent;
    }

    EditorRuntimeState state{
        .renderer = renderer,
        .importedScene = importedScene,
        .sceneDocument = sceneDocument,
        .assets = assets,
        .gltfPath = &gltfPath,
        .hdrPath = &hdrPath,
        .scenePath = &scenePath,
        .project = project,
        .assetRegistry = assetRegistry,
        .sceneDirty = sceneDirty,
        .instanceEntities = instanceEntities,
        .sceneLoadingStatus = &sceneLoadingStatus,
        .sceneLoadRunning = sceneLoadRunning,
        .sceneLoadProgress = sceneLoadProgress,
        .camera = camera,
        .undoStack = undoStack,
        .swapchainExtent = extent,
        .cpuFrameMs = cpuFrameMs,
        .viewport = EditorViewportState{
            .texture = viewportTexture_,
            .renderExtent = renderExtent,
            .displayExtent = displayExtent,
            .textureReady = outputMatchesViewport && viewportTexture_ != VK_NULL_HANDLE,
            .mouseCaptureActive = externalMouseCapture || (camera != nullptr && camera->mouseCaptured()),
        },
    };
    requests = editor_.draw(state);
    if (notifications != nullptr) {
        notifications->draw();
    }

    ImGui::Render();
    return requests;
}

void UiOverlay::record(VkCommandBuffer commandBuffer) {
    if (!frameBegun_) {
        return;
    }
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    frameBegun_ = false;
}

void UiOverlay::onSwapchainRecreated(const Swapchain& swapchain) {
    colorAttachmentFormat_ = swapchain.format();
    ImGui_ImplVulkan_SetMinImageCount(std::max(2u, swapchain.imageCount()));
}

bool UiOverlay::wantsMouse() const {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

bool UiOverlay::wantsKeyboard() const {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}

bool UiOverlay::wantsTextInput() const {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput;
}

bool UiOverlay::viewportInteractionActive() const {
    return editor_.viewportInteractionActive();
}

bool UiOverlay::viewportHovered() const {
    return editor_.viewportHovered();
}

VkExtent2D UiOverlay::desiredRenderExtent(VkExtent2D fallback) const {
    VkExtent2D extent = editor_.desiredRenderExtent(fallback);
    if (extent.width == 0 || extent.height == 0) {
        extent = fallback;
    }
    return extent;
}

void UiOverlay::invalidateViewportTexture() {
    if (viewportTexture_ != VK_NULL_HANDLE && ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplVulkan_RemoveTexture(viewportTexture_);
    }
    viewportTexture_ = VK_NULL_HANDLE;
    viewportImageView_ = VK_NULL_HANDLE;
    viewportTextureExtent_ = {};
    descriptorPoolStats_.viewportDescriptorAllocated = 0;
}

void UiOverlay::checkVkResult(VkResult result) {
    checkVk(result, "ImGui Vulkan backend");
}

void UiOverlay::applyDarkStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(5.0f, 4.0f);
    style.FramePadding = ImVec2(5.0f, 2.0f);
    style.ItemSpacing = ImVec2(5.0f, 3.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 2.0f);
    style.ScrollbarSize = 10.0f;
    style.WindowRounding = 0.0f;
    style.FrameRounding = 1.0f;
    style.GrabRounding = 1.0f;
    style.TabRounding = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowMenuButtonPosition = ImGuiDir_Left;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.018f, 0.020f, 0.023f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.026f, 0.028f, 0.031f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.020f, 0.022f, 0.026f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.115f, 0.120f, 0.130f, 0.85f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.060f, 0.064f, 0.070f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.100f, 0.120f, 0.145f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.125f, 0.155f, 0.200f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.018f, 0.020f, 0.023f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.026f, 0.029f, 0.034f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.016f, 0.018f, 0.021f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.055f, 0.058f, 0.064f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.100f, 0.125f, 0.165f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.090f, 0.095f, 0.105f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.080f, 0.085f, 0.092f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.120f, 0.145f, 0.180f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.150f, 0.190f, 0.250f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.070f, 0.075f, 0.084f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.105f, 0.125f, 0.155f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.135f, 0.170f, 0.230f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.300f, 0.560f, 0.980f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.260f, 0.500f, 0.900f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.105f, 0.110f, 0.120f, 1.0f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.120f, 0.150f, 0.190f, 0.45f);
}

} // namespace rtv
