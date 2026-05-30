#include "rtv/PathTracerRenderer.h"

#include "rtv/AtmosphereLutSystem.h"
#include "rtv/AtmosphereSamplingSystem.h"
#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/ComputePipeline.h"
#include "rtv/DescriptorLayoutCache.h"
#include "rtv/DescriptorWriter.h"
#include "rtv/GraphicsPipeline.h"
#include "rtv/ImageBarrier.h"
#include "rtv/PipelineCache.h"
#include "rtv/RayTracingPipeline.h"
#include "rtv/RayTracingScene.h"
#include "rtv/RenderGraph.h"
#include "rtv/RenderGraphDump.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ShaderCompiler.h"
#include "rtv/ShaderModule.h"
#include "rtv/ShaderReflection.h"
#include "rtv/TemporalSystem.h"
#include "rtv/VulkanContext.h"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace rtv {

namespace {

constexpr uint32_t kHistogramBinCount = 128;
constexpr VkDeviceSize kFrameCameraUniformOffset = 0;
constexpr VkDeviceSize kFramePrevCameraUniformOffset = 4096;
constexpr VkDeviceSize kFrameDenoiserParamsOffset = 8192;
constexpr VkDeviceSize kFrameDebugParamsOffset = 12288;
constexpr VkDeviceSize kFrameTaaParamsOffset = 16384;
constexpr VkDeviceSize kFrameRestirSpatialParamsOffset = 20480;
constexpr VkDeviceSize kFrameFogParamsOffset = 24576;
constexpr VkDeviceSize kFrameMomentParamsOffset = 28672;
constexpr uint32_t kRendererFramesInFlight = 3;
constexpr VkPipelineStageFlags2 kCrossQueueShaderStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
constexpr uint32_t kRayTracingDiagnosticCounterCount = 14;
constexpr uint32_t kWavefrontQueueClearValidationValue = 0x57465131u; // WFQ1

void bufferMemoryBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags2 srcStage,
    VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage,
    VkAccessFlags2 dstAccess,
    VkBuffer buffer,
    VkDeviceSize size,
    VkDeviceSize offset = 0) {
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = offset;
    barrier.size = size;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

float halton(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index > 0) {
        result += fraction * static_cast<float>(index % base);
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

std::filesystem::path glslangPath() {
#if defined(_MSC_VER)
    char* sdk = nullptr;
    size_t sdkLength = 0;
    _dupenv_s(&sdk, &sdkLength, "VULKAN_SDK");
    if (sdk == nullptr) {
        throw std::runtime_error("VULKAN_SDK is not set; cannot locate glslangValidator");
    }
    std::filesystem::path result = std::filesystem::path(sdk) / "Bin" / "glslangValidator.exe";
    std::free(sdk);
    return result;
#else
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk == nullptr) {
        throw std::runtime_error("VULKAN_SDK is not set; cannot locate glslangValidator");
    }
    return std::filesystem::path(sdk) / "Bin" / "glslangValidator";
#endif
}

VkDescriptorSetLayoutBinding descriptorBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages, uint32_t count = 1) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = type;
    result.descriptorCount = count;
    result.stageFlags = stages;
    return result;
}

bool isWavefrontDebugView(RendererDebugView view) {
    return view == RendererDebugView::WavefrontQueueOccupancy ||
        view == RendererDebugView::WavefrontPathDepth ||
        view == RendererDebugView::WavefrontLiveRays ||
        view == RendererDebugView::WavefrontTerminatedRays ||
        view == RendererDebugView::WavefrontMaterialBucket ||
        view == RendererDebugView::WavefrontRestirDi ||
        view == RendererDebugView::WavefrontDirectLighting ||
        view == RendererDebugView::WavefrontRestirGi;
}

bool requiresWavefrontShadowTrace(RendererDebugView view) {
    return view == RendererDebugView::WavefrontDirectLighting;
}

std::vector<VkDescriptorSetLayoutBinding> rayTracingBindings() {
    constexpr VkShaderStageFlags allRt =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR |
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    return {
        descriptorBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allRt),
        descriptorBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allRt),
        descriptorBinding(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(13, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(33, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(34, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(35, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(36, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(37, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allRt),
        descriptorBinding(38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(39, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(40, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(41, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, allRt, 1024),
        descriptorBinding(42, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(43, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(44, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(45, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(46, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(47, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(48, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, allRt),
        descriptorBinding(49, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(50, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(51, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(52, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptorBinding(54, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR),
    };
}

std::vector<VkDescriptorSetLayoutBinding> wavefrontShadeBindings() {
    std::vector<VkDescriptorSetLayoutBinding> bindings = rayTracingBindings();
    for (VkDescriptorSetLayoutBinding& binding : bindings) {
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings.push_back(descriptorBinding(53, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT));
    return bindings;
}

bool restirGiUncompressedLayoutFromEnvironment() {
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT");
    const bool enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv("RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
#endif
}

} // namespace

const char* accumulationResetReasonName(AccumulationResetReason reason) {
    switch (reason) {
    case AccumulationResetReason::Startup: return "Startup";
    case AccumulationResetReason::Resize: return "Resize";
    case AccumulationResetReason::CameraMoved: return "CameraMoved";
    case AccumulationResetReason::Manual: return "Manual";
    case AccumulationResetReason::RenderSettingsChanged: return "RenderSettingsChanged";
    case AccumulationResetReason::LightingChanged: return "LightingChanged";
    case AccumulationResetReason::EnvironmentChanged: return "EnvironmentChanged";
    case AccumulationResetReason::DenoiserSettingsChanged: return "DenoiserChanged";
    case AccumulationResetReason::DebugViewChanged: return "DebugViewChanged";
    case AccumulationResetReason::SceneChanged: return "SceneChanged";
    case AccumulationResetReason::MaterialChanged: return "MaterialChanged";
    case AccumulationResetReason::ShaderReloaded: return "ShaderReloaded";
    }
    return "unknown";
}

PathTracerRenderer::PathTracerRenderer(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    VkFormat swapchainFormat,
    const std::filesystem::path& shaderDirectory,
    const std::filesystem::path& shaderOutputDirectory,
    RendererDebugView debugView,
    const SceneAsset* importedScene,
    const AssetManager* assets,
    std::optional<std::filesystem::path> environmentPath,
    std::optional<std::filesystem::path> sceneCachePath,
    bool resourceAliasingEnabled,
    const RendererSettings* initialSettings)
    : context_(context),
      allocator_(allocator),
      uploader_(uploader),
      scene_(
          allocator,
          uploader,
          importedScene,
          assets,
          std::move(environmentPath),
          std::move(sceneCachePath),
          initialSettings != nullptr
              ? initialSettings->opacityMicromapSubdivisionLevel
              : kDefaultOpacityMicromapSubdivisionLevel),
      resourceAliasingEnabled_(resourceAliasingEnabled) {
    temporalSystem_ = std::make_unique<TemporalSystem>();
    if (!context_.supportsHardwareRayTracing()) {
        throw std::runtime_error("Hardware ray tracing is required but this Vulkan device does not support the required KHR ray tracing features/extensions");
    }
    if (initialSettings != nullptr) {
        settings_ = *initialSettings;
    }
    if (!context_.supportsOpacityMicromaps()) {
        settings_.opacityMicromapsEnabled = false;
    }
    if (!context_.supportsSER()) {
        settings_.shaderExecutionReorderingEnabled = false;
    }
    if (!context_.supportsRayTracingMotionBlur()) {
        settings_.motionBlurEnabled = false;
    }
    if (isWavefrontDebugView(debugView)) {
        settings_.wavefrontShadeEnabled = true;
    }
    if (requiresWavefrontShadowTrace(debugView)) {
        settings_.wavefrontShadowTraceEnabled = true;
    }
    if (debugView == RendererDebugView::CausticVisibility) {
        settings_.mneeCausticsEnabled = true;
    }
    if (settings_.wavefrontShadowTraceEnabled) {
        settings_.wavefrontShadeEnabled = true;
    }
    if (settings_.wavefrontCompactEnabled) {
        settings_.wavefrontShadeEnabled = true;
    }
    if (settings_.wavefrontSortEnabled) {
        settings_.wavefrontCompactEnabled = true;
        settings_.wavefrontShadeEnabled = true;
    }
    if (settings_.wavefrontFinalOutputEnabled) {
        settings_.wavefrontShadowTraceEnabled = true;
        settings_.wavefrontCompactEnabled = true;
        settings_.wavefrontShadeEnabled = true;
    }
    if (settings_.wavefrontShadeEnabled) {
        settings_.wavefrontTraceEnabled = true;
    }
    if (settings_.wavefrontTraceEnabled) {
        settings_.wavefrontPrimaryGenerateEnabled = true;
    }
    if (settings_.wavefrontPrimaryGenerateEnabled) {
        settings_.wavefrontQueuesEnabled = true;
    }
    std::cout << "Renderer backend: hardware ray tracing\n";
    restirGiUncompressedLayout_ = restirGiUncompressedLayoutFromEnvironment();
    if (restirGiUncompressedLayout_) {
        std::cout << "ReSTIR GI reservoir layout: uncompressed validation\n";
    }

    shaderCompiler_ = std::make_unique<ShaderCompiler>(glslangPath());
    shaderOutputDirectory_ = shaderOutputDirectory;
    ShaderCompiler& compiler = *shaderCompiler_;
    const auto denoiserSpv = compiler.compileIfNeeded(shaderDirectory / "denoiser.comp", shaderOutputDirectory);
    const auto momentUpdateSpv = compiler.compileIfNeeded(shaderDirectory / "moment_update.comp", shaderOutputDirectory);
    const auto taaSpv = compiler.compileIfNeeded(shaderDirectory / "taa.comp", shaderOutputDirectory);
    const auto restirSpatialSpv = compiler.compileIfNeeded(shaderDirectory / "restir_spatial.comp", shaderOutputDirectory);
    const auto restirGiSpatialSpv = compiler.compileIfNeeded(shaderDirectory / "restir_gi_spatial.comp", shaderOutputDirectory);
    const auto restirGiFinalSpv = compiler.compileIfNeeded(shaderDirectory / "restir_gi_final.comp", shaderOutputDirectory);
    const auto fogSpv = compiler.compileIfNeeded(shaderDirectory / "fog_integrate.comp", shaderOutputDirectory);
    const auto transmittanceSpv = compiler.compileIfNeeded(shaderDirectory / "transmittance_lut.comp", shaderOutputDirectory);
    const auto multiScatterSpv = compiler.compileIfNeeded(shaderDirectory / "multi_scatter_lut.comp", shaderOutputDirectory);
    const auto skyViewSpv = compiler.compileIfNeeded(shaderDirectory / "sky_view_lut.comp", shaderOutputDirectory);
    const auto skyReprojectSpv = compiler.compileIfNeeded(shaderDirectory / "sky_reproject.comp", shaderOutputDirectory);
    const auto aerialPerspectiveSpv = compiler.compileIfNeeded(shaderDirectory / "aerial_perspective_lut.comp", shaderOutputDirectory);
    const auto skyCdfSpv = compiler.compileIfNeeded(shaderDirectory / "sky_cdf.comp", shaderOutputDirectory);
    const auto selectionSpv = compiler.compileIfNeeded(shaderDirectory / "selection_outline.comp", shaderOutputDirectory);
    const auto histogramSpv = compiler.compileIfNeeded(shaderDirectory / "luminance_histogram.comp", shaderOutputDirectory);
    const auto exposureSpv = compiler.compileIfNeeded(shaderDirectory / "exposure_reduce.comp", shaderOutputDirectory);
    const auto toneMapSpv = compiler.compileIfNeeded(shaderDirectory / "tone_map.comp", shaderOutputDirectory);
    const auto wavefrontQueueClearSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_queue_clear.comp", shaderOutputDirectory);
    const auto wavefrontPrimaryGenerateSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_generate.comp", shaderOutputDirectory);
    const auto wavefrontShadeSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_shade.comp", shaderOutputDirectory);
    const auto wavefrontDebugWriteSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_write.comp", shaderOutputDirectory);
    const auto wavefrontCompactSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_compact.comp", shaderOutputDirectory);
    const auto wavefrontSortSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_sort.comp", shaderOutputDirectory);
    const auto wavefrontTraceValidateSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_trace_validate.comp", shaderOutputDirectory);
    const auto wavefrontDirectValidateSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_direct_validate.comp", shaderOutputDirectory);
    const auto vertSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.vert", shaderOutputDirectory);
    const auto fragSpv = compiler.compileIfNeeded(shaderDirectory / "fullscreen.frag", shaderOutputDirectory);

    denoiserShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(denoiserSpv), "temporal denoiser compute");
    momentUpdateShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(momentUpdateSpv), "moment update compute");
    taaShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(taaSpv), "taa compute");
    restirSpatialShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(restirSpatialSpv), "restir spatial compute");
    restirGiSpatialShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(restirGiSpatialSpv), "restir gi spatial compute");
    restirGiFinalShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(restirGiFinalSpv), "restir gi final compute");
    fogShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(fogSpv), "height fog integrate compute");
    transmittanceShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(transmittanceSpv), "atmosphere transmittance lut compute");
    multiScatterShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(multiScatterSpv), "atmosphere multi scatter lut compute");
    skyViewShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(skyViewSpv), "atmosphere sky view lut compute");
    skyReprojectShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(skyReprojectSpv), "atmosphere sky reproject compute");
    aerialPerspectiveShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(aerialPerspectiveSpv), "atmosphere aerial perspective lut compute");
    skyCdfShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(skyCdfSpv), "atmosphere sky CDF compute");
    selectionOutlineShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(selectionSpv), "selection outline compute");
    luminanceHistogramShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(histogramSpv), "luminance histogram compute");
    exposureReduceShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(exposureSpv), "exposure reduce compute");
    toneMapShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(toneMapSpv), "tone map compute");
    wavefrontQueueClearShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontQueueClearSpv), "wavefront queue clear compute");
    wavefrontPrimaryGenerateShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontPrimaryGenerateSpv), "wavefront primary generate compute");
    wavefrontShadeShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontShadeSpv), "wavefront shade compute");
    wavefrontDebugWriteShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontDebugWriteSpv), "wavefront debug write compute");
    wavefrontCompactShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontCompactSpv), "wavefront queue compact compute");
    wavefrontSortShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontSortSpv), "wavefront queue sort compute");
    wavefrontTraceValidateShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontTraceValidateSpv), "wavefront trace validate compute");
    wavefrontDirectLightingValidateShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontDirectValidateSpv), "wavefront direct lighting validate compute");
    fullscreenVertexShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(vertSpv), "fullscreen vertex");
    fullscreenFragmentShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(fragSpv), "fullscreen fragment");
    const auto raygenSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rgen", shaderOutputDirectory);
    std::optional<std::filesystem::path> raygenMotionSpv;
    if (context_.supportsRayTracingMotionBlur()) {
        raygenMotionSpv = compiler.compileIfNeeded(
            shaderDirectory / "pathtrace.rgen",
            shaderOutputDirectory,
            ".motion",
            {{"RTV_MOTION_BLUR_ENABLED", "1"}});
    }
    const auto wavefrontTraceRaygenSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_trace.rgen", shaderOutputDirectory);
    std::optional<std::filesystem::path> wavefrontTraceRaygenSerSpv;
    if (context_.supportsSER()) {
        wavefrontTraceRaygenSerSpv = compiler.compileIfNeeded(
            shaderDirectory / "wavefront_trace.rgen",
            shaderOutputDirectory,
            ".ser",
            {{"RTV_SER_ENABLED", "1"}});
    }
    const auto wavefrontShadowTraceRaygenSpv = compiler.compileIfNeeded(shaderDirectory / "wavefront_shadow_trace.rgen", shaderOutputDirectory);
    const auto missSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rmiss", shaderOutputDirectory);
    const auto shadowMissSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace_shadow.rmiss", shaderOutputDirectory);
    const auto closestHitSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rchit", shaderOutputDirectory);
    const auto primaryAnyHitSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace.rahit", shaderOutputDirectory);
    const auto shadowAnyHitSpv = compiler.compileIfNeeded(shaderDirectory / "pathtrace_shadow.rahit", shaderOutputDirectory);
    raygenShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(raygenSpv), "path trace raygen");
    if (raygenMotionSpv.has_value()) {
        raygenMotionShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(*raygenMotionSpv), "path trace raygen motion blur");
    }
    wavefrontTraceRaygenShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontTraceRaygenSpv), "wavefront trace raygen");
    if (wavefrontTraceRaygenSerSpv.has_value()) {
        wavefrontTraceRaygenSerShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(*wavefrontTraceRaygenSerSpv), "wavefront trace raygen ser");
    }
    wavefrontShadowTraceRaygenShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(wavefrontShadowTraceRaygenSpv), "wavefront shadow trace raygen");
    primaryMissShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(missSpv), "path trace primary miss");
    shadowMissShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(shadowMissSpv), "path trace shadow miss");
    closestHitShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(closestHitSpv), "path trace closest hit");
    primaryAnyHitShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(primaryAnyHitSpv), "path trace primary any-hit");
    shadowAnyHitShader_ = std::make_unique<ShaderModule>(context_.device(), ShaderCompiler::readSpirv(shadowAnyHitSpv), "path trace shadow any-hit");

    shaderSources_ = {
        shaderDirectory / "denoiser.comp",
        shaderDirectory / "moment_update.comp",
        shaderDirectory / "taa.comp",
        shaderDirectory / "restir_spatial.comp",
        shaderDirectory / "restir_gi_spatial.comp",
        shaderDirectory / "restir_gi_final.comp",
        shaderDirectory / "fog_integrate.comp",
        shaderDirectory / "transmittance_lut.comp",
        shaderDirectory / "multi_scatter_lut.comp",
        shaderDirectory / "sky_view_lut.comp",
        shaderDirectory / "sky_reproject.comp",
        shaderDirectory / "aerial_perspective_lut.comp",
        shaderDirectory / "sky_cdf.comp",
        shaderDirectory / "selection_outline.comp",
        shaderDirectory / "luminance_histogram.comp",
        shaderDirectory / "exposure_reduce.comp",
        shaderDirectory / "tone_map.comp",
        shaderDirectory / "wavefront_queue_clear.comp",
        shaderDirectory / "wavefront_generate.comp",
        shaderDirectory / "wavefront_shade.comp",
        shaderDirectory / "wavefront_write.comp",
        shaderDirectory / "wavefront_compact.comp",
        shaderDirectory / "wavefront_sort.comp",
        shaderDirectory / "wavefront_trace_validate.comp",
        shaderDirectory / "wavefront_direct_validate.comp",
        shaderDirectory / "fullscreen.vert",
        shaderDirectory / "fullscreen.frag",
        shaderDirectory / "pathtrace.rgen",
        shaderDirectory / "wavefront_trace.rgen",
        shaderDirectory / "wavefront_shadow_trace.rgen",
        shaderDirectory / "pathtrace.rmiss",
        shaderDirectory / "pathtrace_shadow.rmiss",
        shaderDirectory / "pathtrace.rchit",
        shaderDirectory / "pathtrace.rahit",
        shaderDirectory / "pathtrace_shadow.rahit",
    };

    layoutCache_ = std::make_unique<DescriptorLayoutCache>(context_.device());
    pipelineCache_ = std::make_unique<PipelineCache>(context_.device(), shaderOutputDirectory / "pipeline_cache.bin");
    atmosphereLutSystem_ = std::make_unique<AtmosphereLutSystem>(
        context_.device(),
        allocator_,
        *layoutCache_,
        *pipelineCache_,
        *transmittanceShader_,
        *multiScatterShader_,
        *skyViewShader_,
        *skyReprojectShader_,
        *aerialPerspectiveShader_,
        *skyCdfShader_);
    auto atmosphereBindings = ShaderReflection::bindingsForSet({raygenShader_->reflection()}, 1);
    for (VkDescriptorSetLayoutBinding& binding : atmosphereBindings) {
        binding.stageFlags = VK_SHADER_STAGE_ALL;
    }
    atmosphereSetLayout_ = layoutCache_->createLayout(std::move(atmosphereBindings));
    denoiserSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({denoiserShader_->reflection()}, 0));
    momentUpdateSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({momentUpdateShader_->reflection()}, 0));
    taaSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({taaShader_->reflection()}, 0));
    restirSpatialSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({restirSpatialShader_->reflection()}, 0));
    restirGiSpatialSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({restirGiSpatialShader_->reflection()}, 0));
    restirGiFinalSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({restirGiFinalShader_->reflection()}, 0));
    fogSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({fogShader_->reflection()}, 0));
    selectionOutlineSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({selectionOutlineShader_->reflection()}, 0));
    luminanceHistogramSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({luminanceHistogramShader_->reflection()}, 0));
    exposureReduceSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({exposureReduceShader_->reflection()}, 0));
    toneMapSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({toneMapShader_->reflection()}, 0));
    wavefrontQueueClearSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({wavefrontQueueClearShader_->reflection()}, 0));
    wavefrontPrimaryGenerateSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({wavefrontPrimaryGenerateShader_->reflection()}, 0));
    {
        std::vector<VkDescriptorBindingFlags> shadeBindingFlags(wavefrontShadeBindings().size(), 0);
        const auto shadeBindings = wavefrontShadeBindings();
        const BindlessCapabilities& shadeBindless = context_.bindlessCapabilities();
        for (size_t i = 0; i < shadeBindings.size(); ++i) {
            if (shadeBindings[i].binding == 41) {
                if (shadeBindless.partiallyBound) {
                    shadeBindingFlags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
                }
                if (shadeBindless.updateAfterBind) {
                    shadeBindingFlags[i] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                }
            }
        }
        const VkDescriptorSetLayoutCreateFlags shadeLayoutFlags =
            shadeBindless.updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        wavefrontShadeSetLayout_ = layoutCache_->createLayout(wavefrontShadeBindings(), shadeLayoutFlags, std::move(shadeBindingFlags));
    }
    wavefrontTraceValidateSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({wavefrontTraceValidateShader_->reflection()}, 0));
    wavefrontDirectLightingValidateSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({wavefrontDirectLightingValidateShader_->reflection()}, 0));
    wavefrontDebugWriteSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({wavefrontDebugWriteShader_->reflection()}, 0));
    wavefrontCompactSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({wavefrontCompactShader_->reflection()}, 0));
    wavefrontSortSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({wavefrontSortShader_->reflection()}, 0));
    graphicsSetLayout_ = layoutCache_->createLayout(ShaderReflection::bindingsForSet({
        fullscreenVertexShader_->reflection(),
        fullscreenFragmentShader_->reflection(),
    }, 0));
    std::vector<VkDescriptorBindingFlags> rtBindingFlags(rayTracingBindings().size(), 0);
    const auto rtBindings = rayTracingBindings();
    const BindlessCapabilities& rtBindless = context_.bindlessCapabilities();
    for (size_t i = 0; i < rtBindings.size(); ++i) {
        if (rtBindings[i].binding == 41) {
            if (rtBindless.partiallyBound) {
                rtBindingFlags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            }
            if (rtBindless.updateAfterBind) {
                rtBindingFlags[i] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            }
        }
    }
    const VkDescriptorSetLayoutCreateFlags rtLayoutFlags =
        rtBindless.updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
    rayTracingSetLayout_ = layoutCache_->createLayout(rayTracingBindings(), rtLayoutFlags, std::move(rtBindingFlags));

    denoiserPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *denoiserShader_,
        std::vector<VkDescriptorSetLayout>{denoiserSetLayout_},
        ShaderReflection::mergePushConstants({denoiserShader_->reflection()}),
        *pipelineCache_);
    momentUpdatePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *momentUpdateShader_,
        std::vector<VkDescriptorSetLayout>{momentUpdateSetLayout_},
        ShaderReflection::mergePushConstants({momentUpdateShader_->reflection()}),
        *pipelineCache_);
    taaPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *taaShader_,
        std::vector<VkDescriptorSetLayout>{taaSetLayout_},
        ShaderReflection::mergePushConstants({taaShader_->reflection()}),
        *pipelineCache_);
    restirSpatialPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *restirSpatialShader_,
        std::vector<VkDescriptorSetLayout>{restirSpatialSetLayout_},
        ShaderReflection::mergePushConstants({restirSpatialShader_->reflection()}),
        *pipelineCache_);
    restirGiSpatialPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *restirGiSpatialShader_,
        std::vector<VkDescriptorSetLayout>{restirGiSpatialSetLayout_},
        ShaderReflection::mergePushConstants({restirGiSpatialShader_->reflection()}),
        *pipelineCache_);
    restirGiFinalPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *restirGiFinalShader_,
        std::vector<VkDescriptorSetLayout>{restirGiFinalSetLayout_},
        ShaderReflection::mergePushConstants({restirGiFinalShader_->reflection()}),
        *pipelineCache_);
    fogPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *fogShader_,
        std::vector<VkDescriptorSetLayout>{fogSetLayout_, atmosphereSetLayout_},
        ShaderReflection::mergePushConstants({fogShader_->reflection()}),
        *pipelineCache_);
    selectionOutlinePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *selectionOutlineShader_,
        std::vector<VkDescriptorSetLayout>{selectionOutlineSetLayout_},
        ShaderReflection::mergePushConstants({selectionOutlineShader_->reflection()}),
        *pipelineCache_);
    luminanceHistogramPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *luminanceHistogramShader_,
        std::vector<VkDescriptorSetLayout>{luminanceHistogramSetLayout_},
        ShaderReflection::mergePushConstants({luminanceHistogramShader_->reflection()}),
        *pipelineCache_);
    exposureReducePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *exposureReduceShader_,
        std::vector<VkDescriptorSetLayout>{exposureReduceSetLayout_},
        ShaderReflection::mergePushConstants({exposureReduceShader_->reflection()}),
        *pipelineCache_);
    toneMapPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *toneMapShader_,
        std::vector<VkDescriptorSetLayout>{toneMapSetLayout_},
        ShaderReflection::mergePushConstants({toneMapShader_->reflection()}),
        *pipelineCache_);
    wavefrontQueueClearPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontQueueClearShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontQueueClearSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontQueueClearShader_->reflection()}),
        *pipelineCache_);
    wavefrontPrimaryGeneratePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontPrimaryGenerateShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontPrimaryGenerateSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontPrimaryGenerateShader_->reflection()}),
        *pipelineCache_);
    wavefrontShadePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontShadeShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontShadeSetLayout_, atmosphereSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontShadeShader_->reflection()}),
        *pipelineCache_);
    wavefrontTraceValidatePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontTraceValidateShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontTraceValidateSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontTraceValidateShader_->reflection()}),
        *pipelineCache_);
    wavefrontDirectLightingValidatePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontDirectLightingValidateShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontDirectLightingValidateSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontDirectLightingValidateShader_->reflection()}),
        *pipelineCache_);
    wavefrontDebugWritePipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontDebugWriteShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontDebugWriteSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontDebugWriteShader_->reflection()}),
        *pipelineCache_);
    wavefrontCompactPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontCompactShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontCompactSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontCompactShader_->reflection()}),
        *pipelineCache_);
    wavefrontSortPipeline_ = std::make_unique<ComputePipeline>(
        context_.device(),
        *wavefrontSortShader_,
        std::vector<VkDescriptorSetLayout>{wavefrontSortSetLayout_},
        ShaderReflection::mergePushConstants({wavefrontSortShader_->reflection()}),
        *pipelineCache_);
    graphicsPipeline_ = std::make_unique<GraphicsPipeline>(
        context_.device(),
        swapchainFormat,
        *fullscreenVertexShader_,
        *fullscreenFragmentShader_,
        std::vector<VkDescriptorSetLayout>{graphicsSetLayout_},
        ShaderReflection::mergePushConstants({
            fullscreenVertexShader_->reflection(),
            fullscreenFragmentShader_->reflection(),
        }),
        *pipelineCache_);
    if (true) {
        rayTracingScene_ = std::make_unique<RayTracingScene>(
            context_,
            allocator_,
            uploader_,
            scene_,
            RayTracingSceneBuildOptions{
                .opacityMicromapsEnabled = settings_.opacityMicromapsEnabled,
                .motionBlurEnabled = settings_.motionBlurEnabled});
        rayTracingPipeline_ = std::make_unique<RayTracingPipeline>(
            context_.device(),
            context_.rayTracingInfo().rayTracingPipelineProperties,
            *raygenShader_,
            *primaryMissShader_,
            *shadowMissShader_,
            *closestHitShader_,
            *primaryAnyHitShader_,
            *shadowAnyHitShader_,
            std::vector<VkDescriptorSetLayout>{rayTracingSetLayout_, atmosphereSetLayout_},
            *pipelineCache_,
            allocator_,
            uploader_,
            rayTracingScene_->opacityMicromapStats().active);
        if (raygenMotionShader_ != nullptr) {
            rayTracingMotionPipeline_ = std::make_unique<RayTracingPipeline>(
                context_.device(),
                context_.rayTracingInfo().rayTracingPipelineProperties,
                *raygenMotionShader_,
                *primaryMissShader_,
                *shadowMissShader_,
                *closestHitShader_,
                *primaryAnyHitShader_,
                *shadowAnyHitShader_,
                std::vector<VkDescriptorSetLayout>{rayTracingSetLayout_, atmosphereSetLayout_},
                *pipelineCache_,
                allocator_,
                uploader_,
                rayTracingScene_->opacityMicromapStats().active,
                true);
        }
        wavefrontTracePipeline_ = std::make_unique<RayTracingPipeline>(
            context_.device(),
            context_.rayTracingInfo().rayTracingPipelineProperties,
            *wavefrontTraceRaygenShader_,
            *wavefrontShadowTraceRaygenShader_,
            *primaryMissShader_,
            *shadowMissShader_,
            *closestHitShader_,
            *primaryAnyHitShader_,
            *shadowAnyHitShader_,
            std::vector<VkDescriptorSetLayout>{rayTracingSetLayout_, atmosphereSetLayout_},
            *pipelineCache_,
            allocator_,
            uploader_,
            rayTracingScene_->opacityMicromapStats().active);
        if (wavefrontTraceRaygenSerShader_ != nullptr) {
            wavefrontTraceSerPipeline_ = std::make_unique<RayTracingPipeline>(
                context_.device(),
                context_.rayTracingInfo().rayTracingPipelineProperties,
                *wavefrontTraceRaygenSerShader_,
                *wavefrontShadowTraceRaygenShader_,
                *primaryMissShader_,
                *shadowMissShader_,
                *closestHitShader_,
                *primaryAnyHitShader_,
                *shadowAnyHitShader_,
                std::vector<VkDescriptorSetLayout>{rayTracingSetLayout_, atmosphereSetLayout_},
                *pipelineCache_,
                allocator_,
                uploader_,
                rayTracingScene_->opacityMicromapStats().active,
                false,
                true);
        }
        std::cout << "RT pipeline: SBT=" << rayTracingPipeline_->sbtBytes() << " bytes\n";
    }

    frames_.reserve(kRendererFramesInFlight);
    profilers_.reserve(kRendererFramesInFlight);
    for (uint32_t i = 0; i < kRendererFramesInFlight; ++i) {
        frames_.push_back(std::make_unique<FrameResources>(context_.device(), allocator_, 64 * 1024));
        profilers_.emplace_back(context_.device(), context_.physicalDevice());
    }
    for (auto& p : profilers_) {
        p.createPipelineStatsQuery(context_.device(), context_.supportsHardwareRayTracing());
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    checkVk(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &fullscreenSampler_), "vkCreateSampler(path tracer fullscreen)");

    camera_.pos = {0.0f, 0.0f, 3.9f, 0.0f};
    camera_.forward = {0.0f, 0.0f, -1.0f, 0.0f};
    camera_.right = {1.0f, 0.0f, 0.0f, 0.0f};
    camera_.up = {0.0f, 1.0f, 0.0f, 0.0f};
    previousCameraPos_ = camera_.pos;
    settings_.materialTextureAnisotropy = scene_.materialTextureAnisotropy();
    settings_.debugView = debugView;
    debugParams_.view = static_cast<uint32_t>(settings_.debugView);
    debugParams_.flags = rayTracingDiagnosticCountersEnabled_ ? rendererDebugFlagRayTracingCounters : 0u;
    debugParams_.scale = settings_.debugScale;
    if (debugView != RendererDebugView::Beauty) {
        std::cout << "Renderer debug view: " << rendererDebugViewName(debugView) << '\n';
    }
}

PathTracerRenderer::~PathTracerRenderer() {
    if (pipelineCache_ && !shaderOutputDirectory_.empty()) {
        pipelineCache_->saveToFile(shaderOutputDirectory_ / "pipeline_cache.bin");
    }
    if (fullscreenSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), fullscreenSampler_, nullptr);
    }
}

bool PathTracerRenderer::shadersNeedReload() {
    if (shaderCompiler_ == nullptr) {
        return false;
    }
    for (const auto& source : shaderSources_) {
        if (shaderCompiler_->needsCompile(source, shaderOutputDirectory_ / source.filename().replace_extension(".spv"))) {
            return true;
        }
    }
    return false;
}

void PathTracerRenderer::beginFrame(uint32_t frameIndex, VkExtent2D renderExtent, VkExtent2D displayExtent) {
    currentFrame_ = frames_.at(frameIndex % frames_.size()).get();
    currentProfiler_ = &profilers_.at(frameIndex % profilers_.size());
    currentProfiler_->collectCompletedFrame();
    releaseRetiredResolutionResources();
    scene_.releaseRetiredMaterialSamplers(temporalFrameIndex_);
    validationLog_.beginFrame(temporalFrameIndex_);
    if (memoryPressureQualityChanged_) {
        validationLog_.recordPass(
            "memory pressure quality tier=" + std::to_string(memoryPressureTier_) +
            " pressure=" + memoryPressureName_ +
            " ratio=" + std::to_string(memoryPressureUsageRatio_) +
            " scale=" + std::to_string(effectiveRenderResolutionScale()) +
            " sppLimit=" + std::to_string(effectiveLimitSamplesPerPixel() ? 1u : 0u) +
            " giHalf=" + std::to_string(effectiveRestirGiHalfResolution() ? 1u : 0u) +
            " denoiserHistory=" + std::to_string(effectiveDenoiserMaxHistoryLength()));
        if (rawImage_.handle() != VK_NULL_HANDLE && memoryPressureTier_ > 0u) {
            resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
        }
        memoryPressureQualityChanged_ = false;
    }
    updateAdaptiveQuality(currentProfiler_->timings());
    if (temporalFrameIndex_ > 0 && temporalFrameIndex_ % 120u == 0u) {
        const GpuFrameTimings& timings = currentProfiler_->timings();
        std::cout << "GPU timings: path=" << timings.pathTraceMs
                  << " ms, denoise=" << timings.denoiserMs
                  << " ms, fullscreen=" << timings.fullscreenMs << " ms\n";
    }
    currentFrame_->beginFrame();
    if (renderExtent.width != renderExtent_.width ||
        renderExtent.height != renderExtent_.height ||
        displayExtent.width != displayExtent_.width ||
        displayExtent.height != displayExtent_.height ||
        rawImage_.handle() == VK_NULL_HANDLE) {
        if (rawImage_.handle() != VK_NULL_HANDLE) {
            retireResolutionResources();
        }
        createResolutionResources(renderExtent, displayExtent);
        resetAccumulation(AccumulationResetReason::Resize);
    }
    bindWavefrontFrameResources();
    ++frameCount_;
    ++temporalFrameIndex_;
    if (temporalSystem_) {
        temporalSystem_->beginFrame(temporalFrameIndex_);
    }
    updateCamera();
}

void PathTracerRenderer::updateAdaptiveQuality(const GpuFrameTimings& timings) {
    adaptiveEffectiveMaxBounces_ = settings_.maxBounces;
    adaptiveEffectiveEnvironmentSamples_ = settings_.environmentDirectSamples;
    adaptiveEffectiveAtrousIterations_ = settings_.atrousIterations;
    adaptiveSkipRestirSpatial_ = false;
    adaptiveSkipDenoiser_ = false;

    if (settings_.adaptiveQualityMode == AdaptiveQualityMode::Off || !settings_.pathTracingEnabled) {
        adaptiveQualityTier_ = 0;
        adaptiveOverBudgetFrames_ = 0;
        adaptiveSmoothedGpuMs_ = 0.0f;
        return;
    }

    const float gpuMs = timings.totalMs();
    if (gpuMs > 0.0f) {
        adaptiveSmoothedGpuMs_ = adaptiveSmoothedGpuMs_ <= 0.0f
            ? gpuMs
            : adaptiveSmoothedGpuMs_ * 0.85f + gpuMs * 0.15f;
    }

    const uint32_t mode = static_cast<uint32_t>(settings_.adaptiveQualityMode);
    const uint32_t maxTier = std::clamp(mode, 1u, 3u);
    const bool moving = cameraChangedThisFrame_;
    const float targetMs = std::clamp(settings_.adaptiveGpuFrameTargetMs, 4.0f, 100.0f);
    const bool overBudget = adaptiveSmoothedGpuMs_ > targetMs * 0.96f || gpuMs > targetMs * 1.05f;
    const bool underBudget = adaptiveSmoothedGpuMs_ <= 0.0f || adaptiveSmoothedGpuMs_ < targetMs * 0.82f;
    const uint32_t stableFramesForFullQuality = mode == 1u ? 6u : (mode == 2u ? 10u : 14u);

    if (moving) {
        adaptiveQualityTier_ = std::max(adaptiveQualityTier_, std::min(maxTier, mode));
        adaptiveOverBudgetFrames_ = 0;
    } else if (overBudget) {
        ++adaptiveOverBudgetFrames_;
        if (adaptiveOverBudgetFrames_ >= 6u) {
            adaptiveQualityTier_ = std::min(maxTier, adaptiveQualityTier_ + 1u);
            adaptiveOverBudgetFrames_ = 0;
        }
    } else if (stillFrameCount_ >= stableFramesForFullQuality && underBudget) {
        adaptiveQualityTier_ = 0;
        adaptiveOverBudgetFrames_ = 0;
    }

    if (adaptiveQualityTier_ == 0) {
        return;
    }

    if (adaptiveQualityTier_ == 1u) {
        adaptiveEffectiveMaxBounces_ = std::max(2u, settings_.maxBounces > 1u ? settings_.maxBounces - 1u : settings_.maxBounces);
        adaptiveEffectiveEnvironmentSamples_ = std::max(1u, settings_.environmentDirectSamples);
        adaptiveEffectiveAtrousIterations_ = std::max(1u, settings_.atrousIterations > 1u ? settings_.atrousIterations - 1u : settings_.atrousIterations);
    } else if (adaptiveQualityTier_ == 2u) {
        adaptiveEffectiveMaxBounces_ = std::max(2u, std::min(settings_.maxBounces, settings_.maxBounces / 2u));
        adaptiveEffectiveEnvironmentSamples_ = 1u;
        adaptiveEffectiveAtrousIterations_ = std::max(2u, settings_.atrousIterations > 1u ? settings_.atrousIterations - 1u : 1u);
    } else {
        adaptiveEffectiveMaxBounces_ = std::min(settings_.maxBounces, 2u);
        adaptiveEffectiveEnvironmentSamples_ = 1u;
        adaptiveEffectiveAtrousIterations_ = 1u;
    }

    if (temporalFrameIndex_ == 1u || temporalFrameIndex_ % 120u == 0u) {
        validationLog_.recordPass(
            "adaptive quality tier=" + std::to_string(adaptiveQualityTier_) +
            " gpuMs=" + std::to_string(adaptiveSmoothedGpuMs_) +
            " bounces=" + std::to_string(adaptiveEffectiveMaxBounces_) +
            " envSamples=" + std::to_string(adaptiveEffectiveEnvironmentSamples_) +
            " atrous=" + std::to_string(adaptiveEffectiveAtrousIterations_) +
            " skipRestir=" + std::to_string(adaptiveSkipRestirSpatial_ ? 1u : 0u) +
            " skipDenoiser=" + std::to_string(adaptiveSkipDenoiser_ ? 1u : 0u));
    }
}

bool PathTracerRenderer::applySettings(const RendererSettings& settings) {
    RendererSettings next = settings;
    next.maxBounces = std::clamp(next.maxBounces, 1u, 16u);
    next.samplesPerPixel = std::clamp(next.samplesPerPixel, 1u, 8u);
    next.atrousIterations = std::clamp(next.atrousIterations, 1u, 5u);
    next.environmentDirectSamples = std::clamp(next.environmentDirectSamples, 1u, 8u);
    next.denoiserStrength = std::max(0.05f, next.denoiserStrength);
    next.denoiserMaxHistoryLength = std::clamp(next.denoiserMaxHistoryLength, 4u, 256u);
    next.momentValidityThreshold = std::clamp(
        std::isfinite(next.momentValidityThreshold) ? next.momentValidityThreshold : 0.20f,
        0.05f,
        0.75f);
    next.taaFeedback = std::clamp(
        std::isfinite(next.taaFeedback) ? next.taaFeedback : 0.06f,
        0.01f,
        0.5f);
    next.taaMotionFeedback = std::clamp(
        std::isfinite(next.taaMotionFeedback) ? next.taaMotionFeedback : 0.90f,
        0.25f,
        0.98f);
    next.taaReactiveFeedback = std::clamp(
        std::isfinite(next.taaReactiveFeedback) ? next.taaReactiveFeedback : 0.98f,
        next.taaMotionFeedback,
        0.99f);
    next.taaSharpeningStrength = std::clamp(next.taaSharpeningStrength, 0.0f, 1.0f);
    next.sunIntensity = std::max(0.0f, next.sunIntensity);
    next.sunIlluminanceLux = std::max(0.0f, next.sunIlluminanceLux);
    next.sunColorTemperatureKelvin = std::clamp(next.sunColorTemperatureKelvin, 1000.0f, 40000.0f);
    next.sunColor = glm::max(next.sunColor, glm::vec3(0.0f));
    if (glm::dot(next.sunDirection, next.sunDirection) > 1.0e-6f) {
        next.sunDirection = glm::normalize(next.sunDirection);
    } else {
        next.sunDirection = glm::vec3(0.0f, 0.8240f, 0.5661f);
    }
    next.skyIntensity = std::max(0.0f, next.skyIntensity);
    next.sunElevation = std::clamp(next.sunElevation, -0.20f, 1.45f);
    constexpr float twoPi = 6.28318530717958647692f;
    if (std::isfinite(next.sunAzimuth)) {
        next.sunAzimuth = std::remainder(next.sunAzimuth, twoPi);
    } else {
        next.sunAzimuth = 0.0f;
    }
    next.exposure = std::max(0.05f, next.exposure);
    if (static_cast<uint32_t>(next.toneMapper) > static_cast<uint32_t>(ToneMapper::AgX)) {
        next.toneMapper = ToneMapper::ACES;
    }
    next.gamma = std::max(0.1f, next.gamma);
    next.contrast = std::max(0.0f, next.contrast);
    next.saturation = std::max(0.0f, next.saturation);
    next.whitePoint = std::max(0.001f, next.whitePoint);
    next.targetLuminance = std::max(0.001f, next.targetLuminance);
    next.minExposure = std::max(0.001f, next.minExposure);
    next.maxExposure = std::max(next.minExposure, next.maxExposure);
    next.adaptationSpeed = std::max(0.0f, next.adaptationSpeed);
    next.histogramMaxLogLuminance = std::max(next.histogramMinLogLuminance + 0.001f, next.histogramMaxLogLuminance);
    next.histogramLowPercentile = std::clamp(next.histogramLowPercentile, 0.0f, 1.0f);
    next.histogramHighPercentile = std::clamp(next.histogramHighPercentile, 0.0f, 1.0f);
    next.histogramTargetPercentile = std::clamp(next.histogramTargetPercentile, 0.0f, 1.0f);
    next.sunAngularRadius = std::isfinite(next.sunAngularRadius)
        ? std::clamp(next.sunAngularRadius, 0.0f, 0.08f)
        : 0.00465f;
    next.rayleighScaleHeight = std::clamp(next.rayleighScaleHeight, 1000.0f, 20000.0f);
    next.mieScaleHeight = std::clamp(next.mieScaleHeight, 200.0f, 5000.0f);
    next.mieAnisotropy = std::clamp(next.mieAnisotropy, 0.0f, 0.99f);
    next.groundAlbedo = std::clamp(next.groundAlbedo, 0.0f, 1.0f);
    next.physicalAperture = std::max(0.1f, next.physicalAperture);
    next.physicalShutterSeconds = std::max(1.0e-6f, next.physicalShutterSeconds);
    next.physicalIso = std::max(1.0f, next.physicalIso);
    next.physicalExposureCompensation = std::clamp(next.physicalExposureCompensation, -10.0f, 10.0f);
    next.dofApertureRadius = std::clamp(
        std::isfinite(next.dofApertureRadius) ? next.dofApertureRadius : 0.0f,
        0.0f,
        1.0f);
    next.dofFocusDistance = std::clamp(
        std::isfinite(next.dofFocusDistance) ? next.dofFocusDistance : 10.0f,
        0.01f,
        10000.0f);
    next.dofBladeCount = next.dofBladeCount == 0u ? 0u : std::clamp(next.dofBladeCount, 3u, 16u);
    next.dofBokehRotation = std::isfinite(next.dofBokehRotation)
        ? std::remainder(next.dofBokehRotation, twoPi)
        : 0.0f;
    if (!context_.supportsRayTracingMotionBlur()) {
        next.motionBlurEnabled = false;
    }
    next.motionBlurShutterOpen = std::clamp(
        std::isfinite(next.motionBlurShutterOpen) ? next.motionBlurShutterOpen : 0.0f,
        0.0f,
        1.0f);
    next.motionBlurShutterClose = std::clamp(
        std::isfinite(next.motionBlurShutterClose) ? next.motionBlurShutterClose : 1.0f,
        0.0f,
        1.0f);
    if (next.motionBlurShutterClose < next.motionBlurShutterOpen) {
        std::swap(next.motionBlurShutterOpen, next.motionBlurShutterClose);
    }
    next.homogeneousVolumeScattering = std::clamp(
        std::isfinite(next.homogeneousVolumeScattering) ? next.homogeneousVolumeScattering : 0.0f,
        0.0f,
        1.0f);
    next.homogeneousVolumeAbsorption = std::clamp(
        std::isfinite(next.homogeneousVolumeAbsorption) ? next.homogeneousVolumeAbsorption : 0.0f,
        0.0f,
        1.0f);
    next.homogeneousVolumeAnisotropy = std::clamp(
        std::isfinite(next.homogeneousVolumeAnisotropy) ? next.homogeneousVolumeAnisotropy : 0.0f,
        -0.95f,
        0.95f);
    if (next.homogeneousVolumeScattering + next.homogeneousVolumeAbsorption <= 1.0e-8f) {
        next.homogeneousVolumeEnabled = false;
    }
    next.indirectStrength = std::max(0.0f, next.indirectStrength);
    next.environmentIntensity = std::max(0.0f, next.environmentIntensity);
    next.environmentBackgroundIntensity = std::max(0.0f, next.environmentBackgroundIntensity);
    next.renderResolutionScale = std::clamp(next.renderResolutionScale, 0.25f, 1.0f);
    const float maxMaterialAnisotropy = allocator_.supportsSamplerAnisotropy() ? allocator_.maxSamplerAnisotropy() : 1.0f;
    next.materialTextureAnisotropy = std::clamp(
        std::isfinite(next.materialTextureAnisotropy) ? next.materialTextureAnisotropy : 1.0f,
        1.0f,
        maxMaterialAnisotropy);
    next.debugScale = std::max(0.05f, next.debugScale);
    next.shadowRayBias = std::clamp(next.shadowRayBias, 0.00001f, 0.05f);
    next.shadowDistanceBias = std::clamp(next.shadowDistanceBias, 0.0f, 0.1f);
    next.fireflyClamp = std::clamp(next.fireflyClamp, 1.0f, 512.0f);
    next.maxFrameDeltaSeconds = std::clamp(
        std::isfinite(next.maxFrameDeltaSeconds) ? next.maxFrameDeltaSeconds : (1.0f / 30.0f),
        1.0f / 240.0f,
        1.0f / 5.0f);
    next.russianRouletteMinSurvival = std::clamp(
        std::isfinite(next.russianRouletteMinSurvival) ? next.russianRouletteMinSurvival : 0.10f,
        0.02f,
        0.50f);
    next.restirGiTemporalMaxAge = std::clamp(next.restirGiTemporalMaxAge, 1u, 64u);
    next.restirGiSpatialRounds = std::clamp(next.restirGiSpatialRounds, 1u, 8u);
    next.restirGiSpatialRadius = std::clamp(
        std::isfinite(next.restirGiSpatialRadius) ? next.restirGiSpatialRadius : 4.25f,
        1.0f,
        8.0f);
    next.restirGiDepthThresholdScale = std::clamp(
        std::isfinite(next.restirGiDepthThresholdScale) ? next.restirGiDepthThresholdScale : 1.0f,
        0.5f,
        2.0f);
    next.restirGiSpatialCompatibilityThreshold = std::clamp(
        std::isfinite(next.restirGiSpatialCompatibilityThreshold) ? next.restirGiSpatialCompatibilityThreshold : 0.05f,
        0.0f,
        0.85f);
    next.restirGiVisibilityRayBudget = std::clamp(next.restirGiVisibilityRayBudget, 0u, 4u);
    if (!context_.supportsOpacityMicromaps()) {
        next.opacityMicromapsEnabled = false;
    }
    if (!context_.supportsSER()) {
        next.shaderExecutionReorderingEnabled = false;
    }
    if (isWavefrontDebugView(next.debugView)) {
        next.wavefrontShadeEnabled = true;
    }
    if (next.debugView == RendererDebugView::WavefrontRestirGi) {
        next.wavefrontCompactEnabled = true;
    }
    if (requiresWavefrontShadowTrace(next.debugView)) {
        next.wavefrontShadowTraceEnabled = true;
    }
    if (next.debugView == RendererDebugView::CausticVisibility) {
        next.mneeCausticsEnabled = true;
    }
    if (next.wavefrontShadowTraceEnabled) {
        next.wavefrontShadeEnabled = true;
    }
    if (next.wavefrontCompactEnabled) {
        next.wavefrontShadeEnabled = true;
    }
    if (next.wavefrontSortEnabled) {
        next.wavefrontCompactEnabled = true;
        next.wavefrontShadeEnabled = true;
    }
    if (next.wavefrontFinalOutputEnabled) {
        next.wavefrontShadowTraceEnabled = true;
        next.wavefrontCompactEnabled = true;
        next.wavefrontShadeEnabled = true;
    }
    if (next.wavefrontShadeEnabled) {
        next.wavefrontTraceEnabled = true;
    }
    if (next.wavefrontTraceEnabled) {
        next.wavefrontPrimaryGenerateEnabled = true;
    }
    if (next.wavefrontPrimaryGenerateEnabled) {
        next.wavefrontQueuesEnabled = true;
    }
    next.opacityMicromapSubdivisionLevel = std::clamp(next.opacityMicromapSubdivisionLevel, 0u, 5u);
    if (static_cast<uint32_t>(next.adaptiveQualityMode) > static_cast<uint32_t>(AdaptiveQualityMode::Aggressive)) {
        next.adaptiveQualityMode = AdaptiveQualityMode::Off;
    }
    next.adaptiveGpuFrameTargetMs = std::clamp(
        std::isfinite(next.adaptiveGpuFrameTargetMs) ? next.adaptiveGpuFrameTargetMs : 16.6f,
        4.0f,
        100.0f);
    if (static_cast<uint32_t>(next.restirMode) > static_cast<uint32_t>(RestirMode::HybridCompare)) {
        next.restirMode = RestirMode::ClassicNee;
    }
    if (static_cast<uint32_t>(next.renderPreset) > static_cast<uint32_t>(RenderPreset::Ultra)) {
        next.renderPreset = RenderPreset::Custom;
    }

    const bool changed =
        next.renderPreset != settings_.renderPreset ||
        next.pathTracingEnabled != settings_.pathTracingEnabled ||
        next.cameraJitterEnabled != settings_.cameraJitterEnabled ||
        next.denoiserEnabled != settings_.denoiserEnabled ||
        next.denoiseWhileMoving != settings_.denoiseWhileMoving ||
        next.taaEnabled != settings_.taaEnabled ||
        next.sunlightEnabled != settings_.sunlightEnabled ||
        next.directLightingEnabled != settings_.directLightingEnabled ||
        next.environmentEnabled != settings_.environmentEnabled ||
        next.maxBounces != settings_.maxBounces ||
        next.samplesPerPixel != settings_.samplesPerPixel ||
        next.limitSamplesPerPixel != settings_.limitSamplesPerPixel ||
        next.atrousIterations != settings_.atrousIterations ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        next.restirMode != settings_.restirMode ||
        next.restirGiEnabled != settings_.restirGiEnabled ||
        next.specularAaEnabled != settings_.specularAaEnabled ||
        next.opacityMicromapsEnabled != settings_.opacityMicromapsEnabled ||
        next.opacityMicromapSubdivisionLevel != settings_.opacityMicromapSubdivisionLevel ||
        next.wavefrontQueuesEnabled != settings_.wavefrontQueuesEnabled ||
        next.wavefrontPrimaryGenerateEnabled != settings_.wavefrontPrimaryGenerateEnabled ||
        next.wavefrontTraceEnabled != settings_.wavefrontTraceEnabled ||
        next.wavefrontShadeEnabled != settings_.wavefrontShadeEnabled ||
        next.wavefrontShadowTraceEnabled != settings_.wavefrontShadowTraceEnabled ||
        next.wavefrontCompactEnabled != settings_.wavefrontCompactEnabled ||
        next.wavefrontSortEnabled != settings_.wavefrontSortEnabled ||
        next.wavefrontFinalOutputEnabled != settings_.wavefrontFinalOutputEnabled ||
        next.shaderExecutionReorderingEnabled != settings_.shaderExecutionReorderingEnabled ||
        next.debugView != settings_.debugView ||
        next.toneMapper != settings_.toneMapper ||
        next.autoExposureEnabled != settings_.autoExposureEnabled ||
        next.usePhysicalCamera != settings_.usePhysicalCamera ||
        std::abs(next.taaFeedback - settings_.taaFeedback) > 0.0001f ||
        std::abs(next.taaMotionFeedback - settings_.taaMotionFeedback) > 0.0001f ||
        std::abs(next.taaReactiveFeedback - settings_.taaReactiveFeedback) > 0.0001f ||
        std::abs(next.taaSharpeningStrength - settings_.taaSharpeningStrength) > 0.0001f ||
        std::abs(next.denoiserStrength - settings_.denoiserStrength) > 0.0001f ||
        next.denoiserMaxHistoryLength != settings_.denoiserMaxHistoryLength ||
        std::abs(next.momentValidityThreshold - settings_.momentValidityThreshold) > 0.0001f ||
        std::abs(next.sunIntensity - settings_.sunIntensity) > 0.0001f ||
        std::abs(next.sunIlluminanceLux - settings_.sunIlluminanceLux) > 0.5f ||
        glm::length(next.sunColor - settings_.sunColor) > 0.0001f ||
        glm::length(next.sunDirection - settings_.sunDirection) > 0.0001f ||
        std::abs(next.skyIntensity - settings_.skyIntensity) > 0.0001f ||
        std::abs(next.sunElevation - settings_.sunElevation) > 0.0001f ||
        std::abs(next.sunAzimuth - settings_.sunAzimuth) > 0.0001f ||
        std::abs(next.exposure - settings_.exposure) > 0.0001f ||
        std::abs(next.gamma - settings_.gamma) > 0.0001f ||
        std::abs(next.contrast - settings_.contrast) > 0.0001f ||
        std::abs(next.saturation - settings_.saturation) > 0.0001f ||
        std::abs(next.brightness - settings_.brightness) > 0.0001f ||
        std::abs(next.whitePoint - settings_.whitePoint) > 0.0001f ||
        std::abs(next.targetLuminance - settings_.targetLuminance) > 0.0001f ||
        std::abs(next.minExposure - settings_.minExposure) > 0.0001f ||
        std::abs(next.maxExposure - settings_.maxExposure) > 0.0001f ||
        std::abs(next.adaptationSpeed - settings_.adaptationSpeed) > 0.0001f ||
        std::abs(next.histogramMinLogLuminance - settings_.histogramMinLogLuminance) > 0.0001f ||
        std::abs(next.histogramMaxLogLuminance - settings_.histogramMaxLogLuminance) > 0.0001f ||
        std::abs(next.histogramLowPercentile - settings_.histogramLowPercentile) > 0.0001f ||
        std::abs(next.histogramHighPercentile - settings_.histogramHighPercentile) > 0.0001f ||
        std::abs(next.histogramTargetPercentile - settings_.histogramTargetPercentile) > 0.0001f ||
        std::abs(next.sunAngularRadius - settings_.sunAngularRadius) > 0.0001f ||
        std::abs(next.rayleighScaleHeight - settings_.rayleighScaleHeight) > 0.5f ||
        std::abs(next.mieScaleHeight - settings_.mieScaleHeight) > 0.5f ||
        std::abs(next.mieAnisotropy - settings_.mieAnisotropy) > 0.0001f ||
        std::abs(next.groundAlbedo - settings_.groundAlbedo) > 0.0001f ||
        std::abs(next.physicalAperture - settings_.physicalAperture) > 0.0001f ||
        std::abs(next.physicalShutterSeconds - settings_.physicalShutterSeconds) > 0.000001f ||
        std::abs(next.physicalIso - settings_.physicalIso) > 0.0001f ||
        std::abs(next.physicalExposureCompensation - settings_.physicalExposureCompensation) > 0.0001f ||
        std::abs(next.dofApertureRadius - settings_.dofApertureRadius) > 0.000001f ||
        std::abs(next.dofFocusDistance - settings_.dofFocusDistance) > 0.0001f ||
        next.dofBladeCount != settings_.dofBladeCount ||
        std::abs(next.dofBokehRotation - settings_.dofBokehRotation) > 0.0001f ||
        next.motionBlurEnabled != settings_.motionBlurEnabled ||
        std::abs(next.motionBlurShutterOpen - settings_.motionBlurShutterOpen) > 0.0001f ||
        std::abs(next.motionBlurShutterClose - settings_.motionBlurShutterClose) > 0.0001f ||
        next.homogeneousVolumeEnabled != settings_.homogeneousVolumeEnabled ||
        std::abs(next.homogeneousVolumeScattering - settings_.homogeneousVolumeScattering) > 0.000001f ||
        std::abs(next.homogeneousVolumeAbsorption - settings_.homogeneousVolumeAbsorption) > 0.000001f ||
        std::abs(next.homogeneousVolumeAnisotropy - settings_.homogeneousVolumeAnisotropy) > 0.0001f ||
        next.mneeCausticsEnabled != settings_.mneeCausticsEnabled ||
        std::abs(next.indirectStrength - settings_.indirectStrength) > 0.0001f ||
        std::abs(next.environmentIntensity - settings_.environmentIntensity) > 0.0001f ||
        std::abs(next.environmentRotation - settings_.environmentRotation) > 0.0001f ||
        std::abs(next.environmentBackgroundIntensity - settings_.environmentBackgroundIntensity) > 0.0001f ||
        std::abs(next.renderResolutionScale - settings_.renderResolutionScale) > 0.0001f ||
        std::abs(next.materialTextureAnisotropy - settings_.materialTextureAnisotropy) > 0.0001f ||
        std::abs(next.debugScale - settings_.debugScale) > 0.0001f ||
        std::abs(next.shadowRayBias - settings_.shadowRayBias) > 0.000001f ||
        std::abs(next.shadowDistanceBias - settings_.shadowDistanceBias) > 0.000001f ||
        std::abs(next.fireflyClamp - settings_.fireflyClamp) > 0.0001f ||
        std::abs(next.maxFrameDeltaSeconds - settings_.maxFrameDeltaSeconds) > 0.000001f ||
        std::abs(next.russianRouletteMinSurvival - settings_.russianRouletteMinSurvival) > 0.0001f ||
        next.restirGiTemporalMaxAge != settings_.restirGiTemporalMaxAge ||
        next.restirGiSpatialRounds != settings_.restirGiSpatialRounds ||
        std::abs(next.restirGiSpatialRadius - settings_.restirGiSpatialRadius) > 0.0001f ||
        std::abs(next.restirGiDepthThresholdScale - settings_.restirGiDepthThresholdScale) > 0.0001f ||
        std::abs(next.restirGiSpatialCompatibilityThreshold - settings_.restirGiSpatialCompatibilityThreshold) > 0.0001f ||
        next.restirGiHalfResolution != settings_.restirGiHalfResolution ||
        next.restirGiVisibilityRayBudget != settings_.restirGiVisibilityRayBudget ||
        next.adaptiveQualityMode != settings_.adaptiveQualityMode ||
        std::abs(next.adaptiveGpuFrameTargetMs - settings_.adaptiveGpuFrameTargetMs) > 0.0001f ||
        next.wavefrontQueuesEnabled != settings_.wavefrontQueuesEnabled ||
        next.wavefrontPrimaryGenerateEnabled != settings_.wavefrontPrimaryGenerateEnabled ||
        next.wavefrontTraceEnabled != settings_.wavefrontTraceEnabled ||
        next.wavefrontShadeEnabled != settings_.wavefrontShadeEnabled ||
        next.wavefrontShadowTraceEnabled != settings_.wavefrontShadowTraceEnabled ||
        next.wavefrontCompactEnabled != settings_.wavefrontCompactEnabled ||
        next.wavefrontSortEnabled != settings_.wavefrontSortEnabled ||
        next.wavefrontFinalOutputEnabled != settings_.wavefrontFinalOutputEnabled ||
        next.shaderExecutionReorderingEnabled != settings_.shaderExecutionReorderingEnabled;
    if (!changed) {
        return false;
    }

    const bool environmentChanged =
        next.environmentEnabled != settings_.environmentEnabled ||
        std::abs(next.environmentIntensity - settings_.environmentIntensity) > 0.0001f ||
        std::abs(next.environmentRotation - settings_.environmentRotation) > 0.0001f ||
        std::abs(next.environmentBackgroundIntensity - settings_.environmentBackgroundIntensity) > 0.0001f;
    const bool lightingChanged =
        next.sunlightEnabled != settings_.sunlightEnabled ||
        next.directLightingEnabled != settings_.directLightingEnabled ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        next.restirMode != settings_.restirMode ||
        next.restirGiEnabled != settings_.restirGiEnabled ||
        std::abs(next.sunIntensity - settings_.sunIntensity) > 0.0001f ||
        std::abs(next.sunIlluminanceLux - settings_.sunIlluminanceLux) > 0.5f ||
        glm::length(next.sunColor - settings_.sunColor) > 0.0001f ||
        glm::length(next.sunDirection - settings_.sunDirection) > 0.0001f ||
        std::abs(next.skyIntensity - settings_.skyIntensity) > 0.0001f ||
        std::abs(next.sunElevation - settings_.sunElevation) > 0.0001f ||
        std::abs(next.sunAzimuth - settings_.sunAzimuth) > 0.0001f ||
        std::abs(next.sunAngularRadius - settings_.sunAngularRadius) > 0.0001f ||
        std::abs(next.rayleighScaleHeight - settings_.rayleighScaleHeight) > 0.5f ||
        std::abs(next.mieScaleHeight - settings_.mieScaleHeight) > 0.5f ||
        std::abs(next.mieAnisotropy - settings_.mieAnisotropy) > 0.0001f ||
        std::abs(next.groundAlbedo - settings_.groundAlbedo) > 0.0001f;
    const bool denoiserChanged =
        next.denoiserEnabled != settings_.denoiserEnabled ||
        next.denoiseWhileMoving != settings_.denoiseWhileMoving ||
        next.atrousIterations != settings_.atrousIterations ||
        std::abs(next.denoiserStrength - settings_.denoiserStrength) > 0.0001f ||
        next.denoiserMaxHistoryLength != settings_.denoiserMaxHistoryLength ||
        std::abs(next.momentValidityThreshold - settings_.momentValidityThreshold) > 0.0001f;
    const bool taaChanged =
        next.taaEnabled != settings_.taaEnabled ||
        std::abs(next.taaFeedback - settings_.taaFeedback) > 0.0001f ||
        std::abs(next.taaMotionFeedback - settings_.taaMotionFeedback) > 0.0001f ||
        std::abs(next.taaReactiveFeedback - settings_.taaReactiveFeedback) > 0.0001f ||
        std::abs(next.taaSharpeningStrength - settings_.taaSharpeningStrength) > 0.0001f;
    const bool debugChanged =
        next.debugView != settings_.debugView ||
        std::abs(next.debugScale - settings_.debugScale) > 0.0001f;
    const bool renderChanged =
        next.pathTracingEnabled != settings_.pathTracingEnabled ||
        next.cameraJitterEnabled != settings_.cameraJitterEnabled ||
        next.adaptiveQualityMode != settings_.adaptiveQualityMode ||
        next.specularAaEnabled != settings_.specularAaEnabled ||
        next.opacityMicromapsEnabled != settings_.opacityMicromapsEnabled ||
        next.opacityMicromapSubdivisionLevel != settings_.opacityMicromapSubdivisionLevel ||
        next.wavefrontQueuesEnabled != settings_.wavefrontQueuesEnabled ||
        next.wavefrontPrimaryGenerateEnabled != settings_.wavefrontPrimaryGenerateEnabled ||
        next.wavefrontTraceEnabled != settings_.wavefrontTraceEnabled ||
        next.wavefrontShadeEnabled != settings_.wavefrontShadeEnabled ||
        next.wavefrontShadowTraceEnabled != settings_.wavefrontShadowTraceEnabled ||
        next.wavefrontCompactEnabled != settings_.wavefrontCompactEnabled ||
        next.wavefrontSortEnabled != settings_.wavefrontSortEnabled ||
        next.wavefrontFinalOutputEnabled != settings_.wavefrontFinalOutputEnabled ||
        next.maxBounces != settings_.maxBounces ||
        next.samplesPerPixel != settings_.samplesPerPixel ||
        next.limitSamplesPerPixel != settings_.limitSamplesPerPixel ||
        next.environmentDirectSamples != settings_.environmentDirectSamples ||
        std::abs(next.indirectStrength - settings_.indirectStrength) > 0.0001f ||
        std::abs(next.dofApertureRadius - settings_.dofApertureRadius) > 0.000001f ||
        std::abs(next.dofFocusDistance - settings_.dofFocusDistance) > 0.0001f ||
        next.dofBladeCount != settings_.dofBladeCount ||
        std::abs(next.dofBokehRotation - settings_.dofBokehRotation) > 0.0001f ||
        next.motionBlurEnabled != settings_.motionBlurEnabled ||
        std::abs(next.motionBlurShutterOpen - settings_.motionBlurShutterOpen) > 0.0001f ||
        std::abs(next.motionBlurShutterClose - settings_.motionBlurShutterClose) > 0.0001f ||
        std::abs(next.shadowRayBias - settings_.shadowRayBias) > 0.000001f ||
        std::abs(next.shadowDistanceBias - settings_.shadowDistanceBias) > 0.000001f ||
        std::abs(next.fireflyClamp - settings_.fireflyClamp) > 0.0001f ||
        std::abs(next.adaptiveGpuFrameTargetMs - settings_.adaptiveGpuFrameTargetMs) > 0.0001f ||
        std::abs(next.russianRouletteMinSurvival - settings_.russianRouletteMinSurvival) > 0.0001f ||
        next.restirGiTemporalMaxAge != settings_.restirGiTemporalMaxAge ||
        next.restirGiSpatialRounds != settings_.restirGiSpatialRounds ||
        std::abs(next.restirGiSpatialRadius - settings_.restirGiSpatialRadius) > 0.0001f ||
        std::abs(next.restirGiDepthThresholdScale - settings_.restirGiDepthThresholdScale) > 0.0001f ||
        std::abs(next.restirGiSpatialCompatibilityThreshold - settings_.restirGiSpatialCompatibilityThreshold) > 0.0001f ||
        next.restirGiHalfResolution != settings_.restirGiHalfResolution ||
        next.restirGiVisibilityRayBudget != settings_.restirGiVisibilityRayBudget;
    const bool renderResolutionChanged =
        std::abs(next.renderResolutionScale - settings_.renderResolutionScale) > 0.0001f;
    const bool materialTextureFilteringChanged =
        std::abs(next.materialTextureAnisotropy - settings_.materialTextureAnisotropy) > 0.0001f;
    const bool motionBlurStructureChanged = next.motionBlurEnabled != settings_.motionBlurEnabled;

    settings_ = next;
    if (motionBlurStructureChanged && rayTracingScene_ != nullptr) {
        rayTracingScene_ = std::make_unique<RayTracingScene>(
            context_,
            allocator_,
            uploader_,
            scene_,
            RayTracingSceneBuildOptions{
                .opacityMicromapsEnabled = settings_.opacityMicromapsEnabled,
                .motionBlurEnabled = settings_.motionBlurEnabled});
    }
    if (renderChanged || denoiserChanged) {
        adaptiveQualityTier_ = 0;
        adaptiveOverBudgetFrames_ = 0;
    }
    if (materialTextureFilteringChanged) {
        const uint64_t retireFrame = temporalFrameIndex_ + static_cast<uint64_t>(frames_.size()) + 1ull;
        scene_.setMaterialTextureAnisotropy(settings_.materialTextureAnisotropy, retireFrame);
    }
    if (taaChanged || renderResolutionChanged) {
        taaHistoryValid_ = false;
    }

    physicalCamera_.setSettings({
        settings_.physicalAperture,
        settings_.physicalShutterSeconds,
        settings_.physicalIso,
        settings_.physicalExposureCompensation,
        settings_.dofApertureRadius,
        settings_.dofFocusDistance,
        settings_.dofBladeCount,
        settings_.dofBokehRotation});
    const bool environmentUploaded = scene_.setEnvironmentControls(
        settings_.environmentEnabled,
        settings_.environmentIntensity,
        settings_.environmentRotation,
        settings_.environmentBackgroundIntensity);
    if (renderResolutionChanged) {
        resetAccumulation(AccumulationResetReason::Resize);
    } else if (materialTextureFilteringChanged) {
        resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
    } else if (environmentChanged || environmentUploaded) {
        resetAccumulation(AccumulationResetReason::EnvironmentChanged);
    } else if (lightingChanged) {
        resetAccumulation(AccumulationResetReason::LightingChanged);
    } else if (renderChanged) {
        resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
    } else if (denoiserChanged) {
        resetAccumulation(AccumulationResetReason::DenoiserSettingsChanged);
    } else if (taaChanged) {
        resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
    } else if (debugChanged) {
        resetAccumulation(AccumulationResetReason::DebugViewChanged);
    }
    return true;
}

const OpacityMicromapDeviceInfo& PathTracerRenderer::opacityMicromapInfo() const {
    return context_.opacityMicromapInfo();
}

const SerDeviceInfo& PathTracerRenderer::serInfo() const {
    return context_.serInfo();
}

const RayTracingMotionBlurDeviceInfo& PathTracerRenderer::rayTracingMotionBlurInfo() const {
    return context_.rayTracingMotionBlurInfo();
}

void PathTracerRenderer::setCameraPose(glm::vec3 position, glm::vec3 forward) {
    if (glm::dot(forward, forward) <= 0.0f) {
        return;
    }
    forward = glm::normalize(forward);
    const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
    glm::vec3 right = glm::cross(forward, worldUp);
    if (glm::dot(right, right) <= 0.0001f) {
        right = {1.0f, 0.0f, 0.0f};
    } else {
        right = glm::normalize(right);
    }
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));

    const glm::vec3 oldPos = glm::vec3(camera_.pos);
    const glm::vec3 oldForward = glm::vec3(camera_.forward);
    if (glm::length(position - oldPos) <= 0.00001f && glm::length(forward - oldForward) <= 0.00001f) {
        return;
    }

    camera_.pos = glm::vec4(position, 0.0f);
    camera_.forward = glm::vec4(forward, 0.0f);
    camera_.right = glm::vec4(right, 0.0f);
    camera_.up = glm::vec4(up, 0.0f);
    cameraChangedThisFrame_ = true;
    resetAccumulation(AccumulationResetReason::CameraMoved);
}

void PathTracerRenderer::setCameraFovY(float fovY) {
    const float clampedFov = std::clamp(fovY, 0.1f, 3.0f);
    if (std::abs(camera_.fovY - clampedFov) <= 0.0001f) {
        return;
    }
    camera_.fovY = clampedFov;
    resetAccumulation(AccumulationResetReason::CameraMoved);
}

void PathTracerRenderer::resetAccumulation(AccumulationResetReason reason) {
    lastResetReason_ = reason;
    ++pickSceneVersion_;
    pendingPick_ = {};
    validationLog_.recordAccumulationInvalidation(accumulationResetReasonName(reason), temporalFrameIndex_);
    if (temporalSystem_) {
        temporalSystem_->setCameraCut(reason != AccumulationResetReason::CameraMoved, reason);
    }
    if (reason != AccumulationResetReason::CameraMoved) {
    denoiserHistoryValid_ = false;
    denoiserFramesSinceReset_ = 0;
    taaHistoryValid_ = false;
        restirGiHistoryValid_ = false;
    }
    frameCount_ = 0;
    if (reason != AccumulationResetReason::CameraMoved) {
        previousJitter_ = glm::vec2(0.0f);
        stillFrameCount_ = 0;
        if (reason == AccumulationResetReason::Resize) {
            temporalFrameIndex_ = 0;
        }
    }
}

void PathTracerRenderer::loadEnvironment(const std::filesystem::path& path) {
    const uint64_t retireFrame = temporalFrameIndex_ + static_cast<uint32_t>(std::max<size_t>(frames_.size(), 1)) + 1u;
    scene_.loadEnvironment(uploader_, path, retireFrame);
    scene_.setEnvironmentControls(
        settings_.environmentEnabled,
        settings_.environmentIntensity,
        settings_.environmentRotation,
        settings_.environmentBackgroundIntensity);
    resetAccumulation(AccumulationResetReason::EnvironmentChanged);
}

bool PathTracerRenderer::updateMaterials(const SceneAsset& scene, const AssetManager& assets) {
    const bool updated = scene_.updateImportedMaterials(uploader_, scene, assets);
    if (updated) {
        resetAccumulation(AccumulationResetReason::MaterialChanged);
    }
    return updated;
}

bool PathTracerRenderer::updateSceneLights(const SceneAsset& scene) {
    const uint64_t retireFrame = temporalFrameIndex_ + static_cast<uint32_t>(std::max<size_t>(frames_.size(), 1)) + 1u;
    const bool updated = scene_.updateSceneLights(uploader_, scene, retireFrame);
    if (updated) {
        resetAccumulation(AccumulationResetReason::LightingChanged);
    }
    return updated;
}

void PathTracerRenderer::refreshMemoryPressureQuality() {
    const auto report = allocator_.memoryBudgetReport();
    uint32_t tier = 0;
    if (report.maxUsageRatio >= 0.95f || report.pressure == "critical") {
        tier = 3;
    } else if (report.maxUsageRatio >= 0.85f || report.pressure == "high") {
        tier = 2;
    } else if (report.maxUsageRatio >= 0.70f || report.pressure == "medium") {
        tier = 1;
    }

    const bool changed = tier != memoryPressureTier_ ||
        report.overrideActive != memoryPressureOverrideActive_ ||
        std::abs(report.maxUsageRatio - memoryPressureUsageRatio_) > 0.01f ||
        report.pressure != memoryPressureName_;
    memoryPressureTier_ = tier;
    memoryPressureActive_ = tier > 0u;
    memoryPressureOverrideActive_ = report.overrideActive;
    memoryPressureUsageRatio_ = report.maxUsageRatio;
    memoryPressureName_ = report.pressure.empty() ? "normal" : report.pressure;
    memoryPressureQualityChanged_ = memoryPressureQualityChanged_ || changed;
}

float PathTracerRenderer::effectiveRenderResolutionScale() const {
    if (memoryPressureTier_ >= 3u) {
        return std::min(settings_.renderResolutionScale, 0.50f);
    }
    if (memoryPressureTier_ >= 2u) {
        return std::min(settings_.renderResolutionScale, 0.60f);
    }
    if (memoryPressureTier_ >= 1u) {
        return std::min(settings_.renderResolutionScale, 0.75f);
    }
    return settings_.renderResolutionScale;
}

bool PathTracerRenderer::updateSceneTransforms(const SceneAsset& scene, const AssetManager& assets) {
    const uint64_t retireFrame = temporalFrameIndex_ + static_cast<uint32_t>(std::max<size_t>(frames_.size(), 1)) + 1u;
    const bool updated = scene_.updateInstanceTransforms(uploader_, scene, assets, retireFrame);
    if (!updated) {
        return false;
    }
    if (rayTracingScene_ == nullptr ||
        !rayTracingScene_->refitTransforms(context_, allocator_, uploader_, scene_)) {
        rayTracingScene_ = std::make_unique<RayTracingScene>(context_, allocator_, uploader_, scene_);
    }
    resetAccumulation(AccumulationResetReason::SceneChanged);
    return true;
}

bool PathTracerRenderer::updateSceneVisibility(const SceneAsset& scene, const AssetManager& assets) {
    const uint64_t retireFrame = temporalFrameIndex_ + static_cast<uint32_t>(std::max<size_t>(frames_.size(), 1)) + 1u;
    const bool updated = scene_.updateInstanceTransforms(uploader_, scene, assets, retireFrame);
    if (!updated) {
        return false;
    }
    if (rayTracingScene_ == nullptr ||
        !rayTracingScene_->refitTransforms(context_, allocator_, uploader_, scene_)) {
        return false;
    }
    resetAccumulation(AccumulationResetReason::SceneChanged);
    return true;
}

void PathTracerRenderer::setSelectedInstanceId(std::optional<uint32_t> instanceId) {
    selectedInstanceId_ = instanceId.value_or(UINT32_MAX);
}

void PathTracerRenderer::requestPickInstanceId(glm::vec2 viewportUv) {
    pendingPick_ = PendingPickRequest{
        .active = true,
        .viewportUv = glm::clamp(viewportUv, glm::vec2(0.0f), glm::vec2(1.0f)),
        .requestFrame = temporalFrameIndex_,
        .sceneVersion = pickSceneVersion_,
    };
}

std::optional<uint32_t> PathTracerRenderer::consumePickedInstanceId() {
    if (!pendingPick_.active) {
        return std::nullopt;
    }
    if (pendingPick_.sceneVersion != pickSceneVersion_) {
        pendingPick_ = {};
        return std::nullopt;
    }
    const uint32_t readbackDelay = static_cast<uint32_t>(std::max<size_t>(frames_.size(), 1));
    if (temporalFrameIndex_ < pendingPick_.requestFrame + readbackDelay) {
        return std::nullopt;
    }
    if (entityIdBuffer_.handle() == VK_NULL_HANDLE || entityIdBuffer_.mappedData() == nullptr || renderExtent_.width == 0 || renderExtent_.height == 0) {
        pendingPick_ = {};
        return std::nullopt;
    }

    const glm::vec2 viewportUv = pendingPick_.viewportUv;
    pendingPick_ = {};
    const uint32_t x = std::min(renderExtent_.width - 1u, static_cast<uint32_t>(viewportUv.x * static_cast<float>(renderExtent_.width)));
    const uint32_t y = std::min(renderExtent_.height - 1u, static_cast<uint32_t>(viewportUv.y * static_cast<float>(renderExtent_.height)));
    const VkDeviceSize offset = (static_cast<VkDeviceSize>(y) * renderExtent_.width + x) * sizeof(uint32_t);

    entityIdBuffer_.invalidate(sizeof(uint32_t), offset);
    const uint32_t id = *reinterpret_cast<const uint32_t*>(static_cast<const std::byte*>(entityIdBuffer_.mappedData()) + offset);
    if (id == UINT32_MAX) {
        return std::nullopt;
    }
    return id;
}

bool PathTracerRenderer::pickPending() const {
    return pendingPick_.active;
}

const GpuFrameTimings& PathTracerRenderer::timings() const {
    static const GpuFrameTimings empty{};
    if (currentProfiler_ != nullptr) {
        return currentProfiler_->timings();
    }
    return profilers_.empty() ? empty : profilers_.front().timings();
}

PathTracerRenderer::AdaptiveQualityState PathTracerRenderer::adaptiveQualityState() const {
    return AdaptiveQualityState{
        adaptiveSmoothedGpuMs_,
        adaptiveQualityTier_,
        adaptiveOverBudgetFrames_,
        adaptiveEffectiveMaxBounces_,
        adaptiveEffectiveEnvironmentSamples_,
        adaptiveEffectiveAtrousIterations_,
        adaptiveSkipRestirSpatial_,
        adaptiveSkipDenoiser_,
    };
}

PathTracerRenderer::MemoryPressureQualityState PathTracerRenderer::memoryPressureQualityState() const {
    return MemoryPressureQualityState{
        memoryPressureActive_,
        memoryPressureOverrideActive_,
        memoryPressureTier_,
        memoryPressureUsageRatio_,
        memoryPressureName_,
        effectiveRenderResolutionScale(),
        effectiveLimitSamplesPerPixel(),
        effectiveRestirGiHalfResolution(),
        effectiveDenoiserMaxHistoryLength(),
    };
}

GpuPipelineStatistics PathTracerRenderer::pipelineStats() const {
    static const GpuPipelineStatistics empty{};
    if (currentProfiler_ != nullptr) {
        return currentProfiler_->pipelineStats();
    }
    return profilers_.empty() ? empty : profilers_.front().pipelineStats();
}

bool PathTracerRenderer::hardwareRayTracingAvailable() const {
    return context_.supportsHardwareRayTracing();
}

RayTracingRendererStats PathTracerRenderer::rayTracingStats() const {
    RayTracingRendererStats stats{};
    stats.active = rayTracingScene_ != nullptr && rayTracingPipeline_ != nullptr;
    if (!stats.active) {
        return stats;
    }
    stats.blasCount = rayTracingScene_->blasCount();
    stats.instanceCount = rayTracingScene_->instanceCount();
    stats.accelerationStructureBytes = rayTracingScene_->accelerationStructureBytes();
    stats.lastTlasRefitMs = rayTracingScene_->lastTlasRefitMs();
    stats.sbtBytes = rayTracingPipeline_->sbtBytes();
    stats.geometry = scene_.rayTracingGeometryStats();
    stats.blasGeometry = rayTracingScene_->blasGeometryStats();
    stats.motionInstances = rayTracingScene_->motionInstanceStats();
    stats.opacityMicromapPreprocess = scene_.opacityMicromapData().stats;
    stats.opacityMicromapBuild = rayTracingScene_->opacityMicromapStats();
    return stats;
}

RayTracingDiagnosticCounters PathTracerRenderer::rayTracingDiagnosticCounters() const {
    RayTracingDiagnosticCounters counters{};
    if (!rayTracingDiagnosticCountersEnabled_ ||
        rayTracingDiagnosticCountersReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        rayTracingDiagnosticCountersReadbackBuffer_.mappedData() == nullptr) {
        return counters;
    }

    rayTracingDiagnosticCountersReadbackBuffer_.invalidate(rayTracingDiagnosticCountersReadbackBuffer_.size());
    const auto* values = static_cast<const uint32_t*>(rayTracingDiagnosticCountersReadbackBuffer_.mappedData());
    counters.cameraAnyHitInvocations = values[0];
    counters.cameraAnyHitIgnored = values[1];
    counters.cameraAnyHitAccepted = values[2];
    counters.shadowAnyHitInvocations = values[3];
    counters.shadowAnyHitIgnored = values[4];
    counters.shadowAnyHitAccepted = values[5];
    counters.surfaceTraceRays = values[6];
    counters.shadowTraceRays = values[7];
    counters.closestHitInvocations = values[8];
    counters.closestHitAlphaMaterials = values[9];
    counters.causticShadowAttempts = values[10];
    counters.causticTransmissiveHits = values[11];
    counters.causticTransmissiveVisible = values[12];
    counters.causticShadowBlocked = values[13];
    return counters;
}

PathTracerRenderer::WavefrontQueueStats PathTracerRenderer::wavefrontQueueStats() const {
    WavefrontQueueStats stats{};
    stats.buffersAllocated =
        wavefrontRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontHitQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontShadowQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontPixelStateBuffer_.handle() != VK_NULL_HANDLE;
    stats.maxPathDepth = wavefrontMaxPathDepth();
    stats.finalOutputEnabled = shouldUseWavefrontFinalOutput();
    stats.traceRaysIndirectSupported = context_.rayTracingInfo().capabilities.traceRaysIndirect;
    stats.rayQueueCapacity = wavefrontRayQueueCapacity_;
    stats.compactedRayQueueCapacity = wavefrontCompactedRayQueueCapacity_;
    stats.sortedRayQueueCapacity = wavefrontSortedRayQueueCapacity_;
    stats.hitQueueCapacity = wavefrontHitQueueCapacity_;
    stats.shadowQueueCapacity = wavefrontShadowQueueCapacity_;
    stats.pixelStateCapacity = wavefrontPixelStateCapacity_;
    stats.rayQueueBytes = static_cast<uint64_t>(wavefrontRayQueueBuffer_.size());
    stats.compactedRayQueueBytes = static_cast<uint64_t>(wavefrontCompactedRayQueueBuffer_.size());
    stats.sortedRayQueueBytes = static_cast<uint64_t>(wavefrontSortedRayQueueBuffer_.size());
    stats.hitQueueBytes = static_cast<uint64_t>(wavefrontHitQueueBuffer_.size());
    stats.shadowQueueBytes = static_cast<uint64_t>(wavefrontShadowQueueBuffer_.size());
    stats.pixelStateBytes = static_cast<uint64_t>(wavefrontPixelStateBuffer_.size());
    stats.totalBytes = stats.rayQueueBytes + stats.compactedRayQueueBytes + stats.sortedRayQueueBytes + stats.hitQueueBytes + stats.shadowQueueBytes + stats.pixelStateBytes;
    if (currentFrame_ != nullptr) {
        stats.transientArenaUsedBytes = static_cast<uint64_t>(currentFrame_->wavefrontTransientUsedBytes());
    }
    for (const auto& frame : frames_) {
        if (frame == nullptr) {
            continue;
        }
        stats.transientArenaHighWaterBytes = std::max<uint64_t>(
            stats.transientArenaHighWaterBytes,
            static_cast<uint64_t>(frame->wavefrontTransientHighWaterBytes()));
        stats.transientArenaCapacityBytes += static_cast<uint64_t>(frame->wavefrontTransientCapacityBytes());
    }
    stats.primaryGenerationEnabled = settings_.wavefrontPrimaryGenerateEnabled && stats.buffersAllocated;
    const VkDeviceSize expectedPixelCount64 = static_cast<VkDeviceSize>(renderExtent_.width) * renderExtent_.height;
    stats.expectedPrimaryRayCount = expectedPixelCount64 > static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(expectedPixelCount64);

    if (wavefrontQueueHeaderReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontQueueHeaderReadbackBuffer_.mappedData() == nullptr ||
        wavefrontQueueHeaderReadbackBuffer_.size() < sizeof(WavefrontQueueHeaderGpu) * 4u) {
        return stats;
    }

    wavefrontQueueHeaderReadbackBuffer_.invalidate(wavefrontQueueHeaderReadbackBuffer_.size());
    const auto* headers = static_cast<const WavefrontQueueHeaderGpu*>(wavefrontQueueHeaderReadbackBuffer_.mappedData());
    stats.rayQueueCount = headers[0].counters.x;
    stats.hitQueueCount = headers[1].counters.x;
    stats.shadowQueueCount = headers[2].counters.x;
    stats.pixelStateCount = headers[3].counters.x;
    stats.clearValidationCounter = headers[0].metadata.z;
    stats.traceEnabled = settings_.wavefrontTraceEnabled && stats.primaryGenerationEnabled &&
        wavefrontTraceValidationBuffer_.handle() != VK_NULL_HANDLE;
    const uint32_t expectedHitQueueCount = stats.traceEnabled ? stats.expectedPrimaryRayCount : 0u;
    const bool countsCleared = stats.primaryGenerationEnabled
        ? (stats.rayQueueCount == stats.expectedPrimaryRayCount &&
           stats.hitQueueCount == expectedHitQueueCount &&
           stats.shadowQueueCount == 0u &&
           stats.pixelStateCount == stats.expectedPrimaryRayCount)
        : (stats.rayQueueCount == 0u &&
           stats.hitQueueCount == 0u &&
           stats.shadowQueueCount == 0u &&
           stats.pixelStateCount == 0u);
    const bool capacitiesMatch =
        headers[0].counters.y == stats.rayQueueCapacity &&
        headers[1].counters.y == stats.hitQueueCapacity &&
        headers[2].counters.y == stats.shadowQueueCapacity &&
        headers[3].counters.y == stats.pixelStateCapacity;
    const bool validationCountersMatch =
        headers[0].metadata.z == kWavefrontQueueClearValidationValue &&
        headers[1].metadata.z == kWavefrontQueueClearValidationValue &&
        headers[2].metadata.z == kWavefrontQueueClearValidationValue &&
        headers[3].metadata.z == kWavefrontQueueClearValidationValue;
    const bool maxDepthMatches =
        headers[0].metadata.x == stats.maxPathDepth &&
        headers[1].metadata.x == stats.maxPathDepth &&
        headers[2].metadata.x == stats.maxPathDepth &&
        headers[3].metadata.x == stats.maxPathDepth;
    stats.clearValidationPassed = stats.buffersAllocated && countsCleared && capacitiesMatch && validationCountersMatch && maxDepthMatches;

    if (!stats.primaryGenerationEnabled ||
        wavefrontRaySampleReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontRaySampleReadbackBuffer_.mappedData() == nullptr ||
        wavefrontRaySampleReadbackBuffer_.size() < sizeof(WavefrontRayGpu) * 3u ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return stats;
    }

    wavefrontRaySampleReadbackBuffer_.invalidate(wavefrontRaySampleReadbackBuffer_.size());
    const auto* rays = static_cast<const WavefrontRayGpu*>(wavefrontRaySampleReadbackBuffer_.mappedData());
    const std::array<uint32_t, 3> sampleIndices{
        0u,
        static_cast<uint32_t>((static_cast<VkDeviceSize>(renderExtent_.height / 2u) * renderExtent_.width) + (renderExtent_.width / 2u)),
        stats.expectedPrimaryRayCount > 0u ? stats.expectedPrimaryRayCount - 1u : 0u,
    };
    auto expectedDirection = [this](uint32_t pixelIndex) {
        const uint32_t width = std::max(renderExtent_.width, 1u);
        const uint32_t height = std::max(renderExtent_.height, 1u);
        const glm::vec2 pixel(
            static_cast<float>(pixelIndex % width),
            static_cast<float>(pixelIndex / width));
        const glm::vec2 jitter = camera_.pathTracingEnabled != 0u
            ? glm::vec2(0.5f) + glm::vec2(camera_.jitter)
            : glm::vec2(0.5f);
        const glm::vec2 uv = (pixel + jitter) / glm::vec2(static_cast<float>(width), static_cast<float>(height));
        const glm::vec2 ndc = uv * 2.0f - glm::vec2(1.0f);
        const float aspect = static_cast<float>(width) / std::max(static_cast<float>(height), 1.0f);
        const float scale = std::tan(camera_.fovY * 0.5f);
        const glm::vec3 pinholeDir = glm::normalize(
            glm::vec3(camera_.forward) +
            glm::vec3(camera_.right) * ndc.x * aspect * scale -
            glm::vec3(camera_.up) * ndc.y * scale);
        const float apertureRadius = std::max(camera_.dofControls.x, 0.0f);
        if (apertureRadius <= 1.0e-6f) {
            return pinholeDir;
        }

        auto fract = [](float value) {
            return value - std::floor(value);
        };
        auto reverseBits = [](uint32_t value) {
            value = ((value & 0x55555555u) << 1u) | ((value >> 1u) & 0x55555555u);
            value = ((value & 0x33333333u) << 2u) | ((value >> 2u) & 0x33333333u);
            value = ((value & 0x0f0f0f0fu) << 4u) | ((value >> 4u) & 0x0f0f0f0fu);
            value = ((value & 0x00ff00ffu) << 8u) | ((value >> 8u) & 0x00ff00ffu);
            return (value << 16u) | (value >> 16u);
        };
        auto blueNoiseHash = [](uint32_t x, uint32_t y) {
            uint32_t h = x * 0xbc9f1d37u + y * 0x9e3779b9u;
            h ^= h >> 16u;
            h *= 0x85ebca6bu;
            h ^= h >> 13u;
            h *= 0xc2b2ae35u;
            h ^= h >> 16u;
            return h;
        };
        auto sampleHashCombine = [&](uint32_t a, uint32_t b) {
            return blueNoiseHash(a ^ (b + 0x9e3779b9u + (a << 6u) + (a >> 2u)), b);
        };
        auto unitFloat = [](uint32_t value) {
            return static_cast<float>(value >> 8u) * (1.0f / 16777216.0f);
        };
        auto blueNoise1d = [&](int32_t x, int32_t y, uint32_t frameIndex) {
            static constexpr float samples[4][4] = {
                {0.0625f, 0.5625f, 0.3125f, 0.8125f},
                {0.1875f, 0.6875f, 0.4375f, 0.9375f},
                {0.8125f, 0.3125f, 0.5625f, 0.0625f},
                {0.9375f, 0.4375f, 0.6875f, 0.1875f},
            };
            const uint32_t ix = static_cast<uint32_t>(x) & 3u;
            const uint32_t iy = static_cast<uint32_t>(y) & 3u;
            return fract(samples[iy][ix] + static_cast<float>(frameIndex & 3u) * 0.25f);
        };
        auto sampleDimension2d = [&]() {
            constexpr uint32_t dimension = 4u;
            const int32_t px = static_cast<int32_t>(pixelIndex % width);
            const int32_t py = static_cast<int32_t>(pixelIndex / width);
            uint32_t seed = sampleHashCombine(static_cast<uint32_t>(px) + 0x632be59bu, static_cast<uint32_t>(py) + 0x85157af5u);
            seed = sampleHashCombine(seed, camera_.temporalFrameIndex * 0x9e3779b9u);
            seed = sampleHashCombine(seed, 0u);
            seed = sampleHashCombine(seed, dimension * 0xc2b2ae35u);
            const uint32_t index = camera_.temporalFrameIndex + 1u + dimension * 131u;
            const float u = static_cast<float>(reverseBits(index ^ (seed & 0xffffu))) * 2.3283064365386963e-10f;
            const float v = fract((static_cast<float>(index) + 0.5f) * 0.5698402909980532f);
            const glm::vec2 scramble(
                unitFloat(sampleHashCombine(seed, dimension + 1u)),
                unitFloat(sampleHashCombine(seed, dimension + 2u)));
            const int32_t ox = static_cast<int32_t>(dimension & 7u);
            const int32_t oy = static_cast<int32_t>((dimension >> 3u) & 7u);
            const glm::vec2 stbn(
                blueNoise1d(px + ox, py + oy, camera_.temporalFrameIndex + dimension),
                blueNoise1d(px + ox + 7, py + oy + 11, camera_.temporalFrameIndex + dimension + 1u));
            return glm::vec2(fract(u + scramble.x + stbn.x * (1.0f / 256.0f)), fract(v + scramble.y + stbn.y * (1.0f / 256.0f)));
        };
        auto concentricDisk = [](glm::vec2 u) {
            const glm::vec2 offset = u * 2.0f - glm::vec2(1.0f);
            if (std::abs(offset.x) < 1.0e-7f && std::abs(offset.y) < 1.0e-7f) {
                return glm::vec2(0.0f);
            }
            float r = 0.0f;
            float theta = 0.0f;
            if (std::abs(offset.x) > std::abs(offset.y)) {
                r = offset.x;
                theta = 0.7853981633974483f * (offset.y / offset.x);
            } else {
                r = offset.y;
                theta = 1.5707963267948966f - 0.7853981633974483f * (offset.x / offset.y);
            }
            return r * glm::vec2(std::cos(theta), std::sin(theta));
        };
        glm::vec2 lens = concentricDisk(sampleDimension2d());
        const float bladeCount = camera_.dofControls.z;
        if (bladeCount >= 3.0f && glm::dot(lens, lens) > 1.0e-8f) {
            const float blades = std::clamp(std::floor(bladeCount + 0.5f), 3.0f, 16.0f);
            const float sector = 6.283185307179586f / blades;
            const float angle = std::atan2(lens.y, lens.x) - camera_.dofControls.w + sector * 0.5f;
            const float local = angle - sector * std::floor(angle / sector) - sector * 0.5f;
            const float radius = std::cos(3.141592653589793f / blades) / std::max(std::cos(local), 1.0e-4f);
            lens *= radius;
        }
        lens *= apertureRadius;
        const glm::vec3 forward = glm::normalize(glm::vec3(camera_.forward));
        const float forwardProjection = std::max(glm::dot(pinholeDir, forward), 1.0e-4f);
        const glm::vec3 focusPoint = glm::vec3(camera_.pos) + pinholeDir * (std::max(camera_.dofControls.y, 0.01f) / forwardProjection);
        const glm::vec3 lensOrigin = glm::vec3(camera_.pos) + glm::vec3(camera_.right) * lens.x + glm::vec3(camera_.up) * lens.y;
        return glm::normalize(focusPoint - lensOrigin);
    };
    auto directionError = [&](uint32_t sampleSlot) {
        const glm::vec3 actual = glm::normalize(glm::vec3(rays[sampleSlot].directionTMax));
        const glm::vec3 expected = expectedDirection(sampleIndices[sampleSlot]);
        return glm::length(actual - expected);
    };

    const bool pixelIdsMatch =
        rays[0].pixelDepthRngFlags.x == sampleIndices[0] &&
        rays[1].pixelDepthRngFlags.x == sampleIndices[1] &&
        rays[2].pixelDepthRngFlags.x == sampleIndices[2];
    stats.sampledPrimaryRayCount = 3u;
    stats.firstRayDirectionError = directionError(0u);
    stats.centerRayDirectionError = directionError(1u);
    stats.cornerRayDirectionError = directionError(2u);
    stats.maxRayDirectionError = std::max({
        stats.firstRayDirectionError,
        stats.centerRayDirectionError,
        stats.cornerRayDirectionError,
    });
    stats.primaryGenerationValidationPassed =
        stats.clearValidationPassed &&
        pixelIdsMatch &&
        stats.rayQueueCount == stats.expectedPrimaryRayCount &&
        stats.pixelStateCount == stats.expectedPrimaryRayCount &&
        stats.maxRayDirectionError <= 1.0e-5f;

    const bool alphaTestedGeometry = rayTracingScene_ != nullptr &&
        (rayTracingScene_->blasGeometryStats().alphaTestedGeometryCount > 0u ||
         scene_.rayTracingGeometryStats().alphaTestedPrimitiveCount > 0u);
    const uint32_t alphaEdgeTolerance = alphaTestedGeometry
        ? std::clamp(stats.expectedPrimaryRayCount / 4096u, 128u, 1024u)
        : 0u;

    if (stats.traceEnabled &&
        wavefrontTraceValidationReadbackBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontTraceValidationReadbackBuffer_.mappedData() != nullptr &&
        wavefrontTraceValidationReadbackBuffer_.size() >= sizeof(WavefrontTraceValidationGpu)) {
        wavefrontTraceValidationReadbackBuffer_.invalidate(wavefrontTraceValidationReadbackBuffer_.size());
        const auto* validation = static_cast<const WavefrontTraceValidationGpu*>(wavefrontTraceValidationReadbackBuffer_.mappedData());
        stats.traceCheckedPixels = validation->counters.x;
        stats.traceHitMismatchCount = validation->counters.y;
        stats.traceInstanceMismatchCount = validation->counters.z;
        stats.traceDepthMismatchCount = validation->counters.w;
        stats.traceNormalMismatchCount = validation->metrics.x;
        stats.traceValidationPassed =
            stats.primaryGenerationValidationPassed &&
            stats.hitQueueCount == stats.expectedPrimaryRayCount &&
            stats.traceCheckedPixels == stats.expectedPrimaryRayCount &&
            stats.traceHitMismatchCount <= alphaEdgeTolerance &&
            stats.traceInstanceMismatchCount == 0u &&
            stats.traceDepthMismatchCount == 0u &&
            stats.traceNormalMismatchCount <= alphaEdgeTolerance;
    }

    stats.shadeEnabled = settings_.wavefrontShadeEnabled && stats.traceEnabled &&
        wavefrontShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    if (stats.shadeEnabled &&
        wavefrontShadeValidationReadbackBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontShadeValidationReadbackBuffer_.mappedData() != nullptr &&
        wavefrontShadeValidationReadbackBuffer_.size() >= sizeof(WavefrontShadeValidationGpu)) {
        wavefrontShadeValidationReadbackBuffer_.invalidate(wavefrontShadeValidationReadbackBuffer_.size());
        const auto* validation = static_cast<const WavefrontShadeValidationGpu*>(wavefrontShadeValidationReadbackBuffer_.mappedData());
        stats.shadeCheckedPixels = validation->counters.x;
        stats.shadeHitCount = validation->counters.y;
        stats.shadeMissCount = validation->counters.z;
        stats.shadeTerminatedCount = validation->counters.w;
        stats.shadeShadowRayCount = validation->metrics.x;
        stats.shadeSecondaryRayCount = validation->metrics.y;
        stats.shadeMaterialCount = validation->metrics.z;
        stats.shadeRestirReservoirWriteCount = validation->restir.x;
        stats.shadeRestirValidCandidateCount = validation->restir.y;
        stats.shadeRestirTemporalMergeCount = validation->restir.z;
        stats.shadeRestirInvalidCandidateCount = validation->restir.w;
        stats.shadeRestirGiReservoirWriteCount = validation->restirGi.x;
        stats.shadeRestirGiValidCandidateCount = validation->restirGi.y;
        stats.shadeRestirGiTemporalMergeCount = validation->restirGi.z;
        stats.shadeRestirGiInvalidCandidateCount = validation->restirGi.w;
        stats.shadeValidationPassed =
            stats.traceValidationPassed &&
            stats.shadeCheckedPixels == stats.expectedPrimaryRayCount &&
            stats.shadeHitCount + stats.shadeMissCount == stats.expectedPrimaryRayCount &&
            stats.shadeMaterialCount == stats.shadeHitCount &&
            stats.shadeShadowRayCount <= stats.expectedPrimaryRayCount &&
            stats.shadeSecondaryRayCount <= stats.expectedPrimaryRayCount;
    }
    stats.compactEnabled = settings_.wavefrontCompactEnabled && stats.shadeEnabled &&
        wavefrontCompactedRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontCompactValidationBuffer_.handle() != VK_NULL_HANDLE;
    if (stats.compactEnabled &&
        wavefrontCompactValidationReadbackBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontCompactValidationReadbackBuffer_.mappedData() != nullptr &&
        wavefrontCompactValidationReadbackBuffer_.size() >= sizeof(WavefrontCompactValidationGpu)) {
        wavefrontCompactValidationReadbackBuffer_.invalidate(wavefrontCompactValidationReadbackBuffer_.size());
        const auto* validation = static_cast<const WavefrontCompactValidationGpu*>(wavefrontCompactValidationReadbackBuffer_.mappedData());
        stats.compactInputRayCount = validation->counters.x;
        stats.compactScannedRayCount = validation->counters.y;
        stats.compactLiveRayCount = validation->counters.z;
        stats.compactOutputRayCount = validation->counters.w;
        stats.compactDroppedInvalidCount = validation->metrics.x;
        stats.compactOverflowCount = validation->metrics.y;
        stats.compactInvalidPixelCount = validation->metrics.z;
        stats.compactMappingMismatchCount = validation->metrics.w;
        stats.compactValidationPassed =
            stats.shadeValidationPassed &&
            stats.compactInputRayCount == stats.expectedPrimaryRayCount + stats.shadeSecondaryRayCount &&
            stats.compactScannedRayCount == stats.shadeSecondaryRayCount &&
            stats.compactLiveRayCount == stats.shadeSecondaryRayCount &&
            stats.compactOutputRayCount == stats.compactLiveRayCount &&
            stats.compactOutputRayCount <= stats.compactedRayQueueCapacity &&
            stats.compactDroppedInvalidCount == 0u &&
            stats.compactOverflowCount == 0u &&
            stats.compactInvalidPixelCount == 0u &&
            stats.compactMappingMismatchCount == 0u;
    }
    stats.sortEnabled = settings_.wavefrontSortEnabled && stats.compactEnabled &&
        wavefrontSortedRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortValidationBuffer_.handle() != VK_NULL_HANDLE;
    if (stats.sortEnabled &&
        wavefrontSortValidationReadbackBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortValidationReadbackBuffer_.mappedData() != nullptr &&
        wavefrontSortValidationReadbackBuffer_.size() >= sizeof(WavefrontSortValidationGpu)) {
        wavefrontSortValidationReadbackBuffer_.invalidate(wavefrontSortValidationReadbackBuffer_.size());
        const auto* validation = static_cast<const WavefrontSortValidationGpu*>(wavefrontSortValidationReadbackBuffer_.mappedData());
        stats.sortInputRayCount = validation->counters.x;
        stats.sortOutputRayCount = validation->counters.y;
        stats.sortActiveBucketCount = validation->counters.z;
        stats.sortVerifiedRayCount = validation->counters.w;
        stats.sortOverflowCount = validation->metrics.x;
        stats.sortInvalidPixelCount = validation->metrics.y;
        stats.sortOrderViolationCount = validation->metrics.z;
        stats.sortBucketCount = validation->metrics.w;
        stats.sortValidationPassed =
            stats.compactValidationPassed &&
            stats.sortInputRayCount == stats.compactOutputRayCount &&
            stats.sortOutputRayCount == stats.compactOutputRayCount &&
            stats.sortVerifiedRayCount == stats.sortOutputRayCount &&
            stats.sortOutputRayCount <= stats.sortedRayQueueCapacity &&
            stats.sortBucketCount == kWavefrontSortBucketCount &&
            stats.sortActiveBucketCount <= kWavefrontSortBucketCount &&
            stats.sortOverflowCount == 0u &&
            stats.sortInvalidPixelCount == 0u &&
            stats.sortOrderViolationCount == 0u;
    }
    stats.secondaryShadeEnabled = stats.compactEnabled && !stats.sortEnabled &&
        wavefrontSecondaryShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    stats.secondaryTraceIndirectEnabled = stats.secondaryShadeEnabled &&
        stats.traceRaysIndirectSupported &&
        wavefrontSortDispatchBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortDispatchBuffer_.supportsDeviceAddress();
    if (stats.secondaryShadeEnabled &&
        wavefrontSecondaryShadeValidationReadbackBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSecondaryShadeValidationReadbackBuffer_.mappedData() != nullptr &&
        wavefrontSecondaryShadeValidationReadbackBuffer_.size() >= sizeof(WavefrontShadeValidationGpu)) {
        wavefrontSecondaryShadeValidationReadbackBuffer_.invalidate(wavefrontSecondaryShadeValidationReadbackBuffer_.size());
        const auto* validation = static_cast<const WavefrontShadeValidationGpu*>(wavefrontSecondaryShadeValidationReadbackBuffer_.mappedData());
        stats.secondaryShadeCheckedRays = validation->counters.x;
        stats.secondaryShadeHitCount = validation->counters.y;
        stats.secondaryShadeMissCount = validation->counters.z;
        stats.secondaryShadeTerminatedCount = validation->counters.w;
        stats.secondaryShadeShadowRayCount = validation->metrics.x;
        stats.secondaryShadeSecondaryRayCount = validation->metrics.y;
        stats.secondaryShadeMaterialCount = validation->metrics.z;
        stats.shadeRestirGiReservoirWriteCount += validation->restirGi.x;
        stats.shadeRestirGiValidCandidateCount += validation->restirGi.y;
        stats.shadeRestirGiTemporalMergeCount += validation->restirGi.z;
        stats.shadeRestirGiInvalidCandidateCount += validation->restirGi.w;
        stats.secondaryShadeValidationPassed =
            stats.compactValidationPassed &&
            stats.secondaryShadeCheckedRays == stats.compactOutputRayCount &&
            stats.secondaryShadeHitCount + stats.secondaryShadeMissCount == stats.compactOutputRayCount &&
            stats.secondaryShadeMaterialCount == stats.secondaryShadeHitCount;
    }
    stats.sortedShadeEnabled = stats.sortEnabled &&
        wavefrontSortedShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    if (stats.sortedShadeEnabled &&
        wavefrontSortedShadeValidationReadbackBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortedShadeValidationReadbackBuffer_.mappedData() != nullptr &&
        wavefrontSortedShadeValidationReadbackBuffer_.size() >= sizeof(WavefrontShadeValidationGpu)) {
        wavefrontSortedShadeValidationReadbackBuffer_.invalidate(wavefrontSortedShadeValidationReadbackBuffer_.size());
        const auto* validation = static_cast<const WavefrontShadeValidationGpu*>(wavefrontSortedShadeValidationReadbackBuffer_.mappedData());
        stats.sortedShadeCheckedRays = validation->counters.x;
        stats.sortedShadeHitCount = validation->counters.y;
        stats.sortedShadeMissCount = validation->counters.z;
        stats.sortedShadeTerminatedCount = validation->counters.w;
        stats.sortedShadeShadowRayCount = validation->metrics.x;
        stats.sortedShadeSecondaryRayCount = validation->metrics.y;
        stats.sortedShadeMaterialCount = validation->metrics.z;
        stats.shadeRestirGiReservoirWriteCount += validation->restirGi.x;
        stats.shadeRestirGiValidCandidateCount += validation->restirGi.y;
        stats.shadeRestirGiTemporalMergeCount += validation->restirGi.z;
        stats.shadeRestirGiInvalidCandidateCount += validation->restirGi.w;
        stats.sortedShadeValidationPassed =
            stats.sortValidationPassed &&
            stats.sortedShadeCheckedRays == stats.sortOutputRayCount &&
            stats.sortedShadeHitCount + stats.sortedShadeMissCount == stats.sortOutputRayCount &&
            stats.sortedShadeMaterialCount == stats.sortedShadeHitCount;
    }
    stats.shadowTraceEnabled = settings_.wavefrontShadowTraceEnabled && stats.shadeEnabled &&
        wavefrontShadowTraceValidationBuffer_.handle() != VK_NULL_HANDLE;
    if (stats.shadowTraceEnabled &&
        wavefrontShadowTraceValidationReadbackBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontShadowTraceValidationReadbackBuffer_.mappedData() != nullptr &&
        wavefrontShadowTraceValidationReadbackBuffer_.size() >= sizeof(WavefrontShadowTraceValidationGpu)) {
        wavefrontShadowTraceValidationReadbackBuffer_.invalidate(wavefrontShadowTraceValidationReadbackBuffer_.size());
        const auto* validation = static_cast<const WavefrontShadowTraceValidationGpu*>(wavefrontShadowTraceValidationReadbackBuffer_.mappedData());
        stats.shadowTraceCheckedRays = validation->counters.x;
        stats.shadowTraceVisibleCount = validation->counters.y;
        stats.shadowTraceOccludedCount = validation->counters.z;
        stats.shadowTraceAppliedCount = validation->counters.w;
        stats.directLightingCheckedPixels = validation->metrics.x;
        stats.directLightingMismatchCount = validation->metrics.y;
        stats.directLightingMaxAbsError = static_cast<float>(validation->metrics.z) / 1000000.0f;
        stats.directLightingMaxRelativeError = static_cast<float>(validation->metrics.w) / 1000000.0f;
        stats.directLightingParityPassed =
            stats.directLightingCheckedPixels == stats.expectedPrimaryRayCount &&
            stats.directLightingMismatchCount <= alphaEdgeTolerance;
        stats.shadowTraceValidationPassed =
            stats.shadeValidationPassed &&
            stats.shadowTraceCheckedRays == stats.shadeShadowRayCount &&
            stats.shadowTraceVisibleCount + stats.shadowTraceOccludedCount == stats.shadowTraceCheckedRays &&
            stats.shadowTraceAppliedCount == stats.shadowTraceVisibleCount;
    }
    return stats;
}

void PathTracerRenderer::setRayTracingDiagnosticCountersEnabled(bool enabled) {
    rayTracingDiagnosticCountersEnabled_ = enabled;
    rayTracingDiagnosticCountersCleared_ = false;
    debugParams_.flags = enabled ? rendererDebugFlagRayTracingCounters : 0u;
}

AtmosphereLutStats PathTracerRenderer::atmosphereLutStats() const {
    return atmosphereLutSystem_ != nullptr ? atmosphereLutSystem_->stats() : AtmosphereLutStats{};
}

VkDescriptorImageInfo PathTracerRenderer::viewportImageDescriptor() const {
    if (presentationImage_.handle() == VK_NULL_HANDLE) {
        return {};
    }
    return presentationImage_.sampledDescriptor(VK_NULL_HANDLE);
}

void PathTracerRenderer::retireResolutionResources() {
    RetiredResolutionResources retired{};
    retired.releaseFrame = temporalFrameIndex_ + static_cast<uint32_t>(std::max<size_t>(frames_.size(), 1)) + 1u;
    auto retireImage = [&](Image& image) {
        if (image.handle() != VK_NULL_HANDLE) {
            retired.images.push_back(std::move(image));
        }
    };
    auto retireBuffer = [&](Buffer& buffer) {
        if (buffer.handle() != VK_NULL_HANDLE) {
            retired.buffers.push_back(std::move(buffer));
        }
    };

    retireImage(rawImage_);
    retireImage(denoisedImage_);
    retireImage(historyImage_);
    retireImage(diffuseResolvedImage_);
    retireImage(specularResolvedImage_);
    retireImage(diffuseHistoryImage_);
    retireImage(specularHistoryImage_);
    retireImage(directDiffuseMomentsImage_);
    retireImage(directSpecularMomentsImage_);
    retireImage(indirectDiffuseMomentsImage_);
    retireImage(indirectSpecularMomentsImage_);
    retireImage(historyLengthImage_);
    retireImage(directDiffuseResolvedMomentsImage_);
    retireImage(directSpecularResolvedMomentsImage_);
    retireImage(indirectDiffuseResolvedMomentsImage_);
    retireImage(indirectSpecularResolvedMomentsImage_);
    retireImage(historyLengthResolvedImage_);
    retireImage(momentDebugImage_);
    retireImage(momentDebugResolvedImage_);
    retireImage(taaImage_);
    retireImage(taaHistoryImage_);
    retireImage(presentationImage_);

    retireBuffer(cameraBuffer_);
    retireBuffer(denoiserParamsBuffer_);
    retireBuffer(prevCameraBuffer_);
    retireBuffer(debugParamsBuffer_);
    retireBuffer(accumulationBuffer_);
    retireBuffer(varianceBuffer_);
    retireBuffer(depthNormalBuffer_);
    retireBuffer(worldPositionBuffer_);
    retireBuffer(previousWorldPositionBuffer_);
    retireBuffer(velocityBuffer_);
    retireBuffer(entityIdBuffer_);
    retireBuffer(pathDataBuffer_);
    retireBuffer(rayTracingDiagnosticCountersBuffer_);
    retireBuffer(rayTracingDiagnosticCountersReadbackBuffer_);
    retireBuffer(wavefrontRayQueueBuffer_);
    retireBuffer(wavefrontCompactedRayQueueBuffer_);
    retireBuffer(wavefrontSortedRayQueueBuffer_);
    retireBuffer(wavefrontHitQueueBuffer_);
    retireBuffer(wavefrontShadowQueueBuffer_);
    retireBuffer(wavefrontPixelStateBuffer_);
    retireBuffer(wavefrontQueueHeaderReadbackBuffer_);
    retireBuffer(wavefrontRaySampleReadbackBuffer_);
    retireBuffer(wavefrontTraceValidationBuffer_);
    retireBuffer(wavefrontTraceValidationReadbackBuffer_);
    retireBuffer(wavefrontShadeValidationBuffer_);
    retireBuffer(wavefrontShadeValidationReadbackBuffer_);
    retireBuffer(wavefrontSecondaryShadeValidationBuffer_);
    retireBuffer(wavefrontSecondaryShadeValidationReadbackBuffer_);
    retireBuffer(wavefrontSortedShadeValidationBuffer_);
    retireBuffer(wavefrontSortedShadeValidationReadbackBuffer_);
    retireBuffer(wavefrontCompactValidationBuffer_);
    retireBuffer(wavefrontCompactValidationReadbackBuffer_);
    retireBuffer(wavefrontSortValidationBuffer_);
    retireBuffer(wavefrontSortValidationReadbackBuffer_);
    retireBuffer(wavefrontSortDispatchBuffer_);
    retireBuffer(wavefrontShadowTraceValidationBuffer_);
    retireBuffer(wavefrontShadowTraceValidationReadbackBuffer_);
    retireBuffer(restirReservoirBuffer_);
    retireBuffer(wavefrontRestirReservoirBuffer_);
    retireBuffer(previousRestirReservoirBuffer_);
    retireBuffer(restirSpatialReservoirBuffer_);
    retireBuffer(restirGiReservoirBuffer_);
    retireBuffer(previousRestirGiReservoirBuffer_);
    retireBuffer(restirGiSpatialReservoirBuffer_);
    retireBuffer(wavefrontRestirGiReservoirBuffer_);
    retireBuffer(selectionParamsBuffer_);
    retireBuffer(histogramBuffer_);
    retireBuffer(exposureBuffer_);

    if (!retired.images.empty() || !retired.buffers.empty()) {
        validationLog_.recordPass("deferred resolution resource retirement release_frame=" + std::to_string(retired.releaseFrame));
        retiredResolutionResources_.push_back(std::move(retired));
    }
}

void PathTracerRenderer::releaseRetiredResolutionResources() {
    retiredResolutionResources_.erase(
        std::remove_if(
            retiredResolutionResources_.begin(),
            retiredResolutionResources_.end(),
            [this](const RetiredResolutionResources& retired) {
                return temporalFrameIndex_ >= retired.releaseFrame;
            }),
        retiredResolutionResources_.end());
}

void PathTracerRenderer::createResolutionResources(VkExtent2D renderExtent, VkExtent2D displayExtent) {
    renderExtent_ = renderExtent;
    displayExtent_ = displayExtent;
    const VkDeviceSize pixelCount = static_cast<VkDeviceSize>(renderExtent.width) * renderExtent.height;
    const uint32_t wavefrontQueueCapacity = settings_.wavefrontQueuesEnabled ? wavefrontQueueCapacityFor(pixelCount) : 0u;
    wavefrontRayQueueCapacity_ = wavefrontQueueCapacity;
    wavefrontCompactedRayQueueCapacity_ = settings_.wavefrontCompactEnabled ? wavefrontQueueCapacity : 0u;
    wavefrontSortedRayQueueCapacity_ = settings_.wavefrontSortEnabled ? wavefrontQueueCapacity : 0u;
    wavefrontHitQueueCapacity_ = wavefrontQueueCapacity;
    wavefrontShadowQueueCapacity_ = wavefrontQueueCapacity;
    wavefrontPixelStateCapacity_ = settings_.wavefrontQueuesEnabled
        ? (pixelCount > static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max())
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(pixelCount))
        : 0u;
    denoiserHistoryValid_ = false;
    taaHistoryValid_ = false;
    rawImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer raw hdr",
    });
    denoisedImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer denoised hdr",
    });
    historyImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer denoiser history",
    });
    diffuseResolvedImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer current diffuse denoiser history",
    });
    specularResolvedImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer current specular denoiser history",
    });
    diffuseHistoryImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer diffuse denoiser history",
    });
    specularHistoryImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer specular denoiser history",
    });
    directDiffuseMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer direct diffuse moments",
    });
    directSpecularMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer direct specular moments",
    });
    indirectDiffuseMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer indirect diffuse moments",
    });
    indirectSpecularMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer indirect specular moments",
    });
    historyLengthImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_UINT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer history length",
    });
    directDiffuseResolvedMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer direct diffuse resolved moments",
    });
    directSpecularResolvedMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer direct specular resolved moments",
    });
    indirectDiffuseResolvedMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer indirect diffuse resolved moments",
    });
    indirectSpecularResolvedMomentsImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer indirect specular resolved moments",
    });
    historyLengthResolvedImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_UINT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer history length resolved",
    });
    momentDebugImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer moment debug",
    });
    momentDebugResolvedImage_.create(allocator_, ImageDesc{
        .width = renderExtent.width,
        .height = renderExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer moment debug resolved",
    });
    taaImage_.create(allocator_, ImageDesc{
        .width = displayExtent.width,
        .height = displayExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer taa hdr",
    });
    taaHistoryImage_.create(allocator_, ImageDesc{
        .width = displayExtent.width,
        .height = displayExtent.height,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "path tracer taa history",
    });
    presentationImage_.create(allocator_, ImageDesc{
        .width = displayExtent.width,
        .height = displayExtent.height,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "path tracer presentation ldr",
    });
    cameraBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(CameraUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "camera uniform",
    });
    denoiserParamsBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(DenoiserParams),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "denoiser params",
    });
    prevCameraBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(PrevCameraUniform),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "denoiser previous camera",
    });
    debugParamsBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(RendererDebugParams),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "renderer debug params",
    });
    accumulationBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(float) * 4,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path accumulation",
    });
    varianceBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path variance packed",
    });
    depthNormalBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t) * 4,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path depth normal roughness packed",
    });
    worldPositionBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t) * 2,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path world position packed",
    });
    previousWorldPositionBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t) * 2,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "previous world position packed",
    });
    velocityBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "screen velocity packed",
    });
    entityIdBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::Readback,
        .persistentMapped = true,
        .debugName = "path entity id pick buffer",
    });
    pathDataBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(PathDataGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "path data channels",
    });
    rayTracingDiagnosticCountersBuffer_.create(allocator_, BufferDesc{
        .size = kRayTracingDiagnosticCounterCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "ray tracing diagnostic counters",
    });
    rayTracingDiagnosticCountersReadbackBuffer_.create(allocator_, BufferDesc{
        .size = kRayTracingDiagnosticCounterCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::Readback,
        .persistentMapped = true,
        .debugName = "ray tracing diagnostic counters readback",
    });
    rayTracingDiagnosticCountersCleared_ = false;
    if (settings_.wavefrontQueuesEnabled) {
        wavefrontQueueHeaderReadbackBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontQueueHeaderGpu) * 4u,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::Readback,
            .persistentMapped = true,
            .debugName = "wavefront queue header readback",
        });
        wavefrontRaySampleReadbackBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontRayGpu) * 3u,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::Readback,
            .persistentMapped = true,
            .debugName = "wavefront primary ray sample readback",
        });
        wavefrontTraceValidationBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontTraceValidationGpu),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::GpuOnly,
            .debugName = "wavefront trace validation counters",
        });
        wavefrontTraceValidationReadbackBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontTraceValidationGpu),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::Readback,
            .persistentMapped = true,
            .debugName = "wavefront trace validation readback",
        });
        wavefrontShadeValidationBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontShadeValidationGpu),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::GpuOnly,
            .debugName = "wavefront shade validation counters",
        });
        wavefrontShadeValidationReadbackBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontShadeValidationGpu),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::Readback,
            .persistentMapped = true,
            .debugName = "wavefront shade validation readback",
        });
        if (settings_.wavefrontCompactEnabled) {
            wavefrontCompactValidationBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontCompactValidationGpu),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::GpuOnly,
                .debugName = "wavefront compact validation counters",
            });
            wavefrontCompactValidationReadbackBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontCompactValidationGpu),
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::Readback,
                .persistentMapped = true,
                .debugName = "wavefront compact validation readback",
            });
            wavefrontSecondaryShadeValidationBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontShadeValidationGpu),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::GpuOnly,
                .debugName = "wavefront secondary shade validation counters",
            });
            wavefrontSecondaryShadeValidationReadbackBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontShadeValidationGpu),
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::Readback,
                .persistentMapped = true,
                .debugName = "wavefront secondary shade validation readback",
            });
            wavefrontSortDispatchBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(uint32_t) * 8u,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory = BufferMemory::GpuOnly,
                .debugName = "wavefront queue indirect dispatch args",
            });
        }
        if (settings_.wavefrontSortEnabled) {
            wavefrontSortValidationBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontSortValidationGpu),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::GpuOnly,
                .debugName = "wavefront sort validation counters",
            });
            wavefrontSortValidationReadbackBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontSortValidationGpu),
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::Readback,
                .persistentMapped = true,
                .debugName = "wavefront sort validation readback",
            });
            wavefrontSortedShadeValidationBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontShadeValidationGpu),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::GpuOnly,
                .debugName = "wavefront sorted shade validation counters",
            });
            wavefrontSortedShadeValidationReadbackBuffer_.create(allocator_, BufferDesc{
                .size = sizeof(WavefrontShadeValidationGpu),
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memory = BufferMemory::Readback,
                .persistentMapped = true,
                .debugName = "wavefront sorted shade validation readback",
            });
        }
        wavefrontShadowTraceValidationBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontShadowTraceValidationGpu),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::GpuOnly,
            .debugName = "wavefront shadow trace validation counters",
        });
        wavefrontShadowTraceValidationReadbackBuffer_.create(allocator_, BufferDesc{
            .size = sizeof(WavefrontShadowTraceValidationGpu),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory = BufferMemory::Readback,
            .persistentMapped = true,
            .debugName = "wavefront shadow trace validation readback",
        });
    }
    restirReservoirBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(RestirReservoirGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir current reservoir",
    });
    wavefrontRestirReservoirBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(RestirReservoirGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "wavefront restir di candidate reservoir",
    });
    previousRestirReservoirBuffer_.create(allocator_, BufferDesc{
        .size = pixelCount * sizeof(RestirReservoirGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir previous reservoir",
    });
    const BufferDesc restirSpatialDesc{
        .size = pixelCount * sizeof(RestirReservoirGpu),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir spatial reservoir",
    };
    const VkDeviceSize restirGiReservoirBytes = pixelCount * restirGiReservoirStride();
    const BufferDesc restirGiCurrentDesc{
        .size = restirGiReservoirBytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = resourceAliasingEnabled_ && restirSpatialDesc.size == restirGiReservoirBytes && restirSpatialDesc.usage == (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
            ? "alias group restir gi current / restir spatial reservoir"
            : "restir gi current reservoir",
    };
    restirGiReservoirBuffer_.create(allocator_, restirGiCurrentDesc);
    if (resourceAliasingEnabled_ &&
        restirSpatialDesc.size == restirGiCurrentDesc.size &&
        restirSpatialDesc.usage == restirGiCurrentDesc.usage &&
        restirSpatialDesc.memory == restirGiCurrentDesc.memory) {
        restirSpatialReservoirBuffer_.aliasFrom(restirGiReservoirBuffer_, restirSpatialDesc);
    } else {
        restirSpatialReservoirBuffer_.create(allocator_, restirSpatialDesc);
    }
    previousRestirGiReservoirBuffer_.create(allocator_, BufferDesc{
        .size = restirGiReservoirBytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir gi previous reservoir",
    });
    restirGiSpatialReservoirBuffer_.create(allocator_, BufferDesc{
        .size = restirGiReservoirBytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "restir gi spatial reservoir",
    });
    wavefrontRestirGiReservoirBuffer_.create(allocator_, BufferDesc{
        .size = restirGiReservoirBytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "wavefront restir gi candidate reservoir",
    });
    selectionParamsBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(SelectionParams),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "selection outline params",
    });
    histogramBuffer_.create(allocator_, BufferDesc{
        .size = kHistogramBinCount * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "auto exposure histogram",
    });
    exposureBuffer_.create(allocator_, BufferDesc{
        .size = sizeof(float) * 4,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "auto exposure state",
    });
    const float exposureState[4] = {settings_.exposure, settings_.exposure, settings_.targetLuminance, 0.0f};
    exposureBuffer_.write(exposureState, sizeof(exposureState));
    exposureBuffer_.flush(sizeof(exposureState));
    if (temporalSystem_) {
        temporalSystem_->createHistorySlot(
            "denoiser_history",
            historyImage_.format(),
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            1.0f);
        temporalSystem_->createHistorySlot(
            "denoiser_diffuse_history",
            diffuseHistoryImage_.format(),
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            0.75f);
        temporalSystem_->createHistorySlot(
            "denoiser_specular_history",
            specularHistoryImage_.format(),
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            0.75f);
        temporalSystem_->createHistorySlot(
            "previous_world_position",
            VK_FORMAT_R32G32_UINT,
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            0.8f);
        temporalSystem_->createHistorySlot(
            "taa_history",
            taaHistoryImage_.format(),
            VkExtent2D{displayExtent.width, displayExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            1.0f);
        temporalSystem_->createHistorySlot(
            "restir_reservoir",
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            0.75f);
        temporalSystem_->createHistorySlot(
            "restir_gi_reservoir",
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VkExtent2D{renderExtent.width, renderExtent.height},
            TemporalSystem::TemporalResidency::Persistent,
            0.75f);
        temporalSystem_->setCameraCut(true, AccumulationResetReason::Resize);
    }
}

void PathTracerRenderer::updateCamera() {
    debugParams_.view = static_cast<uint32_t>(settings_.debugView);
    debugParams_.flags = rayTracingDiagnosticCountersEnabled_ ? rendererDebugFlagRayTracingCounters : 0u;
    debugParams_.selectedInstance = selectedInstanceId_;
    debugParams_.scale = settings_.debugScale;

    camera_.sunIntensity = settings_.sunIntensity;
    camera_.skyIntensity = settings_.skyIntensity;
    camera_.exposure = settings_.usePhysicalCamera
        ? 2.0f * std::exp2(14.0f - physicalCamera_.ev100())
        : settings_.exposure;
    camera_.pathTracingEnabled = settings_.pathTracingEnabled ? 1u : 0u;
    camera_.maxBounces = adaptiveEffectiveMaxBounces_;
    camera_.sunlightEnabled = settings_.sunlightEnabled ? 1u : 0u;
    camera_.directLightingEnabled = settings_.directLightingEnabled ? 1u : 0u;
    camera_.sunAngularRadius = settings_.sunAngularRadius;
    camera_.indirectStrength = settings_.indirectStrength;
    camera_.environmentDirectSamples = adaptiveEffectiveEnvironmentSamples_;
    camera_.renderControls = glm::vec4(
        settings_.shadowRayBias,
        settings_.shadowDistanceBias,
        settings_.fireflyClamp,
        settings_.russianRouletteMinSurvival);
    // Manual exposure in this renderer is still calibrated around the legacy sun scalar.
    // Keep raw lux only for physical-camera mode where exposure is EV-based.
    camera_.sunDirectionIlluminance = glm::vec4(
        settings_.sunDirection,
        settings_.usePhysicalCamera ? settings_.sunIlluminanceLux : settings_.sunIntensity);
    camera_.sunColorAngularRadius = glm::vec4(settings_.sunColor, settings_.sunAngularRadius);
    camera_.restirGiControls = glm::uvec4(
        settings_.restirGiTemporalMaxAge,
        effectiveRestirGiHalfResolution() ? 1u : 0u,
        settings_.restirGiVisibilityRayBudget,
        settings_.specularAaEnabled ? 1u : 0u);
    camera_.pathTraceControls = glm::uvec4(
        settings_.samplesPerPixel,
        effectiveLimitSamplesPerPixel() ? 1u : 0u,
        rayTracingDiagnosticCountersEnabled_ ? 1u : 0u,
        settings_.mneeCausticsEnabled ? 1u : 0u);
    camera_.dofControls = glm::vec4(
        settings_.dofApertureRadius,
        settings_.dofFocusDistance,
        static_cast<float>(settings_.dofBladeCount),
        settings_.dofBokehRotation);
    camera_.motionBlurControls = glm::vec4(
        settings_.motionBlurEnabled && context_.supportsRayTracingMotionBlur() ? 1.0f : 0.0f,
        settings_.motionBlurShutterOpen,
        settings_.motionBlurShutterClose,
        0.0f);
    camera_.volumeControls = glm::vec4(
        settings_.homogeneousVolumeEnabled ? 1.0f : 0.0f,
        settings_.homogeneousVolumeScattering,
        settings_.homogeneousVolumeAbsorption,
        settings_.homogeneousVolumeAnisotropy);
    const bool temporalCameraCut = temporalSystem_ ? temporalSystem_->isCameraCut() : temporalFrameIndex_ <= 1u;
    const bool temporalHistoryAvailable = !temporalCameraCut;
    camera_.atmosphere = glm::vec4(
        settings_.sunElevation,
        static_cast<float>(settings_.restirMode),
        temporalHistoryAvailable ? 1.0f : 0.0f,
        settings_.sunAzimuth);
    camera_.frameCount = frameCount_;
    camera_.temporalFrameIndex = settings_.fixedSeed.has_value() ? *settings_.fixedSeed : temporalFrameIndex_;

    if (cameraChangedThisFrame_) {
        stillFrameCount_ = 0;
    } else {
        stillFrameCount_ = std::min(stillFrameCount_ + 1u, 60u);
    }
    const bool restoreFullJitter = stillFrameCount_ >= 2u;
    const float effectiveJitterScale = cameraChangedThisFrame_ ? 0.0f : (restoreFullJitter ? 1.0f : 0.0f);
    camera_.effectiveJitterScale = effectiveJitterScale;
    camera_.cameraMoving = cameraChangedThisFrame_ ? 1u : 0u;

    const float aspect = renderExtent_.height > 0 ? static_cast<float>(renderExtent_.width) / static_cast<float>(renderExtent_.height) : 1.0f;
    const glm::vec3 eye = glm::vec3(camera_.pos);
    const glm::vec3 center = eye + glm::normalize(glm::vec3(camera_.forward));
    const glm::mat4 view = glm::lookAtRH(eye, center, glm::normalize(glm::vec3(camera_.up)));
    glm::mat4 projection = glm::perspectiveRH_ZO(camera_.fovY, aspect, 0.01f, 1000.0f);
    projection[1][1] *= -1.0f;
    const bool jitterEnabled = settings_.pathTracingEnabled && settings_.taaEnabled && settings_.cameraJitterEnabled && effectiveJitterScale > 0.0f && renderExtent_.width > 0 && renderExtent_.height > 0;
    const uint32_t jitterBase = settings_.fixedSeed.has_value() ? *settings_.fixedSeed : temporalFrameIndex_;
    const uint32_t jitterIndex = settings_.fixedSeed.has_value() ? jitterBase : (jitterBase + 1u);
    const glm::vec2 currentJitter = jitterEnabled
        ? glm::vec2(halton(jitterIndex, 2u) - 0.5f, halton(jitterIndex, 3u) - 0.5f) * effectiveJitterScale
        : glm::vec2(0.0f);
    projection[2][0] -= currentJitter.x * 2.0f / static_cast<float>(std::max(renderExtent_.width, 1u));
    projection[2][1] -= currentJitter.y * 2.0f / static_cast<float>(std::max(renderExtent_.height, 1u));
    const glm::mat4 viewProj = projection * view;
    camera_.jitter = glm::vec4(currentJitter, previousJitter_);
    Buffer& frameUniforms = currentFrame_->uniformRing();
    frameUniforms.write(&camera_, sizeof(camera_), kFrameCameraUniformOffset);
    frameUniforms.flush(sizeof(camera_), kFrameCameraUniformOffset);

    prevCamera_.viewProj = viewProj;
    prevCamera_.invViewProj = glm::inverse(viewProj);
    prevCamera_.prevViewProj = previousViewProj_;
    prevCamera_.currentPos = camera_.pos;
    prevCamera_.prevPos = previousCameraPos_;
    prevCamera_.jitter = glm::vec4(currentJitter, previousJitter_);
    frameUniforms.write(&prevCamera_, sizeof(prevCamera_), kFramePrevCameraUniformOffset);
    frameUniforms.flush(sizeof(prevCamera_), kFramePrevCameraUniformOffset);

    const bool denoiserDebugView =
        debugParams_.view <= 4u ||
        debugParams_.view == static_cast<uint32_t>(RendererDebugView::MotionVectors) ||
        debugParams_.view == static_cast<uint32_t>(RendererDebugView::TemporalReactiveMask) ||
        debugParams_.view == static_cast<uint32_t>(RendererDebugView::TemporalHistoryWeight) ||
        (debugParams_.view >= static_cast<uint32_t>(RendererDebugView::PathDirectDiffuse) &&
         debugParams_.view <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularHistoryWeight)) ||
        (debugParams_.view >= static_cast<uint32_t>(RendererDebugView::DenoiserDirectDiffuseVariance) &&
         debugParams_.view <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularRawVariance));
    const bool allowDenoiserForDebugView = denoiserDebugView;
    const bool stablePreview = shouldRunTaa();
    const bool allowDenoiserWhileMoving = settings_.denoiseWhileMoving || stablePreview || !cameraChangedThisFrame_;
    denoiserParams_.enabled = settings_.pathTracingEnabled && settings_.denoiserEnabled && allowDenoiserForDebugView && allowDenoiserWhileMoving && !adaptiveSkipDenoiser_ ? 1u : 0u;
    denoiserParams_.strength = settings_.denoiserStrength;
    denoiserParams_.frameCount = temporalFrameIndex_;
    denoiserParams_.width = renderExtent_.width;
    denoiserParams_.height = renderExtent_.height;
    denoiserParams_.atrousIterations = adaptiveEffectiveAtrousIterations_;
    denoiserParams_.debugView = denoiserDebugView ? debugParams_.view : 0u;
    denoiserParams_.resetHistory = (temporalCameraCut || !denoiserHistoryValid_) ? 1u : 0u;
    if (denoiserParams_.resetHistory != 0u) {
        denoiserFramesSinceReset_ = 0;
    }
    denoiserParams_.framesSinceReset = denoiserFramesSinceReset_;
    if (denoiserParams_.enabled != 0u || denoiserParams_.debugView != 0u) {
        ++denoiserFramesSinceReset_;
    }
    frameUniforms.write(&denoiserParams_, sizeof(denoiserParams_), kFrameDenoiserParamsOffset);
    frameUniforms.flush(sizeof(denoiserParams_), kFrameDenoiserParamsOffset);

    taaParams_.enabled = shouldRunTaa() ? 1u : 0u;
    taaParams_.frameCount = temporalFrameIndex_;
    taaParams_.width = displayExtent_.width;
    taaParams_.height = displayExtent_.height;
    const float taaFeedback = cameraChangedThisFrame_
        ? std::min(settings_.taaFeedback, 0.05f)
        : settings_.taaFeedback;
    taaParams_.feedback = std::clamp(taaFeedback, 0.01f, 0.5f);
    taaParams_.velocityScale = 64.0f;
    taaParams_.resetHistory = temporalCameraCut ? 1u : 0u;
    taaParams_.sharpeningStrength = settings_.taaSharpeningStrength;
    taaParams_.historyValid = taaHistoryValid_ ? 1u : 0u;
    taaParams_.cameraMoving = cameraChangedThisFrame_ ? 1u : 0u;
    taaParams_.renderWidth = renderExtent_.width;
    taaParams_.renderHeight = renderExtent_.height;
    taaParams_.motionFeedback = settings_.taaMotionFeedback;
    taaParams_.reactiveFeedback = settings_.taaReactiveFeedback;
    frameUniforms.write(&taaParams_, sizeof(taaParams_), kFrameTaaParamsOffset);
    frameUniforms.flush(sizeof(taaParams_), kFrameTaaParamsOffset);
    if (temporalFrameIndex_ == 1u || temporalFrameIndex_ % 120u == 0u) {
        validationLog_.recordPass(
            "temporal state temporal=" + std::to_string(temporalFrameIndex_) +
            " accumulation=" + std::to_string(frameCount_) +
            " jitterScale=" + std::to_string(effectiveJitterScale) +
            " moving=" + std::to_string(cameraChangedThisFrame_ ? 1u : 0u) +
            " taaHistory=" + std::to_string(taaHistoryValid_ ? 1u : 0u) +
            " taaFeedback=" + std::to_string(taaParams_.feedback));
    }

    restirSpatialParams_.width = renderExtent_.width;
    restirSpatialParams_.height = renderExtent_.height;
    restirSpatialParams_.frameCount = temporalFrameIndex_;
    restirSpatialParams_.enabled = (shouldRunRestirSpatial() || shouldUseRestirGiReservoirs()) ? 1u : 0u;
    restirSpatialParams_.giSpatialRounds = settings_.restirGiSpatialRounds;
    restirSpatialParams_.giHalfResolution = effectiveRestirGiHalfResolution() ? 1u : 0u;
    restirSpatialParams_.giTemporalMaxAge = settings_.restirGiTemporalMaxAge;
    restirSpatialParams_.giVisibilityRayBudget = settings_.restirGiVisibilityRayBudget;
    restirSpatialParams_.giSpatialRadius = settings_.restirGiSpatialRadius;
    restirSpatialParams_.giDepthThresholdScale = settings_.restirGiDepthThresholdScale;
    restirSpatialParams_.giSpatialCompatibilityThreshold = settings_.restirGiSpatialCompatibilityThreshold;
    frameUniforms.write(&restirSpatialParams_, sizeof(restirSpatialParams_), kFrameRestirSpatialParamsOffset);
    frameUniforms.flush(sizeof(restirSpatialParams_), kFrameRestirSpatialParamsOffset);

    fogParams_.width = renderExtent_.width;
    fogParams_.height = renderExtent_.height;
    fogParams_.debugView = debugParams_.view;
    fogParams_.enabled = settings_.pathTracingEnabled ? 1u : 0u;
    frameUniforms.write(&fogParams_, sizeof(fogParams_), kFrameFogParamsOffset);
    frameUniforms.flush(sizeof(fogParams_), kFrameFogParamsOffset);

    frameUniforms.write(&debugParams_, sizeof(debugParams_), kFrameDebugParamsOffset);
    frameUniforms.flush(sizeof(debugParams_), kFrameDebugParamsOffset);

    previousViewProj_ = viewProj;
    previousCameraPos_ = camera_.pos;
    previousJitter_ = currentJitter;
}

void PathTracerRenderer::recordPathTrace(VkCommandBuffer commandBuffer, bool deferPostTraceCompute) {
    asyncHistoryCopyPending_ = false;
    asyncTaaHistoryCopyPending_ = false;
    asyncPostProcessPending_ = false;
    if (dumpRenderGraphPath_.has_value() || dumpRenderGraphDotPath_.has_value()) {
        recordRenderGraphPlan();
    }
    currentProfiler_->resetForFrame(commandBuffer);
    if (atmosphereLutSystem_ != nullptr) {
        validationLog_.recordPass("atmosphere lut update");
        currentProfiler_->write(commandBuffer, GpuProfiler::AtmosphereStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        atmosphereLutSystem_->setSkyDirection(settings_.sunDirection, settings_.skyIntensity);
        atmosphereLutSystem_->setAtmosphereParams(settings_.rayleighScaleHeight, settings_.mieScaleHeight, settings_.mieAnisotropy, settings_.groundAlbedo);
        atmosphereLutSystem_->setCameraPosition(glm::vec3(camera_.pos));
        atmosphereLutSystem_->record(commandBuffer, currentFrame_->descriptors(), currentProfiler_);
        if (const AtmosphereSamplingSystem* sampling = atmosphereLutSystem_->samplingSystem()) {
            scene_.setSkyCdfDimensions(sampling->skyViewWidth(), sampling->skyViewHeight());
        }
        currentProfiler_->write(commandBuffer, GpuProfiler::AtmosphereEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    recordPathTraceGraph(commandBuffer);
    currentProfiler_->markStatsSubmitted();
    recordRestirSpatial(commandBuffer);
    recordHeightFog(commandBuffer);

    if (deferPostTraceCompute) {
        currentProfiler_->write(commandBuffer, GpuProfiler::AsyncProducerEnd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        asyncPostProcessPending_ = true;
    } else {
        recordPostTraceCompute(commandBuffer, false);
    }
    cameraChangedThisFrame_ = false;
    if (temporalSystem_) {
        temporalSystem_->endFrame();
    }
    currentProfiler_->markSubmitted();
}

bool PathTracerRenderer::recordAsyncComputeWork(VkCommandBuffer commandBuffer) {
    const bool recordHistoryCopy = asyncHistoryCopyPending_;
    const bool recordTaaHistoryCopy = asyncTaaHistoryCopyPending_;
    const bool recordPostProcess = asyncPostProcessPending_;
    if (!recordHistoryCopy && !recordTaaHistoryCopy && !recordPostProcess) {
        return false;
    }
    asyncHistoryCopyPending_ = false;
    asyncTaaHistoryCopyPending_ = false;
    asyncPostProcessPending_ = false;
    currentProfiler_->write(commandBuffer, GpuProfiler::AsyncComputeStart, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    if (recordPostProcess) {
        recordPostTraceCompute(commandBuffer, false);
    } else {
        if (recordHistoryCopy) {
            copyHistoryResources(commandBuffer);
        }
        if (recordTaaHistoryCopy) {
            copyTaaHistory(commandBuffer);
        }
    }
    currentProfiler_->write(commandBuffer, GpuProfiler::AsyncComputeEnd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    return true;
}

void PathTracerRenderer::recordPostTraceCompute(VkCommandBuffer commandBuffer, bool deferHistoryCopy) {
    if (shouldRunDenoiser()) {
        recordMomentUpdate(commandBuffer);
        recordDenoiser(commandBuffer);
        if (deferHistoryCopy) {
            asyncHistoryCopyPending_ = true;
        } else {
            copyHistoryResources(commandBuffer);
        }
    } else {
        skipDenoiserPass(commandBuffer);
    }
    if (shouldRunTaa()) {
        recordTaa(commandBuffer, deferHistoryCopy);
    }
    if (settings_.autoExposureEnabled) {
        recordAutoExposure(commandBuffer);
    }
    recordToneMap(commandBuffer);
    recordSelectionOutline(commandBuffer);
}

void PathTracerRenderer::bindWavefrontFrameResources() {
    wavefrontRayQueueBuffer_.destroy();
    wavefrontCompactedRayQueueBuffer_.destroy();
    wavefrontSortedRayQueueBuffer_.destroy();
    wavefrontHitQueueBuffer_.destroy();
    wavefrontShadowQueueBuffer_.destroy();
    wavefrontPixelStateBuffer_.destroy();

    if (!settings_.wavefrontQueuesEnabled || currentFrame_ == nullptr || wavefrontRayQueueCapacity_ == 0u) {
        return;
    }

    constexpr VkBufferUsageFlags queueUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr VkDeviceSize queueAlignment = 256;
    const VkDeviceSize rayQueueBytes = sizeof(WavefrontQueueHeaderGpu) + static_cast<VkDeviceSize>(wavefrontRayQueueCapacity_) * sizeof(WavefrontRayGpu);
    const VkDeviceSize compactedRayQueueBytes = settings_.wavefrontCompactEnabled && wavefrontCompactedRayQueueCapacity_ > 0u
        ? sizeof(WavefrontQueueHeaderGpu) + static_cast<VkDeviceSize>(wavefrontCompactedRayQueueCapacity_) * sizeof(WavefrontRayGpu)
        : 0u;
    const VkDeviceSize sortedRayQueueBytes = settings_.wavefrontSortEnabled && wavefrontSortedRayQueueCapacity_ > 0u
        ? sizeof(WavefrontQueueHeaderGpu) + static_cast<VkDeviceSize>(wavefrontSortedRayQueueCapacity_) * sizeof(WavefrontRayGpu)
        : 0u;
    const VkDeviceSize hitQueueBytes = sizeof(WavefrontQueueHeaderGpu) + static_cast<VkDeviceSize>(wavefrontHitQueueCapacity_) * sizeof(WavefrontHitGpu);
    const VkDeviceSize shadowQueueBytes = sizeof(WavefrontQueueHeaderGpu) + static_cast<VkDeviceSize>(wavefrontShadowQueueCapacity_) * sizeof(WavefrontShadowRayGpu);
    const VkDeviceSize pixelStateBytes = sizeof(WavefrontQueueHeaderGpu) + static_cast<VkDeviceSize>(wavefrontPixelStateCapacity_) * sizeof(WavefrontPixelStateGpu);
    VkDeviceSize reserveBytes = 0;
    auto appendReserve = [&](VkDeviceSize bytes) {
        if (bytes == 0u) {
            return;
        }
        reserveBytes = Buffer::alignUp(reserveBytes, queueAlignment) + bytes;
    };
    appendReserve(rayQueueBytes);
    appendReserve(compactedRayQueueBytes);
    appendReserve(sortedRayQueueBytes);
    appendReserve(hitQueueBytes);
    appendReserve(shadowQueueBytes);
    appendReserve(pixelStateBytes);
    currentFrame_->reserveWavefrontTransientBytes(reserveBytes);

    auto allocateQueue = [this, queueUsage, queueAlignment](VkDeviceSize size, const char* name) {
        return currentFrame_->allocateWavefrontTransientBuffer(BufferDesc{
            .size = size,
            .usage = queueUsage,
            .memory = BufferMemory::GpuOnly,
            .debugName = name,
        }, queueAlignment);
    };

    wavefrontRayQueueBuffer_ = allocateQueue(rayQueueBytes, "wavefront ray queue");
    if (compactedRayQueueBytes > 0u) {
        wavefrontCompactedRayQueueBuffer_ = allocateQueue(compactedRayQueueBytes, "wavefront compacted ray queue");
    }
    if (sortedRayQueueBytes > 0u) {
        wavefrontSortedRayQueueBuffer_ = allocateQueue(sortedRayQueueBytes, "wavefront sorted ray queue");
    }
    wavefrontHitQueueBuffer_ = allocateQueue(hitQueueBytes, "wavefront hit queue");
    wavefrontShadowQueueBuffer_ = allocateQueue(shadowQueueBytes, "wavefront shadow ray queue");
    wavefrontPixelStateBuffer_ = allocateQueue(pixelStateBytes, "wavefront pixel state");
}

void PathTracerRenderer::recordPathTraceGraph(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    auto imageResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .bufferOffset = buffer.baseOffset(),
            .external = true,
            .debugName = name,
        };
    };

    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr"));
    graph.resources()[raw.index].hasInitialAccess = true;
    graph.resources()[raw.index].initialAccess = ResourceAccess{
        .stage = rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : pathTraceShaderStage(),
        .access = rawImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .layout = rawImage_.layout(),
    };
    const RenderGraphResourceId accumulation = graph.createBuffer(bufferResource(accumulationBuffer_, "accumulation"));
    const RenderGraphResourceId variance = graph.createBuffer(bufferResource(varianceBuffer_, "variance"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal"));
    const RenderGraphResourceId worldPosition = graph.createBuffer(bufferResource(worldPositionBuffer_, "world position"));
    const RenderGraphResourceId entityIds = graph.createBuffer(bufferResource(entityIdBuffer_, "entity ids"));
    const RenderGraphResourceId velocity = graph.createBuffer(bufferResource(velocityBuffer_, "screen velocity"));
    const RenderGraphResourceId pathData = graph.createBuffer(bufferResource(pathDataBuffer_, "path data"));
    const RenderGraphResourceId rayTracingDiagnosticCounters = graph.createBuffer(bufferResource(rayTracingDiagnosticCountersBuffer_, "ray tracing diagnostic counters"));
    const RenderGraphResourceId restirReservoir = graph.createBuffer(bufferResource(restirReservoirBuffer_, "restir reservoir"));
    RenderGraphResourceId wavefrontRestirReservoir{};
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(bufferResource(previousRestirReservoirBuffer_, "previous restir reservoir"));
    const bool resetRestirHistory =
        settings_.restirMode != RestirMode::ClassicNee &&
        ((temporalSystem_ != nullptr && temporalSystem_->isCameraCut()) ||
         (temporalSystem_ == nullptr && temporalFrameIndex_ <= 1u));
    const bool useRestirGiReservoirs = shouldUseRestirGiReservoirs();
    const bool useWavefrontQueues = settings_.wavefrontQueuesEnabled &&
        wavefrontRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontHitQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontShadowQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontPixelStateBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontPrimaryGenerate = useWavefrontQueues && settings_.wavefrontPrimaryGenerateEnabled;
    const bool useWavefrontTrace = useWavefrontPrimaryGenerate && settings_.wavefrontTraceEnabled &&
        wavefrontTraceValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontShade = useWavefrontTrace && settings_.wavefrontShadeEnabled &&
        wavefrontShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontCompact = useWavefrontShade && settings_.wavefrontCompactEnabled &&
        wavefrontCompactedRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontCompactValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontShadowTrace = useWavefrontShade && settings_.wavefrontShadowTraceEnabled &&
        wavefrontShadowTraceValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontFinalOutput = useWavefrontCompact && useWavefrontShadowTrace && shouldUseWavefrontFinalOutput();
    const bool useWavefrontSort = useWavefrontCompact && !useWavefrontFinalOutput && settings_.wavefrontSortEnabled &&
        wavefrontSortedRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortValidationBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortDispatchBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortedShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontSecondaryShade = useWavefrontCompact && !useWavefrontFinalOutput && !useWavefrontSort &&
        wavefrontSecondaryShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontDebugWrite = !useWavefrontFinalOutput && useWavefrontShade && shouldRunWavefrontDebugWrite();
    RenderGraphResourceId wavefrontRayQueue{};
    RenderGraphResourceId wavefrontCompactedRayQueue{};
    RenderGraphResourceId wavefrontSortedRayQueue{};
    RenderGraphResourceId wavefrontSortDispatch{};
    RenderGraphResourceId wavefrontHitQueue{};
    RenderGraphResourceId wavefrontShadowQueue{};
    RenderGraphResourceId wavefrontPixelState{};
    RenderGraphResourceId wavefrontSortValidation{};
    if (useWavefrontQueues) {
        wavefrontRayQueue = graph.createBuffer(bufferResource(wavefrontRayQueueBuffer_, "wavefront ray queue"));
        if (useWavefrontCompact) {
            wavefrontCompactedRayQueue = graph.createBuffer(bufferResource(wavefrontCompactedRayQueueBuffer_, "wavefront compacted ray queue"));
        }
        if (useWavefrontCompact) {
            wavefrontSortDispatch = graph.createBuffer(bufferResource(wavefrontSortDispatchBuffer_, "wavefront queue indirect dispatch args"));
        }
        if (useWavefrontSort) {
            wavefrontSortedRayQueue = graph.createBuffer(bufferResource(wavefrontSortedRayQueueBuffer_, "wavefront sorted ray queue"));
            wavefrontSortValidation = graph.createBuffer(bufferResource(wavefrontSortValidationBuffer_, "wavefront sort validation"));
        }
        wavefrontHitQueue = graph.createBuffer(bufferResource(wavefrontHitQueueBuffer_, "wavefront hit queue"));
        wavefrontShadowQueue = graph.createBuffer(bufferResource(wavefrontShadowQueueBuffer_, "wavefront shadow ray queue"));
        wavefrontPixelState = graph.createBuffer(bufferResource(wavefrontPixelStateBuffer_, "wavefront pixel state"));
    }
    RenderGraphResourceId restirGiReservoir{};
    RenderGraphResourceId previousRestirGiReservoir{};
    RenderGraphResourceId restirGiSpatialReservoir{};
    RenderGraphResourceId wavefrontRestirGiReservoir{};
    if (useRestirGiReservoirs) {
        restirGiReservoir = graph.createBuffer(bufferResource(restirGiReservoirBuffer_, "restir gi reservoir"));
        previousRestirGiReservoir = graph.createBuffer(bufferResource(previousRestirGiReservoirBuffer_, "previous restir gi reservoir"));
        restirGiSpatialReservoir = graph.createBuffer(bufferResource(restirGiSpatialReservoirBuffer_, "restir gi spatial reservoir"));
    }
    if (useWavefrontQueues && wavefrontRestirGiReservoirBuffer_.handle() != VK_NULL_HANDLE) {
        wavefrontRestirGiReservoir = graph.createBuffer(bufferResource(wavefrontRestirGiReservoirBuffer_, "wavefront restir gi candidate reservoir"));
    }
    if (useWavefrontQueues && wavefrontRestirReservoirBuffer_.handle() != VK_NULL_HANDLE) {
        wavefrontRestirReservoir = graph.createBuffer(bufferResource(wavefrontRestirReservoirBuffer_, "wavefront restir di candidate reservoir"));
    }
    const PipelineDomain traceDomain = PipelineDomain::RayTracing;
    if (resetRestirHistory) {
        graph.addPass("restir_history_clear")
            .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                validationLog_.recordPass("restir history clear");
                currentProfiler_->write(cmd, GpuProfiler::RestirHistoryClearStart, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
                vkCmdFillBuffer(cmd, previousRestirReservoirBuffer_.handle(), 0, previousRestirReservoirBuffer_.size(), 0u);
                currentProfiler_->write(cmd, GpuProfiler::RestirHistoryClearEnd, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            });
    }
    if (useRestirGiReservoirs) {
        RenderGraphPass& restirGiClearPass = graph.addPass("restir_gi_clear")
            .addStorageWrite(restirGiReservoir, PipelineDomain::Transfer);
        if (!restirGiHistoryValid_) {
            restirGiClearPass
                .addStorageWrite(previousRestirGiReservoir, PipelineDomain::Transfer)
                .addStorageWrite(restirGiSpatialReservoir, PipelineDomain::Transfer);
        }
        restirGiClearPass.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                validationLog_.recordPass("restir gi reservoir clear");
                currentProfiler_->write(cmd, GpuProfiler::RestirGiClearStart, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
                vkCmdFillBuffer(cmd, restirGiReservoirBuffer_.handle(), 0, restirGiReservoirBuffer_.size(), 0u);
                if (!restirGiHistoryValid_) {
                    vkCmdFillBuffer(cmd, previousRestirGiReservoirBuffer_.handle(), 0, previousRestirGiReservoirBuffer_.size(), 0u);
                    vkCmdFillBuffer(cmd, restirGiSpatialReservoirBuffer_.handle(), 0, restirGiSpatialReservoirBuffer_.size(), 0u);
                }
                currentProfiler_->write(cmd, GpuProfiler::RestirGiClearEnd, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            });
    }
    if (useWavefrontQueues) {
        graph.addPass("wavefront_queue_clear")
            .addStorageWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageWrite(wavefrontPixelState, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontQueueClearPass(cmd);
            });
    }
    if (useWavefrontPrimaryGenerate) {
        graph.addPass("wavefront_primary_generate")
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontPrimaryGeneratePass(cmd);
            });
    }
    if (useWavefrontTrace) {
        graph.addPass("wavefront_trace_rt")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontTracePass(cmd);
            });
    }
    if (useWavefrontShade) {
        const RenderGraphResourceId wavefrontShadeValidation = graph.createBuffer(bufferResource(wavefrontShadeValidationBuffer_, "wavefront shade validation"));
        graph.addPass("wavefront_shade_validation_clear")
            .addStorageWrite(wavefrontShadeValidation, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                vkCmdFillBuffer(cmd, wavefrontShadeValidationBuffer_.handle(), 0, wavefrontShadeValidationBuffer_.size(), 0u);
            });
        RenderGraphPass& wavefrontShadePass = graph.addPass("wavefront_shade");
        wavefrontShadePass
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(depthNormal, PipelineDomain::Compute)
            .addStorageWrite(worldPosition, PipelineDomain::Compute)
            .addStorageWrite(entityIds, PipelineDomain::Compute)
            .addStorageWrite(variance, PipelineDomain::Compute)
            .addStorageWrite(velocity, PipelineDomain::Compute)
            .addStorageWrite(pathData, PipelineDomain::Compute)
            .addStorageWrite(wavefrontShadeValidation, PipelineDomain::Compute);
        wavefrontShadePass
            .addStorageRead(previousRestirReservoir, PipelineDomain::Compute)
            .addStorageWrite(wavefrontRestirReservoir.valid() ? wavefrontRestirReservoir : restirReservoir, PipelineDomain::Compute);
        wavefrontShadePass.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordWavefrontShadePass(cmd);
        });
    }
    if (useWavefrontCompact && !useWavefrontFinalOutput) {
        const RenderGraphResourceId wavefrontCompactValidation = graph.createBuffer(bufferResource(wavefrontCompactValidationBuffer_, "wavefront compact validation"));
        RenderGraphPass& compactPass = graph.addPass("wavefront_compact")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontSortDispatch, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontCompactValidation, PipelineDomain::Compute);
        compactPass.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordWavefrontCompactPass(cmd);
        });
    }
    if (useWavefrontSort) {
        graph.addPass("wavefront_sort")
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontSortedRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontSortDispatch, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontSortValidation, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontSortPass(cmd);
            });
    }
    RenderGraphResourceId wavefrontShadowTraceValidation{};
    if (useWavefrontShadowTrace && !useWavefrontFinalOutput) {
        wavefrontShadowTraceValidation = graph.createBuffer(bufferResource(wavefrontShadowTraceValidationBuffer_, "wavefront shadow trace validation"));
        graph.addPass("wavefront_shadow_trace_validation_clear")
            .addStorageWrite(wavefrontShadowTraceValidation, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                vkCmdFillBuffer(cmd, wavefrontShadowTraceValidationBuffer_.handle(), 0, wavefrontShadowTraceValidationBuffer_.size(), 0u);
            });
        graph.addPass("wavefront_shadow_trace_rt")
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::RayTracing)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontShadowTraceValidation, PipelineDomain::RayTracing)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontShadowTracePass(cmd);
            });
    }
    if (!useWavefrontFinalOutput) {
        RenderGraphPass& pathTracePass = graph.addPass("path_trace_rt")
            .addStorageWrite(raw, traceDomain)
            .addStorageWrite(entityIds, traceDomain)
            .addStorageReadWrite(accumulation, traceDomain)
            .addStorageWrite(variance, traceDomain)
            .addStorageWrite(depthNormal, traceDomain)
            .addStorageWrite(worldPosition, traceDomain)
            .addStorageWrite(velocity, traceDomain)
            .addStorageWrite(pathData, traceDomain)
            .addStorageWrite(rayTracingDiagnosticCounters, traceDomain)
            .addStorageRead(previousRestirReservoir, traceDomain)
            .addStorageWrite(restirReservoir, traceDomain);
        if (useRestirGiReservoirs) {
            pathTracePass
                .addStorageReadWrite(restirGiReservoir, traceDomain)
                .addStorageRead(previousRestirGiReservoir, traceDomain)
                .addStorageRead(restirGiSpatialReservoir, traceDomain);
        }
        pathTracePass.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordPathTracePass(cmd);
        });
    }
    if (useWavefrontShadowTrace && !useWavefrontFinalOutput) {
        graph.addPass("wavefront_direct_lighting_validate")
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageRead(pathData, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowTraceValidation, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontDirectLightingValidationPass(cmd);
            });
    }
    RenderGraphResourceId wavefrontTraceValidation{};
    if (useWavefrontTrace && !useWavefrontFinalOutput) {
        wavefrontTraceValidation = graph.createBuffer(bufferResource(wavefrontTraceValidationBuffer_, "wavefront trace validation"));
        graph.addPass("wavefront_trace_validation_clear")
            .addStorageWrite(wavefrontTraceValidation, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                vkCmdFillBuffer(cmd, wavefrontTraceValidationBuffer_.handle(), 0, wavefrontTraceValidationBuffer_.size(), 0u);
            });
        graph.addPass("wavefront_trace_validate")
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageRead(entityIds, PipelineDomain::Compute)
            .addStorageWrite(wavefrontTraceValidation, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontTraceValidationPass(cmd);
            });
    }
    if (useRestirGiReservoirs && !useWavefrontFinalOutput) {
        graph.addPass("restir_gi_spatial")
            .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageWrite(restirGiSpatialReservoir, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordRestirGiSpatialPass(cmd);
            });
    }
    if (shouldRunRestirGiFinal()) {
        graph.addPass("restir_gi_final")
            .addStorageReadWrite(raw, PipelineDomain::Compute)
            .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
            .addStorageRead(previousRestirGiReservoir, PipelineDomain::Compute)
            .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordRestirGiFinalPass(cmd);
            });
    }
    if (useWavefrontFinalOutput) {
        const RenderGraphResourceId wavefrontFinalCompactValidation = graph.createBuffer(bufferResource(wavefrontCompactValidationBuffer_, "wavefront final compact validation"));
        const RenderGraphResourceId wavefrontFinalShadeValidation = graph.createBuffer(bufferResource(wavefrontSecondaryShadeValidationBuffer_, "wavefront final shade validation"));
        const uint32_t maxDepth = std::max(1u, wavefrontMaxPathDepth());
        for (uint32_t bounce = 1u; bounce < maxDepth; ++bounce) {
            const std::string suffix = "_bounce_" + std::to_string(bounce);
            graph.addPass(("wavefront_final_compact" + suffix).c_str())
                .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontCompactedRayQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontSortDispatch, PipelineDomain::Compute)
                .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
                .addStorageWrite(wavefrontFinalCompactValidation, PipelineDomain::Compute)
                .setExecuteCallback([this, bounce](FrameGraphContext&, VkCommandBuffer cmd) {
                    recordWavefrontCompactPass(cmd, bounce == 1u);
                });
            graph.addPass(("wavefront_final_hit_queue_clear" + suffix).c_str())
                .addStorageWrite(wavefrontHitQueue, PipelineDomain::Transfer)
                .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                    recordWavefrontHitQueueCountClearPass(cmd);
                });
            graph.addPass(("wavefront_final_secondary_trace_rt" + suffix).c_str())
                .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::RayTracing)
                .addStorageRead(wavefrontSortDispatch, PipelineDomain::RayTracing)
                .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing)
                .setExecuteCallback([this, bounce](FrameGraphContext&, VkCommandBuffer cmd) {
                    recordWavefrontTracePass(
                        cmd,
                        &wavefrontCompactedRayQueueBuffer_,
                        false,
                        &wavefrontSortDispatchBuffer_,
                        sizeof(uint32_t) * 4u,
                        bounce == 1u ? GpuProfiler::WavefrontSecondaryTraceStart : GpuProfiler::Count,
                        bounce == 1u ? GpuProfiler::WavefrontSecondaryTraceEnd : GpuProfiler::Count);
                });
            if (bounce == 1u) {
                graph.addPass(("wavefront_final_shade_validation_clear" + suffix).c_str())
                    .addStorageWrite(wavefrontFinalShadeValidation, PipelineDomain::Transfer)
                    .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                        vkCmdFillBuffer(cmd, wavefrontSecondaryShadeValidationBuffer_.handle(), 0, wavefrontSecondaryShadeValidationBuffer_.size(), 0u);
                    });
            }
            RenderGraphPass& finalSecondaryShadePass = graph.addPass(("wavefront_final_secondary_shade" + suffix).c_str())
                .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
                .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
                .addStorageRead(wavefrontSortDispatch, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
                .addStorageWrite(wavefrontFinalShadeValidation, PipelineDomain::Compute);
            const bool writeProductionRestirGiCandidates = useRestirGiReservoirs && restirGiReservoir.valid();
            if (writeProductionRestirGiCandidates) {
                finalSecondaryShadePass.addStorageWrite(restirGiReservoir, PipelineDomain::Compute);
            }
            const uint32_t finalSecondaryShadeFlags = kWavefrontShadeFlagSortedInput |
                (writeProductionRestirGiCandidates ? kWavefrontShadeFlagRestirGiCandidateWrite : 0u);
            finalSecondaryShadePass.setExecuteCallback([this, bounce, finalSecondaryShadeFlags](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontShadePass(
                    cmd,
                    &wavefrontCompactedRayQueueBuffer_,
                    &wavefrontSecondaryShadeValidationBuffer_,
                    &wavefrontSecondaryShadeValidationReadbackBuffer_,
                    bounce == 1u ? GpuProfiler::WavefrontSecondaryShadeStart : GpuProfiler::Count,
                    bounce == 1u ? GpuProfiler::WavefrontSecondaryShadeEnd : GpuProfiler::Count,
                    "wavefront final secondary shade rays=",
                    finalSecondaryShadeFlags,
                    &wavefrontSortDispatchBuffer_);
            });
        }

        const RenderGraphResourceId wavefrontFinalShadowTraceValidation = graph.createBuffer(bufferResource(wavefrontShadowTraceValidationBuffer_, "wavefront final shadow trace validation"));
        graph.addPass("wavefront_final_shadow_trace_validation_clear")
            .addStorageWrite(wavefrontFinalShadowTraceValidation, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                vkCmdFillBuffer(cmd, wavefrontShadowTraceValidationBuffer_.handle(), 0, wavefrontShadowTraceValidationBuffer_.size(), 0u);
            });
        graph.addPass("wavefront_final_shadow_trace_rt")
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::RayTracing)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontFinalShadowTraceValidation, PipelineDomain::RayTracing)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontShadowTracePass(cmd, true);
            });
        if (useRestirGiReservoirs) {
            graph.addPass("wavefront_final_restir_gi_spatial")
                .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
                .addStorageRead(depthNormal, PipelineDomain::Compute)
                .addStorageWrite(restirGiSpatialReservoir, PipelineDomain::Compute)
                .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                    recordRestirGiSpatialPass(cmd);
                });
        }
        RenderGraphPass& finalWritePass = graph.addPass("wavefront_final_write")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(raw, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontDebugWritePass(cmd, true);
            });
        if (wavefrontRestirReservoir.valid()) {
            finalWritePass.addStorageRead(wavefrontRestirReservoir, PipelineDomain::Compute);
        } else if (restirReservoirBuffer_.handle() != VK_NULL_HANDLE) {
            finalWritePass.addStorageRead(restirReservoir, PipelineDomain::Compute);
        }
        if (useRestirGiReservoirs) {
            finalWritePass.addStorageRead(restirGiReservoir, PipelineDomain::Compute);
            graph.addPass("wavefront_final_restir_gi_final")
                .addStorageReadWrite(raw, PipelineDomain::Compute)
                .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
                .addStorageRead(previousRestirGiReservoir, PipelineDomain::Compute)
                .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Compute)
                .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                    recordRestirGiFinalPass(cmd);
                });
        }
    }
    if (useWavefrontSecondaryShade) {
        const RenderGraphResourceId wavefrontSecondaryShadeValidation = graph.createBuffer(bufferResource(wavefrontSecondaryShadeValidationBuffer_, "wavefront secondary shade validation"));
        RenderGraphPass& secondaryHitClear = graph.addPass("wavefront_secondary_hit_queue_clear")
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::Transfer);
        if (wavefrontTraceValidation.valid()) {
            secondaryHitClear.addStorageRead(wavefrontTraceValidation, PipelineDomain::Transfer);
        }
        secondaryHitClear.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontHitQueueCountClearPass(cmd);
            });
        graph.addPass("wavefront_secondary_trace_rt")
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::RayTracing)
            .addStorageRead(wavefrontSortDispatch, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontTracePass(
                    cmd,
                    &wavefrontCompactedRayQueueBuffer_,
                    false,
                    &wavefrontSortDispatchBuffer_,
                    sizeof(uint32_t) * 4u,
                    GpuProfiler::WavefrontSecondaryTraceStart,
                    GpuProfiler::WavefrontSecondaryTraceEnd);
            });
        graph.addPass("wavefront_secondary_shade_validation_clear")
            .addStorageWrite(wavefrontSecondaryShadeValidation, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                vkCmdFillBuffer(cmd, wavefrontSecondaryShadeValidationBuffer_.handle(), 0, wavefrontSecondaryShadeValidationBuffer_.size(), 0u);
            });
        RenderGraphPass& secondaryShadePass = graph.addPass("wavefront_secondary_shade")
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontSortDispatch, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontSecondaryShadeValidation, PipelineDomain::Compute);
        const bool writeWavefrontRestirGiCandidates = useRestirGiReservoirs && wavefrontRestirGiReservoir.valid();
        if (writeWavefrontRestirGiCandidates) {
            secondaryShadePass.addStorageWrite(wavefrontRestirGiReservoir, PipelineDomain::Compute);
        }
        const uint32_t secondaryShadeFlags = kWavefrontShadeFlagSortedInput |
            (writeWavefrontRestirGiCandidates ? kWavefrontShadeFlagRestirGiCandidateWrite : 0u);
        secondaryShadePass.setExecuteCallback([this, secondaryShadeFlags](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontShadePass(
                    cmd,
                    &wavefrontCompactedRayQueueBuffer_,
                    &wavefrontSecondaryShadeValidationBuffer_,
                    &wavefrontSecondaryShadeValidationReadbackBuffer_,
                    GpuProfiler::WavefrontSecondaryShadeStart,
                    GpuProfiler::WavefrontSecondaryShadeEnd,
                    "wavefront secondary shade rays=",
                    secondaryShadeFlags,
                    &wavefrontSortDispatchBuffer_);
            });
    }
    if (useWavefrontSort) {
        const RenderGraphResourceId wavefrontSortedShadeValidation = graph.createBuffer(bufferResource(wavefrontSortedShadeValidationBuffer_, "wavefront sorted shade validation"));
        RenderGraphPass& sortedHitClear = graph.addPass("wavefront_sorted_hit_queue_clear")
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::Transfer);
        if (wavefrontTraceValidation.valid()) {
            sortedHitClear.addStorageRead(wavefrontTraceValidation, PipelineDomain::Transfer);
        }
        sortedHitClear.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontHitQueueCountClearPass(cmd);
            });
        graph.addPass("wavefront_sorted_trace_rt")
            .addStorageRead(wavefrontSortedRayQueue, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontTracePass(
                    cmd,
                    &wavefrontSortedRayQueueBuffer_,
                    false,
                    nullptr,
                    0,
                    GpuProfiler::WavefrontSortedTraceStart,
                    GpuProfiler::WavefrontSortedTraceEnd);
            });
        graph.addPass("wavefront_sorted_shade_validation_clear")
            .addStorageWrite(wavefrontSortedShadeValidation, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                vkCmdFillBuffer(cmd, wavefrontSortedShadeValidationBuffer_.handle(), 0, wavefrontSortedShadeValidationBuffer_.size(), 0u);
            });
        RenderGraphPass& sortedShadePass = graph.addPass("wavefront_sorted_shade")
            .addStorageRead(wavefrontSortedRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontSortedShadeValidation, PipelineDomain::Compute);
        const bool writeWavefrontRestirGiCandidates = useRestirGiReservoirs && wavefrontRestirGiReservoir.valid();
        if (writeWavefrontRestirGiCandidates) {
            sortedShadePass.addStorageWrite(wavefrontRestirGiReservoir, PipelineDomain::Compute);
        }
        const uint32_t sortedShadeFlags = kWavefrontShadeFlagSortedInput |
            (writeWavefrontRestirGiCandidates ? kWavefrontShadeFlagRestirGiCandidateWrite : 0u);
        sortedShadePass.setExecuteCallback([this, sortedShadeFlags](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontShadePass(
                    cmd,
                    &wavefrontSortedRayQueueBuffer_,
                    &wavefrontSortedShadeValidationBuffer_,
                    &wavefrontSortedShadeValidationReadbackBuffer_,
                    GpuProfiler::WavefrontSortedShadeStart,
                    GpuProfiler::WavefrontSortedShadeEnd,
                    "wavefront sorted shade rays=",
                    sortedShadeFlags);
            });
    }
    if (useWavefrontDebugWrite) {
        RenderGraphPass& debugWritePass = graph.addPass("wavefront_debug_write")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageRead(useWavefrontCompact ? wavefrontCompactedRayQueue : wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(raw, PipelineDomain::Compute)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordWavefrontDebugWritePass(cmd);
            });
        if (wavefrontRestirReservoir.valid()) {
            debugWritePass.addStorageRead(wavefrontRestirReservoir, PipelineDomain::Compute);
        } else if (restirReservoirBuffer_.handle() != VK_NULL_HANDLE) {
            debugWritePass.addStorageRead(restirReservoir, PipelineDomain::Compute);
        }
        if (settings_.debugView == RendererDebugView::WavefrontRestirGi && wavefrontRestirGiReservoir.valid()) {
            debugWritePass.addStorageRead(wavefrontRestirGiReservoir, PipelineDomain::Compute);
        }
    }
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordPathTracePass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("path tracing rt");
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    const bool copyDiagnosticCounters =
        rayTracingDiagnosticCountersEnabled_ &&
        rayTracingDiagnosticCountersBuffer_.handle() != VK_NULL_HANDLE &&
        rayTracingDiagnosticCountersReadbackBuffer_.handle() != VK_NULL_HANDLE;
    if (copyDiagnosticCounters && !rayTracingDiagnosticCountersCleared_) {
        vkCmdFillBuffer(commandBuffer, rayTracingDiagnosticCountersBuffer_.handle(), 0, rayTracingDiagnosticCountersBuffer_.size(), 0u);
        bufferMemoryBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            pathTraceShaderStage(),
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            rayTracingDiagnosticCountersBuffer_.handle(),
            rayTracingDiagnosticCountersBuffer_.size());
        rayTracingDiagnosticCountersCleared_ = true;
    }

    currentProfiler_->write(commandBuffer, GpuProfiler::PathTraceStart, pathTraceShaderStage());
    currentProfiler_->beginPipelineStats(commandBuffer);
    recordHardwarePathTrace(commandBuffer);
    currentProfiler_->endPipelineStats(commandBuffer);
    currentProfiler_->write(commandBuffer, GpuProfiler::PathTraceEnd, pathTraceShaderStage());

    if (copyDiagnosticCounters) {
        bufferMemoryBarrier(
            commandBuffer,
            pathTraceShaderStage(),
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            rayTracingDiagnosticCountersBuffer_.handle(),
            rayTracingDiagnosticCountersBuffer_.size());
        VkBufferCopy copy{};
        copy.size = rayTracingDiagnosticCountersBuffer_.size();
        vkCmdCopyBuffer(
            commandBuffer,
            rayTracingDiagnosticCountersBuffer_.handle(),
            rayTracingDiagnosticCountersReadbackBuffer_.handle(),
            1,
            &copy);
    }
}

void PathTracerRenderer::recordWavefrontQueueClearPass(VkCommandBuffer commandBuffer) {
    if (wavefrontQueueClearPipeline_ == nullptr || wavefrontQueueClearSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (wavefrontRayQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontHitQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontShadowQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE) {
        return;
    }

    validationLog_.recordPass(
        "wavefront queue clear capacity=" + std::to_string(wavefrontRayQueueCapacity_) +
        " maxDepth=" + std::to_string(wavefrontMaxPathDepth()));

    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontQueueClearSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontRayQueueBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontHitQueueBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontShadowQueueBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .update(context_.device(), set);

    const WavefrontQueueClearPush push{
        .rayCapacity = wavefrontRayQueueCapacity_,
        .hitCapacity = wavefrontHitQueueCapacity_,
        .shadowCapacity = wavefrontShadowQueueCapacity_,
        .pixelCapacity = wavefrontPixelStateCapacity_,
        .maxPathDepth = wavefrontMaxPathDepth(),
        .frameIndex = temporalFrameIndex_,
        .validationValue = kWavefrontQueueClearValidationValue,
        .flags = 0u,
    };

    wavefrontQueueClearPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontQueueClearPipeline_->layout(),
        0,
        1,
        &descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        wavefrontQueueClearPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdDispatch(commandBuffer, 1, 1, 1);

    if (wavefrontQueueHeaderReadbackBuffer_.handle() == VK_NULL_HANDLE) {
        return;
    }

    auto copyHeader = [&](const Buffer& source, uint32_t index) {
        bufferMemoryBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            source.handle(),
            sizeof(WavefrontQueueHeaderGpu),
            source.baseOffset());
        VkBufferCopy copy{};
        copy.srcOffset = source.baseOffset();
        copy.dstOffset = sizeof(WavefrontQueueHeaderGpu) * index;
        copy.size = sizeof(WavefrontQueueHeaderGpu);
        vkCmdCopyBuffer(commandBuffer, source.handle(), wavefrontQueueHeaderReadbackBuffer_.handle(), 1, &copy);
    };
    copyHeader(wavefrontRayQueueBuffer_, 0u);
    copyHeader(wavefrontHitQueueBuffer_, 1u);
    copyHeader(wavefrontShadowQueueBuffer_, 2u);
    copyHeader(wavefrontPixelStateBuffer_, 3u);
}

void PathTracerRenderer::recordWavefrontPrimaryGeneratePass(VkCommandBuffer commandBuffer) {
    if (wavefrontPrimaryGeneratePipeline_ == nullptr || wavefrontPrimaryGenerateSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (wavefrontRayQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return;
    }

    const VkDeviceSize pixelCount = static_cast<VkDeviceSize>(renderExtent_.width) * renderExtent_.height;
    validationLog_.recordPass(
        "wavefront primary generate pixels=" + std::to_string(pixelCount) +
        " jitter=(" + std::to_string(camera_.jitter.x) + "," + std::to_string(camera_.jitter.y) + ")");

    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontPrimaryGenerateSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameCameraUniformOffset, sizeof(CameraUniform)))
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontRayQueueBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .update(context_.device(), set);

    const bool cameraCut = temporalSystem_ ? temporalSystem_->isCameraCut() : temporalFrameIndex_ <= 1u;
    const WavefrontPrimaryGeneratePush push{
        .width = renderExtent_.width,
        .height = renderExtent_.height,
        .frameIndex = temporalFrameIndex_,
        .flags = 0u,
        .cameraCut = cameraCut ? 1u : 0u,
    };

    wavefrontPrimaryGeneratePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontPrimaryGeneratePipeline_->layout(),
        0,
        1,
        &descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        wavefrontPrimaryGeneratePipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    wavefrontPrimaryGeneratePipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height, 8u, 8u);

    if (wavefrontQueueHeaderReadbackBuffer_.handle() == VK_NULL_HANDLE) {
        return;
    }

    auto copyHeader = [&](const Buffer& source, uint32_t index) {
        bufferMemoryBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            source.handle(),
            sizeof(WavefrontQueueHeaderGpu),
            source.baseOffset());
        VkBufferCopy copy{};
        copy.srcOffset = source.baseOffset();
        copy.dstOffset = sizeof(WavefrontQueueHeaderGpu) * index;
        copy.size = sizeof(WavefrontQueueHeaderGpu);
        vkCmdCopyBuffer(commandBuffer, source.handle(), wavefrontQueueHeaderReadbackBuffer_.handle(), 1, &copy);
    };
    copyHeader(wavefrontRayQueueBuffer_, 0u);
    copyHeader(wavefrontPixelStateBuffer_, 3u);

    if (wavefrontRaySampleReadbackBuffer_.handle() == VK_NULL_HANDLE || pixelCount == 0) {
        return;
    }

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        wavefrontRayQueueBuffer_.handle(),
        wavefrontRayQueueBuffer_.size(),
        wavefrontRayQueueBuffer_.baseOffset());

    const std::array<uint32_t, 3> sampleIndices{
        0u,
        static_cast<uint32_t>((static_cast<VkDeviceSize>(renderExtent_.height / 2u) * renderExtent_.width) + (renderExtent_.width / 2u)),
        static_cast<uint32_t>(std::min<VkDeviceSize>(pixelCount - 1u, static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max()))),
    };
    std::array<VkBufferCopy, 3> copies{};
    for (uint32_t i = 0; i < static_cast<uint32_t>(copies.size()); ++i) {
        copies[i].srcOffset = wavefrontRayQueueBuffer_.baseOffset() + sizeof(WavefrontQueueHeaderGpu) + static_cast<VkDeviceSize>(sampleIndices[i]) * sizeof(WavefrontRayGpu);
        copies[i].dstOffset = static_cast<VkDeviceSize>(i) * sizeof(WavefrontRayGpu);
        copies[i].size = sizeof(WavefrontRayGpu);
    }
    vkCmdCopyBuffer(
        commandBuffer,
        wavefrontRayQueueBuffer_.handle(),
        wavefrontRaySampleReadbackBuffer_.handle(),
        static_cast<uint32_t>(copies.size()),
        copies.data());
}

void PathTracerRenderer::recordWavefrontShadePass(
    VkCommandBuffer commandBuffer,
    const Buffer* wavefrontRayQueueOverride,
    const Buffer* validationBufferOverride,
    const Buffer* validationReadbackBufferOverride,
    GpuProfiler::Query startQuery,
    GpuProfiler::Query endQuery,
    const char* label,
    uint32_t flags,
    const Buffer* indirectDispatchBufferOverride) {
    const Buffer& rayQueue = wavefrontRayQueueOverride != nullptr ? *wavefrontRayQueueOverride : wavefrontRayQueueBuffer_;
    const Buffer& validationBuffer = validationBufferOverride != nullptr ? *validationBufferOverride : wavefrontShadeValidationBuffer_;
    const Buffer& validationReadbackBuffer = validationReadbackBufferOverride != nullptr ? *validationReadbackBufferOverride : wavefrontShadeValidationReadbackBuffer_;
    const bool sortedInput = (flags & kWavefrontShadeFlagSortedInput) != 0u;
    const bool writeRestirGiCandidate = (flags & kWavefrontShadeFlagRestirGiCandidateWrite) != 0u;
    const Buffer& restirGiCandidateBuffer = settings_.wavefrontFinalOutputEnabled
        ? restirGiReservoirBuffer_
        : wavefrontRestirGiReservoirBuffer_;
    if (wavefrontShadePipeline_ == nullptr || wavefrontShadeSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (rayQueue.handle() == VK_NULL_HANDLE ||
        wavefrontHitQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontShadowQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE ||
        validationBuffer.handle() == VK_NULL_HANDLE ||
        validationReadbackBuffer.handle() == VK_NULL_HANDLE ||
        (writeRestirGiCandidate && restirGiCandidateBuffer.handle() == VK_NULL_HANDLE) ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return;
    }

    validationLog_.recordPass(
        std::string(label != nullptr ? label : (sortedInput ? "wavefront secondary shade rays=" : "wavefront shade pixels=")) +
        std::to_string(static_cast<VkDeviceSize>(renderExtent_.width) * renderExtent_.height));

    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontShadeSetLayout_);
    DescriptorSet atmosphereSet = currentFrame_->descriptors().allocate(atmosphereSetLayout_);
    writeRayTracingDescriptors(set, true, &rayQueue);
    if (wavefrontRestirReservoirBuffer_.handle() != VK_NULL_HANDLE) {
        DescriptorWriter()
            .writeBuffer(38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontRestirReservoirBuffer_.descriptorInfo())
            .update(context_.device(), set);
    }
    if (writeRestirGiCandidate) {
        DescriptorWriter()
            .writeBuffer(43, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiCandidateBuffer.descriptorInfo())
            .update(context_.device(), set);
    }
    DescriptorWriter()
        .writeBuffer(51, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontShadowQueueBuffer_.descriptorInfo())
        .writeBuffer(52, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .writeBuffer(53, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, validationBuffer.descriptorInfo())
        .update(context_.device(), set);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->transmittanceLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->skyViewLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = atmosphereLutSystem_->sampler()})
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->aerialPerspectiveLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->multiScatterLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfRows().descriptorInfo() : scene_.envRows().descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfCols().descriptorInfo() : scene_.envCols().descriptorInfo())
        .update(context_.device(), atmosphereSet);

    const WavefrontShadePush push{
        .width = renderExtent_.width,
        .height = renderExtent_.height,
        .frameIndex = temporalFrameIndex_,
        .maxDepth = wavefrontMaxPathDepth(),
        .flags = flags,
    };

    wavefrontShadePipeline_->bind(commandBuffer);
    const std::array<VkDescriptorSet, 2> descriptorSets{set.handle(), atmosphereSet.handle()};
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontShadePipeline_->layout(),
        0,
        static_cast<uint32_t>(descriptorSets.size()),
        descriptorSets.data(),
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        wavefrontShadePipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    const bool profile = startQuery != GpuProfiler::Count && endQuery != GpuProfiler::Count;
    if (profile) {
        currentProfiler_->write(
            commandBuffer,
            startQuery,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    const VkDeviceSize pixelCount = static_cast<VkDeviceSize>(renderExtent_.width) * renderExtent_.height;
    if (sortedInput && indirectDispatchBufferOverride != nullptr && indirectDispatchBufferOverride->handle() != VK_NULL_HANDLE) {
        vkCmdDispatchIndirect(
            commandBuffer,
            indirectDispatchBufferOverride->handle(),
            indirectDispatchBufferOverride->baseOffset());
    } else {
        const uint32_t dispatchCount = static_cast<uint32_t>(std::min<VkDeviceSize>(
            std::max<VkDeviceSize>(pixelCount, 1u),
            std::numeric_limits<uint32_t>::max()));
        wavefrontShadePipeline_->dispatch(commandBuffer, dispatchCount, 1u, 256u, 1u);
    }
    if (profile) {
        currentProfiler_->write(
            commandBuffer,
            endQuery,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        validationBuffer.handle(),
        validationBuffer.size());
    VkBufferCopy copy{};
    copy.size = sizeof(WavefrontShadeValidationGpu);
    vkCmdCopyBuffer(commandBuffer, validationBuffer.handle(), validationReadbackBuffer.handle(), 1, &copy);
}

void PathTracerRenderer::recordWavefrontDebugWritePass(VkCommandBuffer commandBuffer, bool finalOutput) {
    if (wavefrontDebugWritePipeline_ == nullptr || wavefrontDebugWriteSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    const bool needsRestirReservoir = settings_.debugView == RendererDebugView::WavefrontRestirDi;
    const bool needsRestirGiReservoir = settings_.debugView == RendererDebugView::WavefrontRestirGi;
    if (rawImage_.handle() == VK_NULL_HANDLE ||
        wavefrontRayQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontHitQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontShadowQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE ||
        (needsRestirReservoir && wavefrontRestirReservoirBuffer_.handle() == VK_NULL_HANDLE) ||
        (needsRestirGiReservoir && wavefrontRestirGiReservoirBuffer_.handle() == VK_NULL_HANDLE) ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return;
    }

    validationLog_.recordPass(finalOutput ? "wavefront final write" : "wavefront debug write");
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    const Buffer& compactedQueue = wavefrontCompactedRayQueueBuffer_.handle() != VK_NULL_HANDLE
        ? wavefrontCompactedRayQueueBuffer_
        : wavefrontRayQueueBuffer_;
    const Buffer& restirOrDummyBuffer = wavefrontRestirReservoirBuffer_.handle() != VK_NULL_HANDLE
        ? wavefrontRestirReservoirBuffer_
        : (restirReservoirBuffer_.handle() != VK_NULL_HANDLE ? restirReservoirBuffer_ : wavefrontPixelStateBuffer_);
    const Buffer& restirGiOrDummyBuffer = wavefrontRestirGiReservoirBuffer_.handle() != VK_NULL_HANDLE
        ? wavefrontRestirGiReservoirBuffer_
        : (restirGiReservoirBuffer_.handle() != VK_NULL_HANDLE ? restirGiReservoirBuffer_ : wavefrontPixelStateBuffer_);
    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontDebugWriteSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontRayQueueBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, compactedQueue.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontHitQueueBuffer_.descriptorInfo())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontShadowQueueBuffer_.descriptorInfo())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirOrDummyBuffer.descriptorInfo())
        .writeBuffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiOrDummyBuffer.descriptorInfo())
        .update(context_.device(), set);

    const WavefrontDebugWritePush push{
        .width = renderExtent_.width,
        .height = renderExtent_.height,
        .view = finalOutput ? 0u : static_cast<uint32_t>(settings_.debugView),
        .maxDepth = wavefrontMaxPathDepth(),
        .flags = finalOutput ? kWavefrontDebugWriteFlagFinalOutput : 0u,
    };

    wavefrontDebugWritePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontDebugWritePipeline_->layout(),
        0,
        1,
        &descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        wavefrontDebugWritePipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    wavefrontDebugWritePipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height, 8u, 8u);
}

void PathTracerRenderer::recordWavefrontCompactPass(VkCommandBuffer commandBuffer, bool profile) {
    if (wavefrontCompactPipeline_ == nullptr || wavefrontCompactSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (wavefrontRayQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontCompactedRayQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontCompactValidationBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontCompactValidationReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontSortDispatchBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontRayQueueCapacity_ == 0u ||
        wavefrontCompactedRayQueueCapacity_ == 0u) {
        return;
    }

    validationLog_.recordPass(
        "wavefront compact capacity=" + std::to_string(wavefrontCompactedRayQueueCapacity_) +
        " source_capacity=" + std::to_string(wavefrontRayQueueCapacity_));

    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontCompactSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontRayQueueBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontCompactedRayQueueBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontCompactValidationBuffer_.descriptorInfo())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontSortDispatchBuffer_.descriptorInfo())
        .update(context_.device(), set);

    wavefrontCompactPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontCompactPipeline_->layout(),
        0,
        1,
        &descriptorSet,
        0,
        nullptr);

    WavefrontCompactPush push{
        .sourceCapacity = wavefrontRayQueueCapacity_,
        .compactCapacity = wavefrontCompactedRayQueueCapacity_,
        .pixelCapacity = wavefrontPixelStateCapacity_,
        .mode = 0u,
        .maxPathDepth = wavefrontMaxPathDepth(),
        .frameIndex = temporalFrameIndex_,
        .flags = 0u,
    };

    if (profile) {
        currentProfiler_->write(commandBuffer, GpuProfiler::WavefrontCompactStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    vkCmdPushConstants(
        commandBuffer,
        wavefrontCompactPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdDispatch(commandBuffer, 1, 1, 1);

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        wavefrontCompactedRayQueueBuffer_.handle(),
        wavefrontCompactedRayQueueBuffer_.size(),
        wavefrontCompactedRayQueueBuffer_.baseOffset());
    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        wavefrontCompactValidationBuffer_.handle(),
        wavefrontCompactValidationBuffer_.size());
    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        wavefrontSortDispatchBuffer_.handle(),
        wavefrontSortDispatchBuffer_.size(),
        wavefrontSortDispatchBuffer_.baseOffset());

    push.mode = 1u;
    vkCmdPushConstants(
        commandBuffer,
        wavefrontCompactPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdDispatchIndirect(
        commandBuffer,
        wavefrontSortDispatchBuffer_.handle(),
        wavefrontSortDispatchBuffer_.baseOffset());

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        wavefrontCompactedRayQueueBuffer_.handle(),
        wavefrontCompactedRayQueueBuffer_.size(),
        wavefrontCompactedRayQueueBuffer_.baseOffset());
    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        wavefrontSortDispatchBuffer_.handle(),
        wavefrontSortDispatchBuffer_.size(),
        wavefrontSortDispatchBuffer_.baseOffset());
    push.mode = 2u;
    vkCmdPushConstants(
        commandBuffer,
        wavefrontCompactPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdDispatch(commandBuffer, 1u, 1u, 1u);
    if (profile) {
        currentProfiler_->write(commandBuffer, GpuProfiler::WavefrontCompactEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        wavefrontSortDispatchBuffer_.handle(),
        wavefrontSortDispatchBuffer_.size(),
        wavefrontSortDispatchBuffer_.baseOffset());
    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        wavefrontCompactValidationBuffer_.handle(),
        wavefrontCompactValidationBuffer_.size());
    VkBufferCopy copy{};
    copy.size = sizeof(WavefrontCompactValidationGpu);
    vkCmdCopyBuffer(commandBuffer, wavefrontCompactValidationBuffer_.handle(), wavefrontCompactValidationReadbackBuffer_.handle(), 1, &copy);
}

void PathTracerRenderer::recordWavefrontSortPass(VkCommandBuffer commandBuffer) {
    if (wavefrontSortPipeline_ == nullptr || wavefrontSortSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (wavefrontCompactedRayQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontSortedRayQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontSortValidationBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontSortValidationReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontSortDispatchBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontCompactedRayQueueCapacity_ == 0u ||
        wavefrontSortedRayQueueCapacity_ == 0u) {
        return;
    }

    validationLog_.recordPass(
        "wavefront sort capacity=" + std::to_string(wavefrontSortedRayQueueCapacity_) +
        " source_capacity=" + std::to_string(wavefrontCompactedRayQueueCapacity_));

    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontSortSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontCompactedRayQueueBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontSortedRayQueueBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontSortValidationBuffer_.descriptorInfo())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontSortDispatchBuffer_.descriptorInfo())
        .update(context_.device(), set);

    wavefrontSortPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontSortPipeline_->layout(),
        0,
        1,
        &descriptorSet,
        0,
        nullptr);

    WavefrontSortPush push{
        .sourceCapacity = wavefrontCompactedRayQueueCapacity_,
        .sortCapacity = wavefrontSortedRayQueueCapacity_,
        .pixelCapacity = wavefrontPixelStateCapacity_,
        .mode = 0u,
        .bucketCount = kWavefrontSortBucketCount,
        .frameIndex = temporalFrameIndex_,
        .flags = 0u,
    };
    auto dispatchMode = [&](uint32_t mode, uint32_t groupCount) {
        push.mode = mode;
        vkCmdPushConstants(
            commandBuffer,
            wavefrontSortPipeline_->layout(),
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(push),
            &push);
        vkCmdDispatch(commandBuffer, groupCount, 1, 1);
    };
    auto dispatchIndirectMode = [&](uint32_t mode) {
        push.mode = mode;
        vkCmdPushConstants(
            commandBuffer,
            wavefrontSortPipeline_->layout(),
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(push),
            &push);
        vkCmdDispatchIndirect(commandBuffer, wavefrontSortDispatchBuffer_.handle(), wavefrontSortDispatchBuffer_.baseOffset());
    };
    auto computeBarrier = [&](const Buffer& buffer, VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess) {
        bufferMemoryBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            srcAccess,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            dstAccess,
            buffer.handle(),
            buffer.size(),
            buffer.baseOffset());
    };

    currentProfiler_->write(commandBuffer, GpuProfiler::WavefrontSortStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    dispatchMode(0u, 1u);
    computeBarrier(wavefrontSortValidationBuffer_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    computeBarrier(wavefrontSortedRayQueueBuffer_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        wavefrontSortDispatchBuffer_.handle(),
        wavefrontSortDispatchBuffer_.size(),
        wavefrontSortDispatchBuffer_.baseOffset());

    dispatchIndirectMode(1u);
    computeBarrier(wavefrontSortValidationBuffer_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    dispatchMode(2u, 1u);
    computeBarrier(wavefrontSortValidationBuffer_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    computeBarrier(wavefrontSortedRayQueueBuffer_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    dispatchIndirectMode(3u);
    computeBarrier(wavefrontSortValidationBuffer_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    computeBarrier(wavefrontSortedRayQueueBuffer_, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    currentProfiler_->write(commandBuffer, GpuProfiler::WavefrontSortEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    dispatchIndirectMode(4u);

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        wavefrontSortValidationBuffer_.handle(),
        wavefrontSortValidationBuffer_.size());
    VkBufferCopy copy{};
    copy.size = sizeof(WavefrontSortValidationGpu);
    vkCmdCopyBuffer(commandBuffer, wavefrontSortValidationBuffer_.handle(), wavefrontSortValidationReadbackBuffer_.handle(), 1, &copy);
}

void PathTracerRenderer::recordWavefrontHitQueueCountClearPass(VkCommandBuffer commandBuffer) {
    if (wavefrontHitQueueBuffer_.handle() == VK_NULL_HANDLE) {
        return;
    }

    validationLog_.recordPass("wavefront hit queue count clear");
    vkCmdFillBuffer(
        commandBuffer,
        wavefrontHitQueueBuffer_.handle(),
        wavefrontHitQueueBuffer_.baseOffset(),
        sizeof(uint32_t),
        0u);
}

void PathTracerRenderer::recordWavefrontShadowTracePass(VkCommandBuffer commandBuffer, bool profile) {
    RayTracingPipeline* tracePipeline = settings_.shaderExecutionReorderingEnabled && wavefrontTraceSerPipeline_ != nullptr
        ? wavefrontTraceSerPipeline_.get()
        : wavefrontTracePipeline_.get();
    if (tracePipeline == nullptr || rayTracingScene_ == nullptr || rayTracingSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (wavefrontShadowQueueBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontShadowTraceValidationBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontShadowTraceValidationReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return;
    }

    validationLog_.recordPass("wavefront shadow trace rt");

    DescriptorSet set = currentFrame_->descriptors().allocate(rayTracingSetLayout_);
    DescriptorSet atmosphereSet = currentFrame_->descriptors().allocate(atmosphereSetLayout_);
    writeRayTracingDescriptors(set, true);
    DescriptorWriter()
        .writeBuffer(51, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontShadowQueueBuffer_.descriptorInfo())
        .writeBuffer(52, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .writeBuffer(54, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontShadowTraceValidationBuffer_.descriptorInfo())
        .update(context_.device(), set);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->transmittanceLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->skyViewLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = atmosphereLutSystem_->sampler()})
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->aerialPerspectiveLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->multiScatterLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfRows().descriptorInfo() : scene_.envRows().descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfCols().descriptorInfo() : scene_.envCols().descriptorInfo())
        .update(context_.device(), atmosphereSet);

    tracePipeline->bind(commandBuffer);
    const std::array<VkDescriptorSet, 2> descriptorSets{set.handle(), atmosphereSet.handle()};
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        tracePipeline->layout(),
        0,
        static_cast<uint32_t>(descriptorSets.size()),
        descriptorSets.data(),
        0,
        nullptr);
    if (profile) {
        currentProfiler_->write(commandBuffer, GpuProfiler::WavefrontShadowTraceStart, pathTraceShaderStage());
    }
    tracePipeline->traceRays(commandBuffer, renderExtent_.width, renderExtent_.height, 1u);
    if (profile) {
        currentProfiler_->write(commandBuffer, GpuProfiler::WavefrontShadowTraceEnd, pathTraceShaderStage());
    }

    bufferMemoryBarrier(
        commandBuffer,
        pathTraceShaderStage(),
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        wavefrontShadowTraceValidationBuffer_.handle(),
        wavefrontShadowTraceValidationBuffer_.size());
    VkBufferCopy copy{};
    copy.size = sizeof(WavefrontShadowTraceValidationGpu);
    vkCmdCopyBuffer(commandBuffer, wavefrontShadowTraceValidationBuffer_.handle(), wavefrontShadowTraceValidationReadbackBuffer_.handle(), 1, &copy);
}

void PathTracerRenderer::recordWavefrontDirectLightingValidationPass(VkCommandBuffer commandBuffer) {
    if (wavefrontDirectLightingValidatePipeline_ == nullptr || wavefrontDirectLightingValidateSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (wavefrontPixelStateBuffer_.handle() == VK_NULL_HANDLE ||
        pathDataBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontShadowTraceValidationBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontShadowTraceValidationReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return;
    }

    validationLog_.recordPass("wavefront direct lighting validate");

    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontDirectLightingValidateSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontPixelStateBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pathDataBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontShadowTraceValidationBuffer_.descriptorInfo())
        .update(context_.device(), set);

    const WavefrontDirectLightingValidationPush push{
        .width = renderExtent_.width,
        .height = renderExtent_.height,
        .absoluteEpsilon = 1.0e-3f,
        .relativeEpsilon = 0.05f,
    };

    wavefrontDirectLightingValidatePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontDirectLightingValidatePipeline_->layout(),
        0,
        1,
        &descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        wavefrontDirectLightingValidatePipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    wavefrontDirectLightingValidatePipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height, 8u, 8u);

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        wavefrontShadowTraceValidationBuffer_.handle(),
        wavefrontShadowTraceValidationBuffer_.size());
    VkBufferCopy copy{};
    copy.size = sizeof(WavefrontShadowTraceValidationGpu);
    vkCmdCopyBuffer(commandBuffer, wavefrontShadowTraceValidationBuffer_.handle(), wavefrontShadowTraceValidationReadbackBuffer_.handle(), 1, &copy);
}

void PathTracerRenderer::recordRestirSpatial(VkCommandBuffer commandBuffer) {
    if (!shouldRunRestirSpatial()) {
        if (adaptiveSkipRestirSpatial_) {
            validationLog_.recordPass("adaptive skip restir spatial");
        }
        return;
    }

    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    auto bufferResource = [](const Buffer& buffer, const char* name, std::optional<ResourceAccess> initial = std::nullopt) {
        RenderGraphResource resource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .bufferOffset = buffer.baseOffset(),
            .external = true,
            .debugName = name,
        };
        if (initial.has_value()) {
            resource.hasInitialAccess = true;
            resource.initialAccess = *initial;
        }
        return resource;
    };

    const ResourceAccess pathWrite{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    const bool restirSpatialAliasesGiCurrent = restirSpatialReservoirBuffer_.handle() != VK_NULL_HANDLE &&
        restirSpatialReservoirBuffer_.handle() == restirGiReservoirBuffer_.handle();
    const std::optional<ResourceAccess> aliasInitialAccess = restirSpatialAliasesGiCurrent
        ? std::optional<ResourceAccess>(ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        })
        : std::nullopt;
    const RenderGraphResourceId restirReservoir = graph.createBuffer(bufferResource(restirReservoirBuffer_, "restir reservoir", pathWrite));
    const RenderGraphResourceId restirSpatialReservoir = graph.createBuffer(bufferResource(restirSpatialReservoirBuffer_, "restir spatial reservoir", aliasInitialAccess));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal", pathWrite));

    graph.addPass("restir_spatial")
        .addStorageRead(restirReservoir, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .addStorageWrite(restirSpatialReservoir, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordRestirSpatialPass(cmd);
        });
    graph.addPass("restir_spatial_copy")
        .addStorageRead(restirSpatialReservoir, PipelineDomain::Transfer)
        .addStorageWrite(restirReservoir, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordRestirSpatialCopyPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordRestirSpatialPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("restir spatial reuse");
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirSpatialStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    DescriptorSet set = currentFrame_->descriptors().allocate(restirSpatialSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirReservoirBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirSpatialReservoirBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameRestirSpatialParamsOffset, sizeof(RestirSpatialParams)))
        .update(context_.device(), set);

    restirSpatialPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, restirSpatialPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    restirSpatialPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirSpatialEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordRestirSpatialCopyPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("restir spatial copy");
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirSpatialCopyStart, VK_PIPELINE_STAGE_2_COPY_BIT);
    VkBufferCopy copy{};
    copy.size = restirReservoirBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, restirSpatialReservoirBuffer_.handle(), restirReservoirBuffer_.handle(), 1, &copy);
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirSpatialCopyEnd, VK_PIPELINE_STAGE_2_COPY_BIT);
}

void PathTracerRenderer::recordRestirGiSpatialPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("restir gi spatial reuse");
    if (restirGiSpatialPipeline_ == nullptr || restirGiSpatialSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirGiSpatialStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    DescriptorSet set = currentFrame_->descriptors().allocate(restirGiSpatialSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiReservoirBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiSpatialReservoirBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameRestirSpatialParamsOffset, sizeof(RestirSpatialParams)))
        .update(context_.device(), set);

    restirGiSpatialPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, restirGiSpatialPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    restirGiSpatialPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirGiSpatialEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordRestirGiFinalPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("restir gi final shading");
    if (restirGiFinalPipeline_ == nullptr || restirGiFinalSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirGiFinalStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(restirGiFinalSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiReservoirBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousRestirGiReservoirBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiSpatialReservoirBuffer_.descriptorInfo())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameDebugParamsOffset, sizeof(RendererDebugParams)))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameRestirSpatialParamsOffset, sizeof(RestirSpatialParams)))
        .update(context_.device(), set);

    restirGiFinalPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, restirGiFinalPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    restirGiFinalPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::RestirGiFinalEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordHeightFog(VkCommandBuffer commandBuffer) {
    if (settings_.wavefrontFinalOutputEnabled) {
        return;
    }
    if (fogPipeline_ == nullptr || fogSetLayout_ == VK_NULL_HANDLE || rawImage_.handle() == VK_NULL_HANDLE || depthNormalBuffer_.handle() == VK_NULL_HANDLE) {
        return;
    }

    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    auto imageResource = [](const Image& image, const char* name, ResourceAccess initial) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .hasInitialAccess = true,
            .initialAccess = initial,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name, ResourceAccess initial) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .bufferOffset = buffer.baseOffset(),
            .external = true,
            .hasInitialAccess = true,
            .initialAccess = initial,
            .debugName = name,
        };
    };

    const ResourceAccess pathWrite{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr", pathWrite));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal", pathWrite));
    graph.addPass("fog_integrate")
        .addStorageReadWrite(raw, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordHeightFogPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordHeightFogPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("height fog integrate");
    currentProfiler_->write(commandBuffer, GpuProfiler::FogIntegrateStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(fogSetLayout_);
    DescriptorSet atmosphereSet = currentFrame_->descriptors().allocate(atmosphereSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameCameraUniformOffset, sizeof(CameraUniform)))
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameFogParamsOffset, sizeof(FogParams)))
        .update(context_.device(), set);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->transmittanceLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->skyViewLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = atmosphereLutSystem_->sampler()})
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->aerialPerspectiveLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->multiScatterLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfRows().descriptorInfo() : scene_.envRows().descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfCols().descriptorInfo() : scene_.envCols().descriptorInfo())
        .update(context_.device(), atmosphereSet);

    fogPipeline_->bind(commandBuffer);
    const std::array<VkDescriptorSet, 2> descriptorSets{set.handle(), atmosphereSet.handle()};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, fogPipeline_->layout(), 0, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 0, nullptr);
    fogPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::FogIntegrateEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordRenderGraphPlan() {
    if (rawImage_.handle() == VK_NULL_HANDLE || presentationImage_.handle() == VK_NULL_HANDLE) {
        return;
    }

    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    auto imageResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .bufferOffset = buffer.baseOffset(),
            .external = true,
            .debugName = name,
        };
    };

    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr"));
    const RenderGraphResourceId denoised = graph.createTexture(imageResource(denoisedImage_, "denoised hdr"));
    const RenderGraphResourceId history = graph.createTexture(imageResource(historyImage_, "history hdr"));
    const RenderGraphResourceId diffuseResolved = graph.createTexture(imageResource(diffuseResolvedImage_, "current diffuse history hdr"));
    const RenderGraphResourceId specularResolved = graph.createTexture(imageResource(specularResolvedImage_, "current specular history hdr"));
    const RenderGraphResourceId diffuseHistory = graph.createTexture(imageResource(diffuseHistoryImage_, "diffuse history hdr"));
    const RenderGraphResourceId specularHistory = graph.createTexture(imageResource(specularHistoryImage_, "specular history hdr"));
    const RenderGraphResourceId taa = graph.createTexture(imageResource(taaImage_, "taa hdr"));
    const RenderGraphResourceId taaHistory = graph.createTexture(imageResource(taaHistoryImage_, "taa history hdr"));
    const RenderGraphResourceId presentation = graph.createTexture(imageResource(presentationImage_, "presentation ldr"));
    const RenderGraphResourceId entityIds = graph.createBuffer(bufferResource(entityIdBuffer_, "entity ids"));
    const RenderGraphResourceId accumulation = graph.createBuffer(bufferResource(accumulationBuffer_, "accumulation"));
    const RenderGraphResourceId variance = graph.createBuffer(bufferResource(varianceBuffer_, "variance"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal"));
    const RenderGraphResourceId worldPosition = graph.createBuffer(bufferResource(worldPositionBuffer_, "world position"));
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(bufferResource(previousWorldPositionBuffer_, "previous world position"));
    const RenderGraphResourceId velocity = graph.createBuffer(bufferResource(velocityBuffer_, "screen velocity"));
    const RenderGraphResourceId pathData = graph.createBuffer(bufferResource(pathDataBuffer_, "path data"));
    const RenderGraphResourceId rayTracingDiagnosticCounters = graph.createBuffer(bufferResource(rayTracingDiagnosticCountersBuffer_, "ray tracing diagnostic counters"));
    const RenderGraphResourceId directDiffuseMoments = graph.createTexture(imageResource(directDiffuseMomentsImage_, "direct diffuse moments"));
    const RenderGraphResourceId directSpecularMoments = graph.createTexture(imageResource(directSpecularMomentsImage_, "direct specular moments"));
    const RenderGraphResourceId indirectDiffuseMoments = graph.createTexture(imageResource(indirectDiffuseMomentsImage_, "indirect diffuse moments"));
    const RenderGraphResourceId indirectSpecularMoments = graph.createTexture(imageResource(indirectSpecularMomentsImage_, "indirect specular moments"));
    const RenderGraphResourceId historyLength = graph.createTexture(imageResource(historyLengthImage_, "history length"));
    const RenderGraphResourceId momentDebug = graph.createTexture(imageResource(momentDebugImage_, "moment debug"));
    const RenderGraphResourceId directDiffuseResolvedMoments = graph.createTexture(imageResource(directDiffuseResolvedMomentsImage_, "current direct diffuse moments"));
    const RenderGraphResourceId directSpecularResolvedMoments = graph.createTexture(imageResource(directSpecularResolvedMomentsImage_, "current direct specular moments"));
    const RenderGraphResourceId indirectDiffuseResolvedMoments = graph.createTexture(imageResource(indirectDiffuseResolvedMomentsImage_, "current indirect diffuse moments"));
    const RenderGraphResourceId indirectSpecularResolvedMoments = graph.createTexture(imageResource(indirectSpecularResolvedMomentsImage_, "current indirect specular moments"));
    const RenderGraphResourceId historyLengthResolved = graph.createTexture(imageResource(historyLengthResolvedImage_, "current history length"));
    const RenderGraphResourceId momentDebugResolved = graph.createTexture(imageResource(momentDebugResolvedImage_, "current moment debug"));
    const RenderGraphResourceId restirReservoir = graph.createBuffer(bufferResource(restirReservoirBuffer_, "restir reservoir"));
    RenderGraphResourceId wavefrontRestirReservoir{};
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(bufferResource(previousRestirReservoirBuffer_, "previous restir reservoir"));
    const RenderGraphResourceId restirSpatialReservoir = graph.createBuffer(bufferResource(restirSpatialReservoirBuffer_, "restir spatial reservoir"));
    const bool resetRestirHistory =
        settings_.restirMode != RestirMode::ClassicNee &&
        ((temporalSystem_ != nullptr && temporalSystem_->isCameraCut()) ||
         (temporalSystem_ == nullptr && temporalFrameIndex_ <= 1u));
    const bool useRestirGiReservoirs = shouldUseRestirGiReservoirs();
    const bool useWavefrontQueues = settings_.wavefrontQueuesEnabled &&
        wavefrontRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontHitQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontShadowQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontPixelStateBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontPrimaryGenerate = useWavefrontQueues && settings_.wavefrontPrimaryGenerateEnabled;
    const bool useWavefrontTrace = useWavefrontPrimaryGenerate && settings_.wavefrontTraceEnabled &&
        wavefrontTraceValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontShade = useWavefrontTrace && settings_.wavefrontShadeEnabled &&
        wavefrontShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontCompact = useWavefrontShade && settings_.wavefrontCompactEnabled &&
        wavefrontCompactedRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontCompactValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontShadowTrace = useWavefrontShade && settings_.wavefrontShadowTraceEnabled &&
        wavefrontShadowTraceValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontFinalOutput = useWavefrontCompact && useWavefrontShadowTrace && shouldUseWavefrontFinalOutput();
    const bool useWavefrontSort = useWavefrontCompact && !useWavefrontFinalOutput && settings_.wavefrontSortEnabled &&
        wavefrontSortedRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortValidationBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortDispatchBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontSortedShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontSecondaryShade = useWavefrontCompact && !useWavefrontFinalOutput && !useWavefrontSort &&
        wavefrontSecondaryShadeValidationBuffer_.handle() != VK_NULL_HANDLE;
    const bool useWavefrontDebugWrite = !useWavefrontFinalOutput && useWavefrontShade && shouldRunWavefrontDebugWrite();
    RenderGraphResourceId wavefrontRayQueue{};
    RenderGraphResourceId wavefrontCompactedRayQueue{};
    RenderGraphResourceId wavefrontSortedRayQueue{};
    RenderGraphResourceId wavefrontSortDispatch{};
    RenderGraphResourceId wavefrontHitQueue{};
    RenderGraphResourceId wavefrontShadowQueue{};
    RenderGraphResourceId wavefrontPixelState{};
    if (useWavefrontQueues) {
        wavefrontRayQueue = graph.createBuffer(bufferResource(wavefrontRayQueueBuffer_, "wavefront ray queue"));
        if (useWavefrontCompact) {
            wavefrontCompactedRayQueue = graph.createBuffer(bufferResource(wavefrontCompactedRayQueueBuffer_, "wavefront compacted ray queue"));
        }
        if (useWavefrontCompact) {
            wavefrontSortDispatch = graph.createBuffer(bufferResource(wavefrontSortDispatchBuffer_, "wavefront queue indirect dispatch args"));
        }
        if (useWavefrontSort) {
            wavefrontSortedRayQueue = graph.createBuffer(bufferResource(wavefrontSortedRayQueueBuffer_, "wavefront sorted ray queue"));
        }
        wavefrontHitQueue = graph.createBuffer(bufferResource(wavefrontHitQueueBuffer_, "wavefront hit queue"));
        wavefrontShadowQueue = graph.createBuffer(bufferResource(wavefrontShadowQueueBuffer_, "wavefront shadow ray queue"));
        wavefrontPixelState = graph.createBuffer(bufferResource(wavefrontPixelStateBuffer_, "wavefront pixel state"));
    }
    RenderGraphResourceId restirGiReservoir{};
    RenderGraphResourceId previousRestirGiReservoir{};
    RenderGraphResourceId restirGiSpatialReservoir{};
    RenderGraphResourceId wavefrontRestirGiReservoir{};
    if (useRestirGiReservoirs) {
        restirGiReservoir = graph.createBuffer(bufferResource(restirGiReservoirBuffer_, "restir gi reservoir"));
        previousRestirGiReservoir = graph.createBuffer(bufferResource(previousRestirGiReservoirBuffer_, "previous restir gi reservoir"));
        restirGiSpatialReservoir = graph.createBuffer(bufferResource(restirGiSpatialReservoirBuffer_, "restir gi spatial reservoir"));
    }
    if (useWavefrontQueues && wavefrontRestirGiReservoirBuffer_.handle() != VK_NULL_HANDLE) {
        wavefrontRestirGiReservoir = graph.createBuffer(bufferResource(wavefrontRestirGiReservoirBuffer_, "wavefront restir gi candidate reservoir"));
    }
    if (useWavefrontQueues && wavefrontRestirReservoirBuffer_.handle() != VK_NULL_HANDLE) {
        wavefrontRestirReservoir = graph.createBuffer(bufferResource(wavefrontRestirReservoirBuffer_, "wavefront restir di candidate reservoir"));
    }

    const PipelineDomain traceDomain = PipelineDomain::RayTracing;
    if (resetRestirHistory) {
        graph.addPass("restir_history_clear")
            .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer);
    }
    if (useRestirGiReservoirs) {
        RenderGraphPass& restirGiClearPass = graph.addPass("restir_gi_clear")
            .addStorageWrite(restirGiReservoir, PipelineDomain::Transfer);
        if (!restirGiHistoryValid_) {
            restirGiClearPass
                .addStorageWrite(previousRestirGiReservoir, PipelineDomain::Transfer)
                .addStorageWrite(restirGiSpatialReservoir, PipelineDomain::Transfer);
        }
    }
    if (useWavefrontQueues) {
        graph.addPass("wavefront_queue_clear")
            .addStorageWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageWrite(wavefrontPixelState, PipelineDomain::Compute);
    }
    if (useWavefrontPrimaryGenerate) {
        graph.addPass("wavefront_primary_generate")
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute);
    }
    if (useWavefrontTrace) {
        graph.addPass("wavefront_trace_rt")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing);
    }
    if (useWavefrontShade) {
        const RenderGraphResourceId wavefrontShadeValidation = graph.createBuffer(bufferResource(wavefrontShadeValidationBuffer_, "wavefront shade validation"));
        graph.addPass("wavefront_shade_validation_clear")
            .addStorageWrite(wavefrontShadeValidation, PipelineDomain::Transfer);
        graph.addPass("wavefront_shade")
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(depthNormal, PipelineDomain::Compute)
            .addStorageWrite(worldPosition, PipelineDomain::Compute)
            .addStorageWrite(entityIds, PipelineDomain::Compute)
            .addStorageWrite(variance, PipelineDomain::Compute)
            .addStorageWrite(velocity, PipelineDomain::Compute)
            .addStorageWrite(pathData, PipelineDomain::Compute)
            .addStorageWrite(wavefrontShadeValidation, PipelineDomain::Compute)
            .addStorageRead(previousRestirReservoir, PipelineDomain::Compute)
            .addStorageWrite(wavefrontRestirReservoir.valid() ? wavefrontRestirReservoir : restirReservoir, PipelineDomain::Compute);
    }
    if (useWavefrontCompact && !useWavefrontFinalOutput) {
        const RenderGraphResourceId wavefrontCompactValidation = graph.createBuffer(bufferResource(wavefrontCompactValidationBuffer_, "wavefront compact validation"));
        graph.addPass("wavefront_compact")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontSortDispatch, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontCompactValidation, PipelineDomain::Compute);
    }
    if (useWavefrontSort) {
        const RenderGraphResourceId wavefrontSortValidation = graph.createBuffer(bufferResource(wavefrontSortValidationBuffer_, "wavefront sort validation"));
        graph.addPass("wavefront_sort")
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontSortedRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontSortDispatch, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontSortValidation, PipelineDomain::Compute);
    }
    RenderGraphResourceId wavefrontShadowTraceValidation{};
    if (useWavefrontShadowTrace && !useWavefrontFinalOutput) {
        wavefrontShadowTraceValidation = graph.createBuffer(bufferResource(wavefrontShadowTraceValidationBuffer_, "wavefront shadow trace validation"));
        graph.addPass("wavefront_shadow_trace_validation_clear")
            .addStorageWrite(wavefrontShadowTraceValidation, PipelineDomain::Transfer);
        graph.addPass("wavefront_shadow_trace_rt")
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::RayTracing)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontShadowTraceValidation, PipelineDomain::RayTracing);
    }
    if (!useWavefrontFinalOutput) {
        RenderGraphPass& pathTracePass = graph.addPass("path_trace_rt")
            .addStorageWrite(raw, traceDomain)
            .addStorageWrite(entityIds, traceDomain)
            .addStorageReadWrite(accumulation, traceDomain)
            .addStorageWrite(variance, traceDomain)
            .addStorageWrite(depthNormal, traceDomain)
            .addStorageWrite(worldPosition, traceDomain)
            .addStorageWrite(velocity, traceDomain)
            .addStorageWrite(pathData, traceDomain)
            .addStorageWrite(rayTracingDiagnosticCounters, traceDomain)
            .addStorageRead(previousRestirReservoir, traceDomain)
            .addStorageWrite(restirReservoir, traceDomain);
        if (useRestirGiReservoirs) {
            pathTracePass
                .addStorageReadWrite(restirGiReservoir, traceDomain)
                .addStorageRead(previousRestirGiReservoir, traceDomain)
                .addStorageRead(restirGiSpatialReservoir, traceDomain);
        }
    }
    if (useWavefrontShadowTrace && !useWavefrontFinalOutput) {
        graph.addPass("wavefront_direct_lighting_validate")
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageRead(pathData, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowTraceValidation, PipelineDomain::Compute);
    }
    RenderGraphResourceId wavefrontTraceValidation{};
    if (useWavefrontTrace && !useWavefrontFinalOutput) {
        wavefrontTraceValidation = graph.createBuffer(bufferResource(wavefrontTraceValidationBuffer_, "wavefront trace validation"));
        graph.addPass("wavefront_trace_validation_clear")
            .addStorageWrite(wavefrontTraceValidation, PipelineDomain::Transfer);
        graph.addPass("wavefront_trace_validate")
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageRead(entityIds, PipelineDomain::Compute)
            .addStorageWrite(wavefrontTraceValidation, PipelineDomain::Compute);
    }
    if (useRestirGiReservoirs && !useWavefrontFinalOutput) {
        graph.addPass("restir_gi_spatial")
            .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageWrite(restirGiSpatialReservoir, PipelineDomain::Compute);
    }
    if (shouldRunRestirGiFinal()) {
        graph.addPass("restir_gi_final")
            .addStorageReadWrite(raw, PipelineDomain::Compute)
            .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
            .addStorageRead(previousRestirGiReservoir, PipelineDomain::Compute)
            .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Compute);
    }
    if (useWavefrontFinalOutput) {
        const RenderGraphResourceId wavefrontFinalCompactValidation = graph.createBuffer(bufferResource(wavefrontCompactValidationBuffer_, "wavefront final compact validation"));
        const RenderGraphResourceId wavefrontFinalShadeValidation = graph.createBuffer(bufferResource(wavefrontSecondaryShadeValidationBuffer_, "wavefront final shade validation"));
        const uint32_t maxDepth = std::max(1u, wavefrontMaxPathDepth());
        for (uint32_t bounce = 1u; bounce < maxDepth; ++bounce) {
            const std::string suffix = "_bounce_" + std::to_string(bounce);
            graph.addPass(("wavefront_final_compact" + suffix).c_str())
                .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontCompactedRayQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontSortDispatch, PipelineDomain::Compute)
                .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
                .addStorageWrite(wavefrontFinalCompactValidation, PipelineDomain::Compute);
            graph.addPass(("wavefront_final_hit_queue_clear" + suffix).c_str())
                .addStorageWrite(wavefrontHitQueue, PipelineDomain::Transfer);
            graph.addPass(("wavefront_final_secondary_trace_rt" + suffix).c_str())
                .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::RayTracing)
                .addStorageRead(wavefrontSortDispatch, PipelineDomain::RayTracing)
                .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing);
            if (bounce == 1u) {
                graph.addPass(("wavefront_final_shade_validation_clear" + suffix).c_str())
                    .addStorageWrite(wavefrontFinalShadeValidation, PipelineDomain::Transfer);
            }
            RenderGraphPass& finalSecondaryShadePass = graph.addPass(("wavefront_final_secondary_shade" + suffix).c_str())
                .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
                .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
                .addStorageRead(wavefrontSortDispatch, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
                .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
                .addStorageWrite(wavefrontFinalShadeValidation, PipelineDomain::Compute);
            if (useRestirGiReservoirs && restirGiReservoir.valid()) {
                finalSecondaryShadePass.addStorageWrite(restirGiReservoir, PipelineDomain::Compute);
            }
        }
        const RenderGraphResourceId wavefrontFinalShadowTraceValidation = graph.createBuffer(bufferResource(wavefrontShadowTraceValidationBuffer_, "wavefront final shadow trace validation"));
        graph.addPass("wavefront_final_shadow_trace_validation_clear")
            .addStorageWrite(wavefrontFinalShadowTraceValidation, PipelineDomain::Transfer);
        graph.addPass("wavefront_final_shadow_trace_rt")
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::RayTracing)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontFinalShadowTraceValidation, PipelineDomain::RayTracing);
        if (useRestirGiReservoirs) {
            graph.addPass("wavefront_final_restir_gi_spatial")
                .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
                .addStorageRead(depthNormal, PipelineDomain::Compute)
                .addStorageWrite(restirGiSpatialReservoir, PipelineDomain::Compute);
        }
        RenderGraphPass& finalWritePass = graph.addPass("wavefront_final_write")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(raw, PipelineDomain::Compute);
        if (wavefrontRestirReservoir.valid()) {
            finalWritePass.addStorageRead(wavefrontRestirReservoir, PipelineDomain::Compute);
        } else if (restirReservoirBuffer_.handle() != VK_NULL_HANDLE) {
            finalWritePass.addStorageRead(restirReservoir, PipelineDomain::Compute);
        }
        if (useRestirGiReservoirs) {
            finalWritePass.addStorageRead(restirGiReservoir, PipelineDomain::Compute);
            graph.addPass("wavefront_final_restir_gi_final")
                .addStorageReadWrite(raw, PipelineDomain::Compute)
                .addStorageRead(restirGiReservoir, PipelineDomain::Compute)
                .addStorageRead(previousRestirGiReservoir, PipelineDomain::Compute)
                .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Compute);
        }
    }
    if (useWavefrontSecondaryShade) {
        const RenderGraphResourceId wavefrontSecondaryShadeValidation = graph.createBuffer(bufferResource(wavefrontSecondaryShadeValidationBuffer_, "wavefront secondary shade validation"));
        RenderGraphPass& secondaryHitClear = graph.addPass("wavefront_secondary_hit_queue_clear")
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::Transfer);
        if (wavefrontTraceValidation.valid()) {
            secondaryHitClear.addStorageRead(wavefrontTraceValidation, PipelineDomain::Transfer);
        }
        graph.addPass("wavefront_secondary_trace_rt")
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing);
        graph.addPass("wavefront_secondary_shade_validation_clear")
            .addStorageWrite(wavefrontSecondaryShadeValidation, PipelineDomain::Transfer);
        RenderGraphPass& secondaryShadePass = graph.addPass("wavefront_secondary_shade")
            .addStorageRead(wavefrontCompactedRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontSecondaryShadeValidation, PipelineDomain::Compute);
        if (useRestirGiReservoirs && wavefrontRestirGiReservoir.valid()) {
            secondaryShadePass.addStorageWrite(wavefrontRestirGiReservoir, PipelineDomain::Compute);
        }
    }
    if (useWavefrontSort) {
        const RenderGraphResourceId wavefrontSortedShadeValidation = graph.createBuffer(bufferResource(wavefrontSortedShadeValidationBuffer_, "wavefront sorted shade validation"));
        RenderGraphPass& sortedHitClear = graph.addPass("wavefront_sorted_hit_queue_clear")
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::Transfer);
        if (wavefrontTraceValidation.valid()) {
            sortedHitClear.addStorageRead(wavefrontTraceValidation, PipelineDomain::Transfer);
        }
        graph.addPass("wavefront_sorted_trace_rt")
            .addStorageRead(wavefrontSortedRayQueue, PipelineDomain::RayTracing)
            .addStorageWrite(wavefrontHitQueue, PipelineDomain::RayTracing);
        graph.addPass("wavefront_sorted_shade_validation_clear")
            .addStorageWrite(wavefrontSortedShadeValidation, PipelineDomain::Transfer);
        RenderGraphPass& sortedShadePass = graph.addPass("wavefront_sorted_shade")
            .addStorageRead(wavefrontSortedRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageReadWrite(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(wavefrontSortedShadeValidation, PipelineDomain::Compute);
        if (useRestirGiReservoirs && wavefrontRestirGiReservoir.valid()) {
            sortedShadePass.addStorageWrite(wavefrontRestirGiReservoir, PipelineDomain::Compute);
        }
    }
    if (useWavefrontDebugWrite) {
        RenderGraphPass& debugWritePass = graph.addPass("wavefront_debug_write")
            .addStorageRead(wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageRead(useWavefrontCompact ? wavefrontCompactedRayQueue : wavefrontRayQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontHitQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontShadowQueue, PipelineDomain::Compute)
            .addStorageRead(wavefrontPixelState, PipelineDomain::Compute)
            .addStorageWrite(raw, PipelineDomain::Compute);
        if (wavefrontRestirReservoir.valid()) {
            debugWritePass.addStorageRead(wavefrontRestirReservoir, PipelineDomain::Compute);
        } else if (restirReservoirBuffer_.handle() != VK_NULL_HANDLE) {
            debugWritePass.addStorageRead(restirReservoir, PipelineDomain::Compute);
        }
        if (settings_.debugView == RendererDebugView::WavefrontRestirGi && wavefrontRestirGiReservoir.valid()) {
            debugWritePass.addStorageRead(wavefrontRestirGiReservoir, PipelineDomain::Compute);
        }
    }

    if (shouldRunRestirSpatial()) {
        graph.addPass("restir_spatial")
            .addStorageRead(restirReservoir, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageWrite(restirSpatialReservoir, PipelineDomain::Compute);
        graph.addPass("restir_spatial_copy")
            .addStorageRead(restirSpatialReservoir, PipelineDomain::Transfer)
            .addStorageWrite(restirReservoir, PipelineDomain::Transfer);
    }
    if (!settings_.wavefrontFinalOutputEnabled) {
        graph.addPass("fog_integrate")
            .addStorageReadWrite(raw, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute);
    }

    RenderGraphResourceId toneInput = raw;
    if (shouldRunDenoiser()) {
        graph.addPass("moment_update")
            .addStorageReadWrite(directDiffuseMoments, PipelineDomain::Compute)
            .addStorageReadWrite(directSpecularMoments, PipelineDomain::Compute)
            .addStorageReadWrite(indirectDiffuseMoments, PipelineDomain::Compute)
            .addStorageReadWrite(indirectSpecularMoments, PipelineDomain::Compute)
            .addStorageReadWrite(historyLength, PipelineDomain::Compute)
            .addStorageWrite(directDiffuseResolvedMoments, PipelineDomain::Compute)
            .addStorageWrite(directSpecularResolvedMoments, PipelineDomain::Compute)
            .addStorageWrite(indirectDiffuseResolvedMoments, PipelineDomain::Compute)
            .addStorageWrite(indirectSpecularResolvedMoments, PipelineDomain::Compute)
            .addStorageWrite(historyLengthResolved, PipelineDomain::Compute)
            .addStorageWrite(momentDebugResolved, PipelineDomain::Compute)
            .addStorageRead(pathData, PipelineDomain::Compute)
            .addStorageRead(velocity, PipelineDomain::Compute)
            .addStorageRead(previousWorldPosition, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageRead(worldPosition, PipelineDomain::Compute);
        graph.addPass("temporal_denoiser")
            .addStorageReadWrite(raw, PipelineDomain::Compute)
            .addStorageReadWrite(history, PipelineDomain::Compute)
            .addStorageReadWrite(diffuseHistory, PipelineDomain::Compute)
            .addStorageReadWrite(specularHistory, PipelineDomain::Compute)
            .addStorageReadWrite(directDiffuseMoments, PipelineDomain::Compute)
            .addStorageReadWrite(directSpecularMoments, PipelineDomain::Compute)
            .addStorageReadWrite(indirectDiffuseMoments, PipelineDomain::Compute)
            .addStorageReadWrite(indirectSpecularMoments, PipelineDomain::Compute)
            .addStorageReadWrite(historyLength, PipelineDomain::Compute)
            .addStorageReadWrite(momentDebug, PipelineDomain::Compute)
            .addStorageRead(variance, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageRead(worldPosition, PipelineDomain::Compute)
            .addStorageRead(previousWorldPosition, PipelineDomain::Compute)
            .addStorageRead(velocity, PipelineDomain::Compute)
            .addStorageRead(pathData, PipelineDomain::Compute)
            .addStorageWrite(denoised, PipelineDomain::Compute)
            .addStorageWrite(diffuseResolved, PipelineDomain::Compute)
            .addStorageWrite(specularResolved, PipelineDomain::Compute);
        RenderGraphPass& historyCopyPass = graph.addPass("history_copy")
            .addStorageRead(denoised, PipelineDomain::Transfer)
            .addStorageWrite(history, PipelineDomain::Transfer)
            .addStorageRead(diffuseResolved, PipelineDomain::Transfer)
            .addStorageWrite(diffuseHistory, PipelineDomain::Transfer)
            .addStorageRead(specularResolved, PipelineDomain::Transfer)
            .addStorageWrite(specularHistory, PipelineDomain::Transfer)
            .addStorageRead(worldPosition, PipelineDomain::Transfer)
            .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
            .addStorageRead(directDiffuseResolvedMoments, PipelineDomain::Transfer)
            .addStorageWrite(directDiffuseMoments, PipelineDomain::Transfer)
            .addStorageRead(directSpecularResolvedMoments, PipelineDomain::Transfer)
            .addStorageWrite(directSpecularMoments, PipelineDomain::Transfer)
            .addStorageRead(indirectDiffuseResolvedMoments, PipelineDomain::Transfer)
            .addStorageWrite(indirectDiffuseMoments, PipelineDomain::Transfer)
            .addStorageRead(indirectSpecularResolvedMoments, PipelineDomain::Transfer)
            .addStorageWrite(indirectSpecularMoments, PipelineDomain::Transfer)
            .addStorageRead(historyLengthResolved, PipelineDomain::Transfer)
            .addStorageWrite(historyLength, PipelineDomain::Transfer)
            .addStorageRead(momentDebugResolved, PipelineDomain::Transfer)
            .addStorageWrite(momentDebug, PipelineDomain::Transfer)
            .addStorageRead(restirReservoir, PipelineDomain::Transfer)
            .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer);
        if (useRestirGiReservoirs) {
            historyCopyPass
                .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Transfer)
                .addStorageWrite(previousRestirGiReservoir, PipelineDomain::Transfer);
        }
        toneInput = denoised;
    } else {
        RenderGraphPass& skipDenoiserCopyPass = graph.addPass("skip_denoiser_copy")
            .addStorageRead(raw, PipelineDomain::Transfer)
            .addStorageWrite(denoised, PipelineDomain::Transfer)
            .addStorageRead(worldPosition, PipelineDomain::Transfer)
            .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
            .addStorageRead(restirReservoir, PipelineDomain::Transfer)
            .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer);
        if (useRestirGiReservoirs) {
            skipDenoiserCopyPass
                .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Transfer)
                .addStorageWrite(previousRestirGiReservoir, PipelineDomain::Transfer);
        }
    }

    if (shouldRunTaa()) {
        graph.addPass("taa_resolve")
            .addStorageRead(toneInput, PipelineDomain::Compute)
            .addStorageReadWrite(taaHistory, PipelineDomain::Compute)
            .addStorageRead(velocity, PipelineDomain::Compute)
            .addStorageRead(depthNormal, PipelineDomain::Compute)
            .addStorageRead(pathData, PipelineDomain::Compute)
            .addStorageWrite(taa, PipelineDomain::Compute);
        graph.addPass("taa_history_copy")
            .addStorageRead(taa, PipelineDomain::Transfer)
            .addStorageWrite(taaHistory, PipelineDomain::Transfer);
        toneInput = taa;
    }

    if (settings_.autoExposureEnabled) {
        const RenderGraphResourceId histogram = graph.createBuffer(bufferResource(histogramBuffer_, "luminance histogram"));
        const RenderGraphResourceId exposure = graph.createBuffer(bufferResource(exposureBuffer_, "exposure"));
        graph.addPass("auto_exposure_histogram_clear")
            .addStorageWrite(histogram, PipelineDomain::Transfer);
        graph.addPass("auto_exposure_histogram")
            .addStorageRead(toneInput, PipelineDomain::Compute)
            .addStorageReadWrite(histogram, PipelineDomain::Compute);
        graph.addPass("auto_exposure_reduce")
            .addStorageRead(histogram, PipelineDomain::Compute)
            .addStorageWrite(exposure, PipelineDomain::Compute);
    }

    graph.addPass("tone_map")
        .addStorageRead(toneInput, PipelineDomain::Compute)
        .addStorageWrite(presentation, PipelineDomain::Compute);

    if (selectedInstanceId_ != UINT32_MAX) {
        graph.addPass("selection_outline")
            .addStorageRead(entityIds, PipelineDomain::Compute)
            .addStorageReadWrite(presentation, PipelineDomain::Compute);
    }

    graph.compile();
    const bool hasCompletedTimings = currentProfiler_->timings().totalMs() > 0.0f;
    if (dumpRenderGraphPath_.has_value()) {
        dumpRenderGraphJson(graph, currentProfiler_->timings(), *dumpRenderGraphPath_);
        if (hasCompletedTimings) {
            dumpRenderGraphPath_.reset();
        }
    }
    if (dumpRenderGraphDotPath_.has_value()) {
        dumpRenderGraphDot(graph, currentProfiler_->timings(), *dumpRenderGraphDotPath_);
        if (hasCompletedTimings) {
            dumpRenderGraphDotPath_.reset();
        }
    }
    validationLog_.recordPass("render graph compiled pass count=" + std::to_string(graph.compiledPassOrder().size()));
    for (uint32_t passIndex : graph.compiledPassOrder()) {
        validationLog_.recordPass("render graph pass: " + graph.passes()[passIndex].name());
    }
    for (const RenderGraphBarrier& barrier : graph.compiledBarriers()) {
        const RenderGraphResource& resource = graph.resources()[barrier.resource.index];
        const std::string resourceName = resource.debugName != nullptr ? resource.debugName : "<unnamed>";
        const std::string beforePass = barrier.beforePass < graph.passes().size() ? graph.passes()[barrier.beforePass].name() : "<external>";
        const std::string afterPass = barrier.afterPass < graph.passes().size() ? graph.passes()[barrier.afterPass].name() : "<external>";
        validationLog_.recordBarrier(
            "render graph barrier " + resourceName + " " + beforePass + " -> " + afterPass,
            barrier.before.stage,
            barrier.before.access,
            barrier.after.stage,
            barrier.after.access);
        validationLog_.recordResourceState(ResourceStateEvent{
            .resource = resourceName,
            .beforePass = beforePass,
            .afterPass = afterPass,
            .beforeLayout = barrier.before.layout,
            .afterLayout = barrier.after.layout,
            .beforeStage = barrier.before.stage,
            .afterStage = barrier.after.stage,
            .beforeAccess = barrier.before.access,
            .afterAccess = barrier.after.access,
        });
    }
}

void PathTracerRenderer::writeRayTracingDescriptors(
    DescriptorSet set,
    bool includeWavefrontQueues,
    const Buffer* wavefrontRayQueueOverride) {
    DescriptorWriter writer;
    writer
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, accumulationBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameCameraUniformOffset, sizeof(CameraUniform)))
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, varianceBuffer_.descriptorInfo())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldPositionBuffer_.descriptorInfo())
        .writeBuffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.materials().descriptorInfo())
        .writeBuffer(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, scene_.meshParamsBuffer().descriptorInfo())
        .writeImage(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, scene_.environmentImage().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(13, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = scene_.environmentSampler()})
        .writeBuffer(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.envRows().descriptorInfo())
        .writeBuffer(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.envCols().descriptorInfo())
        .writeBuffer(16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, scene_.envParamsBuffer().descriptorInfo())
        .writeBuffer(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.spheres().descriptorInfo())
        .writeBuffer(18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameDebugParamsOffset, sizeof(RendererDebugParams)))
        .writeBuffer(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.primitiveRecords().descriptorInfo())
        .writeBuffer(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.instanceRecords().descriptorInfo())
        .writeBuffer(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.lightRecords().descriptorInfo())
        .writeImageArray(41, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, scene_.materialCombinedDescriptors())
        .writeBuffer(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.meshRecords().descriptorInfo())
        .writeBuffer(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localVertices().descriptorInfo())
        .writeBuffer(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localIndices().descriptorInfo())
        .writeBuffer(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.localTriangles().descriptorInfo())
        .writeAccelerationStructure(33, rayTracingScene_->tlas())
        .writeBuffer(34, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.rtTriangleMaterialIds().descriptorInfo())
        .writeBuffer(35, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, entityIdBuffer_.descriptorInfo())
        .writeBuffer(36, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, velocityBuffer_.descriptorInfo())
        .writeBuffer(37, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFramePrevCameraUniformOffset, sizeof(PrevCameraUniform)))
        .writeBuffer(38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirReservoirBuffer_.descriptorInfo())
        .writeBuffer(39, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousRestirReservoirBuffer_.descriptorInfo())
        .writeBuffer(40, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, scene_.lightBvhNodes().descriptorInfo())
        .writeBuffer(42, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pathDataBuffer_.descriptorInfo())
        .writeBuffer(43, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiReservoirBuffer_.descriptorInfo())
        .writeBuffer(44, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousRestirGiReservoirBuffer_.descriptorInfo())
        .writeBuffer(45, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, restirGiSpatialReservoirBuffer_.descriptorInfo())
        .writeBuffer(46, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rayTracingScene_->geometryTriangleOffsetsBuffer().descriptorInfo())
        .writeBuffer(47, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rayTracingScene_->meshGeometryRangesBuffer().descriptorInfo())
        .writeBuffer(48, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rayTracingDiagnosticCountersBuffer_.descriptorInfo());
    const Buffer& wavefrontRayQueue = wavefrontRayQueueOverride != nullptr ? *wavefrontRayQueueOverride : wavefrontRayQueueBuffer_;
    if (includeWavefrontQueues &&
        wavefrontRayQueue.handle() != VK_NULL_HANDLE &&
        wavefrontHitQueueBuffer_.handle() != VK_NULL_HANDLE) {
        writer
            .writeBuffer(49, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontRayQueue.descriptorInfo())
            .writeBuffer(50, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontHitQueueBuffer_.descriptorInfo());
    }
    writer.update(context_.device(), set);
}

void PathTracerRenderer::recordHardwarePathTrace(VkCommandBuffer commandBuffer) {
    if (rayTracingPipeline_ == nullptr || rayTracingScene_ == nullptr) {
        throw std::runtime_error("Hardware ray tracing backend is active but RT pipeline/scene is not initialized");
    }
    RayTracingPipeline* pipeline = settings_.motionBlurEnabled &&
            rayTracingScene_->motionBlurActive() &&
            rayTracingMotionPipeline_ != nullptr
        ? rayTracingMotionPipeline_.get()
        : rayTracingPipeline_.get();

    DescriptorSet set = currentFrame_->descriptors().allocate(rayTracingSetLayout_);
    DescriptorSet atmosphereSet = currentFrame_->descriptors().allocate(atmosphereSetLayout_);
    writeRayTracingDescriptors(set, false);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->transmittanceLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->skyViewLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = atmosphereLutSystem_->sampler()})
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->aerialPerspectiveLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->multiScatterLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfRows().descriptorInfo() : scene_.envRows().descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfCols().descriptorInfo() : scene_.envCols().descriptorInfo())
        .update(context_.device(), atmosphereSet);

    pipeline->bind(commandBuffer);
    const std::array<VkDescriptorSet, 2> descriptorSets{set.handle(), atmosphereSet.handle()};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline->layout(), 0, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 0, nullptr);
    pipeline->traceRays(commandBuffer, renderExtent_.width, renderExtent_.height);
}

void PathTracerRenderer::recordWavefrontTracePass(
    VkCommandBuffer commandBuffer,
    const Buffer* wavefrontRayQueueOverride,
    bool copyHitHeaderReadback,
    const Buffer* indirectTraceBuffer,
    VkDeviceSize indirectTraceOffset,
    GpuProfiler::Query profilerStart,
    GpuProfiler::Query profilerEnd) {
    const Buffer& rayQueue = wavefrontRayQueueOverride != nullptr ? *wavefrontRayQueueOverride : wavefrontRayQueueBuffer_;
    RayTracingPipeline* tracePipeline = settings_.shaderExecutionReorderingEnabled && wavefrontTraceSerPipeline_ != nullptr
        ? wavefrontTraceSerPipeline_.get()
        : wavefrontTracePipeline_.get();
    if (tracePipeline == nullptr || rayTracingScene_ == nullptr) {
        return;
    }
    if (rayQueue.handle() == VK_NULL_HANDLE ||
        wavefrontHitQueueBuffer_.handle() == VK_NULL_HANDLE ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return;
    }

    validationLog_.recordPass(
        std::string(wavefrontRayQueueOverride != nullptr ? "wavefront sorted trace rt rays=" : "wavefront trace rt pixels=") +
        std::to_string(static_cast<VkDeviceSize>(renderExtent_.width) * renderExtent_.height) +
        " ser=" + std::to_string(settings_.shaderExecutionReorderingEnabled ? 1u : 0u));

    DescriptorSet set = currentFrame_->descriptors().allocate(rayTracingSetLayout_);
    DescriptorSet atmosphereSet = currentFrame_->descriptors().allocate(atmosphereSetLayout_);
    writeRayTracingDescriptors(set, true, &rayQueue);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->transmittanceLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->skyViewLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = atmosphereLutSystem_->sampler()})
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->aerialPerspectiveLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, atmosphereLutSystem_->multiScatterLut().sampledDescriptor(VK_NULL_HANDLE))
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfRows().descriptorInfo() : scene_.envRows().descriptorInfo())
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, atmosphereLutSystem_->samplingSystem() != nullptr ? atmosphereLutSystem_->samplingSystem()->cdfCols().descriptorInfo() : scene_.envCols().descriptorInfo())
        .update(context_.device(), atmosphereSet);

    tracePipeline->bind(commandBuffer);
    const std::array<VkDescriptorSet, 2> descriptorSets{set.handle(), atmosphereSet.handle()};
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        tracePipeline->layout(),
        0,
        static_cast<uint32_t>(descriptorSets.size()),
        descriptorSets.data(),
        0,
        nullptr);
    const bool useIndirectTrace = indirectTraceBuffer != nullptr &&
        indirectTraceBuffer->handle() != VK_NULL_HANDLE &&
        indirectTraceBuffer->supportsDeviceAddress() &&
        context_.rayTracingInfo().capabilities.traceRaysIndirect;
    if (useIndirectTrace) {
        bufferMemoryBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            indirectTraceBuffer->handle(),
            indirectTraceBuffer->size(),
            indirectTraceBuffer->baseOffset());
        const bool profile = profilerStart != GpuProfiler::Count && profilerEnd != GpuProfiler::Count;
        if (profile) {
            currentProfiler_->write(commandBuffer, profilerStart, pathTraceShaderStage());
        }
        tracePipeline->traceRaysIndirect(commandBuffer, *indirectTraceBuffer, indirectTraceOffset);
        if (profile) {
            currentProfiler_->write(commandBuffer, profilerEnd, pathTraceShaderStage());
        }
    } else {
        const bool profile = profilerStart != GpuProfiler::Count && profilerEnd != GpuProfiler::Count;
        if (profile) {
            currentProfiler_->write(commandBuffer, profilerStart, pathTraceShaderStage());
        }
        tracePipeline->traceRays(commandBuffer, renderExtent_.width, renderExtent_.height);
        if (profile) {
            currentProfiler_->write(commandBuffer, profilerEnd, pathTraceShaderStage());
        }
    }

    if (!copyHitHeaderReadback || wavefrontQueueHeaderReadbackBuffer_.handle() == VK_NULL_HANDLE) {
        return;
    }
    bufferMemoryBarrier(
        commandBuffer,
        pathTraceShaderStage(),
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        wavefrontHitQueueBuffer_.handle(),
        sizeof(WavefrontQueueHeaderGpu),
        wavefrontHitQueueBuffer_.baseOffset());
    VkBufferCopy copy{};
    copy.srcOffset = wavefrontHitQueueBuffer_.baseOffset();
    copy.dstOffset = sizeof(WavefrontQueueHeaderGpu);
    copy.size = sizeof(WavefrontQueueHeaderGpu);
    vkCmdCopyBuffer(commandBuffer, wavefrontHitQueueBuffer_.handle(), wavefrontQueueHeaderReadbackBuffer_.handle(), 1, &copy);
}

void PathTracerRenderer::recordWavefrontTraceValidationPass(VkCommandBuffer commandBuffer) {
    if (wavefrontTraceValidatePipeline_ == nullptr || wavefrontTraceValidateSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    if (wavefrontHitQueueBuffer_.handle() == VK_NULL_HANDLE ||
        depthNormalBuffer_.handle() == VK_NULL_HANDLE ||
        entityIdBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontTraceValidationBuffer_.handle() == VK_NULL_HANDLE ||
        wavefrontTraceValidationReadbackBuffer_.handle() == VK_NULL_HANDLE ||
        renderExtent_.width == 0u || renderExtent_.height == 0u) {
        return;
    }

    validationLog_.recordPass("wavefront trace validate");

    DescriptorSet set = currentFrame_->descriptors().allocate(wavefrontTraceValidateSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontHitQueueBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, entityIdBuffer_.descriptorInfo())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wavefrontTraceValidationBuffer_.descriptorInfo())
        .update(context_.device(), set);

    const WavefrontTraceValidationPush push{
        .width = renderExtent_.width,
        .height = renderExtent_.height,
        .depthEpsilon = 0.005f,
        .normalDotThreshold = 0.999f,
    };

    wavefrontTraceValidatePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefrontTraceValidatePipeline_->layout(),
        0,
        1,
        &descriptorSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        wavefrontTraceValidatePipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(push),
        &push);
    wavefrontTraceValidatePipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height, 8u, 8u);

    bufferMemoryBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        wavefrontTraceValidationBuffer_.handle(),
        wavefrontTraceValidationBuffer_.size());
    VkBufferCopy copy{};
    copy.size = sizeof(WavefrontTraceValidationGpu);
    vkCmdCopyBuffer(commandBuffer, wavefrontTraceValidationBuffer_.handle(), wavefrontTraceValidationReadbackBuffer_.handle(), 1, &copy);
}

void PathTracerRenderer::recordSelectionOutline(VkCommandBuffer commandBuffer) {
    if (selectedInstanceId_ == UINT32_MAX || selectionOutlinePipeline_ == nullptr || selectionOutlineSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    const RenderGraphResourceId presentation = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = presentationImage_.format(),
        .extent = presentationImage_.extent(),
        .image = presentationImage_.handle(),
        .imageRange = presentationImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : kCrossQueueShaderStage,
            .access = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = presentationImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = kCrossQueueShaderStage,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "presentation ldr",
    });
    const RenderGraphResourceId entityIds = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = entityIdBuffer_.size(),
        .buffer = entityIdBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "entity ids",
    });
    graph.addPass("selection_outline")
        .addStorageRead(entityIds, PipelineDomain::Compute)
        .addStorageReadWrite(presentation, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordSelectionOutlinePass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PathTracerRenderer::recordSelectionOutlinePass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("selection outline");
    currentProfiler_->write(commandBuffer, GpuProfiler::SelectionOutlineStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    const SelectionParams params{
        .selectedInstance = selectedInstanceId_,
        .width = displayExtent_.width,
        .height = displayExtent_.height,
        .enabled = 1u,
        .renderWidth = renderExtent_.width,
        .renderHeight = renderExtent_.height,
    };
    selectionParamsBuffer_.write(&params, sizeof(params));
    selectionParamsBuffer_.flush(sizeof(params));

    DescriptorSet set = currentFrame_->descriptors().allocate(selectionOutlineSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, presentationImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, entityIdBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, selectionParamsBuffer_.descriptorInfo())
        .update(context_.device(), set);

    selectionOutlinePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, selectionOutlinePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    selectionOutlinePipeline_->dispatch(commandBuffer, displayExtent_.width, displayExtent_.height, 8, 8);
    currentProfiler_->write(commandBuffer, GpuProfiler::SelectionOutlineEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

}

VkPipelineStageFlags2 PathTracerRenderer::pathTraceShaderStage() const {
    return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
}

void PathTracerRenderer::recordDenoiser(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    auto imageResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .bufferOffset = buffer.baseOffset(),
            .external = true,
            .debugName = name,
        };
    };

    const RenderGraphResourceId raw = graph.createTexture(imageResource(rawImage_, "raw hdr"));
    const RenderGraphResourceId history = graph.createTexture(imageResource(historyImage_, "history hdr"));
    const RenderGraphResourceId diffuseResolved = graph.createTexture(imageResource(diffuseResolvedImage_, "current diffuse history hdr"));
    const RenderGraphResourceId specularResolved = graph.createTexture(imageResource(specularResolvedImage_, "current specular history hdr"));
    const RenderGraphResourceId diffuseHistory = graph.createTexture(imageResource(diffuseHistoryImage_, "diffuse history hdr"));
    const RenderGraphResourceId specularHistory = graph.createTexture(imageResource(specularHistoryImage_, "specular history hdr"));
    const RenderGraphResourceId denoised = graph.createTexture(imageResource(denoisedImage_, "denoised hdr"));
    const RenderGraphResourceId variance = graph.createBuffer(bufferResource(varianceBuffer_, "variance"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal"));
    const RenderGraphResourceId worldPosition = graph.createBuffer(bufferResource(worldPositionBuffer_, "world position"));
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(bufferResource(previousWorldPositionBuffer_, "previous world position"));
    const RenderGraphResourceId velocity = graph.createBuffer(bufferResource(velocityBuffer_, "screen velocity"));
    const RenderGraphResourceId pathData = graph.createBuffer(bufferResource(pathDataBuffer_, "path data"));
    const RenderGraphResourceId directDiffuseMoments = graph.createTexture(imageResource(directDiffuseMomentsImage_, "direct diffuse moments"));
    const RenderGraphResourceId directSpecularMoments = graph.createTexture(imageResource(directSpecularMomentsImage_, "direct specular moments"));
    const RenderGraphResourceId indirectDiffuseMoments = graph.createTexture(imageResource(indirectDiffuseMomentsImage_, "indirect diffuse moments"));
    const RenderGraphResourceId indirectSpecularMoments = graph.createTexture(imageResource(indirectSpecularMomentsImage_, "indirect specular moments"));
    const RenderGraphResourceId historyLength = graph.createTexture(imageResource(historyLengthImage_, "history length"));
    const RenderGraphResourceId momentDebug = graph.createTexture(imageResource(momentDebugImage_, "moment debug"));
    graph.resources()[raw.index].hasInitialAccess = true;
    graph.resources()[raw.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    graph.resources()[history.index].hasInitialAccess = true;
    graph.resources()[history.index].initialAccess = ResourceAccess{
        .stage = historyImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT,
        .access = historyImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .layout = historyImage_.layout(),
    };
    graph.resources()[diffuseHistory.index].hasInitialAccess = true;
    graph.resources()[diffuseHistory.index].initialAccess = ResourceAccess{
        .stage = diffuseHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .access = diffuseHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        .layout = diffuseHistoryImage_.layout(),
    };
    graph.resources()[specularHistory.index].hasInitialAccess = true;
    graph.resources()[specularHistory.index].initialAccess = ResourceAccess{
        .stage = specularHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .access = specularHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        .layout = specularHistoryImage_.layout(),
    };
    graph.resources()[diffuseResolved.index].hasInitialAccess = true;
    graph.resources()[diffuseResolved.index].initialAccess = ResourceAccess{
        .stage = diffuseResolvedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .access = diffuseResolvedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT,
        .layout = diffuseResolvedImage_.layout(),
    };
    graph.resources()[specularResolved.index].hasInitialAccess = true;
    graph.resources()[specularResolved.index].initialAccess = ResourceAccess{
        .stage = specularResolvedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .access = specularResolvedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT,
        .layout = specularResolvedImage_.layout(),
    };
    graph.resources()[denoised.index].hasInitialAccess = true;
    graph.resources()[denoised.index].initialAccess = ResourceAccess{
        .stage = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : kCrossQueueShaderStage,
        .access = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .layout = denoisedImage_.layout(),
    };
    graph.resources()[variance.index].hasInitialAccess = true;
    graph.resources()[variance.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[depthNormal.index].hasInitialAccess = true;
    graph.resources()[depthNormal.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[worldPosition.index].hasInitialAccess = true;
    graph.resources()[worldPosition.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[velocity.index].hasInitialAccess = true;
    graph.resources()[velocity.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[pathData.index].hasInitialAccess = true;
    graph.resources()[pathData.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    auto setMomentInitialAccess = [&](RenderGraphResourceId id, const Image& image) {
        graph.resources()[id.index].hasInitialAccess = true;
        graph.resources()[id.index].initialAccess = ResourceAccess{
            .stage = image.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = image.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            .layout = image.layout(),
        };
    };
    setMomentInitialAccess(directDiffuseMoments, directDiffuseMomentsImage_);
    setMomentInitialAccess(directSpecularMoments, directSpecularMomentsImage_);
    setMomentInitialAccess(indirectDiffuseMoments, indirectDiffuseMomentsImage_);
    setMomentInitialAccess(indirectSpecularMoments, indirectSpecularMomentsImage_);
    setMomentInitialAccess(historyLength, historyLengthImage_);
    setMomentInitialAccess(momentDebug, momentDebugImage_);

    graph.addPass("temporal_denoiser")
        .addStorageReadWrite(raw, PipelineDomain::Compute)
        .addStorageReadWrite(history, PipelineDomain::Compute)
        .addStorageReadWrite(diffuseHistory, PipelineDomain::Compute)
        .addStorageReadWrite(specularHistory, PipelineDomain::Compute)
        .addStorageReadWrite(directDiffuseMoments, PipelineDomain::Compute)
        .addStorageReadWrite(directSpecularMoments, PipelineDomain::Compute)
        .addStorageReadWrite(indirectDiffuseMoments, PipelineDomain::Compute)
        .addStorageReadWrite(indirectSpecularMoments, PipelineDomain::Compute)
        .addStorageReadWrite(historyLength, PipelineDomain::Compute)
        .addStorageReadWrite(momentDebug, PipelineDomain::Compute)
        .addStorageRead(variance, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .addStorageRead(worldPosition, PipelineDomain::Compute)
        .addStorageRead(previousWorldPosition, PipelineDomain::Compute)
        .addStorageRead(velocity, PipelineDomain::Compute)
        .addStorageRead(pathData, PipelineDomain::Compute)
        .addStorageWrite(denoised, PipelineDomain::Compute)
        .addStorageWrite(diffuseResolved, PipelineDomain::Compute)
        .addStorageWrite(specularResolved, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordDenoiserPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordDenoiserPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("temporal denoiser compute");
    currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    historyImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    diffuseResolvedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    specularResolvedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    diffuseHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    specularHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    momentDebugImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(denoiserSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawImage_.storageDescriptor())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, varianceBuffer_.descriptorInfo())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, historyImage_.storageDescriptor())
        .writeImage(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, denoisedImage_.storageDescriptor())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameDenoiserParamsOffset, sizeof(DenoiserParams)))
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldPositionBuffer_.descriptorInfo())
        .writeBuffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousWorldPositionBuffer_.descriptorInfo())
        .writeBuffer(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFramePrevCameraUniformOffset, sizeof(PrevCameraUniform)))
        .writeBuffer(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, velocityBuffer_.descriptorInfo())
        .writeBuffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pathDataBuffer_.descriptorInfo())
        .writeImage(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, diffuseHistoryImage_.storageDescriptor())
        .writeImage(12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, specularHistoryImage_.storageDescriptor())
        .writeImage(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, diffuseResolvedImage_.storageDescriptor())
        .writeImage(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, specularResolvedImage_.storageDescriptor())
        .writeImage(15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, directDiffuseMomentsImage_.storageDescriptor())
        .writeImage(16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, directSpecularMomentsImage_.storageDescriptor())
        .writeImage(17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, indirectDiffuseMomentsImage_.storageDescriptor())
        .writeImage(18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, indirectSpecularMomentsImage_.storageDescriptor())
        .writeImage(19, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, historyLengthImage_.storageDescriptor())
        .writeImage(20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, momentDebugImage_.storageDescriptor())
        .update(context_.device(), set);

    denoiserPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, denoiserPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    denoiserPipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::DenoiserEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordMomentUpdate(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    auto imageResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .debugName = name,
        };
    };
    auto bufferResource = [](const Buffer& buffer, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = buffer.size(),
            .buffer = buffer.handle(),
            .bufferOffset = buffer.baseOffset(),
            .external = true,
            .debugName = name,
        };
    };

    const RenderGraphResourceId prev_dd = graph.createTexture(imageResource(directDiffuseMomentsImage_, "prev direct diffuse moments"));
    const RenderGraphResourceId prev_ds = graph.createTexture(imageResource(directSpecularMomentsImage_, "prev direct specular moments"));
    const RenderGraphResourceId prev_id = graph.createTexture(imageResource(indirectDiffuseMomentsImage_, "prev indirect diffuse moments"));
    const RenderGraphResourceId prev_is = graph.createTexture(imageResource(indirectSpecularMomentsImage_, "prev indirect specular moments"));
    const RenderGraphResourceId prev_hl = graph.createTexture(imageResource(historyLengthImage_, "prev history length"));
    const RenderGraphResourceId out_dd = graph.createTexture(imageResource(directDiffuseResolvedMomentsImage_, "out direct diffuse moments"));
    const RenderGraphResourceId out_ds = graph.createTexture(imageResource(directSpecularResolvedMomentsImage_, "out direct specular moments"));
    const RenderGraphResourceId out_id = graph.createTexture(imageResource(indirectDiffuseResolvedMomentsImage_, "out indirect diffuse moments"));
    const RenderGraphResourceId out_is = graph.createTexture(imageResource(indirectSpecularResolvedMomentsImage_, "out indirect specular moments"));
    const RenderGraphResourceId out_hl = graph.createTexture(imageResource(historyLengthResolvedImage_, "out history length"));
    const RenderGraphResourceId out_md = graph.createTexture(imageResource(momentDebugResolvedImage_, "out moment debug"));
    const RenderGraphResourceId pathData = graph.createBuffer(bufferResource(pathDataBuffer_, "path data"));
    const RenderGraphResourceId velocity = graph.createBuffer(bufferResource(velocityBuffer_, "velocity"));
    const RenderGraphResourceId prevWorldPos = graph.createBuffer(bufferResource(previousWorldPositionBuffer_, "prev world position"));
    const RenderGraphResourceId depthNormal = graph.createBuffer(bufferResource(depthNormalBuffer_, "depth normal"));
    const RenderGraphResourceId worldPos = graph.createBuffer(bufferResource(worldPositionBuffer_, "world position"));

    graph.resources()[prev_dd.index].hasInitialAccess = true;
    graph.resources()[prev_dd.index].initialAccess = ResourceAccess{
        .stage = directDiffuseMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .access = directDiffuseMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .layout = directDiffuseMomentsImage_.layout(),
    };
    graph.resources()[prev_ds.index].hasInitialAccess = true;
    graph.resources()[prev_ds.index].initialAccess = ResourceAccess{
        .stage = directSpecularMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .access = directSpecularMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .layout = directSpecularMomentsImage_.layout(),
    };
    graph.resources()[prev_id.index].hasInitialAccess = true;
    graph.resources()[prev_id.index].initialAccess = ResourceAccess{
        .stage = indirectDiffuseMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .access = indirectDiffuseMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .layout = indirectDiffuseMomentsImage_.layout(),
    };
    graph.resources()[prev_is.index].hasInitialAccess = true;
    graph.resources()[prev_is.index].initialAccess = ResourceAccess{
        .stage = indirectSpecularMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .access = indirectSpecularMomentsImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .layout = indirectSpecularMomentsImage_.layout(),
    };
    graph.resources()[prev_hl.index].hasInitialAccess = true;
    graph.resources()[prev_hl.index].initialAccess = ResourceAccess{
        .stage = historyLengthImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .access = historyLengthImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .layout = historyLengthImage_.layout(),
    };
    auto setMomentOutputInitialAccess = [&](RenderGraphResourceId id, const Image& image) {
        graph.resources()[id.index].hasInitialAccess = true;
        graph.resources()[id.index].initialAccess = ResourceAccess{
            .stage = image.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = image.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            .layout = image.layout(),
        };
    };
    setMomentOutputInitialAccess(out_dd, directDiffuseResolvedMomentsImage_);
    setMomentOutputInitialAccess(out_ds, directSpecularResolvedMomentsImage_);
    setMomentOutputInitialAccess(out_id, indirectDiffuseResolvedMomentsImage_);
    setMomentOutputInitialAccess(out_is, indirectSpecularResolvedMomentsImage_);
    setMomentOutputInitialAccess(out_hl, historyLengthResolvedImage_);
    setMomentOutputInitialAccess(out_md, momentDebugResolvedImage_);
    graph.resources()[pathData.index].hasInitialAccess = true;
    graph.resources()[pathData.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[velocity.index].hasInitialAccess = true;
    graph.resources()[velocity.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[prevWorldPos.index].hasInitialAccess = true;
    graph.resources()[prevWorldPos.index].initialAccess = ResourceAccess{
        .stage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    };
    graph.resources()[depthNormal.index].hasInitialAccess = true;
    graph.resources()[depthNormal.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    graph.resources()[worldPos.index].hasInitialAccess = true;
    graph.resources()[worldPos.index].initialAccess = ResourceAccess{
        .stage = pathTraceShaderStage(),
        .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };

    graph.addPass("moment_update")
        .addStorageReadWrite(prev_dd, PipelineDomain::Compute)
        .addStorageReadWrite(prev_ds, PipelineDomain::Compute)
        .addStorageReadWrite(prev_id, PipelineDomain::Compute)
        .addStorageReadWrite(prev_is, PipelineDomain::Compute)
        .addStorageReadWrite(prev_hl, PipelineDomain::Compute)
        .addStorageWrite(out_dd, PipelineDomain::Compute)
        .addStorageWrite(out_ds, PipelineDomain::Compute)
        .addStorageWrite(out_id, PipelineDomain::Compute)
        .addStorageWrite(out_is, PipelineDomain::Compute)
        .addStorageWrite(out_hl, PipelineDomain::Compute)
        .addStorageWrite(out_md, PipelineDomain::Compute)
        .addStorageRead(pathData, PipelineDomain::Compute)
        .addStorageRead(velocity, PipelineDomain::Compute)
        .addStorageRead(prevWorldPos, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .addStorageRead(worldPos, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordMomentUpdatePass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordMomentUpdatePass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("moment update");
    currentProfiler_->write(commandBuffer, GpuProfiler::MomentUpdateStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    directDiffuseMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    directSpecularMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectDiffuseMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectSpecularMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    historyLengthImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    directDiffuseResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    directSpecularResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectDiffuseResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectSpecularResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    historyLengthResolvedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    momentDebugResolvedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    momentParams_.resetHistory = denoiserParams_.resetHistory;
    momentParams_.framesSinceReset = denoiserParams_.framesSinceReset;
    momentParams_.width = renderExtent_.width;
    momentParams_.height = renderExtent_.height;
    momentParams_.maxHistoryLength = effectiveDenoiserMaxHistoryLength();
    momentParams_.validityThreshold = settings_.momentValidityThreshold;

    auto& frameUniforms = currentFrame_->uniformRing();
    frameUniforms.write(&momentParams_, sizeof(momentParams_), kFrameMomentParamsOffset);
    frameUniforms.flush(sizeof(momentParams_), kFrameMomentParamsOffset);

    DescriptorSet set = currentFrame_->descriptors().allocate(momentUpdateSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pathDataBuffer_.descriptorInfo())
        .writeImage(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, directDiffuseMomentsImage_.storageDescriptor())
        .writeImage(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, directSpecularMomentsImage_.storageDescriptor())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, indirectDiffuseMomentsImage_.storageDescriptor())
        .writeImage(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, indirectSpecularMomentsImage_.storageDescriptor())
        .writeImage(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, historyLengthImage_.storageDescriptor())
        .writeImage(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, directDiffuseResolvedMomentsImage_.storageDescriptor())
        .writeImage(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, directSpecularResolvedMomentsImage_.storageDescriptor())
        .writeImage(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, indirectDiffuseResolvedMomentsImage_.storageDescriptor())
        .writeImage(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, indirectSpecularResolvedMomentsImage_.storageDescriptor())
        .writeImage(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, historyLengthResolvedImage_.storageDescriptor())
        .writeBuffer(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameMomentParamsOffset, sizeof(MomentParams)))
        .writeBuffer(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, velocityBuffer_.descriptorInfo())
        .writeBuffer(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, previousWorldPositionBuffer_.descriptorInfo())
        .writeBuffer(14, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFramePrevCameraUniformOffset, sizeof(PrevCameraUniform)))
        .writeBuffer(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldPositionBuffer_.descriptorInfo())
        .writeImage(17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, momentDebugResolvedImage_.storageDescriptor())
        .update(context_.device(), set);

    momentUpdatePipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, momentUpdatePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    momentUpdatePipeline_->dispatch(commandBuffer, renderExtent_.width, renderExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::MomentUpdateEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::copyHistoryResources(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    auto copySourceInitialAccess = [](const Image& image) {
        ResourceAccess access{};
        access.layout = image.layout();
        switch (image.layout()) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            access.stage = VK_PIPELINE_STAGE_2_NONE;
            access.access = VK_ACCESS_2_NONE;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            access.stage = kCrossQueueShaderStage;
            access.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            access.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            access.access = VK_ACCESS_2_TRANSFER_READ_BIT;
            break;
        default:
            access.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            access.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            break;
        }
        return access;
    };
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = denoisedImage_.format(),
        .extent = denoisedImage_.extent(),
        .image = denoisedImage_.handle(),
        .imageRange = denoisedImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = kCrossQueueShaderStage,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "denoised hdr",
    });
    const RenderGraphResourceId history = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = historyImage_.format(),
        .extent = historyImage_.extent(),
        .image = historyImage_.handle(),
        .imageRange = historyImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "history hdr",
    });
    const RenderGraphResourceId diffuseResolved = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = diffuseResolvedImage_.format(),
        .extent = diffuseResolvedImage_.extent(),
        .image = diffuseResolvedImage_.handle(),
        .imageRange = diffuseResolvedImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = copySourceInitialAccess(denoisedImage_),
        .debugName = "current diffuse history hdr",
    });
    const RenderGraphResourceId specularResolved = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = specularResolvedImage_.format(),
        .extent = specularResolvedImage_.extent(),
        .image = specularResolvedImage_.handle(),
        .imageRange = specularResolvedImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "current specular history hdr",
    });
    const RenderGraphResourceId diffuseHistory = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = diffuseHistoryImage_.format(),
        .extent = diffuseHistoryImage_.extent(),
        .image = diffuseHistoryImage_.handle(),
        .imageRange = diffuseHistoryImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "diffuse history hdr",
    });
    const RenderGraphResourceId specularHistory = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = specularHistoryImage_.format(),
        .extent = specularHistoryImage_.extent(),
        .image = specularHistoryImage_.handle(),
        .imageRange = specularHistoryImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "specular history hdr",
    });
    auto momentCopyResource = [](const Image& image, const char* name) {
        return RenderGraphResource{
            .type = RenderGraphResource::Type::Texture,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .format = image.format(),
            .extent = image.extent(),
            .image = image.handle(),
            .imageRange = image.fullRange(),
            .external = true,
            .hasInitialAccess = true,
            .initialAccess = ResourceAccess{
                .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .layout = VK_IMAGE_LAYOUT_GENERAL,
            },
            .hasFinalAccess = true,
            .finalAccess = ResourceAccess{
                .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .layout = VK_IMAGE_LAYOUT_GENERAL,
            },
            .debugName = name,
        };
    };
    const RenderGraphResourceId directDiffuseResolvedMoments = graph.createTexture(momentCopyResource(directDiffuseResolvedMomentsImage_, "current direct diffuse moments"));
    const RenderGraphResourceId directSpecularResolvedMoments = graph.createTexture(momentCopyResource(directSpecularResolvedMomentsImage_, "current direct specular moments"));
    const RenderGraphResourceId indirectDiffuseResolvedMoments = graph.createTexture(momentCopyResource(indirectDiffuseResolvedMomentsImage_, "current indirect diffuse moments"));
    const RenderGraphResourceId indirectSpecularResolvedMoments = graph.createTexture(momentCopyResource(indirectSpecularResolvedMomentsImage_, "current indirect specular moments"));
    const RenderGraphResourceId historyLengthResolved = graph.createTexture(momentCopyResource(historyLengthResolvedImage_, "current history length"));
    const RenderGraphResourceId momentDebugResolved = graph.createTexture(momentCopyResource(momentDebugResolvedImage_, "current moment debug"));
    const RenderGraphResourceId directDiffuseMoments = graph.createTexture(momentCopyResource(directDiffuseMomentsImage_, "direct diffuse moments"));
    const RenderGraphResourceId directSpecularMoments = graph.createTexture(momentCopyResource(directSpecularMomentsImage_, "direct specular moments"));
    const RenderGraphResourceId indirectDiffuseMoments = graph.createTexture(momentCopyResource(indirectDiffuseMomentsImage_, "indirect diffuse moments"));
    const RenderGraphResourceId indirectSpecularMoments = graph.createTexture(momentCopyResource(indirectSpecularMomentsImage_, "indirect specular moments"));
    const RenderGraphResourceId historyLength = graph.createTexture(momentCopyResource(historyLengthImage_, "history length"));
    const RenderGraphResourceId momentDebug = graph.createTexture(momentCopyResource(momentDebugImage_, "moment debug"));
    const RenderGraphResourceId worldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = worldPositionBuffer_.size(),
        .buffer = worldPositionBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "world position",
    });
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousWorldPositionBuffer_.size(),
        .buffer = previousWorldPositionBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous world position",
    });
    const RenderGraphResourceId restirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = restirReservoirBuffer_.size(),
        .buffer = restirReservoirBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "restir reservoir",
    });
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousRestirReservoirBuffer_.size(),
        .buffer = previousRestirReservoirBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous restir reservoir",
    });
    const bool useRestirGiReservoirs = shouldUseRestirGiReservoirs();
    RenderGraphResourceId restirGiSpatialReservoir{};
    RenderGraphResourceId previousRestirGiReservoir{};
    if (useRestirGiReservoirs) {
        restirGiSpatialReservoir = graph.createBuffer(RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = restirGiSpatialReservoirBuffer_.size(),
            .buffer = restirGiSpatialReservoirBuffer_.handle(),
            .external = true,
            .hasInitialAccess = true,
            .initialAccess = ResourceAccess{
                .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            },
            .debugName = "restir gi spatial reservoir",
        });
        previousRestirGiReservoir = graph.createBuffer(RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = previousRestirGiReservoirBuffer_.size(),
            .buffer = previousRestirGiReservoirBuffer_.handle(),
            .external = true,
            .hasFinalAccess = true,
            .finalAccess = ResourceAccess{
                .stage = pathTraceShaderStage(),
                .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            },
            .debugName = "previous restir gi reservoir",
        });
    }
    RenderGraphPass& historyCopyPass = graph.addPass("history_copy")
        .addStorageRead(denoised, PipelineDomain::Transfer)
        .addStorageWrite(history, PipelineDomain::Transfer)
        .addStorageRead(diffuseResolved, PipelineDomain::Transfer)
        .addStorageWrite(diffuseHistory, PipelineDomain::Transfer)
        .addStorageRead(specularResolved, PipelineDomain::Transfer)
        .addStorageWrite(specularHistory, PipelineDomain::Transfer)
        .addStorageRead(directDiffuseResolvedMoments, PipelineDomain::Transfer)
        .addStorageWrite(directDiffuseMoments, PipelineDomain::Transfer)
        .addStorageRead(directSpecularResolvedMoments, PipelineDomain::Transfer)
        .addStorageWrite(directSpecularMoments, PipelineDomain::Transfer)
        .addStorageRead(indirectDiffuseResolvedMoments, PipelineDomain::Transfer)
        .addStorageWrite(indirectDiffuseMoments, PipelineDomain::Transfer)
        .addStorageRead(indirectSpecularResolvedMoments, PipelineDomain::Transfer)
        .addStorageWrite(indirectSpecularMoments, PipelineDomain::Transfer)
        .addStorageRead(historyLengthResolved, PipelineDomain::Transfer)
        .addStorageWrite(historyLength, PipelineDomain::Transfer)
        .addStorageRead(momentDebugResolved, PipelineDomain::Transfer)
        .addStorageWrite(momentDebug, PipelineDomain::Transfer)
        .addStorageRead(worldPosition, PipelineDomain::Transfer)
        .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
        .addStorageRead(restirReservoir, PipelineDomain::Transfer)
        .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer);
    if (useRestirGiReservoirs) {
        historyCopyPass
            .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Transfer)
            .addStorageWrite(previousRestirGiReservoir, PipelineDomain::Transfer);
    }
    historyCopyPass.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            copyHistoryResourcesPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    historyImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    diffuseHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    specularHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    directDiffuseMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    directSpecularMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectDiffuseMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectSpecularMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    historyLengthImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    momentDebugImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    directDiffuseResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    directSpecularResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectDiffuseResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    indirectSpecularResolvedMomentsImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    historyLengthResolvedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    momentDebugResolvedImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
}

void PathTracerRenderer::copyHistoryResourcesPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("history copy");
    currentProfiler_->write(commandBuffer, GpuProfiler::HistoryCopyStart, VK_PIPELINE_STAGE_2_COPY_BIT);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    historyImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    diffuseResolvedImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    specularResolvedImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    diffuseHistoryImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    specularHistoryImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy imageCopy{};
    imageCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopy.srcSubresource.layerCount = 1;
    imageCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopy.dstSubresource.layerCount = 1;
    imageCopy.extent = denoisedImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        denoisedImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        historyImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &imageCopy);

    VkImageCopy diffuseCopy = imageCopy;
    diffuseCopy.extent = diffuseResolvedImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        diffuseResolvedImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        diffuseHistoryImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &diffuseCopy);

    VkImageCopy specularCopy = imageCopy;
    specularCopy.extent = specularResolvedImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        specularResolvedImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        specularHistoryImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &specularCopy);

    auto copyImage = [&](const Image& src, const Image& dst) {
        src.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        dst.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent = src.extent();
        vkCmdCopyImage(commandBuffer, src.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    };
    copyImage(directDiffuseResolvedMomentsImage_, directDiffuseMomentsImage_);
    copyImage(directSpecularResolvedMomentsImage_, directSpecularMomentsImage_);
    copyImage(indirectDiffuseResolvedMomentsImage_, indirectDiffuseMomentsImage_);
    copyImage(indirectSpecularResolvedMomentsImage_, indirectSpecularMomentsImage_);
    copyImage(historyLengthResolvedImage_, historyLengthImage_);
    copyImage(momentDebugResolvedImage_, momentDebugImage_);

    VkBufferCopy copy{};
    copy.size = worldPositionBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, worldPositionBuffer_.handle(), previousWorldPositionBuffer_.handle(), 1, &copy);
    VkBufferCopy restirCopy{};
    restirCopy.size = restirReservoirBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, restirReservoirBuffer_.handle(), previousRestirReservoirBuffer_.handle(), 1, &restirCopy);
    if (shouldUseRestirGiReservoirs()) {
        VkBufferCopy restirGiCopy{};
        restirGiCopy.size = restirGiSpatialReservoirBuffer_.size();
        vkCmdCopyBuffer(commandBuffer, restirGiSpatialReservoirBuffer_.handle(), previousRestirGiReservoirBuffer_.handle(), 1, &restirGiCopy);
        restirGiHistoryValid_ = true;
    }
    if (temporalSystem_) {
        temporalSystem_->markSlotWritten("denoiser_history");
        temporalSystem_->markSlotWritten("denoiser_diffuse_history");
        temporalSystem_->markSlotWritten("denoiser_specular_history");
        temporalSystem_->markSlotWritten("previous_world_position");
        temporalSystem_->markSlotWritten("restir_reservoir");
    }
    denoiserHistoryValid_ = true;
    currentProfiler_->write(commandBuffer, GpuProfiler::HistoryCopyEnd, VK_PIPELINE_STAGE_2_COPY_BIT);
}

bool PathTracerRenderer::shouldRunDenoiser() const {
    if (settings_.wavefrontFinalOutputEnabled) {
        return false;
    }
    if (denoiserParams_.enabled != 0u) {
        return true;
    }
    if (denoiserParams_.debugView >= 1u && denoiserParams_.debugView <= 4u) {
        return true;
    }
    if (denoiserParams_.debugView == static_cast<uint32_t>(RendererDebugView::MotionVectors)) {
        return true;
    }
    if (denoiserParams_.debugView == static_cast<uint32_t>(RendererDebugView::TemporalReactiveMask) ||
        denoiserParams_.debugView == static_cast<uint32_t>(RendererDebugView::TemporalHistoryWeight)) {
        return true;
    }
    if (denoiserParams_.debugView >= static_cast<uint32_t>(RendererDebugView::PathDirectDiffuse) &&
        denoiserParams_.debugView <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularHistoryWeight)) {
        return true;
    }
    if (denoiserParams_.debugView >= static_cast<uint32_t>(RendererDebugView::DenoiserDirectDiffuseVariance) &&
        denoiserParams_.debugView <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularRawVariance)) {
        return true;
    }
    return false;
}

bool PathTracerRenderer::isNonDenoiserDebugView() const {
    const uint32_t view = debugParams_.view;
    if (view == 0u) {
        return false;
    }
    if (view <= 4u) {
        return false;
    }
    if (view == static_cast<uint32_t>(RendererDebugView::MotionVectors)) {
        return false;
    }
    if (view == static_cast<uint32_t>(RendererDebugView::TemporalReactiveMask) ||
        view == static_cast<uint32_t>(RendererDebugView::TemporalHistoryWeight)) {
        return false;
    }
    if (view >= static_cast<uint32_t>(RendererDebugView::PathDirectDiffuse) &&
        view <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularHistoryWeight)) {
        return false;
    }
    if (view >= static_cast<uint32_t>(RendererDebugView::DenoiserDirectDiffuseVariance) &&
        view <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularRawVariance)) {
        return false;
    }
    return true;
}

bool PathTracerRenderer::shouldRunTaa() const {
    return !settings_.wavefrontFinalOutputEnabled &&
        settings_.pathTracingEnabled &&
        settings_.taaEnabled &&
        taaPipeline_ != nullptr &&
        taaSetLayout_ != VK_NULL_HANDLE &&
        taaImage_.handle() != VK_NULL_HANDLE &&
        taaHistoryImage_.handle() != VK_NULL_HANDLE &&
        velocityBuffer_.handle() != VK_NULL_HANDLE;
}

bool PathTracerRenderer::shouldRunRestirSpatial() const {
    return !settings_.wavefrontFinalOutputEnabled &&
        !adaptiveSkipRestirSpatial_ &&
        settings_.restirMode != RestirMode::ClassicNee &&
        restirSpatialPipeline_ != nullptr &&
        restirSpatialSetLayout_ != VK_NULL_HANDLE &&
        restirReservoirBuffer_.handle() != VK_NULL_HANDLE &&
        restirSpatialReservoirBuffer_.handle() != VK_NULL_HANDLE &&
        depthNormalBuffer_.handle() != VK_NULL_HANDLE;
}

bool PathTracerRenderer::shouldUseRestirGiReservoirs() const {
    return settings_.restirGiEnabled ||
        settings_.debugView == RendererDebugView::RestirGiValidity ||
        settings_.debugView == RendererDebugView::RestirGiAge ||
        settings_.debugView == RendererDebugView::RestirGiInitial ||
        settings_.debugView == RendererDebugView::RestirGiTemporal ||
        settings_.debugView == RendererDebugView::RestirGiSpatial ||
        settings_.debugView == RendererDebugView::RestirGiFinal ||
        settings_.debugView == RendererDebugView::RestirGiNormal ||
        settings_.debugView == RendererDebugView::RestirGiHitDistance ||
        settings_.debugView == RendererDebugView::WavefrontRestirGi;
}

bool PathTracerRenderer::shouldRunRestirGiFinal() const {
    return !settings_.wavefrontFinalOutputEnabled &&
        (settings_.restirGiEnabled ||
        settings_.debugView == RendererDebugView::RestirGiSpatial ||
        settings_.debugView == RendererDebugView::RestirGiFinal);
}

bool PathTracerRenderer::shouldUseWavefrontFinalOutput() const {
    return settings_.wavefrontFinalOutputEnabled &&
        settings_.wavefrontQueuesEnabled &&
        settings_.wavefrontPrimaryGenerateEnabled &&
        settings_.wavefrontTraceEnabled &&
        settings_.wavefrontShadeEnabled &&
        settings_.wavefrontCompactEnabled &&
        settings_.wavefrontShadowTraceEnabled &&
        wavefrontDebugWritePipeline_ != nullptr &&
        wavefrontDebugWriteSetLayout_ != VK_NULL_HANDLE &&
        rawImage_.handle() != VK_NULL_HANDLE &&
        wavefrontRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontCompactedRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontHitQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontShadowQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontPixelStateBuffer_.handle() != VK_NULL_HANDLE;
}

bool PathTracerRenderer::shouldRunWavefrontDebugWrite() const {
    const bool needsRestirReservoir = settings_.debugView == RendererDebugView::WavefrontRestirDi;
    const bool needsRestirGiReservoir = settings_.debugView == RendererDebugView::WavefrontRestirGi;
    return isWavefrontDebugView(settings_.debugView) &&
        wavefrontDebugWritePipeline_ != nullptr &&
        wavefrontDebugWriteSetLayout_ != VK_NULL_HANDLE &&
        rawImage_.handle() != VK_NULL_HANDLE &&
        wavefrontRayQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontHitQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontShadowQueueBuffer_.handle() != VK_NULL_HANDLE &&
        wavefrontPixelStateBuffer_.handle() != VK_NULL_HANDLE &&
        (!needsRestirReservoir || wavefrontRestirReservoirBuffer_.handle() != VK_NULL_HANDLE) &&
        (!needsRestirGiReservoir || wavefrontRestirGiReservoirBuffer_.handle() != VK_NULL_HANDLE);
}

bool PathTracerRenderer::effectiveLimitSamplesPerPixel() const {
    return settings_.limitSamplesPerPixel || memoryPressureTier_ > 0u;
}

bool PathTracerRenderer::effectiveRestirGiHalfResolution() const {
    return settings_.restirGiHalfResolution || memoryPressureTier_ > 0u;
}

uint32_t PathTracerRenderer::wavefrontMaxPathDepth() const {
    return std::max(1u, adaptiveEffectiveMaxBounces_);
}

uint32_t PathTracerRenderer::wavefrontQueueCapacityFor(VkDeviceSize pixelCount) const {
    if (pixelCount == 0) {
        return 1u;
    }
    const VkDeviceSize multiplier = settings_.wavefrontShadeEnabled
        ? static_cast<VkDeviceSize>(std::max(1u, wavefrontMaxPathDepth()))
        : 1u;
    const VkDeviceSize capacity = pixelCount > std::numeric_limits<VkDeviceSize>::max() / multiplier
        ? static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max())
        : pixelCount * multiplier;
    return capacity > static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(capacity);
}

uint32_t PathTracerRenderer::effectiveDenoiserMaxHistoryLength() const {
    if (memoryPressureTier_ >= 3u) {
        return std::min(settings_.denoiserMaxHistoryLength, 16u);
    }
    if (memoryPressureTier_ >= 2u) {
        return std::min(settings_.denoiserMaxHistoryLength, 24u);
    }
    if (memoryPressureTier_ >= 1u) {
        return std::min(settings_.denoiserMaxHistoryLength, 32u);
    }
    return settings_.denoiserMaxHistoryLength;
}

VkDeviceSize PathTracerRenderer::restirGiReservoirStride() const {
    return restirGiUncompressedLayout_
        ? sizeof(RestirGiReservoirUncompressedGpu)
        : sizeof(RestirGiReservoirGpu);
}

const char* PathTracerRenderer::restirGiReservoirLayoutName() const {
    return restirGiUncompressedLayout_ ? "uncompressed" : "compressed";
}

const Image& PathTracerRenderer::postDenoiseImage() const {
    return denoisedImage_;
}

const Image& PathTracerRenderer::hdrPostProcessImage() const {
    return shouldRunTaa() ? taaImage_ : postDenoiseImage();
}

void PathTracerRenderer::recordTaa(VkCommandBuffer commandBuffer, bool deferHistoryCopy) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    const Image& inputImage = postDenoiseImage();
    const RenderGraphResourceId input = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = inputImage.format(),
        .extent = inputImage.extent(),
        .image = inputImage.handle(),
        .imageRange = inputImage.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = inputImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_NONE
                : (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
            .access = inputImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_ACCESS_2_NONE
                : (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            .layout = inputImage.layout(),
        },
        .debugName = "taa input hdr",
    });
    const RenderGraphResourceId output = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = taaImage_.format(),
        .extent = taaImage_.extent(),
        .image = taaImage_.handle(),
        .imageRange = taaImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = taaImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : kCrossQueueShaderStage,
            .access = taaImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = taaImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = kCrossQueueShaderStage,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "taa output hdr",
    });
    const RenderGraphResourceId history = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = taaHistoryImage_.format(),
        .extent = taaHistoryImage_.extent(),
        .image = taaHistoryImage_.handle(),
        .imageRange = taaHistoryImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = taaHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT,
            .access = taaHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .layout = taaHistoryImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "taa history hdr",
    });
    const RenderGraphResourceId velocity = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = velocityBuffer_.size(),
        .buffer = velocityBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        },
        .debugName = "screen velocity",
    });
    const RenderGraphResourceId depthNormal = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = depthNormalBuffer_.size(),
        .buffer = depthNormalBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "depth normal",
    });
    const RenderGraphResourceId pathData = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = pathDataBuffer_.size(),
        .buffer = pathDataBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "path data",
    });
    graph.addPass("taa_resolve")
        .addStorageRead(input, PipelineDomain::Compute)
        .addStorageReadWrite(history, PipelineDomain::Compute)
        .addStorageRead(velocity, PipelineDomain::Compute)
        .addStorageRead(depthNormal, PipelineDomain::Compute)
        .addStorageRead(pathData, PipelineDomain::Compute)
        .addStorageWrite(output, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordTaaPass(cmd);
        });
    if (!deferHistoryCopy) {
        graph.addPass("taa_history_copy")
            .addStorageRead(output, PipelineDomain::Transfer)
            .addStorageWrite(history, PipelineDomain::Transfer)
            .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
                recordTaaHistoryCopyPass(cmd);
            });
    }
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
    taaImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (deferHistoryCopy) {
        asyncTaaHistoryCopyPending_ = true;
    } else {
        taaHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    }
}

void PathTracerRenderer::recordTaaPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("taa resolve");
    currentProfiler_->write(commandBuffer, GpuProfiler::TaaStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    const Image& inputImage = postDenoiseImage();
    inputImage.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    taaHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    taaImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = currentFrame_->descriptors().allocate(taaSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, inputImage.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .writeImage(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, taaHistoryImage_.storageDescriptor())
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, taaImage_.storageDescriptor())
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, velocityBuffer_.descriptorInfo())
        .writeBuffer(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, currentFrame_->uniformRing().descriptorInfo(kFrameTaaParamsOffset, sizeof(TaaParams)))
        .writeBuffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, depthNormalBuffer_.descriptorInfo())
        .writeBuffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pathDataBuffer_.descriptorInfo())
        .update(context_.device(), set);

    taaPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, taaPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    taaPipeline_->dispatch(commandBuffer, displayExtent_.width, displayExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::TaaEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::copyTaaHistory(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    const RenderGraphResourceId output = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = taaImage_.format(),
        .extent = taaImage_.extent(),
        .image = taaImage_.handle(),
        .imageRange = taaImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = taaImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_NONE
                : kCrossQueueShaderStage,
            .access = taaImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_ACCESS_2_NONE
                : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = taaImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = kCrossQueueShaderStage,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "taa output hdr",
    });
    const RenderGraphResourceId history = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = taaHistoryImage_.format(),
        .extent = taaHistoryImage_.extent(),
        .image = taaHistoryImage_.handle(),
        .imageRange = taaHistoryImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = taaHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_NONE
                : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = taaHistoryImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_ACCESS_2_NONE
                : VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = taaHistoryImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "taa history hdr",
    });
    graph.addPass("taa_history_copy")
        .addStorageRead(output, PipelineDomain::Transfer)
        .addStorageWrite(history, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordTaaHistoryCopyPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
    taaImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    taaHistoryImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
}

void PathTracerRenderer::recordTaaHistoryCopyPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("taa history copy");
    currentProfiler_->write(commandBuffer, GpuProfiler::TaaHistoryCopyStart, VK_PIPELINE_STAGE_2_COPY_BIT);
    taaImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    taaHistoryImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = taaImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        taaImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        taaHistoryImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);
    if (temporalSystem_) {
        temporalSystem_->markSlotWritten("taa_history");
    }
    taaHistoryValid_ = true;
    currentProfiler_->write(commandBuffer, GpuProfiler::TaaHistoryCopyEnd, VK_PIPELINE_STAGE_2_COPY_BIT);
}

void PathTracerRenderer::recordAutoExposure(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    const Image& sourceImage = hdrPostProcessImage();
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = sourceImage.format(),
        .extent = sourceImage.extent(),
        .image = sourceImage.handle(),
        .imageRange = sourceImage.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = sourceImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_2_NONE
                : kCrossQueueShaderStage,
            .access = sourceImage.layout() == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_ACCESS_2_NONE
                : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = sourceImage.layout(),
        },
        .debugName = "post temporal hdr",
    });
    const RenderGraphResourceId histogram = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = histogramBuffer_.size(),
        .buffer = histogramBuffer_.handle(),
        .external = true,
        .debugName = "luminance histogram",
    });
    const RenderGraphResourceId exposure = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = exposureBuffer_.size(),
        .buffer = exposureBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "exposure",
    });
    graph.addPass("auto_exposure_histogram_clear")
        .addStorageWrite(histogram, PipelineDomain::Transfer)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            validationLog_.recordPass("auto exposure histogram clear");
            currentProfiler_->write(cmd, GpuProfiler::AutoExposureHistogramClearStart, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            vkCmdFillBuffer(cmd, histogramBuffer_.handle(), 0, histogramBuffer_.size(), 0);
            currentProfiler_->write(cmd, GpuProfiler::AutoExposureHistogramClearEnd, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        });
    graph.addPass("auto_exposure_histogram")
        .addStorageRead(denoised, PipelineDomain::Compute)
        .addStorageReadWrite(histogram, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordAutoExposureHistogramPass(cmd);
        });
    graph.addPass("auto_exposure_reduce")
        .addStorageRead(histogram, PipelineDomain::Compute)
        .addStorageWrite(exposure, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordAutoExposureReducePass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
}

void PathTracerRenderer::recordAutoExposureHistogramPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("auto exposure histogram");
    currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureHistogramStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    DescriptorSet histogramSet = currentFrame_->descriptors().allocate(luminanceHistogramSetLayout_);
    const Image& sourceImage = hdrPostProcessImage();
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sourceImage.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .writeBuffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, histogramBuffer_.descriptorInfo())
        .update(context_.device(), histogramSet);

    luminanceHistogramPipeline_->bind(commandBuffer);
    VkDescriptorSet descriptorSet = histogramSet.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, luminanceHistogramPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const HistogramParams histogramParams{
        .width = sourceImage.extent().width,
        .height = sourceImage.extent().height,
        .minLogLuminance = settings_.histogramMinLogLuminance,
        .maxLogLuminance = settings_.histogramMaxLogLuminance,
    };
    vkCmdPushConstants(
        commandBuffer,
        luminanceHistogramPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(histogramParams),
        &histogramParams);
    luminanceHistogramPipeline_->dispatch(commandBuffer, sourceImage.extent().width, sourceImage.extent().height);
    currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureHistogramEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordAutoExposureReducePass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("auto exposure reduce");
    currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureReduceStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    DescriptorSet exposureSet = currentFrame_->descriptors().allocate(exposureReduceSetLayout_);
    DescriptorWriter()
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, histogramBuffer_.descriptorInfo())
        .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, exposureBuffer_.descriptorInfo())
        .update(context_.device(), exposureSet);

    exposureReducePipeline_->bind(commandBuffer);
    VkDescriptorSet descriptorSet = exposureSet.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, exposureReducePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const ExposureReduceParams exposureParams{
        .pixelCount = hdrPostProcessImage().extent().width * hdrPostProcessImage().extent().height,
        .targetLuminance = settings_.targetLuminance,
        .minExposure = settings_.minExposure,
        .maxExposure = settings_.maxExposure,
        .adaptationSpeed = settings_.adaptationSpeed,
        .lowPercentile = settings_.histogramLowPercentile,
        .highPercentile = settings_.histogramHighPercentile,
        .targetPercentile = settings_.histogramTargetPercentile,
        .deltaSeconds = frameDeltaSeconds_,
        .minLogLuminance = settings_.histogramMinLogLuminance,
        .maxLogLuminance = settings_.histogramMaxLogLuminance,
    };
    vkCmdPushConstants(
        commandBuffer,
        exposureReducePipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(exposureParams),
        &exposureParams);
    exposureReducePipeline_->dispatch(commandBuffer, 1, 1, 1, 1);
    currentProfiler_->write(commandBuffer, GpuProfiler::AutoExposureReduceEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::recordToneMap(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    const Image& sourceImage = hdrPostProcessImage();
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = sourceImage.format(),
        .extent = sourceImage.extent(),
        .image = sourceImage.handle(),
        .imageRange = sourceImage.fullRange(),
        .external = true,
        .debugName = "post temporal hdr",
    });
    const RenderGraphResourceId presentation = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = presentationImage_.format(),
        .extent = presentationImage_.extent(),
        .image = presentationImage_.handle(),
        .imageRange = presentationImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : kCrossQueueShaderStage,
            .access = presentationImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = presentationImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = kCrossQueueShaderStage,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "presentation ldr",
    });
    const RenderGraphResourceId exposure = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = exposureBuffer_.size(),
        .buffer = exposureBuffer_.handle(),
        .external = true,
        .debugName = "exposure",
    });
    graph.addPass("tone_map")
        .addStorageRead(denoised, PipelineDomain::Compute)
        .addStorageRead(exposure, PipelineDomain::Compute)
        .addStorageWrite(presentation, PipelineDomain::Compute)
        .setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            recordToneMapPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PathTracerRenderer::recordToneMapPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("tone map compute");
    currentProfiler_->write(commandBuffer, GpuProfiler::ToneMapStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    presentationImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet toneMapSet = currentFrame_->descriptors().allocate(toneMapSetLayout_);
    const Image& sourceImage = hdrPostProcessImage();
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sourceImage.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .writeImage(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, presentationImage_.storageDescriptor())
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, exposureBuffer_.descriptorInfo())
        .update(context_.device(), toneMapSet);

    toneMapPipeline_->bind(commandBuffer);
    const VkDescriptorSet descriptorSet = toneMapSet.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, toneMapPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const float effectiveExposure = settings_.usePhysicalCamera
        ? 2.0f * std::exp2(14.0f - physicalCamera_.ev100())
        : settings_.exposure;
    const ToneMapParams toneMapParams{
        .toneMapper = static_cast<uint32_t>(settings_.toneMapper),
        .debugView = static_cast<uint32_t>(settings_.debugView),
        .autoExposureEnabled = settings_.autoExposureEnabled ? 1u : 0u,
        .exposure = effectiveExposure,
        .gamma = settings_.gamma,
        .contrast = settings_.contrast,
        .saturation = settings_.saturation,
        .brightness = settings_.brightness,
        .whitePoint = settings_.whitePoint,
    };
    vkCmdPushConstants(
        commandBuffer,
        toneMapPipeline_->layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(toneMapParams),
        &toneMapParams);
    toneMapPipeline_->dispatch(commandBuffer, displayExtent_.width, displayExtent_.height);
    currentProfiler_->write(commandBuffer, GpuProfiler::ToneMapEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracerRenderer::skipDenoiserPass(VkCommandBuffer commandBuffer) {
    RenderGraph graph(&allocator_, resourceAliasingEnabled_);
    const RenderGraphResourceId raw = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = rawImage_.format(),
        .extent = rawImage_.extent(),
        .image = rawImage_.handle(),
        .imageRange = rawImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        },
        .debugName = "raw hdr",
    });
    const RenderGraphResourceId denoised = graph.createTexture(RenderGraphResource{
        .type = RenderGraphResource::Type::Texture,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .format = denoisedImage_.format(),
        .extent = denoisedImage_.extent(),
        .image = denoisedImage_.handle(),
        .imageRange = denoisedImage_.fullRange(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : kCrossQueueShaderStage,
            .access = denoisedImage_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = denoisedImage_.layout(),
        },
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = kCrossQueueShaderStage,
            .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        .debugName = "denoised hdr",
    });
    const RenderGraphResourceId worldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = worldPositionBuffer_.size(),
        .buffer = worldPositionBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "world position",
    });
    const RenderGraphResourceId previousWorldPosition = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousWorldPositionBuffer_.size(),
        .buffer = previousWorldPositionBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous world position",
    });
    const RenderGraphResourceId restirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = restirReservoirBuffer_.size(),
        .buffer = restirReservoirBuffer_.handle(),
        .external = true,
        .hasInitialAccess = true,
        .initialAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        },
        .debugName = "restir reservoir",
    });
    const RenderGraphResourceId previousRestirReservoir = graph.createBuffer(RenderGraphResource{
        .type = RenderGraphResource::Type::Buffer,
        .lifetime = RenderGraphResource::Lifetime::Persistent,
        .size = previousRestirReservoirBuffer_.size(),
        .buffer = previousRestirReservoirBuffer_.handle(),
        .external = true,
        .hasFinalAccess = true,
        .finalAccess = ResourceAccess{
            .stage = pathTraceShaderStage(),
            .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        },
        .debugName = "previous restir reservoir",
    });
    const bool useRestirGiReservoirs = shouldUseRestirGiReservoirs();
    RenderGraphResourceId restirGiSpatialReservoir{};
    RenderGraphResourceId previousRestirGiReservoir{};
    if (useRestirGiReservoirs) {
        restirGiSpatialReservoir = graph.createBuffer(RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = restirGiSpatialReservoirBuffer_.size(),
            .buffer = restirGiSpatialReservoirBuffer_.handle(),
            .external = true,
            .hasInitialAccess = true,
            .initialAccess = ResourceAccess{
                .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            },
            .debugName = "restir gi spatial reservoir",
        });
        previousRestirGiReservoir = graph.createBuffer(RenderGraphResource{
            .type = RenderGraphResource::Type::Buffer,
            .lifetime = RenderGraphResource::Lifetime::Persistent,
            .size = previousRestirGiReservoirBuffer_.size(),
            .buffer = previousRestirGiReservoirBuffer_.handle(),
            .external = true,
            .hasFinalAccess = true,
            .finalAccess = ResourceAccess{
                .stage = pathTraceShaderStage(),
                .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            },
            .debugName = "previous restir gi reservoir",
        });
    }
    RenderGraphPass& skipDenoiserCopyGraphPass = graph.addPass("skip_denoiser_copy")
        .addStorageRead(raw, PipelineDomain::Transfer)
        .addStorageWrite(denoised, PipelineDomain::Transfer)
        .addStorageRead(worldPosition, PipelineDomain::Transfer)
        .addStorageWrite(previousWorldPosition, PipelineDomain::Transfer)
        .addStorageRead(restirReservoir, PipelineDomain::Transfer)
        .addStorageWrite(previousRestirReservoir, PipelineDomain::Transfer);
    if (useRestirGiReservoirs) {
        skipDenoiserCopyGraphPass
            .addStorageRead(restirGiSpatialReservoir, PipelineDomain::Transfer)
            .addStorageWrite(previousRestirGiReservoir, PipelineDomain::Transfer);
    }
    skipDenoiserCopyGraphPass.setExecuteCallback([this](FrameGraphContext&, VkCommandBuffer cmd) {
            skipDenoiserCopyPass(cmd);
        });
    graph.compile();
    graph.execute(commandBuffer, temporalFrameIndex_);
    rawImage_.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PathTracerRenderer::skipDenoiserCopyPass(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass(adaptiveSkipDenoiser_ ? "adaptive skip denoiser copy" : "skip denoiser copy");
    currentProfiler_->write(commandBuffer, GpuProfiler::SkipDenoiserCopyStart, VK_PIPELINE_STAGE_2_COPY_BIT);
    rawImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    denoisedImage_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = rawImage_.extent();
    vkCmdCopyImage(
        commandBuffer,
        rawImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        denoisedImage_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);

    VkBufferCopy worldCopy{};
    worldCopy.size = worldPositionBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, worldPositionBuffer_.handle(), previousWorldPositionBuffer_.handle(), 1, &worldCopy);
    VkBufferCopy restirCopy{};
    restirCopy.size = restirReservoirBuffer_.size();
    vkCmdCopyBuffer(commandBuffer, restirReservoirBuffer_.handle(), previousRestirReservoirBuffer_.handle(), 1, &restirCopy);
    if (shouldUseRestirGiReservoirs()) {
        VkBufferCopy restirGiCopy{};
        restirGiCopy.size = restirGiSpatialReservoirBuffer_.size();
        vkCmdCopyBuffer(commandBuffer, restirGiSpatialReservoirBuffer_.handle(), previousRestirGiReservoirBuffer_.handle(), 1, &restirGiCopy);
        restirGiHistoryValid_ = true;
    }
    if (temporalSystem_) {
        temporalSystem_->markSlotWritten("previous_world_position");
        temporalSystem_->markSlotWritten("restir_reservoir");
    }
    if (isNonDenoiserDebugView()) {
        denoiserHistoryValid_ = true;
    } else {
        denoiserHistoryValid_ = false;
    }
    currentProfiler_->write(commandBuffer, GpuProfiler::SkipDenoiserCopyEnd, VK_PIPELINE_STAGE_2_COPY_BIT);
}

void PathTracerRenderer::recordFullscreen(VkCommandBuffer commandBuffer, VkExtent2D swapchainExtent) {
    validationLog_.recordPass("fullscreen presentation");
    currentProfiler_->write(commandBuffer, GpuProfiler::FullscreenStart, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    DescriptorSet set = currentFrame_->descriptors().allocate(graphicsSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, presentationImage_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = fullscreenSampler_})
        .update(context_.device(), set);

    graphicsPipeline_->bind(commandBuffer, swapchainExtent);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    currentProfiler_->write(commandBuffer, GpuProfiler::FullscreenEnd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
}

void PathTracerRenderer::recordEditorPresentationStart(VkCommandBuffer commandBuffer) {
    validationLog_.recordPass("editor viewport presentation");
    currentProfiler_->write(commandBuffer, GpuProfiler::EditorPresentationStart, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
}

void PathTracerRenderer::recordEditorPresentationEnd(VkCommandBuffer commandBuffer) {
    currentProfiler_->write(commandBuffer, GpuProfiler::EditorPresentationEnd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
}

VkDeviceSize PathTracerRenderer::estimatedTextureMemory() const {
    VkDeviceSize total = 0;
    auto texSize = [](const Image& img) {
        if (img.handle() == VK_NULL_HANDLE) return VkDeviceSize(0);
        uint32_t bpp = 4;
        switch (img.format()) {
        case VK_FORMAT_R16G16_SFLOAT: bpp = 4; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
        case VK_FORMAT_R16G16B16A16_UINT: bpp = 8; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: bpp = 16; break;
        default: break;
        }
        return VkDeviceSize(img.width()) * img.height() * bpp;
    };
    total += texSize(rawImage_);
    total += texSize(denoisedImage_);
    total += texSize(historyImage_);
    total += texSize(diffuseResolvedImage_);
    total += texSize(specularResolvedImage_);
    total += texSize(diffuseHistoryImage_);
    total += texSize(specularHistoryImage_);
    total += texSize(directDiffuseMomentsImage_);
    total += texSize(directSpecularMomentsImage_);
    total += texSize(indirectDiffuseMomentsImage_);
    total += texSize(indirectSpecularMomentsImage_);
    total += texSize(historyLengthImage_);
    total += texSize(directDiffuseResolvedMomentsImage_);
    total += texSize(directSpecularResolvedMomentsImage_);
    total += texSize(indirectDiffuseResolvedMomentsImage_);
    total += texSize(indirectSpecularResolvedMomentsImage_);
    total += texSize(historyLengthResolvedImage_);
    total += texSize(momentDebugImage_);
    total += texSize(momentDebugResolvedImage_);
    total += texSize(taaImage_);
    total += texSize(taaHistoryImage_);
    total += texSize(presentationImage_);
    return total;
}

VkDeviceSize PathTracerRenderer::estimatedBufferMemory() const {
    VkDeviceSize total = 0;
    total += accumulationBuffer_.size();
    total += varianceBuffer_.size();
    total += depthNormalBuffer_.size();
    total += worldPositionBuffer_.size();
    total += previousWorldPositionBuffer_.size();
    total += velocityBuffer_.size();
    total += entityIdBuffer_.size();
    total += pathDataBuffer_.size();
    total += rayTracingDiagnosticCountersBuffer_.size();
    total += rayTracingDiagnosticCountersReadbackBuffer_.size();
    for (const auto& frame : frames_) {
        if (frame != nullptr) {
            total += frame->wavefrontTransientCapacityBytes();
        }
    }
    total += wavefrontQueueHeaderReadbackBuffer_.size();
    total += wavefrontRaySampleReadbackBuffer_.size();
    total += wavefrontTraceValidationBuffer_.size();
    total += wavefrontTraceValidationReadbackBuffer_.size();
    total += wavefrontShadeValidationBuffer_.size();
    total += wavefrontShadeValidationReadbackBuffer_.size();
    total += wavefrontSecondaryShadeValidationBuffer_.size();
    total += wavefrontSecondaryShadeValidationReadbackBuffer_.size();
    total += wavefrontSortedShadeValidationBuffer_.size();
    total += wavefrontSortedShadeValidationReadbackBuffer_.size();
    total += wavefrontCompactValidationBuffer_.size();
    total += wavefrontCompactValidationReadbackBuffer_.size();
    total += wavefrontSortValidationBuffer_.size();
    total += wavefrontSortValidationReadbackBuffer_.size();
    total += wavefrontShadowTraceValidationBuffer_.size();
    total += wavefrontShadowTraceValidationReadbackBuffer_.size();
    return total;
}

VkDeviceSize PathTracerRenderer::temporalHistoryMemory() const {
    if (temporalSystem_) {
        return temporalSystem_->totalHistoryMemoryBytes();
    }
    return 0;
}

VkDeviceSize PathTracerRenderer::restirReservoirMemory() const {
    VkDeviceSize total = 0;
    std::array<VkBuffer, 8> seen{};
    uint32_t seenCount = 0;
    auto addUnique = [&](const Buffer& buffer) {
        if (buffer.handle() == VK_NULL_HANDLE) {
            return;
        }
        for (uint32_t i = 0; i < seenCount; ++i) {
            if (seen[i] == buffer.handle()) {
                return;
            }
        }
        seen[seenCount++] = buffer.handle();
        total += buffer.size();
    };
    addUnique(restirReservoirBuffer_);
    addUnique(wavefrontRestirReservoirBuffer_);
    addUnique(previousRestirReservoirBuffer_);
    addUnique(restirSpatialReservoirBuffer_);
    addUnique(restirGiReservoirBuffer_);
    addUnique(previousRestirGiReservoirBuffer_);
    addUnique(restirGiSpatialReservoirBuffer_);
    addUnique(wavefrontRestirGiReservoirBuffer_);
    return total;
}

PathTracerRenderer::RestirReservoirMemoryBreakdown PathTracerRenderer::restirReservoirMemoryBreakdown() const {
    return RestirReservoirMemoryBreakdown{
        .diCurrentBytes = restirReservoirBuffer_.size(),
        .diPreviousBytes = previousRestirReservoirBuffer_.size(),
        .diSpatialBytes = restirSpatialReservoirBuffer_.size(),
        .giCurrentBytes = restirGiReservoirBuffer_.size(),
        .giPreviousBytes = previousRestirGiReservoirBuffer_.size(),
        .giSpatialBytes = restirGiSpatialReservoirBuffer_.size(),
    };
}

DescriptorAllocator::Stats PathTracerRenderer::descriptorPoolStats() const {
    DescriptorAllocator::Stats total{};
    for (const auto& frame : frames_) {
        if (!frame) {
            continue;
        }
        const DescriptorAllocator::Stats stats = frame->descriptors().stats();
        total.setsPerPool = std::max(total.setsPerPool, stats.setsPerPool);
        total.maxPools += stats.maxPools;
        total.usedPools += stats.usedPools;
        total.freePools += stats.freePools;
        total.poolCount += stats.poolCount;
        total.capacitySets += stats.capacitySets;
        total.allocatedSets += stats.allocatedSets;
        total.peakAllocatedSets += stats.peakAllocatedSets;
        total.failedAllocations += stats.failedAllocations;
        total.fragmentedPoolFailures += stats.fragmentedPoolFailures;
        total.poolGrowthCount += stats.poolGrowthCount;
    }
    return total;
}

} // namespace rtv
