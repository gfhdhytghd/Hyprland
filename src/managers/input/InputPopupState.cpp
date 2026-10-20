#include "InputPopupState.hpp"

bool SInputPopupState::visible() const {
    return mapped && focused;
}

bool SInputPopupState::visibleOnMonitor(const CBox& monitorBox) const {
    return visible() && !box.empty() && box.overlaps(monitorBox);
}
