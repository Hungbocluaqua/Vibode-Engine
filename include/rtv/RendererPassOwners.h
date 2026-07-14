#pragma once

#include "rtv/passes/CompositePass.h"
#include "rtv/passes/DebugViewPass.h"
#include "rtv/passes/DenoiserPass.h"
#include "rtv/passes/DlssPass.h"
#include "rtv/passes/GBufferPass.h"
#include "rtv/passes/NrdPass.h"
#include "rtv/passes/PathTracePass.h"
#include "rtv/passes/ProfilerPass.h"
#include "rtv/passes/RegirPass.h"
#include "rtv/passes/RestirDIPass.h"
#include "rtv/passes/RestirGIPass.h"
#include "rtv/passes/TemporalAAPass.h"

#include <array>
#include <cstddef>
#include <string_view>

#include <nlohmann/json.hpp>

namespace rtv {

namespace renderer_pass_owners {

using GBufferPassOwner = passes::GBufferPass;
using PathTracePassOwner = passes::PathTracePass;
using RestirDIPassOwner = passes::RestirDIPass;
using RestirGIPassOwner = passes::RestirGIPass;
using RegirPassOwner = passes::RegirPass;
using TemporalAAPassOwner = passes::TemporalAAPass;
using DenoiserPassOwner = passes::DenoiserPass;
using NrdPassOwner = passes::NrdPass;
using DlssPassOwner = passes::DlssPass;
using CompositePassOwner = passes::CompositePass;
using DebugViewPassOwner = passes::DebugViewPass;
using ProfilerPassOwner = passes::ProfilerPass;

} // namespace renderer_pass_owners

struct RendererPassOwnerMetadata {
    const char* contractId = "";
    const char* passName = "";
    const char* ownerSymbol = "";
    const char* metadataHeader = "";
    const char* plannedImplementationHeader = "";
    const char* extractionState = "";
};

inline constexpr std::array<RendererPassOwnerMetadata, 12> rendererPassOwnerRegistry() {
    return {{
        {
            renderer_pass_owners::GBufferPassOwner::kContractId,
            renderer_pass_owners::GBufferPassOwner::kPassName,
            "rtv::passes::GBufferPass",
            "include/rtv/passes/GBufferPass.h",
            "include/rtv/passes/GBufferPass.h",
            renderer_pass_owners::GBufferPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::PathTracePassOwner::kContractId,
            renderer_pass_owners::PathTracePassOwner::kPassName,
            "rtv::passes::PathTracePass",
            "include/rtv/passes/PathTracePass.h",
            "include/rtv/passes/PathTracePass.h",
            renderer_pass_owners::PathTracePassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::RestirDIPassOwner::kContractId,
            renderer_pass_owners::RestirDIPassOwner::kPassName,
            "rtv::passes::RestirDIPass",
            "include/rtv/passes/RestirDIPass.h",
            "include/rtv/passes/RestirDIPass.h",
            renderer_pass_owners::RestirDIPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::RestirGIPassOwner::kContractId,
            renderer_pass_owners::RestirGIPassOwner::kPassName,
            "rtv::passes::RestirGIPass",
            "include/rtv/passes/RestirGIPass.h",
            "include/rtv/passes/RestirGIPass.h",
            renderer_pass_owners::RestirGIPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::RegirPassOwner::kContractId,
            renderer_pass_owners::RegirPassOwner::kPassName,
            "rtv::passes::RegirPass",
            "include/rtv/passes/RegirPass.h",
            "include/rtv/passes/RegirPass.h",
            renderer_pass_owners::RegirPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::TemporalAAPassOwner::kContractId,
            renderer_pass_owners::TemporalAAPassOwner::kPassName,
            "rtv::passes::TemporalAAPass",
            "include/rtv/passes/TemporalAAPass.h",
            "include/rtv/passes/TemporalAAPass.h",
            renderer_pass_owners::TemporalAAPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::DenoiserPassOwner::kContractId,
            renderer_pass_owners::DenoiserPassOwner::kPassName,
            "rtv::passes::DenoiserPass",
            "include/rtv/passes/DenoiserPass.h",
            "include/rtv/passes/DenoiserPass.h",
            renderer_pass_owners::DenoiserPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::NrdPassOwner::kContractId,
            renderer_pass_owners::NrdPassOwner::kPassName,
            "rtv::passes::NrdPass",
            "include/rtv/passes/NrdPass.h",
            "include/rtv/passes/NrdPass.h",
            renderer_pass_owners::NrdPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::DlssPassOwner::kContractId,
            renderer_pass_owners::DlssPassOwner::kPassName,
            "rtv::passes::DlssPass",
            "include/rtv/passes/DlssPass.h",
            "include/rtv/passes/DlssPass.h",
            renderer_pass_owners::DlssPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::CompositePassOwner::kContractId,
            renderer_pass_owners::CompositePassOwner::kPassName,
            "rtv::passes::CompositePass",
            "include/rtv/passes/CompositePass.h",
            "include/rtv/passes/CompositePass.h",
            renderer_pass_owners::CompositePassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::DebugViewPassOwner::kContractId,
            renderer_pass_owners::DebugViewPassOwner::kPassName,
            "rtv::passes::DebugViewPass",
            "include/rtv/passes/DebugViewPass.h",
            "include/rtv/passes/DebugViewPass.h",
            renderer_pass_owners::DebugViewPassOwner::kExtractionState,
        },
        {
            renderer_pass_owners::ProfilerPassOwner::kContractId,
            renderer_pass_owners::ProfilerPassOwner::kPassName,
            "rtv::passes::ProfilerPass",
            "include/rtv/passes/ProfilerPass.h",
            "include/rtv/passes/ProfilerPass.h",
            renderer_pass_owners::ProfilerPassOwner::kExtractionState,
        },
    }};
}

inline const RendererPassOwnerMetadata* rendererPassOwnerMetadataForId(std::string_view contractId) {
    static constexpr auto registry = rendererPassOwnerRegistry();
    for (const RendererPassOwnerMetadata& owner : registry) {
        if (owner.contractId == contractId) {
            return &owner;
        }
    }
    return nullptr;
}

inline nlohmann::json rendererPassOwnerMetadataJson(const RendererPassOwnerMetadata& owner) {
    return nlohmann::json{
        {"contract_id", owner.contractId},
        {"pass_name", owner.passName},
        {"owner_symbol", owner.ownerSymbol},
        {"metadata_header", owner.metadataHeader},
        {"planned_implementation_header", owner.plannedImplementationHeader},
        {"extraction_state", owner.extractionState},
    };
}

} // namespace rtv
