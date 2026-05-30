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

const char* renderPresetName(RenderPreset preset) {
    switch (preset) {
    case RenderPreset::Custom: return "custom";
    case RenderPreset::Low: return "low";
    case RenderPreset::Balanced: return "balanced";
    case RenderPreset::Ultra: return "ultra";
    }
    return "custom";
}

RenderPreset parseRenderPreset(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "low" || key == "performance") { return RenderPreset::Low; }
    if (key == "balanced" || key == "game" || key == "default") { return RenderPreset::Balanced; }
    if (key == "ultra" || key == "quality" || key == "reference") { return RenderPreset::Ultra; }
    return RenderPreset::Custom;
}

RendererDebugView parseRendererDebugView(std::string_view value) {
    const std::string key = normalized(value);
    if (key == "variance") { return RendererDebugView::Variance; }
    if (key == "normal" || key == "normals") { return RendererDebugView::Normals; }
    if (key == "reprojection" || key == "reprojectionconfidence") { return RendererDebugView::ReprojectionConfidence; }
    if (key == "rejection" || key == "denoiserrejection") { return RendererDebugView::DenoiserRejection; }
    if (key == "depth") { return RendererDebugView::Depth; }
    if (key == "roughness") { return RendererDebugView::Roughness; }
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
    case RendererDebugView::RestirPairwiseMis: return "restir-pairwise-mis";
    case RendererDebugView::RestirGiValidity: return "restir-gi-validity";
    case RendererDebugView::RestirGiAge: return "restir-gi-age";
    case RendererDebugView::RestirGiInitial: return "restir-gi-initial";
    case RendererDebugView::RestirGiTemporal: return "restir-gi-temporal";
    case RendererDebugView::RestirGiSpatial: return "restir-gi-spatial";
    case RendererDebugView::RestirGiFinal: return "restir-gi-final";
    case RendererDebugView::RestirGiNormal: return "restir-gi-normal";
    case RendererDebugView::RestirGiHitDistance: return "restir-gi-hit-distance";
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
    }
    return "beauty";
}

} // namespace rtv
