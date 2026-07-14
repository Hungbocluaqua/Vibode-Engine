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
    gi.SetResamplingMode(rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial);
    auto& pt = context_->GetReSTIRPTContext();
    pt.SetResamplingMode(rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial);
}

} // namespace rtv
