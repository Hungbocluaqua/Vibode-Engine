#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

namespace rtv::passes {

struct NrdPass {
    static constexpr const char* kContractId = "nrd";
    static constexpr const char* kPassName = "NrdPass";
    static constexpr const char* kRole = "denoising";
    static constexpr const char* kExtractionState =
        "contract-module plus debug-view compatibility policy; scheduling and SDK resources remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings& settings) {
        return settings.denoiserEnabled && settings.denoiserBackend == DenoiserBackend::Nrd;
    }

    static constexpr bool supportsDebugView(RendererDebugView view) {
        return view == RendererDebugView::Beauty ||
            view == RendererDebugView::NrdValidation ||
            view == RendererDebugView::NrdDiffuseConfidence ||
            view == RendererDebugView::NrdSpecularConfidence ||
            view == RendererDebugView::NrdRawConfidenceGradient ||
            view == RendererDebugView::NrdFilteredConfidenceGradient ||
            view == RendererDebugView::NrdConfidenceHistory;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::Nrd;
        contract.role = RendererPassContractRole::Denoising;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/NrdPass.h (contract + debug-view policy), src/rtv/PathTracerRenderer.cpp (SDK resources/scheduling)";
        contract.featureFlagsRequired = "denoiserEnabled && denoiserBackend == nrd";
        contract.inputs = rendererContractArray({"depth", "normal/roughness", "motion", "hit distance", "diffuse/specular radiance", "confidence"});
        contract.outputs = rendererContractArray({"NRD denoised diffuse", "NRD denoised specular", "NRD validation image"});
        contract.historyResources = rendererContractArray({"NRD runtime internal history", "NRD confidence history"});
        contract.descriptorLayouts = rendererContractArray({"NRD guide descriptor set", "external SDK resource tags"});
        contract.pushConstants = rendererContractArray({"NrdPrepareParams", "NrdResolveParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/nrd_prepare.comp", "shaders/nrd_resolve.comp", "shaders/nrd_confidence_filter.comp"});
        contract.rendergraphReads = rendererContractArray({"guide buffers", "raw radiance", "confidence history"});
        contract.rendergraphWrites = rendererContractArray({"nrd guide images", "nrd output images", "nrd validation output"});
        contract.requiredBarriers = rendererContractArray({"guide preparation before SDK evaluate", "SDK output before composite/TAA"});
        contract.cameraHistoryResetBehavior = "Reset SDK history on camera cut, invalid guides, denoiser backend change, resolution/render-scale change, or DLSS/NRD mode transition.";
        contract.debugOutputs = rendererContractArray({"nrd-validation", "nrd-diffuse-confidence", "nrd-specular-confidence", "nrd-confidence-history"});
        contract.profilingSections = rendererContractArray({"nrd_prepare", "nrd_resolve"});
        contract.validationChecks = rendererContractArray({"guide availability", "normal length", "roughness range", "motion convention", "SDK availability"});
        return contract;
    }
};

} // namespace rtv::passes
