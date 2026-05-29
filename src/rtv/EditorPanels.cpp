#include "rtv/EditorPanels.h"

#include "rtv/RendererDebug.h"

#include <imgui.h>

namespace rtv {

const std::array<RendererDebugView, 86>& editorDebugViews() {
    static constexpr std::array<RendererDebugView, 86> views = {
        RendererDebugView::Beauty,
        RendererDebugView::Variance,
        RendererDebugView::Normals,
        RendererDebugView::ReprojectionConfidence,
        RendererDebugView::DenoiserRejection,
        RendererDebugView::Depth,
        RendererDebugView::Roughness,
        RendererDebugView::DirectLighting,
        RendererDebugView::PathDirectDiffuse,
        RendererDebugView::PathDirectSpecular,
        RendererDebugView::IndirectLighting,
        RendererDebugView::PathIndirectDiffuse,
        RendererDebugView::PathIndirectSpecular,
        RendererDebugView::EmissiveContribution,
        RendererDebugView::EmissiveContinuation,
        RendererDebugView::EnvironmentContribution,
        RendererDebugView::InstanceId,
        RendererDebugView::MeshId,
        RendererDebugView::LightPdf,
        RendererDebugView::BsdfPdf,
        RendererDebugView::MisWeight,
        RendererDebugView::SunMisWeight,
        RendererDebugView::SunLightPdf,
        RendererDebugView::SunPreviousBsdfPdf,
        RendererDebugView::RisRawLightPdf,
        RendererDebugView::RisEffectiveLightPdf,
        RendererDebugView::RisPdfRatio,
        RendererDebugView::SampleDimension,
        RendererDebugView::SampleScramble,
        RendererDebugView::DirectSampleType,
        RendererDebugView::Albedo,
        RendererDebugView::MaterialOcclusion,
        RendererDebugView::PathDataAlbedo,
        RendererDebugView::PathDataMetrics,
        RendererDebugView::DenoiserKernelRadius,
        RendererDebugView::DenoiserHitDistance,
        RendererDebugView::DenoiserVirtualMotion,
        RendererDebugView::DenoiserDiffuseDebug,
        RendererDebugView::DenoiserSpecularDebug,
        RendererDebugView::DenoiserEmissiveClamp,
        RendererDebugView::DenoiserVarianceConfidence,
        RendererDebugView::DenoiserDiffuseChannelConfidence,
        RendererDebugView::DenoiserFrameBlend,
        RendererDebugView::DenoiserMaxHitDistanceDelta,
        RendererDebugView::DenoiserDiffuseOnScreen,
        RendererDebugView::DenoiserBaseDisocclusion,
        RendererDebugView::DenoiserSpecularChannelConfidence,
        RendererDebugView::DenoiserSpecularHistoryWeight,
        RendererDebugView::ClayMaterial,
        RendererDebugView::FirstBounceThroughput,
        RendererDebugView::SecondaryEnvironmentMiss,
        RendererDebugView::BounceCount,
        RendererDebugView::SecondaryEnvironmentRadiance,
        RendererDebugView::WhiteEnvironmentTransport,
        RendererDebugView::MotionVectors,
        RendererDebugView::AtmosphereSkyView,
        RendererDebugView::AtmosphereTransmittance,
        RendererDebugView::AtmosphereAerialPerspective,
        RendererDebugView::AtmosphereMultiScatter,
        RendererDebugView::TemporalReactiveMask,
        RendererDebugView::TemporalHistoryWeight,
        RendererDebugView::RestirReservoirAge,
        RendererDebugView::RestirReservoirConfidence,
        RendererDebugView::RestirReservoirM,
        RendererDebugView::RestirPairwiseMis,
        RendererDebugView::RestirGiValidity,
        RendererDebugView::RestirGiAge,
        RendererDebugView::RestirGiInitial,
        RendererDebugView::RestirGiTemporal,
        RendererDebugView::RestirGiSpatial,
        RendererDebugView::RestirGiFinal,
        RendererDebugView::RestirGiNormal,
        RendererDebugView::RestirGiHitDistance,
        RendererDebugView::DenoiserDirectDiffuseVariance,
        RendererDebugView::DenoiserDirectSpecularVariance,
        RendererDebugView::DenoiserIndirectDiffuseVariance,
        RendererDebugView::DenoiserIndirectSpecularVariance,
        RendererDebugView::DenoiserDiffuseHistoryLength,
        RendererDebugView::DenoiserSpecularHistoryLength,
        RendererDebugView::MomentUpdateValidity,
        RendererDebugView::MomentDisocclusionConfidence,
        RendererDebugView::MomentNormalCone,
        RendererDebugView::MomentDepthDelta,
        RendererDebugView::MomentHistoryKindValid,
        RendererDebugView::DenoiserDiffuseRawVariance,
        RendererDebugView::DenoiserSpecularRawVariance,
    };
    return views;
}

int editorDebugViewIndex(RendererDebugView view) {
    const auto& views = editorDebugViews();
    for (int i = 0; i < static_cast<int>(views.size()); ++i) {
        if (views[static_cast<size_t>(i)] == view) {
            return i;
        }
    }
    return 0;
}

void editorDebugViewCombo(const char* label, RendererSettings& settings, bool& changed) {
    auto selectable = [&](RendererDebugView view) {
        const bool selected = settings.debugView == view;
        if (ImGui::Selectable(rendererDebugViewName(view), selected)) {
            settings.debugView = view;
            changed = true;
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    };

    int selectedDebug = editorDebugViewIndex(settings.debugView);
    if (ImGui::BeginCombo(label, rendererDebugViewName(settings.debugView))) {
        (void)selectedDebug;
        ImGui::SeparatorText("Core");
        selectable(RendererDebugView::Beauty);
        selectable(RendererDebugView::Depth);
        selectable(RendererDebugView::Normals);
        selectable(RendererDebugView::MotionVectors);
        selectable(RendererDebugView::Variance);
        selectable(RendererDebugView::ReprojectionConfidence);
        selectable(RendererDebugView::DenoiserRejection);
        ImGui::SeparatorText("Material");
        selectable(RendererDebugView::Albedo);
        selectable(RendererDebugView::PathDataAlbedo);
        selectable(RendererDebugView::PathDataMetrics);
        selectable(RendererDebugView::DenoiserKernelRadius);
        selectable(RendererDebugView::DenoiserHitDistance);
        selectable(RendererDebugView::DenoiserVirtualMotion);
        selectable(RendererDebugView::DenoiserDiffuseDebug);
        selectable(RendererDebugView::DenoiserSpecularDebug);
        selectable(RendererDebugView::DenoiserEmissiveClamp);
        selectable(RendererDebugView::DenoiserVarianceConfidence);
        selectable(RendererDebugView::DenoiserDiffuseChannelConfidence);
        selectable(RendererDebugView::DenoiserFrameBlend);
        selectable(RendererDebugView::DenoiserMaxHitDistanceDelta);
        selectable(RendererDebugView::DenoiserDiffuseOnScreen);
        selectable(RendererDebugView::DenoiserBaseDisocclusion);
        selectable(RendererDebugView::DenoiserSpecularChannelConfidence);
        selectable(RendererDebugView::DenoiserSpecularHistoryWeight);
        selectable(RendererDebugView::DenoiserDirectDiffuseVariance);
        selectable(RendererDebugView::DenoiserDirectSpecularVariance);
        selectable(RendererDebugView::DenoiserIndirectDiffuseVariance);
        selectable(RendererDebugView::DenoiserIndirectSpecularVariance);
        selectable(RendererDebugView::DenoiserDiffuseHistoryLength);
        selectable(RendererDebugView::DenoiserSpecularHistoryLength);
        selectable(RendererDebugView::MomentUpdateValidity);
        selectable(RendererDebugView::MomentDisocclusionConfidence);
        selectable(RendererDebugView::MomentNormalCone);
        selectable(RendererDebugView::MomentDepthDelta);
        selectable(RendererDebugView::MomentHistoryKindValid);
        selectable(RendererDebugView::DenoiserDiffuseRawVariance);
        selectable(RendererDebugView::DenoiserSpecularRawVariance);
        selectable(RendererDebugView::Roughness);
        selectable(RendererDebugView::ClayMaterial);
        selectable(RendererDebugView::InstanceId);
        selectable(RendererDebugView::MeshId);
        ImGui::SeparatorText("Lighting");
        selectable(RendererDebugView::DirectLighting);
        selectable(RendererDebugView::PathDirectDiffuse);
        selectable(RendererDebugView::PathDirectSpecular);
        selectable(RendererDebugView::IndirectLighting);
        selectable(RendererDebugView::PathIndirectDiffuse);
        selectable(RendererDebugView::PathIndirectSpecular);
        selectable(RendererDebugView::EmissiveContribution);
        selectable(RendererDebugView::EmissiveContinuation);
        selectable(RendererDebugView::EnvironmentContribution);
        selectable(RendererDebugView::LightPdf);
        selectable(RendererDebugView::BsdfPdf);
        selectable(RendererDebugView::MisWeight);
        selectable(RendererDebugView::SunMisWeight);
        selectable(RendererDebugView::SunLightPdf);
        selectable(RendererDebugView::SunPreviousBsdfPdf);
        selectable(RendererDebugView::RisRawLightPdf);
        selectable(RendererDebugView::RisEffectiveLightPdf);
        selectable(RendererDebugView::RisPdfRatio);
        selectable(RendererDebugView::SampleDimension);
        selectable(RendererDebugView::SampleScramble);
        selectable(RendererDebugView::DirectSampleType);
        ImGui::SeparatorText("Transport");
        selectable(RendererDebugView::FirstBounceThroughput);
        selectable(RendererDebugView::SecondaryEnvironmentMiss);
        selectable(RendererDebugView::BounceCount);
        selectable(RendererDebugView::SecondaryEnvironmentRadiance);
        selectable(RendererDebugView::WhiteEnvironmentTransport);
        ImGui::SeparatorText("Atmosphere");
        selectable(RendererDebugView::AtmosphereSkyView);
        selectable(RendererDebugView::AtmosphereTransmittance);
        selectable(RendererDebugView::AtmosphereAerialPerspective);
        selectable(RendererDebugView::AtmosphereMultiScatter);
        ImGui::SeparatorText("Temporal / ReSTIR");
        selectable(RendererDebugView::TemporalReactiveMask);
        selectable(RendererDebugView::TemporalHistoryWeight);
        selectable(RendererDebugView::RestirReservoirAge);
        selectable(RendererDebugView::RestirReservoirConfidence);
        selectable(RendererDebugView::RestirReservoirM);
        selectable(RendererDebugView::RestirPairwiseMis);
        selectable(RendererDebugView::RestirGiValidity);
        selectable(RendererDebugView::RestirGiAge);
        selectable(RendererDebugView::RestirGiInitial);
        selectable(RendererDebugView::RestirGiTemporal);
        selectable(RendererDebugView::RestirGiSpatial);
        selectable(RendererDebugView::RestirGiFinal);
        selectable(RendererDebugView::RestirGiNormal);
        selectable(RendererDebugView::RestirGiHitDistance);
        ImGui::EndCombo();
    }
}

void requestSettings(EditorRequests& requests, const RendererSettings& settings) {
    requests.settings = settings;
}

} // namespace rtv
