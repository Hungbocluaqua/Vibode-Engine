#include "rtv/NsightMarkers.h"

#include <atomic>

#if defined(RTV_HAS_NVTX)
#include <nvtx3/nvToolsExt.h>
#endif

namespace rtv {

namespace {

std::atomic_bool gMarkersEnabled{true};

#if defined(RTV_HAS_NVTX)
nvtxDomainHandle_t markerDomain() {
    static nvtxDomainHandle_t domain = nvtxDomainCreateA("rtvulkan.renderer");
    return domain;
}

nvtxEventAttributes_t markerAttributes(const char* name) {
    nvtxEventAttributes_t attributes{};
    attributes.version = NVTX_VERSION;
    attributes.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
    attributes.colorType = NVTX_COLOR_ARGB;
    attributes.color = 0xFF3687E0u;
    attributes.messageType = NVTX_MESSAGE_TYPE_ASCII;
    attributes.message.ascii = name;
    return attributes;
}
#endif

} // namespace

void setNsightMarkersEnabled(bool enabled) {
    gMarkersEnabled.store(enabled, std::memory_order_relaxed);
}

bool nsightMarkersEnabled() {
    return gMarkersEnabled.load(std::memory_order_relaxed);
}

void beginNsightRange(const char* name) {
#if defined(RTV_HAS_NVTX)
    if (!nsightMarkersEnabled() || name == nullptr || name[0] == '\0') {
        return;
    }
    const nvtxEventAttributes_t attributes = markerAttributes(name);
    nvtxDomainRangePushEx(markerDomain(), &attributes);
#else
    (void)name;
#endif
}

void endNsightRange() {
#if defined(RTV_HAS_NVTX)
    if (nsightMarkersEnabled()) {
        nvtxDomainRangePop(markerDomain());
    }
#endif
}

void markNsightEvent(const char* name) {
#if defined(RTV_HAS_NVTX)
    if (!nsightMarkersEnabled() || name == nullptr || name[0] == '\0') {
        return;
    }
    const nvtxEventAttributes_t attributes = markerAttributes(name);
    nvtxDomainMarkEx(markerDomain(), &attributes);
#else
    (void)name;
#endif
}

ScopedNsightRange::ScopedNsightRange(const char* name)
    : active_(nsightMarkersEnabled() && name != nullptr && name[0] != '\0') {
    if (active_) {
        beginNsightRange(name);
    }
}

ScopedNsightRange::~ScopedNsightRange() {
    if (active_) {
        endNsightRange();
    }
}

} // namespace rtv
