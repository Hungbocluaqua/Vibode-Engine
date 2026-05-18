#pragma once

#include "rtv/BvhBuilder.h"

namespace rtv {

void buildSahBinaryBvh(BvhBuildResult& result);
void buildMortonBinaryBvh(BvhBuildResult& result);

} // namespace rtv
