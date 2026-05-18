#pragma once

#include "rtv/EntityId.h"

#include <cstdint>

namespace rtv {

enum class EditorSelectionKind : uint32_t {
    None,
    Object,
    Material,
    Asset,
    Camera,
    Light,
};

struct EditorSelectionId {
    EditorSelectionKind kind = EditorSelectionKind::None;
    uint32_t index = UINT32_MAX;
    EntityId entity{};

    [[nodiscard]] bool valid() const {
        return kind != EditorSelectionKind::None && (index != UINT32_MAX || entity.valid());
    }
};

class EditorSelection {
public:
    void clear();
    void selectObject(uint32_t id);
    void selectEntity(EntityId id);
    void selectMaterial(uint32_t id);
    void selectAsset(uint32_t id);
    void selectCamera(uint32_t id = 0);
    void selectLight(uint32_t id);

    [[nodiscard]] EditorSelectionId current() const { return current_; }
    [[nodiscard]] bool is(EditorSelectionKind kind) const { return current_.kind == kind; }
    [[nodiscard]] uint32_t index() const { return current_.index; }
    [[nodiscard]] EntityId entityId() const { return current_.entity; }

private:
    EditorSelectionId current_{};
};

} // namespace rtv
