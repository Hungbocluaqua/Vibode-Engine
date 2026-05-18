#pragma once

#include <cstdint>
#include <functional>
#include <limits>

namespace rtv {

struct EntityId {
    uint32_t index = std::numeric_limits<uint32_t>::max();
    uint32_t generation = 0;

    [[nodiscard]] bool valid() const { return index != std::numeric_limits<uint32_t>::max(); }

    friend bool operator==(EntityId lhs, EntityId rhs) {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }

    friend bool operator!=(EntityId lhs, EntityId rhs) {
        return !(lhs == rhs);
    }
};

[[nodiscard]] constexpr EntityId nullEntityId() {
    return {};
}

} // namespace rtv

template <>
struct std::hash<rtv::EntityId> {
    size_t operator()(rtv::EntityId id) const noexcept {
        return (static_cast<size_t>(id.generation) << 32u) ^ static_cast<size_t>(id.index);
    }
};
