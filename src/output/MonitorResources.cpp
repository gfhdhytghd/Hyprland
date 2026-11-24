#include "MonitorResources.hpp"
#include "../managers/screenshare/ScreenshareManager.hpp"
#include "../helpers/cm/ColorManagement.hpp"
#include "../render/Renderer.hpp"
#include "../render/SceneResources.hpp"
#include "../render/OpenGL.hpp"
#include "../config/ConfigValue.hpp"
#include <cstdint>
#include <format>
#include <limits>

using namespace Monitor;
using namespace NColorManagement;

static void prepareWorkBufferRelease() {
    // Timer expiry also runs when no frame has made the rendering context current.
    if (Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
}

CMonitorResources::CMonitorResources(WP<CMonitor> monitor, DRMFormat format, Vector2D size, NColorManagement::PImageDescription imageDescription) :
    m_stencilTex(g_pHyprRenderer->createStencilTexture(size.x, size.y)), m_blurFB(g_pHyprRenderer->createFB(std::format("Monitor {} blur FB", monitor->m_name))),
    m_monitor(monitor), m_drmFormat(format), m_size(size), m_imageDescription(imageDescription),
    m_workBuffers(
        [this] {
            auto buffer = g_pHyprRenderer->createFB(std::format("Monitor {} workbuffer", m_monitor->m_name));
            if (buffer)
                buffer->addStencil(m_stencilTex);
            return buffer;
        },
        prepareWorkBufferRelease, std::numeric_limits<uint64_t>::max()),
    m_sizedWorkBuffers([this] { return g_pHyprRenderer->createFB(std::format("Monitor {} sized workbuffer", m_monitor->m_name)); }, prepareWorkBufferRelease) {
    initFB(m_blurFB);
    monitor->m_blurFBDirty = true;
    m_sceneResources       = makeShared<Render::CSceneResources>(monitor);
}

const SP<Render::CSceneResources>& CMonitorResources::sceneResources() const {
    return m_sceneResources;
}

bool CMonitorResources::prepareSceneResources(Render::CSceneResources& resources) const {
    return resources.prepare(m_size, m_drmFormat, m_imageDescription);
}

void CMonitorResources::initFB(SP<Render::IFramebuffer> fb) {
    fb->addStencil(m_stencilTex);
    fb->alloc(m_size.x, m_size.y, m_drmFormat);
    fb->setImageDescription(m_imageDescription);
}

void CMonitorResources::setImageDescription(NColorManagement::PImageDescription imageDescription) {
    if (m_imageDescription == imageDescription)
        return;
    m_imageDescription = imageDescription;
    m_blurFB->setImageDescription(imageDescription);
    m_workBuffers.setImageDescription(imageDescription);
    m_sizedWorkBuffers.setImageDescription(imageDescription);
    if (m_mirrorTex)
        m_mirrorTex->m_imageDescription = getMirrorTexImageDescription();
    // retagging leaves the old encoding in the cached blur and in undamaged work buffer regions (e.g. render:fp16_sdr_tf toggled at runtime)
    m_monitor->m_blurFBDirty = true;
    g_pHyprRenderer->damageMonitor(m_monitor.lock());
    invalidateMirrorFB();
}

SP<Render::IFramebuffer> CMonitorResources::getUnusedWorkBuffer() {
    return m_workBuffers.acquire(m_size, m_drmFormat, m_imageDescription);
}

SP<Render::IFramebuffer> CMonitorResources::getUnusedWorkBuffer(const Vector2D& size) {
    return m_sizedWorkBuffers.acquire(size, m_drmFormat, m_imageDescription);
}

SP<Render::IFramebuffer> CMonitorResources::getUnusedWorkBuffer(DRMFormat format, NColorManagement::PImageDescription imageDescription, std::optional<Vector2D> size) {
    if (!imageDescription)
        return nullptr;

    return size ? m_sizedWorkBuffers.acquire(*size, format, imageDescription, true) : m_workBuffers.acquire(m_size, format, imageDescription, true);
}

void CMonitorResources::forEachUnusedFB(std::function<void(SP<Render::IFramebuffer>)> callback, bool includeNamed) {
    m_workBuffers.forEachUnused(callback);
    m_sizedWorkBuffers.forEachUnused(callback);
    if (includeNamed) {
        if (m_blurFB && m_blurFB->isAllocated() && m_blurFB.strongRef() < 2)
            callback(m_blurFB);
        if (hasMirrorFB() && m_monitorMirrorFB.strongRef() < 2)
            callback(m_monitorMirrorFB);
        if (m_physicalMirrorFB && m_physicalMirrorFB->isAllocated() && m_physicalMirrorFB.strongRef() < 2)
            callback(m_physicalMirrorFB);
    }
}

bool CMonitorResources::hasMirrorFB() const {
    return m_monitorMirrorFB && m_monitorMirrorFB->isAllocated();
}

bool CMonitorResources::shouldKeepMirrorFB() const {
    return !m_monitor->m_mirrors.empty() || Screenshare::mgr()->isOutputBeingSSd(m_monitor.lock());
}

void CMonitorResources::releaseMirrorFB() {
    if (m_monitorMirrorFB)
        m_monitorMirrorFB->release();
    m_physicalMirrorFB.reset();

    invalidateMirrorFB();
}

void CMonitorResources::invalidateMirrorFB() {
    m_mirrorFBValid            = false;
    m_mirrorFBNeedsFullRefresh = true;
    m_mirrorFBStaleDamage.clear();
}

void CMonitorResources::markMirrorFBStale(const CRegion& damage) {
    if (damage.empty() || !hasMirrorFB() || !m_mirrorFBValid)
        return;

    m_mirrorFBStaleDamage.add(damage).intersect(CBox{{}, mirrorFBDamageSize()});
}

void CMonitorResources::markMirrorFBStale() {
    if (!hasMirrorFB() || !m_mirrorFBValid)
        return;

    m_mirrorFBNeedsFullRefresh = true;
    m_mirrorFBStaleDamage.clear();
}

void CMonitorResources::markMirrorFBUpdated() {
    if (!needsSeparatePhysicalMirrorFB())
        m_physicalMirrorFB.reset();
    m_mirrorFBValid            = true;
    m_mirrorFBNeedsFullRefresh = false;
    m_mirrorFBStaleDamage.clear();
}

CRegion CMonitorResources::pendingMirrorFBDamage() const {
    const auto DAMAGE_SIZE = mirrorFBDamageSize();
    if (!hasMirrorFB() || !m_mirrorFBValid || m_mirrorFBNeedsFullRefresh)
        return CRegion{0, 0, DAMAGE_SIZE.x, DAMAGE_SIZE.y};

    if (needsSeparatePhysicalMirrorFB() && (!m_physicalMirrorFB || !m_physicalMirrorFB->isAllocated()))
        return CRegion{0, 0, DAMAGE_SIZE.x, DAMAGE_SIZE.y};

    return m_mirrorFBStaleDamage.copy();
}

bool CMonitorResources::needsSeparatePhysicalMirrorFB() const {
    return m_mirrorTex && std::ranges::any_of(m_monitor->m_mirrors, [](const auto& mirror) { return mirror && isHDR(mirror->m_imageDescription->value()); });
}

SP<Render::IFramebuffer>& CMonitorResources::copyFramebuffer(eCopySource source) {
    return source == eCopySource::HDR_MIRROR && m_mirrorTex ? m_physicalMirrorFB : m_monitorMirrorFB;
}

SP<Render::IFramebuffer> CMonitorResources::mirrorFB(eCopySource source) {
    auto& framebuffer = copyFramebuffer(source);
    if (!framebuffer)
        framebuffer = g_pHyprRenderer->createFB(std::format("Monitor {} mirror FB", m_monitor->m_name));

    if (!framebuffer)
        return nullptr;

    // HDR mirrors keep the scene encoding when the SDR capture attachment is active.
    const auto SDR_TEXTURE = source == eCopySource::CAPTURE ? m_mirrorTex : nullptr;
    const auto FORMAT      = SDR_TEXTURE ? SDR_TEXTURE->m_drmFormat : m_drmFormat;
    const auto DESCRIPTION = SDR_TEXTURE ? SDR_TEXTURE->m_imageDescription : m_imageDescription;

    if (!framebuffer->isAllocated() || framebuffer->m_drmFormat != FORMAT || framebuffer->imageDescription() != DESCRIPTION) {
        invalidateMirrorFB();
        if (!framebuffer->alloc(m_size.x, m_size.y, FORMAT))
            return nullptr;
        framebuffer->setImageDescription(DESCRIPTION);
    }

    return framebuffer;
}

SP<Render::ITexture> CMonitorResources::getMirrorTexture(eCopySource source) {
    const auto& FB = copyFramebuffer(source);
    return FB && FB->isAllocated() ? FB->getTexture() : nullptr;
}

SP<Render::ITexture> CMonitorResources::getPhysicalMirrorTexture(PHLMONITOR destination) {
    return getMirrorTexture(isHDR(destination->m_imageDescription->value()) ? eCopySource::HDR_MIRROR : eCopySource::CAPTURE);
}

NColorManagement::PImageDescription CMonitorResources::getMirrorTexImageDescription() {
    // The unmodified copy is BT.709 SDR with the sRGB encoding, see the mirror output of doColorManagement.
    return DEFAULT_SRGB_IMAGE_DESCRIPTION;
}

Vector2D CMonitorResources::mirrorFBDamageSize() const {
    return m_monitor->m_transformedSize;
}

void CMonitorResources::enableMirror() {
    if (m_mirrorTex)
        return;
    m_mirrorTex = g_pHyprRenderer->createTexture();
    m_mirrorTex->allocate({m_size.x, m_size.y}, m_monitor->m_activeMonitorRule.m_enable10bit ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888);
    m_mirrorTex->m_imageDescription = getMirrorTexImageDescription();
    m_monitor->m_blurFBDirty        = true;
    invalidateMirrorFB();
    // The capture FB switches to this texture's encoding and needs a full copy.
    g_pHyprRenderer->damageMonitor(m_monitor.lock());
}

void CMonitorResources::disableMirror() {
    if (!m_mirrorTex)
        return;
    m_monitor->m_blurFBDirty = true;
    m_mirrorTex.reset();
    invalidateMirrorFB();
    g_pHyprRenderer->damageMonitor(m_monitor.lock());
}

void CMonitorResources::refreshBlurFB() {
    static auto PBLURENABLED = CConfigValue<Config::BOOL>("decoration:blur:enabled");

    // TODO: this is super naive. It does free VRAM, but I don't want to make an auto-initializing wrapper
    // for now and this is good enough.
    if (!*PBLURENABLED) {
        m_blurFB->addStencil(nullptr);
        m_blurFB->alloc(1, 1, m_drmFormat);
    } else {
        m_blurFB->addStencil(m_stencilTex);
        m_blurFB->alloc(m_size.x, m_size.y, m_drmFormat);
    }
}
