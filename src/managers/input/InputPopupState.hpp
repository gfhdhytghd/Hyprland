#pragma once

#include "../../helpers/math/Math.hpp"

struct SInputPopupState {
    bool mapped  = false;
    bool focused = false;
    CBox box;

    bool visible() const;
    bool visibleOnMonitor(const CBox& monitorBox) const;
};
