#include "rtv/SceneRegistry.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rtv {

EntityId SceneRegistry::createEntity(std::string name) {
    uint32_t index = 0;
    if (!freeList_.empty()) {
        index = freeList_.back();
        freeList_.pop_back();
    } else {
        index = static_cast<uint32_t>(slots_.size());
        slots_.push_back(Slot{});
    }

    Slot& slot = slots_[index];
    Entity entity;
    entity.id = EntityId{index, slot.generation};
    entity.name = std::move(name);
    slot.entity = std::move(entity);
    ++liveCount_;
    markDirty(SceneUpdateKind::TopologyChanged);
    return slot.entity->id;
}

bool SceneRegistry::destroyEntity(EntityId id) {
    if (!validIndex(id) || !slots_[id.index].entity.has_value()) {
        return false;
    }

    Entity& removed = *slots_[id.index].entity;
    if (Entity* parent = entity(removed.parent)) {
        parent->children.erase(
            std::remove(parent->children.begin(), parent->children.end(), id),
            parent->children.end());
    }
    for (EntityId childId : removed.children) {
        if (Entity* child = entity(childId)) {
            child->parent = {};
        }
    }
    for (Slot& slot : slots_) {
        if (!slot.entity.has_value()) {
            continue;
        }
        slot.entity->children.erase(
            std::remove(slot.entity->children.begin(), slot.entity->children.end(), id),
            slot.entity->children.end());
        if (slot.entity->parent == id) {
            slot.entity->parent = {};
        }
    }

    slots_[id.index].entity.reset();
    ++slots_[id.index].generation;
    freeList_.push_back(id.index);
    --liveCount_;
    markDirty(SceneUpdateKind::TopologyChanged);
    return true;
}

bool SceneRegistry::renameEntity(EntityId id, std::string name) {
    Entity* item = entity(id);
    if (item == nullptr) {
        return false;
    }
    if (item->name == name) {
        return false;
    }
    item->name = std::move(name);
    markDirty(SceneUpdateKind::None);
    return true;
}

Entity* SceneRegistry::entity(EntityId id) {
    if (!validIndex(id)) {
        return nullptr;
    }
    return slots_[id.index].entity ? &*slots_[id.index].entity : nullptr;
}

const Entity* SceneRegistry::entity(EntityId id) const {
    if (!validIndex(id)) {
        return nullptr;
    }
    return slots_[id.index].entity ? &*slots_[id.index].entity : nullptr;
}

std::vector<Entity*> SceneRegistry::entities() {
    std::vector<Entity*> result;
    result.reserve(liveCount_);
    for (Slot& slot : slots_) {
        if (slot.entity.has_value()) {
            result.push_back(&*slot.entity);
        }
    }
    return result;
}

std::vector<const Entity*> SceneRegistry::entities() const {
    std::vector<const Entity*> result;
    result.reserve(liveCount_);
    for (const Slot& slot : slots_) {
        if (slot.entity.has_value()) {
            result.push_back(&*slot.entity);
        }
    }
    return result;
}

MeshRenderer& SceneRegistry::addMeshRenderer(EntityId id, MeshRenderer renderer) {
    Entity* item = entity(id);
    if (item == nullptr) {
        throw std::runtime_error("Cannot add MeshRenderer to missing entity");
    }
    item->meshRenderer = std::move(renderer);
    markDirty(SceneUpdateKind::TopologyChanged);
    return *item->meshRenderer;
}

Light& SceneRegistry::addLight(EntityId id, Light light) {
    Entity* item = entity(id);
    if (item == nullptr) {
        throw std::runtime_error("Cannot add Light to missing entity");
    }
    item->light = light;
    markDirty(SceneUpdateKind::LightOnly);
    return *item->light;
}

Camera& SceneRegistry::addCamera(EntityId id, Camera camera) {
    Entity* item = entity(id);
    if (item == nullptr) {
        throw std::runtime_error("Cannot add Camera to missing entity");
    }
    item->camera = camera;
    markDirty(SceneUpdateKind::CameraOnly);
    return *item->camera;
}

bool SceneRegistry::removeMeshRenderer(EntityId id) {
    Entity* item = entity(id);
    if (item == nullptr || !item->meshRenderer.has_value()) {
        return false;
    }
    item->meshRenderer.reset();
    markDirty(SceneUpdateKind::TopologyChanged);
    return true;
}

bool SceneRegistry::removeLight(EntityId id) {
    Entity* item = entity(id);
    if (item == nullptr || !item->light.has_value()) {
        return false;
    }
    item->light.reset();
    markDirty(SceneUpdateKind::LightOnly);
    return true;
}

bool SceneRegistry::removeCamera(EntityId id) {
    Entity* item = entity(id);
    if (item == nullptr || !item->camera.has_value()) {
        return false;
    }
    item->camera.reset();
    markDirty(SceneUpdateKind::CameraOnly);
    return true;
}

MeshRenderer* SceneRegistry::meshRenderer(EntityId id) {
    Entity* item = entity(id);
    return item != nullptr && item->meshRenderer.has_value() ? &*item->meshRenderer : nullptr;
}

Light* SceneRegistry::light(EntityId id) {
    Entity* item = entity(id);
    return item != nullptr && item->light.has_value() ? &*item->light : nullptr;
}

Camera* SceneRegistry::camera(EntityId id) {
    Entity* item = entity(id);
    return item != nullptr && item->camera.has_value() ? &*item->camera : nullptr;
}

const MeshRenderer* SceneRegistry::meshRenderer(EntityId id) const {
    const Entity* item = entity(id);
    return item != nullptr && item->meshRenderer.has_value() ? &*item->meshRenderer : nullptr;
}

const Light* SceneRegistry::light(EntityId id) const {
    const Entity* item = entity(id);
    return item != nullptr && item->light.has_value() ? &*item->light : nullptr;
}

const Camera* SceneRegistry::camera(EntityId id) const {
    const Entity* item = entity(id);
    return item != nullptr && item->camera.has_value() ? &*item->camera : nullptr;
}

void SceneRegistry::markDirty(SceneUpdateKind kind) {
    pendingUpdate_ = combine(pendingUpdate_, kind);
}

void SceneRegistry::clearDirty() {
    pendingUpdate_ = SceneUpdateKind::None;
    for (Slot& slot : slots_) {
        if (slot.entity.has_value()) {
            slot.entity->transform.markClean();
        }
    }
}

bool SceneRegistry::validIndex(EntityId id) const {
    return id.valid() &&
        id.index < slots_.size() &&
        slots_[id.index].generation == id.generation;
}

SceneUpdateKind SceneRegistry::combine(SceneUpdateKind current, SceneUpdateKind next) {
    if (next == SceneUpdateKind::None) {
        return current;
    }
    if (current == SceneUpdateKind::None) {
        return next;
    }
    if (current == next) {
        return current;
    }
    if (current == SceneUpdateKind::RendererSettingsOnly) {
        return next;
    }
    if (next == SceneUpdateKind::RendererSettingsOnly) {
        return current;
    }
    if (current == SceneUpdateKind::TopologyChanged || next == SceneUpdateKind::TopologyChanged) {
        return SceneUpdateKind::TopologyChanged;
    }
    return SceneUpdateKind::TopologyChanged;
}

} // namespace rtv
