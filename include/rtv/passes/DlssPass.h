#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

#include <cstdint>

namespace rtv::passes {

struct DlssPass {
    static constexpr const char* kContractId = "dlss";
    static constexpr const char* kPassName = "DlssPass";
    static constexpr const char* kRole = "upscaling";
    static constexpr const char* kExtractionState =
        "contract-module plus request/debug-view/run runtime policy; scheduling and SDK resources remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings& settings) {
        return settings.temporalUpscaler == TemporalUpscaler::Dlss || settings.dlssRayReconstructionEnabled;
    }

    static bool isUpscaleActive(const RendererSettings& settings) {
        return settings.temporalUpscaler == TemporalUpscaler::Dlss;
    }

    static bool isRayReconstructionActive(const RendererSettings& settings) {
        return settings.dlssRayReconstructionEnabled;
    }

    static bool isRequested(const RendererSettings& settings) {
        return settings.temporalUpscaler == TemporalUpscaler::Dlss ||
            settings.dlssRayReconstructionEnabled ||
            settings.dlssFrameGenerationEnabled;
    }

    static constexpr bool isUpscaleDebugView(RendererDebugView view) {
        return view == RendererDebugView::DlssDepth ||
            view == RendererDebugView::DlssMotionVectors ||
            view == RendererDebugView::DlssInputColor ||
            view == RendererDebugView::DlssOutputColor;
    }

    static constexpr bool isRayReconstructionDebugView(RendererDebugView view) {
        return view == RendererDebugView::DlssRrDiffuseAlbedo ||
            view == RendererDebugView::DlssRrSpecularAlbedo ||
            view == RendererDebugView::DlssRrNormals ||
            view == RendererDebugView::DlssRrRoughness ||
            view == RendererDebugView::DlssRrDiffuseHitDistance ||
            view == RendererDebugView::DlssRrSpecularHitDistance ||
            view == RendererDebugView::DlssRrReflectedAlbedo ||
            view == RendererDebugView::DlssRrDisocclusionMask ||
            view == RendererDebugView::DlssRrDiffuseRayDirection ||
            view == RendererDebugView::DlssRrSpecularRayDirection ||
            view == RendererDebugView::DlssRrDiffuseRayDirectionHitDistance ||
            view == RendererDebugView::DlssRrSpecularRayDirectionHitDistance;
    }

    static constexpr bool isGuideDebugView(RendererDebugView view) {
        return isUpscaleDebugView(view) || isRayReconstructionDebugView(view);
    }

    static bool shouldBypassTemporalUpscalerForDebugView(const RendererSettings& settings) {
        if (settings.debugView == RendererDebugView::Beauty || isUpscaleDebugView(settings.debugView)) {
            return false;
        }
        if (isRayReconstructionDebugView(settings.debugView)) {
            return !settings.dlssRayReconstructionEnabled;
        }
        return true;
    }

    static constexpr uint32_t guideVisualizationMode(RendererDebugView view, bool rayReconstructionActive) {
        constexpr uint32_t kHdrColor = 0u;
        constexpr uint32_t kLinearColor = 1u;
        constexpr uint32_t kScalar01 = 2u;
        constexpr uint32_t kLinearDepth = 3u;
        constexpr uint32_t kHardwareDepth = 4u;
        constexpr uint32_t kMotionPixels = 5u;
        constexpr uint32_t kDirection = 6u;
        constexpr uint32_t kDistance = 7u;
        switch (view) {
        case RendererDebugView::DlssInputColor:
        case RendererDebugView::DlssOutputColor:
            return kHdrColor;
        case RendererDebugView::DlssDepth:
            return rayReconstructionActive ? kLinearDepth : kHardwareDepth;
        case RendererDebugView::DlssMotionVectors:
            return kMotionPixels;
        case RendererDebugView::DlssRrDiffuseAlbedo:
        case RendererDebugView::DlssRrSpecularAlbedo:
        case RendererDebugView::DlssRrReflectedAlbedo:
            return kLinearColor;
        case RendererDebugView::DlssRrNormals:
        case RendererDebugView::DlssRrDiffuseRayDirection:
        case RendererDebugView::DlssRrSpecularRayDirection:
            return kDirection;
        case RendererDebugView::DlssRrRoughness:
        case RendererDebugView::DlssRrDisocclusionMask:
            return kScalar01;
        case RendererDebugView::DlssRrDiffuseHitDistance:
        case RendererDebugView::DlssRrSpecularHitDistance:
        case RendererDebugView::DlssRrDiffuseRayDirectionHitDistance:
        case RendererDebugView::DlssRrSpecularRayDirectionHitDistance:
            return kDistance;
        default:
            return kHdrColor;
        }
    }

    static constexpr float guideVisualizationScale(RendererDebugView view, bool rayReconstructionActive) {
        switch (view) {
        case RendererDebugView::DlssMotionVectors:
            return 1.0f / 32.0f;
        case RendererDebugView::DlssDepth:
            return rayReconstructionActive ? 0.025f : 1.0f;
        case RendererDebugView::DlssRrDiffuseHitDistance:
        case RendererDebugView::DlssRrSpecularHitDistance:
        case RendererDebugView::DlssRrDiffuseRayDirectionHitDistance:
        case RendererDebugView::DlssRrSpecularRayDirectionHitDistance:
            return 0.05f;
        default:
            return 1.0f;
        }
    }

    static bool requestsUpscaleRun(
        const RendererSettings& settings,
        bool bypassForDebugView,
        bool rayReconstructionRunning,
        TemporalUpscaler effectiveUpscaler) {
        return !settings.wavefrontFinalOutputEnabled &&
            settings.pathTracingEnabled &&
            settings.taaEnabled &&
            !bypassForDebugView &&
            !rayReconstructionRunning &&
            effectiveUpscaler == TemporalUpscaler::Dlss;
    }

    static bool requestsRayReconstructionRun(
        const RendererSettings& settings,
        bool bypassForDebugView,
        bool runtimeSupported) {
        return !settings.wavefrontFinalOutputEnabled &&
            settings.pathTracingEnabled &&
            settings.taaEnabled &&
            !bypassForDebugView &&
            isRayReconstructionActive(settings) &&
            runtimeSupported;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::Dlss;
        contract.role = RendererPassContractRole::Upscaling;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/DlssPass.h (contract + request/debug/guide/run policy), src/rtv/PathTracerRenderer.cpp (SDK resources/scheduling)";
        contract.featureFlagsRequired = "temporalUpscaler == dlss || dlssRayReconstructionEnabled";
        contract.inputs = rendererContractArray({"input color", "depth", "motion vectors", "exposure", "DLSS RR guide buffers"});
        contract.outputs = rendererContractArray({"DLSS output color", "DLSS RR output color", "guide visualization"});
        contract.historyResources = rendererContractArray({"Streamline/NGX feature state", "PSR guide signatures"});
        contract.descriptorLayouts = rendererContractArray({"DLSS guide descriptor set", "external SDK resource tags"});
        contract.pushConstants = rendererContractArray({"DlssGuideParams", "Streamline/NGX constants"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/dlss_rr_guides.comp", "shaders/dlss_guide_visualize.comp", "shaders/psr_guides.glsl"});
        contract.rendergraphReads = rendererContractArray({"denoised/raw input color", "depth", "motion", "normal/roughness", "hit distance"});
        contract.rendergraphWrites = rendererContractArray({"dlss guide images", "dlss output", "psr guide signatures"});
        contract.requiredBarriers = rendererContractArray({"guide generation before SDK evaluate", "SDK output before composite"});
        contract.cameraHistoryResetBehavior = "Reset SDK history on camera cut, DLSS mode change, invalid guide state, render scale change, or resolution change.";
        contract.debugOutputs = rendererContractArray({"dlss-depth", "dlss-motion-vectors", "dlss-input-color", "dlss-output-color", "dlss-rr-normals", "dlss-rr-roughness"});
        contract.profilingSections = nlohmann::json::array();
        if (isRayReconstructionActive(settings)) {
            contract.profilingSections.push_back("dlss_rr_guides");
            contract.profilingSections.push_back("dlss_rr");
        } else if (isUpscaleActive(settings)) {
            contract.profilingSections.push_back("dlss_guides");
            contract.profilingSections.push_back("dlss");
        } else {
            contract.profilingSections.push_back("DLSS inactive external SDK timings");
        }
        contract.validationChecks = rendererContractArray({"SDK availability", "tag count", "evaluation failure count", "guide image format/extent"});
        return contract;
    }
};

} // namespace rtv::passes
