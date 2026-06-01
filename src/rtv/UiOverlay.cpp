#include "rtv/UiOverlay.h"

#include "rtv/Check.h"
#include "rtv/AssetManager.h"
#include "rtv/BufferUploader.h"
#include "rtv/CameraController.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/CommandSystem.h"
#include "rtv/NotificationManager.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/RendererDebug.h"
#include "rtv/Swapchain.h"
#include "rtv/VulkanContext.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

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

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext;
}

bool isStandalonePreviewTexturePath(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return true;
    }
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr" ||
        ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".rtlevel" || ext == ".mscene" || ext == ".vproject" ||
        ext == ".mtl" || ext == ".ies" || ext == ".vdb";
}

bool isRasterGpuPreviewPath(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr";
}

int64_t pathWriteStampForPreview(const std::filesystem::path& path) {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : stamp.time_since_epoch().count();
}

uint64_t pathSizeForPreview(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return 0;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0u : static_cast<uint64_t>(size);
}

uint8_t toneMapHdrPreview(float value) {
    const float mapped = std::clamp(value / (1.0f + value), 0.0f, 1.0f);
    return static_cast<uint8_t>(std::pow(mapped, 1.0f / 2.2f) * 255.0f + 0.5f);
}

uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) | (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(a) << 24u);
}

void fillRect(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    x0 = std::min(x0, width);
    x1 = std::min(x1, width);
    y0 = std::min(y0, height);
    y1 = std::min(y1, height);
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            pixels[static_cast<size_t>(y) * width + x] = color;
        }
    }
}

void drawRectOutline(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    fillRect(pixels, width, height, x0, y0, x1, std::min(y0 + 2u, y1), color);
    fillRect(pixels, width, height, x0, y1 > 2u ? y1 - 2u : y0, x1, y1, color);
    fillRect(pixels, width, height, x0, y0, std::min(x0 + 2u, x1), y1, color);
    fillRect(pixels, width, height, x1 > 2u ? x1 - 2u : x0, y0, x1, y1, color);
}

void drawCircle(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, int cx, int cy, int radius, uint32_t color) {
    const int r2 = radius * radius;
    for (int y = std::max(0, cy - radius); y < std::min<int>(static_cast<int>(height), cy + radius); ++y) {
        for (int x = std::max(0, cx - radius); x < std::min<int>(static_cast<int>(width), cx + radius); ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx + dy * dy <= r2) {
                pixels[static_cast<size_t>(y) * width + static_cast<uint32_t>(x)] = color;
            }
        }
    }
}

uint32_t previewAccentForPath(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return rgba(218, 176, 86);
    const std::string ext = lowerExtension(path);
    if (ext == ".gltf" || ext == ".glb" || ext == ".obj") return rgba(105, 170, 230);
    if (ext == ".rtlevel" || ext == ".mscene" || ext == ".vproject") return rgba(116, 150, 230);
    if (ext == ".mtl") return rgba(170, 120, 210);
    if (ext == ".ies") return rgba(245, 190, 90);
    if (ext == ".vdb") return rgba(120, 210, 190);
    return rgba(120, 150, 180);
}

std::vector<uint32_t> generateStandalonePreviewPixels(const std::filesystem::path& path, uint32_t& outWidth, uint32_t& outHeight) {
    outWidth = 256;
    outHeight = 144;
    const uint32_t accent = previewAccentForPath(path);
    std::vector<uint32_t> pixels(static_cast<size_t>(outWidth) * outHeight, rgba(18, 21, 26));
    fillRect(pixels, outWidth, outHeight, 0, 0, outWidth, 30, rgba(30, 36, 46));
    fillRect(pixels, outWidth, outHeight, 0, 30, 5, outHeight, accent);
    drawRectOutline(pixels, outWidth, outHeight, 1, 1, outWidth - 1, outHeight - 1, rgba(64, 76, 92));

    std::error_code ec;
    const bool directory = std::filesystem::is_directory(path, ec);
    const std::string ext = lowerExtension(path);
    if (directory) {
        fillRect(pixels, outWidth, outHeight, 32, 54, 90, 92, rgba(70, 55, 26));
        fillRect(pixels, outWidth, outHeight, 42, 44, 76, 58, rgba(95, 76, 36));
        drawRectOutline(pixels, outWidth, outHeight, 32, 54, 90, 92, accent);
    } else if (ext == ".gltf" || ext == ".glb" || ext == ".obj") {
        drawRectOutline(pixels, outWidth, outHeight, 36, 62, 100, 98, accent);
        drawRectOutline(pixels, outWidth, outHeight, 54, 44, 118, 80, rgba(88, 126, 170));
        fillRect(pixels, outWidth, outHeight, 118, 58, 154, 62, accent);
        fillRect(pixels, outWidth, outHeight, 118, 78, 174, 82, rgba(88, 126, 170));
        fillRect(pixels, outWidth, outHeight, 118, 98, 146, 102, rgba(88, 126, 170));
    } else if (ext == ".rtlevel" || ext == ".mscene" || ext == ".vproject") {
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t x = 34 + (i % 2u) * 48u;
            const uint32_t y = 50 + (i / 2u) * 34u;
            drawRectOutline(pixels, outWidth, outHeight, x, y, x + 34, y + 22, accent);
        }
        fillRect(pixels, outWidth, outHeight, 130, 58, 196, 62, rgba(92, 112, 150));
        fillRect(pixels, outWidth, outHeight, 130, 78, 180, 82, rgba(92, 112, 150));
        fillRect(pixels, outWidth, outHeight, 130, 98, 210, 102, rgba(92, 112, 150));
    } else if (ext == ".mtl") {
        drawCircle(pixels, outWidth, outHeight, 70, 76, 34, rgba(80, 60, 100));
        drawCircle(pixels, outWidth, outHeight, 78, 66, 11, rgba(190, 165, 220));
        drawRectOutline(pixels, outWidth, outHeight, 126, 52, 202, 101, accent);
    } else if (ext == ".ies") {
        drawCircle(pixels, outWidth, outHeight, 72, 54, 8, accent);
        for (uint32_t i = 0; i < 7; ++i) {
            fillRect(pixels, outWidth, outHeight, 68 + i * 8, 66 + i * 5, 76 + i * 8, 104 + i * 2, rgba(95, 80, 42));
        }
        fillRect(pixels, outWidth, outHeight, 124, 66, 198, 70, accent);
        fillRect(pixels, outWidth, outHeight, 124, 88, 178, 92, rgba(132, 105, 50));
    } else if (ext == ".vdb") {
        for (uint32_t i = 0; i < 6; ++i) {
            drawCircle(pixels, outWidth, outHeight, 48 + static_cast<int>(i) * 18, 68 + static_cast<int>(i % 3u) * 9, 22, rgba(50, 98, 92));
        }
        drawRectOutline(pixels, outWidth, outHeight, 132, 50, 204, 104, accent);
    } else {
        drawRectOutline(pixels, outWidth, outHeight, 42, 46, 108, 104, accent);
        fillRect(pixels, outWidth, outHeight, 124, 62, 204, 66, rgba(92, 112, 150));
        fillRect(pixels, outWidth, outHeight, 124, 82, 180, 86, rgba(92, 112, 150));
    }
    return pixels;
}

std::vector<uint32_t> downsampleRgbaPreview(const uint32_t* pixels, int width, int height, uint32_t& outWidth, uint32_t& outHeight) {
    constexpr int maxPreviewSide = 256;
    const int sourceMax = std::max(width, height);
    const float scale = sourceMax > maxPreviewSide ? static_cast<float>(maxPreviewSide) / static_cast<float>(sourceMax) : 1.0f;
    outWidth = std::max(1u, static_cast<uint32_t>(static_cast<float>(width) * scale));
    outHeight = std::max(1u, static_cast<uint32_t>(static_cast<float>(height) * scale));

    std::vector<uint32_t> result(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight));
    for (uint32_t y = 0; y < outHeight; ++y) {
        const int sourceY = std::clamp(static_cast<int>((static_cast<uint64_t>(y) * static_cast<uint64_t>(height)) / outHeight), 0, height - 1);
        for (uint32_t x = 0; x < outWidth; ++x) {
            const int sourceX = std::clamp(static_cast<int>((static_cast<uint64_t>(x) * static_cast<uint64_t>(width)) / outWidth), 0, width - 1);
            result[static_cast<size_t>(y) * outWidth + x] = pixels[static_cast<size_t>(sourceY) * static_cast<size_t>(width) + sourceX];
        }
    }
    return result;
}

std::vector<uint32_t> loadStandalonePreviewPixels(const std::filesystem::path& path, uint32_t& outWidth, uint32_t& outHeight) {
    if (!isRasterGpuPreviewPath(path)) {
        return generateStandalonePreviewPixels(path, outWidth, outHeight);
    }

    const std::string filename = path.string();
    int width = 0;
    int height = 0;
    int channels = 0;
    if (lowerExtension(path) == ".hdr") {
        float* hdr = stbi_loadf(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (hdr == nullptr || width <= 0 || height <= 0) {
            if (hdr != nullptr) {
                stbi_image_free(hdr);
            }
            return {};
        }
        std::vector<uint32_t> ldr(static_cast<size_t>(width) * static_cast<size_t>(height));
        for (size_t i = 0; i < ldr.size(); ++i) {
            const size_t source = i * 4u;
            const uint32_t r = toneMapHdrPreview(hdr[source + 0]);
            const uint32_t g = toneMapHdrPreview(hdr[source + 1]);
            const uint32_t b = toneMapHdrPreview(hdr[source + 2]);
            ldr[i] = r | (g << 8u) | (b << 16u) | (255u << 24u);
        }
        stbi_image_free(hdr);
        return downsampleRgbaPreview(ldr.data(), width, height, outWidth, outHeight);
    }

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (data == nullptr || width <= 0 || height <= 0) {
        if (data != nullptr) {
            stbi_image_free(data);
        }
        return {};
    }
    const auto* rgba = reinterpret_cast<const uint32_t*>(data);
    std::vector<uint32_t> pixels = downsampleRgbaPreview(rgba, width, height, outWidth, outHeight);
    stbi_image_free(data);
    return pixels;
}

} // namespace

UiOverlay::UiOverlay(GLFWwindow* window, const VulkanContext& context, const Swapchain& swapchain, ResourceAllocator& allocator, BufferUploader& uploader)
    : window_(window),
      context_(context),
      allocator_(allocator),
      uploader_(uploader) {
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
    textureRetireFrameDelay_ = std::max<uint64_t>(CommandSystem::framesInFlight, swapchain.imageCount()) + 1u;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    checkVk(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &assetPreviewSampler_), "vkCreateSampler(editor asset previews)");
}

UiOverlay::~UiOverlay() {
    if (context_.device() != VK_NULL_HANDLE) {
        std::cerr << "Device idle wait: UiOverlay teardown\n";
        vkDeviceWaitIdle(context_.device());
    }
    invalidateEditorTextures();
    invalidateAssetPreviewTextures();
    invalidateViewportTexture();
    releaseRetiredTextures(true);
    ImGui_ImplVulkan_Shutdown();
    if (assetPreviewSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), assetPreviewSampler_, nullptr);
        assetPreviewSampler_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UiOverlay::beginFrame() {
    ++uiFrameSerial_;
    releaseRetiredTextures();
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
        CameraController* camera,
        const UndoStack* undoStack,
        const EditorRenderJobStatus* renderJob,
        const EditorPlacementStatus* placement,
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
        .uiTextures = EditorUiTextureProvider{.user = this, .acquire = &UiOverlay::acquireEditorTextureCallback, .acquireAssetPreview = &UiOverlay::acquireEditorAssetPreviewCallback},
        .renderJob = renderJob,
        .placement = placement,
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
        editor_.handleNotificationAction(notifications->consumeRequestedAction(), requests);
    }

    ImGui::Render();
    return requests;
}

EditorRequests UiOverlay::buildProjectManager(
    const ProjectContext* project,
    const std::string& sceneLoadingStatus,
    bool sceneLoadRunning,
    float sceneLoadProgress,
    NotificationManager* notifications) {
    EditorRequests requests;
    if (!frameBegun_) {
        return requests;
    }

    requests = editor_.drawProjectManagerLauncher(ProjectManagerRuntimeState{
        .project = project,
        .sceneLoadingStatus = &sceneLoadingStatus,
        .sceneLoadRunning = sceneLoadRunning,
        .sceneLoadProgress = sceneLoadProgress,
        .standaloneLauncher = true,
    });
    if (notifications != nullptr) {
        notifications->draw();
        editor_.handleNotificationAction(notifications->consumeRequestedAction(), requests);
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
    textureRetireFrameDelay_ = std::max<uint64_t>(CommandSystem::framesInFlight, swapchain.imageCount()) + 1u;
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
        retireTextureDescriptor(viewportTexture_);
    }
    viewportTexture_ = VK_NULL_HANDLE;
    viewportImageView_ = VK_NULL_HANDLE;
    viewportTextureExtent_ = {};
    descriptorPoolStats_.viewportDescriptorAllocated = 0;
}

VkDescriptorSet UiOverlay::acquireEditorTextureCallback(void* user, VkImageView imageView, VkImageLayout imageLayout) {
    auto* overlay = static_cast<UiOverlay*>(user);
    return overlay != nullptr ? overlay->acquireEditorTexture(imageView, imageLayout) : VK_NULL_HANDLE;
}

VkDescriptorSet UiOverlay::acquireEditorAssetPreviewCallback(void* user, const std::filesystem::path& path, uint32_t* width, uint32_t* height) {
    auto* overlay = static_cast<UiOverlay*>(user);
    return overlay != nullptr ? overlay->acquireEditorAssetPreviewTexture(path, width, height) : VK_NULL_HANDLE;
}

VkDescriptorSet UiOverlay::acquireEditorTexture(VkImageView imageView, VkImageLayout imageLayout) {
    if (imageView == VK_NULL_HANDLE || ImGui::GetCurrentContext() == nullptr) {
        return VK_NULL_HANDLE;
    }
    const UiTextureKey key{.imageView = imageView, .imageLayout = imageLayout};
    if (const auto it = editorTextures_.find(key); it != editorTextures_.end()) {
        return it->second;
    }
    VkDescriptorSet texture = ImGui_ImplVulkan_AddTexture(imageView, imageLayout);
    if (texture != VK_NULL_HANDLE) {
        editorTextures_.emplace(key, texture);
    }
    return texture;
}

VkDescriptorSet UiOverlay::acquireEditorAssetPreviewTexture(const std::filesystem::path& path, uint32_t* width, uint32_t* height) {
    if (ImGui::GetCurrentContext() == nullptr || !isStandalonePreviewTexturePath(path)) {
        return VK_NULL_HANDLE;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return VK_NULL_HANDLE;
    }

    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    const int64_t writeStamp = pathWriteStampForPreview(path);
    const uint64_t sourceSize = pathSizeForPreview(path);
    if (auto it = assetPreviewTextures_.find(key); it != assetPreviewTextures_.end()) {
        AssetPreviewTexture& cached = it->second;
        if (cached.writeStamp == writeStamp && cached.sourceSize == sourceSize && cached.descriptor != VK_NULL_HANDLE) {
            if (width != nullptr) *width = cached.width;
            if (height != nullptr) *height = cached.height;
            return cached.descriptor;
        }
        if (cached.descriptor != VK_NULL_HANDLE) {
            retireAssetPreviewTexture(std::move(cached));
        }
        assetPreviewTextures_.erase(it);
    }

    uint32_t previewWidth = 0;
    uint32_t previewHeight = 0;
    std::vector<uint32_t> pixels = loadStandalonePreviewPixels(path, previewWidth, previewHeight);
    if (pixels.empty() || previewWidth == 0 || previewHeight == 0) {
        return VK_NULL_HANDLE;
    }

    AssetPreviewTexture preview;
    preview.width = previewWidth;
    preview.height = previewHeight;
    preview.writeStamp = writeStamp;
    preview.sourceSize = sourceSize;
    preview.image.create(allocator_, ImageDesc{
        .width = previewWidth,
        .height = previewHeight,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "editor standalone asset preview texture",
    });
    uploader_.uploadToImage2D(
        preview.image,
        pixels.data(),
        static_cast<VkDeviceSize>(pixels.size() * sizeof(uint32_t)),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    preview.descriptor = ImGui_ImplVulkan_AddTexture(preview.image.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (preview.descriptor == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    if (width != nullptr) *width = preview.width;
    if (height != nullptr) *height = preview.height;
    const VkDescriptorSet descriptor = preview.descriptor;
    assetPreviewTextures_.emplace(key, std::move(preview));
    return descriptor;
}

uint64_t UiOverlay::nextTextureReleaseFrame() const {
    return uiFrameSerial_ + textureRetireFrameDelay_;
}

void UiOverlay::retireTextureDescriptor(VkDescriptorSet descriptor) {
    if (descriptor == VK_NULL_HANDLE) {
        return;
    }
    retiredTextureDescriptors_.push_back(RetiredTextureDescriptor{
        .descriptor = descriptor,
        .releaseFrame = nextTextureReleaseFrame(),
    });
}

void UiOverlay::retireAssetPreviewTexture(AssetPreviewTexture&& texture) {
    if (texture.descriptor == VK_NULL_HANDLE && texture.image.handle() == VK_NULL_HANDLE) {
        return;
    }
    RetiredAssetPreviewTexture retired{};
    retired.texture = std::move(texture);
    retired.releaseFrame = nextTextureReleaseFrame();
    retiredAssetPreviewTextures_.push_back(std::move(retired));
}

void UiOverlay::releaseRetiredTextures(bool force) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    retiredTextureDescriptors_.erase(
        std::remove_if(
            retiredTextureDescriptors_.begin(),
            retiredTextureDescriptors_.end(),
            [this, force](const RetiredTextureDescriptor& retired) {
                if (!force && uiFrameSerial_ < retired.releaseFrame) {
                    return false;
                }
                if (retired.descriptor != VK_NULL_HANDLE) {
                    ImGui_ImplVulkan_RemoveTexture(retired.descriptor);
                }
                return true;
            }),
        retiredTextureDescriptors_.end());

    retiredAssetPreviewTextures_.erase(
        std::remove_if(
            retiredAssetPreviewTextures_.begin(),
            retiredAssetPreviewTextures_.end(),
            [this, force](const RetiredAssetPreviewTexture& retired) {
                if (!force && uiFrameSerial_ < retired.releaseFrame) {
                    return false;
                }
                if (retired.texture.descriptor != VK_NULL_HANDLE) {
                    ImGui_ImplVulkan_RemoveTexture(retired.texture.descriptor);
                }
                return true;
            }),
        retiredAssetPreviewTextures_.end());
}

void UiOverlay::invalidateEditorTextures() {
    if (ImGui::GetCurrentContext() != nullptr) {
        for (const auto& entry : editorTextures_) {
            if (entry.second != VK_NULL_HANDLE) {
                retireTextureDescriptor(entry.second);
            }
        }
    }
    editorTextures_.clear();
}

void UiOverlay::invalidateAssetPreviewTextures() {
    if (ImGui::GetCurrentContext() != nullptr) {
        for (auto& entry : assetPreviewTextures_) {
            if (entry.second.descriptor != VK_NULL_HANDLE || entry.second.image.handle() != VK_NULL_HANDLE) {
                retireAssetPreviewTexture(std::move(entry.second));
            }
        }
    }
    assetPreviewTextures_.clear();
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
    colors[ImGuiCol_WindowBg] = editorWindowBgColor();
    colors[ImGuiCol_ChildBg] = editorChildBgColor();
    colors[ImGuiCol_PopupBg] = editorPopupBgColor();
    colors[ImGuiCol_Border] = editorBorderColor();
    colors[ImGuiCol_FrameBg] = editorFrameBgColor();
    colors[ImGuiCol_FrameBgHovered] = editorFrameBgHoveredColor();
    colors[ImGuiCol_FrameBgActive] = editorFrameBgActiveColor();
    colors[ImGuiCol_TitleBg] = editorTitleBgColor(false);
    colors[ImGuiCol_TitleBgActive] = editorTitleBgColor(true);
    colors[ImGuiCol_MenuBarBg] = editorMenuBarBgColor();
    colors[ImGuiCol_Tab] = editorTabColor(false);
    colors[ImGuiCol_TabHovered] = editorTabColor(false, true);
    colors[ImGuiCol_TabActive] = editorTabColor(true);
    colors[ImGuiCol_Header] = editorHeaderColor(false);
    colors[ImGuiCol_HeaderHovered] = editorHeaderColor(false, true);
    colors[ImGuiCol_HeaderActive] = editorHeaderColor(true);
    colors[ImGuiCol_Button] = editorButtonColor(false);
    colors[ImGuiCol_ButtonHovered] = editorButtonColor(false, true);
    colors[ImGuiCol_ButtonActive] = editorButtonColor(true);
    colors[ImGuiCol_CheckMark] = editorCheckMarkColor();
    colors[ImGuiCol_SliderGrab] = editorSliderGrabColor();
    colors[ImGuiCol_Separator] = editorSeparatorColor();
    colors[ImGuiCol_ResizeGrip] = editorResizeGripColor();
}

} // namespace rtv
