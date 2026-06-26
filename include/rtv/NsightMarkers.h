#pragma once

namespace rtv {

void setNsightMarkersEnabled(bool enabled);
[[nodiscard]] bool nsightMarkersEnabled();
void beginNsightRange(const char* name);
void endNsightRange();
void markNsightEvent(const char* name);

class ScopedNsightRange {
public:
    explicit ScopedNsightRange(const char* name);
    ~ScopedNsightRange();

    ScopedNsightRange(const ScopedNsightRange&) = delete;
    ScopedNsightRange& operator=(const ScopedNsightRange&) = delete;

private:
    bool active_ = false;
};

} // namespace rtv
