#include "rtv/EditorSelection.h"

#include <algorithm>

namespace rtv {

void EditorSelection::clear() {
    current_ = {};
    multiSelected_.clear();
    lastClickedId_ = {};
    pickPending_ = false;
}

void EditorSelection::selectObject(uint32_t id) {
    clear();
    current_ = {EditorSelectionKind::Object, id};
}

void EditorSelection::selectEntity(EntityId id) {
    clear();
    multiSelected_.clear();
    lastClickedId_ = id;
    current_ = EditorSelectionId{.kind = EditorSelectionKind::Object, .index = id.index, .entity = id};
    multiSelected_.push_back(id);
}

void EditorSelection::toggleEntity(EntityId id) {
    current_ = EditorSelectionId{.kind = EditorSelectionKind::Object, .index = id.index, .entity = id};
    auto it = std::find(multiSelected_.begin(), multiSelected_.end(), id);
    if (it != multiSelected_.end()) {
        multiSelected_.erase(it);
        if (multiSelected_.empty()) {
            current_ = {};
        } else {
            const EntityId first = multiSelected_.front();
            current_ = EditorSelectionId{.kind = EditorSelectionKind::Object, .index = first.index, .entity = first};
        }
    } else {
        multiSelected_.push_back(id);
    }
    lastClickedId_ = id;
}

void EditorSelection::selectRange(EntityId from, EntityId to) {
    current_ = EditorSelectionId{.kind = EditorSelectionKind::Object, .index = to.index, .entity = to};
    if (!from.valid()) {
        multiSelected_.clear();
        multiSelected_.push_back(to);
    } else {
        if (std::find(multiSelected_.begin(), multiSelected_.end(), from) == multiSelected_.end()) {
            multiSelected_.push_back(from);
        }
        if (std::find(multiSelected_.begin(), multiSelected_.end(), to) == multiSelected_.end()) {
            multiSelected_.push_back(to);
        }
    }
    lastClickedId_ = to;
}

void EditorSelection::selectRangeFromFlattenedList(const std::vector<EntityId>& flattenedEntities, EntityId to) {
    if (!lastClickedId_.valid()) {
        selectEntity(to);
        return;
    }
    current_ = EditorSelectionId{.kind = EditorSelectionKind::Object, .index = to.index, .entity = to};
    multiSelected_.clear();
    bool inRange = false;
    for (const EntityId& eid : flattenedEntities) {
        if (eid == lastClickedId_ || eid == to) {
            inRange = !inRange;
            multiSelected_.push_back(eid);
        } else if (inRange) {
            multiSelected_.push_back(eid);
        }
    }
    if (std::find(multiSelected_.begin(), multiSelected_.end(), lastClickedId_) == multiSelected_.end()) {
        multiSelected_.push_back(lastClickedId_);
    }
    if (std::find(multiSelected_.begin(), multiSelected_.end(), to) == multiSelected_.end()) {
        multiSelected_.push_back(to);
    }
    lastClickedId_ = to;
}

void EditorSelection::selectMaterial(uint32_t id) {
    clear();
    multiSelected_.clear();
    current_ = {EditorSelectionKind::Material, id};
}

void EditorSelection::selectAsset(uint32_t id) {
    clear();
    multiSelected_.clear();
    current_ = {EditorSelectionKind::Asset, id};
}

void EditorSelection::selectCamera(uint32_t id) {
    clear();
    multiSelected_.clear();
    current_ = {EditorSelectionKind::Camera, id};
}

void EditorSelection::selectLight(uint32_t id) {
    clear();
    multiSelected_.clear();
    current_ = {EditorSelectionKind::Light, id};
}

} // namespace rtv
