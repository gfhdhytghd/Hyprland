#pragma once

#include <drm_mode.h>
#include <vector>
#include <cstdint>
#include "WaylandProtocol.hpp"
#include "../output/Monitor.hpp"
#include "core/Compositor.hpp"
#include "color-management-v1.hpp"
#include "../helpers/cm/ColorManagement.hpp"

class CColorManager;
class CColorManagementOutput;
class CWLOutputProtocol;
class CColorManagementImageDescription;
class CColorManagementProtocol;

class CColorManager {
  public:
    CColorManager(SP<CWpColorManagerV1> resource);

    bool       good();
    wl_client* client();

  private:
    SP<CWpColorManagerV1> m_resource;
};

class CColorManagementOutput {
  public:
    CColorManagementOutput(SP<CWpColorManagementOutputV1> resource, WP<CWLOutputResource> output);

    bool                                 good();
    wl_client*                           client();
    bool                                 inert();

    WP<CColorManagementOutput>           m_self;
    WP<CColorManagementImageDescription> m_imageDescription;

  private:
    SP<CWpColorManagementOutputV1> m_resource;
    wl_client*                     m_client = nullptr;
    WP<CWLOutputResource>          m_output;
    // The object stays bound to the output global, not to the client's wl_output object.
    PHLMONITORREF         m_monitor;
    WP<CWLOutputProtocol> m_outputGlobal;

    friend class CColorManagementProtocol;
    friend class CColorManagementImageDescription;
};

class CColorManagementSurface {
  public:
    CColorManagementSurface(SP<CWpColorManagementSurfaceV1> resource, SP<CWLSurfaceResource> surface_);

    bool                                       good();
    wl_client*                                 client();

    WP<CColorManagementSurface>                m_self;
    WP<CWLSurfaceResource>                     m_surface;

  private:
    SP<CWpColorManagementSurfaceV1> m_resource;
    wl_client*                      m_client = nullptr;
    CHyprSignalListener             m_destroyListener;
};

class CColorManagementFeedbackSurface {
  public:
    CColorManagementFeedbackSurface(SP<CWpColorManagementSurfaceFeedbackV1> resource, SP<CWLSurfaceResource> surface_);

    bool                                good();
    wl_client*                          client();

    WP<CColorManagementFeedbackSurface> m_self;
    WP<CWLSurfaceResource>              m_surface;

  private:
    SP<CWpColorManagementSurfaceFeedbackV1> m_resource;
    wl_client*                              m_client = nullptr;

    NColorManagement::PImageDescription     m_currentPreferred;

    struct {
        CHyprSignalListener destroy;
        CHyprSignalListener enter;
        CHyprSignalListener leave;
    } m_listeners;

    void onPreferredChanged();

    friend class CColorManagementProtocol;
};

class CColorManagementIccCreator {
  public:
    CColorManagementIccCreator(SP<CWpImageDescriptionCreatorIccV1> resource);

    bool                                good();
    wl_client*                          client();

    WP<CColorManagementIccCreator>      m_self;

    NColorManagement::SImageDescription m_settings;
    struct SIccFile {
        int      fd     = -1;
        uint32_t length = 0;
        uint32_t offset = 0;
        bool     operator==(const SIccFile& i2) const {
            return fd == i2.fd;
        }
    } m_icc;

  private:
    SP<CWpImageDescriptionCreatorIccV1> m_resource;
    wl_client*                          m_client = nullptr;
};

class CColorManagementParametricCreator {
  public:
    CColorManagementParametricCreator(SP<CWpImageDescriptionCreatorParamsV1> resource);

    bool                                  good();
    wl_client*                            client();

    WP<CColorManagementParametricCreator> m_self;

    NColorManagement::SImageDescription   m_settings;

  private:
    enum eValuesSet : uint32_t { // NOLINT
        PC_TF                   = (1 << 0),
        PC_TF_POWER             = (1 << 1),
        PC_PRIMARIES            = (1 << 2),
        PC_LUMINANCES           = (1 << 3),
        PC_MASTERING_PRIMARIES  = (1 << 4),
        PC_MASTERING_LUMINANCES = (1 << 5),
        PC_CLL                  = (1 << 6),
        PC_FALL                 = (1 << 7),
    };

    SP<CWpImageDescriptionCreatorParamsV1> m_resource;
    wl_client*                             m_client    = nullptr;
    uint32_t                               m_valuesSet = 0; // enum eValuesSet
};

class CColorManagementImageDescription {
  public:
    CColorManagementImageDescription(SP<CWpImageDescriptionV1> resource, bool allowGetInformation);
    static SP<CColorManagementImageDescription> fromReference(wl_resource* res);

    bool                                        good();
    wl_client*                                  client();
    SP<CWpImageDescriptionV1>                   resource();
    bool                                        sendMaybeReady();
    bool                                        ready() const;

    WP<CColorManagementImageDescription>        m_self;

    NColorManagement::PImageDescription         m_settings;

  private:
    SP<CWpImageDescriptionV1> m_resource;
    wl_client*                m_client              = nullptr;
    bool                      m_allowGetInformation = false;
    bool                      m_ready               = false;

    friend class CColorManager;
    friend class CColorManagementOutput;
};

class CColorManagementImageDescriptionInfo {
  public:
    CColorManagementImageDescriptionInfo(SP<CWpImageDescriptionInfoV1> resource, const NColorManagement::SImageDescription& settings_);

    bool       good();
    wl_client* client();

  private:
    SP<CWpImageDescriptionInfoV1>       m_resource;
    wl_client*                          m_client = nullptr;
    NColorManagement::SImageDescription m_settings;
};

class CColorManagementProtocol : public IWaylandProtocol {
  public:
    CColorManagementProtocol(const wl_interface* iface, const int& ver, const std::string& name, bool debug = false);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

    void         onImagePreferredChanged();
    void         recheckFeedbacks();
    void         onMonitorImageDescriptionChanged(PHLMONITORREF monitor);

    bool         isClientCMAware(wl_client* client);

  private:
    void                                               schedulePreferredCheck();
    void                                               destroyResource(CColorManager* resource);
    void                                               destroyResource(CColorManagementOutput* resource);
    void                                               destroyResource(CColorManagementSurface* resource);
    void                                               destroyResource(CColorManagementFeedbackSurface* resource);
    void                                               destroyResource(CColorManagementIccCreator* resource);
    void                                               destroyResource(CColorManagementParametricCreator* resource);
    void                                               destroyResource(CColorManagementImageDescription* resource);

    std::vector<SP<CColorManager>>                     m_managers;
    std::vector<SP<CColorManagementOutput>>            m_outputs;
    std::vector<SP<CColorManagementSurface>>           m_surfaces;
    std::vector<SP<CColorManagementFeedbackSurface>>   m_feedbackSurfaces;
    std::vector<SP<CColorManagementIccCreator>>        m_iccCreators;
    std::vector<SP<CColorManagementParametricCreator>> m_parametricCreators;
    std::vector<SP<CColorManagementImageDescription>>  m_imageDescriptions;
    bool                                               m_debug                   = false;
    bool                                               m_preferredCheckScheduled = false;

    struct {
        CHyprSignalListener configReloaded;
        CHyprSignalListener monitorRemoved;
        CHyprSignalListener workspaceMoved;
        CHyprSignalListener windowMoved;
    } m_listeners;

    friend class CColorManager;
    friend class CColorManagementOutput;
    friend class CColorManagementSurface;
    friend class CColorManagementFeedbackSurface;
    friend class CColorManagementIccCreator;
    friend class CColorManagementParametricCreator;
    friend class CColorManagementImageDescription;
};

namespace PROTO {
    inline UP<CColorManagementProtocol> colorManagement;
};
