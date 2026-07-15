#include "rtv/RtxdiRuntime.h"

#include <algorithm>
#include <stdexcept>

namespace rtv {

namespace {

uint64_t reservoirBytes(
    const RTXDI_ReservoirBufferParameters& params,
    uint32_t bufferCount,
    uint64_t reservoirStride) {
    return static_cast<uint64_t>(params.reservoirArrayPitch) * bufferCount * reservoirStride;
}

} // namespace

RtxdiRuntime::RtxdiRuntime(const RtxdiRuntimeConfig& config) {
    recreate(config);
}

void RtxdiRuntime::recreate(const RtxdiRuntimeConfig& config) {
    if (config.renderWidth == 0 || config.renderHeight == 0) {
        throw std::invalid_argument("RTXDI render extent must be non-zero");
    }

    config_ = config;
    rtxdi::ImportanceSamplingContext_StaticParameters params{};
    params.renderWidth = config.renderWidth;
    params.renderHeight = config.renderHeight;
    params.CheckerboardSamplingMode = config.checkerboard
        ? rtxdi::CheckerboardMode::Black
        : rtxdi::CheckerboardMode::Off;
    context_ = std::make_unique<rtxdi::ImportanceSamplingContext>(params);
    applyQualityPreset();
}

void RtxdiRuntime::beginFrame(uint32_t frameIndex) {
    context_->GetReSTIRDIContext().SetFrameIndex(frameIndex);
    context_->GetReSTIRGIContext().SetFrameIndex(frameIndex);
    context_->GetReSTIRPTContext().SetFrameIndex(frameIndex);
}

RtxdiMemoryRequirements RtxdiRuntime::memoryRequirements() const {
    RtxdiMemoryRequirements result{};
    result.diReservoirBytes = reservoirBytes(
        context_->GetReSTIRDIContext().GetReservoirBufferParameters(),
        rtxdi::c_NumReSTIRDIReservoirBuffers,
        sizeof(RTXDI_PackedDIReservoir));
    result.giReservoirBytes = reservoirBytes(
        context_->GetReSTIRGIContext().GetReservoirBufferParameters(),
        rtxdi::c_NumReSTIRGIReservoirBuffers,
        sizeof(RTXDI_PackedGIReservoir));
    result.ptReservoirBytes = reservoirBytes(
        context_->GetReSTIRPTContext().GetReservoirBufferParameters(),
        rtxdi::c_NumReSTIRPTReservoirBuffers,
        sizeof(RTXDI_PackedPTReservoir));
    result.totalReservoirBytes = result.diReservoirBytes + result.giReservoirBytes + result.ptReservoirBytes;
    return result;
}

void RtxdiRuntime::applyQualityPreset() {
    auto& di = context_->GetReSTIRDIContext();
    auto initial = rtxdi::GetDefaultReSTIRDIInitialSamplingParams();
    auto temporal = rtxdi::GetDefaultReSTIRDITemporalResamplingParams();
    auto boiling = rtxdi::GetDefaultReSTIRDIBoilingFilterParams();
    auto spatial = rtxdi::GetDefaultReSTIRDISpatialResamplingParams();
    auto shading = rtxdi::GetDefaultReSTIRDIShadingParams();

    di.SetResamplingMode(rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial);
    switch (config_.qualityPreset) {
    case RtxdiQualityPreset::Fast:
        initial.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
        initial.numLocalLightSamples = 4;
        initial.numBrdfSamples = 0;
        initial.numInfiniteLightSamples = 1;
        temporal.enableVisibilityShortcut = true;
        temporal.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Off;
        boiling.enableBoilingFilter = true;
        boiling.boilingFilterStrength = 0.2f;
        spatial.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Off;
        spatial.numSamples = 1;
        spatial.numDisocclusionBoostSamples = 2;
        shading.reuseFinalVisibility = true;
        break;
    case RtxdiQualityPreset::Medium:
        initial.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        initial.numLocalLightSamples = 8;
        initial.numBrdfSamples = 1;
        initial.numInfiniteLightSamples = 1;
        temporal.enableVisibilityShortcut = true;
        temporal.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        boiling.enableBoilingFilter = true;
        boiling.boilingFilterStrength = 0.2f;
        spatial.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
        spatial.numSamples = 1;
        spatial.numDisocclusionBoostSamples = 8;
        shading.reuseFinalVisibility = true;
        break;
    case RtxdiQualityPreset::Unbiased:
    case RtxdiQualityPreset::Reference:
        initial.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        initial.numLocalLightSamples = config_.qualityPreset == RtxdiQualityPreset::Reference ? 16 : 8;
        initial.numBrdfSamples = 1;
        initial.numInfiniteLightSamples = 1;
        temporal.enableVisibilityShortcut = false;
        temporal.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        boiling.enableBoilingFilter = false;
        spatial.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        spatial.numSamples = config_.qualityPreset == RtxdiQualityPreset::Reference ? 8 : 1;
        spatial.numDisocclusionBoostSamples = config_.qualityPreset == RtxdiQualityPreset::Reference ? 32 : 8;
        shading.reuseFinalVisibility = false;
        break;
    case RtxdiQualityPreset::Ultra:
        initial.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        initial.numLocalLightSamples = 16;
        initial.numBrdfSamples = 1;
        initial.numInfiniteLightSamples = 1;
        temporal.enableVisibilityShortcut = false;
        temporal.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        boiling.enableBoilingFilter = false;
        spatial.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        spatial.numSamples = 4;
        spatial.numDisocclusionBoostSamples = 16;
        shading.reuseFinalVisibility = false;
        break;
    }

    di.SetInitialSamplingParameters(initial);
    di.SetTemporalResamplingParameters(temporal);
    di.SetBoilingFilterParameters(boiling);
    di.SetSpatialResamplingParameters(spatial);
    di.SetShadingParameters(shading);

    auto& gi = context_->GetReSTIRGIContext();
    auto giTemporal = rtxdi::GetDefaultReSTIRGITemporalResamplingParams();
    auto giBoiling = rtxdi::GetDefaultReSTIRGIBoilingFilterParams();
    auto giSpatial = rtxdi::GetDefaultReSTIRGISpatialResamplingParams();
    auto giFinal = rtxdi::GetDefaultReSTIRGIFinalShadingParams();
    gi.SetResamplingMode(rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial);

    auto& pt = context_->GetReSTIRPTContext();
    auto ptInitial = rtxdi::GetDefaultReSTIRPTInitialSamplingParams();
    auto ptTemporal = rtxdi::GetDefaultReSTIRPTTemporalResamplingParams();
    auto ptReconnection = rtxdi::GetDefaultReSTIRPTReconnectionParameters();
    auto ptHybrid = rtxdi::GetDefaultReSTIRPTHybridShiftParams();
    auto ptBoiling = rtxdi::GetDefaultReSTIRPTBoilingFilterParams();
    auto ptSpatial = rtxdi::GetDefaultReSTIRPTSpatialResamplingParams();
    pt.SetResamplingMode(rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial);

    switch (config_.qualityPreset) {
    case RtxdiQualityPreset::Fast:
        giTemporal.maxHistoryLength = 8;
        giTemporal.maxReservoirAge = 16;
        giTemporal.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Off;
        giSpatial.numSamples = 1;
        giSpatial.samplingRadius = 16.0f;
        giSpatial.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Off;
        giBoiling.enableBoilingFilter = 1;
        giBoiling.boilingFilterStrength = 0.2f;
        ptInitial.numInitialSamples = 1;
        ptInitial.maxBounceDepth = 3;
        ptInitial.maxRcVertexLength = 2;
        ptTemporal.maxHistoryLength = 8;
        ptTemporal.maxReservoirAge = 16;
        ptTemporal.enableVisibilityBeforeCombine = 0;
        ptSpatial.numSpatialSamples = 1;
        ptSpatial.numDisocclusionBoostSamples = 4;
        ptSpatial.samplingRadius = 16.0f;
        ptBoiling.enableBoilingFilter = 1;
        ptBoiling.boilingFilterStrength = 0.2f;
        break;
    case RtxdiQualityPreset::Medium:
        giTemporal.maxHistoryLength = 16;
        giTemporal.maxReservoirAge = 32;
        giTemporal.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Basic;
        giSpatial.numSamples = 2;
        giSpatial.samplingRadius = 24.0f;
        giSpatial.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Basic;
        giBoiling.enableBoilingFilter = 1;
        giBoiling.boilingFilterStrength = 0.15f;
        ptInitial.numInitialSamples = 1;
        ptInitial.maxBounceDepth = 5;
        ptInitial.maxRcVertexLength = 3;
        ptTemporal.maxHistoryLength = 16;
        ptTemporal.maxReservoirAge = 24;
        ptTemporal.enableVisibilityBeforeCombine = 1;
        ptSpatial.numSpatialSamples = 2;
        ptSpatial.numDisocclusionBoostSamples = 8;
        ptSpatial.samplingRadius = 24.0f;
        ptBoiling.enableBoilingFilter = 1;
        ptBoiling.boilingFilterStrength = 0.15f;
        break;
    case RtxdiQualityPreset::Unbiased:
        giTemporal.maxHistoryLength = 24;
        giTemporal.maxReservoirAge = 48;
        giTemporal.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Raytraced;
        giSpatial.numSamples = 2;
        giSpatial.samplingRadius = 24.0f;
        giSpatial.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Raytraced;
        giBoiling.enableBoilingFilter = 0;
        ptInitial.numInitialSamples = 1;
        ptInitial.maxBounceDepth = 6;
        ptInitial.maxRcVertexLength = 4;
        ptTemporal.maxHistoryLength = 24;
        ptTemporal.maxReservoirAge = 31;
        ptTemporal.enableVisibilityBeforeCombine = 1;
        ptSpatial.numSpatialSamples = 2;
        ptSpatial.numDisocclusionBoostSamples = 8;
        ptSpatial.samplingRadius = 24.0f;
        ptBoiling.enableBoilingFilter = 0;
        break;
    case RtxdiQualityPreset::Ultra:
        giTemporal.maxHistoryLength = 32;
        giTemporal.maxReservoirAge = 64;
        giTemporal.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Raytraced;
        giSpatial.numSamples = 4;
        giSpatial.samplingRadius = 32.0f;
        giSpatial.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Raytraced;
        giBoiling.enableBoilingFilter = 0;
        ptInitial.numInitialSamples = 2;
        ptInitial.maxBounceDepth = 8;
        ptInitial.maxRcVertexLength = 5;
        ptTemporal.maxHistoryLength = 32;
        ptTemporal.maxReservoirAge = 31;
        ptTemporal.enableVisibilityBeforeCombine = 1;
        ptSpatial.numSpatialSamples = 4;
        ptSpatial.numDisocclusionBoostSamples = 16;
        ptSpatial.samplingRadius = 32.0f;
        ptBoiling.enableBoilingFilter = 0;
        break;
    case RtxdiQualityPreset::Reference:
        giTemporal.maxHistoryLength = 64;
        giTemporal.maxReservoirAge = 96;
        giTemporal.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Raytraced;
        giSpatial.numSamples = 8;
        giSpatial.samplingRadius = 48.0f;
        giSpatial.biasCorrectionMode = RTXDI_GIBiasCorrectionMode::Raytraced;
        giBoiling.enableBoilingFilter = 0;
        ptInitial.numInitialSamples = 4;
        ptInitial.maxBounceDepth = 12;
        ptInitial.maxRcVertexLength = 8;
        ptTemporal.maxHistoryLength = 64;
        ptTemporal.maxReservoirAge = 31;
        ptTemporal.enableVisibilityBeforeCombine = 1;
        ptSpatial.numSpatialSamples = 8;
        ptSpatial.numDisocclusionBoostSamples = 32;
        ptSpatial.samplingRadius = 48.0f;
        ptBoiling.enableBoilingFilter = 0;
        break;
    }

    giFinal.enableFinalVisibility = 1;
    gi.SetTemporalResamplingParameters(giTemporal);
    gi.SetBoilingFilterParameters(giBoiling);
    gi.SetSpatialResamplingParameters(giSpatial);
    gi.SetFinalShadingParameters(giFinal);

    ptHybrid.maxBounceDepth = ptInitial.maxBounceDepth;
    ptHybrid.maxRcVertexLength = ptInitial.maxRcVertexLength;
    ptReconnection.reconnectionMode = RTXDI_PTReconnectionMode::Footprint;
    pt.SetInitialSamplingParameters(ptInitial);
    pt.SetTemporalResamplingParameters(ptTemporal);
    pt.SetReconnectionParameters(ptReconnection);
    pt.SetHybridShiftParameters(ptHybrid);
    pt.SetBoilingFilterParameters(ptBoiling);
    pt.SetSpatialResamplingParameters(ptSpatial);
}

} // namespace rtv
