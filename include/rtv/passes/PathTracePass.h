#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace rtv::passes {

struct PathTracePass {
    static constexpr const char* kContractId = "path_trace";
    static constexpr const char* kPassName = "PathTracePass";
    static constexpr const char* kRole = "geometry";
    static constexpr const char* kExtractionState =
        "contract-module plus kernel/beauty-fast-path policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings& settings) {
        return settings.pathTracingEnabled;
    }

    static bool effectiveLimitSamplesPerPixel(const RendererSettings& settings, uint32_t memoryPressureTier) {
        return settings.limitSamplesPerPixel || memoryPressureTier > 0u;
    }

    static uint32_t wavefrontMaxPathDepth(uint32_t adaptiveEffectiveMaxBounces) {
        return std::max(1u, adaptiveEffectiveMaxBounces);
    }

    static uint32_t wavefrontQueueCapacityFor(
        uint64_t pixelCount,
        bool wavefrontShadeEnabled,
        uint32_t maxPathDepth) {
        if (pixelCount == 0u) {
            return 1u;
        }
        const uint64_t multiplier = wavefrontShadeEnabled
            ? static_cast<uint64_t>(std::max(1u, maxPathDepth))
            : 1ull;
        const uint64_t capacity = pixelCount > std::numeric_limits<uint64_t>::max() / multiplier
            ? static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
            : pixelCount * multiplier;
        return capacity > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(capacity);
    }

    static bool native2BSettingsEligible(
        const RendererSettings& settings,
        bool effectiveLimitSamplesPerPixel,
        bool regirActive) {
        return settings.pathTraceKernelMode == PathTraceKernelMode::Native2B &&
            settings.pathTracingEnabled &&
            settings.maxBounces == 2u &&
            settings.samplesPerPixel == 1u &&
            effectiveLimitSamplesPerPixel &&
            settings.restirGiMode == RestirGiMode::Off &&
            !settings.restirGiEnabled &&
            !settings.homogeneousVolumeEnabled &&
            !settings.motionBlurEnabled &&
            !regirActive &&
            settings.debugView == RendererDebugView::Beauty &&
            !settings.wavefrontFinalOutputEnabled;
    }

    static const char* native2BSettingsFallbackReason(
        const RendererSettings& settings,
        bool effectiveLimitSamplesPerPixel,
        bool regirActive) {
        if (settings.pathTraceKernelMode != PathTraceKernelMode::Native2B) return nullptr;
        if (!settings.pathTracingEnabled) return "path_tracing_disabled";
        if (settings.maxBounces != 2u) return "requires_exactly_two_bounces";
        if (settings.samplesPerPixel != 1u || !effectiveLimitSamplesPerPixel) return "requires_effective_one_spp";
        if (settings.restirGiMode != RestirGiMode::Off || settings.restirGiEnabled) return "restir_gi_enabled";
        if (settings.homogeneousVolumeEnabled) return "volume_enabled";
        if (settings.motionBlurEnabled) return "motion_blur_enabled";
        if (regirActive) return "regir_enabled";
        if (settings.debugView != RendererDebugView::Beauty) return "debug_view_active";
        if (settings.wavefrontFinalOutputEnabled) return "wavefront_final_output_enabled";
        return nullptr;
    }

    static bool requestsGenericBeautyFastPath(
        const RendererSettings& settings,
        PathTraceKernelMode effectiveKernelMode,
        bool restirDiValidationFull,
        bool restirGiInitialFull,
        bool diagnosticCountersEnabled,
        bool regirActive) {
        return effectiveKernelMode == PathTraceKernelMode::Generic &&
            settings.debugView == RendererDebugView::Beauty &&
            settings.pathTracingEnabled &&
            !settings.motionBlurEnabled &&
            !settings.wavefrontFinalOutputEnabled &&
            !diagnosticCountersEnabled &&
            !restirDiValidationFull &&
            !restirGiInitialFull &&
            !regirActive;
    }

    static bool requestsRegirBeautyFastPath(
        const RendererSettings& settings,
        PathTraceKernelMode effectiveKernelMode,
        bool restirDiValidationFull,
        bool restirGiInitialFull,
        bool diagnosticCountersEnabled,
        bool regirActive) {
        return effectiveKernelMode == PathTraceKernelMode::Generic &&
            settings.finalBounceFastPathEnabled &&
            settings.debugView == RendererDebugView::Beauty &&
            settings.pathTracingEnabled &&
            !settings.motionBlurEnabled &&
            !settings.wavefrontFinalOutputEnabled &&
            !diagnosticCountersEnabled &&
            !restirDiValidationFull &&
            !restirGiInitialFull &&
            regirActive;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::PathTrace;
        contract.role = RendererPassContractRole::Geometry;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/PathTracePass.h (contract + kernel/fast-path/effective-limit/wavefront-capacity policy), src/rtv/PathTracerRenderer.cpp (resources/scheduling)";
        contract.featureFlagsRequired = "pathTracingEnabled";
        contract.inputs = rendererContractArray({"GBufferPass surface state", "scene lights", "environment", "ReSTIR/ReGIR sampling services"});
        contract.outputs = rendererContractArray({"raw radiance", "direct/indirect channels", "hit distance", "throughput/bounce debug data"});
        contract.historyResources = rendererContractArray({"accumulation buffer", "previous frame radiance when accumulation is valid"});
        contract.descriptorLayouts = rendererContractArray({"ray tracing descriptor set", "bindless textures", "reservoir descriptor sets"});
        contract.pushConstants = rendererContractArray({"RendererDebugParams", "CameraUniform", "raygen extent"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/pathtrace.rgen", "shaders/pathtrace_integrator.glsl", "shaders/pathtrace_lighting.glsl"});
        contract.rendergraphReads = rendererContractArray({"TLAS", "light buffers", "ReSTIR DI final reservoir", "ReGIR reservoirs"});
        contract.rendergraphWrites = rendererContractArray({"radiance image", "path data buffers", "debug counters"});
        contract.requiredBarriers = rendererContractArray({"accumulation read/write", "ray tracing writes to denoiser/tonemap reads"});
        contract.cameraHistoryResetBehavior = "Reset accumulation on camera movement, render setting changes, shader reload, scene/material/light changes, and temporal reset reasons.";
        contract.debugOutputs = rendererContractArray({"beauty", "direct-lighting", "indirect-lighting", "path-direct-diffuse", "path-indirect-specular", "bounce-count"});
        contract.profilingSections = rendererContractArray({"path_trace"});
        contract.validationChecks = rendererContractArray({"NaN/Inf counters", "ray count statistics", "image diff against high-spp references"});
        return contract;
    }
};

} // namespace rtv::passes
