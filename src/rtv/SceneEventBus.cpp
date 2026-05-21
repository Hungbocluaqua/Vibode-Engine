#include "rtv/SceneEventBus.h"

#include <utility>

namespace rtv {

void SceneEventBus::subscribe(Handler handler) {
    handlers_.push_back(std::move(handler));
}

void SceneEventBus::publish(const SceneEvent& event) const {
    for (const Handler& handler : handlers_) {
        handler(event);
    }
}

void SceneEventBus::clear() {
    handlers_.clear();
}

} // namespace rtv
