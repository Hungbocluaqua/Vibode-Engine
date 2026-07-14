#pragma once

#include "rtv/RendererPassContractTypes.h"
#include "rtv/RendererPassOwners.h"
#include "rtv/RendererSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace rtv {

inline const char* rendererPassContractIdName(RendererPassContractId id) {
    switch (id) {
    case RendererPassContractId::GBuffer: return "gbuffer";
    case RendererPassContractId::PathTrace: return "path_trace";
    case RendererPassContractId::RestirDI: return "restir_di";
    case RendererPassContractId::RestirGI: return "restir_gi";
    case RendererPassContractId::Regir: return "regir";
    case RendererPassContractId::TemporalAA: return "temporal_aa";
    case RendererPassContractId::Denoiser: return "denoiser";
    case RendererPassContractId::Nrd: return "nrd";
    case RendererPassContractId::Dlss: return "dlss";
    case RendererPassContractId::Composite: return "composite";
    case RendererPassContractId::DebugView: return "debug_view";
    case RendererPassContractId::Profiler: return "profiler";
    }
    return "unknown";
}

inline const char* rendererPassContractRoleName(RendererPassContractRole role) {
    switch (role) {
    case RendererPassContractRole::Geometry: return "geometry";
    case RendererPassContractRole::LightingReuse: return "lighting_reuse";
    case RendererPassContractRole::Temporal: return "temporal";
    case RendererPassContractRole::Denoising: return "denoising";
    case RendererPassContractRole::Upscaling: return "upscaling";
    case RendererPassContractRole::Output: return "output";
    case RendererPassContractRole::Diagnostics: return "diagnostics";
    }
    return "unknown";
}

inline bool rendererStandaloneRestirDiPassRequested(const RendererSettings& settings) {
    return passes::RestirDIPass::isActive(settings);
}

inline nlohmann::json rendererPassContractJson(const RendererPassContract& contract) {
    const RendererPassOwnerMetadata* owner = rendererPassOwnerMetadataForId(rendererPassContractIdName(contract.id));
    nlohmann::json result = {
        {"id", rendererPassContractIdName(contract.id)},
        {"role", rendererPassContractRoleName(contract.role)},
        {"name", contract.name},
        {"active_by_current_settings", contract.activeByCurrentSettings},
        {"required_for_diagnostic_profile", contract.requiredForDiagnosticProfile},
        {"current_owner_file", contract.currentOwnerFile},
        {"owner_metadata", owner != nullptr ? rendererPassOwnerMetadataJson(*owner) : nlohmann::json(nullptr)},
        {"feature_flags_required", contract.featureFlagsRequired},
        {"inputs", contract.inputs},
        {"outputs", contract.outputs},
        {"history_resources", contract.historyResources},
        {"descriptor_layouts", contract.descriptorLayouts},
        {"push_constants", contract.pushConstants},
        {"pipeline_shader_dependencies", contract.pipelineShaderDependencies},
        {"rendergraph_reads", contract.rendergraphReads},
        {"rendergraph_writes", contract.rendergraphWrites},
        {"required_barriers", contract.requiredBarriers},
        {"camera_history_reset_behavior", contract.cameraHistoryResetBehavior},
        {"debug_outputs", contract.debugOutputs},
        {"profiling_sections", contract.profilingSections},
        {"validation_checks", contract.validationChecks},
    };
    return result;
}

inline bool rendererReadJsonArtifact(
    const std::filesystem::path& path,
    nlohmann::json& out,
    std::string& error) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            error = "not readable";
            return false;
        }
        file >> out;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

inline std::vector<RendererPassContract> rendererPassContracts(const RendererSettings& settings) {
    std::vector<RendererPassContract> passes;
    passes.reserve(12);
    passes.push_back(rtv::passes::GBufferPass::contract(settings));
    passes.push_back(rtv::passes::PathTracePass::contract(settings));

    passes.push_back(rtv::passes::RestirDIPass::contract(settings));
    passes.push_back(rtv::passes::RestirGIPass::contract(settings));
    passes.push_back(rtv::passes::RegirPass::contract(settings));

    passes.push_back(rtv::passes::TemporalAAPass::contract(settings));
    passes.push_back(rtv::passes::DenoiserPass::contract(settings));
    passes.push_back(rtv::passes::NrdPass::contract(settings));

    passes.push_back(rtv::passes::DlssPass::contract(settings));
    passes.push_back(rtv::passes::CompositePass::contract(settings));
    passes.push_back(rtv::passes::DebugViewPass::contract(settings));
    passes.push_back(rtv::passes::ProfilerPass::contract(settings));

    return passes;
}

inline nlohmann::json rendererSettingsSnapshotJson(const RendererSettings& settings) {
    return nlohmann::json{
        {"restir_di_mode", restirDiModeName(settings.restirDiMode)},
        {"restir_gi_mode", restirGiModeName(settings.restirGiMode)},
        {"lighting_reuse_mode", lightingReuseModeName(settings.lightingReuseMode)},
        {"denoiser_backend", denoiserBackendName(settings.denoiserBackend)},
        {"temporal_upscaler", temporalUpscalerName(settings.temporalUpscaler)},
    };
}

inline nlohmann::json rendererPassContractsJson(const RendererSettings& settings) {
    const std::vector<RendererPassContract> contracts = rendererPassContracts(settings);
    nlohmann::json passes = nlohmann::json::array();
    for (const RendererPassContract& contract : contracts) {
        passes.push_back(rendererPassContractJson(contract));
    }

    return nlohmann::json{
        {"schema_version", 1},
        {"coordinator", "PathTracerRenderer"},
        {"status", "contract metadata; algorithm extraction remains staged behind the current coordinator"},
        {"settings_snapshot", rendererSettingsSnapshotJson(settings)},
        {"pass_count", contracts.size()},
        {"passes", std::move(passes)},
        {"passed", contracts.size() == 12u},
    };
}

inline nlohmann::json rendererPassContractValidationJson(const RendererSettings& settings) {
    constexpr size_t kExpectedPassCount = 12;
    const std::vector<RendererPassContract> contracts = rendererPassContracts(settings);
    nlohmann::json requiredPasses = nlohmann::json::array();
    nlohmann::json activePasses = nlohmann::json::array();
    nlohmann::json missingRequiredFields = nlohmann::json::array();

    auto requireString = [&](const RendererPassContract& contract, const char* fieldName, const char* value) {
        if (value == nullptr || value[0] == '\0') {
            missingRequiredFields.push_back({
                {"pass", contract.name},
                {"field", fieldName},
            });
        }
    };
    auto requireArray = [&](const RendererPassContract& contract, const char* fieldName, const nlohmann::json& value) {
        if (!value.is_array() || value.empty()) {
            missingRequiredFields.push_back({
                {"pass", contract.name},
                {"field", fieldName},
            });
        }
    };

    for (const RendererPassContract& contract : contracts) {
        if (contract.requiredForDiagnosticProfile) {
            requiredPasses.push_back(contract.name);
        }
        if (contract.activeByCurrentSettings) {
            activePasses.push_back(contract.name);
        }
        requireString(contract, "name", contract.name);
        requireString(contract, "current_owner_file", contract.currentOwnerFile);
        requireString(contract, "feature_flags_required", contract.featureFlagsRequired);
        requireArray(contract, "inputs", contract.inputs);
        requireArray(contract, "outputs", contract.outputs);
        requireArray(contract, "history_resources", contract.historyResources);
        requireArray(contract, "descriptor_layouts", contract.descriptorLayouts);
        requireArray(contract, "push_constants", contract.pushConstants);
        requireArray(contract, "pipeline_shader_dependencies", contract.pipelineShaderDependencies);
        requireArray(contract, "rendergraph_reads", contract.rendergraphReads);
        requireArray(contract, "rendergraph_writes", contract.rendergraphWrites);
        requireArray(contract, "required_barriers", contract.requiredBarriers);
        requireString(contract, "camera_history_reset_behavior", contract.cameraHistoryResetBehavior);
        requireArray(contract, "debug_outputs", contract.debugOutputs);
        requireArray(contract, "profiling_sections", contract.profilingSections);
        requireArray(contract, "validation_checks", contract.validationChecks);
    }

    const bool expectedPassCount = contracts.size() == kExpectedPassCount;
    const bool passed = expectedPassCount && missingRequiredFields.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"expected_pass_count", kExpectedPassCount},
        {"pass_count", contracts.size()},
        {"required_passes", std::move(requiredPasses)},
        {"active_passes", std::move(activePasses)},
        {"missing_required_fields", std::move(missingRequiredFields)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererPassOwnerRegistryValidationJson(const RendererSettings& settings) {
    constexpr size_t kExpectedPassCount = 12;
    const std::vector<RendererPassContract> contracts = rendererPassContracts(settings);
    constexpr auto registry = rendererPassOwnerRegistry();
    nlohmann::json ownerRows = nlohmann::json::array();
    nlohmann::json contractModuleOwnedRows = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    uint32_t contractModuleOwnedCount = 0;
    auto stringPresent = [](const char* value) {
        return value != nullptr && value[0] != '\0';
    };

    for (const RendererPassContract& contract : contracts) {
        const char* contractId = rendererPassContractIdName(contract.id);
        const RendererPassOwnerMetadata* owner = rendererPassOwnerMetadataForId(contractId);
        if (owner == nullptr) {
            failures.push_back({
                {"code", "missing_owner_metadata"},
                {"pass", contract.name},
                {"contract_id", contractId},
            });
            continue;
        }
        const bool rowValid =
            stringPresent(owner->contractId) &&
            stringPresent(owner->passName) &&
            stringPresent(owner->ownerSymbol) &&
            stringPresent(owner->metadataHeader) &&
            stringPresent(owner->plannedImplementationHeader) &&
            stringPresent(owner->extractionState);
        ownerRows.push_back(rendererPassOwnerMetadataJson(*owner));
        if (!rowValid) {
            failures.push_back({
                {"code", "incomplete_owner_metadata"},
                {"pass", contract.name},
                {"contract_id", contractId},
                {"owner_metadata", rendererPassOwnerMetadataJson(*owner)},
            });
        }
        if (std::string_view(owner->passName) != contract.name) {
            failures.push_back({
                {"code", "owner_pass_name_mismatch"},
                {"pass", contract.name},
                {"contract_id", contractId},
                {"owner_pass_name", owner->passName},
            });
        }
        const bool contractMetadataOwnedByModule =
            std::string_view(contract.currentOwnerFile).find(owner->metadataHeader) != std::string_view::npos;
        if (contractMetadataOwnedByModule) {
            ++contractModuleOwnedCount;
            contractModuleOwnedRows.push_back({
                {"pass", contract.name},
                {"contract_id", contractId},
                {"metadata_header", owner->metadataHeader},
                {"current_owner_file", contract.currentOwnerFile},
            });
        }
    }

    if (registry.size() != kExpectedPassCount) {
        failures.push_back({
            {"code", "unexpected_owner_registry_count"},
            {"expected", kExpectedPassCount},
            {"actual", registry.size()},
        });
    }
    if (contracts.size() != kExpectedPassCount) {
        failures.push_back({
            {"code", "unexpected_contract_count"},
            {"expected", kExpectedPassCount},
            {"actual", contracts.size()},
        });
    }

    return nlohmann::json{
        {"schema_version", 1},
        {"expected_pass_count", kExpectedPassCount},
        {"contract_count", contracts.size()},
        {"owner_registry_count", registry.size()},
        {"contract_module_owned_count", contractModuleOwnedCount},
        {"owner_rows", std::move(ownerRows)},
        {"contract_module_owned_rows", std::move(contractModuleOwnedRows)},
        {"failure_count", failures.size()},
        {"failures", std::move(failures)},
        {"passed", failures.empty()},
    };
}

inline bool rendererPassProfilingSectionIsAggregate(const std::string& section) {
    return section.find(' ') != std::string::npos ||
        section == "queue_lane_ms" ||
        section == "all per_pass_gpu_ms fields";
}

inline nlohmann::json rendererPassTimingCoverageJson(const RendererSettings& settings, const nlohmann::json& perPassGpuMs) {
    const std::vector<RendererPassContract> contracts = rendererPassContracts(settings);
    nlohmann::json sections = nlohmann::json::array();
    nlohmann::json unmappedSections = nlohmann::json::array();
    nlohmann::json activeUnmappedSections = nlohmann::json::array();

    uint32_t coveredSectionCount = 0;
    uint32_t aggregateSectionCount = 0;
    uint32_t unmappedSectionCount = 0;
    uint32_t activeUnmappedSectionCount = 0;

    for (const RendererPassContract& contract : contracts) {
        if (!contract.profilingSections.is_array()) {
            continue;
        }
        for (const nlohmann::json& sectionValue : contract.profilingSections) {
            if (!sectionValue.is_string()) {
                continue;
            }
            const std::string section = sectionValue.get<std::string>();
            nlohmann::json sectionReport = {
                {"pass", contract.name},
                {"pass_id", rendererPassContractIdName(contract.id)},
                {"role", rendererPassContractRoleName(contract.role)},
                {"active_by_current_settings", contract.activeByCurrentSettings},
                {"section", section},
            };
            if (rendererPassProfilingSectionIsAggregate(section)) {
                ++aggregateSectionCount;
                sectionReport["status"] = "aggregate_or_external";
                sectionReport["timed_in_per_pass_gpu_ms"] = false;
            } else if (perPassGpuMs.is_object() && perPassGpuMs.contains(section) && perPassGpuMs[section].is_number()) {
                ++coveredSectionCount;
                sectionReport["status"] = "timed";
                sectionReport["timed_in_per_pass_gpu_ms"] = true;
                sectionReport["gpu_ms"] = perPassGpuMs[section];
            } else {
                ++unmappedSectionCount;
                sectionReport["status"] = "unmapped";
                sectionReport["timed_in_per_pass_gpu_ms"] = false;
                unmappedSections.push_back({
                    {"pass", contract.name},
                    {"section", section},
                    {"active_by_current_settings", contract.activeByCurrentSettings},
                });
                if (contract.activeByCurrentSettings) {
                    ++activeUnmappedSectionCount;
                    activeUnmappedSections.push_back({
                        {"pass", contract.name},
                        {"section", section},
                    });
                }
            }
            sections.push_back(std::move(sectionReport));
        }
    }

    return nlohmann::json{
        {"schema_version", 1},
        {"per_pass_gpu_ms_available", perPassGpuMs.is_object()},
        {"pass_count", contracts.size()},
        {"covered_section_count", coveredSectionCount},
        {"aggregate_or_external_section_count", aggregateSectionCount},
        {"unmapped_section_count", unmappedSectionCount},
        {"active_unmapped_section_count", activeUnmappedSectionCount},
        {"unmapped_sections", std::move(unmappedSections)},
        {"active_unmapped_sections", std::move(activeUnmappedSections)},
        {"sections", std::move(sections)},
        {"passed", perPassGpuMs.is_object() && activeUnmappedSectionCount == 0},
    };
}

inline bool rendererDebugOutputIsAggregateOrArtifact(const std::string& output) {
    return output.find(".json") != std::string::npos ||
        output == "debug export manifest" ||
        output == "debug view selected by RendererDebugView" ||
        output == "all registered RendererDebugView names";
}

inline nlohmann::json rendererDebugOutputCoverageJson(
    const RendererSettings& settings,
    const std::vector<std::string>& exportableDebugViews) {
    const std::vector<RendererPassContract> contracts = rendererPassContracts(settings);
    nlohmann::json outputs = nlohmann::json::array();
    nlohmann::json missingOutputs = nlohmann::json::array();
    nlohmann::json activeMissingOutputs = nlohmann::json::array();

    uint32_t declaredOutputCount = 0;
    uint32_t exportableOutputCount = 0;
    uint32_t aggregateOrArtifactCount = 0;
    uint32_t missingOutputCount = 0;
    uint32_t activeMissingOutputCount = 0;

    for (const RendererPassContract& contract : contracts) {
        if (!contract.debugOutputs.is_array()) {
            continue;
        }
        for (const nlohmann::json& outputValue : contract.debugOutputs) {
            if (!outputValue.is_string()) {
                continue;
            }
            ++declaredOutputCount;
            const std::string output = outputValue.get<std::string>();
            const bool aggregateOrArtifact = rendererDebugOutputIsAggregateOrArtifact(output);
            const bool exportable = !aggregateOrArtifact &&
                std::find(exportableDebugViews.begin(), exportableDebugViews.end(), output) != exportableDebugViews.end();

            nlohmann::json outputReport = {
                {"pass", contract.name},
                {"pass_id", rendererPassContractIdName(contract.id)},
                {"role", rendererPassContractRoleName(contract.role)},
                {"active_by_current_settings", contract.activeByCurrentSettings},
                {"debug_output", output},
            };
            if (aggregateOrArtifact) {
                ++aggregateOrArtifactCount;
                outputReport["status"] = "aggregate_or_artifact";
                outputReport["exportable_debug_view"] = false;
            } else if (exportable) {
                ++exportableOutputCount;
                outputReport["status"] = "exportable";
                outputReport["exportable_debug_view"] = true;
            } else {
                ++missingOutputCount;
                outputReport["status"] = "missing_exportable_debug_view";
                outputReport["exportable_debug_view"] = false;
                missingOutputs.push_back({
                    {"pass", contract.name},
                    {"debug_output", output},
                    {"active_by_current_settings", contract.activeByCurrentSettings},
                });
                if (contract.activeByCurrentSettings) {
                    ++activeMissingOutputCount;
                    activeMissingOutputs.push_back({
                        {"pass", contract.name},
                        {"debug_output", output},
                    });
                }
            }
            outputs.push_back(std::move(outputReport));
        }
    }

    return nlohmann::json{
        {"schema_version", 1},
        {"exportable_debug_view_count", exportableDebugViews.size()},
        {"declared_debug_output_count", declaredOutputCount},
        {"exportable_output_count", exportableOutputCount},
        {"aggregate_or_artifact_count", aggregateOrArtifactCount},
        {"missing_output_count", missingOutputCount},
        {"active_missing_output_count", activeMissingOutputCount},
        {"missing_outputs", std::move(missingOutputs)},
        {"active_missing_outputs", std::move(activeMissingOutputs)},
        {"outputs", std::move(outputs)},
        {"passed", activeMissingOutputCount == 0},
    };
}

inline bool rendererStringStartsWith(const std::string& value, const char* prefix) {
    const std::string prefixString(prefix);
    return value.size() >= prefixString.size() &&
        value.compare(0, prefixString.size(), prefixString) == 0;
}

inline bool rendererStringContains(const std::string& value, const char* needle) {
    return value.find(needle) != std::string::npos;
}

inline const char* rendererDebugViewOwnerForName(const std::string& name) {
    if (rendererStringStartsWith(name, "restir-di") || rendererStringStartsWith(name, "restir-reservoir")) {
        return "RestirDIPass";
    }
    if (rendererStringStartsWith(name, "restir-gi")) {
        return "RestirGIPass";
    }
    if (rendererStringStartsWith(name, "regir")) {
        return "RegirPass";
    }
    if (rendererStringStartsWith(name, "denoiser") || rendererStringStartsWith(name, "moment")) {
        return "DenoiserPass";
    }
    if (rendererStringStartsWith(name, "nrd")) {
        return "NrdPass";
    }
    if (rendererStringStartsWith(name, "dlss")) {
        return "DlssPass";
    }
    if (rendererStringStartsWith(name, "psr")) {
        return "NrdPass";
    }
    if (rendererStringStartsWith(name, "temporal") ||
        rendererStringContains(name, "motion-vector") ||
        rendererStringContains(name, "reprojection")) {
        return "TemporalAAPass";
    }
    if (name == "albedo" ||
        name == "normals" ||
        name == "depth" ||
        name == "roughness" ||
        rendererStringStartsWith(name, "material") ||
        rendererStringContains(name, "instance-id") ||
        rendererStringContains(name, "mesh-id")) {
        return "GBufferPass";
    }
    if (name == "beauty" ||
        rendererStringContains(name, "lighting") ||
        rendererStringContains(name, "path-") ||
        rendererStringContains(name, "bounce") ||
        rendererStringContains(name, "pdf") ||
        rendererStringContains(name, "mis-weight") ||
        rendererStringContains(name, "throughput") ||
        rendererStringContains(name, "radiance") ||
        rendererStringContains(name, "environment") ||
        rendererStringContains(name, "sun")) {
        return "PathTracePass";
    }
    if (rendererStringStartsWith(name, "atmosphere") ||
        rendererStringStartsWith(name, "fog") ||
        rendererStringContains(name, "tone-map") ||
        rendererStringContains(name, "fullscreen")) {
        return "CompositePass";
    }
    if (rendererStringStartsWith(name, "wavefront") ||
        rendererStringStartsWith(name, "adaptive") ||
        rendererStringContains(name, "validation") ||
        rendererStringContains(name, "mismatch")) {
        return "DebugViewPass";
    }
    return "DebugViewPass";
}

inline const char* rendererDebugViewValueRangeForName(const std::string& name) {
    if (rendererStringContains(name, "normal")) {
        return "world-space normal or encoded normal vector, remapped for PNG export";
    }
    if (rendererStringContains(name, "motion")) {
        return "signed screen-space pixel motion, visualized around zero";
    }
    if (rendererStringContains(name, "depth") || rendererStringContains(name, "hit-distance")) {
        return "finite nonnegative distance or depth, normalized for diagnostic export";
    }
    if (rendererStringContains(name, "confidence") ||
        rendererStringContains(name, "valid") ||
        rendererStringContains(name, "mask") ||
        rendererStringContains(name, "occupancy") ||
        rendererStringContains(name, "accepted") ||
        rendererStringContains(name, "visibility")) {
        return "[0, 1] confidence, mask, occupancy, or visibility scalar";
    }
    if (rendererStringContains(name, "pdf") ||
        rendererStringContains(name, "weight") ||
        rendererStringContains(name, "variance") ||
        rendererStringContains(name, "m") ||
        rendererStringContains(name, "age") ||
        rendererStringContains(name, "count")) {
        return "nonnegative scalar diagnostic, log/normalized for PNG export when needed";
    }
    if (rendererStringContains(name, "id") ||
        rendererStringContains(name, "class") ||
        rendererStringContains(name, "type") ||
        rendererStringContains(name, "cell") ||
        rendererStringContains(name, "selected-light") ||
        rendererStringContains(name, "generation")) {
        return "categorical integer id or class encoded as a debug color";
    }
    if (rendererStringContains(name, "albedo") ||
        rendererStringContains(name, "color") ||
        rendererStringContains(name, "lighting") ||
        rendererStringContains(name, "radiance") ||
        name == "beauty") {
        return "linear HDR color or contribution, tone-mapped or normalized for PNG export";
    }
    return "renderer diagnostic scalar/vector normalized for PNG export";
}

inline const char* rendererDebugViewMeaningForName(const std::string& name) {
    if (rendererStringStartsWith(name, "restir-di")) {
        return "ReSTIR DI reservoir, light identity, PDF, visibility, or reuse diagnostic.";
    }
    if (rendererStringStartsWith(name, "restir-gi")) {
        return "ReSTIR GI receiver, sample, reservoir, visibility, path-class, or reuse diagnostic.";
    }
    if (rendererStringStartsWith(name, "regir")) {
        return "ReGIR grid, reservoir, query, environment, or reuse diagnostic.";
    }
    if (rendererStringStartsWith(name, "denoiser") || rendererStringStartsWith(name, "moment")) {
        return "Engine denoiser guide, history, variance, moment, rejection, or confidence diagnostic.";
    }
    if (rendererStringStartsWith(name, "nrd") || rendererStringStartsWith(name, "psr")) {
        return "NRD or primary-surface-replacement guide and confidence diagnostic.";
    }
    if (rendererStringStartsWith(name, "dlss")) {
        return "DLSS or DLSS Ray Reconstruction input/output guide diagnostic.";
    }
    if (rendererStringStartsWith(name, "temporal") ||
        rendererStringContains(name, "motion-vector") ||
        rendererStringContains(name, "reprojection")) {
        return "Temporal reprojection, motion, reactive-mask, or history confidence diagnostic.";
    }
    if (name == "albedo" || name == "normals" || name == "depth" || name == "roughness" || rendererStringStartsWith(name, "material")) {
        return "Primary-surface guide data produced by the renderer-application bridge.";
    }
    if (rendererStringStartsWith(name, "wavefront")) {
        return "Wavefront path tracing queue, ray state, material bucket, or lighting diagnostic.";
    }
    if (rendererStringStartsWith(name, "adaptive")) {
        return "Adaptive sampling density, fill, disocclusion, or sample-count diagnostic.";
    }
    if (name == "beauty" || rendererStringContains(name, "lighting") || rendererStringContains(name, "path-")) {
        return "Path-traced color contribution or path-channel diagnostic.";
    }
    return "Renderer debug output for inspecting feature state, bad pixels, or validation evidence.";
}

inline nlohmann::json rendererDebugViewRegistryEntryJson(const std::string& name) {
    return nlohmann::json{
        {"stable_name", name},
        {"owner_pass", rendererDebugViewOwnerForName(name)},
        {"value_range", rendererDebugViewValueRangeForName(name)},
        {"meaning", rendererDebugViewMeaningForName(name)},
        {"export_artifact_format", "png"},
        {"failure_hints", rendererContractArray({
            "Inspect this view next to beauty, profile.json, and rendergraph.json.",
            "Compare against the owning pass timing and validation counters.",
            "If values are all black/white/NaN-looking, verify guide resources, reset reason, and pass activation.",
        })},
    };
}

inline nlohmann::json rendererDebugViewRegistryValidationJson(
    const RendererSettings& settings,
    const std::vector<std::string>& exportableDebugViews) {
    nlohmann::json registry = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json duplicateNames = nlohmann::json::array();
    nlohmann::json activeContractViews = nlohmann::json::array();
    nlohmann::json missingActiveRegistryEntries = nlohmann::json::array();
    nlohmann::json missingRequiredMetadata = nlohmann::json::array();

    std::vector<std::string> seenNames;
    seenNames.reserve(exportableDebugViews.size());
    for (const std::string& name : exportableDebugViews) {
        if (name.empty()) {
            failures.push_back({
                {"code", "empty_debug_view_name"},
                {"message", "Exportable debug view names must be stable non-empty strings."},
            });
            continue;
        }
        if (std::find(seenNames.begin(), seenNames.end(), name) != seenNames.end()) {
            duplicateNames.push_back(name);
            continue;
        }
        seenNames.push_back(name);
        nlohmann::json entry = rendererDebugViewRegistryEntryJson(name);
        if (entry.value("owner_pass", std::string{}).empty() ||
            entry.value("value_range", std::string{}).empty() ||
            entry.value("meaning", std::string{}).empty() ||
            !entry.contains("failure_hints") ||
            !entry["failure_hints"].is_array() ||
            entry["failure_hints"].empty()) {
            missingRequiredMetadata.push_back(name);
        }
        registry.push_back(std::move(entry));
    }

    auto registryContains = [&](const std::string& name) {
        return std::find(seenNames.begin(), seenNames.end(), name) != seenNames.end();
    };

    const std::vector<RendererPassContract> contracts = rendererPassContracts(settings);
    for (const RendererPassContract& contract : contracts) {
        if (!contract.activeByCurrentSettings || !contract.debugOutputs.is_array()) {
            continue;
        }
        for (const nlohmann::json& outputValue : contract.debugOutputs) {
            if (!outputValue.is_string()) {
                continue;
            }
            const std::string output = outputValue.get<std::string>();
            if (rendererDebugOutputIsAggregateOrArtifact(output)) {
                continue;
            }
            activeContractViews.push_back({
                {"pass", contract.name},
                {"pass_id", rendererPassContractIdName(contract.id)},
                {"debug_view", output},
                {"registered", registryContains(output)},
            });
            if (!registryContains(output)) {
                missingActiveRegistryEntries.push_back({
                    {"pass", contract.name},
                    {"debug_view", output},
                });
            }
        }
    }

    if (exportableDebugViews.empty()) {
        failures.push_back({
            {"code", "no_exportable_debug_views"},
            {"message", "Debug view registry requires at least one exportable view."},
        });
    }
    if (!duplicateNames.empty()) {
        failures.push_back({
            {"code", "duplicate_debug_view_names"},
            {"duplicate_count", duplicateNames.size()},
        });
    }
    if (!missingRequiredMetadata.empty()) {
        failures.push_back({
            {"code", "debug_view_metadata_incomplete"},
            {"missing_count", missingRequiredMetadata.size()},
        });
    }
    if (!missingActiveRegistryEntries.empty()) {
        failures.push_back({
            {"code", "active_debug_view_missing_registry_entry"},
            {"missing_count", missingActiveRegistryEntries.size()},
        });
    }
    if (registry.size() != exportableDebugViews.size()) {
        warnings.push_back({
            {"code", "registry_size_differs_from_export_list"},
            {"registry_count", registry.size()},
            {"exportable_count", exportableDebugViews.size()},
        });
    }

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"contract", "debug_view_registry"},
        {"exportable_debug_view_count", exportableDebugViews.size()},
        {"registered_debug_view_count", registry.size()},
        {"active_contract_debug_view_count", activeContractViews.size()},
        {"duplicate_names", std::move(duplicateNames)},
        {"missing_required_metadata", std::move(missingRequiredMetadata)},
        {"missing_active_registry_entries", std::move(missingActiveRegistryEntries)},
        {"active_contract_views", std::move(activeContractViews)},
        {"registry", std::move(registry)},
        {"failure_count", failures.size()},
        {"warning_count", warnings.size()},
        {"failures", std::move(failures)},
        {"warnings", std::move(warnings)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererTemporalContractJson() {
    return nlohmann::json{
        {"schema_version", 1},
        {"frame_timeline", {
            {"frame_index", "monotonic rendered frame index"},
            {"accumulation_frame_index", "resets to zero when accumulation history is invalidated"},
            {"previous_frame_index", "last frame with valid temporal history"},
            {"camera_cut_flag", "global reset input for TAA, denoiser, ReSTIR, NRD, DLSS, and ReGIR"},
            {"history_valid_flag", "per-pass history validity plus shared reset reason"},
            {"jitter_index", "shared temporal jitter sequence index"},
            {"reset_reason", "startup, resize, camera moved, settings, lighting, environment, denoiser, debug, scene, material, shader"},
        }},
        {"motion_vectors", {
            {"space", "screen pixels"},
            {"direction", "current-to-previous reprojection"},
            {"jitter", "consumer must state whether jitter is included; DLSS receives explicit jitter offsets"},
            {"validity", "invalid surface or disocclusion must zero confidence rather than silently reuse history"},
        }},
        {"depth", {
            {"primary", "renderer guide depth"},
            {"validity", "finite positive depth for hit surfaces; explicit invalid mask for miss/sky paths"},
            {"reset_on", rendererContractArray({"resolution change", "render scale change", "camera cut", "scene reload"})},
        }},
        {"normal", {
            {"space", "world space"},
            {"kind", "shading normal for denoising/reprojection, geometric normal available for visibility/backface policy"},
            {"normalization", "unit length within validation tolerance"},
        }},
        {"shared_reset_rules", rendererContractArray({
            "startup",
            "camera cut",
            "resolution change",
            "render scale change",
            "DLSS/NRD/denoiser mode change",
            "scene reload",
            "material reload",
            "light topology or generation change",
            "explicit accumulation reset",
        })},
        {"debug_overlay", rendererContractArray({
            "history valid",
            "motion magnitude",
            "disocclusion",
            "reset reason",
            "TAA rejection",
            "ReSTIR temporal rejection",
            "NRD/DLSS guide validity",
        })},
    };
}

inline bool rendererTemporalResetReasonKnown(const std::string& reason) {
    return reason == "Startup" ||
        reason == "Resize" ||
        reason == "CameraMoved" ||
        reason == "Manual" ||
        reason == "RenderSettingsChanged" ||
        reason == "LightingChanged" ||
        reason == "EnvironmentChanged" ||
        reason == "DenoiserChanged" ||
        reason == "DebugViewChanged" ||
        reason == "SceneChanged" ||
        reason == "MaterialChanged" ||
        reason == "ShaderReloaded";
}

inline nlohmann::json rendererTemporalRuntimeValidationJson(
    const RendererSettings& settings,
    const std::string& lastResetReason,
    uint32_t frameCount,
    uint32_t profiledFrames,
    uint64_t temporalHistoryBytes,
    bool restirDiHistoryValid,
    bool restirGiHistoryValid,
    bool regirTemporalHistoryValid,
    const nlohmann::json& temporalSystemDiagnostics) {
    const bool restirDiActive = passes::RestirDIPass::isAnyModeActive(settings);
    const bool restirGiActive = passes::RestirGIPass::isActive(settings);
    const bool regirActive = passes::RegirPass::isActive(settings);
    const bool engineDenoiserActive = passes::DenoiserPass::isActive(settings);
    const bool nrdActive = passes::NrdPass::isActive(settings);
    const bool taaActive = passes::TemporalAAPass::isActive(settings);
    const bool dlssActive = passes::DlssPass::isActive(settings);
    const bool anyTemporalFeatureActive =
        restirDiActive || restirGiActive || regirActive || engineDenoiserActive || taaActive || nrdActive || dlssActive;

    auto findHistorySlot = [&](const char* slotName) -> const nlohmann::json* {
        if (!temporalSystemDiagnostics.is_object() ||
            !temporalSystemDiagnostics.value("available", false) ||
            !temporalSystemDiagnostics.contains("slots") ||
            !temporalSystemDiagnostics["slots"].is_array()) {
            return nullptr;
        }
        for (const nlohmann::json& slot : temporalSystemDiagnostics["slots"]) {
            if (slot.is_object() && slot.value("name", std::string{}) == slotName) {
                return &slot;
            }
        }
        return nullptr;
    };
    auto historySlotExists = [&](const char* slotName) {
        return findHistorySlot(slotName) != nullptr;
    };
    auto historySlotValid = [&](const char* slotName) -> nlohmann::json {
        const nlohmann::json* slot = findHistorySlot(slotName);
        return slot != nullptr && slot->contains("valid") && (*slot)["valid"].is_boolean()
            ? (*slot)["valid"]
            : nlohmann::json(nullptr);
    };
    auto requiredSlotStatuses = [&](const nlohmann::json& requiredSlots) {
        nlohmann::json statuses = nlohmann::json::array();
        if (!requiredSlots.is_array()) {
            return statuses;
        }
        for (const nlohmann::json& requiredSlot : requiredSlots) {
            if (!requiredSlot.is_string()) {
                continue;
            }
            const std::string slotName = requiredSlot.get<std::string>();
            const nlohmann::json* slot = findHistorySlot(slotName.c_str());
            nlohmann::json status = {
                {"slot", slotName},
                {"exists", slot != nullptr},
            };
            if (slot != nullptr) {
                status["valid"] = slot->value("valid", false);
                status["resident"] = slot->value("resident", false);
                status["last_written_frame"] = slot->value("last_written_frame", uint64_t{0});
                status["estimated_bytes"] = slot->value("estimated_bytes", uint64_t{0});
            } else {
                status["valid"] = false;
                status["resident"] = false;
                status["last_written_frame"] = nullptr;
                status["estimated_bytes"] = 0;
            }
            statuses.push_back(std::move(status));
        }
        return statuses;
    };
    auto slotStatusesReady = [](const nlohmann::json& statuses) {
        if (!statuses.is_array() || statuses.empty()) {
            return nlohmann::json(nullptr);
        }
        for (const nlohmann::json& status : statuses) {
            if (!status.value("exists", false) ||
                !status.value("valid", false) ||
                !status.value("resident", false)) {
                return nlohmann::json(false);
            }
        }
        return nlohmann::json(true);
    };

    nlohmann::json features = nlohmann::json::array();
    auto feature = [&](const char* name, bool active, nlohmann::json historyValid, const char* owner, nlohmann::json requiredSlots) {
        const nlohmann::json slotStatuses = requiredSlotStatuses(requiredSlots);
        nlohmann::json historyReady = active ? slotStatusesReady(slotStatuses) : nlohmann::json(nullptr);
        if (!active) {
            historyValid = nullptr;
        }
        if (active && historyValid.is_boolean() && !historyReady.is_boolean()) {
            historyReady = historyValid;
        }
        features.push_back({
            {"name", name},
            {"active", active},
            {"history_valid", std::move(historyValid)},
            {"history_ready", std::move(historyReady)},
            {"owner", owner},
            {"required_temporal_system_slots", std::move(requiredSlots)},
            {"temporal_system_slot_status", std::move(slotStatuses)},
        });
    };
    feature("restir_di", restirDiActive, restirDiHistoryValid, "RestirDIPass", rendererContractArray({"restir_reservoir", "previous_world_position"}));
    feature("restir_gi", restirGiActive, restirGiHistoryValid, "RestirGIPass", rendererContractArray({"restir_gi_reservoir", "previous_world_position"}));
    feature("regir", regirActive, regirTemporalHistoryValid, "RegirPass", rendererContractArray({}));
    feature("denoiser", engineDenoiserActive, historySlotValid("denoiser_history"), "DenoiserPass", rendererContractArray({"denoiser_history", "previous_world_position"}));
    feature("nrd", nrdActive, nullptr, "NrdPass", rendererContractArray({"denoiser_diffuse_history", "denoiser_specular_history", "previous_world_position"}));
    feature("taa_tsr", taaActive, historySlotValid("taa_history"), "TemporalAAPass", rendererContractArray({"taa_history", "previous_world_position"}));
    feature("dlss", dlssActive, historySlotValid("previous_world_position"), "DlssPass", rendererContractArray({"previous_world_position"}));

    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json requiredHistorySlots = nlohmann::json::array();
    nlohmann::json missingRequiredHistorySlots = nlohmann::json::array();
    nlohmann::json unreadyRequiredHistorySlots = nlohmann::json::array();
    auto requireHistorySlot = [&](const char* featureName, bool active, const char* slotName) {
        if (!active) {
            return;
        }
        const nlohmann::json* slot = findHistorySlot(slotName);
        requiredHistorySlots.push_back({
            {"feature", featureName},
            {"slot", slotName},
        });
        if (slot == nullptr) {
            missingRequiredHistorySlots.push_back({
                {"feature", featureName},
                {"slot", slotName},
            });
            return;
        }
        if (!slot->value("valid", false) || !slot->value("resident", false)) {
            unreadyRequiredHistorySlots.push_back({
                {"feature", featureName},
                {"slot", slotName},
                {"valid", slot->value("valid", false)},
                {"resident", slot->value("resident", false)},
                {"last_written_frame", slot->value("last_written_frame", uint64_t{0})},
                {"estimated_bytes", slot->value("estimated_bytes", uint64_t{0})},
            });
        }
    };
    requireHistorySlot("restir_di", restirDiActive, "restir_reservoir");
    requireHistorySlot("restir_di", restirDiActive, "previous_world_position");
    requireHistorySlot("restir_gi", restirGiActive, "restir_gi_reservoir");
    requireHistorySlot("restir_gi", restirGiActive, "previous_world_position");
    requireHistorySlot("denoiser", engineDenoiserActive, "denoiser_history");
    requireHistorySlot("denoiser", engineDenoiserActive, "previous_world_position");
    requireHistorySlot("nrd", nrdActive, "denoiser_diffuse_history");
    requireHistorySlot("nrd", nrdActive, "denoiser_specular_history");
    requireHistorySlot("nrd", nrdActive, "previous_world_position");
    requireHistorySlot("taa_tsr", taaActive, "taa_history");
    requireHistorySlot("taa_tsr", taaActive, "previous_world_position");
    requireHistorySlot("dlss", dlssActive, "previous_world_position");

    const bool resetReasonKnown = rendererTemporalResetReasonKnown(lastResetReason);
    if (!resetReasonKnown) {
        failures.push_back({
            {"code", "unknown_reset_reason"},
            {"reason", lastResetReason},
        });
    }
    if (profiledFrames == 0u && frameCount > 0u) {
        failures.push_back({
            {"code", "no_profiled_frames"},
            {"reason", "Temporal runtime validation needs at least one measured frame after warmup."},
        });
    }
    if (anyTemporalFeatureActive && temporalHistoryBytes == 0ull) {
        failures.push_back({
            {"code", "missing_temporal_history_memory"},
            {"reason", "At least one temporal feature is active, but the renderer reported zero temporal history memory."},
        });
    }
    if (anyTemporalFeatureActive && !temporalSystemDiagnostics.value("available", false)) {
        failures.push_back({
            {"code", "missing_temporal_system"},
            {"reason", "At least one temporal feature is active, but TemporalSystem diagnostics are unavailable."},
        });
    }
    if (!missingRequiredHistorySlots.empty()) {
        failures.push_back({
            {"code", "missing_required_temporal_history_slots"},
            {"missing_slot_count", missingRequiredHistorySlots.size()},
        });
    }
    if (!unreadyRequiredHistorySlots.empty()) {
        warnings.push_back({
            {"code", "unready_required_temporal_history_slots"},
            {"reason", "Required TemporalSystem slots exist but are not valid/resident yet; this can be expected on startup or immediately after a reset."},
            {"slot_count", unreadyRequiredHistorySlots.size()},
        });
    }

    const bool nonCameraResetInvalidatesHistoryAtReset =
        lastResetReason != "CameraMoved";
    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"last_accumulation_reset_reason", lastResetReason},
        {"reset_reason_known", resetReasonKnown},
        {"frame_count", frameCount},
        {"profiled_frames", profiledFrames},
        {"temporal_history_bytes", temporalHistoryBytes},
        {"temporal_system_available", temporalSystemDiagnostics.value("available", false)},
        {"temporal_system_slot_count", temporalSystemDiagnostics.value("slot_count", uint64_t{0})},
        {"any_temporal_feature_active", anyTemporalFeatureActive},
        {"non_camera_reset_invalidates_history_at_reset", nonCameraResetInvalidatesHistoryAtReset},
        {"camera_move_preserves_temporal_history_policy", lastResetReason == "CameraMoved"},
        {"required_history_slots", std::move(requiredHistorySlots)},
        {"missing_required_history_slots", std::move(missingRequiredHistorySlots)},
        {"unready_required_history_slots", std::move(unreadyRequiredHistorySlots)},
        {"features", std::move(features)},
        {"failures", std::move(failures)},
        {"warnings", std::move(warnings)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererApplicationBridgeContractJson() {
    return nlohmann::json{
        {"schema_version", 1},
        {"surface_data", rendererContractArray({
            "world position",
            "geometric normal",
            "shading normal",
            "view vector",
            "material id",
            "roughness",
            "metallic",
            "base color",
            "emissive",
            "object/instance id",
            "motion vector",
            "validity mask",
        })},
        {"previous_surface_data", rendererContractArray({
            "previous world position",
            "previous normal",
            "previous material id",
            "previous object/instance id",
            "previous depth",
            "previous validity",
        })},
        {"light_identity", {
            {"analytic_lights", "stable authored light id plus generation"},
            {"emissive_triangles", "stable mesh/primitive/material identity plus generation"},
            {"environment_samples", "environment distribution generation plus source kind"},
            {"deleted_or_unmapped", "reject temporal reuse and mark explicit invalid reason"},
        }},
        {"visibility", {
            {"ray_origin_bias", "RendererSettings shadowRayBias/shadowDistanceBias policy"},
            {"backface_policy", "hardware backface culling is explicit and scene-dependent"},
            {"alpha_test_policy", "any-hit visibility honors alpha-tested material state"},
            {"transparent_policy", "transparent/blended materials are visibility-policy inputs, not implicit opaque hits"},
        }},
        {"validation", rendererContractArray({
            "normal length",
            "roughness range",
            "motion vector range",
            "depth validity",
            "material id validity",
            "light id validity",
        })},
    };
}

inline nlohmann::json rendererApplicationBridgeRuntimeValidationJson(
    const RendererSettings& settings,
    const nlohmann::json& temporalSystemDiagnostics,
    const nlohmann::json& rayTracingGeometry,
    const nlohmann::json& sceneLights,
    const nlohmann::json& rayTracingDiagnosticCounters,
    const nlohmann::json& accelerationStructureDiagnostics,
    const nlohmann::json& restirDiDiagnostics,
    const nlohmann::json& nvidiaIntegrations) {
    const bool restirDiActive = passes::RestirDIPass::isAnyModeActive(settings);
    const bool restirGiActive = passes::RestirGIPass::isActive(settings);
    const bool regirActive = passes::RegirPass::isActive(settings);
    const bool denoiserActive = settings.denoiserEnabled;
    const bool nrdActive = passes::NrdPass::isActive(settings);
    const bool taaActive = passes::TemporalAAPass::isActive(settings);
    const bool dlssActive = passes::DlssPass::isActive(settings);
    const bool anyTemporalBridgeActive =
        restirDiActive || restirGiActive || regirActive || denoiserActive || nrdActive || taaActive || dlssActive;

    auto hasField = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field);
    };
    auto hasBool = [&](const nlohmann::json& object, const char* field) {
        return hasField(object, field) && object[field].is_boolean();
    };
    auto hasNumber = [&](const nlohmann::json& object, const char* field) {
        return hasField(object, field) && object[field].is_number();
    };
    auto numberValue = [&](const nlohmann::json& object, const char* field) -> uint64_t {
        if (!hasNumber(object, field)) {
            return 0ull;
        }
        if (object[field].is_number_unsigned()) {
            return object[field].get<uint64_t>();
        }
        if (object[field].is_number_integer()) {
            const int64_t signedValue = object[field].get<int64_t>();
            return signedValue > 0 ? static_cast<uint64_t>(signedValue) : 0ull;
        }
        return static_cast<uint64_t>(std::max(0.0, object[field].get<double>()));
    };
    auto temporalSlotExists = [&](const char* slotName) {
        if (!temporalSystemDiagnostics.is_object() ||
            !temporalSystemDiagnostics.value("available", false) ||
            !temporalSystemDiagnostics.contains("slots") ||
            !temporalSystemDiagnostics["slots"].is_array()) {
            return false;
        }
        for (const nlohmann::json& slot : temporalSystemDiagnostics["slots"]) {
            if (slot.is_object() && slot.value("name", std::string{}) == slotName) {
                return true;
            }
        }
        return false;
    };
    auto stringArrayContains = [](const nlohmann::json& values, const char* required) {
        if (!values.is_array()) {
            return false;
        }
        for (const nlohmann::json& value : values) {
            if (value.is_string() && value.get<std::string>() == required) {
                return true;
            }
        }
        return false;
    };

    nlohmann::json checks = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    auto recordCheck = [&](const char* name,
                           bool required,
                           bool passed,
                           const char* message,
                           nlohmann::json evidence = nlohmann::json::object()) {
        checks.push_back({
            {"name", name},
            {"required", required},
            {"passed", passed},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
        if (required && !passed) {
            failures.push_back({
                {"code", name},
                {"message", message},
            });
        } else if (!required && !passed) {
            warnings.push_back({
                {"code", name},
                {"message", message},
            });
        }
    };

    const uint64_t opaquePrimitiveCount = numberValue(rayTracingGeometry, "opaque_primitive_count");
    const uint64_t alphaTestedPrimitiveCount = numberValue(rayTracingGeometry, "alpha_tested_primitive_count");
    const uint64_t blendedPrimitiveCount = numberValue(rayTracingGeometry, "blended_primitive_count");
    const uint64_t totalPrimitiveClasses =
        opaquePrimitiveCount + alphaTestedPrimitiveCount + blendedPrimitiveCount;
    const uint64_t geometryTriangleCount =
        numberValue(rayTracingGeometry, "opaque_triangle_count") +
        numberValue(rayTracingGeometry, "alpha_tested_triangle_count") +
        numberValue(rayTracingGeometry, "blended_triangle_count");

    recordCheck(
        "surface_geometry_profile_present",
        settings.pathTracingEnabled,
        rayTracingGeometry.is_object() &&
            hasNumber(rayTracingGeometry, "opaque_triangle_count") &&
            hasNumber(rayTracingGeometry, "alpha_tested_triangle_count") &&
            hasNumber(rayTracingGeometry, "blended_triangle_count") &&
            (geometryTriangleCount > 0ull || numberValue(accelerationStructureDiagnostics, "triangle_count") > 0ull),
        "Path tracing profiles must expose surface geometry class counts for the renderer-application bridge.",
        {
            {"geometry_triangle_count", geometryTriangleCount},
            {"acceleration_structure_triangle_count", numberValue(accelerationStructureDiagnostics, "triangle_count")},
        });
    recordCheck(
        "surface_material_classes_reported",
        settings.pathTracingEnabled,
        rayTracingGeometry.is_object() &&
            hasNumber(rayTracingGeometry, "opaque_primitive_count") &&
            hasNumber(rayTracingGeometry, "alpha_tested_primitive_count") &&
            hasNumber(rayTracingGeometry, "blended_primitive_count"),
        "Surface material classes must be explicit so visibility and guide generation can distinguish opaque, alpha-tested, and blended surfaces.",
        {
            {"opaque_primitive_count", opaquePrimitiveCount},
            {"alpha_tested_primitive_count", alphaTestedPrimitiveCount},
            {"blended_primitive_count", blendedPrimitiveCount},
        });
    recordCheck(
        "visibility_backface_policy_explicit",
        settings.pathTracingEnabled,
        hasBool(rayTracingGeometry, "hardware_backface_culling_enabled") ||
            hasBool(accelerationStructureDiagnostics, "hardware_backface_culling_enabled"),
        "The RAB visibility bridge must report whether hardware backface culling is active.",
        {
            {"ray_tracing_geometry_has_policy", hasBool(rayTracingGeometry, "hardware_backface_culling_enabled")},
            {"acceleration_structure_has_policy", hasBool(accelerationStructureDiagnostics, "hardware_backface_culling_enabled")},
        });
    recordCheck(
        "visibility_alpha_policy_observable",
        alphaTestedPrimitiveCount > 0ull || blendedPrimitiveCount > 0ull,
        rayTracingDiagnosticCounters.is_object() &&
            hasNumber(rayTracingDiagnosticCounters, "primary_any_hit_alpha_tested") &&
            hasNumber(rayTracingDiagnosticCounters, "primary_any_hit_blended") &&
            hasNumber(rayTracingDiagnosticCounters, "terminal_any_hit_alpha_tested") &&
            hasNumber(rayTracingDiagnosticCounters, "terminal_any_hit_blended"),
        "Scenes with alpha-tested or blended geometry should expose any-hit visibility counters for bridge debugging.",
        {
            {"alpha_tested_primitive_count", alphaTestedPrimitiveCount},
            {"blended_primitive_count", blendedPrimitiveCount},
            {"primary_any_hit_alpha_tested", numberValue(rayTracingDiagnosticCounters, "primary_any_hit_alpha_tested")},
            {"primary_any_hit_blended", numberValue(rayTracingDiagnosticCounters, "primary_any_hit_blended")},
        });
    recordCheck(
        "scene_light_records_reported",
        restirDiActive || restirGiActive || regirActive,
        sceneLights.is_object() &&
            hasNumber(sceneLights, "record_count") &&
            hasNumber(sceneLights, "emissive_count") &&
            hasNumber(sceneLights, "authored_count"),
        "Lighting reuse modes must report scene light record counts and source classes.",
        {
            {"record_count", numberValue(sceneLights, "record_count")},
            {"emissive_count", numberValue(sceneLights, "emissive_count")},
            {"authored_count", numberValue(sceneLights, "authored_count")},
        });

    const nlohmann::json restirLightHistoryMapping =
        restirDiDiagnostics.is_object() && restirDiDiagnostics.contains("light_history_mapping")
            ? restirDiDiagnostics["light_history_mapping"]
            : nlohmann::json::object();
    recordCheck(
        "restir_di_light_history_mapping_contract",
        restirDiActive,
        restirLightHistoryMapping.is_object() &&
            restirLightHistoryMapping.value("cached_index_fast_path", false) &&
            restirLightHistoryMapping.value("stable_identity_remap_on_cached_mismatch", false) &&
            stringArrayContains(restirLightHistoryMapping.value("stable_identity_fields", nlohmann::json::array()), "identity_hash") &&
            stringArrayContains(restirLightHistoryMapping.value("stable_identity_fields", nlohmann::json::array()), "identity_generation") &&
            stringArrayContains(restirLightHistoryMapping.value("stable_identity_fields", nlohmann::json::array()), "light_kind"),
        "ReSTIR DI must isolate light identity remapping behind a stable application-bridge contract.",
        restirLightHistoryMapping);
    const nlohmann::json directLightOwnership =
        restirDiDiagnostics.is_object() && restirDiDiagnostics.contains("direct_light_ownership")
            ? restirDiDiagnostics["direct_light_ownership"]
            : nlohmann::json::object();
    recordCheck(
        "restir_di_direct_light_ownership_contract",
        restirDiActive,
        directLightOwnership.is_object() &&
            hasField(directLightOwnership, "emissive_and_analytic_lights") &&
            hasField(directLightOwnership, "sun") &&
            hasField(directLightOwnership, "environment"),
        "Direct light ownership must state which light classes are owned by ReSTIR DI versus classic NEE.",
        directLightOwnership);
    recordCheck(
        "previous_surface_temporal_slot",
        anyTemporalBridgeActive,
        temporalSlotExists("previous_world_position"),
        "Temporal consumers must share an explicit previous-world-position bridge slot.",
        {
            {"temporal_system_available", temporalSystemDiagnostics.value("available", false)},
            {"slot_count", temporalSystemDiagnostics.value("slot_count", uint64_t{0})},
        });
    const nlohmann::json nrdGuideContract =
        nvidiaIntegrations.is_object() && nvidiaIntegrations.contains("nrd_guide_contract")
            ? nvidiaIntegrations["nrd_guide_contract"]
            : nlohmann::json::object();
    recordCheck(
        "nrd_guide_contract",
        nrdActive,
        nrdGuideContract.is_object() &&
            nrdGuideContract.value("motion_vectors", false) &&
            nrdGuideContract.value("normal_roughness", false) &&
            nrdGuideContract.value("view_z", false),
        "NRD mode must expose motion, normal/roughness, and view-Z guide contract evidence.",
        nrdGuideContract);
    const nlohmann::json dlssGuideContract =
        nvidiaIntegrations.is_object() && nvidiaIntegrations.contains("dlss_guide_contract")
            ? nvidiaIntegrations["dlss_guide_contract"]
            : nlohmann::json::object();
    recordCheck(
        "dlss_guide_contract",
        passes::DlssPass::isUpscaleActive(settings),
        dlssGuideContract.is_object() &&
            hasField(dlssGuideContract, "depth_convention") &&
            hasField(dlssGuideContract, "motion_convention") &&
            hasField(dlssGuideContract, "jitter_convention") &&
            dlssGuideContract.contains("tagged_resources") &&
            dlssGuideContract["tagged_resources"].is_array(),
        "DLSS upscale mode must expose depth, motion, jitter, and resource tagging guide evidence.",
        dlssGuideContract);
    const nlohmann::json dlssRrGuideContract =
        nvidiaIntegrations.is_object() && nvidiaIntegrations.contains("dlss_ray_reconstruction_guide_contract")
            ? nvidiaIntegrations["dlss_ray_reconstruction_guide_contract"]
            : nlohmann::json::object();
    const nlohmann::json dlssRrGuideValidator =
        dlssRrGuideContract.is_object() && dlssRrGuideContract.contains("guide_consistency_validator")
            ? dlssRrGuideContract["guide_consistency_validator"]
            : nlohmann::json::object();
    recordCheck(
        "dlss_ray_reconstruction_guide_contract",
        settings.dlssRayReconstructionEnabled,
        dlssRrGuideContract.is_object() &&
            dlssRrGuideContract.contains("guide_images") &&
            dlssRrGuideContract["guide_images"].is_array() &&
            dlssRrGuideValidator.value("previous_world_position", false) &&
            dlssRrGuideValidator.value("material_id", false) &&
            dlssRrGuideValidator.value("instance_id", false),
        "DLSS Ray Reconstruction must expose guide images plus previous-surface/material/instance consistency evidence.",
        dlssRrGuideContract);

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"contract", "renderer_application_bridge"},
        {"feature_state", {
            {"path_tracing", settings.pathTracingEnabled},
            {"restir_di", restirDiActive},
            {"restir_gi", restirGiActive},
            {"regir", regirActive},
            {"denoiser", denoiserActive},
            {"nrd", nrdActive},
            {"taa_tsr", taaActive},
            {"dlss", dlssActive},
            {"dlss_ray_reconstruction", settings.dlssRayReconstructionEnabled},
        }},
        {"surface_class_totals", {
            {"opaque_primitive_count", opaquePrimitiveCount},
            {"alpha_tested_primitive_count", alphaTestedPrimitiveCount},
            {"blended_primitive_count", blendedPrimitiveCount},
            {"total_classified_primitive_count", totalPrimitiveClasses},
        }},
        {"check_count", checks.size()},
        {"failure_count", failures.size()},
        {"warning_count", warnings.size()},
        {"checks", std::move(checks)},
        {"failures", std::move(failures)},
        {"warnings", std::move(warnings)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererSupportedModeMatrixJson() {
    auto row = [](const char* name,
                  const char* status,
                  nlohmann::json args,
                  const char* referencePath,
                  const char* notes) {
        return nlohmann::json{
            {"name", name},
            {"status", status},
            {"canonical_args", std::move(args)},
            {"reference_or_gate", referencePath},
            {"notes", notes},
        };
    };

    return nlohmann::json{
        {"schema_version", 1},
        {"failure_policy", rendererContractArray({
            "image diff above threshold fails",
            "new NaN/Inf or invalid reservoir count fails",
            "missing required debug output fails",
            "missing timing section fails",
            "unsupported feature combinations must fail clearly",
        })},
        {"modes", nlohmann::json::array({
            row("path_tracing_only", "supported-diagnostic",
                rendererContractArray({"--restir-di", "off", "--restir-gi", "off", "--regir", "off", "--disable-pass", "Denoiser", "--disable-pass", "TAA"}),
                "scripts/render_compare_references.ps1",
                "Use for ground-truth and renderer-core regressions."),
            row("restir_di_only", "supported",
                rendererContractArray({"--restir-di", "production", "--restir-gi", "off", "--regir", "off"}),
                "scripts/restir_reference_matrix.ps1 -ModeSet di",
                "DI temporal/spatial/final plus counters and debug views."),
            row("restir_gi", "supported",
                rendererContractArray({"--restir-di", "off", "--restir-gi", "production", "--regir", "off"}),
                "scripts/renderer_quality_validation_matrix.ps1 -Mode restir_gi",
                "GI must not depend on DI execution."),
            row("restir_di_gi", "supported-default",
                rendererContractArray({"--restir-di", "production", "--restir-gi", "production"}),
                "scripts/renderer_quality_validation_matrix.ps1 -Mode restir_di_gi",
                "Default real-time beauty path."),
            row("restir_di_regir", "supported",
                rendererContractArray({"--restir-di", "production", "--restir-gi", "off", "--regir", "on"}),
                "scripts/renderer_quality_validation_matrix.ps1 -Mode restir_di_regir",
                "ReGIR is a sampling service and must remain optional."),
            row("engine_denoiser_taa", "supported-default",
                rendererContractArray({"--denoiser", "on", "--denoiser-backend", "engine", "--temporal-upscaler", "taa-tsr"}),
                "scripts/renderer_quality_validation_matrix.ps1 -Mode engine_denoiser_taa",
                "Current safe denoiser/upscaler policy default."),
            row("nrd_enabled", "runtime-gated",
                rendererContractArray({"--denoiser", "on", "--denoiser-backend", "nrd"}),
                "scripts/renderer_quality_validation_matrix.ps1 -Mode nrd_enabled",
                "Falls back when NRD SDK/runtime is unavailable; profile must report reason."),
            row("dlss_enabled", "runtime-gated",
                rendererContractArray({"--temporal-upscaler", "dlss", "--dlss", "on"}),
                "scripts/renderer_quality_validation_matrix.ps1 -Mode dlss_enabled",
                "Falls back when DLSS is unavailable; guide tags and evaluation status must be reported."),
            row("dlss_ray_reconstruction", "runtime-gated-experimental",
                rendererContractArray({"--temporal-upscaler", "dlss", "--dlss", "on", "--dlss-rr", "on"}),
                "scripts/renderer_quality_validation_matrix.ps1 -Mode dlss_ray_reconstruction",
                "Falls back when DLSS RR is unavailable; guide contracts and unavailable reason must be reported."),
        })},
    };
}

inline nlohmann::json rendererCurrentModeSupportJson(const RendererSettings& settings) {
    const bool restirDiActive = passes::RestirDIPass::isAnyModeActive(settings);
    const bool restirGiActive = passes::RestirGIPass::isActive(settings);
    const bool regirActive = passes::RegirPass::isActive(settings);
    const bool engineDenoiserActive = passes::DenoiserPass::isActive(settings);
    const bool nrdActive = passes::NrdPass::isActive(settings);
    const bool dlssActive = passes::DlssPass::isActive(settings);
    const bool dlssRayReconstructionActive = passes::DlssPass::isRayReconstructionActive(settings);
    const bool taaTsrActive = passes::TemporalAAPass::isActive(settings);

    nlohmann::json matchedModes = nlohmann::json::array();
    nlohmann::json unsupportedReasons = nlohmann::json::array();
    nlohmann::json experimentalNotes = nlohmann::json::array();

    auto match = [&](const char* name, const char* reason) {
        matchedModes.push_back({
            {"name", name},
            {"reason", reason},
        });
    };
    auto unsupported = [&](const char* code, const char* reason) {
        unsupportedReasons.push_back({
            {"code", code},
            {"reason", reason},
        });
    };
    auto experimental = [&](const char* code, const char* reason) {
        experimentalNotes.push_back({
            {"code", code},
            {"reason", reason},
        });
    };

    if (!settings.pathTracingEnabled) {
        unsupported("path_tracing_disabled", "The RTXDI-quality renderer mode matrix assumes path tracing is the active renderer backend.");
    }
    if (!restirDiActive && !restirGiActive && !regirActive && !settings.denoiserEnabled && !taaTsrActive && !dlssActive) {
        match("path_tracing_only", "ReSTIR, ReGIR, denoising, TAA/TSR, and DLSS are inactive.");
    }
    if (restirDiActive && !restirGiActive && !regirActive) {
        match("restir_di_only", "ReSTIR DI is active without ReSTIR GI or ReGIR.");
    }
    if (!restirDiActive && restirGiActive && !regirActive) {
        match("restir_gi", "ReSTIR GI is active without ReSTIR DI or ReGIR.");
    }
    if (restirDiActive && restirGiActive) {
        match("restir_di_gi", "ReSTIR DI and ReSTIR GI are active together.");
    }
    if (restirDiActive && regirActive) {
        match("restir_di_regir", "ReSTIR DI is active with ReGIR light sampling.");
    } else if (regirActive) {
        experimental("regir_without_restir_di", "The current supported matrix documents ReGIR with ReSTIR DI; this combination should be treated as experimental until a dedicated gate exists.");
    }
    if (nrdActive) {
        match("nrd_enabled", "NRD denoiser backend is requested while denoising is enabled.");
    }
    if (dlssActive) {
        match("dlss_enabled", "DLSS temporal upscaler or DLSS Ray Reconstruction is requested.");
    }
    if (dlssRayReconstructionActive) {
        match("dlss_ray_reconstruction", "DLSS Ray Reconstruction is requested.");
    }
    if (engineDenoiserActive && taaTsrActive && !nrdActive && !dlssActive) {
        match("engine_denoiser_taa", "Engine denoiser and TAA/TSR temporal path are active.");
    }
    if (settings.temporalUpscaler == TemporalUpscaler::Nis) {
        experimental("nis_temporal_upscaler", "NIS is not part of the current RTXDI-quality supported mode matrix.");
    }
    if (passes::DlssPass::isRayReconstructionActive(settings) && !passes::DlssPass::isUpscaleActive(settings)) {
        unsupported("dlss_rr_without_dlss_upscaler", "DLSS Ray Reconstruction requires the DLSS upscaler contract for guide tagging and SDK evaluation.");
    }
    if (settings.lightingReuseMode == LightingReuseMode::ExperimentalRestirPT ||
        settings.lightingReuseMode == LightingReuseMode::ValidateRestirPTAgainstLegacy) {
        experimental("restir_pt_lighting_reuse", "ReSTIR PT modes are experimental and need a dedicated supported-mode gate before they can be promoted.");
    }

    const bool passed = unsupportedReasons.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"settings_snapshot", rendererSettingsSnapshotJson(settings)},
        {"feature_state", {
            {"path_tracing", settings.pathTracingEnabled},
            {"restir_di", restirDiActive},
            {"restir_gi", restirGiActive},
            {"regir", regirActive},
            {"denoiser", settings.denoiserEnabled},
            {"nrd", nrdActive},
            {"dlss", dlssActive},
            {"taa_tsr", taaTsrActive},
            {"dlss_ray_reconstruction", settings.dlssRayReconstructionEnabled},
        }},
        {"matched_modes", std::move(matchedModes)},
        {"unsupported_reasons", std::move(unsupportedReasons)},
        {"experimental_notes", std::move(experimentalNotes)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererActivePassRuntimeValidationJson(
    const RendererSettings& settings,
    const nlohmann::json& activePasses,
    const nlohmann::json& perPassGpuMs,
    const nlohmann::json& restirDiDiagnostics,
    const nlohmann::json& restirGiDiagnostics,
    const nlohmann::json& temporalRuntimeValidation,
    const nlohmann::json& nvidiaIntegrations) {
    const bool restirDiActive = passes::RestirDIPass::isActive(settings);
    const bool restirGiActive = passes::RestirGIPass::isActive(settings);
    const bool regirActive = passes::RegirPass::isActive(settings);
    const bool engineDenoiserActive = passes::DenoiserPass::isActive(settings);
    const bool taaActive = passes::TemporalAAPass::isActive(settings);
    const bool effectiveDlssRayReconstruction =
        nvidiaIntegrations.is_object() &&
        nvidiaIntegrations.contains("effective_dlss_ray_reconstruction") &&
        nvidiaIntegrations["effective_dlss_ray_reconstruction"].is_boolean() &&
        nvidiaIntegrations["effective_dlss_ray_reconstruction"].get<bool>();
    const std::string effectiveTemporalUpscaler =
        nvidiaIntegrations.is_object() &&
            nvidiaIntegrations.contains("effective_temporal_upscaler") &&
            nvidiaIntegrations["effective_temporal_upscaler"].is_string()
        ? nvidiaIntegrations["effective_temporal_upscaler"].get<std::string>()
        : std::string{};
    const bool engineDenoiserRunExpected = engineDenoiserActive && !effectiveDlssRayReconstruction;
    const bool dlssUpscaleRunExpected = effectiveTemporalUpscaler == "dlss" && !effectiveDlssRayReconstruction;
    const bool dlssRayReconstructionRunExpected = effectiveDlssRayReconstruction;

    auto activePass = [&](const char* name) {
        return activePasses.is_object() && activePasses.value(name, false);
    };
    auto timedPass = [&](const char* name) {
        return perPassGpuMs.is_object() &&
            perPassGpuMs.contains(name) &&
            perPassGpuMs[name].is_number() &&
            perPassGpuMs[name].get<double>() > 0.0;
    };
    auto diagnosticActive = [](const nlohmann::json& diagnostics, const char* passName) {
        return diagnostics.is_object() &&
            diagnostics.contains("active_passes") &&
            diagnostics["active_passes"].is_object() &&
            diagnostics["active_passes"].value(passName, false);
    };
    auto temporalFeatureReady = [&](const char* featureName) {
        if (!temporalRuntimeValidation.is_object() ||
            !temporalRuntimeValidation.contains("features") ||
            !temporalRuntimeValidation["features"].is_array()) {
            return false;
        }
        for (const nlohmann::json& feature : temporalRuntimeValidation["features"]) {
            if (!feature.is_object() || feature.value("name", std::string{}) != featureName) {
                continue;
            }
            return feature.value("active", false) &&
                feature.contains("history_ready") &&
                feature["history_ready"].is_boolean() &&
                feature["history_ready"].get<bool>();
        }
        return false;
    };

    nlohmann::json checks = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    auto addCheck = [&](const char* name,
                        bool required,
                        bool passed,
                        const char* message,
                        nlohmann::json evidence = nlohmann::json::object()) {
        checks.push_back({
            {"name", name},
            {"required", required},
            {"passed", passed},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
        if (required && !passed) {
            failures.push_back({
                {"code", name},
                {"message", message},
            });
        } else if (!required && !passed) {
            warnings.push_back({
                {"code", name},
                {"message", message},
            });
        }
    };
    auto requireActive = [&](const char* featureName,
                             bool required,
                             const char* passKey,
                             const char* timingKey,
                             bool diagnosticEvidence = false) {
        const bool active = activePass(passKey);
        const bool timed = timingKey != nullptr && timingKey[0] != '\0' && timedPass(timingKey);
        addCheck(
            featureName,
            required,
            !required || active || timed || diagnosticEvidence,
            "Requested feature pass must have active-pass, timing, or subsystem diagnostic evidence.",
            {
                {"pass_key", passKey},
                {"timing_key", timingKey == nullptr ? "" : timingKey},
                {"active_pass", active},
                {"timed", timed},
                {"diagnostic_evidence", diagnosticEvidence},
            });
    };
    auto requireInactive = [&](const char* featureName, bool shouldBeInactive, nlohmann::json passKeys) {
        nlohmann::json activeKeys = nlohmann::json::array();
        if (passKeys.is_array()) {
            for (const nlohmann::json& keyValue : passKeys) {
                if (keyValue.is_string()) {
                    const std::string key = keyValue.get<std::string>();
                    if (activePass(key.c_str())) {
                        activeKeys.push_back(key);
                    }
                }
            }
        }
        addCheck(
            featureName,
            shouldBeInactive,
            !shouldBeInactive || activeKeys.empty(),
            "Disabled feature must not leave active renderer pass evidence.",
            {
                {"active_keys", std::move(activeKeys)},
                {"pass_keys", std::move(passKeys)},
            });
    };

    requireActive("path_trace_pass_active", settings.pathTracingEnabled, "path_trace", "path_trace");
    requireActive("restir_di_final_pass_active", restirDiActive, "restir_di_final", "restir_di_final", diagnosticActive(restirDiDiagnostics, "final"));
    requireActive("restir_di_temporal_pass_active", restirDiActive && settings.restirDiTemporalEnabled, "restir_di_temporal", "restir_di_temporal", diagnosticActive(restirDiDiagnostics, "temporal"));
    requireActive("restir_di_spatial_pass_active", restirDiActive && settings.restirDiSpatialEnabled && settings.restirDiSpatialRounds > 0u, "restir_di_spatial", "restir_di_spatial", diagnosticActive(restirDiDiagnostics, "spatial"));
    requireActive("restir_gi_temporal_pass_active", restirGiActive, "restir_gi_temporal", "restir_gi_temporal", diagnosticActive(restirGiDiagnostics, "temporal"));
    requireActive("restir_gi_final_pass_active", restirGiActive, "restir_gi_final", "restir_gi_final", diagnosticActive(restirGiDiagnostics, "final"));
    requireActive("restir_gi_spatial_pass_active", restirGiActive && settings.restirGiSpatialRounds > 0u, "restir_gi_spatial", "restir_gi_spatial", diagnosticActive(restirGiDiagnostics, "spatial"));
    requireActive("restir_gi_upsample_pass_active", restirGiActive && settings.restirGiHalfResolution, "restir_gi_upsample", "restir_gi_upsample", diagnosticActive(restirGiDiagnostics, "upsample"));
    requireActive("regir_pass_active", regirActive, "regir", "regir_build");
    requireActive("regir_temporal_reuse_pass_active", regirActive && settings.regirTemporalReuse && settings.regirGridMode != RegirGridMode::Hash, "regir_temporal_reuse", "regir_temporal_reuse");
    requireActive("regir_spatial_reuse_pass_active", regirActive && settings.regirSpatialReuse && settings.regirGridMode != RegirGridMode::Hash, "regir_spatial_reuse", "regir_spatial_reuse");
    requireActive("engine_denoiser_pass_active", engineDenoiserRunExpected, "denoiser", "denoiser", temporalFeatureReady("denoiser"));
    requireActive("moment_update_pass_active", engineDenoiserRunExpected, "moment_update", "moment_update", temporalFeatureReady("denoiser"));
    requireActive("taa_pass_active", taaActive, "taa", "taa", temporalFeatureReady("taa_tsr"));
    requireActive("taa_history_copy_pass_active", taaActive, "taa_history_copy", "taa_history_copy", temporalFeatureReady("taa_tsr"));
    requireActive("dlss_guides_pass_active", dlssUpscaleRunExpected, "dlss_guides", "dlss_guides");
    requireActive("dlss_pass_active", dlssUpscaleRunExpected, "dlss", "dlss");
    requireActive("dlss_rr_guides_pass_active", dlssRayReconstructionRunExpected, "dlss_rr_guides", "dlss_rr_guides");
    requireActive("dlss_rr_pass_active", dlssRayReconstructionRunExpected, "dlss_rr", "dlss_rr");

    requireInactive(
        "restir_di_disabled_passes_inactive",
        !restirDiActive,
        rendererContractArray({"restir_di_temporal", "restir_di_spatial", "restir_di_final"}));
    requireInactive(
        "restir_gi_disabled_passes_inactive",
        !restirGiActive,
        rendererContractArray({"restir_gi_temporal", "restir_gi_spatial", "restir_gi_final", "restir_gi_upsample"}));
    requireInactive(
        "regir_disabled_passes_inactive",
        !regirActive,
        rendererContractArray({"regir", "regir_temporal_reuse", "regir_spatial_reuse", "regir_visibility_reuse", "regir_environment", "regir_sun"}));
    requireInactive(
        "engine_denoiser_disabled_passes_inactive",
        !engineDenoiserActive,
        rendererContractArray({"denoiser", "moment_update"}));
    requireInactive(
        "taa_disabled_passes_inactive",
        !taaActive,
        rendererContractArray({"taa", "taa_history_copy"}));

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"active_passes_available", activePasses.is_object()},
        {"per_pass_gpu_ms_available", perPassGpuMs.is_object()},
        {"feature_state", {
            {"path_tracing", settings.pathTracingEnabled},
            {"restir_di", restirDiActive},
            {"restir_gi", restirGiActive},
            {"regir", regirActive},
            {"engine_denoiser", engineDenoiserActive},
            {"engine_denoiser_run_expected", engineDenoiserRunExpected},
            {"taa_tsr", taaActive},
            {"dlss_upscale_run_expected", dlssUpscaleRunExpected},
            {"effective_dlss_ray_reconstruction", effectiveDlssRayReconstruction},
            {"effective_temporal_upscaler", effectiveTemporalUpscaler},
        }},
        {"check_count", checks.size()},
        {"failure_count", failures.size()},
        {"warning_count", warnings.size()},
        {"checks", std::move(checks)},
        {"failures", std::move(failures)},
        {"warnings", std::move(warnings)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererRestirReservoirRuntimeValidationJson(
    const RendererSettings& settings,
    const nlohmann::json& restirDiDiagnostics,
    const nlohmann::json& restirGiDiagnostics) {
    const bool restirDiRequired = passes::RestirDIPass::isActive(settings);
    const bool restirGiRequired = passes::RestirGIPass::isActive(settings);

    auto contractBlock = [](const nlohmann::json& diagnostics) {
        return diagnostics.is_object() && diagnostics.contains("reservoir_contract_validation")
            ? diagnostics["reservoir_contract_validation"]
            : nlohmann::json::object();
    };
    auto numberValue = [](const nlohmann::json& object, const char* field) -> uint64_t {
        if (!object.is_object() || !object.contains(field) || !object[field].is_number()) {
            return 0ull;
        }
        if (object[field].is_number_unsigned()) {
            return object[field].get<uint64_t>();
        }
        if (object[field].is_number_integer()) {
            const int64_t value = object[field].get<int64_t>();
            return value > 0 ? static_cast<uint64_t>(value) : 0ull;
        }
        return static_cast<uint64_t>(std::max(0.0, object[field].get<double>()));
    };

    const nlohmann::json diContract = contractBlock(restirDiDiagnostics);
    const nlohmann::json giContract = contractBlock(restirGiDiagnostics);

    nlohmann::json checks = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    auto addCheck = [&](const char* name,
                        bool required,
                        bool passed,
                        const char* message,
                        nlohmann::json evidence = nlohmann::json::object()) {
        checks.push_back({
            {"name", name},
            {"required", required},
            {"passed", passed},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
        if (required && !passed) {
            failures.push_back({
                {"code", name},
                {"message", message},
            });
        } else if (!required && !passed) {
            warnings.push_back({
                {"code", name},
                {"message", message},
            });
        }
    };
    auto validateContract = [&](const char* name, bool required, const nlohmann::json& contract) {
        const bool present = contract.is_object() && !contract.empty();
        const bool active = present && contract.value("active", false);
        const bool checked = present && contract.value("checked", false);
        const bool passed = present && contract.value("passed", false);
        const uint64_t violationCount = numberValue(contract, "violation_count");
        const uint64_t invalidSourcePdf = numberValue(contract, "invalid_source_pdf_count");
        const uint64_t invalidTargetPdf = numberValue(contract, "invalid_target_pdf_count");
        const uint64_t nonFinite = numberValue(contract, "non_finite_count");

        addCheck(
            name,
            required,
            !required || (present && active && checked && passed && violationCount == 0ull),
            "Active ReSTIR reservoir mode must be checked and free of contract, PDF, target, parity, or non-finite violations.",
            {
                {"present", present},
                {"active", active},
                {"checked", checked},
                {"passed", passed},
                {"violation_count", violationCount},
                {"invalid_source_pdf_count", invalidSourcePdf},
                {"invalid_target_pdf_count", invalidTargetPdf},
                {"non_finite_count", nonFinite},
                {"contract", contract},
            });
        addCheck(
            std::string(name).append("_inactive_contract_clean").c_str(),
            !required,
            required || !present || passed,
            "Inactive ReSTIR reservoir contract blocks must not report latent failures.",
            {
                {"present", present},
                {"active", active},
                {"checked", checked},
                {"passed", passed},
                {"violation_count", violationCount},
            });
    };

    validateContract("restir_di_reservoir_contract", restirDiRequired, diContract);
    validateContract("restir_gi_reservoir_contract", restirGiRequired, giContract);

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"feature_state", {
            {"standalone_restir_di", restirDiRequired},
            {"restir_gi", restirGiRequired},
        }},
        {"check_count", checks.size()},
        {"failure_count", failures.size()},
        {"warning_count", warnings.size()},
        {"checks", std::move(checks)},
        {"failures", std::move(failures)},
        {"warnings", std::move(warnings)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererDenoiserUpscalerRuntimeValidationJson(
    const RendererSettings& settings,
    const nlohmann::json& nvidiaIntegrations,
    const nlohmann::json& temporalRuntimeValidation,
    const nlohmann::json& activePassRuntimeValidation) {
    const bool engineDenoiserRequested = passes::DenoiserPass::isActive(settings);
    const bool nrdRequested = passes::NrdPass::isActive(settings);
    const bool taaTsrRequested = passes::TemporalAAPass::isActive(settings);
    const bool dlssRequested = passes::DlssPass::isUpscaleActive(settings);
    const bool dlssRayReconstructionRequested = passes::DlssPass::isRayReconstructionActive(settings);
    const bool safeDefaultRequested =
        engineDenoiserRequested && taaTsrRequested && !dlssRayReconstructionRequested;

    auto hasField = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field);
    };
    auto hasString = [&](const nlohmann::json& object, const char* field) {
        return hasField(object, field) && object[field].is_string();
    };
    auto hasBool = [&](const nlohmann::json& object, const char* field) {
        return hasField(object, field) && object[field].is_boolean();
    };
    auto hasNumber = [&](const nlohmann::json& object, const char* field) {
        return hasField(object, field) && object[field].is_number();
    };
    auto hasArray = [&](const nlohmann::json& object, const char* field) {
        return hasField(object, field) && object[field].is_array();
    };
    auto hasNonEmptyString = [&](const nlohmann::json& object, const char* field) {
        return hasString(object, field) && !object[field].get<std::string>().empty();
    };
    auto stringValue = [&](const nlohmann::json& object, const char* field) -> std::string {
        return hasString(object, field) ? object[field].get<std::string>() : std::string{};
    };
    auto boolValue = [&](const nlohmann::json& object, const char* field) -> bool {
        return hasBool(object, field) && object[field].get<bool>();
    };
    auto objectValue = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field) && object[field].is_object()
            ? object[field]
            : nlohmann::json::object();
    };
    auto arrayContainsString = [](const nlohmann::json& values, const char* expected) {
        if (!values.is_array()) {
            return false;
        }
        for (const nlohmann::json& value : values) {
            if (value.is_string() && value.get<std::string>() == expected) {
                return true;
            }
        }
        return false;
    };
    auto taggedResourceRolePresent = [&](const nlohmann::json& guideContract, const char* role) {
        if (!hasArray(guideContract, "tagged_resources")) {
            return false;
        }
        for (const nlohmann::json& resource : guideContract["tagged_resources"]) {
            if (resource.is_object() && stringValue(resource, "role") == role) {
                return true;
            }
        }
        return false;
    };
    auto temporalFeature = [&](const char* featureName) {
        if (!temporalRuntimeValidation.is_object() ||
            !temporalRuntimeValidation.contains("features") ||
            !temporalRuntimeValidation["features"].is_array()) {
            return nlohmann::json::object();
        }
        for (const nlohmann::json& feature : temporalRuntimeValidation["features"]) {
            if (feature.is_object() && feature.value("name", std::string{}) == featureName) {
                return feature;
            }
        }
        return nlohmann::json::object();
    };
    auto temporalFeatureReady = [&](const char* featureName) {
        const nlohmann::json feature = temporalFeature(featureName);
        return feature.is_object() &&
            feature.value("active", false) &&
            feature.contains("history_ready") &&
            feature["history_ready"].is_boolean() &&
            feature["history_ready"].get<bool>();
    };
    auto activePassCheckPassed = [&](const char* checkName) {
        if (!activePassRuntimeValidation.is_object() ||
            !activePassRuntimeValidation.contains("checks") ||
            !activePassRuntimeValidation["checks"].is_array()) {
            return false;
        }
        for (const nlohmann::json& check : activePassRuntimeValidation["checks"]) {
            if (check.is_object() && check.value("name", std::string{}) == checkName) {
                return check.value("passed", false);
            }
        }
        return false;
    };
    auto availabilityReported = [&](const char* availableField, const char* reasonField) {
        return hasBool(nvidiaIntegrations, availableField) &&
            (boolValue(nvidiaIntegrations, availableField) || hasNonEmptyString(nvidiaIntegrations, reasonField));
    };

    const nlohmann::json backendPolicy = objectValue(nvidiaIntegrations, "backend_comparison_policy");
    const nlohmann::json activeMode = objectValue(backendPolicy, "active_mode");
    const nlohmann::json safeDefault = objectValue(backendPolicy, "current_safe_default");
    const nlohmann::json nrdGuideContract = objectValue(nvidiaIntegrations, "nrd_guide_contract");
    const nlohmann::json dlssGuideContract = objectValue(nvidiaIntegrations, "dlss_guide_contract");
    const nlohmann::json dlssExposureContract = objectValue(nvidiaIntegrations, "dlss_exposure_contract");
    const nlohmann::json dlssRrGuideContract = objectValue(nvidiaIntegrations, "dlss_ray_reconstruction_guide_contract");
    const nlohmann::json dlssRrValidator = objectValue(dlssRrGuideContract, "guide_consistency_validator");

    const std::string requestedDenoiser = stringValue(nvidiaIntegrations, "requested_denoiser_backend");
    const std::string effectiveDenoiser = stringValue(nvidiaIntegrations, "effective_denoiser_backend");
    const std::string requestedUpscaler = stringValue(nvidiaIntegrations, "requested_temporal_upscaler");
    const std::string effectiveUpscaler = stringValue(nvidiaIntegrations, "effective_temporal_upscaler");
    const bool requestedDlssRr = boolValue(nvidiaIntegrations, "requested_dlss_ray_reconstruction");
    const bool effectiveDlssRr = boolValue(nvidiaIntegrations, "effective_dlss_ray_reconstruction");
    const bool engineDenoiserRunExpected = engineDenoiserRequested && !effectiveDlssRr;

    nlohmann::json checks = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    auto addCheck = [&](const char* name,
                        bool passed,
                        const char* message,
                        nlohmann::json evidence = nlohmann::json::object()) {
        checks.push_back({
            {"name", name},
            {"passed", passed},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
        if (!passed) {
            failures.push_back({
                {"code", name},
                {"message", message},
            });
        }
    };

    addCheck(
        "nvidia_backend_diagnostics_present",
        nvidiaIntegrations.is_object() &&
            hasString(nvidiaIntegrations, "requested_denoiser_backend") &&
            hasString(nvidiaIntegrations, "effective_denoiser_backend") &&
            hasString(nvidiaIntegrations, "requested_temporal_upscaler") &&
            hasString(nvidiaIntegrations, "effective_temporal_upscaler") &&
            hasBool(nvidiaIntegrations, "requested_dlss_ray_reconstruction") &&
            hasBool(nvidiaIntegrations, "effective_dlss_ray_reconstruction"),
        "Profile must expose requested and effective denoiser/upscaler backends.",
        {
            {"requested_denoiser_backend", requestedDenoiser},
            {"effective_denoiser_backend", effectiveDenoiser},
            {"requested_temporal_upscaler", requestedUpscaler},
            {"effective_temporal_upscaler", effectiveUpscaler},
            {"requested_dlss_ray_reconstruction", requestedDlssRr},
            {"effective_dlss_ray_reconstruction", effectiveDlssRr},
        });
    addCheck(
        "backend_comparison_policy_present",
        backendPolicy.is_object() &&
            backendPolicy.value("schema_version", 0) == 1 &&
            activeMode.is_object() &&
            safeDefault.is_object() &&
            hasArray(backendPolicy, "fallback_order") &&
            !backendPolicy["fallback_order"].empty() &&
            hasNonEmptyString(backendPolicy, "failure_policy") &&
            hasNonEmptyString(backendPolicy, "promotion_rule"),
        "Profile must expose the Q5D backend comparison policy, fallback order, and failure/promotion rules.",
        backendPolicy);
    addCheck(
        "active_mode_matches_profile_fields",
        activeMode.is_object() &&
            stringValue(activeMode, "requested_denoiser_backend") == requestedDenoiser &&
            stringValue(activeMode, "effective_denoiser_backend") == effectiveDenoiser &&
            stringValue(activeMode, "requested_temporal_upscaler") == requestedUpscaler &&
            stringValue(activeMode, "effective_temporal_upscaler") == effectiveUpscaler &&
            boolValue(activeMode, "requested_dlss_ray_reconstruction") == requestedDlssRr &&
            boolValue(activeMode, "effective_dlss_ray_reconstruction") == effectiveDlssRr,
        "Backend policy active mode must mirror the top-level backend request/effective fields.",
        activeMode);
    addCheck(
        "requested_backends_match_settings",
        (!settings.denoiserEnabled || requestedDenoiser == denoiserBackendName(settings.denoiserBackend)) &&
            requestedUpscaler == temporalUpscalerName(settings.temporalUpscaler) &&
            requestedDlssRr == settings.dlssRayReconstructionEnabled,
        "Requested denoiser/upscaler backend diagnostics must match RendererSettings.",
        {
            {"settings_denoiser_enabled", settings.denoiserEnabled},
            {"settings_denoiser_backend", denoiserBackendName(settings.denoiserBackend)},
            {"settings_temporal_upscaler", temporalUpscalerName(settings.temporalUpscaler)},
            {"settings_dlss_ray_reconstruction_enabled", settings.dlssRayReconstructionEnabled},
            {"requested_denoiser_backend", requestedDenoiser},
            {"requested_temporal_upscaler", requestedUpscaler},
            {"requested_dlss_ray_reconstruction", requestedDlssRr},
        });
    addCheck(
        "safe_default_effective_when_requested",
        !safeDefaultRequested ||
            (effectiveDenoiser == "engine" &&
             effectiveUpscaler == "taa-tsr" &&
             !effectiveDlssRr &&
             stringValue(safeDefault, "denoiser_backend") == "engine" &&
             stringValue(safeDefault, "temporal_upscaler") == "taa-tsr" &&
             !boolValue(safeDefault, "dlss_ray_reconstruction")),
        "Engine denoiser plus TAA/TSR must remain the safe default when requested.",
        {
            {"safe_default_requested", safeDefaultRequested},
            {"current_safe_default", safeDefault},
            {"effective_denoiser_backend", effectiveDenoiser},
            {"effective_temporal_upscaler", effectiveUpscaler},
            {"effective_dlss_ray_reconstruction", effectiveDlssRr},
        });
    addCheck(
        "nrd_backends_mutually_exclusive",
        hasBool(nvidiaIntegrations, "nrd_backends_mutually_exclusive") &&
            boolValue(nvidiaIntegrations, "nrd_backends_mutually_exclusive"),
        "NRD and engine denoiser backend ownership must be mutually exclusive.",
        {{"nrd_backends_mutually_exclusive", boolValue(nvidiaIntegrations, "nrd_backends_mutually_exclusive")}});
    addCheck(
        "engine_denoiser_temporal_and_pass_ready",
        !engineDenoiserRunExpected ||
            (temporalFeatureReady("denoiser") &&
             activePassCheckPassed("engine_denoiser_pass_active") &&
             activePassCheckPassed("moment_update_pass_active")),
        "The engine denoiser path must have temporal history and active-pass evidence when it owns reconstruction.",
        {
            {"engine_denoiser_requested", engineDenoiserRequested},
            {"engine_denoiser_run_expected", engineDenoiserRunExpected},
            {"effective_dlss_ray_reconstruction", effectiveDlssRr},
            {"denoiser_temporal_feature", temporalFeature("denoiser")},
            {"engine_denoiser_pass_active_check", activePassCheckPassed("engine_denoiser_pass_active")},
            {"moment_update_pass_active_check", activePassCheckPassed("moment_update_pass_active")},
        });
    addCheck(
        "taa_tsr_temporal_and_pass_ready",
        !taaTsrRequested ||
            (temporalFeatureReady("taa_tsr") &&
             activePassCheckPassed("taa_pass_active") &&
             activePassCheckPassed("taa_history_copy_pass_active")),
        "The TAA/TSR path must have temporal history and active-pass evidence when requested.",
        {
            {"taa_tsr_requested", taaTsrRequested},
            {"taa_tsr_temporal_feature", temporalFeature("taa_tsr")},
            {"taa_pass_active_check", activePassCheckPassed("taa_pass_active")},
            {"taa_history_copy_pass_active_check", activePassCheckPassed("taa_history_copy_pass_active")},
        });
    addCheck(
        "nrd_runtime_contract_reported",
        !nrdRequested ||
            (availabilityReported("nrd_available", "nrd_unavailable_reason") &&
             nrdGuideContract.is_object() &&
             nrdGuideContract.value("motion_vectors", false) &&
             nrdGuideContract.value("normal_roughness", false) &&
             nrdGuideContract.value("view_z", false) &&
             nrdGuideContract.value("diffuse_radiance_hit_distance", false) &&
             nrdGuideContract.value("specular_radiance_hit_distance", false) &&
             (!boolValue(nvidiaIntegrations, "nrd_available") || !effectiveDenoiser.empty())),
        "Requested NRD mode must report availability/fallback reason and all guide contracts.",
        {
            {"nrd_requested", nrdRequested},
            {"nrd_available", boolValue(nvidiaIntegrations, "nrd_available")},
            {"nrd_unavailable_reason", stringValue(nvidiaIntegrations, "nrd_unavailable_reason")},
            {"effective_denoiser_backend", effectiveDenoiser},
            {"nrd_guide_contract", nrdGuideContract},
        });
    addCheck(
        "nrd_temporal_ready_when_effective",
        effectiveDenoiser != "nrd" || temporalFeatureReady("nrd"),
        "The NRD temporal feature must be ready when NRD is the effective denoiser backend.",
        {
            {"effective_denoiser_backend", effectiveDenoiser},
            {"nrd_temporal_feature", temporalFeature("nrd")},
        });
    addCheck(
        "dlss_runtime_contract_reported",
        !dlssRequested ||
            (availabilityReported("dlss_available", "dlss_unavailable_reason") &&
             dlssGuideContract.is_object() &&
             hasNonEmptyString(dlssGuideContract, "depth_convention") &&
             hasNonEmptyString(dlssGuideContract, "motion_convention") &&
             hasNonEmptyString(dlssGuideContract, "jitter_convention") &&
             taggedResourceRolePresent(dlssGuideContract, "scaling-input-color") &&
             taggedResourceRolePresent(dlssGuideContract, "scaling-output-color") &&
             taggedResourceRolePresent(dlssGuideContract, "depth") &&
             taggedResourceRolePresent(dlssGuideContract, "motion-vectors")),
        "Requested DLSS upscaling must report availability/fallback reason, guide conventions, and tagged resources.",
        {
            {"dlss_requested", dlssRequested},
            {"dlss_available", boolValue(nvidiaIntegrations, "dlss_available")},
            {"dlss_unavailable_reason", stringValue(nvidiaIntegrations, "dlss_unavailable_reason")},
            {"effective_temporal_upscaler", effectiveUpscaler},
            {"dlss_guide_contract", dlssGuideContract},
        });
    addCheck(
        "dlss_exposure_contract_reported",
        dlssExposureContract.is_object() &&
            dlssExposureContract.value("schema_version", 0) == 1 &&
            hasNumber(dlssExposureContract, "manual_exposure") &&
            hasNumber(dlssExposureContract, "pre_exposure") &&
            hasNumber(dlssExposureContract, "exposure_scale") &&
            hasBool(dlssExposureContract, "auto_exposure_enabled") &&
            hasBool(dlssExposureContract, "exposure_buffer_available") &&
            hasBool(dlssExposureContract, "exposure_buffer_passed_to_sdk") &&
            hasNonEmptyString(dlssExposureContract, "policy"),
        "DLSS diagnostics must report manual exposure, pre-exposure, exposure scale, auto-exposure state, and SDK exposure-buffer policy.",
        dlssExposureContract);
    addCheck(
        "dlss_guide_ready_when_effective",
        effectiveUpscaler != "dlss" || dlssGuideContract.value("guide_pass_ready", false),
        "DLSS guide pass must be ready when DLSS is the effective temporal upscaler.",
        {
            {"effective_temporal_upscaler", effectiveUpscaler},
            {"guide_pass_ready", dlssGuideContract.value("guide_pass_ready", false)},
        });
    addCheck(
        "dlss_ray_reconstruction_runtime_contract_reported",
        !dlssRayReconstructionRequested ||
            (availabilityReported("dlss_ray_reconstruction_available", "dlss_ray_reconstruction_unavailable_reason") &&
             dlssRrGuideContract.is_object() &&
             hasArray(dlssRrGuideContract, "guide_images") &&
             !dlssRrGuideContract["guide_images"].empty() &&
             dlssRrGuideContract.value("guide_images_allocated", false) &&
             dlssRrGuideContract.value("psr_guide_buffer_allocated", false) &&
             dlssRrGuideContract.value("psr_history_signatures_allocated", false) &&
             dlssRrValidator.value("previous_world_position", false) &&
             dlssRrValidator.value("material_id", false) &&
             dlssRrValidator.value("instance_id", false) &&
             dlssRrValidator.value("psr_history_signature", false) &&
             arrayContainsString(dlssRrGuideContract.value("guide_images", nlohmann::json::array()), "depth") &&
             arrayContainsString(dlssRrGuideContract.value("guide_images", nlohmann::json::array()), "motion") &&
             arrayContainsString(dlssRrGuideContract.value("guide_images", nlohmann::json::array()), "reflected_albedo") &&
             arrayContainsString(dlssRrGuideContract.value("guide_images", nlohmann::json::array()), "disocclusion_mask")),
        "Requested DLSS Ray Reconstruction must report availability/fallback reason, allocated guide images, and consistency validation.",
        {
            {"dlss_ray_reconstruction_requested", dlssRayReconstructionRequested},
            {"dlss_ray_reconstruction_available", boolValue(nvidiaIntegrations, "dlss_ray_reconstruction_available")},
            {"dlss_ray_reconstruction_unavailable_reason", stringValue(nvidiaIntegrations, "dlss_ray_reconstruction_unavailable_reason")},
            {"effective_dlss_ray_reconstruction", effectiveDlssRr},
            {"dlss_ray_reconstruction_guide_contract", dlssRrGuideContract},
        });
    addCheck(
        "dlss_ray_reconstruction_guides_ready_when_effective",
        !effectiveDlssRr || dlssRrGuideContract.value("guide_pass_ready", false),
        "DLSS Ray Reconstruction guides must be ready when DLSS RR is effective.",
        {
            {"effective_dlss_ray_reconstruction", effectiveDlssRr},
            {"guide_pass_ready", dlssRrGuideContract.value("guide_pass_ready", false)},
        });
    addCheck(
        "dormant_optional_sdk_fallback_reasons_reported",
        (nrdRequested || boolValue(nvidiaIntegrations, "nrd_available") || hasNonEmptyString(nvidiaIntegrations, "nrd_unavailable_reason")) &&
            (dlssRequested || boolValue(nvidiaIntegrations, "dlss_available") || hasNonEmptyString(nvidiaIntegrations, "dlss_unavailable_reason")) &&
            (dlssRayReconstructionRequested || boolValue(nvidiaIntegrations, "dlss_ray_reconstruction_available") ||
             hasNonEmptyString(nvidiaIntegrations, "dlss_ray_reconstruction_unavailable_reason")),
        "Dormant optional NVIDIA SDK paths must still report availability or fallback reasons.",
        {
            {"nrd_requested", nrdRequested},
            {"nrd_available", boolValue(nvidiaIntegrations, "nrd_available")},
            {"nrd_unavailable_reason", stringValue(nvidiaIntegrations, "nrd_unavailable_reason")},
            {"dlss_requested", dlssRequested},
            {"dlss_available", boolValue(nvidiaIntegrations, "dlss_available")},
            {"dlss_unavailable_reason", stringValue(nvidiaIntegrations, "dlss_unavailable_reason")},
            {"dlss_ray_reconstruction_requested", dlssRayReconstructionRequested},
            {"dlss_ray_reconstruction_available", boolValue(nvidiaIntegrations, "dlss_ray_reconstruction_available")},
            {"dlss_ray_reconstruction_unavailable_reason", stringValue(nvidiaIntegrations, "dlss_ray_reconstruction_unavailable_reason")},
        });

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"contract", "denoiser_upscaler_backend_policy"},
        {"feature_state", {
            {"engine_denoiser_requested", engineDenoiserRequested},
            {"engine_denoiser_run_expected", engineDenoiserRunExpected},
            {"nrd_requested", nrdRequested},
            {"taa_tsr_requested", taaTsrRequested},
            {"dlss_requested", dlssRequested},
            {"dlss_ray_reconstruction_requested", dlssRayReconstructionRequested},
            {"safe_default_requested", safeDefaultRequested},
        }},
        {"effective_state", {
            {"denoiser_backend", effectiveDenoiser},
            {"temporal_upscaler", effectiveUpscaler},
            {"dlss_ray_reconstruction", effectiveDlssRr},
        }},
        {"check_count", checks.size()},
        {"failure_count", failures.size()},
        {"checks", std::move(checks)},
        {"failures", std::move(failures)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererPerformanceBudgetValidationJson(
    const RendererSettings& settings,
    uint32_t profiledFrames,
    const nlohmann::json& resolution,
    const nlohmann::json& gpuFrameMs,
    const nlohmann::json& perPassGpuMs) {
    auto objectField = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field) && object[field].is_object()
            ? object[field]
            : nlohmann::json::object();
    };
    auto numberField = [](const nlohmann::json& object, const char* field, double fallback = 0.0) {
        return object.is_object() && object.contains(field) && object[field].is_number()
            ? object[field].get<double>()
            : fallback;
    };
    auto hasNumberField = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field) && object[field].is_number();
    };

    const nlohmann::json renderExtent = objectField(resolution, "render_extent");
    const double renderWidth = std::max(1.0, numberField(renderExtent, "width", 1280.0));
    const double renderHeight = std::max(1.0, numberField(renderExtent, "height", 720.0));
    const double renderScale = std::max(0.01, numberField(resolution, "render_scale", settings.renderResolutionScale));
    const double pixelScale = std::max(0.25, (renderWidth * renderHeight) / (1280.0 * 720.0));
    const double sppScale = static_cast<double>(std::max(1u, settings.samplesPerPixel));

    const bool gpuFrameStatsPresent =
        gpuFrameMs.is_object() &&
        hasNumberField(gpuFrameMs, "avg") &&
        hasNumberField(gpuFrameMs, "p95") &&
        hasNumberField(gpuFrameMs, "p99");
    bool anyNonZeroPassTiming = false;
    if (perPassGpuMs.is_object()) {
        for (const auto& item : perPassGpuMs.items()) {
            if (item.value().is_number() && item.value().get<double>() > 0.0) {
                anyNonZeroPassTiming = true;
                break;
            }
        }
    }
    const bool frameTimingAvailable = gpuFrameStatsPresent && numberField(gpuFrameMs, "avg") > 0.0;
    const bool timingEvaluationAvailable = frameTimingAvailable || anyNonZeroPassTiming;
    const bool strictTimingExpected = profiledFrames >= 30u;

    auto sectionBudgetBaseMs = [](const std::string& section) {
        if (section.find("path_trace") == 0) { return 18.0; }
        if (section.find("restir_di_temporal") == 0) { return 3.5; }
        if (section.find("restir_di_spatial") == 0) { return 4.5; }
        if (section.find("restir_di_final") == 0) { return 3.5; }
        if (section.find("restir_di_history_copy") == 0) { return 1.0; }
        if (section.find("restir_gi_temporal") == 0) { return 5.5; }
        if (section.find("restir_gi_spatial") == 0) { return 6.5; }
        if (section.find("restir_gi_final") == 0) { return 5.0; }
        if (section.find("restir_gi_upsample") == 0) { return 2.0; }
        if (section.find("restir_gi_clear") == 0) { return 1.0; }
        if (section.find("regir_build") == 0) { return 4.0; }
        if (section.find("regir_temporal_reuse") == 0) { return 2.0; }
        if (section.find("regir_spatial_reuse") == 0) { return 2.5; }
        if (section.find("denoiser") == 0) { return 5.0; }
        if (section.find("moment_update") == 0) { return 2.0; }
        if (section.find("taa") == 0) { return 2.5; }
        if (section.find("dlss_rr_guides") == 0) { return 2.5; }
        if (section.find("dlss_rr") == 0) { return 4.0; }
        if (section.find("dlss_guides") == 0) { return 1.5; }
        if (section.find("dlss") == 0) { return 2.5; }
        if (section.find("history_copy") == 0) { return 1.0; }
        if (section.find("atmosphere") == 0) { return 2.0; }
        if (section.find("fog") == 0) { return 1.5; }
        if (section.find("tone_map") == 0) { return 1.0; }
        if (section.find("fullscreen") == 0) { return 1.0; }
        if (section.find("adaptive_sampling") == 0) { return 2.0; }
        if (section.find("wavefront") == 0) { return 4.0; }
        return 2.0;
    };

    nlohmann::json passBudgets = nlohmann::json::array();
    nlohmann::json budgetViolations = nlohmann::json::array();
    nlohmann::json missingTimingSections = nlohmann::json::array();
    uint32_t budgetedSectionCount = 0;
    uint32_t activeBudgetedSectionCount = 0;
    uint32_t measuredSectionCount = 0;
    uint32_t activeMeasuredSectionCount = 0;

    const std::vector<RendererPassContract> contracts = rendererPassContracts(settings);
    for (const RendererPassContract& contract : contracts) {
        if (!contract.profilingSections.is_array()) {
            continue;
        }
        for (const nlohmann::json& sectionValue : contract.profilingSections) {
            if (!sectionValue.is_string()) {
                continue;
            }
            const std::string section = sectionValue.get<std::string>();
            if (rendererPassProfilingSectionIsAggregate(section)) {
                continue;
            }
            ++budgetedSectionCount;
            if (contract.activeByCurrentSettings) {
                ++activeBudgetedSectionCount;
            }
            const double budgetMs = sectionBudgetBaseMs(section) * pixelScale * sppScale;
            const bool timingPresent =
                perPassGpuMs.is_object() &&
                perPassGpuMs.contains(section) &&
                perPassGpuMs[section].is_number();
            const double observedMs = timingPresent ? perPassGpuMs[section].get<double>() : 0.0;
            if (timingPresent) {
                ++measuredSectionCount;
                if (contract.activeByCurrentSettings) {
                    ++activeMeasuredSectionCount;
                }
            } else if (contract.activeByCurrentSettings) {
                missingTimingSections.push_back({
                    {"pass", contract.name},
                    {"section", section},
                });
            }
            const bool overBudget = timingEvaluationAvailable && timingPresent && observedMs > budgetMs;
            if (overBudget) {
                budgetViolations.push_back({
                    {"scope", "pass"},
                    {"pass", contract.name},
                    {"section", section},
                    {"observed_ms", observedMs},
                    {"budget_ms", budgetMs},
                    {"over_by_ms", observedMs - budgetMs},
                });
            }
            passBudgets.push_back({
                {"pass", contract.name},
                {"pass_id", rendererPassContractIdName(contract.id)},
                {"role", rendererPassContractRoleName(contract.role)},
                {"active_by_current_settings", contract.activeByCurrentSettings},
                {"section", section},
                {"budget_ms", budgetMs},
                {"observed_ms", timingPresent ? nlohmann::json(observedMs) : nlohmann::json(nullptr)},
                {"timing_present", timingPresent},
                {"over_budget", overBudget},
            });
        }
    }

    const double fullFrameBudgetMs = 33.333 * pixelScale * sppScale;
    const double modeFrameBudgetMs = fullFrameBudgetMs *
        (passes::DlssPass::isUpscaleActive(settings) ? 1.15 : 1.0) *
        (passes::NrdPass::isActive(settings) ? 1.10 : 1.0) *
        (passes::DlssPass::isRayReconstructionActive(settings) ? 1.20 : 1.0);
    const double observedFrameAvgMs = numberField(gpuFrameMs, "avg");
    const double observedFrameP95Ms = numberField(gpuFrameMs, "p95");
    if (timingEvaluationAvailable && frameTimingAvailable && observedFrameAvgMs > modeFrameBudgetMs) {
        budgetViolations.push_back({
            {"scope", "full_frame_avg"},
            {"observed_ms", observedFrameAvgMs},
            {"budget_ms", modeFrameBudgetMs},
            {"over_by_ms", observedFrameAvgMs - modeFrameBudgetMs},
        });
    }
    if (timingEvaluationAvailable && frameTimingAvailable && observedFrameP95Ms > modeFrameBudgetMs * 1.35) {
        budgetViolations.push_back({
            {"scope", "full_frame_p95"},
            {"observed_ms", observedFrameP95Ms},
            {"budget_ms", modeFrameBudgetMs * 1.35},
            {"over_by_ms", observedFrameP95Ms - modeFrameBudgetMs * 1.35},
        });
    }

    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    if (!gpuFrameStatsPresent) {
        failures.push_back({
            {"code", "gpu_frame_stats_missing"},
            {"message", "Performance budget validation requires gpu_frame_ms avg/p95/p99 fields."},
        });
    }
    if (!perPassGpuMs.is_object()) {
        failures.push_back({
            {"code", "per_pass_gpu_ms_missing"},
            {"message", "Performance budget validation requires per_pass_gpu_ms timing fields."},
        });
    }
    if (!missingTimingSections.empty()) {
        failures.push_back({
            {"code", "active_budgeted_sections_missing_timing"},
            {"missing_count", missingTimingSections.size()},
        });
    }
    if (strictTimingExpected && !timingEvaluationAvailable) {
        failures.push_back({
            {"code", "strict_profile_has_no_nonzero_gpu_timings"},
            {"message", "Profiles with at least 30 measured frames must have non-zero GPU timing data for budget enforcement."},
        });
    } else if (!timingEvaluationAvailable) {
        warnings.push_back({
            {"code", "budget_evaluation_deferred_no_nonzero_gpu_timings"},
            {"message", "GPU timings are present but zero; budget overrun checks are deferred for this short smoke profile."},
        });
    }
    if (!budgetViolations.empty()) {
        failures.push_back({
            {"code", "performance_budget_exceeded"},
            {"violation_count", budgetViolations.size()},
        });
    }

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"contract", "performance_budget"},
        {"resolution", {
            {"render_width", renderWidth},
            {"render_height", renderHeight},
            {"render_scale", renderScale},
            {"pixel_scale_720p", pixelScale},
        }},
        {"sample_scale", sppScale},
        {"profiled_frames", profiledFrames},
        {"strict_timing_expected", strictTimingExpected},
        {"timing_evaluation_available", timingEvaluationAvailable},
        {"full_frame_budget_ms", modeFrameBudgetMs},
        {"full_frame_p95_budget_ms", modeFrameBudgetMs * 1.35},
        {"observed_frame_avg_ms", observedFrameAvgMs},
        {"observed_frame_p95_ms", observedFrameP95Ms},
        {"budgeted_section_count", budgetedSectionCount},
        {"active_budgeted_section_count", activeBudgetedSectionCount},
        {"measured_section_count", measuredSectionCount},
        {"active_measured_section_count", activeMeasuredSectionCount},
        {"missing_timing_sections", std::move(missingTimingSections)},
        {"pass_budgets", std::move(passBudgets)},
        {"budget_violations", std::move(budgetViolations)},
        {"failure_count", failures.size()},
        {"warning_count", warnings.size()},
        {"failures", std::move(failures)},
        {"warnings", std::move(warnings)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererRegirPromotionGateJson(
    const RendererSettings& settings,
    const passes::RegirPass::PromotionDiagnostics& diagnostics) {
    const bool regirRequested = passes::RegirPass::isRequested(settings);
    const bool hashGrid = passes::RegirPass::hashGridActive(settings, regirRequested);
    const bool activeGrid = passes::RegirPass::activeGridMode(settings, regirRequested);
    const bool sparseGrid = hashGrid || activeGrid;
    const bool hashSaturated = passes::RegirPass::hashGridSaturated(hashGrid, diagnostics.hashSaturationCount);
    const bool hashReuseFallback = passes::RegirPass::hashReuseFallback(settings, hashGrid);
    const bool unsupportedAdvanced = passes::RegirPass::unsupportedAdvancedRequested(
        settings,
        hashGrid,
        diagnostics.hashSaturationCount);
    const uint32_t queryPeriod = passes::RegirPass::effectiveFiniteQueryFramePeriod(settings, hashGrid);
    const uint64_t totalCells = diagnostics.totalCellCount > 0ull
        ? diagnostics.totalCellCount
        : passes::RegirPass::gridCellCount(settings);
    const uint64_t denseBytes = passes::RegirPass::denseReservoirBytes(
        settings,
        diagnostics.denseReservoirBytes,
        64ull);
    const uint64_t activeCells = passes::RegirPass::effectiveActiveCellCount(
        sparseGrid,
        diagnostics.feedbackAvailable,
        diagnostics.activeCellCount,
        totalCells,
        regirRequested);
    const uint64_t effectiveBytes = passes::RegirPass::effectiveReservoirBytes(
        sparseGrid,
        diagnostics.feedbackAvailable,
        diagnostics.effectiveReservoirBytes,
        denseBytes,
        regirRequested);
    const bool quickPlumbingPassed = passes::RegirPass::quickPromotionPlumbingPassed(settings, diagnostics);

    const std::filesystem::path fullPromotionArtifactPath =
        std::filesystem::path("out") / "regir_promotion_gate" / "summary.json";
    nlohmann::json fullPromotionArtifact = nlohmann::json::object();
    std::string fullPromotionArtifactError;
    const bool fullPromotionArtifactLoaded =
        rendererReadJsonArtifact(fullPromotionArtifactPath, fullPromotionArtifact, fullPromotionArtifactError);
    const bool fullPromotionArtifactOk =
        fullPromotionArtifactLoaded &&
        fullPromotionArtifact.value("ok", false) &&
        !fullPromotionArtifact.value("quick", true);
    const bool fullPromotionArtifactEligible =
        fullPromotionArtifactOk &&
        fullPromotionArtifact.value("promotion_eligible", false);
    auto artifactStepPassed = [&](const char* stepName) {
        if (!fullPromotionArtifactLoaded ||
            !fullPromotionArtifact.contains("steps") ||
            !fullPromotionArtifact["steps"].is_array()) {
            return false;
        }
        for (const nlohmann::json& step : fullPromotionArtifact["steps"]) {
            if (step.is_object() &&
                step.value("name", std::string{}) == stepName &&
                step.value("ok", false) &&
                step.value("exit_code", -1) == 0) {
                return true;
            }
        }
        return false;
    };

    const bool finiteLightReferenceMatrixPassed =
        diagnostics.finiteLightReferenceMatrixPassed || artifactStepPassed("finite_bias");
    const bool environmentMatrixPassed =
        diagnostics.environmentMatrixPassed || artifactStepPassed("environment");
    const bool visibilityReuseValidationPassed =
        diagnostics.visibilityReuseValidationPassed || artifactStepPassed("visibility");
    const bool equalTimeQualityPassed =
        diagnostics.equalTimeQualityPassed || artifactStepPassed("equal_time");
    const bool manyLightReferencePassed =
        diagnostics.manyLightReferencePassed || fullPromotionArtifactEligible;
    const bool runtimeFullPromotionEligible = passes::RegirPass::fullPromotionEligible(settings, diagnostics);
    const bool fullPromotionEligible =
        runtimeFullPromotionEligible ||
        (quickPlumbingPassed &&
         finiteLightReferenceMatrixPassed &&
         environmentMatrixPassed &&
         visibilityReuseValidationPassed &&
         equalTimeQualityPassed &&
         manyLightReferencePassed);

    nlohmann::json matrix = nlohmann::json::array();
    auto addMatrixRow = [&](const char* name,
                            const char* scope,
                            bool passed,
                            bool requiredForFullPromotion,
                            const char* command) {
        matrix.push_back({
            {"name", name},
            {"scope", scope},
            {"passed", passed},
            {"required_for_full_promotion", requiredForFullPromotion},
            {"command", command},
        });
    };
    addMatrixRow(
        "quick_plumbing_profile",
        "current profile",
        quickPlumbingPassed,
        false,
        ".\\scripts\\regir_promotion_gate.ps1 -Quick");
    addMatrixRow(
        "finite_light_bias_matrix",
        "finite many-light scenes",
        finiteLightReferenceMatrixPassed,
        true,
        ".\\scripts\\regir_bias_validation.ps1");
    addMatrixRow(
        "environment_matrix",
        "uniform/bright-texel/rotated/procedural/dynamic environment scenes",
        environmentMatrixPassed,
        true,
        ".\\scripts\\regir_environment_validation.ps1");
    addMatrixRow(
        "visibility_reuse_convergence",
        "visibility reuse convergence scenes",
        visibilityReuseValidationPassed,
        true,
        ".\\scripts\\regir_visibility_validation.ps1");
    addMatrixRow(
        "bistro_equal_time_quality",
        "Bistro equal-time quality/performance",
        equalTimeQualityPassed,
        true,
        ".\\scripts\\regir_equal_time_validation.ps1");
    addMatrixRow(
        "many_light_reference_cases",
        "scene-specific many-light references",
        manyLightReferencePassed,
        true,
        ".\\scripts\\regir_promotion_gate.ps1");

    nlohmann::json blockers = nlohmann::json::array();
    if (regirRequested && !quickPlumbingPassed) {
        blockers.push_back("quick ReGIR plumbing profile has not passed");
    }
    if (!finiteLightReferenceMatrixPassed) {
        blockers.push_back("finite-light high-SPP bias validation matrix is missing");
    }
    if (!environmentMatrixPassed) {
        blockers.push_back("environment-source validation matrix is missing");
    }
    if (!visibilityReuseValidationPassed) {
        blockers.push_back("visibility-reuse convergence validation is missing");
    }
    if (!equalTimeQualityPassed) {
        blockers.push_back("Bistro equal-time quality/performance gate is missing");
    }
    if (!manyLightReferencePassed) {
        blockers.push_back("many-light reference scenes are missing");
    }

    nlohmann::json failures = nlohmann::json::array();
    if (fullPromotionEligible && !quickPlumbingPassed) {
        failures.push_back({
            {"code", "regir_full_promotion_without_quick_gate"},
            {"message", "Full ReGIR promotion cannot be claimed before the quick plumbing gate passes."},
        });
    }

    return nlohmann::json{
        {"schema_version", 1},
        {"contract", "regir_quality_promotion_gate"},
        {"requested", regirRequested},
        {"effective_grid_mode", regirRequested ? regirGridModeName(passes::RegirPass::effectiveGridMode(settings, true)) : "off"},
        {"finite_query_frame_period", regirRequested ? queryPeriod : 0u},
        {"finite_query_probability", passes::RegirPass::finiteQueryProbability(regirRequested, queryPeriod)},
        {"quick_plumbing_passed", quickPlumbingPassed},
        {"full_promotion_eligible", fullPromotionEligible},
        {"completion_claim_policy", "full_promotion_eligible may be true only after every required full-promotion matrix row passes"},
        {"full_promotion_artifact", {
            {"path", fullPromotionArtifactPath.string()},
            {"loaded", fullPromotionArtifactLoaded},
            {"load_error", fullPromotionArtifactLoaded ? nlohmann::json(nullptr) : nlohmann::json(fullPromotionArtifactError)},
            {"ok", fullPromotionArtifactOk},
            {"promotion_eligible", fullPromotionArtifactEligible},
            {"summary", fullPromotionArtifactLoaded ? fullPromotionArtifact : nlohmann::json(nullptr)},
        }},
        {"advanced_fallback", {
            {"used", regirRequested && unsupportedAdvanced},
            {"reason", hashSaturated
                ? "hash_table_saturated_using_canonical_query_fallback"
                : (hashReuseFallback
                    ? "hash_grid_spatial_temporal_reuse_not_implemented"
                    : "")},
            {"spatial_reuse_effective", passes::RegirPass::spatialReuseEffective(settings, regirRequested)},
            {"temporal_reuse_effective", passes::RegirPass::temporalReuseEffective(settings, regirRequested)},
            {"visibility_reuse_effective", regirRequested && settings.regirVisibilityReuse},
        }},
        {"grid_evidence", {
            {"feedback_available", diagnostics.feedbackAvailable},
            {"total_cell_count", totalCells},
            {"active_cell_count", activeCells},
            {"hash_collision_count", diagnostics.hashCollisionCount},
            {"hash_saturation_count", diagnostics.hashSaturationCount},
            {"hash_cell_capacity", hashGrid ? diagnostics.hashCellCapacity : 0u},
            {"dense_memory_bytes", regirRequested ? denseBytes : 0ull},
            {"effective_memory_bytes", effectiveBytes},
            {"backing_memory_bytes", regirRequested ? diagnostics.backingBytes : 0ull},
        }},
        {"infinite_source_evidence", {
            {"environment_effective", diagnostics.environmentEffective},
            {"sun_effective", diagnostics.sunEffective},
            {"environment_bank_size", diagnostics.environmentBankSize},
            {"sun_bank_size", diagnostics.sunBankSize},
            {"valid_environment_reservoirs", diagnostics.validEnvironmentReservoirs},
            {"valid_sun_reservoirs", diagnostics.validSunReservoirs},
            {"environment_bank_bytes", diagnostics.environmentBankBytes},
        }},
        {"temporal_history_valid", diagnostics.temporalHistoryValid},
        {"timing_evidence", {
            {"profiled_frames", diagnostics.profiledFrames},
            {"gpu_frame_avg_ms", diagnostics.gpuFrameAvgMs},
            {"regir_total_gpu_ms", passes::RegirPass::totalGpuMs(diagnostics)},
            {"regir_build_ms", diagnostics.regirBuildMs},
            {"regir_spatial_reuse_ms", diagnostics.regirSpatialReuseMs},
            {"regir_temporal_reuse_ms", diagnostics.regirTemporalReuseMs},
        }},
        {"scene_matrix", std::move(matrix)},
        {"completion_blockers", std::move(blockers)},
        {"failure_count", failures.size()},
        {"failures", std::move(failures)},
        {"passed", failures.empty()},
    };
}

inline nlohmann::json rendererDiagnosticRuntimeValidationJson(
    const RendererSettings& settings,
    const nlohmann::json& diagnosticReadiness,
    const nlohmann::json& nsightAnalysisPlan,
    const nlohmann::json& rayTracingShaderMap,
    const nlohmann::json& accelerationStructureDiagnostics,
    const nlohmann::json& barrierSyncDiagnostics) {
    auto hasString = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field) && object[field].is_string();
    };
    auto hasNumber = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field) && object[field].is_number();
    };
    auto hasBool = [](const nlohmann::json& object, const char* field) {
        return object.is_object() && object.contains(field) && object[field].is_boolean();
    };
    auto arrayContainsString = [](const nlohmann::json& values, const char* expected) {
        if (!values.is_array()) {
            return false;
        }
        for (const nlohmann::json& value : values) {
            if (value.is_string() && value.get<std::string>() == expected) {
                return true;
            }
        }
        return false;
    };
    auto shaderStagePresent = [&](const char* stageName) {
        if (!rayTracingShaderMap.is_object() ||
            !rayTracingShaderMap.contains("shader_groups") ||
            !rayTracingShaderMap["shader_groups"].is_array()) {
            return false;
        }
        for (const nlohmann::json& group : rayTracingShaderMap["shader_groups"]) {
            if (group.is_object() && group.value("stage", std::string{}) == stageName) {
                return hasString(group, "file") && hasString(group, "marker") && hasString(group, "role");
            }
        }
        return false;
    };
    auto nsightStepPresent = [&](const char* scopeName) {
        if (!nsightAnalysisPlan.is_object() ||
            !nsightAnalysisPlan.contains("steps") ||
            !nsightAnalysisPlan["steps"].is_array()) {
            return false;
        }
        for (const nlohmann::json& step : nsightAnalysisPlan["steps"]) {
            if (step.is_object() &&
                step.value("scope", std::string{}) == scopeName &&
                step.contains("commands") &&
                step["commands"].is_array() &&
                !step["commands"].empty() &&
                hasString(step, "why")) {
                return true;
            }
        }
        return false;
    };

    nlohmann::json checks = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    auto addCheck = [&](const char* name,
                        bool required,
                        bool passed,
                        const char* message,
                        nlohmann::json evidence = nlohmann::json::object()) {
        checks.push_back({
            {"name", name},
            {"required", required},
            {"passed", passed},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
        if (required && !passed) {
            failures.push_back({
                {"code", name},
                {"message", message},
            });
        } else if (!required && !passed) {
            warnings.push_back({
                {"code", name},
                {"message", message},
            });
        }
    };

    uint32_t warningReadinessFailures = 0;
    uint32_t errorReadinessFailures = 0;
    if (diagnosticReadiness.is_object() &&
        diagnosticReadiness.contains("checks") &&
        diagnosticReadiness["checks"].is_array()) {
        for (const nlohmann::json& check : diagnosticReadiness["checks"]) {
            if (!check.is_object() || check.value("pass", false)) {
                continue;
            }
            const std::string severity = check.value("severity", std::string{});
            if (severity == "warning" || severity == "info") {
                ++warningReadinessFailures;
            } else {
                ++errorReadinessFailures;
            }
        }
    }

    addCheck(
        "diagnostic_readiness_schema_present",
        true,
        diagnosticReadiness.is_object() &&
            diagnosticReadiness.value("schema", std::string{}) == "RendererDiagnosticReadinessV1" &&
            diagnosticReadiness.contains("checks") &&
            diagnosticReadiness["checks"].is_array() &&
            !diagnosticReadiness["checks"].empty(),
        "Profile must include RendererDiagnosticReadinessV1 checks.",
        diagnosticReadiness);
    addCheck(
        "diagnostic_readiness_no_error_failures",
        true,
        errorReadinessFailures == 0u,
        "Diagnostic readiness may include warnings, but error-severity readiness failures must fail the renderer contract gate.",
        {
            {"status", diagnosticReadiness.value("status", std::string{})},
            {"warning_readiness_failures", warningReadinessFailures},
            {"error_readiness_failures", errorReadinessFailures},
        });
    addCheck(
        "nsight_analysis_plan_present",
        true,
        nsightAnalysisPlan.is_object() &&
            nsightAnalysisPlan.value("schema", std::string{}) == "NsightAnalysisPlanV1" &&
            nsightAnalysisPlan.contains("recommended_metric_sets") &&
            nsightAnalysisPlan["recommended_metric_sets"].is_array() &&
            !nsightAnalysisPlan["recommended_metric_sets"].empty() &&
            nsightAnalysisPlan.contains("recommended_capture_flags") &&
            nsightAnalysisPlan["recommended_capture_flags"].is_array() &&
            !nsightAnalysisPlan["recommended_capture_flags"].empty() &&
            nsightStepPresent("whole_frame") &&
            nsightStepPresent("dominant_marker"),
        "Profile must include actionable Nsight capture flags, metric sets, and whole-frame/dominant-marker follow-up steps.",
        nsightAnalysisPlan);
    addCheck(
        "ray_tracing_nsight_step_present",
        settings.pathTracingEnabled,
        !settings.pathTracingEnabled || nsightStepPresent("ray_tracing"),
        "Path tracing profiles must include a ray-tracing-specific Nsight follow-up step.",
        nsightAnalysisPlan);
    addCheck(
        "ray_tracing_shader_map_present",
        settings.pathTracingEnabled,
        !settings.pathTracingEnabled ||
            (rayTracingShaderMap.is_object() &&
             rayTracingShaderMap.value("schema", std::string{}) == "RayTracingShaderMapV1" &&
             shaderStagePresent("raygen") &&
             shaderStagePresent("miss") &&
             shaderStagePresent("closesthit") &&
             shaderStagePresent("anyhit") &&
             rayTracingShaderMap.contains("counter_mapping") &&
             rayTracingShaderMap["counter_mapping"].is_object()),
        "Path tracing profiles must map shader groups and diagnostic counter names for Nsight/RenderDoc attribution.",
        rayTracingShaderMap);
    addCheck(
        "acceleration_structure_diagnostics_present",
        settings.pathTracingEnabled,
        !settings.pathTracingEnabled ||
            (accelerationStructureDiagnostics.is_object() &&
             accelerationStructureDiagnostics.value("schema", std::string{}) == "AccelerationStructureDiagnosticsV1" &&
             hasNumber(accelerationStructureDiagnostics, "triangle_count") &&
             hasNumber(accelerationStructureDiagnostics, "blas_count") &&
             hasNumber(accelerationStructureDiagnostics, "as_bytes") &&
             hasBool(accelerationStructureDiagnostics, "hardware_backface_culling_enabled") &&
             accelerationStructureDiagnostics.contains("recommendations") &&
             accelerationStructureDiagnostics["recommendations"].is_array() &&
             !accelerationStructureDiagnostics["recommendations"].empty()),
        "Path tracing profiles must expose AS size/topology/backface policy diagnostics and follow-up recommendations.",
        accelerationStructureDiagnostics);
    addCheck(
        "barrier_sync_diagnostics_present",
        true,
        barrierSyncDiagnostics.is_object() &&
            barrierSyncDiagnostics.value("schema", std::string{}) == "BarrierSyncDiagnosticsV1" &&
            barrierSyncDiagnostics.contains("queue_lane_ms") &&
            barrierSyncDiagnostics["queue_lane_ms"].is_object() &&
            barrierSyncDiagnostics.contains("async_compute") &&
            barrierSyncDiagnostics["async_compute"].is_object() &&
            barrierSyncDiagnostics.contains("rendergraph_outputs") &&
            barrierSyncDiagnostics["rendergraph_outputs"].is_array() &&
            arrayContainsString(barrierSyncDiagnostics["rendergraph_outputs"], "--dump-rendergraph") &&
            barrierSyncDiagnostics.contains("nsight_followup") &&
            barrierSyncDiagnostics["nsight_followup"].is_array() &&
            !barrierSyncDiagnostics["nsight_followup"].empty(),
        "Profiles must expose queue lanes, async compute state, RenderGraph dump commands, and Nsight Systems follow-up guidance.",
        barrierSyncDiagnostics);

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"warning_readiness_failure_count", warningReadinessFailures},
        {"error_readiness_failure_count", errorReadinessFailures},
        {"check_count", checks.size()},
        {"failure_count", failures.size()},
        {"warning_count", warnings.size()},
        {"checks", std::move(checks)},
        {"failures", std::move(failures)},
        {"warnings", std::move(warnings)},
        {"passed", passed},
    };
}

inline nlohmann::json rendererReviewChecklistJson() {
    return nlohmann::json{
        {"schema_version", 1},
        {"checks", rendererContractArray({
            "Does the pass declare all inputs, outputs, history resources, descriptor bindings, push constants, shader dependencies, and RenderGraph reads/writes?",
            "Does the pass obey the shared temporal reset and motion-vector contract?",
            "Does it update validation modes, debug views, and profile counters?",
            "Does it affect GPU timings or resource lifetime/barrier behavior?",
            "Does it change supported feature combinations or fallback policy?",
            "Does any reference image or threshold update include a reason?",
        })},
    };
}

inline nlohmann::json rendererArchitectureDocumentationJson() {
    return nlohmann::json{
        {"schema_version", 1},
        {"document", "renderer_architecture_quality_lock"},
        {"purpose", "Machine-readable renderer ownership rules emitted with every profile so review, CI, and debug packages do not depend on implicit PathTracerRenderer knowledge."},
        {"coordinator_rules", rendererContractArray({
            "PathTracerRenderer may coordinate frame execution, shared device objects, and legacy scheduling while extraction is in progress.",
            "Feature behavior must be described by pass contracts before scheduling, descriptor, resource, or shader behavior is changed.",
            "Coordinator-only state must be surfaced through runtime diagnostics until it is moved behind a pass-owned module.",
            "Unsupported feature combinations must fail through the supported mode matrix or explicit runtime validation, not through silent fallback.",
        })},
        {"pass_ownership_rules", rendererContractArray({
            "Each major renderer feature owns its inputs, outputs, history resources, descriptor layouts, push constants, shader dependencies, debug views, timings, and validation checks.",
            "A pass may write another pass's resource only through a declared shared contract or RenderGraph ownership edge.",
            "History resources must name their reset reasons and report validity through temporal diagnostics.",
            "Debug views and profiler names are part of the pass API and must remain stable or include a migration note.",
        })},
        {"global_data_contracts", {
            {"temporal", rendererContractArray({
                "Frame index, accumulation index, previous-frame state, camera cut, history validity, jitter, and reset reason come from the shared temporal contract.",
                "Motion vectors use one documented sign/unit convention before TAA, ReSTIR, NRD, DLSS, or debug views consume them.",
                "Temporal features must reject history on camera cut, resolution or render-scale change, scene/material/light topology changes, and explicit accumulation resets.",
            })},
            {"renderer_application_bridge", rendererContractArray({
                "Surface, material, light identity, environment sampling, visibility, alpha test, and backface behavior are described by the application bridge contract.",
                "Lighting algorithms consume bridge facts instead of embedding scattered scene/material assumptions.",
                "Bridge validation must cover normal length, roughness range, depth validity, motion bounds, material id validity, and light id validity.",
            })},
            {"denoiser_upscaler_guides", rendererContractArray({
                "Depth, normal, roughness, albedo, motion, hit distance, radiance, confidence, exposure, reactive masks, and replacement-surface guides must declare owner, format, range, and coordinate space.",
                "NRD, DLSS, DLSS Ray Reconstruction, internal denoiser, TAA, and disabled modes share one backend policy and clear fallback reason.",
            })},
            {"rendergraph", rendererContractArray({
                "RenderGraph dumps must include pass/resource ownership, read/write intervals, lifetime validation, alias groups, physical backing diagnostics, and barrier/sync follow-up hints.",
                "Resource dependency bugs should be visible from rendergraph.json before inspecting shader side effects.",
            })},
        }},
        {"how_to_add_temporal_feature", rendererContractArray({
            "Add the feature's history slot to TemporalSystem or report why the history is stateless.",
            "Declare required reset reasons in the pass contract.",
            "Expose debug views for history validity, motion/disocclusion, reset reason, and feature-specific rejection.",
            "Add runtime validation to renderer_contracts.temporal_runtime_validation or a stricter feature-specific section.",
        })},
        {"how_to_add_debug_view", rendererContractArray({
            "Register the view with a stable RendererDebugView enum and rendererDebugViewName string.",
            "Add owner-pass metadata, value range, meaning, and failure hint to debug_view_registry_validation.",
            "Include the view in the owning pass contract and export filter if it is useful in headless diagnostics.",
            "Add or update validation evidence so missing active debug outputs fail clearly.",
        })},
        {"how_to_add_renderer_mode", rendererContractArray({
            "Add the mode to supported_mode_matrix with support status, required features, and unsupported combinations.",
            "Update active_pass_runtime_validation so requested features match active pass/timing evidence.",
            "Update denoiser/upscaler, ReSTIR, ReGIR, temporal, and performance budget validation when the mode touches them.",
            "Add reference-image, performance, or diagnostic-package coverage before promoting the mode from experimental to supported.",
        })},
        {"reference_image_policy", rendererContractArray({
            "Reference updates must include a reason, fixed scene wrapper, render settings, effective SPP, and whether fixed seed was used only for preview.",
            "Threshold changes must include scene/mode-specific rationale and keep denoiser, TAA, auto exposure, and backface-culling policies explicit.",
            "Sponza Heavy and Bistro Interior references keep hardware backface culling off unless a new reference note proves no outside-light leakage.",
        })},
        {"review_artifacts", rendererContractArray({
            "profile.json renderer_contracts quality gate",
            "rendergraph.json resource ownership and lifetime validation",
            "present PNG or image comparison output",
            "debug view exports for the affected pass",
            "pass timing or performance budget diff",
        })},
        {"passed", true},
    };
}

inline nlohmann::json rendererPlanPhaseQualityLockJson(const nlohmann::json& rendererContracts) {
    auto sectionPresent = [&](const char* sectionName) {
        return rendererContracts.is_object() && rendererContracts.contains(sectionName);
    };
    auto sectionPassed = [&](const char* sectionName) {
        return sectionPresent(sectionName) &&
            rendererContracts[sectionName].is_object() &&
            rendererContracts[sectionName].value("passed", false);
    };
    auto sectionEvidence = [&](std::initializer_list<const char*> sectionNames) {
        nlohmann::json evidence = nlohmann::json::array();
        for (const char* sectionName : sectionNames) {
            evidence.push_back({
                {"section", sectionName},
                {"present", sectionPresent(sectionName)},
                {"passed", sectionPassed(sectionName)},
            });
        }
        return evidence;
    };

    nlohmann::json phases = nlohmann::json::array();
    nlohmann::json completionBlockers = nlohmann::json::array();
    uint32_t executableEvidencePhaseCount = 0;
    uint32_t completionProvenPhaseCount = 0;
    auto addPhase = [&](uint32_t number,
                        const char* name,
                        const char* status,
                        bool completionProven,
                        nlohmann::json evidenceSections,
                        const char* acceptanceGate,
                        const char* remainingWork) {
        bool hasExecutableEvidence = false;
        if (evidenceSections.is_array()) {
            for (const nlohmann::json& evidence : evidenceSections) {
                if (evidence.is_object() && evidence.value("present", false)) {
                    hasExecutableEvidence = true;
                    break;
                }
            }
        }
        if (hasExecutableEvidence) {
            ++executableEvidencePhaseCount;
        }
        if (completionProven) {
            ++completionProvenPhaseCount;
        } else {
            completionBlockers.push_back({
                {"phase", number},
                {"name", name},
                {"remaining_work", remainingWork},
            });
        }
        phases.push_back({
            {"phase", number},
            {"name", name},
            {"status", status},
            {"has_executable_evidence", hasExecutableEvidence},
            {"completion_proven", completionProven},
            {"evidence_sections", std::move(evidenceSections)},
            {"acceptance_gate", acceptanceGate},
            {"remaining_work", remainingWork},
        });
    };

    const bool passContractsComplete = sectionPassed("contract_validation");
    const bool passOwnerRegistryPassed = sectionPassed("pass_owner_registry_validation");
    const bool debugCoveragePassed = sectionPassed("debug_output_coverage");
    const bool debugRegistryPassed = sectionPassed("debug_view_registry_validation");
    const bool temporalRuntimePassed = sectionPassed("temporal_runtime_validation");
    const bool bridgeRuntimePassed = sectionPassed("application_bridge_runtime_validation");
    const bool activePassRuntimePassed = sectionPassed("active_pass_runtime_validation");
    const bool reservoirRuntimePassed = sectionPassed("restir_reservoir_runtime_validation");
    const bool restirReferenceMatrixPassed = sectionPassed("restir_reference_matrix_artifact");
    const bool denoiserUpscalerPassed = sectionPassed("denoiser_upscaler_runtime_validation");
    const bool diagnosticRuntimePassed = sectionPassed("diagnostic_runtime_validation");
    const bool performanceBudgetPassed = sectionPassed("performance_budget_validation");
    const bool regirPromotionGatePassed = sectionPassed("regir_promotion_gate");
    const bool regirFullPromotionEligible =
        sectionPresent("regir_promotion_gate") &&
        rendererContracts["regir_promotion_gate"].is_object() &&
        rendererContracts["regir_promotion_gate"].value("full_promotion_eligible", false);
    const bool currentModeSupported = sectionPassed("current_mode_support");
    const bool renderGraphArtifactValidated =
        sectionPresent("rendergraph_artifact_validation") &&
        sectionPassed("rendergraph_artifact_validation") &&
        rendererContracts["rendergraph_artifact_validation"].is_object() &&
        rendererContracts["rendergraph_artifact_validation"].value("artifact_requested", false) &&
        rendererContracts["rendergraph_artifact_validation"].value("validated", false);
    const bool architectureDocumentationPassed = sectionPassed("architecture_documentation");
    const uint32_t passContractModuleOwnedCount =
        sectionPresent("pass_owner_registry_validation") &&
            rendererContracts["pass_owner_registry_validation"].is_object()
        ? rendererContracts["pass_owner_registry_validation"].value("contract_module_owned_count", 0u)
        : 0u;
    const char* phase2Status = "partial";
    const bool phase2Complete =
        passContractsComplete &&
        passOwnerRegistryPassed &&
        activePassRuntimePassed &&
        passContractModuleOwnedCount >= 12u;
    if (phase2Complete) {
        phase2Status = "all-pass-module-contract-ownership";
    } else if (passContractsComplete && passOwnerRegistryPassed && activePassRuntimePassed && passContractModuleOwnedCount >= 3u) {
        phase2Status = "partial-pass-module-contract-ownership";
    } else if (passContractsComplete && passOwnerRegistryPassed && activePassRuntimePassed) {
        phase2Status = "compile-time-pass-modules";
    } else if (passContractsComplete && activePassRuntimePassed) {
        phase2Status = "staged-contract-only";
    }

    addPhase(
        0,
        "Baseline Audit",
        passContractsComplete && debugRegistryPassed ? "executable-baseline-metadata" : "partial",
        passContractsComplete && debugRegistryPassed,
        sectionEvidence({"pass_contracts", "contract_validation", "debug_view_registry_validation", "supported_mode_matrix"}),
        "Profile must enumerate pass contracts, debug views, supported modes, and current settings.",
        "Keep extending baseline evidence as real pass classes replace coordinator-owned contracts.");
    addPhase(
        1,
        "Validation Baseline And Quality Gates",
        diagnosticRuntimePassed && currentModeSupported ? "executable-quality-gate" : "partial",
        diagnosticRuntimePassed && currentModeSupported,
        sectionEvidence({"diagnostic_runtime_validation", "current_mode_support", "performance_budget_validation"}),
        "Validation, diagnostics, mode support, and performance budget gates must be machine-readable.",
        "Broaden beyond Cornell smoke with reference-scene image/performance matrices.");
    addPhase(
        2,
        "Renderer Pass Ownership Split",
        phase2Status,
        phase2Complete,
        sectionEvidence({"pass_contracts", "contract_validation", "pass_owner_registry_validation", "active_pass_runtime_validation"}),
        "Every major pass declares ownership, owner-tag metadata, and runtime activation evidence.",
        "Continue extracting scheduling and GPU-resource code as pass modules grow beyond contract/policy ownership.");
    addPhase(
        3,
        "Renderer Application Bridge",
        bridgeRuntimePassed ? "executable-runtime-contract" : "partial",
        bridgeRuntimePassed,
        sectionEvidence({"application_bridge_contract", "application_bridge_runtime_validation"}),
        "Surface, light, visibility, and guide bridge evidence must pass at runtime.",
        "Add a dedicated bridge ABI validation pass for larger scene/material variation.");
    addPhase(
        4,
        "ReSTIR DI Hardening",
        reservoirRuntimePassed && restirReferenceMatrixPassed ? "full-reference-matrix-contract" :
            (reservoirRuntimePassed ? "partial-runtime-contract" : "partial"),
        reservoirRuntimePassed && bridgeRuntimePassed && debugCoveragePassed && restirReferenceMatrixPassed,
        sectionEvidence({"restir_reservoir_runtime_validation", "application_bridge_runtime_validation", "debug_output_coverage", "restir_reference_matrix_artifact"}),
        "DI reservoir/debug/runtime contracts must be checked and free of invalid PDF/target/non-finite evidence.",
        "Complete standalone DI pass extraction, deterministic remap tables, and DI reference comparison matrix.");
    addPhase(
        5,
        "ReSTIR GI And Path Tracing Cleanup",
        reservoirRuntimePassed && temporalRuntimePassed && restirReferenceMatrixPassed ? "full-reference-matrix-contract" :
            (reservoirRuntimePassed && temporalRuntimePassed ? "partial-runtime-contract" : "partial"),
        reservoirRuntimePassed && temporalRuntimePassed && activePassRuntimePassed && restirReferenceMatrixPassed,
        sectionEvidence({"restir_reservoir_runtime_validation", "temporal_runtime_validation", "active_pass_runtime_validation", "restir_reference_matrix_artifact"}),
        "GI/path temporal and reservoir contracts must have runtime evidence.",
        "Finish GI receiver/sample ownership split and reference comparison coverage for GI-only/path-tracing variants.");
    addPhase(
        6,
        "ReGIR Quality Pass",
        debugCoveragePassed && currentModeSupported && regirPromotionGatePassed && regirFullPromotionEligible
            ? "full-promotion-gate-contract"
            : (debugCoveragePassed && currentModeSupported && regirPromotionGatePassed ? "promotion-gate-contract" : "partial"),
        debugCoveragePassed && currentModeSupported && regirPromotionGatePassed && regirFullPromotionEligible,
        sectionEvidence({"debug_output_coverage", "performance_budget_validation", "current_mode_support", "regir_promotion_gate"}),
        "ReGIR debug outputs, supported mode evidence, and performance budget evidence must be present.",
        "Run and import the full scene-specific ReGIR quality/performance matrices and many-light reference cases.");
    addPhase(
        7,
        "Temporal Contract Unification",
        temporalRuntimePassed ? "executable-runtime-contract" : "partial",
        temporalRuntimePassed,
        sectionEvidence({"temporal_contract", "temporal_runtime_validation"}),
        "Shared reset reasons, required history slots, and temporal feature readiness must pass.",
        "Expand movement/camera-cut/material-light-change validation scenes.");
    addPhase(
        8,
        "NRD, DLSS, And Denoiser Contract Pass",
        denoiserUpscalerPassed ? "executable-runtime-contract" : "partial",
        denoiserUpscalerPassed,
        sectionEvidence({"denoiser_upscaler_runtime_validation", "application_bridge_runtime_validation", "debug_view_registry_validation"}),
        "Backend policy, SDK fallback reasons, guide contracts, and active temporal evidence must pass.",
        "Run backend comparison matrices on systems with NRD/DLSS/DLSS RR enabled.");
    addPhase(
        9,
        "RenderGraph And Resource Lifetime Cleanup",
        renderGraphArtifactValidated ? "executable-rendergraph-artifact" :
            (diagnosticRuntimePassed ? "external-rendergraph-artifact-required" : "partial"),
        renderGraphArtifactValidated,
        sectionEvidence({"rendergraph_artifact_validation", "diagnostic_runtime_validation"}),
        "Profile-requested rendergraph.json must validate resource ownership, lifetime, and aliasing.",
        "Promote rendergraph.json resource_lifetime_validation into CI/package validators across scenes and broader mode/scene coverage.");
    addPhase(
        10,
        "Performance Optimization",
        performanceBudgetPassed ? "executable-budget-contract" : "partial",
        performanceBudgetPassed,
        sectionEvidence({"performance_budget_validation", "diagnostic_runtime_validation"}),
        "Per-pass and full-frame performance budgets must be emitted and enforceable on strict profiles.",
        "Replace conservative placeholder budgets with measured scene/mode baselines and Nsight trace-backed thresholds.");
    addPhase(
        11,
        "Debuggability And Developer UX",
        debugRegistryPassed && diagnosticRuntimePassed ? "executable-debug-contract" : "partial",
        debugRegistryPassed && diagnosticRuntimePassed,
        sectionEvidence({"debug_view_registry_validation", "debug_output_coverage", "diagnostic_runtime_validation"}),
        "Debug views, diagnostic readiness, shader maps, and capture guidance must be machine-readable.",
        "Keep improving UI overlays and one-command diagnostic bundles with rendered debug images.");
    addPhase(
        12,
        "Documentation And Long-Term Quality Lock",
        architectureDocumentationPassed && sectionPresent("review_checklist") ? "executable-profile-embedded-documentation" : "partial",
        architectureDocumentationPassed && sectionPresent("review_checklist") && passContractsComplete && currentModeSupported,
        sectionEvidence({"architecture_documentation", "pass_contracts", "supported_mode_matrix", "review_checklist"}),
        "Profile must include renderer ownership rules, pass contracts, supported modes, review checklist, and phase-completion evidence without overclaiming completion.",
        "Keep external docs synchronized if docs/scripts are intentionally kept ignored or published elsewhere.");

    constexpr uint32_t kExpectedPhaseCount = 13u;
    nlohmann::json failures = nlohmann::json::array();
    if (phases.size() != kExpectedPhaseCount) {
        failures.push_back({
            {"code", "unexpected_phase_count"},
            {"expected", kExpectedPhaseCount},
            {"actual", phases.size()},
        });
    }
    if (executableEvidencePhaseCount == 0u) {
        failures.push_back({
            {"code", "no_executable_phase_evidence"},
            {"message", "Quality lock must point at at least one executable evidence section."},
        });
    }

    const bool fullPlanComplete = completionProvenPhaseCount == kExpectedPhaseCount;
    return nlohmann::json{
        {"schema_version", 1},
        {"contract", "rtxdi_quality_renderer_plan_phase_lock"},
        {"source_plan", "docs/RTXDI_QUALITY_RENDERER_IMPROVEMENT_PLAN.md"},
        {"phase_count", phases.size()},
        {"expected_phase_count", kExpectedPhaseCount},
        {"executable_evidence_phase_count", executableEvidencePhaseCount},
        {"completion_proven_phase_count", completionProvenPhaseCount},
        {"incomplete_phase_count", kExpectedPhaseCount - completionProvenPhaseCount},
        {"full_plan_complete", fullPlanComplete},
        {"completion_claim_policy", "Do not mark the full plan complete until every phase row has completion_proven=true and current evidence proves every acceptance gate."},
        {"phases", std::move(phases)},
        {"completion_blockers", std::move(completionBlockers)},
        {"failure_count", failures.size()},
        {"failures", std::move(failures)},
        {"passed", failures.empty()},
    };
}

inline nlohmann::json rendererQualityGateValidationJson(
    const nlohmann::json& rendererContracts,
    bool validationEnabled,
    uint32_t validationErrorCount) {
    nlohmann::json checks = nlohmann::json::array();
    nlohmann::json failures = nlohmann::json::array();
    auto addCheck = [&](const char* name, bool passed, const char* message, nlohmann::json evidence = nlohmann::json::object()) {
        checks.push_back({
            {"name", name},
            {"passed", passed},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
        if (!passed) {
            failures.push_back({
                {"code", name},
                {"message", message},
            });
        }
    };
    auto contractPassed = [&](const char* sectionName) {
        if (!rendererContracts.is_object() ||
            !rendererContracts.contains(sectionName) ||
            !rendererContracts[sectionName].is_object()) {
            return false;
        }
        return rendererContracts[sectionName].value("passed", false);
    };
    auto evidenceFor = [&](const char* sectionName) {
        return rendererContracts.is_object() && rendererContracts.contains(sectionName)
            ? rendererContracts[sectionName]
            : nlohmann::json(nullptr);
    };

    addCheck(
        "renderer_validation_log_clean",
        validationEnabled && validationErrorCount == 0u,
        validationEnabled
            ? "Renderer validation log must not report errors."
            : "Vulkan validation must be enabled before the renderer validation log can pass.",
        {
            {"validation_enabled", validationEnabled},
            {"validation_error_count", validationErrorCount},
        });
    addCheck(
        "pass_contracts_complete",
        contractPassed("contract_validation"),
        "Every major pass contract must declare required ownership fields.",
        evidenceFor("contract_validation"));
    addCheck(
        "pass_owner_registry_valid",
        contractPassed("pass_owner_registry_validation"),
        "Every major pass contract must map to compile-time owner metadata for staged pass extraction.",
        evidenceFor("pass_owner_registry_validation"));
    addCheck(
        "active_pass_timing_covered",
        contractPassed("profile_timing_coverage"),
        "Active renderer pass contracts must map to profile timing sections.",
        evidenceFor("profile_timing_coverage"));
    addCheck(
        "active_debug_outputs_covered",
        contractPassed("debug_output_coverage"),
        "Active renderer pass contracts must map to exportable debug outputs or explicit aggregate artifacts.",
        evidenceFor("debug_output_coverage"));
    addCheck(
        "debug_view_registry_valid",
        contractPassed("debug_view_registry_validation"),
        "Exportable debug views must have stable names, owner-pass metadata, value ranges, meanings, and failure hints.",
        evidenceFor("debug_view_registry_validation"));
    addCheck(
        "temporal_contract_runtime_valid",
        contractPassed("temporal_runtime_validation"),
        "Temporal history resources, reset reasons, and required slots must satisfy the shared runtime contract.",
        evidenceFor("temporal_runtime_validation"));
    addCheck(
        "application_bridge_runtime_valid",
        contractPassed("application_bridge_runtime_validation"),
        "Surface, light, visibility, and guide bridge evidence must satisfy the runtime RAB contract.",
        evidenceFor("application_bridge_runtime_validation"));
    addCheck(
        "denoiser_upscaler_runtime_valid",
        contractPassed("denoiser_upscaler_runtime_validation"),
        "Denoiser/upscaler backend policy, fallback reasons, guide contracts, and active temporal evidence must satisfy the runtime contract.",
        evidenceFor("denoiser_upscaler_runtime_validation"));
    addCheck(
        "performance_budget_runtime_valid",
        contractPassed("performance_budget_validation"),
        "Active renderer modes must have per-pass and full-frame budgets, with strict profiles failing on over-budget timings.",
        evidenceFor("performance_budget_validation"));
    addCheck(
        "regir_promotion_gate_valid",
        contractPassed("regir_promotion_gate"),
        "ReGIR promotion evidence must be machine-readable and must not claim full promotion before the required scene matrices pass.",
        evidenceFor("regir_promotion_gate"));
    addCheck(
        "active_pass_runtime_valid",
        contractPassed("active_pass_runtime_validation"),
        "Requested renderer modes must match active pass and timing evidence, and disabled features must not leave hidden active passes.",
        evidenceFor("active_pass_runtime_validation"));
    addCheck(
        "restir_reservoir_runtime_valid",
        contractPassed("restir_reservoir_runtime_validation"),
        "Active ReSTIR DI/GI reservoir contracts must be checked and free of PDF, target, parity, or non-finite violations.",
        evidenceFor("restir_reservoir_runtime_validation"));
    addCheck(
        "rendergraph_artifact_valid_when_requested",
        contractPassed("rendergraph_artifact_validation"),
        "Requested RenderGraph artifacts must include passing resource ownership and lifetime validation.",
        evidenceFor("rendergraph_artifact_validation"));
    addCheck(
        "diagnostic_runtime_valid",
        contractPassed("diagnostic_runtime_validation"),
        "Profile diagnostics must include readiness checks, Nsight guidance, shader maps, acceleration-structure attribution, and barrier/sync follow-up data.",
        evidenceFor("diagnostic_runtime_validation"));
    addCheck(
        "current_mode_supported",
        contractPassed("current_mode_support"),
        "The requested renderer feature combination must be part of the supported mode matrix or fail clearly.",
        evidenceFor("current_mode_support"));
    addCheck(
        "architecture_documentation_present",
        contractPassed("architecture_documentation"),
        "Profile must include renderer ownership rules, global contracts, mode-addition rules, debug-view rules, and reference-image policy.",
        evidenceFor("architecture_documentation"));
    addCheck(
        "plan_phase_quality_lock_valid",
        contractPassed("plan_phase_quality_lock"),
        "The profile must include a phase-by-phase quality lock that reports executable evidence and refuses to overclaim full-plan completion.",
        evidenceFor("plan_phase_quality_lock"));

    const bool passed = failures.empty();
    return nlohmann::json{
        {"schema_version", 1},
        {"gate", "renderer_quality_contracts"},
        {"check_count", checks.size()},
        {"failure_count", failures.size()},
        {"checks", std::move(checks)},
        {"failures", std::move(failures)},
        {"passed", passed},
    };
}

} // namespace rtv
