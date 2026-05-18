#include "rtv/GpuValidation.h"

#include <cstddef>
#include <sstream>

namespace rtv {

void RendererValidationLog::recordBarrier(std::string label, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    std::ostringstream stream;
    stream << label << ": stage " << srcStage << "/" << dstStage << " access " << srcAccess << "/" << dstAccess;
    barrierEvents_.push_back(stream.str());
}

void RendererValidationLog::recordAccumulationInvalidation(std::string reason, uint64_t frame) {
    invalidations_.push_back({std::move(reason), frame});
    if (invalidations_.size() > 64) {
        invalidations_.erase(invalidations_.begin(), invalidations_.begin() + static_cast<std::ptrdiff_t>(invalidations_.size() - 64));
    }
}

void RendererValidationLog::beginFrame(uint64_t frame) {
    currentFrame_ = frame;
    passEvents_.clear();
    barrierEvents_.clear();
}

void RendererValidationLog::recordPass(std::string label) {
    std::ostringstream stream;
    stream << "frame " << currentFrame_ << ": " << label;
    passEvents_.push_back(stream.str());
}

} // namespace rtv
