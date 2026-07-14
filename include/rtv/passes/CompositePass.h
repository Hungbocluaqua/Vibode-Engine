#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

namespace rtv::passes {

struct CompositePass {
    static constexpr const char* kContractId = "composite";
    static constexpr const char* kPassName = "CompositePass";
    static constexpr const char* kRole = "output";
    static constexpr const char* kExtractionState =
        "contract-module plus output-source selection policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    enum class HdrPostProcessSource {
        AdaptiveFilled,
        AdaptiveDebug,
        TemporalOutput,
        PostDenoise,
    };

    static bool isActive(const RendererSettings&) {
        return true;
    }

    static HdrPostProcessSource selectHdrPostProcessSource(
        const RendererSettings& settings,
        bool adaptiveFilledReady,
        bool adaptiveDebugReady,
        bool temporalOutputReady) {
        if (settings.debugView == RendererDebugView::AdaptiveFilledImage && adaptiveFilledReady) {
            return HdrPostProcessSource::AdaptiveFilled;
        }
        if (adaptiveDebugReady) {
            return HdrPostProcessSource::AdaptiveDebug;
        }
        return temporalOutputReady
            ? HdrPostProcessSource::TemporalOutput
            : HdrPostProcessSource::PostDenoise;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::Composite;
        contract.role = RendererPassContractRole::Output;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/CompositePass.h (contract + output-source policy), src/rtv/PathTracerRenderer.cpp (GPU resource runtime)";
        contract.featureFlagsRequired = "always";
        contract.inputs = rendererContractArray({"beauty source", "exposure", "debug view output", "selection outline"});
        contract.outputs = rendererContractArray({"presentable image", "saved present frame"});
        contract.historyResources = rendererContractArray({"auto exposure history"});
        contract.descriptorLayouts = rendererContractArray({"tone map descriptor set", "fullscreen descriptor set"});
        contract.pushConstants = rendererContractArray({"ToneMapParams", "RendererDebugParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"tone mapping compute shader", "fullscreen/editor presentation shaders"});
        contract.rendergraphReads = rendererContractArray({"denoised/TAA/DLSS/debug source", "exposure buffer"});
        contract.rendergraphWrites = rendererContractArray({"presentation image", "swapchain image"});
        contract.requiredBarriers = rendererContractArray({"source to tone map", "tone map to present"});
        contract.cameraHistoryResetBehavior = "Does not own temporal history; consumes the selected current-frame output.";
        contract.debugOutputs = rendererContractArray({"beauty", "debug view selected by RendererDebugView"});
        contract.profilingSections = rendererContractArray({"tone_map", "fullscreen", "editor_presentation"});
        contract.validationChecks = rendererContractArray({"nonblank present PNG", "image diff output", "swapchain extent validity"});
        return contract;
    }
};

} // namespace rtv::passes
