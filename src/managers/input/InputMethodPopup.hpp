#pragma once

#include "../../desktop/view/WLSurface.hpp"
#include "../../macros.hpp"
#include "../../helpers/math/Math.hpp"
#include "../../helpers/signal/Signal.hpp"

class CInputMethodPopupV2;
struct SInputPopupState;

class CInputPopup {
  public:
    CInputPopup(SP<CInputMethodPopupV2> popup);

    void damageEntire();
    void damageSurface();

    bool isVecInPopup(const Vector2D& point);

    // Only mapped popups whose base surface (ti -> owner) is focused are visible.
    bool                   shouldBeRendered();
    SInputPopupState       state();

    CBox                   globalBox();
    SP<CWLSurfaceResource> getSurface();

    void                   onCommit();

  private:
    SP<Desktop::View::CWLSurface> queryOwner();
    void                          updateBox();

    void                          onDestroy();
    void                          onMap();
    void                          onUnmap();

    WP<CInputMethodPopupV2>       m_popup;
    SP<Desktop::View::CWLSurface> m_surface;
    CBox                          m_lastBoxLocal;
    CBox                          m_lastBoxGlobal;
    MONITORID                     m_lastMonitor = MONITOR_INVALID;

    struct {
        CHyprSignalListener map;
        CHyprSignalListener unmap;
        CHyprSignalListener destroy;
        CHyprSignalListener commit;
    } m_listeners;
};
