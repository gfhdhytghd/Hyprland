#pragma once

#include "Monitor.hpp"
#include "WorkBufferPool.hpp"
#include "../helpers/Format.hpp"
#include "../render/Framebuffer.hpp"
#include <hyprutils/math/Vector2D.hpp>

namespace Render {
    class CSceneResources;
}

namespace Monitor {
    class CMonitorResources {
      public:
        CMonitorResources(WP<CMonitor> monitor, DRMFormat format, Vector2D size, NColorManagement::PImageDescription imageDescription);

        SP<Render::IFramebuffer> getUnusedWorkBuffer();
        SP<Render::IFramebuffer> getUnusedWorkBuffer(const Vector2D& size);
        SP<Render::IFramebuffer> getUnusedWorkBuffer(DRMFormat format, NColorManagement::PImageDescription imageDescription, std::optional<Vector2D> size = std::nullopt);
        void                     forEachUnusedFB(std::function<void(SP<Render::IFramebuffer>)> callback, bool includeNamed = false);
        bool                     hasMirrorFB() const;
        bool                     shouldKeepMirrorFB() const;
        void                     releaseMirrorFB();
        void                     invalidateMirrorFB();
        void                     markMirrorFBStale(const CRegion& damage);
        void                     markMirrorFBStale();
        void                     markMirrorFBUpdated();
        CRegion                  pendingMirrorFBDamage() const;
        void                     enableMirror();
        void                     disableMirror();
        enum class eCopySource : uint8_t {
            CAPTURE,
            HDR_MIRROR
        };
        SP<Render::IFramebuffer>           mirrorFB(eCopySource source = eCopySource::CAPTURE);
        SP<Render::ITexture>               getMirrorTexture(eCopySource source = eCopySource::CAPTURE);
        SP<Render::ITexture>               getPhysicalMirrorTexture(PHLMONITOR destination);
        bool                               needsSeparatePhysicalMirrorFB() const;
        void                               refreshBlurFB();
        const SP<Render::CSceneResources>& sceneResources() const;
        bool                               prepareSceneResources(Render::CSceneResources& resources) const;
        SP<Render::ITexture>               m_mirrorTex;

        SP<Render::ITexture>               m_stencilTex; // TODO fix blur ignore alpha and remove
        SP<Render::IFramebuffer>           m_blurFB;

      private:
        void                                initFB(SP<Render::IFramebuffer> fb);
        void                                setImageDescription(NColorManagement::PImageDescription imageDescription);
        NColorManagement::PImageDescription getMirrorTexImageDescription();
        Vector2D                            mirrorFBDamageSize() const;

        SP<Render::IFramebuffer>&           copyFramebuffer(eCopySource source);

        SP<Render::IFramebuffer>            m_monitorMirrorFB;
        SP<Render::IFramebuffer>            m_physicalMirrorFB;
        CRegion                             m_mirrorFBStaleDamage;
        WP<CMonitor>                        m_monitor;
        DRMFormat                           m_drmFormat;
        Vector2D                            m_size;
        NColorManagement::PImageDescription m_imageDescription;
        bool                                m_mirrorFBValid            = false;
        bool                                m_mirrorFBNeedsFullRefresh = true;

        CWorkBufferPool                     m_workBuffers;
        CWorkBufferPool                     m_sizedWorkBuffers;
        SP<Render::CSceneResources>         m_sceneResources;

        friend class CMonitor;
    };
}
