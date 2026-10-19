#pragma once

#include "ColorManagement.hpp"
#include <lcms2.h>

namespace NColorManagement {
    // Recover native colorimetry from tags, falling back to relative-colorimetric sampling.
    std::expected<SPCPRimaries, std::string> iccPrimaries(cmsHPROFILE profile);

    // Fill image.icc.lutDataPacked, indexed by sRGB-encoded BT.709 input (see applyIcc3DLut).
    std::expected<void, std::string> iccBuildLUTData(cmsHPROFILE profile, SImageDescription& image);
}
