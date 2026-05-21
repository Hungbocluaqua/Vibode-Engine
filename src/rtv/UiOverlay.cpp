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
#include <stdexcept>

namespace rtv {

namespace {

struct ImGuiVulkanLoaderData {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
};

PFN_vkVoidFunction imguiVulkanFunctionLoader(const char* functionName, void* userData) {
    const auto* loader = static_cast<const ImGuiVulkanLoaderData*>(userData);
    if (loader != nullptr && loader->device != VK_NULL_HANDLE) {
        if (PFN_vkVoidFunction function = vkGetDeviceProcAddr(loader->device, functionName)) {
            return function;
        }
    }
    return loader != nullptr ? vkGetInstanceProcAddr(loader->instance, functionName) : nullptr;
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
    const VkFormat colorFormat = swapchain.format();
    renderingInfo.pColorAttachmentFormats = &colorFormat;

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
    const std::vector<EntityId>* instanceEntities,
    const std::string& sceneLoadingStatus,
    const CameraController* camera,
    float cpuFrameMs,
    NotificationManager* notifications) {
    EditorRequests requests;
    if (!frameBegun_) {
        return requests;
    }

    const VkExtent2D renderExtent = renderer.renderExtent();
    VkExtent2D targetExtent = editor_.desiredRenderExtent(extent);
    const float renderScale = renderer.settings().renderResolutionScale;
    targetExtent.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(targetExtent.width) * renderScale));
    targetExtent.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(targetExtent.height) * renderScale));
    const bool outputMatchesViewport = renderExtent.width == targetExtent.width && renderExtent.height == targetExtent.height;

    const VkDescriptorImageInfo descriptor = outputMatchesViewport ? renderer.viewportImageDescriptor() : VkDescriptorImageInfo{};
    if (descriptor.imageView != VK_NULL_HANDLE && descriptor.imageView != viewportImageView_) {
        invalidateViewportTexture();
        viewportTexture_ = ImGui_ImplVulkan_AddTexture(descriptor.imageView, descriptor.imageLayout);
        viewportImageView_ = descriptor.imageView;
        viewportTextureExtent_ = renderExtent;
    }

    EditorRuntimeState state{
        .renderer = renderer,
        .importedScene = importedScene,
        .sceneDocument = sceneDocument,
        .assets = assets,
        .gltfPath = &gltfPath,
        .hdrPath = &hdrPath,
        .instanceEntities = instanceEntities,
        .sceneLoadingStatus = &sceneLoadingStatus,
        .camera = camera,
        .swapchainExtent = extent,
        .cpuFrameMs = cpuFrameMs,
        .viewport = EditorViewportState{
            .texture = viewportTexture_,
            .renderExtent = renderExtent,
            .textureReady = outputMatchesViewport && viewportTexture_ != VK_NULL_HANDLE,
            .mouseCaptureActive = camera != nullptr && camera->mouseCaptured(),
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
}

void UiOverlay::checkVkResult(VkResult result) {
    checkVk(result, "ImGui Vulkan backend");
}

void UiOverlay::applyDarkStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowMenuButtonPosition = ImGuiDir_Left;
}

} // namespace rtv
