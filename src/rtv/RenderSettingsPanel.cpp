#include "rtv/RenderSettingsPanel.h"

#include <imgui.h>
#include <rtv/PhysicalCamera.h>

#include <cmath>

namespace rtv {

namespace {

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

} // namespace

void RenderSettingsPanel::draw(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Render Settings")) {
        ImGui::End();
        return;
    }

    RendererSettings settings = state.renderer.settings();
    if (state.sceneDocument != nullptr) {
        const RenderSettings& render = state.sceneDocument->renderSettings();
        const Environment& environment = state.sceneDocument->environment();
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
        settings.sunlightEnabled = render.sunlightEnabled;
        settings.sunIntensity = render.sunIntensity;
        settings.skyIntensity = render.skyIntensity;
        settings.sunElevation = render.sunElevation;
        settings.sunAngularRadius = render.sunAngularRadius;
        settings.indirectStrength = render.indirectStrength;
        settings.restirMode = render.restirMode;
        settings.denoiserEnabled = render.denoiserEnabled;
        settings.atrousIterations = render.atrousIterations;
        settings.denoiserStrength = render.denoiserStrength;
        settings.taaEnabled = render.taaEnabled;
        settings.taaFeedback = render.taaFeedback;
        settings.debugView = render.debugView;
        settings.renderResolutionScale = render.resolutionScale;
        settings.requestedBackend = render.requestedBackend;
        settings.accumulationLimit = render.accumulationLimit;
        settings.usePhysicalCamera = render.usePhysicalCamera;
        settings.physicalAperture = render.physicalAperture;
        settings.physicalShutterSeconds = render.physicalShutterSeconds;
        settings.physicalIso = render.physicalIso;
        settings.physicalExposureCompensation = render.physicalExposureCompensation;
        settings.environmentEnabled = environment.enabled;
        settings.environmentIntensity = environment.intensity;
        settings.environmentRotation = environment.rotation;
        settings.environmentBackgroundIntensity = environment.backgroundIntensity;
    }
    bool changed = false;
    uint32_t minBounces = 1;
    uint32_t maxBounces = 16;
    uint32_t minEnvSamples = 1;
    uint32_t maxEnvSamples = 8;
    uint32_t minAtrous = 1;
    uint32_t maxAtrous = 5;

    const char* backendItems[] = {"Auto", "Compute", "Hardware Ray Tracing"};
    int backendIndex = settings.requestedBackend == RendererBackend::Compute
        ? 1
        : (settings.requestedBackend == RendererBackend::HardwareRayTracing ? 2 : 0);
    if (ImGui::Combo("Backend", &backendIndex, backendItems, 3)) {
        if (backendIndex == 2 && !state.renderer.hardwareRayTracingAvailable()) {
            settings.requestedBackend = RendererBackend::Auto;
        } else {
            settings.requestedBackend = backendIndex == 1
                ? RendererBackend::Compute
                : (backendIndex == 2 ? RendererBackend::HardwareRayTracing : RendererBackend::Auto);
        }
        changed = true;
    }
    ImGui::Text("Active Backend: %s", rendererBackendDisplayName(state.renderer.activeBackend()));
    ImGui::Text("Hardware RT: %s", state.renderer.hardwareRayTracingAvailable() ? "Available" : "Unavailable");

    ImGui::SeparatorText("Rendering");
    editorDebugViewCombo("Debug View", settings, changed);
    changed |= ImGui::SliderScalar("Max Bounces", ImGuiDataType_U32, &settings.maxBounces, &minBounces, &maxBounces);
    tooltip("Number of ray bounces. Higher is more accurate and slower; 4-8 for preview, 16 for final.");
    changed |= ImGui::SliderScalar("Environment Samples", ImGuiDataType_U32, &settings.environmentDirectSamples, &minEnvSamples, &maxEnvSamples);
    tooltip("Environment light samples per bounce. Higher values reduce fireflies.");
    changed |= ImGui::Checkbox("Path Tracing", &settings.pathTracingEnabled);
    changed |= ImGui::Checkbox("Camera Jitter", &settings.cameraJitterEnabled);
    tooltip("Halton sub-pixel jitter for temporal accumulation and motion-vector validation.");
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
    changed |= ImGui::SliderFloat("Render Resolution Scale", &settings.renderResolutionScale, 0.25f, 1.0f, "%.2f");

    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* toneMapperItems2[] = {"Linear", "Reinhard", "Reinhard White", "ACES", "PBR Neutral"};
        int toneMapperIndex2 = static_cast<int>(settings.toneMapper);
        if (toneMapperIndex2 < 0 || toneMapperIndex2 > 4) {
            toneMapperIndex2 = 3;
        }
        if (ImGui::Combo("Tone Mapper", &toneMapperIndex2, toneMapperItems2, 5)) {
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
            PhysicalCamera pc({settings.physicalAperture, settings.physicalShutterSeconds, settings.physicalIso, settings.physicalExposureCompensation});
            ImGui::Text("EV100: %.1f", pc.ev100());
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
        changed |= ImGui::Checkbox("Sunlight", &settings.sunlightEnabled);
        changed |= ImGui::SliderFloat("Sun Intensity", &settings.sunIntensity, 0.0f, 10.0f, "%.2f");
        changed |= ImGui::SliderFloat("Sun Elevation", &settings.sunElevation, -0.20f, 1.45f, "%.2f rad");
        tooltip("Analytical atmosphere sun angle. Low values validate sunset and horizon scattering.");
        changed |= ImGui::SliderFloat("Sun Size", &settings.sunAngularRadius, 0.0f, 0.08f, "%.4f");
        changed |= ImGui::SliderFloat("Sky Intensity", &settings.skyIntensity, 0.0f, 3.0f, "%.2f");
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
        changed |= ImGui::Checkbox("TAA", &settings.taaEnabled);
        tooltip("HDR temporal anti-aliasing pass after denoising and before tone mapping.");
        changed |= ImGui::SliderFloat("TAA Feedback", &settings.taaFeedback, 0.01f, 0.5f, "%.2f");
        tooltip("Lower values keep more history; higher values react faster to motion and lighting changes.");
    }

    if (ImGui::Button("Reset Accumulation")) {
        requests.resetAccumulation = AccumulationResetReason::Manual;
    }

    if (changed) {
        if (state.sceneDocument != nullptr) {
            RenderSettings& render = state.sceneDocument->renderSettings();
            Environment& environment = state.sceneDocument->environment();
            const bool environmentChanged =
                environment.enabled != settings.environmentEnabled ||
                std::abs(environment.intensity - settings.environmentIntensity) > 0.0001f ||
                std::abs(environment.rotation - settings.environmentRotation) > 0.0001f ||
                std::abs(environment.backgroundIntensity - settings.environmentBackgroundIntensity) > 0.0001f;
            const bool lightingChanged =
                render.sunlightEnabled != settings.sunlightEnabled ||
                render.directLightingEnabled != settings.directLightingEnabled ||
                render.environmentDirectSamples != settings.environmentDirectSamples ||
                render.restirMode != settings.restirMode ||
                std::abs(render.sunIntensity - settings.sunIntensity) > 0.0001f ||
                std::abs(render.skyIntensity - settings.skyIntensity) > 0.0001f ||
                std::abs(render.sunElevation - settings.sunElevation) > 0.0001f ||
                std::abs(render.sunAngularRadius - settings.sunAngularRadius) > 0.0001f;
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
            render.sunlightEnabled = settings.sunlightEnabled;
            render.sunIntensity = settings.sunIntensity;
            render.skyIntensity = settings.skyIntensity;
            render.sunElevation = settings.sunElevation;
            render.sunAngularRadius = settings.sunAngularRadius;
            render.indirectStrength = settings.indirectStrength;
            render.restirMode = settings.restirMode;
            render.denoiserEnabled = settings.denoiserEnabled;
            render.atrousIterations = settings.atrousIterations;
            render.denoiserStrength = settings.denoiserStrength;
            render.taaEnabled = settings.taaEnabled;
            render.taaFeedback = settings.taaFeedback;
            render.debugView = settings.debugView;
            render.resolutionScale = settings.renderResolutionScale;
            render.requestedBackend = settings.requestedBackend;
            render.accumulationLimit = settings.accumulationLimit;
            render.usePhysicalCamera = settings.usePhysicalCamera;
            render.physicalAperture = settings.physicalAperture;
            render.physicalShutterSeconds = settings.physicalShutterSeconds;
            render.physicalIso = settings.physicalIso;
            render.physicalExposureCompensation = settings.physicalExposureCompensation;
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
