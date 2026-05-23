#pragma once

#include "rtv/Entity.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

class SceneRegistry {
public:
    [[nodiscard]] EntityId createEntity(std::string name = "Entity");
    bool destroyEntity(EntityId id);
    bool renameEntity(EntityId id, std::string name);

    [[nodiscard]] Entity* entity(EntityId id);
    [[nodiscard]] const Entity* entity(EntityId id) const;
    [[nodiscard]] Entity* entityByUuid(uint64_t uuid);
    [[nodiscard]] const Entity* entityByUuid(uint64_t uuid) const;
    [[nodiscard]] bool contains(EntityId id) const { return entity(id) != nullptr; }

    [[nodiscard]] std::vector<Entity*> entities();
    [[nodiscard]] std::vector<const Entity*> entities() const;

    MeshRenderer& addMeshRenderer(EntityId id, MeshRenderer renderer = {});
    Light& addLight(EntityId id, Light light = {});
    Camera& addCamera(EntityId id, Camera camera = {});
    bool removeMeshRenderer(EntityId id);
    bool removeLight(EntityId id);
    bool removeCamera(EntityId id);

    [[nodiscard]] MeshRenderer* meshRenderer(EntityId id);
    [[nodiscard]] Light* light(EntityId id);
    [[nodiscard]] Camera* camera(EntityId id);
    [[nodiscard]] const MeshRenderer* meshRenderer(EntityId id) const;
    [[nodiscard]] const Light* light(EntityId id) const;
    [[nodiscard]] const Camera* camera(EntityId id) const;

    [[nodiscard]] bool effectiveVisible(EntityId id) const;

    void markDirty(SceneUpdateKind kind);
    void clearDirty();
    [[nodiscard]] bool dirty() const { return pendingUpdate_ != SceneUpdateKind::None; }
    [[nodiscard]] SceneUpdateKind pendingUpdate() const { return pendingUpdate_; }
    [[nodiscard]] size_t liveCount() const { return liveCount_; }

    void ensureUuidCounter(uint64_t minUuid);

private:
    struct Slot {
        uint32_t generation = 1;
        std::optional<Entity> entity;
    };

    [[nodiscard]] bool validIndex(EntityId id) const;
    static SceneUpdateKind combine(SceneUpdateKind current, SceneUpdateKind next);

    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
    std::unordered_map<uint64_t, EntityId> uuidIndex_;
    size_t liveCount_ = 0;
    uint64_t uuidCounter_ = 1;
    SceneUpdateKind pendingUpdate_ = SceneUpdateKind::None;
};

} // namespace rtv
