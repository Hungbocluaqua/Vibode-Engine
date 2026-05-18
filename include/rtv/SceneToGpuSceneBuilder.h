#pragma once

#include "rtv/MeshAsset.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/SceneComponents.h"
#include "rtv/SceneDocument.h"

#include <vector>

namespace rtv {

class AssetManager;

struct SceneGpuBuildResult {
    SceneUpdateKind updateKind = SceneUpdateKind::None;
    SceneAsset sceneAsset{};
    std::vector<EntityId> instanceEntities;
    RendererSettings rendererSettings{};
    AccumulationResetReason accumulationReason = AccumulationResetReason::Manual;
    bool requiresRendererRebuild = false;
};

class SceneToGpuSceneBuilder {
public:
    [[nodiscard]] SceneGpuBuildResult build(const SceneDocument& document, const AssetManager* assets, const RendererSettings& currentSettings) const;
    [[nodiscard]] static AccumulationResetReason accumulationReasonFor(SceneUpdateKind kind);
};

} // namespace rtv
