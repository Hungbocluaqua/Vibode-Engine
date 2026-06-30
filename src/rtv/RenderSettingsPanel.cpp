#include "rtv/RenderSettingsPanel.h"

#include "rtv/EditorUiStyle.h"
#include "rtv/SceneRenderSettingsSync.h"
#include "rtv/SunController.h"
#include "rtv/VulkanContext.h"

#include <imgui.h>
#include <rtv/PhysicalCamera.h>

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

const char* serReorderingHintName(VkRayTracingInvocationReorderModeNV hint) {
    switch (hint) {
        case VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV: return "reorder";
        case VK_RAY_TRACING_INVOCATION_REORDER_MODE_NONE_NV: return "none";
        default: return "unknown";
    }
}

} // namespace

void RenderSettingsPanel::draw(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin(EditorDockWindowTitle::RenderSettings)) {
        ImGui::End();
        return;
    }

    RendererSettings settings = state.renderer.settings();
    if (state.sceneDocument != nullptr) {
        const RenderSettings& render = state.sceneDocument->renderSettings();
        const Environment& environment = state.sceneDocument->environment();
        settings.renderPreset = render.renderPreset;
        settings.pathTracingEnabled = render.pathTracingEnabled;
        settings.cameraJitterEnabled = render.cameraJitterEnabled;
        settings.directLightingEnabled = render.directLightingEnabled;
        settings.secondaryDirectLightingEnabled = render.secondaryDirectLightingEnabled;
        settings.maxBounces = render.maxBounces;
        settings.pathTraceKernelMode = render.pathTraceKernelMode;
        settings.finalBounceFastPathEnabled = render.finalBounceFastPathEnabled;
        settings.native2BTerminalDirectSampleProbability = render.native2BTerminalDirectSampleProbability;
        settings.blendedDecalShadowMode = render.blendedDecalShadowMode;
        settings.native2BDirectReuseMode = render.native2BDirectReuseMode;
        settings.environmentDirectSamples = render.environmentDirectSamples;
        settings.toneMapper = render.toneMapper;
        settings.exposure = render.exposure;
        settings.gamma = render.gamma;
        settings.contrast = render.contrast;
        settings.saturation = render.saturation;
        settings.brightness = render.brightness;
        settings.whitePoint = render.whitePoint;
        settings.autoExposureEnabled = render.autoExposureEnabled;
        settings.targetLuminance = render.targetLuminance;
        settings.minExposure = render.minExposure;
        settings.maxExposure = render.maxExposure;
        settings.adaptationSpeed = render.adaptationSpeed;
        settings.histogramMinLogLuminance = render.histogramMinLogLuminance;
        settings.histogramMaxLogLuminance = render.histogramMaxLogLuminance;
        settings.histogramLowPercentile = render.histogramLowPercentile;
        settings.histogramHighPercentile = render.histogramHighPercentile;
        settings.histogramTargetPercentile = render.histogramTargetPercentile;
        SunController::applyToRendererSettings(*state.sceneDocument, settings);
        settings.skyIntensity = render.skyIntensity;
        settings.indirectStrength = render.indirectStrength;
        settings.restirMode = render.restirMode;
        settings.restirDiMode = render.restirDiMode;
        settings.restirDiTemporalEnabled = render.restirDiTemporalEnabled;
        settings.restirDiSpatialEnabled = render.restirDiSpatialEnabled;
        settings.restirDiFinalVisibilityEnabled = render.restirDiFinalVisibilityEnabled;
        settings.restirDiSpatialRounds = render.restirDiSpatialRounds;
        settings.restirDiSpatialRadius = render.restirDiSpatialRadius;
        settings.restirDiTemporalMaxAge = render.restirDiTemporalMaxAge;
        settings.restirDiMaxM = render.restirDiMaxM;
        settings.restirDiVisibilityRayBudget = render.restirDiVisibilityRayBudget;
        settings.restirDiProductionStabilizationEnabled = render.restirDiProductionStabilizationEnabled;
        settings.restirDiClampLuminance = render.restirDiClampLuminance;
        settings.restirDiIncludeSun = render.restirDiIncludeSun;
        settings.restirDiIncludeEnvironment = render.restirDiIncludeEnvironment;
        settings.restirDiReservoirLayout = render.restirDiReservoirLayout;
        settings.restirGiEnabled = render.restirGiEnabled;
        settings.restirGiMode = render.restirGiMode;
        settings.restirGiReservoirLayout = render.restirGiReservoirLayout;
        settings.denoiserEnabled = render.denoiserEnabled;
        settings.denoiserBackend = render.denoiserBackend;
        settings.denoiseWhileMoving = render.denoiseWhileMoving;
        settings.samplesPerPixel = render.samplesPerPixel;
        settings.limitSamplesPerPixel = render.limitSamplesPerPixel;
        settings.atrousIterations = render.atrousIterations;
        settings.denoiserStrength = render.denoiserStrength;
        settings.denoiserMaxHistoryLength = render.denoiserMaxHistoryLength;
        settings.momentValidityThreshold = render.momentValidityThreshold;
        settings.taaEnabled = render.taaEnabled;
        settings.temporalUpscaler = render.temporalUpscaler;
        settings.dlssFrameGenerationEnabled = render.dlssFrameGenerationEnabled;
        settings.dlssRayReconstructionEnabled = render.dlssRayReconstructionEnabled;
        settings.streamlineReflexEnabled = render.streamlineReflexEnabled;
        settings.streamlineNvPerfEnabled = render.streamlineNvPerfEnabled;
        settings.dlssSharpeningStrength = render.dlssSharpeningStrength;
        settings.taaFeedback = render.taaFeedback;
        settings.taaMotionFeedback = render.taaMotionFeedback;
        settings.taaReactiveFeedback = render.taaReactiveFeedback;
        settings.taaSharpeningStrength = render.taaSharpeningStrength;
        settings.debugView = render.debugView;
        settings.renderResolutionScale = render.resolutionScale;
        settings.accumulationLimit = render.accumulationLimit;
        settings.materialTextureAnisotropy = render.materialTextureAnisotropy;
        settings.specularAaEnabled = render.specularAaEnabled;
        settings.opacityMicromapsEnabled = render.opacityMicromapsEnabled;
        settings.compactImportedEmissiveTriangleSampling = render.compactImportedEmissiveTriangleSampling;
        settings.shadowRayBias = render.shadowRayBias;
        settings.shadowDistanceBias = render.shadowDistanceBias;
        settings.fireflyClamp = render.fireflyClamp;
        settings.restirGiTemporalMaxAge = render.restirGiTemporalMaxAge;
        settings.restirGiSpatialRounds = render.restirGiSpatialRounds;
        settings.restirGiSpatialRadius = render.restirGiSpatialRadius;
        settings.restirGiDepthThresholdScale = render.restirGiDepthThresholdScale;
        settings.restirGiSpatialCompatibilityThreshold = render.restirGiSpatialCompatibilityThreshold;
        settings.restirGiHalfResolution = render.restirGiHalfResolution;
        settings.restirGiVisibilityRayBudget = render.restirGiVisibilityRayBudget;
        settings.restirGiFinalStabilizationEnabled = render.restirGiFinalStabilizationEnabled;
        settings.restirGiActiveTileMaskMode = render.restirGiActiveTileMaskMode;
        settings.restirHistoryCopyMode = render.restirHistoryCopyMode;
        settings.lightingReuseMode = render.lightingReuseMode;
        settings.pathReservoirLayout = render.pathReservoirLayout;
        settings.regirGridDimensions = render.regirGridDimensions;
        settings.regirReservoirsPerCell = render.regirReservoirsPerCell;
        settings.regirCandidatesPerReservoir = render.regirCandidatesPerReservoir;
        settings.regirGridPadding = render.regirGridPadding;
        settings.regirCanonicalMix = render.regirCanonicalMix;
        settings.regirQueryMode = render.regirQueryMode;
        settings.regirGridMode = render.regirGridMode;
        settings.regirFiniteQueryFramePeriod = render.regirFiniteQueryFramePeriod;
        settings.regirSpatialReuse = render.regirSpatialReuse;
        settings.regirSpatialRounds = render.regirSpatialRounds;
        settings.regirTemporalReuse = render.regirTemporalReuse;
        settings.regirTemporalHistory = render.regirTemporalHistory;
        settings.regirTemporalMaxM = render.regirTemporalMaxM;
        settings.regirVisibilityReuse = render.regirVisibilityReuse;
        settings.regirEnvironment = render.regirEnvironment;
        settings.adaptiveSamplingMode = render.adaptiveSamplingMode;
        settings.adaptiveSamplingBudget = render.adaptiveSamplingBudget;
        settings.adaptiveWeightVariance = render.adaptiveWeightVariance;
        settings.adaptiveWeightHistory = render.adaptiveWeightHistory;
        settings.adaptiveWeightMotion = render.adaptiveWeightMotion;
        settings.adaptiveWeightDisocclusion = render.adaptiveWeightDisocclusion;
        settings.adaptiveWeightReactive = render.adaptiveWeightReactive;
        settings.adaptiveWeightEdge = render.adaptiveWeightEdge;
        settings.adaptiveWeightSpecular = render.adaptiveWeightSpecular;
        settings.adaptiveWeightDI = render.adaptiveWeightDI;
        settings.adaptiveWeightGI = render.adaptiveWeightGI;
        settings.adaptiveWeightVolumetric = render.adaptiveWeightVolumetric;
        settings.adaptiveQualityMode = render.adaptiveQualityMode;
        settings.adaptiveGpuFrameTargetMs = render.adaptiveGpuFrameTargetMs;
        settings.usePhysicalCamera = render.usePhysicalCamera;
        settings.physicalAperture = render.physicalAperture;
        settings.physicalShutterSeconds = render.physicalShutterSeconds;
        settings.physicalIso = render.physicalIso;
        settings.physicalExposureCompensation = render.physicalExposureCompensation;
        settings.dofApertureRadius = render.dofApertureRadius;
        settings.dofFocusDistance = render.dofFocusDistance;
        settings.dofBladeCount = render.dofBladeCount;
        settings.dofBokehRotation = render.dofBokehRotation;
        settings.motionBlurEnabled = render.motionBlurEnabled;
        settings.motionBlurShutterOpen = render.motionBlurShutterOpen;
        settings.motionBlurShutterClose = render.motionBlurShutterClose;
        settings.homogeneousVolumeEnabled = render.homogeneousVolumeEnabled;
        settings.homogeneousVolumeScattering = render.homogeneousVolumeScattering;
        settings.homogeneousVolumeAbsorption = render.homogeneousVolumeAbsorption;
        settings.homogeneousVolumeAnisotropy = render.homogeneousVolumeAnisotropy;
        settings.mneeCausticsEnabled = render.mneeCausticsEnabled;
        settings.environmentEnabled = environment.enabled;
        settings.environmentIntensity = environment.intensity;
        settings.environmentRotation = environment.rotation;
        settings.environmentBackgroundIntensity = environment.backgroundIntensity;
        applySceneWorldComponentsToRendererSettings(*state.sceneDocument, settings);
    }
    bool changed = false;
    bool presetApplied = false;
    uint32_t minBounces = 1;
    uint32_t maxBounces = 16;
    uint32_t minEnvSamples = 1;
    uint32_t maxEnvSamples = 8;
    uint32_t minSpp = 1;
    uint32_t maxSpp = kMaxSamplesPerPixel;
    uint32_t minAtrous = 1;
    uint32_t maxAtrous = 5;
    uint32_t minRestirGiAge = 1;
    uint32_t maxRestirGiAge = 64;
    uint32_t minRestirGiRounds = 1;
    uint32_t maxRestirGiRounds = 8;
    uint32_t minRestirGiVisibilityRays = 0;
    uint32_t maxRestirGiVisibilityRays = 4;
    const PathTracerRenderer::NvidiaIntegrationStatus nvidiaStatus = state.renderer.nvidiaIntegrationStatus();
    const bool nrdCanRequest = nvidiaStatus.nrdRequestable || nvidiaStatus.nrdAvailable;
    const bool dlssCanRequest = nvidiaStatus.dlssRequestable || nvidiaStatus.dlssAvailable;
    const bool dlssRayReconstructionCanRequest = nvidiaStatus.dlssRayReconstructionRequestable || nvidiaStatus.dlssRayReconstructionAvailable;
    const bool dlssFrameGenerationCanRequest = nvidiaStatus.dlssFrameGenerationRequestable || nvidiaStatus.dlssFrameGenerationAvailable;
    const bool reflexCanRequest = nvidiaStatus.streamlineReflex.requestable || nvidiaStatus.streamlineReflex.supported;
    const bool nvperfCanRequest = nvidiaStatus.streamlineNvPerf.requestable || nvidiaStatus.streamlineNvPerf.supported;

    ImGui::SeparatorText("Preview Actions");
    if (editorIconTextButton("RenderSettingsResetAccumulation", EditorGlyphIcon::Reset, "Reset Accumulation")) {
        requests.resetAccumulation = AccumulationResetReason::Manual;
    }
    tooltip("Clear path tracing accumulation and rebuild the current preview from a fresh sample history.");
    ImGui::SameLine();
    if (editorIconTextButton("RenderSettingsCycleDebugView", EditorGlyphIcon::DrawDebug, "Debug View")) {
        requests.toggleDebugView = true;
    }
    tooltip("Cycle the active renderer debug view for the current viewport preview.");
    if (editorIconTextButton("RenderSettingsCycleIntermediate", EditorGlyphIcon::Stats, "Intermediate")) {
        requests.cycleIntermediateView = true;
    }
    tooltip("Cycle intermediate render targets exposed by the renderer diagnostic path.");
    ImGui::SameLine();
    if (editorIconTextButton("RenderSettingsToggleDenoiser", EditorGlyphIcon::Render, "Denoiser", settings.denoiserEnabled)) {
        requests.toggleDenoiser = true;
    }
    tooltip("Toggle denoising without leaving the docked Render Settings workflow.");

    ImGui::SeparatorText("Rendering");
    const char* renderPresetItems[] = {"Custom", "Low", "Balanced", "Ultra", "Native 30"};
    int renderPresetIndex = static_cast<int>(settings.renderPreset);
    if (renderPresetIndex < 0 || renderPresetIndex > 4) {
        renderPresetIndex = 0;
    }
    if (ImGui::Combo("Render Preset", &renderPresetIndex, renderPresetItems, 5)) {
        applyRenderPreset(settings, static_cast<RenderPreset>(renderPresetIndex));
        presetApplied = true;
        changed = true;
    }
    tooltip("Game-ready presets tune path tracing, ReSTIR, denoiser, TAA, and render scale together.");
    editorDebugViewCombo("Debug View", settings, changed);
    changed |= ImGui::SliderScalar("Max Bounces", ImGuiDataType_U32, &settings.maxBounces, &minBounces, &maxBounces);
    tooltip("Number of ray bounces. Higher is more accurate and slower; 4-8 for preview, 16 for final.");
    const char* pathTraceKernelItems[] = {"Generic", "Native2B"};
    int pathTraceKernelIndex = static_cast<int>(settings.pathTraceKernelMode);
    if (pathTraceKernelIndex < 0 || pathTraceKernelIndex > 1) {
        pathTraceKernelIndex = 0;
    }
    if (ImGui::Combo("Path Trace Kernel", &pathTraceKernelIndex, pathTraceKernelItems, 2)) {
        settings.pathTraceKernelMode = static_cast<PathTraceKernelMode>(pathTraceKernelIndex);
        changed = true;
    }
    tooltip("Native2B is a strict native 1 SPP, 2-bounce beauty specialization. It falls back unless the current settings match its quality-safe gates.");
    const PathTraceKernelMode effectiveKernel = state.renderer.effectivePathTraceKernelMode();
    ImGui::TextDisabled("Effective: %s", pathTraceKernelModeName(effectiveKernel));
    if (settings.pathTraceKernelMode == PathTraceKernelMode::Native2B) {
        const char* fallbackReason = state.renderer.pathTraceKernelFallbackReason();
        if (fallbackReason != nullptr && fallbackReason[0] != '\0') {
            ImGui::TextDisabled("Fallback: %s", fallbackReason);
        } else {
            ImGui::TextDisabled("Terminal payload: %s", state.renderer.native2BTerminalPayloadActive() ? "active" : "inactive");
        }
    }
    const char* blendedDecalShadowItems[] = {"Exact", "Opaque Shadow", "Alpha Cutout Proxy"};
    int blendedDecalShadowIndex = static_cast<int>(settings.blendedDecalShadowMode);
    if (blendedDecalShadowIndex < 0 || blendedDecalShadowIndex > 2) {
        blendedDecalShadowIndex = 0;
    }
    if (ImGui::Combo("Blended Decal Shadows", &blendedDecalShadowIndex, blendedDecalShadowItems, 3)) {
        settings.blendedDecalShadowMode = static_cast<BlendedDecalShadowMode>(blendedDecalShadowIndex);
        changed = true;
    }
    tooltip("Experimental Native2B traversal shortcut for decal-like BLEND materials. Exact is the quality-preserving default.");
    const char* native2BDirectReuseItems[] = {"Off", "RIS", "Temporal"};
    int native2BDirectReuseIndex = static_cast<int>(settings.native2BDirectReuseMode);
    if (native2BDirectReuseIndex < 0 || native2BDirectReuseIndex > 2) {
        native2BDirectReuseIndex = 0;
    }
    if (ImGui::Combo("Native2B Direct Reuse", &native2BDirectReuseIndex, native2BDirectReuseItems, 3)) {
        settings.native2BDirectReuseMode = static_cast<Native2BDirectReuseMode>(native2BDirectReuseIndex);
        changed = true;
    }
    tooltip("Experimental terminal direct-light reuse. Off keeps the exact estimator.");
    changed |= ImGui::SliderFloat("Terminal Direct Rate", &settings.native2BTerminalDirectSampleProbability, 0.25f, 1.0f, "%.2f");
    tooltip("Native2B widened quality/performance gate. 1.00 samples every terminal env/sun direct light; lower values sample stochastically and rely on temporal accumulation.");
    changed |= ImGui::SliderScalar("Environment Samples", ImGuiDataType_U32, &settings.environmentDirectSamples, &minEnvSamples, &maxEnvSamples);
    tooltip("Environment light samples per bounce. Higher values reduce fireflies.");
    changed |= ImGui::Checkbox("Limit to 1 SPP", &settings.limitSamplesPerPixel);
    tooltip("Keeps real-time path tracing at one path sample per pixel per frame. Disable for stills or high-end budgets.");
    changed |= ImGui::SliderScalar("Samples Per Pixel", ImGuiDataType_U32, &settings.samplesPerPixel, &minSpp, &maxSpp);
    tooltip("Requested path samples per pixel per frame when the 1 SPP limiter is disabled.");
    changed |= ImGui::Checkbox("Path Tracing", &settings.pathTracingEnabled);
    changed |= ImGui::Checkbox("TAA Camera Jitter", &settings.cameraJitterEnabled);
    tooltip("Halton sub-pixel jitter. It is only applied while TAA is enabled.");
    changed |= ImGui::Checkbox("Direct Lighting", &settings.directLightingEnabled);
    changed |= ImGui::Checkbox("Secondary Bounce Direct", &settings.secondaryDirectLightingEnabled);
    tooltip("Samples direct lighting at secondary surface hits so two-bounce renders keep bounce illumination.");
    changed |= ImGui::SliderFloat("Indirect Strength", &settings.indirectStrength, 0.0f, 4.0f, "%.2f");
    tooltip("Multiplier for indirect lighting contribution.");
    const char* restirModeItems[] = {"Classic NEE", "ReSTIR Only", "Hybrid Compare"};
    int restirModeIndex = static_cast<int>(settings.restirMode);
    if (restirModeIndex < 0 || restirModeIndex > 2) {
        restirModeIndex = 0;
    }
    if (ImGui::Combo("ReSTIR Mode", &restirModeIndex, restirModeItems, 3)) {
        settings.restirMode = static_cast<RestirMode>(restirModeIndex);
        changed = true;
    }
    tooltip("Hybrid ReSTIR direct-light mode. Classic NEE remains the reference baseline.");
    const char* restirDiModeItems[] = {"Off", "Legacy", "Production", "Reference Validation", "Hybrid Compare"};
    int restirDiModeIndex = static_cast<int>(settings.restirDiMode);
    if (ImGui::Combo("ReSTIR DI Pipeline", &restirDiModeIndex, restirDiModeItems, 5)) {
        settings.restirDiMode = static_cast<RestirDiMode>(restirDiModeIndex);
        if (settings.restirDiMode == RestirDiMode::Off || settings.restirDiMode == RestirDiMode::Legacy) {
            settings.restirDiReservoirLayout = RestirDiReservoirLayout::Legacy;
        } else if (settings.restirDiMode == RestirDiMode::ReferenceValidation) {
            settings.restirDiReservoirLayout = RestirDiReservoirLayout::ValidationFull;
            settings.restirDiFinalVisibilityEnabled = true;
            settings.restirDiProductionStabilizationEnabled = false;
        } else if (settings.restirDiReservoirLayout == RestirDiReservoirLayout::Legacy) {
            settings.restirDiReservoirLayout = RestirDiReservoirLayout::ProductionPacked;
        }
        changed = true;
    }
    tooltip("Selects the rollback path, shipping ReSTIR DI pipeline, strict reference estimator, or comparison mode.");
    if (settings.restirDiMode == RestirDiMode::Production ||
        settings.restirDiMode == RestirDiMode::ReferenceValidation ||
        settings.restirDiMode == RestirDiMode::HybridCompare) {
        if (ImGui::CollapsingHeader("ReSTIR DI Tuning")) {
            const char* layoutItems[] = {"Legacy", "Production Packed", "Validation Full"};
            int layoutIndex = static_cast<int>(settings.restirDiReservoirLayout);
            if (ImGui::Combo("DI Reservoir Layout", &layoutIndex, layoutItems, 3)) {
                const auto requested = static_cast<RestirDiReservoirLayout>(layoutIndex);
                if (requested != RestirDiReservoirLayout::Legacy &&
                    (settings.restirDiMode != RestirDiMode::ReferenceValidation ||
                     requested == RestirDiReservoirLayout::ValidationFull)) {
                    settings.restirDiReservoirLayout = requested;
                    changed = true;
                }
            }
            tooltip("Production Packed is the shipping ABI. Validation Full is the inspectable reference ABI.");
            changed |= ImGui::Checkbox("DI Temporal Reuse", &settings.restirDiTemporalEnabled);
            changed |= ImGui::Checkbox("DI Spatial Reuse", &settings.restirDiSpatialEnabled);
            changed |= ImGui::Checkbox("DI Final Visibility", &settings.restirDiFinalVisibilityEnabled);
            tooltip("Required in Reference Validation; traces current visibility before finalizing a reused sample.");
            const uint32_t minRounds = 1u;
            const uint32_t maxRounds = 16u;
            const uint32_t minAge = 1u;
            const uint32_t maxAge = 255u;
            const uint32_t minM = 1u;
            const uint32_t maxM = 255u;
            const uint32_t minVisibilityRays = 0u;
            const uint32_t maxVisibilityRays = 4u;
            changed |= ImGui::SliderScalar("DI Spatial Rounds", ImGuiDataType_U32, &settings.restirDiSpatialRounds, &minRounds, &maxRounds);
            changed |= ImGui::SliderFloat("DI Spatial Radius", &settings.restirDiSpatialRadius, 0.5f, 32.0f, "%.2f");
            changed |= ImGui::SliderScalar("DI Temporal Max Age", ImGuiDataType_U32, &settings.restirDiTemporalMaxAge, &minAge, &maxAge);
            changed |= ImGui::SliderScalar("DI Max M", ImGuiDataType_U32, &settings.restirDiMaxM, &minM, &maxM);
            changed |= ImGui::SliderScalar("DI Visibility Ray Budget", ImGuiDataType_U32, &settings.restirDiVisibilityRayBudget, &minVisibilityRays, &maxVisibilityRays);
            tooltip("Maximum shifted-sample visibility queries per pixel in each DI reuse stage. Zero falls back to the current-frame candidate.");
            changed |= ImGui::Checkbox("DI Production Stabilization", &settings.restirDiProductionStabilizationEnabled);
            tooltip("Biased temporal and luminance stabilization for production. Reference Validation disables it.");
            changed |= ImGui::SliderFloat("DI Luminance Clamp", &settings.restirDiClampLuminance, 0.0f, 1000.0f, "%.1f");
            ImGui::BeginDisabled();
            bool sunOutside = false;
            bool environmentOutside = false;
            ImGui::Checkbox("Sample Sun In DI", &sunOutside);
            ImGui::Checkbox("Sample Environment In DI", &environmentOutside);
            ImGui::EndDisabled();
            tooltip("Sun and environment use specialized samplers outside ReSTIR DI and are composed exactly once.");
        }
    }
    if (ImGui::Checkbox("ReSTIR GI", &settings.restirGiEnabled)) {
        settings.restirGiMode = settings.restirGiEnabled ? RestirGiMode::Production : RestirGiMode::Off;
        settings.restirGiReservoirLayout = settings.restirGiEnabled
            ? RestirGiReservoirLayout::ProductionPacked
            : RestirGiReservoirLayout::LegacyCachePacked;
        changed = true;
    }
    tooltip("Enables ReSTIR GI reservoir reuse and final GI contribution in normal beauty rendering.");
    {
        const char* giModeItems[] = {"Off", "Legacy Cache", "Production", "Reference Validation"};
        int giMode = static_cast<int>(settings.restirGiMode);
        if (giMode < 0 || giMode > 3) {
            giMode = 2;
            settings.restirGiMode = RestirGiMode::Production;
            changed = true;
        }
        if (ImGui::Combo("GI Mode", &giMode, giModeItems, 4)) {
            settings.restirGiMode = static_cast<RestirGiMode>(giMode);
            if (settings.restirGiMode == RestirGiMode::Off ||
                settings.restirGiMode == RestirGiMode::LegacyCache) {
                settings.restirGiReservoirLayout = RestirGiReservoirLayout::LegacyCachePacked;
            } else if (settings.restirGiMode == RestirGiMode::ReferenceValidation) {
                settings.restirGiReservoirLayout = RestirGiReservoirLayout::ValidationFull;
                settings.restirGiFinalStabilizationEnabled = false;
            } else if (settings.restirGiReservoirLayout == RestirGiReservoirLayout::LegacyCachePacked) {
                settings.restirGiReservoirLayout = RestirGiReservoirLayout::ProductionPacked;
            }
            changed = true;
        }
        tooltip("Production is the default GI path. Reference Validation forces validation-full layout and disables biased final stabilization.");
    }
    {
        const char* layoutItems[] = {"Legacy Cache Packed", "Production Packed", "Validation Full"};
        int layout = static_cast<int>(settings.restirGiReservoirLayout);
        if (layout < 0 || layout > 2) {
            layout = static_cast<int>(RestirGiReservoirLayout::ProductionPacked);
            settings.restirGiReservoirLayout = RestirGiReservoirLayout::ProductionPacked;
        }
        const bool forceLegacyLayout = settings.restirGiMode == RestirGiMode::Off ||
            settings.restirGiMode == RestirGiMode::LegacyCache;
        const bool forceValidationLayout = settings.restirGiMode == RestirGiMode::ReferenceValidation;
        ImGui::BeginDisabled(forceLegacyLayout || forceValidationLayout);
        if (ImGui::Combo("GI Reservoir Layout", &layout, layoutItems, 3)) {
            settings.restirGiReservoirLayout = static_cast<RestirGiReservoirLayout>(layout);
            changed = true;
        }
        ImGui::EndDisabled();
        if (forceLegacyLayout && settings.restirGiReservoirLayout != RestirGiReservoirLayout::LegacyCachePacked) {
            settings.restirGiReservoirLayout = RestirGiReservoirLayout::LegacyCachePacked;
            changed = true;
        } else if (forceValidationLayout && settings.restirGiReservoirLayout != RestirGiReservoirLayout::ValidationFull) {
            settings.restirGiReservoirLayout = RestirGiReservoirLayout::ValidationFull;
            changed = true;
        } else if (settings.restirGiMode == RestirGiMode::Production &&
                   settings.restirGiReservoirLayout == RestirGiReservoirLayout::LegacyCachePacked) {
            settings.restirGiReservoirLayout = RestirGiReservoirLayout::ProductionPacked;
            changed = true;
        }
        tooltip("Legacy mode uses legacy-cache-packed. Production defaults to production-packed; validation-full is available for ABI comparisons.");
    }
    if (ImGui::CollapsingHeader("ReSTIR GI Tuning")) {
        const char* presetItems[] = {"Custom", "Reference", "Balanced", "Performance"};
        int preset = 0;
        if (ImGui::Combo("GI Preset", &preset, presetItems, 4) && preset != 0) {
            if (preset == 1) {
                settings.restirGiTemporalMaxAge = 32;
                settings.restirGiSpatialRounds = 6;
                settings.restirGiSpatialRadius = 4.25f;
                settings.restirGiDepthThresholdScale = 0.85f;
                settings.restirGiSpatialCompatibilityThreshold = 0.10f;
                settings.restirGiHalfResolution = false;
                settings.restirGiVisibilityRayBudget = 0;
                settings.restirGiFinalStabilizationEnabled = false;
            } else if (preset == 2) {
                settings.restirGiTemporalMaxAge = 12;
                settings.restirGiSpatialRounds = 2;
                settings.restirGiSpatialRadius = 3.0f;
                settings.restirGiDepthThresholdScale = 0.85f;
                settings.restirGiSpatialCompatibilityThreshold = 0.08f;
                settings.restirGiHalfResolution = false;
                settings.restirGiVisibilityRayBudget = 1;
                settings.restirGiFinalStabilizationEnabled = true;
            } else {
                settings.restirGiTemporalMaxAge = 16;
                settings.restirGiSpatialRounds = 2;
                settings.restirGiSpatialRadius = 3.0f;
                settings.restirGiDepthThresholdScale = 1.15f;
                settings.restirGiSpatialCompatibilityThreshold = 0.0f;
                settings.restirGiHalfResolution = true;
                settings.restirGiVisibilityRayBudget = 1;
                settings.restirGiFinalStabilizationEnabled = true;
            }
            changed = true;
        }
        tooltip("Applies ReSTIR GI reservoir reuse presets. Custom values remain editable below.");
        changed |= ImGui::Checkbox("GI Final Stabilization", &settings.restirGiFinalStabilizationEnabled);
        tooltip("Applies confidence, motion, history, and luminance clamps to the final ReSTIR GI contribution. Disable for raw reservoir A/B checks.");
        changed |= ImGui::Checkbox("GI Half Resolution Reuse", &settings.restirGiHalfResolution);
        tooltip("Uses one spatial GI reservoir per 2x2 pixel group for the GI debug/final path.");
        int activeTileMaskMode = static_cast<int>(settings.restirGiActiveTileMaskMode);
        if (ImGui::Combo("GI Active Tile Mask", &activeTileMaskMode, "Off\0On\0Auto\0")) {
            settings.restirGiActiveTileMaskMode = static_cast<RestirGiActiveTileMaskMode>(activeTileMaskMode);
            changed = true;
        }
        tooltip("Skips production GI reuse/final work on 16x16 tiles without reusable GI candidates. Auto probes off/on and keeps the faster mode.");
        int historyCopyMode = static_cast<int>(settings.restirHistoryCopyMode);
        if (ImGui::Combo("ReSTIR History Copy", &historyCopyMode, "Copy\0Ping-pong\0")) {
            settings.restirHistoryCopyMode = static_cast<RestirHistoryCopyMode>(historyCopyMode);
            changed = true;
        }
        tooltip("Experimental: ping-pong can avoid DI/GI history copies when supported. Copy remains the default.");
        changed |= ImGui::SliderScalar("GI Temporal Max Age", ImGuiDataType_U32, &settings.restirGiTemporalMaxAge, &minRestirGiAge, &maxRestirGiAge);
        changed |= ImGui::SliderScalar("GI Spatial Rounds", ImGuiDataType_U32, &settings.restirGiSpatialRounds, &minRestirGiRounds, &maxRestirGiRounds);
        changed |= ImGui::SliderFloat("GI Spatial Radius", &settings.restirGiSpatialRadius, 1.0f, 8.0f, "%.2f");
        changed |= ImGui::SliderFloat("GI Depth Threshold Scale", &settings.restirGiDepthThresholdScale, 0.5f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("GI Compatibility Cutoff", &settings.restirGiSpatialCompatibilityThreshold, 0.0f, 0.85f, "%.2f");
        changed |= ImGui::SliderScalar("GI Visibility Rays", ImGuiDataType_U32, &settings.restirGiVisibilityRayBudget, &minRestirGiVisibilityRays, &maxRestirGiVisibilityRays);
        tooltip("Ray-query visibility budget per pixel for temporal/spatial GI reuse. Zero validates all configured GI reuse candidates.");
    }
    if (ImGui::CollapsingHeader("Renderer Roadmap")) {
        const char* lightingReuseItems[] = {"Legacy DI/GI", "Legacy DI/GI + ReGIR", "Experimental ReSTIR PT", "Validate ReSTIR PT"};
        int lightingReuseIndex = static_cast<int>(settings.lightingReuseMode);
        if (lightingReuseIndex < 0 || lightingReuseIndex > 3) {
            lightingReuseIndex = 0;
        }
        if (ImGui::Combo("Lighting Reuse Mode", &lightingReuseIndex, lightingReuseItems, 4)) {
            settings.lightingReuseMode = static_cast<LightingReuseMode>(lightingReuseIndex);
            if (settings.lightingReuseMode == LightingReuseMode::ExperimentalRestirPT ||
                settings.lightingReuseMode == LightingReuseMode::ValidateRestirPTAgainstLegacy) {
                settings.pathReservoirLayout = ReservoirLayout::PathSpace;
            }
            changed = true;
        }
        tooltip("Selects the production legacy path, future ReGIR path, or experimental ReSTIR PT validation path.");

        if (settings.lightingReuseMode == LightingReuseMode::LegacyRestirDiGiPlusReGIR &&
            ImGui::TreeNode("ReGIR Tuning")) {
            const char* regirGridModeItems[] = {"Dense", "Active", "Hash"};
            int regirGridModeIndex = static_cast<int>(settings.regirGridMode);
            if (regirGridModeIndex < 0 || regirGridModeIndex > 2) {
                regirGridModeIndex = 0;
            }
            if (ImGui::Combo("Grid Mode", &regirGridModeIndex, regirGridModeItems, 3)) {
                settings.regirGridMode = static_cast<RegirGridMode>(regirGridModeIndex);
                changed = true;
            }
            tooltip("Dense is the stable baseline. Active and Hash compact work around the previous frame's queried cells.");

            const char* regirQueryModeItems[] = {"Deterministic", "Stochastic"};
            int regirQueryModeIndex = static_cast<int>(settings.regirQueryMode);
            if (regirQueryModeIndex < 0 || regirQueryModeIndex > 1) {
                regirQueryModeIndex = 1;
            }
            if (ImGui::Combo("Query Mode", &regirQueryModeIndex, regirQueryModeItems, 2)) {
                settings.regirQueryMode = static_cast<RegirQueryMode>(regirQueryModeIndex);
                changed = true;
            }
            tooltip("Stochastic cell lookup reduces structured reuse artifacts. Deterministic is useful for debugging.");

            const uint32_t minFiniteQueryPeriod = 0u;
            const uint32_t maxFiniteQueryPeriod = 4096u;
            changed |= ImGui::SliderScalar(
                "Finite Query Period",
                ImGuiDataType_U32,
                &settings.regirFiniteQueryFramePeriod,
                &minFiniteQueryPeriod,
                &maxFiniteQueryPeriod);
            tooltip("Frames between coherent finite-light queries. Zero selects the automatic grid-mode period.");

            const uint32_t minGridDim = 1u;
            const uint32_t maxGridDim = 128u;
            changed |= ImGui::SliderScalar("Grid X", ImGuiDataType_U32, &settings.regirGridDimensions.x, &minGridDim, &maxGridDim);
            changed |= ImGui::SliderScalar("Grid Y", ImGuiDataType_U32, &settings.regirGridDimensions.y, &minGridDim, &maxGridDim);
            changed |= ImGui::SliderScalar("Grid Z", ImGuiDataType_U32, &settings.regirGridDimensions.z, &minGridDim, &maxGridDim);
            tooltip("World-space grid dimensions. Large dense grids can allocate substantial reservoir memory.");

            const uint32_t minReservoirs = 1u;
            const uint32_t maxReservoirs = 64u;
            const uint32_t minCandidates = 1u;
            const uint32_t maxCandidates = 256u;
            changed |= ImGui::SliderScalar("Reservoirs Per Cell", ImGuiDataType_U32, &settings.regirReservoirsPerCell, &minReservoirs, &maxReservoirs);
            changed |= ImGui::SliderScalar("Candidates Per Reservoir", ImGuiDataType_U32, &settings.regirCandidatesPerReservoir, &minCandidates, &maxCandidates);
            changed |= ImGui::SliderFloat("Grid Padding", &settings.regirGridPadding, 0.0f, 0.5f, "%.2f");
            tooltip("Expands the light bounds before fitting the grid. Small padding avoids edge-cell misses.");

            changed |= ImGui::SliderFloat("Canonical Mix", &settings.regirCanonicalMix, 0.0f, 1.0f, "%.2f");
            tooltip("Fraction of secondary NEE samples that remain canonical for bias-safe ReGIR mixing.");

            changed |= ImGui::Checkbox("Spatial Reuse", &settings.regirSpatialReuse);
            const uint32_t minSpatialRounds = 1u;
            const uint32_t maxSpatialRounds = 8u;
            changed |= ImGui::SliderScalar("Spatial Rounds", ImGuiDataType_U32, &settings.regirSpatialRounds, &minSpatialRounds, &maxSpatialRounds);
            changed |= ImGui::Checkbox("Temporal Reuse", &settings.regirTemporalReuse);
            const uint32_t minTemporalHistory = 0u;
            const uint32_t maxTemporalHistory = 128u;
            const uint32_t minTemporalMaxM = 1u;
            const uint32_t maxTemporalMaxM = 1024u;
            changed |= ImGui::SliderScalar("Temporal History", ImGuiDataType_U32, &settings.regirTemporalHistory, &minTemporalHistory, &maxTemporalHistory);
            changed |= ImGui::SliderScalar("Temporal Max M", ImGuiDataType_U32, &settings.regirTemporalMaxM, &minTemporalMaxM, &maxTemporalMaxM);

            changed |= ImGui::Checkbox("Infinite Lights", &settings.regirEnvironment);
            tooltip("Experimental global ReGIR banks for environment/sky and analytical-sun samples. Default off; inspect coverage with the ReGIR environment source debug view.");
            changed |= ImGui::Checkbox("Visibility Reuse", &settings.regirVisibilityReuse);
            tooltip("Experimental NEE++ mode. Resamples a bounded candidate set with current-point visibility and reuses the selected transmittance.");
            ImGui::TreePop();
        }

        const char* reservoirLayoutItems[] = {"Legacy DI", "Legacy GI", "Path Space", "Path Space Compressed"};
        int reservoirLayoutIndex = static_cast<int>(settings.pathReservoirLayout);
        if (reservoirLayoutIndex < 0 || reservoirLayoutIndex > 3) {
            reservoirLayoutIndex = 0;
        }
        if (ImGui::Combo("Path Reservoir Layout", &reservoirLayoutIndex, reservoirLayoutItems, 4)) {
            settings.pathReservoirLayout = static_cast<ReservoirLayout>(reservoirLayoutIndex);
            changed = true;
        }
        tooltip("Records the requested reservoir packing contract for GRIS/ReSTIR PT work.");

        const char* adaptiveSamplingItems[] = {"Disabled", "Heuristic", "Neural"};
        int adaptiveSamplingIndex = static_cast<int>(settings.adaptiveSamplingMode);
        if (adaptiveSamplingIndex < 0 || adaptiveSamplingIndex > 2) {
            adaptiveSamplingIndex = 0;
        }
        if (ImGui::Combo("Adaptive Sampling", &adaptiveSamplingIndex, adaptiveSamplingItems, 3)) {
            settings.adaptiveSamplingMode = static_cast<AdaptiveSamplingMode>(adaptiveSamplingIndex);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Adaptive Sampling Budget", &settings.adaptiveSamplingBudget, 0.11f, 4.0f, "%.2f spp");
        if (ImGui::TreeNode("Adaptive Signal Weights")) {
            changed |= ImGui::SliderFloat("Variance", &settings.adaptiveWeightVariance, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("History", &settings.adaptiveWeightHistory, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Motion", &settings.adaptiveWeightMotion, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Disocclusion", &settings.adaptiveWeightDisocclusion, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Reactive", &settings.adaptiveWeightReactive, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Edges", &settings.adaptiveWeightEdge, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Specular", &settings.adaptiveWeightSpecular, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("ReSTIR DI", &settings.adaptiveWeightDI, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("ReSTIR GI", &settings.adaptiveWeightGI, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Volumetric", &settings.adaptiveWeightVolumetric, 0.0f, 1.0f, "%.2f");
            ImGui::TreePop();
        }
    }
    const char* tsrPresetItems[] = {"Native", "Quality", "Balanced", "Performance"};
    int tsrPreset = settings.renderResolutionScale >= 0.99f ? 0 :
        (settings.renderResolutionScale >= 0.74f ? 1 : (settings.renderResolutionScale >= 0.59f ? 2 : 3));
    if (ImGui::Combo("TSR Preset", &tsrPreset, tsrPresetItems, 4)) {
        const float presetScales[] = {1.0f, 0.77f, 0.67f, 0.50f};
        settings.renderResolutionScale = presetScales[tsrPreset];
        changed = true;
    }
    const char* temporalItems[] = {"TAA / TSR", "DLSS", "NIS"};
    int temporalIndex = static_cast<int>(settings.temporalUpscaler);
    if (temporalIndex < 0 || temporalIndex > 2) {
        temporalIndex = 0;
    }
    if (ImGui::BeginCombo("Temporal Upscaler", temporalItems[temporalIndex])) {
        if (ImGui::Selectable("TAA / TSR", settings.temporalUpscaler == TemporalUpscaler::TaaTsr)) {
            settings.temporalUpscaler = TemporalUpscaler::TaaTsr;
            settings.dlssRayReconstructionEnabled = false;
            settings.dlssFrameGenerationEnabled = false;
            changed = true;
        }
        ImGui::BeginDisabled(!dlssCanRequest);
        if (ImGui::Selectable("DLSS", settings.temporalUpscaler == TemporalUpscaler::Dlss)) {
            settings.temporalUpscaler = TemporalUpscaler::Dlss;
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::Selectable("NIS", settings.temporalUpscaler == TemporalUpscaler::Nis)) {
            settings.temporalUpscaler = TemporalUpscaler::Nis;
            settings.dlssRayReconstructionEnabled = false;
            settings.dlssFrameGenerationEnabled = false;
            changed = true;
        }
        ImGui::EndCombo();
    }
    tooltip(settings.temporalUpscaler == TemporalUpscaler::Dlss && !dlssCanRequest
        ? nvidiaStatus.dlssUnavailableReason.c_str()
        : (settings.temporalUpscaler == TemporalUpscaler::Nis
            ? nvidiaStatus.streamlineNis.unavailableReason.c_str()
            : "Selects the post-denoise temporal resolve/upscale backend."));
    ImGui::BeginDisabled(!dlssCanRequest);
    changed |= ImGui::SliderFloat("DLSS Sharpening", &settings.dlssSharpeningStrength, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    tooltip(dlssCanRequest
        ? "Sharpening amount passed to DLSS Super Resolution."
        : nvidiaStatus.dlssUnavailableReason.c_str());
    bool rrEnabled = settings.dlssRayReconstructionEnabled;
    ImGui::BeginDisabled(!dlssRayReconstructionCanRequest);
    if (ImGui::Checkbox("DLSS Ray Reconstruction", &rrEnabled)) {
        settings.dlssRayReconstructionEnabled = rrEnabled;
        if (rrEnabled) {
            settings.temporalUpscaler = TemporalUpscaler::Dlss;
        }
        changed = true;
    }
    ImGui::EndDisabled();
    tooltip(dlssRayReconstructionCanRequest && nvidiaStatus.dlssRayReconstructionAvailable
        ? "Uses DLSS Ray Reconstruction as the temporal denoiser/upscaler."
        : nvidiaStatus.dlssRayReconstructionUnavailableReason.c_str());
    bool fgEnabled = settings.dlssFrameGenerationEnabled;
    ImGui::BeginDisabled(!dlssFrameGenerationCanRequest);
    if (ImGui::Checkbox("DLSS Frame Generation", &fgEnabled)) {
        settings.dlssFrameGenerationEnabled = fgEnabled;
        if (fgEnabled) {
            settings.temporalUpscaler = TemporalUpscaler::Dlss;
        }
        changed = true;
    }
    ImGui::EndDisabled();
    tooltip(dlssFrameGenerationCanRequest
        ? "Enables DLSS Frame Generation when the presentation path supports generated frames."
        : nvidiaStatus.dlssFrameGenerationUnavailableReason.c_str());
    bool reflexEnabled = settings.streamlineReflexEnabled;
    ImGui::BeginDisabled(!reflexCanRequest);
    if (ImGui::Checkbox("Streamline Reflex", &reflexEnabled)) {
        settings.streamlineReflexEnabled = reflexEnabled;
        changed = true;
    }
    ImGui::EndDisabled();
    tooltip(reflexCanRequest
        ? "Emits Streamline Reflex/PCL latency markers at simulation, render-submit, and present boundaries."
        : nvidiaStatus.streamlineReflex.unavailableReason.c_str());
    bool nvperfEnabled = settings.streamlineNvPerfEnabled;
    ImGui::BeginDisabled(!nvperfCanRequest);
    if (ImGui::Checkbox("Streamline NvPerf", &nvperfEnabled)) {
        settings.streamlineNvPerfEnabled = nvperfEnabled;
        changed = true;
    }
    ImGui::EndDisabled();
    tooltip(nvperfCanRequest
        ? "Requests Streamline NvPerf evaluation for Nsight Perf HUD and per-frame performance diagnostics."
        : nvidiaStatus.streamlineNvPerf.unavailableReason.c_str());
    changed |= ImGui::SliderFloat("Render Resolution Scale", &settings.renderResolutionScale, 0.25f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Material Anisotropy", &settings.materialTextureAnisotropy, 1.0f, 16.0f, "%.1fx");
    tooltip("Anisotropic filtering level for material textures. Unsupported devices clamp to 1x.");
    changed |= ImGui::Checkbox("Specular AA", &settings.specularAaEnabled);
    tooltip("Raises effective specular roughness for high-frequency normal maps without changing material roughness.");
    const OpacityMicromapDeviceInfo& ommInfo = state.renderer.opacityMicromapInfo();
    if (!ommInfo.supported) {
        settings.opacityMicromapsEnabled = false;
    }
    ImGui::BeginDisabled(!ommInfo.supported);
    changed |= ImGui::Checkbox("Opacity Micromaps", &settings.opacityMicromapsEnabled);
    ImGui::EndDisabled();
    tooltip(ommInfo.supported
        ? "Builds hardware opacity micromaps for eligible alpha-tested BLAS geometry."
        : ommInfo.disabledReason.c_str());
    const SerDeviceInfo& serInfo = state.renderer.serInfo();
    ImGui::Text("SER: %s", serInfo.supported ? "available" : "unavailable");
    tooltip(serInfo.supported
        ? serReorderingHintName(serInfo.reorderingHint)
        : serInfo.disabledReason.c_str());
    if (!serInfo.supported) {
        settings.shaderExecutionReorderingEnabled = false;
    }
    ImGui::BeginDisabled(!serInfo.supported);
    changed |= ImGui::Checkbox("Wavefront SER", &settings.shaderExecutionReorderingEnabled);
    ImGui::EndDisabled();
    tooltip(serInfo.supported
        ? "Enables shader execution reordering hints for the opt-in wavefront trace raygen path."
        : serInfo.disabledReason.c_str());
    const char* adaptiveItems[] = {"Off", "Conservative", "Balanced", "Aggressive"};
    int adaptiveIndex = static_cast<int>(settings.adaptiveQualityMode);
    if (adaptiveIndex < 0 || adaptiveIndex > 3) {
        adaptiveIndex = 0;
    }
    if (ImGui::Combo("Adaptive Quality", &adaptiveIndex, adaptiveItems, 4)) {
        settings.adaptiveQualityMode = static_cast<AdaptiveQualityMode>(adaptiveIndex);
        changed = true;
    }
    tooltip("Dynamically lowers expensive path-tracing controls while moving or over the GPU frame target.");
    changed |= ImGui::SliderFloat("Adaptive GPU Target", &settings.adaptiveGpuFrameTargetMs, 4.0f, 100.0f, "%.1f ms");
    tooltip("Target smoothed GPU frame time used by adaptive quality modes.");

    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* toneMapperItems2[] = {"Linear", "Reinhard", "Reinhard White", "ACES", "PBR Neutral", "AgX"};
        int toneMapperIndex2 = static_cast<int>(settings.toneMapper);
        if (toneMapperIndex2 < 0 || toneMapperIndex2 > 5) {
            toneMapperIndex2 = 3;
        }
        if (ImGui::Combo("Tone Mapper", &toneMapperIndex2, toneMapperItems2, 6)) {
            settings.toneMapper = static_cast<ToneMapper>(toneMapperIndex2);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 64.0f, "%.2f");
        tooltip("Overall brightness multiplier. Higher values make the image brighter.");
        changed |= ImGui::Checkbox("Auto Exposure", &settings.autoExposureEnabled);
        changed |= ImGui::Checkbox("Physical Camera", &settings.usePhysicalCamera);
        tooltip("Use physically based exposure from aperture, shutter speed, and ISO.");
        if (settings.usePhysicalCamera) {
            changed |= ImGui::SliderFloat("Aperture (f-stop)", &settings.physicalAperture, 1.0f, 32.0f, "f/%.1f");
            tooltip("Aperture f-number. Lower values let in more light.");
            changed |= ImGui::SliderFloat("Shutter Speed", &settings.physicalShutterSeconds, 1.0f / 8000.0f, 30.0f, "%.4f s");
            tooltip("Shutter duration in seconds.");
            changed |= ImGui::SliderFloat("ISO", &settings.physicalIso, 50.0f, 12800.0f, "%.0f");
            tooltip("Sensor sensitivity. Higher values brighten the image but add noise.");
            changed |= ImGui::SliderFloat("Exposure Compensation", &settings.physicalExposureCompensation, -5.0f, 5.0f, "%.1f EV");
            tooltip("Exposure compensation offset in EV.");
            PhysicalCamera pc({
                settings.physicalAperture,
                settings.physicalShutterSeconds,
                settings.physicalIso,
                settings.physicalExposureCompensation,
                settings.dofApertureRadius,
                settings.dofFocusDistance,
                settings.dofBladeCount,
                settings.dofBokehRotation});
            ImGui::Text("EV100: %.1f", pc.ev100());
        }
        if (ImGui::TreeNodeEx("Depth of Field", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::SliderFloat("Aperture Radius", &settings.dofApertureRadius, 0.0f, 0.25f, "%.4f");
            tooltip("Thin-lens aperture radius in scene units. Zero keeps the pinhole camera path.");
            changed |= ImGui::SliderFloat("Focus Distance", &settings.dofFocusDistance, 0.05f, 100.0f, "%.2f");
            tooltip("Distance from the camera to the sharp focus plane.");
            int bladeCount = static_cast<int>(settings.dofBladeCount);
            if (ImGui::SliderInt("Aperture Blades", &bladeCount, 0, 16)) {
                settings.dofBladeCount = bladeCount <= 0 ? 0u : static_cast<uint32_t>(std::max(bladeCount, 3));
                changed = true;
            }
            tooltip("Use zero for circular bokeh, or 3-16 for polygonal bokeh.");
            changed |= ImGui::SliderFloat("Bokeh Rotation", &settings.dofBokehRotation, -3.14159f, 3.14159f, "%.2f rad");
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Motion Blur", ImGuiTreeNodeFlags_DefaultOpen)) {
            const RayTracingMotionBlurDeviceInfo& motionInfo = state.renderer.rayTracingMotionBlurInfo();
            if (!motionInfo.supported) {
                settings.motionBlurEnabled = false;
            }
            ImGui::BeginDisabled(!motionInfo.supported);
            changed |= ImGui::Checkbox("Ray Traced Motion Blur", &settings.motionBlurEnabled);
            changed |= ImGui::SliderFloat("Shutter Open", &settings.motionBlurShutterOpen, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Shutter Close", &settings.motionBlurShutterClose, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            tooltip(motionInfo.supported
                ? "Samples ray time across previous/current TLAS instance transforms."
                : motionInfo.disabledReason.c_str());
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Volume", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::Checkbox("Homogeneous Volume", &settings.homogeneousVolumeEnabled);
            changed |= ImGui::SliderFloat("Scattering", &settings.homogeneousVolumeScattering, 0.0f, 0.01f, "%.5f");
            changed |= ImGui::SliderFloat("Absorption", &settings.homogeneousVolumeAbsorption, 0.0f, 0.01f, "%.5f");
            changed |= ImGui::SliderFloat("Anisotropy", &settings.homogeneousVolumeAnisotropy, -0.95f, 0.95f, "%.2f");
            tooltip("Path-traced global homogeneous medium. Values are scene-unit extinction coefficients.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Caustics", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::Checkbox("MNEE Caustics", &settings.mneeCausticsEnabled);
            tooltip("Experimental single-interface transmissive caustic visibility for delta glass shadow paths.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::SliderFloat("Target Luminance", &settings.targetLuminance, 0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Min Exposure", &settings.minExposure, 0.01f, 64.0f, "%.2f");
            changed |= ImGui::SliderFloat("Max Exposure", &settings.maxExposure, 0.01f, 64.0f, "%.2f");
            changed |= ImGui::SliderFloat("Adaptation Speed", &settings.adaptationSpeed, 0.0f, 10.0f, "%.2f");
            changed |= ImGui::SliderFloat("Contrast", &settings.contrast, 0.0f, 2.0f, "%.2f");
            changed |= ImGui::SliderFloat("Saturation", &settings.saturation, 0.0f, 2.0f, "%.2f");
            changed |= ImGui::SliderFloat("Brightness", &settings.brightness, -1.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Gamma", &settings.gamma, 0.1f, 4.0f, "%.2f");
            changed |= ImGui::SliderFloat("White Point", &settings.whitePoint, 0.1f, 16.0f, "%.2f");
            changed |= ImGui::SliderFloat("Histogram Min EV", &settings.histogramMinLogLuminance, -20.0f, 0.0f, "%.1f");
            tooltip("Controls the lower bound of the auto-exposure metering range.");
            changed |= ImGui::SliderFloat("Histogram Max EV", &settings.histogramMaxLogLuminance, 0.0f, 20.0f, "%.1f");
            tooltip("Controls the upper bound of the auto-exposure metering range.");
            changed |= ImGui::SliderFloat("Histogram Low", &settings.histogramLowPercentile, 0.0f, 0.5f, "%.2f");
            tooltip("Controls the low percentile used by auto-exposure metering.");
            changed |= ImGui::SliderFloat("Histogram High", &settings.histogramHighPercentile, 0.5f, 1.0f, "%.2f");
            tooltip("Controls the high percentile used by auto-exposure metering.");
            changed |= ImGui::SliderFloat("Histogram Target", &settings.histogramTargetPercentile, 0.0f, 1.0f, "%.2f");
            tooltip("Controls the target percentile used by auto-exposure metering.");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Sun / Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (state.sceneDocument != nullptr) {
            const SunDerivedState sun = SunController::derivedState(*state.sceneDocument);
            ImGui::Text("Primary Sun: %s", SunController::primarySunEntity(*state.sceneDocument).valid() ? "Scene" : "Missing");
            ImGui::Text("Sun: %s  %.0f lux  %.5f rad", sun.enabled ? "On" : "Off", sun.illuminanceLux, sun.angularRadiusRadians);
            ImGui::Text("Direction: elev %.2f rad, az %.2f rad", sun.elevation, sun.azimuth);
            if (ImGui::Button("Create Primary Sun")) {
                requests.ensurePrimarySun = true;
                requests.sceneUpdate = SceneUpdateKind::LightOnly;
            }
        }
    }

    if (ImGui::CollapsingHeader("Atmosphere")) {
        changed |= ImGui::SliderFloat("Sky Intensity", &settings.skyIntensity, 0.0f, 3.0f, "%.2f");
        tooltip("Multiplier for atmospheric sky radiance.");
        changed |= ImGui::SliderFloat("Rayleigh Scale Height", &settings.rayleighScaleHeight, 1000.0f, 20000.0f, "%.0f m");
        tooltip("Scale height for Rayleigh (molecular) scattering. Higher = thicker blue atmosphere.");
        changed |= ImGui::SliderFloat("Mie Scale Height", &settings.mieScaleHeight, 200.0f, 5000.0f, "%.0f m");
        tooltip("Scale height for Mie (aerosol) scattering. Lower = denser horizon haze.");
        changed |= ImGui::SliderFloat("Mie Anisotropy", &settings.mieAnisotropy, 0.0f, 0.99f, "%.2f");
        tooltip("Forward/backward scattering asymmetry. Higher = more forward scattered light.");
        changed |= ImGui::SliderFloat("Ground Albedo", &settings.groundAlbedo, 0.0f, 1.0f, "%.2f");
        tooltip("Planetary ground reflectance. Affects atmospheric light bouncing off the terrain.");
    }

    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Show Environment", &settings.environmentEnabled);
        changed |= ImGui::SliderFloat("Environment Intensity", &settings.environmentIntensity, 0.0f, 8.0f, "%.2f");
        changed |= ImGui::SliderFloat("Background Intensity", &settings.environmentBackgroundIntensity, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Environment Rotation", &settings.environmentRotation, -6.28318f, 6.28318f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Denoiser / TAA", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Denoiser", &settings.denoiserEnabled);
        const char* denoiserBackendItems[] = {"Engine", "NRD"};
        int denoiserBackendIndex = static_cast<int>(settings.denoiserBackend);
        if (denoiserBackendIndex < 0 || denoiserBackendIndex > 1) {
            denoiserBackendIndex = 0;
        }
        if (ImGui::BeginCombo("Denoiser Backend", denoiserBackendItems[denoiserBackendIndex])) {
            if (ImGui::Selectable("Engine", settings.denoiserBackend == DenoiserBackend::Engine)) {
                settings.denoiserBackend = DenoiserBackend::Engine;
                changed = true;
            }
            ImGui::BeginDisabled(!nrdCanRequest);
            if (ImGui::Selectable("NRD", settings.denoiserBackend == DenoiserBackend::Nrd)) {
                settings.denoiserBackend = DenoiserBackend::Nrd;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::EndCombo();
        }
        tooltip(settings.denoiserBackend == DenoiserBackend::Nrd && !nrdCanRequest
            ? nvidiaStatus.nrdUnavailableReason.c_str()
            : "Selects the active denoiser backend.");
        changed |= ImGui::Checkbox("Denoise While Moving", &settings.denoiseWhileMoving);
        changed |= ImGui::SliderScalar("A-trous Iterations", ImGuiDataType_U32, &settings.atrousIterations, &minAtrous, &maxAtrous);
        tooltip("Denoiser iterations. More is smoother and slower.");
        changed |= ImGui::SliderFloat("Denoiser Strength", &settings.denoiserStrength, 0.05f, 4.0f, "%.2f");
        tooltip("Higher values denoise more aggressively and may lose detail.");
        const uint32_t minHistory = 4;
        const uint32_t maxHistory = 256;
        changed |= ImGui::SliderScalar("Max History Length", ImGuiDataType_U32, &settings.denoiserMaxHistoryLength, &minHistory, &maxHistory);
        tooltip("Maximum temporal history length for moment tracking. Higher values stabilize static scenes more.");
        changed |= ImGui::SliderFloat("Moment Validity Threshold", &settings.momentValidityThreshold, 0.05f, 0.75f, "%.2f");
        tooltip("Threshold for moment history validity. Lower = more history, more ghosting. Higher = stricter, less ghosting, more noise.");
        changed |= ImGui::Checkbox("TAA", &settings.taaEnabled);
        tooltip("HDR temporal anti-aliasing pass after denoising and before tone mapping.");
        changed |= ImGui::SliderFloat("TAA Feedback", &settings.taaFeedback, 0.01f, 0.5f, "%.2f");
        tooltip("Lower values keep more history; higher values react faster to motion and lighting changes.");
        changed |= ImGui::SliderFloat("TAA Motion Feedback", &settings.taaMotionFeedback, 0.25f, 0.98f, "%.2f");
        tooltip("Current-frame blend target while the camera is moving. Lower values stabilize noisy motion; higher values reduce ghosting.");
        changed |= ImGui::SliderFloat("TAA Reactive Feedback", &settings.taaReactiveFeedback, 0.25f, 0.99f, "%.2f");
        tooltip("Current-frame blend used for strong reactive or disocclusion cases while moving.");
        changed |= ImGui::SliderFloat("TAA Sharpening", &settings.taaSharpeningStrength, 0.0f, 1.0f, "%.2f");
        tooltip("Unsharp mask amount applied by the TAA resolve.");
    }

    if (ImGui::CollapsingHeader("Artifact Controls")) {
        changed |= ImGui::SliderFloat("Shadow Ray Bias", &settings.shadowRayBias, 0.00001f, 0.05f, "%.5f");
        tooltip("Surface offset used for secondary shadow rays.");
        changed |= ImGui::SliderFloat("Shadow Distance Bias", &settings.shadowDistanceBias, 0.0f, 0.1f, "%.5f");
        tooltip("Reduces the maximum distance of finite shadow rays to avoid self hits at the light.");
        changed |= ImGui::SliderFloat("Firefly Clamp", &settings.fireflyClamp, 1.0f, 512.0f, "%.1f");
        tooltip("Luminance clamp for single path samples before accumulation.");
    }

    if (changed) {
        if (!presetApplied) {
            settings.renderPreset = RenderPreset::Custom;
        }
        if (state.sceneDocument != nullptr) {
            RenderSettings& render = state.sceneDocument->renderSettings();
            Environment& environment = state.sceneDocument->environment();
            const bool environmentChanged =
                environment.enabled != settings.environmentEnabled ||
                std::abs(environment.intensity - settings.environmentIntensity) > 0.0001f ||
                std::abs(environment.rotation - settings.environmentRotation) > 0.0001f ||
                std::abs(environment.backgroundIntensity - settings.environmentBackgroundIntensity) > 0.0001f;
            const bool lightingChanged =
                render.directLightingEnabled != settings.directLightingEnabled ||
                render.environmentDirectSamples != settings.environmentDirectSamples ||
                render.restirMode != settings.restirMode ||
                render.restirDiMode != settings.restirDiMode ||
                render.restirDiTemporalEnabled != settings.restirDiTemporalEnabled ||
                render.restirDiSpatialEnabled != settings.restirDiSpatialEnabled ||
                render.restirDiFinalVisibilityEnabled != settings.restirDiFinalVisibilityEnabled ||
                render.restirDiReservoirLayout != settings.restirDiReservoirLayout ||
                render.restirGiEnabled != settings.restirGiEnabled ||
                render.restirGiMode != settings.restirGiMode ||
                render.restirGiReservoirLayout != settings.restirGiReservoirLayout ||
                render.lightingReuseMode != settings.lightingReuseMode ||
                render.pathReservoirLayout != settings.pathReservoirLayout ||
                render.regirGridDimensions != settings.regirGridDimensions ||
                render.regirReservoirsPerCell != settings.regirReservoirsPerCell ||
                render.regirCandidatesPerReservoir != settings.regirCandidatesPerReservoir ||
                std::abs(render.regirGridPadding - settings.regirGridPadding) > 0.0001f ||
                std::abs(render.regirCanonicalMix - settings.regirCanonicalMix) > 0.0001f ||
                render.regirQueryMode != settings.regirQueryMode ||
                render.regirGridMode != settings.regirGridMode ||
                render.regirFiniteQueryFramePeriod != settings.regirFiniteQueryFramePeriod ||
                render.regirSpatialReuse != settings.regirSpatialReuse ||
                render.regirSpatialRounds != settings.regirSpatialRounds ||
                render.regirTemporalReuse != settings.regirTemporalReuse ||
                render.regirTemporalHistory != settings.regirTemporalHistory ||
                render.regirTemporalMaxM != settings.regirTemporalMaxM ||
                render.regirVisibilityReuse != settings.regirVisibilityReuse ||
                render.regirEnvironment != settings.regirEnvironment ||
                render.adaptiveSamplingMode != settings.adaptiveSamplingMode ||
                std::abs(render.skyIntensity - settings.skyIntensity) > 0.0001f;
            render.renderPreset = settings.renderPreset;
            render.pathTracingEnabled = settings.pathTracingEnabled;
            render.cameraJitterEnabled = settings.cameraJitterEnabled;
            render.directLightingEnabled = settings.directLightingEnabled;
            render.secondaryDirectLightingEnabled = settings.secondaryDirectLightingEnabled;
            render.maxBounces = settings.maxBounces;
            render.pathTraceKernelMode = settings.pathTraceKernelMode;
            render.finalBounceFastPathEnabled = settings.finalBounceFastPathEnabled;
            render.native2BTerminalDirectSampleProbability = settings.native2BTerminalDirectSampleProbability;
            render.blendedDecalShadowMode = settings.blendedDecalShadowMode;
            render.native2BDirectReuseMode = settings.native2BDirectReuseMode;
            render.environmentDirectSamples = settings.environmentDirectSamples;
            render.toneMapper = settings.toneMapper;
            render.exposure = settings.exposure;
            render.gamma = settings.gamma;
            render.contrast = settings.contrast;
            render.saturation = settings.saturation;
            render.brightness = settings.brightness;
            render.whitePoint = settings.whitePoint;
            render.autoExposureEnabled = settings.autoExposureEnabled;
            render.targetLuminance = settings.targetLuminance;
            render.minExposure = settings.minExposure;
            render.maxExposure = settings.maxExposure;
            render.adaptationSpeed = settings.adaptationSpeed;
            render.histogramMinLogLuminance = settings.histogramMinLogLuminance;
            render.histogramMaxLogLuminance = settings.histogramMaxLogLuminance;
            render.histogramLowPercentile = settings.histogramLowPercentile;
            render.histogramHighPercentile = settings.histogramHighPercentile;
            render.histogramTargetPercentile = settings.histogramTargetPercentile;
            render.skyIntensity = settings.skyIntensity;
            render.indirectStrength = settings.indirectStrength;
            render.restirMode = settings.restirMode;
            render.restirDiMode = settings.restirDiMode;
            render.restirDiTemporalEnabled = settings.restirDiTemporalEnabled;
            render.restirDiSpatialEnabled = settings.restirDiSpatialEnabled;
            render.restirDiFinalVisibilityEnabled = settings.restirDiFinalVisibilityEnabled;
            render.restirDiSpatialRounds = settings.restirDiSpatialRounds;
            render.restirDiSpatialRadius = settings.restirDiSpatialRadius;
            render.restirDiTemporalMaxAge = settings.restirDiTemporalMaxAge;
            render.restirDiMaxM = settings.restirDiMaxM;
            render.restirDiVisibilityRayBudget = settings.restirDiVisibilityRayBudget;
            render.restirDiProductionStabilizationEnabled = settings.restirDiProductionStabilizationEnabled;
            render.restirDiClampLuminance = settings.restirDiClampLuminance;
            render.restirDiIncludeSun = settings.restirDiIncludeSun;
            render.restirDiIncludeEnvironment = settings.restirDiIncludeEnvironment;
            render.restirDiReservoirLayout = settings.restirDiReservoirLayout;
            render.restirGiMode = settings.restirGiMode;
            render.restirGiReservoirLayout = settings.restirGiReservoirLayout;
            render.restirGiEnabled = settings.restirGiEnabled;
            render.denoiserEnabled = settings.denoiserEnabled;
            render.denoiserBackend = settings.denoiserBackend;
            render.denoiseWhileMoving = settings.denoiseWhileMoving;
            render.samplesPerPixel = settings.samplesPerPixel;
            render.limitSamplesPerPixel = settings.limitSamplesPerPixel;
            render.atrousIterations = settings.atrousIterations;
            render.denoiserStrength = settings.denoiserStrength;
            render.denoiserMaxHistoryLength = settings.denoiserMaxHistoryLength;
            render.momentValidityThreshold = settings.momentValidityThreshold;
            render.taaEnabled = settings.taaEnabled;
            render.temporalUpscaler = settings.temporalUpscaler;
            render.dlssFrameGenerationEnabled = settings.dlssFrameGenerationEnabled;
            render.dlssRayReconstructionEnabled = settings.dlssRayReconstructionEnabled;
            render.streamlineReflexEnabled = settings.streamlineReflexEnabled;
            render.streamlineNvPerfEnabled = settings.streamlineNvPerfEnabled;
            render.dlssSharpeningStrength = settings.dlssSharpeningStrength;
            render.taaFeedback = settings.taaFeedback;
            render.taaMotionFeedback = settings.taaMotionFeedback;
            render.taaReactiveFeedback = settings.taaReactiveFeedback;
            render.taaSharpeningStrength = settings.taaSharpeningStrength;
            render.debugView = settings.debugView;
            render.resolutionScale = settings.renderResolutionScale;
            render.accumulationLimit = settings.accumulationLimit;
            render.materialTextureAnisotropy = settings.materialTextureAnisotropy;
            render.specularAaEnabled = settings.specularAaEnabled;
            render.opacityMicromapsEnabled = settings.opacityMicromapsEnabled;
            render.compactImportedEmissiveTriangleSampling = settings.compactImportedEmissiveTriangleSampling;
            render.shadowRayBias = settings.shadowRayBias;
            render.shadowDistanceBias = settings.shadowDistanceBias;
            render.fireflyClamp = settings.fireflyClamp;
            render.restirGiTemporalMaxAge = settings.restirGiTemporalMaxAge;
            render.restirGiSpatialRounds = settings.restirGiSpatialRounds;
            render.restirGiSpatialRadius = settings.restirGiSpatialRadius;
            render.restirGiDepthThresholdScale = settings.restirGiDepthThresholdScale;
            render.restirGiSpatialCompatibilityThreshold = settings.restirGiSpatialCompatibilityThreshold;
            render.restirGiHalfResolution = settings.restirGiHalfResolution;
            render.restirGiVisibilityRayBudget = settings.restirGiVisibilityRayBudget;
            render.restirGiFinalStabilizationEnabled = settings.restirGiFinalStabilizationEnabled;
            render.restirGiActiveTileMaskMode = settings.restirGiActiveTileMaskMode;
            render.restirHistoryCopyMode = settings.restirHistoryCopyMode;
            render.lightingReuseMode = settings.lightingReuseMode;
            render.pathReservoirLayout = settings.pathReservoirLayout;
            render.regirGridDimensions = settings.regirGridDimensions;
            render.regirReservoirsPerCell = settings.regirReservoirsPerCell;
            render.regirCandidatesPerReservoir = settings.regirCandidatesPerReservoir;
            render.regirGridPadding = settings.regirGridPadding;
            render.regirCanonicalMix = settings.regirCanonicalMix;
            render.regirQueryMode = settings.regirQueryMode;
            render.regirGridMode = settings.regirGridMode;
            render.regirFiniteQueryFramePeriod = settings.regirFiniteQueryFramePeriod;
            render.regirSpatialReuse = settings.regirSpatialReuse;
            render.regirSpatialRounds = settings.regirSpatialRounds;
            render.regirTemporalReuse = settings.regirTemporalReuse;
            render.regirTemporalHistory = settings.regirTemporalHistory;
            render.regirTemporalMaxM = settings.regirTemporalMaxM;
            render.regirVisibilityReuse = settings.regirVisibilityReuse;
            render.regirEnvironment = settings.regirEnvironment;
            render.adaptiveSamplingMode = settings.adaptiveSamplingMode;
            render.adaptiveSamplingBudget = settings.adaptiveSamplingBudget;
            render.adaptiveWeightVariance = settings.adaptiveWeightVariance;
            render.adaptiveWeightHistory = settings.adaptiveWeightHistory;
            render.adaptiveWeightMotion = settings.adaptiveWeightMotion;
            render.adaptiveWeightDisocclusion = settings.adaptiveWeightDisocclusion;
            render.adaptiveWeightReactive = settings.adaptiveWeightReactive;
            render.adaptiveWeightEdge = settings.adaptiveWeightEdge;
            render.adaptiveWeightSpecular = settings.adaptiveWeightSpecular;
            render.adaptiveWeightDI = settings.adaptiveWeightDI;
            render.adaptiveWeightGI = settings.adaptiveWeightGI;
            render.adaptiveWeightVolumetric = settings.adaptiveWeightVolumetric;
            render.adaptiveQualityMode = settings.adaptiveQualityMode;
            render.adaptiveGpuFrameTargetMs = settings.adaptiveGpuFrameTargetMs;
            render.usePhysicalCamera = settings.usePhysicalCamera;
            render.physicalAperture = settings.physicalAperture;
            render.physicalShutterSeconds = settings.physicalShutterSeconds;
            render.physicalIso = settings.physicalIso;
            render.physicalExposureCompensation = settings.physicalExposureCompensation;
            render.dofApertureRadius = settings.dofApertureRadius;
            render.dofFocusDistance = settings.dofFocusDistance;
            render.dofBladeCount = settings.dofBladeCount;
            render.dofBokehRotation = settings.dofBokehRotation;
            render.motionBlurEnabled = settings.motionBlurEnabled;
            render.motionBlurShutterOpen = settings.motionBlurShutterOpen;
            render.motionBlurShutterClose = settings.motionBlurShutterClose;
            render.homogeneousVolumeEnabled = settings.homogeneousVolumeEnabled;
            render.homogeneousVolumeScattering = settings.homogeneousVolumeScattering;
            render.homogeneousVolumeAbsorption = settings.homogeneousVolumeAbsorption;
            render.homogeneousVolumeAnisotropy = settings.homogeneousVolumeAnisotropy;
            render.mneeCausticsEnabled = settings.mneeCausticsEnabled;
            environment.enabled = settings.environmentEnabled;
            environment.intensity = settings.environmentIntensity;
            environment.rotation = settings.environmentRotation;
            environment.backgroundIntensity = settings.environmentBackgroundIntensity;
            const SceneUpdateKind kind = environmentChanged
                ? SceneUpdateKind::EnvironmentOnly
                : (lightingChanged ? SceneUpdateKind::LightOnly : SceneUpdateKind::RendererSettingsOnly);
            state.sceneDocument->markDirty(kind);
            requests.sceneUpdate = kind;
        }
        requestSettings(requests, settings);
    }

    ImGui::End();
}

} // namespace rtv
