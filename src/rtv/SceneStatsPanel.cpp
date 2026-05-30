#include "rtv/SceneStatsPanel.h"

#include "rtv/PathTracerRenderer.h"

#include <imgui.h>

namespace rtv {

void SceneStatsPanel::draw(const EditorRuntimeState& state) {
    if (!ImGui::Begin("Scene Stats")) {
        ImGui::End();
        return;
    }

    const MeshParamsUniform& meshParams = state.renderer.scene().meshParams();
    const RayTracingRendererStats rt = state.renderer.rayTracingStats();

    ImGui::SeparatorText("Scene");
    if (state.sceneDocument != nullptr) {
        ImGui::Text("Entities: %zu", state.sceneDocument->registry().liveCount());
    }
    ImGui::Text("Meshes: %u", meshParams.meshCount);
    ImGui::Text("Primitives: %u", meshParams.primitiveCount);
    ImGui::Text("Triangles: %u", meshParams.triangleCount);
    ImGui::Text("Vertices: %u", meshParams.vertexCount);
    ImGui::Text("Instances: %u", meshParams.instanceCount);
    ImGui::Text("Materials: %u", meshParams.materialCount);
    ImGui::Text("Textures: %zu", state.renderer.scene().materialTextureDescriptors().size());

    ImGui::SeparatorText("Geometry");
    ImGui::Text("Local vertices: %u", meshParams.localVertexCount);
    ImGui::Text("Local triangles: %u", meshParams.localTriangleCount);
    ImGui::Text("Spheres: %u", meshParams.sphereCount);
    ImGui::Text("Lights: %u", meshParams.lightCount);
    ImGui::Text("Emissive area: %.3f", meshParams.emissiveTotalArea);

    ImGui::SeparatorText("BVH");
    ImGui::Text("BVH nodes: %u", meshParams.bvhNodeCount);
    ImGui::Text("Local BVH nodes: %u", meshParams.localBvhNodeCount);
    ImGui::Text("TLAS nodes: %u", meshParams.tlasNodeCount);
    ImGui::Text("TLAS indices: %u", meshParams.tlasInstanceIndexCount);

    if (rt.active) {
        ImGui::SeparatorText("Hardware RT");
        ImGui::Text("BLAS: %u", rt.blasCount);
        ImGui::Text("Instances: %u", rt.instanceCount);
        ImGui::Text("Opaque primitives: %u", rt.geometry.opaquePrimitiveCount);
        ImGui::Text("Alpha-tested primitives: %u", rt.geometry.alphaTestedPrimitiveCount);
        ImGui::Text("Blended primitives: %u", rt.geometry.blendedPrimitiveCount);
        constexpr double mb = 1024.0 * 1024.0;
        ImGui::Text("AS memory: %.2f MB", static_cast<double>(rt.accelerationStructureBytes) / mb);
        ImGui::Text("SBT: %.2f KB", static_cast<double>(rt.sbtBytes) / 1024.0);
        ImGui::Text("TLAS refit: %.3f ms", rt.lastTlasRefitMs);
    }

    ImGui::End();
}

} // namespace rtv
