#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/SceneComponents.h"
#include "rtv/SceneDocument.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

class AssetManager;

struct GpuSkinningInstancePlan {
    uint32_t nodeIndex = UINT32_MAX;
    uint32_t meshHandleIndex = UINT32_MAX;
    int32_t skinIndex = -1;
    uint32_t jointMatrixOffset = 0;
    uint32_t jointMatrixCount = 0;
    uint64_t jointUploadByteOffset = 0;
    uint64_t jointUploadBytes = 0;
    uint32_t sourceVertexOffset = 0;
    uint32_t currentVertexOffset = 0;
    uint32_t previousVertexOffset = 0;
    uint32_t morphDeltaOffset = 0;
    uint32_t morphDeltaCount = 0;
    float morphWeight = 0.0f;
    uint32_t vertexCount = 0;
    bool morphBeforeSkinning = false;
    bool cpuFallbackActive = true;
};

struct AnimatedGeometryStats {
    uint32_t meshInstanceCount = 0;
    uint32_t staticMeshInstanceCount = 0;
    uint32_t transformOnlyCandidateInstanceCount = 0;
    uint32_t deformingInstanceCount = 0;
    uint32_t morphDeformingInstanceCount = 0;
    uint32_t skinnedDeformingInstanceCount = 0;
    uint32_t combinedMorphSkinnedInstanceCount = 0;
    uint32_t runtimeMeshInstanceCount = 0;
    uint32_t materialOverrideRuntimeMeshInstanceCount = 0;
    uint32_t missingMeshInstanceCount = 0;
    uint32_t totalPrimitiveCount = 0;
    uint32_t totalTriangleCount = 0;
    uint32_t gpuSkinningCandidateInstanceCount = 0;
    uint32_t gpuSkinningMorphPreSkinInstanceCount = 0;
    uint32_t gpuSkinningJointMatrixCount = 0;
    uint32_t gpuSkinningCurrentVertexCount = 0;
    uint32_t gpuSkinningPreviousVertexCount = 0;
    uint64_t gpuSkinningSourceVertexUploadBytes = 0;
    uint64_t gpuSkinningMorphDeltaUploadBytes = 0;
    uint64_t gpuSkinningPreviousJointUploadBytes = 0;
    uint64_t gpuSkinningCurrentVertexBufferBytes = 0;
    uint64_t gpuSkinningPreviousVertexBufferBytes = 0;
    uint64_t gpuSkinningJointUploadBytes = 0;
    uint32_t gpuSkinningCpuFallbackInstanceCount = 0;
    uint32_t gpuSkinningDispatchRecordCount = 0;
    std::string transformOnlyPolicy = "tlas_refit_candidate";
    std::string deformingPolicy = "blas_rebuild_required_currently_sync";
    std::string morphPolicy = "cpu_baked_runtime_mesh_current_frame";
    std::string skinningPolicy = "cpu_baked_runtime_mesh_current_frame";
    std::string gpuSkinningDataPolicy = "plan_only_cpu_runtime_mesh_fallback";
    std::string gpuSkinningJointUploadPolicy = "budgeted_joint_upload_required_not_recorded";
    std::string gpuSkinningBufferPolicy = "current_previous_skinned_vertex_buffers_required_not_allocated";
    std::string gpuSkinningComputeShader = "shaders/gpu_skinning.comp";
};

struct SceneGpuBuildResult {
    SceneUpdateKind updateKind = SceneUpdateKind::None;
    SceneAsset sceneAsset{};
    std::vector<EntityId> instanceEntities;
    AnimatedGeometryStats animatedGeometry{};
    std::vector<GpuSkinningInstancePlan> gpuSkinningPlan;
    std::vector<glm::mat4> gpuSkinningJointMatrices;
    std::vector<glm::mat4> gpuSkinningPreviousJointMatrices;
    std::vector<PathTracerRenderer::GpuSkinningSourceVertex> gpuSkinningSourceVertices;
    std::vector<GpuLocalVertex> gpuSkinningMorphDeltas;
    RendererSettings rendererSettings{};
    AccumulationResetReason accumulationReason = AccumulationResetReason::Manual;
    bool requiresRendererRebuild = false;
};

class SceneToGpuSceneBuilder {
public:
    [[nodiscard]] SceneGpuBuildResult build(const SceneDocument& document, AssetManager* assets, const RendererSettings& currentSettings) const;
    [[nodiscard]] static AccumulationResetReason accumulationReasonFor(SceneUpdateKind kind);
};

} // namespace rtv
