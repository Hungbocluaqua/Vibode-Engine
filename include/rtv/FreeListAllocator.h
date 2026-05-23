#pragma once

#include <cstdint>
#include <vector>

namespace rtv {

template <typename IndexType = uint32_t>
class FreeListAllocator {
public:
    explicit FreeListAllocator(IndexType capacity)
        : capacity_(capacity) {
        freeIndices_.reserve(static_cast<size_t>(capacity));
        for (IndexType i = 0; i < capacity; ++i) {
            freeIndices_.push_back(capacity - 1 - i);
        }
    }

    IndexType allocate() {
        if (!freeIndices_.empty()) {
            const IndexType index = freeIndices_.back();
            freeIndices_.pop_back();
            ++allocatedCount_;
            return index;
        }
        if (nextIndex_ < capacity_) {
            const IndexType index = nextIndex_++;
            ++allocatedCount_;
            return index;
        }
        return UINT32_MAX;
    }

    void free(IndexType index) {
        if (index < capacity_) {
            freeIndices_.push_back(index);
            --allocatedCount_;
        }
    }

    void clear() {
        freeIndices_.clear();
        freeIndices_.reserve(static_cast<size_t>(capacity_));
        for (IndexType i = 0; i < capacity_; ++i) {
            freeIndices_.push_back(capacity_ - 1 - i);
        }
        nextIndex_ = 0;
        allocatedCount_ = 0;
    }

    [[nodiscard]] IndexType allocatedCount() const { return allocatedCount_; }
    [[nodiscard]] IndexType freeCount() const { return static_cast<IndexType>(freeIndices_.size()); }
    [[nodiscard]] IndexType capacity() const { return capacity_; }
    [[nodiscard]] float fragmentationRatio() const {
        if (capacity_ == 0) return 0.0f;
        return static_cast<float>(freeIndices_.size()) / static_cast<float>(capacity_);
    }

private:
    IndexType capacity_ = 0;
    IndexType nextIndex_ = 0;
    IndexType allocatedCount_ = 0;
    std::vector<IndexType> freeIndices_;
};

} // namespace rtv
