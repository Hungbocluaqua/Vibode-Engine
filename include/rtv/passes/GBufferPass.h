#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

namespace rtv::passes {

struct GBufferPass {
    static constexpr const char* kContractId = "gbuffer";
    static constexpr const char* kPassName = "GBufferPass";
    static constexpr const char* kRole = "geometry";
    static constexpr const char* kExtractionState =
        "contract-module; scheduling and algorithm work remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings& settings) {
        return settings.pathTracingEnabled;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::GBuffer;
        contract.role = RendererPassContractRole::Geometry;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/GBufferPass.h (contract), src/rtv/PathTracerRenderer.cpp (runtime)";
        contract.featureFlagsRequired = "pathTracingEnabled";
        contract.inputs = rendererContractArray({"camera uniform", "scene TLAS", "instance transforms", "materials", "textures"});
        contract.outputs = rendererContractArray({"path data albedo", "normal/depth guides", "motion vectors", "world position", "roughness"});
        contract.historyResources = rendererContractArray({"previous world position", "previous material/object identity"});
        contract.descriptorLayouts = rendererContractArray({"global scene set", "bindless material texture set", "guide image set"});
        contract.pushConstants = rendererContractArray({"RendererDebugParams", "camera jitter", "render extent"});
        contract.pipelineShaderDependencies = rendererContractArray({"shaders/pathtrace.rgen", "shaders/nrd_prepare.comp", "shaders/dlss_rr_guides.comp"});
        contract.rendergraphReads = rendererContractArray({"scene buffers", "material buffers", "previous-frame surface state"});
        contract.rendergraphWrites = rendererContractArray({"surface guide buffers", "path data buffers", "motion/depth/normal images"});
        contract.requiredBarriers = rendererContractArray({"ray tracing writes to compute shader reads", "guide image layout GENERAL"});
        contract.cameraHistoryResetBehavior = "Reset guide history on camera cut, scene reload, material reload, resolution change, or render scale change.";
        contract.debugOutputs = rendererContractArray({"albedo", "normals", "depth", "roughness", "motion-vectors", "material-alpha", "material-transmission"});
        contract.profilingSections = rendererContractArray({"path_trace"});
        contract.validationChecks = rendererContractArray({"normal length", "roughness range", "depth finite", "motion convention", "material id validity"});
        return contract;
    }
};

} // namespace rtv::passes
