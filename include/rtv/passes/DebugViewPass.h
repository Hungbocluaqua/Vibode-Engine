#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

#include <cstdint>

namespace rtv::passes {

struct DebugViewPass {
    static constexpr const char* kContractId = "debug_view";
    static constexpr const char* kPassName = "DebugViewPass";
    static constexpr const char* kRole = "diagnostics";
    static constexpr const char* kExtractionState =
        "contract-module plus wavefront debug-view policy; scheduling and GPU resources remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings&) {
        return true;
    }

    static bool isSelected(const RendererSettings& settings) {
        return settings.debugView != RendererDebugView::Beauty;
    }

    static constexpr bool isWavefrontView(RendererDebugView view) {
        return view == RendererDebugView::WavefrontQueueOccupancy ||
            view == RendererDebugView::WavefrontPathDepth ||
            view == RendererDebugView::WavefrontLiveRays ||
            view == RendererDebugView::WavefrontTerminatedRays ||
            view == RendererDebugView::WavefrontMaterialBucket ||
            view == RendererDebugView::WavefrontRestirDi ||
            view == RendererDebugView::WavefrontDirectLighting ||
            view == RendererDebugView::WavefrontRestirGi;
    }

    static constexpr bool requiresWavefrontShadowTrace(RendererDebugView view) {
        return view == RendererDebugView::WavefrontDirectLighting;
    }

    static constexpr bool requiresWavefrontRestirReservoir(RendererDebugView view) {
        return view == RendererDebugView::WavefrontRestirDi;
    }

    static constexpr bool requiresWavefrontRestirGiReservoir(RendererDebugView view) {
        return view == RendererDebugView::WavefrontRestirGi;
    }

    static constexpr bool isNonDenoiserDebugView(uint32_t view) {
        if (view == 0u) {
            return false;
        }
        if (view <= 4u) {
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
        if (view >= static_cast<uint32_t>(RendererDebugView::DlssDepth) &&
            view <= static_cast<uint32_t>(RendererDebugView::DlssRrSpecularRayDirectionHitDistance)) {
            return false;
        }
        return true;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::DebugView;
        contract.role = RendererPassContractRole::Diagnostics;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isSelected(settings);
        contract.currentOwnerFile = "include/rtv/passes/DebugViewPass.h (contract + debug-view policy), src/rtv/RendererDebug.cpp; src/rtv/DiagnosticImageExport.cpp (runtime)";
        contract.featureFlagsRequired = "debugView != beauty || save-debug-views";
        contract.inputs = rendererContractArray({"registered pass debug outputs", "renderer debug params"});
        contract.outputs = rendererContractArray({"debug view PNG", "debug export manifest"});
        contract.historyResources = rendererContractArray({"none; reads pass-owned resources only"});
        contract.descriptorLayouts = rendererContractArray({"debug visualization descriptor set"});
        contract.pushConstants = rendererContractArray({"RendererDebugParams"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/pathtrace.rgen", "debug visualization compute paths"});
        contract.rendergraphReads = rendererContractArray({"pass-owned debug resources"});
        contract.rendergraphWrites = rendererContractArray({"debug output image"});
        contract.requiredBarriers = rendererContractArray({"owner pass output to debug read"});
        contract.cameraHistoryResetBehavior = "Debug views must not keep production history alive or mutate pass-owned resources.";
        contract.debugOutputs = rendererContractArray({"all registered RendererDebugView names"});
        contract.profilingSections = rendererContractArray({"debug export wall time outside GPU pass timings"});
        contract.validationChecks = rendererContractArray({"missing required debug view list", "export manifest completeness"});
        return contract;
    }
};

} // namespace rtv::passes
