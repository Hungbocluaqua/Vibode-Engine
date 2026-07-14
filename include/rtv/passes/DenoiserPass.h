#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

#include <algorithm>
#include <cstdint>

namespace rtv::passes {

struct DenoiserPass {
    static constexpr const char* kContractId = "denoiser";
    static constexpr const char* kPassName = "DenoiserPass";
    static constexpr const char* kRole = "denoising";
    static constexpr const char* kExtractionState =
        "contract-module plus engine-denoiser run/debug policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings& settings) {
        return settings.denoiserEnabled && settings.denoiserBackend == DenoiserBackend::Engine;
    }

    static uint32_t effectiveMaxHistoryLength(const RendererSettings& settings, uint32_t memoryPressureTier) {
        if (memoryPressureTier >= 3u) {
            return std::min(settings.denoiserMaxHistoryLength, 16u);
        }
        if (memoryPressureTier >= 2u) {
            return std::min(settings.denoiserMaxHistoryLength, 24u);
        }
        if (memoryPressureTier >= 1u) {
            return std::min(settings.denoiserMaxHistoryLength, 32u);
        }
        return settings.denoiserMaxHistoryLength;
    }

    static constexpr bool requestsDebugOutput(uint32_t debugView) {
        if (debugView >= 1u && debugView <= 4u) {
            return true;
        }
        if (debugView == static_cast<uint32_t>(RendererDebugView::TemporalReactiveMask) ||
            debugView == static_cast<uint32_t>(RendererDebugView::TemporalHistoryWeight)) {
            return true;
        }
        if (debugView >= static_cast<uint32_t>(RendererDebugView::PathDirectDiffuse) &&
            debugView <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularHistoryWeight)) {
            return true;
        }
        if (debugView >= static_cast<uint32_t>(RendererDebugView::DenoiserDirectDiffuseVariance) &&
            debugView <= static_cast<uint32_t>(RendererDebugView::DenoiserSpecularRawVariance)) {
            return true;
        }
        return false;
    }

    static bool requestsRun(
        const RendererSettings& settings,
        DenoiserBackend effectiveBackend,
        uint32_t enabledParam,
        uint32_t debugViewParam) {
        if (settings.wavefrontFinalOutputEnabled) {
            return false;
        }
        const bool engineDebugView = debugViewParam != 0u;
        if (effectiveBackend != DenoiserBackend::Engine && !engineDebugView) {
            return false;
        }
        return enabledParam != 0u || requestsDebugOutput(debugViewParam);
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::Denoiser;
        contract.role = RendererPassContractRole::Denoising;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/DenoiserPass.h (contract + run/debug/history-length policy), src/rtv/PathTracerRenderer.cpp (resources/scheduling)";
        contract.featureFlagsRequired = "denoiserEnabled && denoiserBackend == engine";
        contract.inputs = rendererContractArray({"raw radiance", "albedo", "normal", "depth", "hit distance", "moments"});
        contract.outputs = rendererContractArray({"denoised color", "variance/confidence", "moments"});
        contract.historyResources = rendererContractArray({"denoiser history", "moment history"});
        contract.descriptorLayouts = rendererContractArray({"denoiser descriptor set", "moment descriptor set"});
        contract.pushConstants = rendererContractArray({"DenoiserParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/denoiser.comp", "shaders/moment_update.comp"});
        contract.rendergraphReads = rendererContractArray({"path trace output", "guide buffers", "denoiser history"});
        contract.rendergraphWrites = rendererContractArray({"denoised image", "moment buffers", "history images"});
        contract.requiredBarriers = rendererContractArray({"path trace to moment update", "moment update to denoiser", "denoiser to TAA/tonemap"});
        contract.cameraHistoryResetBehavior = "Reset on camera cut, invalid guides, disocclusion spike, denoiser setting changes, scene/material reload, or render extent changes.";
        contract.debugOutputs = rendererContractArray({"denoiser-rejection", "denoiser-hit-distance", "denoiser-variance-confidence", "moment-update-validity"});
        contract.profilingSections = rendererContractArray({"moment_update", "denoiser", "history_copy"});
        contract.validationChecks = rendererContractArray({"NaN/Inf guide checks", "history reset count", "variance range", "moment validity"});
        return contract;
    }
};

} // namespace rtv::passes
