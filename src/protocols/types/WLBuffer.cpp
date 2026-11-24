#include "WLBuffer.hpp"
#include "Buffer.hpp"
#include "../core/Compositor.hpp"
#include "../DRMSyncobj.hpp"
#include "../../helpers/sync/SyncTimeline.hpp"
#include <xf86drm.h>

CWLBufferResource::CWLBufferResource(WP<CWlBuffer> resource_) : m_resource(resource_.lock()) {
    if UNLIKELY (!good())
        return;

    m_resource->setOnDestroy([this](CWlBuffer* r) {
        if (const auto buffer = m_buffer.lock())
            buffer->events.destroy.emit();
    });
    m_resource->setDestroy([](CWlBuffer* r) { wl_resource_destroy(r->resource()); });

    m_resource->setData(this);
}

bool CWLBufferResource::good() {
    return m_resource->resource();
}

void CWLBufferResource::sendRelease() {
    m_resource->sendRelease();
}

wl_resource* CWLBufferResource::getResource() {
    return m_resource->resource();
}

SP<CWLBufferResource> CWLBufferResource::fromResource(wl_resource* res) {
    auto data = sc<CWLBufferResource*>(sc<CWlBuffer*>(wl_resource_get_user_data(res))->data());
    return data ? data->m_self.lock() : nullptr;
}

SP<CWLBufferResource> CWLBufferResource::create(WP<CWlBuffer> resource) {
    auto p    = SP<CWLBufferResource>(new CWLBufferResource(resource));
    p->m_self = p;
    return p;
}
