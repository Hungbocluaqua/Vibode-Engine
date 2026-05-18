#include "rtv/EditorSelection.h"

namespace rtv {

void EditorSelection::clear() {
    current_ = {};
}

void EditorSelection::selectObject(uint32_t id) {
    current_ = {EditorSelectionKind::Object, id};
}

void EditorSelection::selectEntity(EntityId id) {
    current_ = EditorSelectionId{.kind = EditorSelectionKind::Object, .index = id.index, .entity = id};
}

void EditorSelection::selectMaterial(uint32_t id) {
    current_ = {EditorSelectionKind::Material, id};
}

void EditorSelection::selectAsset(uint32_t id) {
    current_ = {EditorSelectionKind::Asset, id};
}

void EditorSelection::selectCamera(uint32_t id) {
    current_ = {EditorSelectionKind::Camera, id};
}

void EditorSelection::selectLight(uint32_t id) {
    current_ = {EditorSelectionKind::Light, id};
}

} // namespace rtv
