#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

namespace rtv::passes {

struct TemporalAAPass {
    static constexpr const char* kContractId = "temporal_aa";
    static constexpr const char* kPassName = "TemporalAAPass";
    static constexpr const char* kRole = "temporal";
    static constexpr const char* kExtractionState =
        "contract-module plus temporal resolve run policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings& settings) {
        return settings.taaEnabled &&
            (settings.temporalUpscaler == TemporalUpscaler::TaaTsr ||
                settings.temporalUpscaler == TemporalUpscaler::Nis);
    }

    static constexpr bool supportsTemporalResolve(TemporalUpscaler effectiveUpscaler) {
        return effectiveUpscaler == TemporalUpscaler::TaaTsr ||
            effectiveUpscaler == TemporalUpscaler::Nis;
    }

    static bool requestsRun(
        const RendererSettings& settings,
        bool bypassForDebugView,
        TemporalUpscaler effectiveUpscaler) {
        return !settings.wavefrontFinalOutputEnabled &&
            settings.pathTracingEnabled &&
            settings.taaEnabled &&
            !bypassForDebugView &&
            supportsTemporalResolve(effectiveUpscaler);
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::TemporalAA;
        contract.role = RendererPassContractRole::Temporal;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/TemporalAAPass.h (contract + run policy), src/rtv/PathTracerRenderer.cpp (resources/scheduling)";
        contract.featureFlagsRequired = "taaEnabled && temporalUpscaler in {taa-tsr, nis}";
        contract.inputs = rendererContractArray({"current color", "motion vectors", "depth", "reactive mask", "previous TAA history"});
        contract.outputs = rendererContractArray({"TAA/TSR color", "history weight", "reprojection confidence"});
        contract.historyResources = rendererContractArray({"taa history image"});
        contract.descriptorLayouts = rendererContractArray({"TAA descriptor set"});
        contract.pushConstants = rendererContractArray({"TaaParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/taa.comp", "shaders/temporal_common.glsl"});
        contract.rendergraphReads = rendererContractArray({"denoised/raw color", "motion vectors", "depth", "taa history"});
        contract.rendergraphWrites = rendererContractArray({"taa output", "taa history copy"});
        contract.requiredBarriers = rendererContractArray({"denoiser to TAA", "TAA to history copy", "TAA to tonemap"});
        contract.cameraHistoryResetBehavior = "Reset on camera cut, resolution/render-scale change, jitter reset, scene/material/light reload, or explicit accumulation reset.";
        contract.debugOutputs = rendererContractArray({"motion-vectors", "reprojection-confidence", "temporal-history-weight", "temporal-reactive-mask"});
        contract.profilingSections = rendererContractArray({"taa", "taa_history_copy"});
        contract.validationChecks = rendererContractArray({"static camera convergence", "slow pan preservation", "camera cut rejection", "disocclusion rejection"});
        return contract;
    }
};

} // namespace rtv::passes
