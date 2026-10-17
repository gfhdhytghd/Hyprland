#pragma once

#include "ColorManagement.hpp"
#include <lcms2.h>

namespace NColorManagement {
    // Recover native colorimetry from tags, falling back to relative-colorimetric sampling.
    std::expected<SPCPRimaries, std::string> iccPrimaries(cmsHPROFILE profile);
}
