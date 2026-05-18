#pragma once

#include <Volk/volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

struct AccumulationInvalidationEvent {
    std::string reason;
    uint64_t frame = 0;
};

class RendererValidationLog {
public:
    void recordBarrier(std::string label, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
    void recordAccumulationInvalidation(std::string reason, uint64_t frame);
    void beginFrame(uint64_t frame);
    void recordPass(std::string label);

    [[nodiscard]] const std::vector<std::string>& barrierEvents() const { return barrierEvents_; }
    [[nodiscard]] const std::vector<AccumulationInvalidationEvent>& invalidations() const { return invalidations_; }
    [[nodiscard]] const std::vector<std::string>& passEvents() const { return passEvents_; }

private:
    std::vector<std::string> barrierEvents_;
    std::vector<AccumulationInvalidationEvent> invalidations_;
    std::vector<std::string> passEvents_;
    uint64_t currentFrame_ = 0;
};

} // namespace rtv
