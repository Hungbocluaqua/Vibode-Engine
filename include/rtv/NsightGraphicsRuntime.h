#pragma once

#include <Volk/volk.h>

namespace rtv {

bool initializeNsightGraphicsRuntime();
void emitNsightFrameBoundary(VkQueue queue, VkImage outputImage);
[[nodiscard]] bool nsightGraphicsRuntimeActive();

} // namespace rtv
