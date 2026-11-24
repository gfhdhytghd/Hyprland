#include "GammaControl.hpp"
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include "../output/Monitor.hpp"
#include "../protocols/core/Output.hpp"
#include "../render/Renderer.hpp"
using namespace Hyprutils::OS;

CGammaControl::CGammaControl(SP<CZwlrGammaControlV1> resource_, wl_resource* output) : m_resource(resource_) {
    if UNLIKELY (!resource_->resource())
        return;

    m_resource->setDestroy([this](CZwlrGammaControlV1* gamma) { PROTO::gamma->destroyGammaControl(this); });
    m_resource->setOnDestroy([this](CZwlrGammaControlV1* gamma) { PROTO::gamma->destroyGammaControl(this); });

    m_resource->setSetGamma([this](CZwlrGammaControlV1*, int32_t fd) { setGamma(fd); });

    auto OUTPUTRES = CWLOutputResource::fromResource(output);

    if UNLIKELY (!OUTPUTRES || !OUTPUTRES->m_owner || OUTPUTRES->m_owner->isDefunct()) {
        LOG(Log::ERR, "No output in CGammaControl");
        m_resource->sendFailed();
        return;
    }

    const auto monitor = OUTPUTRES->m_monitor.lock();

    if UNLIKELY (!monitor || !monitor->m_output) {
        LOG(Log::ERR, "No CMonitor");
        m_resource->sendFailed();
        return;
    }

    for (auto const& g : PROTO::gamma->m_gammaControllers) {
        if UNLIKELY (g->m_monitor == monitor) {
            m_resource->sendFailed();
            return;
        }
    }

    m_gammaSize = monitor->m_output->getGammaSize();

    if UNLIKELY (m_gammaSize <= 0) {
        LOG(Log::ERR, "Output {} doesn't support gamma", monitor->m_name);
        m_resource->sendFailed();
        return;
    }

    m_monitor = monitor;

    m_resource->sendGammaSize(m_gammaSize);

    m_listeners.monitorDestroy    = m_monitor->m_events.destroy.listen([this] { fail(); });
    m_listeners.monitorDisconnect = m_monitor->m_events.disconnect.listen([this] { fail(); });
}

CGammaControl::~CGammaControl() {
    if (m_gammaTable.empty() || !m_monitor || !m_monitor->m_output)
        return;

    // reset the ramp if the client dies for whatever reason and doesn't unset the gamma, keeping the monitor's own LUTs
    m_monitor->setClientGammaRamp({});
}

bool CGammaControl::good() {
    return m_resource->resource();
}

void CGammaControl::setGamma(int fd) {
    CFileDescriptor gammaFd{fd};
    if (!m_monitor)
        return;

    LOG(Log::DEBUG, "setGamma for {}", m_monitor->m_name);

    if UNLIKELY (m_monitor->gammaRampsInUse()) {
        LOG(Log::ERR, "Monitor has gamma ramps in use (ICC?)");
        fail();
        return;
    }

    struct stat info = {};
    if (fstat(gammaFd.get(), &info) < 0) {
        fail();
        return;
    }

    if (!S_ISREG(info.st_mode) || info.st_size != sc<off_t>(m_gammaSize * 3 * sizeof(uint16_t))) {
        m_resource->error(ZWLR_GAMMA_CONTROL_V1_ERROR_INVALID_GAMMA, "Gamma ramps size mismatch");
        return;
    }

    std::vector<uint16_t> channels(m_gammaSize * 3);
    const auto            bytes = std::as_writable_bytes(std::span{channels});
    for (size_t offset = 0; offset < bytes.size();) {
        const auto count = pread(gammaFd.get(), bytes.data() + offset, bytes.size() - offset, offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            fail();
            return;
        }
        offset += count;
    }

    // Wayland supplies three consecutive channels. Aquamarine takes interleaved RGB.
    m_gammaTable.resize(channels.size());
    for (size_t i = 0; i < m_gammaSize; ++i) {
        m_gammaTable[i * 3]     = channels[i];
        m_gammaTable[i * 3 + 1] = channels[m_gammaSize + i];
        m_gammaTable[i * 3 + 2] = channels[m_gammaSize * 2 + i];
    }

    applyToMonitor();
}

void CGammaControl::applyToMonitor() {
    if UNLIKELY (!m_monitor || !m_monitor->m_output)
        return; // ??

    LOG(Log::DEBUG, "setting to monitor {}", m_monitor->m_name);

    if (m_gammaTable.empty()) {
        m_monitor->setClientGammaRamp({});
        return;
    }

    m_monitor->setClientGammaRamp(m_gammaTable);

    if (!m_monitor->m_state.test()) {
        LOG(Log::DEBUG, "setting to monitor {} failed", m_monitor->m_name);
        fail();
        return;
    }

    g_pHyprRenderer->damageMonitor(m_monitor.lock());
}

PHLMONITOR CGammaControl::getMonitor() {
    return m_monitor.lock();
}

void CGammaControl::fail() {
    m_listeners.monitorDisconnect.reset();
    m_listeners.monitorDestroy.reset();
    if (!m_gammaTable.empty() && m_monitor && m_monitor->m_output)
        m_monitor->setClientGammaRamp({});
    m_gammaTable.clear();
    m_monitor.reset();
    m_resource->sendFailed();
}

CGammaControlProtocol::CGammaControlProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CGammaControlProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CZwlrGammaControlManagerV1>(client, ver, id)).get();
    RESOURCE->setOnDestroy([this](CZwlrGammaControlManagerV1* p) { this->onManagerResourceDestroy(p->resource()); });

    RESOURCE->setDestroy([this](CZwlrGammaControlManagerV1* pMgr) { this->onManagerResourceDestroy(pMgr->resource()); });
    RESOURCE->setGetGammaControl([this](CZwlrGammaControlManagerV1* pMgr, uint32_t id, wl_resource* output) { this->onGetGammaControl(pMgr, id, output); });
}

void CGammaControlProtocol::onManagerResourceDestroy(wl_resource* res) {
    std::erase_if(m_managers, [&](const auto& other) { return other->resource() == res; });
}

void CGammaControlProtocol::destroyGammaControl(CGammaControl* gamma) {
    std::erase_if(m_gammaControllers, [&](const auto& other) { return other.get() == gamma; });
}

void CGammaControlProtocol::onGetGammaControl(CZwlrGammaControlManagerV1* pMgr, uint32_t id, wl_resource* output) {
    const auto CLIENT   = pMgr->client();
    const auto RESOURCE = m_gammaControllers.emplace_back(makeUnique<CGammaControl>(makeShared<CZwlrGammaControlV1>(CLIENT, pMgr->version(), id), output)).get();

    if UNLIKELY (!RESOURCE->good()) {
        pMgr->noMemory();
        m_gammaControllers.pop_back();
        return;
    }
}

void CGammaControlProtocol::applyGammaToState(PHLMONITOR pMonitor) {
    for (auto const& g : m_gammaControllers) {
        if (g->getMonitor() != pMonitor)
            continue;

        g->applyToMonitor();
        break;
    }
}
