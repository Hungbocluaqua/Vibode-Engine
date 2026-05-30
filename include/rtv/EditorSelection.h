#pragma once

#include "rtv/EntityId.h"

#include <cstdint>
#include <vector>

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

    void toggleEntity(EntityId id);
    void selectRange(EntityId from, EntityId to);
    void selectRangeFromFlattenedList(const std::vector<EntityId>& flattenedEntities, EntityId to);

    [[nodiscard]] EntityId lastClickedId() const { return lastClickedId_; }
    void setLastClickedId(EntityId id) { lastClickedId_ = id; }
    void setPickPending(bool pending) { pickPending_ = pending; }
    [[nodiscard]] bool pickPending() const { return pickPending_; }

    [[nodiscard]] bool isSelected(EntityId id) const;
    [[nodiscard]] size_t selectionCount() const { return multiSelected_.empty() ? 0 : multiSelected_.size(); }

    [[nodiscard]] EditorSelectionId current() const { return current_; }
    [[nodiscard]] bool is(EditorSelectionKind kind) const { return current_.kind == kind; }
    [[nodiscard]] uint32_t index() const { return current_.index; }
    [[nodiscard]] EntityId entityId() const { return current_.entity; }
    [[nodiscard]] const std::vector<EntityId>& selectedEntities() const { return multiSelected_; }

private:
    EditorSelectionId current_{};
    std::vector<EntityId> multiSelected_;
    EntityId lastClickedId_{};
    bool pickPending_ = false;
};

} // namespace rtv
