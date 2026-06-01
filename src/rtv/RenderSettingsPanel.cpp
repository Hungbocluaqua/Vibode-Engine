#include "rtv/RenderSettingsPanel.h"

#include "rtv/EditorUiStyle.h"
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
        settings.maxBounces = render.maxBounces;
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
        settings.restirGiEnabled = render.restirGiEnabled;
        settings.denoiserEnabled = render.denoiserEnabled;
        settings.denoiseWhileMoving = render.denoiseWhileMoving;
        settings.samplesPerPixel = render.samplesPerPixel;
        settings.limitSamplesPerPixel = render.limitSamplesPerPixel;
        settings.atrousIterations = render.atrousIterations;
        settings.denoiserStrength = render.denoiserStrength;
        settings.denoiserMaxHistoryLength = render.denoiserMaxHistoryLength;
        settings.momentValidityThreshold = render.momentValidityThreshold;
        settings.taaEnabled = render.taaEnabled;
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
    }
    bool changed = false;
    bool presetApplied = false;
    uint32_t minBounces = 1;
    uint32_t maxBounces = 16;
    uint32_t minEnvSamples = 1;
    uint32_t maxEnvSamples = 8;
    uint32_t minSpp = 1;
    uint32_t maxSpp = 8;
    uint32_t minAtrous = 1;
    uint32_t maxAtrous = 5;
    uint32_t minRestirGiAge = 1;
    uint32_t maxRestirGiAge = 64;
    uint32_t minRestirGiRounds = 1;
    uint32_t maxRestirGiRounds = 8;
    uint32_t minRestirGiVisibilityRays = 0;
    uint32_t maxRestirGiVisibilityRays = 4;

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
    const char* renderPresetItems[] = {"Custom", "Low", "Balanced", "Ultra"};
    int renderPresetIndex = static_cast<int>(settings.renderPreset);
    if (renderPresetIndex < 0 || renderPresetIndex > 3) {
        renderPresetIndex = 0;
    }
    if (ImGui::Combo("Render Preset", &renderPresetIndex, renderPresetItems, 4)) {
        applyRenderPreset(settings, static_cast<RenderPreset>(renderPresetIndex));
        presetApplied = true;
        changed = true;
    }
    tooltip("Game-ready presets tune path tracing, ReSTIR, denoiser, TAA, and render scale together.");
    editorDebugViewCombo("Debug View", settings, changed);
    changed |= ImGui::SliderScalar("Max Bounces", ImGuiDataType_U32, &settings.maxBounces, &minBounces, &maxBounces);
    tooltip("Number of ray bounces. Higher is more accurate and slower; 4-8 for preview, 16 for final.");
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
    changed |= ImGui::Checkbox("ReSTIR GI", &settings.restirGiEnabled);
    tooltip("Enables ReSTIR GI reservoir reuse and final GI contribution in normal beauty rendering.");
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
            } else if (preset == 2) {
                settings.restirGiTemporalMaxAge = 24;
                settings.restirGiSpatialRounds = 4;
                settings.restirGiSpatialRadius = 4.25f;
                settings.restirGiDepthThresholdScale = 1.0f;
                settings.restirGiSpatialCompatibilityThreshold = 0.05f;
                settings.restirGiHalfResolution = false;
            } else {
                settings.restirGiTemporalMaxAge = 16;
                settings.restirGiSpatialRounds = 2;
                settings.restirGiSpatialRadius = 3.0f;
                settings.restirGiDepthThresholdScale = 1.15f;
                settings.restirGiSpatialCompatibilityThreshold = 0.0f;
                settings.restirGiHalfResolution = true;
            }
            changed = true;
        }
        tooltip("Applies ReSTIR GI reservoir reuse presets. Custom values remain editable below.");
        changed |= ImGui::Checkbox("GI Half Resolution Reuse", &settings.restirGiHalfResolution);
        tooltip("Uses one spatial GI reservoir per 2x2 pixel group for the GI debug/final path.");
        changed |= ImGui::SliderScalar("GI Temporal Max Age", ImGuiDataType_U32, &settings.restirGiTemporalMaxAge, &minRestirGiAge, &maxRestirGiAge);
        changed |= ImGui::SliderScalar("GI Spatial Rounds", ImGuiDataType_U32, &settings.restirGiSpatialRounds, &minRestirGiRounds, &maxRestirGiRounds);
        changed |= ImGui::SliderFloat("GI Spatial Radius", &settings.restirGiSpatialRadius, 1.0f, 8.0f, "%.2f");
        changed |= ImGui::SliderFloat("GI Depth Threshold Scale", &settings.restirGiDepthThresholdScale, 0.5f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("GI Compatibility Cutoff", &settings.restirGiSpatialCompatibilityThreshold, 0.0f, 0.85f, "%.2f");
        changed |= ImGui::SliderScalar("GI Visibility Rays", ImGuiDataType_U32, &settings.restirGiVisibilityRayBudget, &minRestirGiVisibilityRays, &maxRestirGiVisibilityRays);
        tooltip("Reserved budget for the future ray-query visibility validation pass; current GI spatial reuse remains conservative.");
    }
    const char* tsrPresetItems[] = {"Native", "Quality", "Balanced", "Performance"};
    int tsrPreset = settings.renderResolutionScale >= 0.99f ? 0 :
        (settings.renderResolutionScale >= 0.74f ? 1 : (settings.renderResolutionScale >= 0.59f ? 2 : 3));
    if (ImGui::Combo("TSR Preset", &tsrPreset, tsrPresetItems, 4)) {
        const float presetScales[] = {1.0f, 0.77f, 0.67f, 0.50f};
        settings.renderResolutionScale = presetScales[tsrPreset];
        changed = true;
    }
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
        changed |= ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 8.0f, "%.2f");
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
            changed |= ImGui::SliderFloat("Min Exposure", &settings.minExposure, 0.01f, 8.0f, "%.2f");
            changed |= ImGui::SliderFloat("Max Exposure", &settings.maxExposure, 0.01f, 16.0f, "%.2f");
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
                render.restirGiEnabled != settings.restirGiEnabled ||
                std::abs(render.skyIntensity - settings.skyIntensity) > 0.0001f;
            render.renderPreset = settings.renderPreset;
            render.pathTracingEnabled = settings.pathTracingEnabled;
            render.cameraJitterEnabled = settings.cameraJitterEnabled;
            render.directLightingEnabled = settings.directLightingEnabled;
            render.maxBounces = settings.maxBounces;
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
            render.restirGiEnabled = settings.restirGiEnabled;
            render.denoiserEnabled = settings.denoiserEnabled;
            render.denoiseWhileMoving = settings.denoiseWhileMoving;
            render.samplesPerPixel = settings.samplesPerPixel;
            render.limitSamplesPerPixel = settings.limitSamplesPerPixel;
            render.atrousIterations = settings.atrousIterations;
            render.denoiserStrength = settings.denoiserStrength;
            render.denoiserMaxHistoryLength = settings.denoiserMaxHistoryLength;
            render.momentValidityThreshold = settings.momentValidityThreshold;
            render.taaEnabled = settings.taaEnabled;
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
