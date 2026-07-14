#pragma once

#include <nlohmann/json.hpp>

#include <initializer_list>

namespace rtv {

inline nlohmann::json rendererContractArray(std::initializer_list<const char*> values) {
    nlohmann::json result = nlohmann::json::array();
    for (const char* value : values) {
        result.push_back(value);
    }
    return result;
}

enum class RendererPassContractId {
    GBuffer,
    PathTrace,
    RestirDI,
    RestirGI,
    Regir,
    TemporalAA,
    Denoiser,
    Nrd,
    Dlss,
    Composite,
    DebugView,
    Profiler,
};

enum class RendererPassContractRole {
    Geometry,
    LightingReuse,
    Temporal,
    Denoising,
    Upscaling,
    Output,
    Diagnostics,
};

struct RendererPassContract {
    RendererPassContractId id = RendererPassContractId::GBuffer;
    RendererPassContractRole role = RendererPassContractRole::Geometry;
    const char* name = "";
    bool activeByCurrentSettings = false;
    bool requiredForDiagnosticProfile = true;
    const char* currentOwnerFile = "";
    const char* featureFlagsRequired = "";
    nlohmann::json inputs = nlohmann::json::array();
    nlohmann::json outputs = nlohmann::json::array();
    nlohmann::json historyResources = nlohmann::json::array();
    nlohmann::json descriptorLayouts = nlohmann::json::array();
    nlohmann::json pushConstants = nlohmann::json::array();
    nlohmann::json pipelineShaderDependencies = nlohmann::json::array();
    nlohmann::json rendergraphReads = nlohmann::json::array();
    nlohmann::json rendergraphWrites = nlohmann::json::array();
    nlohmann::json requiredBarriers = nlohmann::json::array();
    const char* cameraHistoryResetBehavior = "";
    nlohmann::json debugOutputs = nlohmann::json::array();
    nlohmann::json profilingSections = nlohmann::json::array();
    nlohmann::json validationChecks = nlohmann::json::array();
};

} // namespace rtv
