#include "rtv/RendererDebug.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace rtv {

namespace {

[[nodiscard]] std::string normalized(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    result.erase(std::remove(result.begin(), result.end(), '-'), result.end());
    result.erase(std::remove(result.begin(), result.end(), '_'), result.end());
    return result;
}

} // namespace

const char* toneMapperName(ToneMapper toneMapper) {
    switch (toneMapper) {
    case ToneMapper::Linear: return "linear";
    case ToneMapper::Reinhard: return "reinhard";
    case ToneMapper::ReinhardWhite: return "reinhard-white";
    case ToneMapper::ACES: return "aces";
    case ToneMapper::PBRNeutral: return "pbr-neutral";
    case ToneMapper::AgX: return "agx";
    }
    return "aces";
}

const char* restirModeName(RestirMode mode) {
    switch (mode) {
    case RestirMode::ClassicNee: return "classic-nee";
    case RestirMode::RestirOnly: return "restir-only";
    case RestirMode::HybridCompare: return "hybrid-compare";
    }
    return "classic-nee";
}

const char* restirDiModeName(RestirDiMode mode) {
    switch (mode) {
    case RestirDiMode::Off: return "off";
    case RestirDiMode::Legacy: return "legacy";
    case RestirDiMode::Production: return "production";
    case RestirDiMode::ReferenceValidation: return "reference-validation";
    case RestirDiMode::HybridCompare: return "hybrid-compare";
    }
    return "off";
}

bool tryParseRestirDiMode(std::string_view value, RestirDiMode& out) {
    const std::string key = normalized(value);
    if (key == "off" || key == "disabled" || key == "none") { out = RestirDiMode::Off; return true; }
    if (key == "legacy" || key == "old" || key == "current") { out = RestirDiMode::Legacy; return true; }
    if (key == "production" || key == "prod" || key == "new" || key == "default") { out = RestirDiMode::Production; return true; }
    if (key == "reference" || key == "validation" || key == "referencevalidation" || key == "ref") { out = RestirDiMode::ReferenceValidation; return true; }
    if (key == "hybrid" || key == "hybridcompare" || key == "compare") { out = RestirDiMode::HybridCompare; return true; }
    return false;
}

RestirDiMode parseRestirDiMode(std::string_view value) {
    RestirDiMode mode = RestirDiMode::Off;
    return tryParseRestirDiMode(value, mode) ? mode : RestirDiMode::Off;
}

const char* restirDiReservoirLayoutName(RestirDiReservoirLayout layout) {
    switch (layout) {
    case RestirDiReservoirLayout::Legacy: return "legacy";
    case RestirDiReservoirLayout::ProductionPacked: return "production-packed";
    case RestirDiReservoirLayout::ValidationFull: return "validation-full";
    }
    return "legacy";
}

RestirDiReservoirLayout parseRestirDiReservoirLayout(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "legacy" || key == "old") { return RestirDiReservoirLayout::Legacy; }
    if (key == "packed" || key == "production" || key == "productionpacked" || key == "compact") { return RestirDiReservoirLayout::ProductionPacked; }
    if (key == "full" || key == "validation" || key == "validationfull" || key == "uncompressed") { return RestirDiReservoirLayout::ValidationFull; }
    return RestirDiReservoirLayout::Legacy;
}

const char* restirGiModeName(RestirGiMode mode) {
    switch (mode) {
    case RestirGiMode::Off: return "off";
    case RestirGiMode::LegacyCache: return "legacy-cache";
    case RestirGiMode::Production: return "production";
    case RestirGiMode::ReferenceValidation: return "reference-validation";
    }
    return "off";
}

bool tryParseRestirGiMode(std::string_view value, RestirGiMode& out) {
    const std::string key = normalized(value);
    if (key == "off" || key == "false" || key == "0" || key == "disabled" || key == "none") { out = RestirGiMode::Off; return true; }
    if (key == "legacy" || key == "legacycache" || key == "old" || key == "current") { out = RestirGiMode::LegacyCache; return true; }
    if (key == "on" || key == "true" || key == "1" ||
        key == "production" || key == "prod" || key == "new" || key == "default") { out = RestirGiMode::Production; return true; }
    if (key == "reference" || key == "validation" || key == "referencevalidation" || key == "ref") { out = RestirGiMode::ReferenceValidation; return true; }
    return false;
}

RestirGiMode parseRestirGiMode(std::string_view value) {
    RestirGiMode mode = RestirGiMode::LegacyCache;
    return tryParseRestirGiMode(value, mode) ? mode : RestirGiMode::LegacyCache;
}

const char* restirGiReservoirLayoutName(RestirGiReservoirLayout layout) {
    switch (layout) {
    case RestirGiReservoirLayout::LegacyCachePacked: return "legacy-cache-packed";
    case RestirGiReservoirLayout::ProductionPacked: return "production-packed";
    case RestirGiReservoirLayout::ValidationFull: return "validation-full";
    }
    return "production-packed";
}

RestirGiReservoirLayout parseRestirGiReservoirLayout(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "legacy" || key == "legacycachepacked" || key == "legacycache" || key == "old") { return RestirGiReservoirLayout::LegacyCachePacked; }
    if (key == "packed" || key == "production" || key == "productionpacked" || key == "compact" || key == "compressed") { return RestirGiReservoirLayout::ProductionPacked; }
    if (key == "full" || key == "validation" || key == "validationfull" || key == "uncompressed") { return RestirGiReservoirLayout::ValidationFull; }
    return RestirGiReservoirLayout::ProductionPacked;
}

const char* restirGiActiveTileMaskModeName(RestirGiActiveTileMaskMode mode) {
    switch (mode) {
    case RestirGiActiveTileMaskMode::Off: return "off";
    case RestirGiActiveTileMaskMode::On: return "on";
    case RestirGiActiveTileMaskMode::Auto: return "auto";
    }
    return "off";
}

RestirGiActiveTileMaskMode parseRestirGiActiveTileMaskMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "on" || key == "true" || key == "1" || key == "yes") {
        return RestirGiActiveTileMaskMode::On;
    }
    if (key == "auto") {
        return RestirGiActiveTileMaskMode::Auto;
    }
    return RestirGiActiveTileMaskMode::Off;
}

const char* restirHistoryCopyModeName(RestirHistoryCopyMode mode) {
    switch (mode) {
    case RestirHistoryCopyMode::Copy: return "copy";
    case RestirHistoryCopyMode::PingPong: return "pingpong";
    }
    return "copy";
}

RestirHistoryCopyMode parseRestirHistoryCopyMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "pingpong") {
        return RestirHistoryCopyMode::PingPong;
    }
    return RestirHistoryCopyMode::Copy;
}

const char* reservoirLayoutName(ReservoirLayout layout) {
    switch (layout) {
    case ReservoirLayout::LegacyDI: return "legacy-di";
    case ReservoirLayout::LegacyGI: return "legacy-gi";
    case ReservoirLayout::PathSpace: return "path-space";
    case ReservoirLayout::PathSpaceCompressed: return "path-space-compressed";
    }
    return "legacy-di";
}

ReservoirLayout parseReservoirLayout(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "legacygi" || key == "gi" || key == "restirgi") {
        return ReservoirLayout::LegacyGI;
    }
    if (key == "pathspace" || key == "path" || key == "restirpt") {
        return ReservoirLayout::PathSpace;
    }
    if (key == "pathspacecompressed" || key == "compressed" || key == "compactpath") {
        return ReservoirLayout::PathSpaceCompressed;
    }
    return ReservoirLayout::LegacyDI;
}

const char* lightingReuseModeName(LightingReuseMode mode) {
    switch (mode) {
    case LightingReuseMode::LegacyRestirDiGi: return "legacy";
    case LightingReuseMode::LegacyRestirDiGiPlusReGIR: return "legacy-regir";
    case LightingReuseMode::ExperimentalRestirPT: return "experimental-restir-pt";
    case LightingReuseMode::ValidateRestirPTAgainstLegacy: return "validate-restir-pt";
    }
    return "legacy";
}

LightingReuseMode parseLightingReuseMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "legacyregir" || key == "regir" || key == "legacyrestirdigiplusregir") {
        return LightingReuseMode::LegacyRestirDiGiPlusReGIR;
    }
    if (key == "experimentalrestirpt" || key == "restirpt" || key == "experimental") {
        return LightingReuseMode::ExperimentalRestirPT;
    }
    if (key == "validaterestirpt" || key == "validate" || key == "compare" || key == "ab") {
        return LightingReuseMode::ValidateRestirPTAgainstLegacy;
    }
    return LightingReuseMode::LegacyRestirDiGi;
}

const char* regirQueryModeName(RegirQueryMode mode) {
    switch (mode) {
    case RegirQueryMode::Deterministic: return "deterministic";
    case RegirQueryMode::Stochastic: return "stochastic";
    }
    return "stochastic";
}

RegirQueryMode parseRegirQueryMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "deterministic" || key == "debug" || key == "repro") {
        return RegirQueryMode::Deterministic;
    }
    if (key == "stochastic" || key == "jittered" || key == "production") {
        return RegirQueryMode::Stochastic;
    }
    return RegirQueryMode::Stochastic;
}

const char* regirGridModeName(RegirGridMode mode) {
    switch (mode) {
    case RegirGridMode::Dense: return "dense";
    case RegirGridMode::Active: return "active";
    case RegirGridMode::Hash: return "hash";
    }
    return "dense";
}

RegirGridMode parseRegirGridMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "active" || key == "activegrid") {
        return RegirGridMode::Active;
    }
    if (key == "hash" || key == "hashed" || key == "hashgrid") {
        return RegirGridMode::Hash;
    }
    return RegirGridMode::Dense;
}

const char* adaptiveSamplingModeName(AdaptiveSamplingMode mode) {
    switch (mode) {
    case AdaptiveSamplingMode::Disabled: return "disabled";
    case AdaptiveSamplingMode::Heuristic: return "heuristic";
    case AdaptiveSamplingMode::Neural: return "neural";
    }
    return "disabled";
}

AdaptiveSamplingMode parseAdaptiveSamplingMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "on" || key == "true" || key == "1" || key == "heuristic") {
        return AdaptiveSamplingMode::Heuristic;
    }
    if (key == "neural" || key == "learned" || key == "unet") {
        return AdaptiveSamplingMode::Neural;
    }
    return AdaptiveSamplingMode::Disabled;
}

const char* mixedSidedSplitModeName(MixedSidedSplitMode mode) {
    switch (mode) {
    case MixedSidedSplitMode::Off: return "off";
    case MixedSidedSplitMode::Compact: return "compact";
    }
    return "off";
}

MixedSidedSplitMode parseMixedSidedSplitMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "compact" || key == "on" || key == "true" || key == "1" || key == "yes") {
        return MixedSidedSplitMode::Compact;
    }
    return MixedSidedSplitMode::Off;
}

const char* pathTraceKernelModeName(PathTraceKernelMode mode) {
    switch (mode) {
    case PathTraceKernelMode::Generic: return "generic";
    case PathTraceKernelMode::Native2B: return "native2b";
    }
    return "generic";
}

PathTraceKernelMode parsePathTraceKernelMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "native2b" || key == "native2bounce" || key == "native2" || key == "2bounce") {
        return PathTraceKernelMode::Native2B;
    }
    return PathTraceKernelMode::Generic;
}

const char* blendedDecalShadowModeName(BlendedDecalShadowMode mode) {
    switch (mode) {
    case BlendedDecalShadowMode::Exact: return "exact";
    case BlendedDecalShadowMode::OpaqueShadow: return "opaque-shadow";
    case BlendedDecalShadowMode::AlphaCutoutProxy: return "alpha-cutout-proxy";
    }
    return "exact";
}

BlendedDecalShadowMode parseBlendedDecalShadowMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "opaqueshadow" || key == "opaque" || key == "shadowopaque") {
        return BlendedDecalShadowMode::OpaqueShadow;
    }
    if (key == "alphacutoutproxy" || key == "cutout" || key == "alphacutout" || key == "proxy") {
        return BlendedDecalShadowMode::AlphaCutoutProxy;
    }
    return BlendedDecalShadowMode::Exact;
}

const char* native2BDirectReuseModeName(Native2BDirectReuseMode mode) {
    switch (mode) {
    case Native2BDirectReuseMode::Off: return "off";
    case Native2BDirectReuseMode::Ris: return "ris";
    case Native2BDirectReuseMode::Temporal: return "temporal";
    }
    return "off";
}

Native2BDirectReuseMode parseNative2BDirectReuseMode(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "ris") {
        return Native2BDirectReuseMode::Ris;
    }
    if (key == "temporal" || key == "temporalreuse") {
        return Native2BDirectReuseMode::Temporal;
    }
    return Native2BDirectReuseMode::Off;
}

const char* renderPresetName(RenderPreset preset) {
    switch (preset) {
    case RenderPreset::Custom: return "custom";
    case RenderPreset::Low: return "low";
    case RenderPreset::Balanced: return "balanced";
    case RenderPreset::Ultra: return "ultra";
    case RenderPreset::Native30: return "native30";
    }
    return "custom";
}

RenderPreset parseRenderPreset(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "low" || key == "performance") { return RenderPreset::Low; }
    if (key == "balanced" || key == "game" || key == "default") { return RenderPreset::Balanced; }
    if (key == "ultra" || key == "quality" || key == "reference") { return RenderPreset::Ultra; }
    if (key == "native30" || key == "native" || key == "1440p" || key == "realtimenative") { return RenderPreset::Native30; }
    return RenderPreset::Custom;
}

const char* denoiserBackendName(DenoiserBackend backend) {
    switch (backend) {
    case DenoiserBackend::Engine: return "engine";
    case DenoiserBackend::Nrd: return "nrd";
    }
    return "engine";
}

DenoiserBackend parseDenoiserBackend(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "nrd" || key == "nvidia" || key == "nvidianrd") { return DenoiserBackend::Nrd; }
    return DenoiserBackend::Engine;
}

const char* temporalUpscalerName(TemporalUpscaler upscaler) {
    switch (upscaler) {
    case TemporalUpscaler::TaaTsr: return "taa-tsr";
    case TemporalUpscaler::Dlss: return "dlss";
    case TemporalUpscaler::Nis: return "nis";
    }
    return "taa-tsr";
}

TemporalUpscaler parseTemporalUpscaler(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "dlss" || key == "ngx" || key == "nvidiadlss") { return TemporalUpscaler::Dlss; }
    if (key == "nis" || key == "nvidiaimage" || key == "nvidiaimagescaling" || key == "nvidiascaling") {
        return TemporalUpscaler::Nis;
    }
    return TemporalUpscaler::TaaTsr;
}

RendererDebugView parseRendererDebugView(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "variance") { return RendererDebugView::Variance; }
    if (key == "normal" || key == "normals") { return RendererDebugView::Normals; }
    if (key == "reprojection" || key == "reprojectionconfidence") { return RendererDebugView::ReprojectionConfidence; }
    if (key == "rejection" || key == "denoiserrejection") { return RendererDebugView::DenoiserRejection; }
    if (key == "depth") { return RendererDebugView::Depth; }
    if (key == "roughness") { return RendererDebugView::Roughness; }
    if (key == "metallic" || key == "metalness" || key == "metal") { return RendererDebugView::Metallic; }
    if (key == "alpha" || key == "materialalpha" || key == "materialopacity") { return RendererDebugView::MaterialAlpha; }
    if (key == "transmission" || key == "materialtransmission" || key == "transmissionfactor") { return RendererDebugView::MaterialTransmission; }
    if (key == "materialworkflow" || key == "workflow" || key == "materialtype" || key == "mattype") { return RendererDebugView::MaterialWorkflow; }
    if (key == "restirdilight" || key == "restirdiselectedlight" || key == "dilight") { return RendererDebugView::RestirDiSelectedLight; }
    if (key == "restirditarget" || key == "restirditargetfunction" || key == "ditarget") { return RendererDebugView::RestirDiTarget; }
    if (key == "restirdisourcepdf" || key == "restirdipdf" || key == "disourcepdf") { return RendererDebugView::RestirDiSourcePdf; }
    if (key == "restirdivisibility" || key == "restirdivis" || key == "divisibility") { return RendererDebugView::RestirDiVisibility; }
    if (key == "restirdirejection" || key == "restirdirejectionreason" || key == "direjection") { return RendererDebugView::RestirDiRejectionReason; }
    if (key == "restirditemporalacceptance" || key == "ditacceptance") { return RendererDebugView::RestirDiTemporalAcceptance; }
    if (key == "restirdispatialacceptance" || key == "disacceptance") { return RendererDebugView::RestirDiSpatialAcceptance; }
    if (key == "restirdifinal" || key == "restirdicontribution" || key == "difinalcontribution") { return RendererDebugView::RestirDiFinalContribution; }
    if (key == "restirdireceiver" || key == "restirdireceiverposition" || key == "direceiver") { return RendererDebugView::RestirDiReceiverPosition; }
    if (key == "restirdinormal" || key == "restirdireceivernormal" || key == "dinormal") { return RendererDebugView::RestirDiReceiverNormal; }
    if (key == "restirdilightversion" || key == "dilightversion") { return RendererDebugView::RestirDiLightVersion; }
    if (key == "restirdilightmapstatus" || key == "dilightmapstatus" || key == "restirdilightmapping" || key == "dilightmapping") { return RendererDebugView::RestirDiLightMapStatus; }
    if (key == "restirdiweightsum" || key == "diweightsum" || key == "restirdiw") { return RendererDebugView::RestirDiWeightSum; }
    if (key == "restirdim" || key == "dim" || key == "restirdisamplecount") { return RendererDebugView::RestirDiM; }
    if (key == "restirdilightclass" || key == "dilightclass" || key == "restirdilighttype" || key == "dilighttype") { return RendererDebugView::RestirDiLightClass; }
    if (key == "restirdiage" || key == "diage") { return RendererDebugView::RestirDiAge; }
    if (key == "restirdiconfidence" || key == "diconfidence") { return RendererDebugView::RestirDiConfidence; }
    if (key == "restirdireferencediff" || key == "direferencediff") { return RendererDebugView::RestirDiReferenceDiff; }
    if (key == "restirdiinitial" || key == "restirdiinitreservoir" || key == "diinitialreservoir") { return RendererDebugView::RestirDiInitialReservoir; }
    if (key == "restirditemporalreservoir" || key == "ditemporalreservoir") { return RendererDebugView::RestirDiTemporalReservoir; }
    if (key == "restirdispatialreservoir" || key == "dispatialreservoir") { return RendererDebugView::RestirDiSpatialReservoir; }
    if (key == "restirdifinalreservoir" || key == "difinalreservoir") { return RendererDebugView::RestirDiFinalReservoir; }
    if (key == "direct" || key == "directlighting") { return RendererDebugView::DirectLighting; }
    if (key == "indirect" || key == "indirectlighting") { return RendererDebugView::IndirectLighting; }
    if (key == "emissive" || key == "emissivecontribution") { return RendererDebugView::EmissiveContribution; }
    if (key == "emissivecontinuation" || key == "emissivecontinue" || key == "continuedemissive") {
        return RendererDebugView::EmissiveContinuation;
    }
    if (key == "environment" || key == "env" || key == "environmentcontribution") { return RendererDebugView::EnvironmentContribution; }
    if (key == "traversal" || key == "traversalsteps") { return RendererDebugView::TraversalSteps; }
    if (key == "bvh" || key == "bvhdepth") { return RendererDebugView::BvhDepth; }
    if (key == "instance" || key == "instanceid") { return RendererDebugView::InstanceId; }
    if (key == "mesh" || key == "meshid") { return RendererDebugView::MeshId; }
    if (key == "tlas" || key == "tlassteps") { return RendererDebugView::TlasSteps; }
    if (key == "mismatch" || key == "traversalmismatch" || key == "tlasmismatch") { return RendererDebugView::TraversalMismatch; }
    if (key == "lightpdf" || key == "directpdf") { return RendererDebugView::LightPdf; }
    if (key == "bsdfpdf" || key == "brdfpdf") { return RendererDebugView::BsdfPdf; }
    if (key == "mis" || key == "misweight") { return RendererDebugView::MisWeight; }
    if (key == "sunmis" || key == "sunmisweight") { return RendererDebugView::SunMisWeight; }
    if (key == "sunpdf" || key == "sunlightpdf") { return RendererDebugView::SunLightPdf; }
    if (key == "sunbsdfpdf" || key == "sunpreviousbsdfpdf" || key == "sunprevbsdfpdf") {
        return RendererDebugView::SunPreviousBsdfPdf;
    }
    if (key == "risrawpdf" || key == "risrawlightpdf") { return RendererDebugView::RisRawLightPdf; }
    if (key == "riseffectivepdf" || key == "riseffectivelightpdf") { return RendererDebugView::RisEffectiveLightPdf; }
    if (key == "rispdfratio" || key == "risratio") { return RendererDebugView::RisPdfRatio; }
    if (key == "sampledimension" || key == "sampledimensions" || key == "samplingdimension") { return RendererDebugView::SampleDimension; }
    if (key == "samplescramble" || key == "samplescrambling" || key == "scramble") { return RendererDebugView::SampleScramble; }
    if (key == "pathdirectdiffuse" || key == "directdiffuse") { return RendererDebugView::PathDirectDiffuse; }
    if (key == "pathdirectspecular" || key == "directspecular") { return RendererDebugView::PathDirectSpecular; }
    if (key == "pathindirectdiffuse" || key == "indirectdiffuse") { return RendererDebugView::PathIndirectDiffuse; }
    if (key == "pathindirectspecular" || key == "indirectspecular") { return RendererDebugView::PathIndirectSpecular; }
    if (key == "pathdataalbedo" || key == "pathalbedo") { return RendererDebugView::PathDataAlbedo; }
    if (key == "pathdatametrics" || key == "pathmetrics" || key == "pathhitdistance") { return RendererDebugView::PathDataMetrics; }
    if (key == "denoiserkernelradius" || key == "kernelradius" || key == "filterradius") { return RendererDebugView::DenoiserKernelRadius; }
    if (key == "denoiserhitdistance" || key == "hitdistance" || key == "hitdistancefilter" || key == "hitdistancerejection") { return RendererDebugView::DenoiserHitDistance; }
    if (key == "denoiservirtualmotion" || key == "virtualmotion" || key == "specularvirtualmotion" || key == "specularvelocity") { return RendererDebugView::DenoiserVirtualMotion; }
    if (key == "denoiserdiffusehistory" || key == "diffusehistory" || key == "diffusehistoryconfidence" || key == "denoiserdiffusedebug" || key == "diffusedebug") { return RendererDebugView::DenoiserDiffuseDebug; }
    if (key == "denoiserspecularhistory" || key == "specularhistory" || key == "specularhistoryconfidence" || key == "denoiserspeculardebug" || key == "speculardebug") { return RendererDebugView::DenoiserSpecularDebug; }
    if (key == "denoiseremissiveclamp" || key == "emissiveclamp" || key == "emissiveantiflicker") { return RendererDebugView::DenoiserEmissiveClamp; }
    if (key == "denoiservarianceconfidence" || key == "varianceconfidence") { return RendererDebugView::DenoiserVarianceConfidence; }
    if (key == "denoiserdiffusechannelconfidence" || key == "diffusechannelconfidence" || key == "channelconfidence") { return RendererDebugView::DenoiserDiffuseChannelConfidence; }
    if (key == "denoiserframeblend" || key == "frameblend") { return RendererDebugView::DenoiserFrameBlend; }
    if (key == "denoisermaxhitdistancedelta" || key == "maxhitdistancedelta" || key == "hitdistancedelta") { return RendererDebugView::DenoiserMaxHitDistanceDelta; }
    if (key == "denoiserdiffuseonscreen" || key == "diffuseonscreen") { return RendererDebugView::DenoiserDiffuseOnScreen; }
    if (key == "denoiserbasedisocclusion" || key == "basedisocclusion") { return RendererDebugView::DenoiserBaseDisocclusion; }
    if (key == "denoiserspecularchannelconfidence" || key == "specularchannelconfidence" || key == "specularconfidence") { return RendererDebugView::DenoiserSpecularChannelConfidence; }
    if (key == "denoiserspecularhistoryweight" || key == "specularhistoryweight") { return RendererDebugView::DenoiserSpecularHistoryWeight; }
    if (key == "nrdvalidation" || key == "nrdvalidationoutput") { return RendererDebugView::NrdValidation; }
    if (key == "nrddiffuseconfidence" || key == "nrddiffconfidence" || key == "nrddiffusehistoryconfidence") {
        return RendererDebugView::NrdDiffuseConfidence;
    }
    if (key == "nrdspecularconfidence" || key == "nrdspecconfidence" || key == "nrdspecularhistoryconfidence") {
        return RendererDebugView::NrdSpecularConfidence;
    }
    if (key == "nrdrawconfidencegradient" || key == "nrdrawgradient" || key == "nrdconfidencegradient") {
        return RendererDebugView::NrdRawConfidenceGradient;
    }
    if (key == "nrdfilteredconfidencegradient" || key == "nrdfilteredgradient" || key == "nrdconfidencegradientfiltered") {
        return RendererDebugView::NrdFilteredConfidenceGradient;
    }
    if (key == "nrdconfidencehistory" || key == "nrdhistoryconfidence" || key == "nrdhistory" ||
        key == "nrdconfidencehistoryvalidity" || key == "nrdsourcepixelhistory") {
        return RendererDebugView::NrdConfidenceHistory;
    }
    if (key == "psractivemask" || key == "psractive" || key == "primarysurfacereplacement") { return RendererDebugView::PsrActiveMask; }
    if (key == "psrdepth" || key == "psrviewz" || key == "replacementdepth") { return RendererDebugView::PsrDepth; }
    if (key == "psrmotion" || key == "psrvelocity" || key == "replacementmotion") { return RendererDebugView::PsrMotion; }
    if (key == "psrnormalroughness" || key == "psrnormal" || key == "replacementnormal") { return RendererDebugView::PsrNormalRoughness; }
    if (key == "psrhitdistance" || key == "replacementhitdistance") { return RendererDebugView::PsrHitDistance; }
    if (key == "psralbedof0" || key == "psralbedo" || key == "psrf0") { return RendererDebugView::PsrAlbedoF0; }
    if (key == "psrraydirection" || key == "psrdirection" || key == "reflectionraydirection") { return RendererDebugView::PsrRayDirection; }
    if (key == "dlssdepth" || key == "dlssguidedepth") { return RendererDebugView::DlssDepth; }
    if (key == "dlssmotion" || key == "dlssmotionvectors" || key == "dlssguidevelocity") { return RendererDebugView::DlssMotionVectors; }
    if (key == "dlssinput" || key == "dlssinputcolor" || key == "dlssscalinginputcolor") { return RendererDebugView::DlssInputColor; }
    if (key == "dlssoutput" || key == "dlssoutputcolor" || key == "dlssscalingoutputcolor") { return RendererDebugView::DlssOutputColor; }
    if (key == "dlssrrdiffusealbedo" || key == "rrdiffusealbedo") { return RendererDebugView::DlssRrDiffuseAlbedo; }
    if (key == "dlssrrspecularalbedo" || key == "rrspecularalbedo" || key == "dlssrrspecularf0") { return RendererDebugView::DlssRrSpecularAlbedo; }
    if (key == "dlssrrnormals" || key == "dlssrrnormal" || key == "rrnormals") { return RendererDebugView::DlssRrNormals; }
    if (key == "dlssrrroughness" || key == "rrroughness") { return RendererDebugView::DlssRrRoughness; }
    if (key == "dlssrrdiffusehitdistance" || key == "rrdiffusehitdistance") { return RendererDebugView::DlssRrDiffuseHitDistance; }
    if (key == "dlssrrspecularhitdistance" || key == "rrspecularhitdistance") { return RendererDebugView::DlssRrSpecularHitDistance; }
    if (key == "dlssrrreflectedalbedo" || key == "rrreflectedalbedo") { return RendererDebugView::DlssRrReflectedAlbedo; }
    if (key == "dlssrrdisocclusion" || key == "dlssrrdisocclusionmask" || key == "rrdisocclusion") { return RendererDebugView::DlssRrDisocclusionMask; }
    if (key == "dlssrrdiffuseraydirection" || key == "rrdiffuseraydirection") { return RendererDebugView::DlssRrDiffuseRayDirection; }
    if (key == "dlssrrspecularraydirection" || key == "rrspecularraydirection") { return RendererDebugView::DlssRrSpecularRayDirection; }
    if (key == "dlssrrdiffuseraydirectionhitdistance" || key == "rrdiffuseraydirectionhitdistance") { return RendererDebugView::DlssRrDiffuseRayDirectionHitDistance; }
    if (key == "dlssrrspecularraydirectionhitdistance" || key == "rrspecularraydirectionhitdistance") { return RendererDebugView::DlssRrSpecularRayDirectionHitDistance; }
    if (key == "directsample" || key == "directsampletype" || key == "sampletype") { return RendererDebugView::DirectSampleType; }
    if (key == "albedo" || key == "basecolor" || key == "basecolour") { return RendererDebugView::Albedo; }
    if (key == "occlusion" || key == "ao" || key == "materialocclusion" || key == "aotexture") {
        return RendererDebugView::MaterialOcclusion;
    }
    if (key == "clay" || key == "claymaterial" || key == "balancedclay" || key == "balancedclaymaterial" ||
        key == "white" || key == "whitematerial" || key == "whitematerialmode") {
        return RendererDebugView::ClayMaterial;
    }
    if (key == "firstbounce" || key == "firstbouncethroughput" || key == "throughput" || key == "firstbounceweight") {
        return RendererDebugView::FirstBounceThroughput;
    }
    if (key == "secondaryenvmiss" || key == "secondaryenvironmentmiss" || key == "envmiss" || key == "skyescape") {
        return RendererDebugView::SecondaryEnvironmentMiss;
    }
    if (key == "bouncecount" || key == "bounces") { return RendererDebugView::BounceCount; }
    if (key == "secondaryenvradiance" || key == "secondaryenvironmentradiance" || key == "envradiance") {
        return RendererDebugView::SecondaryEnvironmentRadiance;
    }
    if (key == "whiteenv" || key == "whiteenvironment" || key == "whiteenvironmenttransport" || key == "whitetransport") {
        return RendererDebugView::WhiteEnvironmentTransport;
    }
    if (key == "motion" || key == "motionvectors" || key == "velocity" || key == "velocitybuffer") {
        return RendererDebugView::MotionVectors;
    }
    if (key == "skinnedmotion" || key == "skinnedmotionvectors" || key == "skinnedvelocity" || key == "skinnedvelocitybuffer") {
        return RendererDebugView::SkinnedMotionVectors;
    }
    if (key == "atmospheresky" || key == "atmosphereskyview" || key == "skyviewlut") {
        return RendererDebugView::AtmosphereSkyView;
    }
    if (key == "atmospheretransmittance" || key == "transmittancelut") {
        return RendererDebugView::AtmosphereTransmittance;
    }
    if (key == "atmosphereaerial" || key == "aerialperspective" || key == "aerialperspectivelut") {
        return RendererDebugView::AtmosphereAerialPerspective;
    }
    if (key == "atmospheremultiscatter" || key == "multiscatter" || key == "multiscatterlut") {
        return RendererDebugView::AtmosphereMultiScatter;
    }
    if (key == "reactive" || key == "reactivemask" || key == "temporalreactive" || key == "temporalreactivemask") {
        return RendererDebugView::TemporalReactiveMask;
    }
    if (key == "historyweight" || key == "temporalhistory" || key == "temporalhistoryweight") {
        return RendererDebugView::TemporalHistoryWeight;
    }
    if (key == "restirage" || key == "restirreservoirage" || key == "reservoirage") {
        return RendererDebugView::RestirReservoirAge;
    }
    if (key == "restirconfidence" || key == "restirreservoirconfidence" || key == "reservoirconfidence") {
        return RendererDebugView::RestirReservoirConfidence;
    }
    if (key == "restirm" || key == "restirreservoirm" || key == "reservoirm" || key == "restirsamplecount") {
        return RendererDebugView::RestirReservoirM;
    }
    if (key == "restirpairwisemis" || key == "restirtemporalweight" || key == "restirmisweight" || key == "pairwisemis") {
        return RendererDebugView::RestirPairwiseMis;
    }
    if (key == "restirgivalidity" || key == "restirgivalid" || key == "gireservoirvalidity" || key == "givalidity") {
        return RendererDebugView::RestirGiValidity;
    }
    if (key == "restirgiage" || key == "gireservoirage" || key == "giage") {
        return RendererDebugView::RestirGiAge;
    }
    if (key == "restirgiinitial" || key == "restirgiinit" || key == "giinitial") {
        return RendererDebugView::RestirGiInitial;
    }
    if (key == "restirgitemporal" || key == "gitemporal") {
        return RendererDebugView::RestirGiTemporal;
    }
    if (key == "restirgispatial" || key == "gispatial") {
        return RendererDebugView::RestirGiSpatial;
    }
    if (key == "restirgifinal" || key == "gifinal" || key == "restirgicontribution") {
        return RendererDebugView::RestirGiFinal;
    }
    if (key == "restirginormal" || key == "ginormal" || key == "gireservoirnormal") {
        return RendererDebugView::RestirGiNormal;
    }
    if (key == "restirgihitdistance" || key == "gihitdistance" || key == "gireservoirhitdistance") {
        return RendererDebugView::RestirGiHitDistance;
    }
    if (key == "restirgigrid" || key == "gigrid" || key == "restirgiperiodicity" || key == "giperiodicity") {
        return RendererDebugView::RestirGiGrid;
    }
    if (key == "restirgipathclass" || key == "gipathclass" || key == "restirgiclass" || key == "giclass") {
        return RendererDebugView::RestirGiPathClass;
    }
    if (key == "restirgitarget" || key == "gitarget" || key == "restirgitargetfunction") {
        return RendererDebugView::RestirGiTarget;
    }
    if (key == "restirgisourcepdf" || key == "gisourcepdf" || key == "restirgipdf") {
        return RendererDebugView::RestirGiSourcePdf;
    }
    if (key == "restirgiweightsum" || key == "giweightsum" || key == "restirgiw") {
        return RendererDebugView::RestirGiWeightSum;
    }
    if (key == "restirgim" || key == "gim" || key == "restirgisamplecount") {
        return RendererDebugView::RestirGiM;
    }
    if (key == "restirgiconfidence" || key == "giconfidence") {
        return RendererDebugView::RestirGiConfidence;
    }
    if (key == "restirgivisibility" || key == "restirgivis" || key == "givisibility") {
        return RendererDebugView::RestirGiVisibility;
    }
    if (key == "adaptivedensity" || key == "adaptivedensitymap" || key == "samplingdensity" || key == "densitymap") {
        return RendererDebugView::AdaptiveDensityMap;
    }
    if (key == "adaptivesamplecount" || key == "samplecountmap" || key == "adaptivespp" || key == "sppmap") {
        return RendererDebugView::AdaptiveSampleCount;
    }
    if (key == "adaptiveunsampled" || key == "adaptiveunsampledpixels" || key == "unsampledpixels") {
        return RendererDebugView::AdaptiveUnsampledPixels;
    }
    if (key == "adaptivefilled" || key == "adaptivefilledimage" || key == "filledimage") {
        return RendererDebugView::AdaptiveFilledImage;
    }
    if (key == "adaptivedisocclusion" || key == "adaptivedisocclusionmask" || key == "disocclusionmask") {
        return RendererDebugView::AdaptiveDisocclusionMask;
    }
    if (key == "regirgrid" || key == "regirgridoccupancy" || key == "regiroccupancy") {
        return RendererDebugView::RegirGridOccupancy;
    }
    if (key == "regirweight" || key == "regirreservoirweight" || key == "regirreservoir") {
        return RendererDebugView::RegirReservoirWeight;
    }
    if (key == "regirselectedlight" || key == "regirlight" || key == "regirselected") {
        return RendererDebugView::RegirSelectedLight;
    }
    if (key == "regirquerycount" || key == "regirqueries" || key == "regirquery") {
        return RendererDebugView::RegirQueryCount;
    }
    if (key == "regirmisweight" || key == "regirmis") {
        return RendererDebugView::RegirMisWeight;
    }
    if (key == "regireffectivepdf" || key == "regirpdf") {
        return RendererDebugView::RegirEffectivePdf;
    }
    if (key == "regircanonicalused" || key == "regircanonical" || key == "regirsource") {
        return RendererDebugView::RegirCanonicalUsed;
    }
    if (key == "regirquerycell" || key == "regircell") {
        return RendererDebugView::RegirQueryCell;
    }
    if (key == "regiractivecelloccupancy" || key == "regiractivecell" || key == "regiractiveoccupancy") {
        return RendererDebugView::RegirActiveCellOccupancy;
    }
    if (key == "regirhashcollisions" || key == "regirhashcollision" || key == "regircollisions") {
        return RendererDebugView::RegirHashCollisions;
    }
    if (key == "regirspatialinputweight" || key == "regirspatialinput" || key == "regirinputweight") {
        return RendererDebugView::RegirSpatialInputWeight;
    }
    if (key == "regirspatialoutputweight" || key == "regirspatialoutput" || key == "regiroutputweight") {
        return RendererDebugView::RegirSpatialOutputWeight;
    }
    if (key == "regirspatialneighborcount" || key == "regirspatialneighbors" || key == "regirneighborcount") {
        return RendererDebugView::RegirSpatialNeighborCount;
    }
    if (key == "regirinfinitelightsource" || key == "regirinfinitesource" ||
        key == "regirenvironmentsource" || key == "regirenvsource" || key == "regirenvkind") {
        return RendererDebugView::RegirEnvironmentSource;
    }
    if (key == "regirenvironmentpdf" || key == "regirenvpdf" || key == "regirenveffectivepdf") {
        return RendererDebugView::RegirEnvironmentPdf;
    }
    if (key == "regirenvironmentdirection" || key == "regirenvdirection" || key == "regirenvdir") {
        return RendererDebugView::RegirEnvironmentDirection;
    }
    if (key == "regirenvironmentweight" || key == "regirenvweight" || key == "regirenvm") {
        return RendererDebugView::RegirEnvironmentWeight;
    }
    if (key == "regirenvironmentgeneration" || key == "regirenvgeneration" || key == "regirenvstale") {
        return RendererDebugView::RegirEnvironmentGeneration;
    }
    if (key == "wavefrontqueueoccupancy" || key == "wavefrontoccupancy" || key == "queueoccupancy") {
        return RendererDebugView::WavefrontQueueOccupancy;
    }
    if (key == "wavefrontpathdepth" || key == "wavefrontdepth" || key == "queuedepth") {
        return RendererDebugView::WavefrontPathDepth;
    }
    if (key == "wavefrontliverays" || key == "wavefrontlive" || key == "liverays") {
        return RendererDebugView::WavefrontLiveRays;
    }
    if (key == "wavefrontterminatedrays" || key == "wavefrontterminated" || key == "terminatedrays") {
        return RendererDebugView::WavefrontTerminatedRays;
    }
    if (key == "wavefrontmaterialbucket" || key == "materialbucket" || key == "materialbuckets") {
        return RendererDebugView::WavefrontMaterialBucket;
    }
    if (key == "wavefrontrestirdi" || key == "wavefrontrestir" || key == "wavefrontreservoir" || key == "wavefrontrestirdireservoir") {
        return RendererDebugView::WavefrontRestirDi;
    }
    if (key == "wavefrontrestirgi" || key == "wavefrontgireservoir" || key == "wavefrontrestirgireservoir") {
        return RendererDebugView::WavefrontRestirGi;
    }
    if (key == "wavefrontdirectlighting" || key == "wavefrontdirect" || key == "wavefrontdirectlight") {
        return RendererDebugView::WavefrontDirectLighting;
    }
    if (key == "causticvisibility" || key == "caustics" || key == "mneecaustics" || key == "causticshadow") {
        return RendererDebugView::CausticVisibility;
    }
    if (key == "denoiserdirectdiffusevariance" || key == "directdiffusevariance" || key == "ddvariance") {
        return RendererDebugView::DenoiserDirectDiffuseVariance;
    }
    if (key == "denoiserdirectspecularvariance" || key == "directspecularvariance" || key == "dsvariance") {
        return RendererDebugView::DenoiserDirectSpecularVariance;
    }
    if (key == "denoiserindirectdiffusevariance" || key == "indirectdiffusevariance" || key == "idvariance") {
        return RendererDebugView::DenoiserIndirectDiffuseVariance;
    }
    if (key == "denoiserindirectspecularvariance" || key == "indirectspecularvariance" || key == "isvariance") {
        return RendererDebugView::DenoiserIndirectSpecularVariance;
    }
    if (key == "denoiserdiffusehistorylength" || key == "diffusehistorylength") {
        return RendererDebugView::DenoiserDiffuseHistoryLength;
    }
    if (key == "denoiserspecularhistorylength" || key == "specularhistorylength") {
        return RendererDebugView::DenoiserSpecularHistoryLength;
    }
    if (key == "momentupdatevalidity" || key == "validity") {
        return RendererDebugView::MomentUpdateValidity;
    }
    if (key == "momentdisocclusionconfidence" || key == "disocclusionconfidence") {
        return RendererDebugView::MomentDisocclusionConfidence;
    }
    if (key == "momentnormalcone" || key == "normalcone") {
        return RendererDebugView::MomentNormalCone;
    }
    if (key == "momentdepthdelta" || key == "depthdelta") {
        return RendererDebugView::MomentDepthDelta;
    }
    if (key == "momenthistorykindvalid" || key == "historykindvalid") {
        return RendererDebugView::MomentHistoryKindValid;
    }
    if (key == "denoiserdiffuserawvariance" || key == "diffuserawvariance") {
        return RendererDebugView::DenoiserDiffuseRawVariance;
    }
    if (key == "denoiserspecularrawvariance" || key == "specularrawvariance") {
        return RendererDebugView::DenoiserSpecularRawVariance;
    }
    return RendererDebugView::Beauty;
}

const char* rendererDebugViewName(RendererDebugView view) {
    switch (view) {
    case RendererDebugView::Beauty: return "beauty";
    case RendererDebugView::Variance: return "variance";
    case RendererDebugView::Normals: return "normals";
    case RendererDebugView::ReprojectionConfidence: return "reprojection-confidence";
    case RendererDebugView::DenoiserRejection: return "denoiser-rejection";
    case RendererDebugView::Depth: return "depth";
    case RendererDebugView::Roughness: return "roughness";
    case RendererDebugView::DirectLighting: return "direct-lighting";
    case RendererDebugView::IndirectLighting: return "indirect-lighting";
    case RendererDebugView::EmissiveContribution: return "emissive-contribution";
    case RendererDebugView::EnvironmentContribution: return "environment-contribution";
    case RendererDebugView::TraversalSteps: return "traversal-steps";
    case RendererDebugView::BvhDepth: return "bvh-depth";
    case RendererDebugView::InstanceId: return "instance-id";
    case RendererDebugView::MeshId: return "mesh-id";
    case RendererDebugView::TlasSteps: return "tlas-steps";
    case RendererDebugView::TraversalMismatch: return "traversal-mismatch";
    case RendererDebugView::LightPdf: return "light-pdf";
    case RendererDebugView::BsdfPdf: return "bsdf-pdf";
    case RendererDebugView::MisWeight: return "mis-weight";
    case RendererDebugView::DirectSampleType: return "direct-sample-type";
    case RendererDebugView::Albedo: return "albedo";
    case RendererDebugView::ClayMaterial: return "clay-material";
    case RendererDebugView::FirstBounceThroughput: return "first-bounce-throughput";
    case RendererDebugView::SecondaryEnvironmentMiss: return "secondary-environment-miss";
    case RendererDebugView::BounceCount: return "bounce-count";
    case RendererDebugView::SecondaryEnvironmentRadiance: return "secondary-environment-radiance";
    case RendererDebugView::WhiteEnvironmentTransport: return "white-environment-transport";
    case RendererDebugView::MotionVectors: return "motion-vectors";
    case RendererDebugView::SkinnedMotionVectors: return "skinned-motion-vectors";
    case RendererDebugView::AtmosphereSkyView: return "atmosphere-sky-view";
    case RendererDebugView::AtmosphereTransmittance: return "atmosphere-transmittance";
    case RendererDebugView::AtmosphereAerialPerspective: return "atmosphere-aerial-perspective";
    case RendererDebugView::AtmosphereMultiScatter: return "atmosphere-multi-scatter";
    case RendererDebugView::TemporalReactiveMask: return "temporal-reactive-mask";
    case RendererDebugView::TemporalHistoryWeight: return "temporal-history-weight";
    case RendererDebugView::RestirReservoirAge: return "restir-reservoir-age";
    case RendererDebugView::RestirReservoirConfidence: return "restir-reservoir-confidence";
    case RendererDebugView::RestirReservoirM: return "restir-reservoir-m";
    case RendererDebugView::EmissiveContinuation: return "emissive-continuation";
    case RendererDebugView::SunMisWeight: return "sun-mis-weight";
    case RendererDebugView::SunLightPdf: return "sun-light-pdf";
    case RendererDebugView::SunPreviousBsdfPdf: return "sun-previous-bsdf-pdf";
    case RendererDebugView::RisRawLightPdf: return "ris-raw-light-pdf";
    case RendererDebugView::RisEffectiveLightPdf: return "ris-effective-light-pdf";
    case RendererDebugView::RisPdfRatio: return "ris-pdf-ratio";
    case RendererDebugView::SampleDimension: return "sample-dimension";
    case RendererDebugView::SampleScramble: return "sample-scramble";
    case RendererDebugView::PathDirectDiffuse: return "path-direct-diffuse";
    case RendererDebugView::PathDirectSpecular: return "path-direct-specular";
    case RendererDebugView::PathIndirectDiffuse: return "path-indirect-diffuse";
    case RendererDebugView::PathIndirectSpecular: return "path-indirect-specular";
    case RendererDebugView::PathDataAlbedo: return "path-data-albedo";
    case RendererDebugView::PathDataMetrics: return "path-data-metrics";
    case RendererDebugView::DenoiserKernelRadius: return "denoiser-kernel-radius";
    case RendererDebugView::DenoiserHitDistance: return "denoiser-hit-distance";
    case RendererDebugView::DenoiserVirtualMotion: return "denoiser-virtual-motion";
    case RendererDebugView::DenoiserDiffuseDebug: return "denoiser-diffuse-debug";
    case RendererDebugView::DenoiserSpecularDebug: return "denoiser-specular-debug";
    case RendererDebugView::DenoiserEmissiveClamp: return "denoiser-emissive-clamp";
    case RendererDebugView::DenoiserVarianceConfidence: return "denoiser-variance-confidence";
    case RendererDebugView::DenoiserDiffuseChannelConfidence: return "denoiser-diffuse-channel-confidence";
    case RendererDebugView::DenoiserFrameBlend: return "denoiser-frame-blend";
    case RendererDebugView::DenoiserMaxHitDistanceDelta: return "denoiser-max-hit-distance-delta";
    case RendererDebugView::DenoiserDiffuseOnScreen: return "denoiser-diffuse-on-screen";
    case RendererDebugView::DenoiserBaseDisocclusion: return "denoiser-base-disocclusion";
    case RendererDebugView::DenoiserSpecularChannelConfidence: return "denoiser-specular-channel-confidence";
    case RendererDebugView::DenoiserSpecularHistoryWeight: return "denoiser-specular-history-weight";
    case RendererDebugView::NrdValidation: return "nrd-validation";
    case RendererDebugView::NrdDiffuseConfidence: return "nrd-diffuse-confidence";
    case RendererDebugView::NrdSpecularConfidence: return "nrd-specular-confidence";
    case RendererDebugView::NrdRawConfidenceGradient: return "nrd-raw-confidence-gradient";
    case RendererDebugView::NrdFilteredConfidenceGradient: return "nrd-filtered-confidence-gradient";
    case RendererDebugView::NrdConfidenceHistory: return "nrd-confidence-history";
    case RendererDebugView::PsrActiveMask: return "psr-active-mask";
    case RendererDebugView::PsrDepth: return "psr-depth";
    case RendererDebugView::PsrMotion: return "psr-motion";
    case RendererDebugView::PsrNormalRoughness: return "psr-normal-roughness";
    case RendererDebugView::PsrHitDistance: return "psr-hit-distance";
    case RendererDebugView::PsrAlbedoF0: return "psr-albedo-f0";
    case RendererDebugView::PsrRayDirection: return "psr-ray-direction";
    case RendererDebugView::DlssDepth: return "dlss-depth";
    case RendererDebugView::DlssMotionVectors: return "dlss-motion-vectors";
    case RendererDebugView::DlssInputColor: return "dlss-input-color";
    case RendererDebugView::DlssOutputColor: return "dlss-output-color";
    case RendererDebugView::DlssRrDiffuseAlbedo: return "dlss-rr-diffuse-albedo";
    case RendererDebugView::DlssRrSpecularAlbedo: return "dlss-rr-specular-albedo";
    case RendererDebugView::DlssRrNormals: return "dlss-rr-normals";
    case RendererDebugView::DlssRrRoughness: return "dlss-rr-roughness";
    case RendererDebugView::DlssRrDiffuseHitDistance: return "dlss-rr-diffuse-hit-distance";
    case RendererDebugView::DlssRrSpecularHitDistance: return "dlss-rr-specular-hit-distance";
    case RendererDebugView::DlssRrReflectedAlbedo: return "dlss-rr-reflected-albedo";
    case RendererDebugView::DlssRrDisocclusionMask: return "dlss-rr-disocclusion-mask";
    case RendererDebugView::DlssRrDiffuseRayDirection: return "dlss-rr-diffuse-ray-direction";
    case RendererDebugView::DlssRrSpecularRayDirection: return "dlss-rr-specular-ray-direction";
    case RendererDebugView::DlssRrDiffuseRayDirectionHitDistance: return "dlss-rr-diffuse-ray-direction-hit-distance";
    case RendererDebugView::DlssRrSpecularRayDirectionHitDistance: return "dlss-rr-specular-ray-direction-hit-distance";
    case RendererDebugView::RestirPairwiseMis: return "restir-pairwise-mis";
    case RendererDebugView::RestirGiValidity: return "restir-gi-validity";
    case RendererDebugView::RestirGiAge: return "restir-gi-age";
    case RendererDebugView::RestirGiInitial: return "restir-gi-initial";
    case RendererDebugView::RestirGiTemporal: return "restir-gi-temporal";
    case RendererDebugView::RestirGiSpatial: return "restir-gi-spatial";
    case RendererDebugView::RestirGiFinal: return "restir-gi-final";
    case RendererDebugView::RestirGiNormal: return "restir-gi-normal";
    case RendererDebugView::RestirGiHitDistance: return "restir-gi-hit-distance";
    case RendererDebugView::RestirGiGrid: return "restir-gi-grid";
    case RendererDebugView::RestirGiPathClass: return "restir-gi-path-class";
    case RendererDebugView::RestirGiTarget: return "restir-gi-target";
    case RendererDebugView::RestirGiSourcePdf: return "restir-gi-source-pdf";
    case RendererDebugView::RestirGiWeightSum: return "restir-gi-weight-sum";
    case RendererDebugView::RestirGiM: return "restir-gi-m";
    case RendererDebugView::RestirGiConfidence: return "restir-gi-confidence";
    case RendererDebugView::RestirGiVisibility: return "restir-gi-visibility";
    case RendererDebugView::AdaptiveDensityMap: return "adaptive-density-map";
    case RendererDebugView::AdaptiveSampleCount: return "adaptive-sample-count";
    case RendererDebugView::AdaptiveUnsampledPixels: return "adaptive-unsampled-pixels";
    case RendererDebugView::AdaptiveFilledImage: return "adaptive-filled-image";
    case RendererDebugView::AdaptiveDisocclusionMask: return "adaptive-disocclusion-mask";
    case RendererDebugView::RegirGridOccupancy: return "regir-grid-occupancy";
    case RendererDebugView::RegirReservoirWeight: return "regir-reservoir-weight";
    case RendererDebugView::RegirSelectedLight: return "regir-selected-light";
    case RendererDebugView::RegirQueryCount: return "regir-query-count";
    case RendererDebugView::RegirMisWeight: return "regir-mis-weight";
    case RendererDebugView::RegirEffectivePdf: return "regir-effective-pdf";
    case RendererDebugView::RegirCanonicalUsed: return "regir-canonical-used";
    case RendererDebugView::RegirQueryCell: return "regir-query-cell";
    case RendererDebugView::RegirActiveCellOccupancy: return "regir-active-cell-occupancy";
    case RendererDebugView::RegirHashCollisions: return "regir-hash-collisions";
    case RendererDebugView::RegirSpatialInputWeight: return "regir-spatial-input-weight";
    case RendererDebugView::RegirSpatialOutputWeight: return "regir-spatial-output-weight";
    case RendererDebugView::RegirSpatialNeighborCount: return "regir-spatial-neighbor-count";
    case RendererDebugView::RegirEnvironmentSource: return "regir-infinite-source";
    case RendererDebugView::RegirEnvironmentPdf: return "regir-environment-pdf";
    case RendererDebugView::RegirEnvironmentDirection: return "regir-environment-direction";
    case RendererDebugView::RegirEnvironmentWeight: return "regir-environment-weight";
    case RendererDebugView::RegirEnvironmentGeneration: return "regir-environment-generation";
    case RendererDebugView::RestirDiSelectedLight: return "restir-di-selected-light";
    case RendererDebugView::RestirDiTarget: return "restir-di-target";
    case RendererDebugView::RestirDiSourcePdf: return "restir-di-source-pdf";
    case RendererDebugView::RestirDiVisibility: return "restir-di-visibility";
    case RendererDebugView::RestirDiRejectionReason: return "restir-di-rejection-reason";
    case RendererDebugView::RestirDiTemporalAcceptance: return "restir-di-temporal-accepted";
    case RendererDebugView::RestirDiSpatialAcceptance: return "restir-di-spatial-accepted";
    case RendererDebugView::RestirDiFinalContribution: return "restir-di-final";
    case RendererDebugView::RestirDiReceiverPosition: return "restir-di-receiver-position";
    case RendererDebugView::RestirDiReceiverNormal: return "restir-di-receiver-normal";
    case RendererDebugView::RestirDiLightVersion: return "restir-di-light-version";
    case RendererDebugView::RestirDiLightMapStatus: return "restir-di-light-map-status";
    case RendererDebugView::RestirDiInitialReservoir: return "restir-di-initial-reservoir";
    case RendererDebugView::RestirDiTemporalReservoir: return "restir-di-temporal-reservoir";
    case RendererDebugView::RestirDiSpatialReservoir: return "restir-di-spatial-reservoir";
    case RendererDebugView::RestirDiFinalReservoir: return "restir-di-final-reservoir";
    case RendererDebugView::RestirDiWeightSum: return "restir-di-weight-sum";
    case RendererDebugView::RestirDiM: return "restir-di-m";
    case RendererDebugView::RestirDiLightClass: return "restir-di-light-type";
    case RendererDebugView::RestirDiAge: return "restir-di-age";
    case RendererDebugView::RestirDiConfidence: return "restir-di-confidence";
    case RendererDebugView::RestirDiReferenceDiff: return "restir-di-reference-diff";
    case RendererDebugView::WavefrontQueueOccupancy: return "wavefront-queue-occupancy";
    case RendererDebugView::WavefrontPathDepth: return "wavefront-path-depth";
    case RendererDebugView::WavefrontLiveRays: return "wavefront-live-rays";
    case RendererDebugView::WavefrontTerminatedRays: return "wavefront-terminated-rays";
    case RendererDebugView::WavefrontMaterialBucket: return "wavefront-material-bucket";
    case RendererDebugView::WavefrontRestirDi: return "wavefront-restir-di";
    case RendererDebugView::WavefrontDirectLighting: return "wavefront-direct-lighting";
    case RendererDebugView::WavefrontRestirGi: return "wavefront-restir-gi";
    case RendererDebugView::CausticVisibility: return "caustic-visibility";
    case RendererDebugView::DenoiserDirectDiffuseVariance: return "denoiser-direct-diffuse-variance";
    case RendererDebugView::DenoiserDirectSpecularVariance: return "denoiser-direct-specular-variance";
    case RendererDebugView::DenoiserIndirectDiffuseVariance: return "denoiser-indirect-diffuse-variance";
    case RendererDebugView::DenoiserIndirectSpecularVariance: return "denoiser-indirect-specular-variance";
    case RendererDebugView::DenoiserDiffuseHistoryLength: return "denoiser-diffuse-history-length";
    case RendererDebugView::DenoiserSpecularHistoryLength: return "denoiser-specular-history-length";
    case RendererDebugView::MomentUpdateValidity: return "moment-update-validity";
    case RendererDebugView::MomentDisocclusionConfidence: return "moment-disocclusion-confidence";
    case RendererDebugView::MomentNormalCone: return "moment-normal-cone";
    case RendererDebugView::MomentDepthDelta: return "moment-depth-delta";
    case RendererDebugView::MomentHistoryKindValid: return "moment-history-kind-valid";
    case RendererDebugView::DenoiserDiffuseRawVariance: return "denoiser-diffuse-raw-variance";
    case RendererDebugView::DenoiserSpecularRawVariance: return "denoiser-specular-raw-variance";
    case RendererDebugView::MaterialOcclusion: return "material-occlusion";
    case RendererDebugView::Metallic: return "metallic";
    case RendererDebugView::MaterialAlpha: return "material-alpha";
    case RendererDebugView::MaterialTransmission: return "material-transmission";
    case RendererDebugView::MaterialWorkflow: return "material-workflow";
    }
    return "beauty";
}

} // namespace rtv
