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
#include <cstdlib>
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

std::filesystem::path findEditorTablerIconFont() {
    std::filesystem::path cursor = std::filesystem::current_path();
    for (int depth = 0; depth < 8 && !cursor.empty(); ++depth) {
        const std::filesystem::path local = cursor / "third_party" / "tabler-icons" / "tabler-icons.ttf";
        if (std::filesystem::exists(local)) {
            return local;
        }
        const std::filesystem::path nested = cursor / "native" / "vulkan" / "third_party" / "tabler-icons" / "tabler-icons.ttf";
        if (std::filesystem::exists(nested)) {
            return nested;
        }

        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }
    return {};
}

std::filesystem::path findEditorUiTextFont(bool bold = false) {
#if defined(_WIN32)
    char* windir = nullptr;
    size_t windirLength = 0;
    if (_dupenv_s(&windir, &windirLength, "WINDIR") == 0 && windir != nullptr) {
        const std::filesystem::path fonts = std::filesystem::path(windir) / "Fonts";
        std::free(windir);
        const std::array<std::filesystem::path, 3> candidates = bold
            ? std::array<std::filesystem::path, 3>{{fonts / "segoeuib.ttf", fonts / "segoeui.ttf", fonts / "tahomabd.ttf"}}
            : std::array<std::filesystem::path, 3>{{fonts / "segoeui.ttf", fonts / "tahoma.ttf", fonts / "segoeuib.ttf"}};
        for (const std::filesystem::path& candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
    } else if (windir != nullptr) {
        std::free(windir);
    }
#endif
    return {};
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

const char* debugViewLabel(RendererDebugView view) {
    return rendererDebugViewName(view);
}

const std::array<RendererDebugView, 73>& rendererOnlyDebugViews() {
    static const std::array<RendererDebugView, 73> views{{
        RendererDebugView::Beauty,
        RendererDebugView::DirectLighting,
        RendererDebugView::IndirectLighting,
        RendererDebugView::Normals,
        RendererDebugView::Depth,
        RendererDebugView::MotionVectors,
        RendererDebugView::Albedo,
        RendererDebugView::Roughness,
        RendererDebugView::Variance,
        RendererDebugView::DenoiserRejection,
        RendererDebugView::NrdValidation,
        RendererDebugView::NrdDiffuseConfidence,
        RendererDebugView::NrdSpecularConfidence,
        RendererDebugView::NrdRawConfidenceGradient,
        RendererDebugView::NrdFilteredConfidenceGradient,
        RendererDebugView::NrdConfidenceHistory,
        RendererDebugView::PsrActiveMask,
        RendererDebugView::PsrDepth,
        RendererDebugView::PsrMotion,
        RendererDebugView::PsrNormalRoughness,
        RendererDebugView::PsrHitDistance,
        RendererDebugView::PsrAlbedoF0,
        RendererDebugView::PsrRayDirection,
        RendererDebugView::DlssDepth,
        RendererDebugView::DlssMotionVectors,
        RendererDebugView::DlssInputColor,
        RendererDebugView::DlssOutputColor,
        RendererDebugView::DlssRrDiffuseAlbedo,
        RendererDebugView::DlssRrSpecularAlbedo,
        RendererDebugView::DlssRrNormals,
        RendererDebugView::DlssRrRoughness,
        RendererDebugView::DlssRrDiffuseHitDistance,
        RendererDebugView::DlssRrSpecularHitDistance,
        RendererDebugView::DlssRrReflectedAlbedo,
        RendererDebugView::DlssRrDisocclusionMask,
        RendererDebugView::DlssRrDiffuseRayDirection,
        RendererDebugView::DlssRrSpecularRayDirection,
        RendererDebugView::DlssRrDiffuseRayDirectionHitDistance,
        RendererDebugView::DlssRrSpecularRayDirectionHitDistance,
        RendererDebugView::ReprojectionConfidence,
        RendererDebugView::RestirDiFinalContribution,
        RendererDebugView::RestirDiTemporalReservoir,
        RendererDebugView::RestirDiSpatialReservoir,
        RendererDebugView::RestirDiLightMapStatus,
        RendererDebugView::RestirGiFinal,
        RendererDebugView::RestirGiTemporal,
        RendererDebugView::RestirGiSpatial,
        RendererDebugView::RestirGiTarget,
        RendererDebugView::RestirGiSourcePdf,
        RendererDebugView::RestirGiWeightSum,
        RendererDebugView::RestirGiM,
        RendererDebugView::RestirGiConfidence,
        RendererDebugView::RestirGiVisibility,
        RendererDebugView::AdaptiveDensityMap,
        RendererDebugView::RegirGridOccupancy,
        RendererDebugView::RegirReservoirWeight,
        RendererDebugView::RegirSelectedLight,
        RendererDebugView::RegirQueryCount,
        RendererDebugView::RegirMisWeight,
        RendererDebugView::RegirEffectivePdf,
        RendererDebugView::RegirCanonicalUsed,
        RendererDebugView::RegirQueryCell,
        RendererDebugView::RegirActiveCellOccupancy,
        RendererDebugView::RegirHashCollisions,
        RendererDebugView::RegirSpatialInputWeight,
        RendererDebugView::RegirSpatialOutputWeight,
        RendererDebugView::RegirSpatialNeighborCount,
        RendererDebugView::RegirEnvironmentSource,
        RendererDebugView::RegirEnvironmentPdf,
        RendererDebugView::RegirEnvironmentDirection,
        RendererDebugView::RegirEnvironmentWeight,
        RendererDebugView::RegirEnvironmentGeneration,
        RendererDebugView::WavefrontQueueOccupancy,
    }};
    return views;
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
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoDefaultParent = true;
    io.IniFilename = "rtv_editor.ini";

    loadEditorFonts();
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
    initInfo.PipelineInfoForViewports.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
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
        const std::unordered_map<AssetGuid, MaterialAsset>* dirtyMaterialAssets,
        const std::unordered_map<AssetGuid, std::filesystem::path>* materialAssetAutosavePaths,
        bool sceneDirty,
        bool projectSettingsDirty,
        const std::vector<EntityId>* instanceEntities,
        const std::string& sceneLoadingStatus,
        bool sceneLoadRunning,
        float sceneLoadProgress,
        CameraController* camera,
        const UndoStack* undoStack,
        const EditorRenderJobStatus* renderJob,
        const EditorPlacementStatus* placement,
        const EditorJobCenterState* jobCenter,
        const std::vector<std::filesystem::path>* pendingDroppedFiles,
        float cpuFrameMs,
        NotificationManager* notifications,
        bool externalMouseCapture) {
    EditorRequests requests;
    rendererOnlyMode_ = false;
    rendererOnlyViewportHovered_ = false;
    rendererOnlyViewportActive_ = false;
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
        .dirtyMaterialAssets = dirtyMaterialAssets,
        .materialAssetAutosavePaths = materialAssetAutosavePaths,
        .sceneDirty = sceneDirty,
        .projectSettingsDirty = projectSettingsDirty,
        .instanceEntities = instanceEntities,
        .sceneLoadingStatus = &sceneLoadingStatus,
        .sceneLoadRunning = sceneLoadRunning,
        .sceneLoadProgress = sceneLoadProgress,
        .camera = camera,
        .undoStack = undoStack,
        .editorPreferencesPath = project != nullptr && !project->editorPreferencesPath.empty() ? project->editorPreferencesPath : EditorPreferences::defaultPath(),
        .uiTextures = EditorUiTextureProvider{.user = this, .acquire = &UiOverlay::acquireEditorTextureCallback, .acquireAssetPreview = &UiOverlay::acquireEditorAssetPreviewCallback},
        .renderJob = renderJob,
        .placement = placement,
        .jobCenter = jobCenter,
        .pendingDroppedFiles = pendingDroppedFiles,
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

RendererOnlyRequests UiOverlay::buildRendererOnly(
    PathTracerRenderer& renderer,
    VkExtent2D extent,
    const std::optional<std::filesystem::path>& gltfPath,
    const std::optional<std::filesystem::path>& scenePath,
    const std::optional<std::filesystem::path>& nativePackageScenePath,
    CameraController* camera,
    float cpuFrameMs,
    bool captureReadyPrinted,
    uint32_t captureReadyFrames,
    uint32_t captureReadyAfterFrames) {
    (void)extent;
    RendererOnlyRequests requests;
    rendererOnlyMode_ = true;
    rendererOnlyViewportHovered_ = false;
    rendererOnlyViewportActive_ = false;
    rendersPathTracerInViewport_ = true;
    if (!frameBegun_) {
        return requests;
    }

    const VkExtent2D renderExtent = renderer.renderExtent();
    const VkExtent2D displayExtent = renderer.displayExtent();
    const VkDescriptorImageInfo descriptor = renderer.viewportImageDescriptor();
    if (descriptor.imageView != VK_NULL_HANDLE && descriptor.imageView != viewportImageView_) {
        invalidateViewportTexture();
        viewportTexture_ = ImGui_ImplVulkan_AddTexture(descriptor.imageView, descriptor.imageLayout);
        descriptorPoolStats_.viewportDescriptorAllocated = viewportTexture_ != VK_NULL_HANDLE ? 1u : 0u;
        viewportImageView_ = descriptor.imageView;
        viewportTextureExtent_ = displayExtent;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags canvasFlags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("RendererOnlyViewport", nullptr, canvasFlags)) {
        const ImVec2 canvas = ImGui::GetContentRegionAvail();
        rendererOnlyViewportHovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        rendererOnlyViewportActive_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (viewportTexture_ != VK_NULL_HANDLE && canvas.x > 1.0f && canvas.y > 1.0f) {
            const float imageAspect = displayExtent.height > 0u
                ? static_cast<float>(displayExtent.width) / static_cast<float>(displayExtent.height)
                : 1.0f;
            ImVec2 imageSize = canvas;
            if (imageSize.y > 1.0f && imageSize.x / imageSize.y > imageAspect) {
                imageSize.x = imageSize.y * imageAspect;
            } else if (imageAspect > 0.0f) {
                imageSize.y = imageSize.x / imageAspect;
            }
            ImGui::SetCursorPos(ImVec2((canvas.x - imageSize.x) * 0.5f, (canvas.y - imageSize.y) * 0.5f));
            ImGui::Image(reinterpret_cast<ImTextureID>(viewportTexture_), imageSize);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    const float panelWidth = std::min(380.0f, std::max(300.0f, viewport->WorkSize.x * 0.26f));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - panelWidth - 12.0f, viewport->WorkPos.y + 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, std::min(viewport->WorkSize.y - 24.0f, 690.0f)), ImGuiCond_Always);
    ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Renderer", nullptr, panelFlags)) {
        const std::filesystem::path activePath = scenePath.value_or(gltfPath.value_or(nativePackageScenePath.value_or(std::filesystem::path{})));
        ImGui::TextUnformatted(activePath.empty() ? "Renderer Only" : activePath.filename().string().c_str());
        ImGui::Text("CPU %.2f ms  GPU %.2f ms", cpuFrameMs, renderer.timings().totalMs());
        ImGui::Text("%ux%u -> %ux%u", renderExtent.width, renderExtent.height, displayExtent.width, displayExtent.height);
        ImGui::Text("Capture %s (%u/%u)",
            captureReadyPrinted ? "ready" : "warming",
            std::min(captureReadyFrames, captureReadyAfterFrames),
            captureReadyAfterFrames);
        ImGui::Separator();

        RendererSettings settings = renderer.settings();
        bool changed = false;

        int preset = static_cast<int>(settings.renderPreset);
        const char* presetItems[] = {"Custom", "Low", "Balanced", "Ultra", "Native30"};
        if (ImGui::Combo("Preset", &preset, presetItems, IM_ARRAYSIZE(presetItems))) {
            settings.renderPreset = static_cast<RenderPreset>(preset);
            applyRenderPreset(settings, settings.renderPreset);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Render scale", &settings.renderResolutionScale, 0.25f, 1.0f, "%.2f");
        int spp = static_cast<int>(settings.samplesPerPixel);
        if (ImGui::SliderInt("SPP", &spp, 1, 8)) {
            settings.samplesPerPixel = static_cast<uint32_t>(std::max(1, spp));
            settings.renderPreset = RenderPreset::Custom;
            changed = true;
        }
        int bounces = static_cast<int>(settings.maxBounces);
        if (ImGui::SliderInt("Bounces", &bounces, 1, 16)) {
            settings.maxBounces = static_cast<uint32_t>(std::max(1, bounces));
            settings.renderPreset = RenderPreset::Custom;
            changed = true;
        }
        changed |= ImGui::Checkbox("Denoiser", &settings.denoiserEnabled);
        changed |= ImGui::Checkbox("TAA/TSR", &settings.taaEnabled);
        bool dlss = settings.temporalUpscaler == TemporalUpscaler::Dlss;
        if (ImGui::Checkbox("DLSS", &dlss)) {
            settings.temporalUpscaler = dlss ? TemporalUpscaler::Dlss : TemporalUpscaler::TaaTsr;
            changed = true;
        }
        changed |= ImGui::Checkbox("NvPerf", &settings.streamlineNvPerfEnabled);
        changed |= ImGui::Checkbox("ReSTIR GI", &settings.restirGiEnabled);
        changed |= ImGui::Checkbox("ReSTIR DI Temporal", &settings.restirDiTemporalEnabled);
        changed |= ImGui::Checkbox("ReSTIR DI Spatial", &settings.restirDiSpatialEnabled);

        const auto& debugViews = rendererOnlyDebugViews();
        int debugIndex = 0;
        for (size_t i = 0; i < debugViews.size(); ++i) {
            if (debugViews[i] == settings.debugView) {
                debugIndex = static_cast<int>(i);
                break;
            }
        }
        if (ImGui::BeginCombo("Debug view", debugViewLabel(debugViews[debugIndex]))) {
            for (size_t i = 0; i < debugViews.size(); ++i) {
                const bool selected = debugIndex == static_cast<int>(i);
                if (ImGui::Selectable(debugViewLabel(debugViews[i]), selected)) {
                    settings.debugView = debugViews[i];
                    settings.renderPreset = RenderPreset::Custom;
                    changed = true;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        int tone = static_cast<int>(settings.toneMapper);
        const char* toneItems[] = {"Linear", "Reinhard", "ReinhardWhite", "ACES", "PBRNeutral", "AgX"};
        if (ImGui::Combo("Tone map", &tone, toneItems, IM_ARRAYSIZE(toneItems))) {
            settings.toneMapper = static_cast<ToneMapper>(tone);
            settings.renderPreset = RenderPreset::Custom;
            changed = true;
        }
        changed |= ImGui::SliderFloat("Exposure", &settings.exposure, -8.0f, 8.0f, "%.2f");
        changed |= ImGui::Checkbox("Auto exposure", &settings.autoExposureEnabled);

        if (camera != nullptr) {
            float moveSpeed = camera->moveSpeed();
            float fastSpeed = camera->fastMoveSpeed();
            if (ImGui::SliderFloat("Camera speed", &moveSpeed, 0.1f, 25.0f, "%.2f")) {
                requests.cameraMoveSpeed = moveSpeed;
            }
            if (ImGui::SliderFloat("Fast speed", &fastSpeed, 0.5f, 75.0f, "%.2f")) {
                requests.cameraFastMoveSpeed = fastSpeed;
            }
        }

        if (changed) {
            requests.settings = settings;
            requests.resetAccumulation = AccumulationResetReason::RenderSettingsChanged;
        }

        ImGui::Separator();
        const GpuFrameTimings& timings = renderer.timings();
        ImGui::Text("PathTrace %.2f  DI %.2f/%.2f/%.2f",
            timings.pathTraceMs,
            timings.restirDiTemporalMs,
            timings.restirDiSpatialMs,
            timings.restirDiFinalMs);
        ImGui::Text("GI %.2f/%.2f/%.2f  Denoise %.2f",
            timings.restirGiTemporalMs,
            timings.restirGiSpatialMs,
            timings.restirGiFinalMs,
            timings.denoiserMs);
        ImGui::Text("TAA %.2f  ToneMap %.2f  Present %.2f",
            timings.taaMs,
            timings.toneMapMs,
            timings.fullscreenMs + timings.editorPresentationMs);

        ImGui::SeparatorText("Nsight Perf SDK");
        const NsightPerfMarkerStatus perfStatus = nsightPerfMarkerStatus();
        if (!rendererOnlyNsightOutputInitialized_) {
            const std::string initialOutput = perfStatus.configuredOutputDirectory.empty()
                ? std::string("out/nvperf")
                : perfStatus.configuredOutputDirectory;
            std::snprintf(rendererOnlyNsightOutput_.data(), rendererOnlyNsightOutput_.size(), "%s", initialOutput.c_str());
            rendererOnlyNsightDelayFrames_ = static_cast<int>(perfStatus.startAfterFrames);
            rendererOnlyNsightOutputInitialized_ = true;
        }
        ImGui::InputText("Report output", rendererOnlyNsightOutput_.data(), rendererOnlyNsightOutput_.size());
        ImGui::InputInt("Start delay", &rendererOnlyNsightDelayFrames_);
        rendererOnlyNsightDelayFrames_ = std::max(0, rendererOnlyNsightDelayFrames_);
        const char* perfState = perfStatus.collectionActive
            ? "collecting"
            : perfStatus.captureRequested
                ? "queued"
                : perfStatus.captureSucceeded
                    ? "complete"
                    : perfStatus.captureFailed
                        ? "failed"
                        : perfStatus.reportGeneratorInitialized
                            ? "ready"
                            : "unavailable";
        ImGui::Text("State: %s  frames: %llu", perfState, static_cast<unsigned long long>(perfStatus.framesObserved));
        ImGui::Text("Ranges: %llu/%llu  failures: %llu/%llu",
            static_cast<unsigned long long>(perfStatus.pushedRanges),
            static_cast<unsigned long long>(perfStatus.poppedRanges),
            static_cast<unsigned long long>(perfStatus.failedPushes),
            static_cast<unsigned long long>(perfStatus.failedPops));
        if (!perfStatus.unavailableReason.empty()) {
            ImGui::TextWrapped("%s", perfStatus.unavailableReason.c_str());
        }
        if (ImGui::Button("Start GPU Report")) {
            NsightPerfReportOptions options;
            options.outputDirectory = rendererOnlyNsightOutput_.data();
            options.startAfterFrames = static_cast<uint32_t>(rendererOnlyNsightDelayFrames_);
            options.nestingLevels = 2;
            options.enableHtmlReport = true;
            options.enableCsvReport = true;
            requests.startNsightPerfReport = std::move(options);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel Report")) {
            requests.cancelNsightPerfReport = true;
        }
        if (!perfStatus.lastReportDirectory.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open Report")) {
                requests.openNsightPerfReport = true;
            }
            ImGui::TextWrapped("%s", perfStatus.lastReportDirectory.c_str());
        }

        ImGui::Separator();
        if (ImGui::Button("Save PNG")) {
            requests.savePresentFrame = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Debug Views")) {
            requests.saveDebugViews = true;
        }
        if (ImGui::Button("Profile JSON")) {
            requests.dumpProfileJson = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Accum")) {
            requests.resetAccumulation = AccumulationResetReason::Manual;
        }
        if (ImGui::Button("Print CAPTURE_READY")) {
            requests.printCaptureReady = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Run Quick A/B")) {
            requests.runQuickExperimentMatrix = true;
        }
    }
    ImGui::End();

    ImGui::Render();
    return requests;
}

EditorRequests UiOverlay::buildProjectManager(
    const ProjectContext* project,
    const AssetRegistry* assetRegistry,
    const std::optional<std::filesystem::path>& scenePath,
    bool sceneDirty,
    bool projectSettingsDirty,
    size_t dirtyMaterialAssetCount,
    const std::string& sceneLoadingStatus,
    bool sceneLoadRunning,
    float sceneLoadProgress,
    const EditorJobCenterState* jobCenter,
    NotificationManager* notifications) {
    EditorRequests requests;
    rendererOnlyMode_ = false;
    rendererOnlyViewportHovered_ = false;
    rendererOnlyViewportActive_ = false;
    if (!frameBegun_) {
        return requests;
    }

    requests = editor_.drawProjectManagerLauncher(ProjectManagerRuntimeState{
        .project = project,
        .assetRegistry = assetRegistry,
        .scenePath = &scenePath,
        .sceneLoadingStatus = &sceneLoadingStatus,
        .sceneLoadRunning = sceneLoadRunning,
        .sceneLoadProgress = sceneLoadProgress,
        .sceneDirty = sceneDirty,
        .projectSettingsDirty = projectSettingsDirty,
        .dirtyMaterialAssetCount = dirtyMaterialAssetCount,
        .cookProjectRunning = jobCenter != nullptr && jobCenter->cookProjectRunning,
        .cookProjectOutputDir = jobCenter != nullptr ? jobCenter->cookProjectOutputDir : std::filesystem::path{},
        .completedCookProjectSerial = jobCenter != nullptr ? jobCenter->completedCookProjectSerial : 0,
        .completedCookProjectSuccess = jobCenter != nullptr && jobCenter->completedCookProjectSuccess,
        .completedCookProjectStatus = jobCenter != nullptr ? jobCenter->completedCookProjectStatus : std::string{},
        .completedCookProjectOutputDir = jobCenter != nullptr ? jobCenter->completedCookProjectOutputDir : std::filesystem::path{},
        .completedCookProjectManifestPath = jobCenter != nullptr ? jobCenter->completedCookProjectManifestPath : std::filesystem::path{},
        .completedCookProjectValidationReportPath = jobCenter != nullptr ? jobCenter->completedCookProjectValidationReportPath : std::filesystem::path{},
        .completedCookProjectLogPath = jobCenter != nullptr ? jobCenter->completedCookProjectLogPath : std::filesystem::path{},
        .completedCookProjectExitCode = jobCenter != nullptr ? jobCenter->completedCookProjectExitCode : 0,
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

void UiOverlay::renderPlatformWindows() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) {
        return;
    }
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
}

void UiOverlay::onSwapchainRecreated(const Swapchain& swapchain) {
    colorAttachmentFormat_ = swapchain.format();
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
    return rendererOnlyViewportActive_ || editor_.viewportInteractionActive();
}

bool UiOverlay::viewportHovered() const {
    return rendererOnlyViewportHovered_ || editor_.viewportHovered();
}

VkExtent2D UiOverlay::desiredRenderExtent(VkExtent2D fallback) const {
    if (renderExtentOverride_.has_value() && renderExtentOverride_->width > 0u && renderExtentOverride_->height > 0u) {
        return *renderExtentOverride_;
    }
    if (rendererOnlyMode_) {
        return fallback;
    }
    VkExtent2D extent = editor_.desiredRenderExtent(fallback);
    if (extent.width == 0 || extent.height == 0) {
        extent = fallback;
    }
    return extent;
}

void UiOverlay::setRenderExtentOverride(std::optional<VkExtent2D> extent) {
    const bool changed = renderExtentOverride_.has_value() != extent.has_value() ||
        (extent.has_value() &&
            (renderExtentOverride_->width != extent->width || renderExtentOverride_->height != extent->height));
    renderExtentOverride_ = extent;
    if (changed) {
        invalidateViewportTexture();
    }
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

void UiOverlay::invalidateRendererTextures() {
    invalidateViewportTexture();
    invalidateEditorTextures();
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

void UiOverlay::loadEditorFonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return;
    }

    const std::filesystem::path textFontPath = findEditorUiTextFont();
    if (!textFontPath.empty()) {
        ImFontConfig textConfig{};
        textConfig.PixelSnapH = true;
        textConfig.OversampleH = 2;
        textConfig.OversampleV = 1;
        if (ImFont* textFont = io.Fonts->AddFontFromFileTTF(textFontPath.string().c_str(), 15.0f, &textConfig, io.Fonts->GetGlyphRangesDefault())) {
            io.FontDefault = textFont;
        }
    }

    editorSetHeadingFont(nullptr);
    const std::filesystem::path headingFontPath = findEditorUiTextFont(true);
    if (!headingFontPath.empty()) {
        ImFontConfig headingConfig{};
        headingConfig.PixelSnapH = true;
        headingConfig.OversampleH = 2;
        headingConfig.OversampleV = 1;
        editorSetHeadingFont(io.Fonts->AddFontFromFileTTF(
            headingFontPath.string().c_str(),
            15.0f,
            &headingConfig,
            io.Fonts->GetGlyphRangesDefault()));
    }

    if (io.Fonts->Fonts.empty()) {
        io.Fonts->AddFontDefault();
    }

    const std::filesystem::path fontPath = findEditorTablerIconFont();
    if (fontPath.empty()) {
        editorSetTablerIconFont(nullptr);
        std::cerr << "[UiOverlay] Tabler icon font not found; falling back to built-in editor glyphs.\n";
        return;
    }

    ImFontConfig iconConfig{};
    iconConfig.PixelSnapH = true;
    iconConfig.OversampleH = 1;
    iconConfig.OversampleV = 1;
    iconConfig.GlyphMinAdvanceX = 13.0f;
    iconConfig.GlyphMaxAdvanceX = 18.0f;
    static constexpr ImWchar tablerRanges[] = {0xEA00, 0xFFFF, 0};
    ImFont* iconFont = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 18.0f, &iconConfig, tablerRanges);
    if (iconFont == nullptr) {
        editorSetTablerIconFont(nullptr);
        std::cerr << "[UiOverlay] Failed to load Tabler icon font from " << fontPath.string() << "; falling back to built-in editor glyphs.\n";
        return;
    }

    editorSetTablerIconFont(iconFont);
}

void UiOverlay::applyDarkStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(EditorUiMetric::panelPaddingX, EditorUiMetric::panelPaddingY);
    style.FramePadding = ImVec2(EditorUiMetric::rowPaddingX, EditorUiMetric::rowPaddingY);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 11.0f;
    style.WindowRounding = 0.0f;
    style.FrameRounding = EditorUiMetric::compactButtonRounding;
    style.GrabRounding = EditorUiMetric::compactButtonRounding;
    style.TabRounding = EditorUiMetric::dockTabRounding;
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
