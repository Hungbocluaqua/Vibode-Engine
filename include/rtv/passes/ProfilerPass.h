#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererSettings.h"

namespace rtv::passes {

struct ProfilerPass {
    static constexpr const char* kContractId = "profiler";
    static constexpr const char* kPassName = "ProfilerPass";
    static constexpr const char* kRole = "diagnostics";
    static constexpr const char* kExtractionState =
        "contract-module; scheduling and algorithm work remain coordinated by PathTracerRenderer";

    static bool isActive(const RendererSettings&) {
        return true;
    }

    static RendererPassContract contract(const RendererSettings& settings) {
        RendererPassContract contract;
        contract.id = RendererPassContractId::Profiler;
        contract.role = RendererPassContractRole::Diagnostics;
        contract.name = kPassName;
        contract.activeByCurrentSettings = isActive(settings);
        contract.currentOwnerFile = "include/rtv/passes/ProfilerPass.h (contract), src/rtv/HeadlessDiagnostics.cpp; include/rtv/GpuProfiler.h (runtime)";
        contract.featureFlagsRequired = "profile || diagnostic output";
        contract.inputs = rendererContractArray({"GPU timestamps", "CPU frame timings", "RenderGraph dump", "debug counters"});
        contract.outputs = rendererContractArray({"profile.json", "rendergraph.json", "debug package", "performance history row"});
        contract.historyResources = rendererContractArray({"rolling performance history outside renderer"});
        contract.descriptorLayouts = rendererContractArray({"none"});
        contract.pushConstants = rendererContractArray({"none"});
        contract.pipelineShaderDependencies = rendererContractArray({"none"});
        contract.rendergraphReads = rendererContractArray({"pass timing timestamps", "diagnostic counters"});
        contract.rendergraphWrites = rendererContractArray({"JSON diagnostics", "budget check input"});
        contract.requiredBarriers = rendererContractArray({"timestamp query readback before JSON write"});
        contract.cameraHistoryResetBehavior = "Profiler state is per-run; history is external to renderer frame history.";
        contract.debugOutputs = rendererContractArray({"profile.json", "capture_validation.json", "summary.json"});
        contract.profilingSections = rendererContractArray({"all per_pass_gpu_ms fields", "queue_lane_ms"});
        contract.validationChecks = rendererContractArray({"missing timing section", "validation error count", "budget thresholds", "diagnostic readiness"});
        return contract;
    }
};

} // namespace rtv::passes
