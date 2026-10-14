#include "ColorManagement.hpp"
#include <drm_mode.h>
#include <algorithm>
#include <cmath>

using namespace NColorManagement;

static const hdr_output_metadata NO_HDR_METADATA = {.hdmi_metadata_type1 = hdr_metadata_infoframe{.eotf = 0}};

hdr_output_metadata              NColorManagement::createHDRMetadata(const SImageDescription& settings) {
    uint8_t eotf = 0;
    switch (settings.transferFunction) {
        case CM_TRANSFER_FUNCTION_GAMMA22:
        case CM_TRANSFER_FUNCTION_SRGB: eotf = 0; break; // used to send primaries and luminances to AQ. ignored for now
        case CM_TRANSFER_FUNCTION_ST2084_PQ: eotf = 2; break;
        case CM_TRANSFER_FUNCTION_EXT_LINEAR:
            eotf = 2;
            break; // should be Windows scRGB
        // case CM_TRANSFER_FUNCTION_HLG: eotf = 3; break; TODO check display capabilities first
        default: return NO_HDR_METADATA; // empty metadata for SDR
    }

    const auto toNits  = [](double value) { return sc<uint16_t>(std::clamp(std::round(value), 0.0, 65535.0)); };
    const auto to16Bit = [](float value) { return sc<uint16_t>(std::clamp(std::round(value * 50000.0), 0.0, 65535.0)); };

    const auto colorimetry = settings.masteringPrimaries != SPCPRimaries{} ? settings.masteringPrimaries : settings.getPrimaries();
    const auto luminances  = settings.masteringLuminances.max > 0 ? settings.masteringLuminances :
                                                                    SImageDescription::SPCMasteringLuminances{.min = settings.luminances.min, .max = settings.luminances.max};

    LOG(Log::TRACE, "ColorManagement primaries {},{} {},{} {},{} {},{}", colorimetry.red.x, colorimetry.red.y, colorimetry.green.x, colorimetry.green.y, colorimetry.blue.x,
        colorimetry.blue.y, colorimetry.white.x, colorimetry.white.y);
    LOG(Log::TRACE, "ColorManagement min {}, max {}, cll {}, fall {}", luminances.min, luminances.max, settings.maxCLL, settings.maxFALL);
    return hdr_output_metadata{
        .metadata_type = 0,
        .hdmi_metadata_type1 =
            hdr_metadata_infoframe{
                .eotf          = eotf,
                .metadata_type = 0,
                .display_primaries =
                    {
                        {.x = to16Bit(colorimetry.red.x), .y = to16Bit(colorimetry.red.y)},
                        {.x = to16Bit(colorimetry.green.x), .y = to16Bit(colorimetry.green.y)},
                        {.x = to16Bit(colorimetry.blue.x), .y = to16Bit(colorimetry.blue.y)},
                    },
                .white_point                     = {.x = to16Bit(colorimetry.white.x), .y = to16Bit(colorimetry.white.y)},
                .max_display_mastering_luminance = toNits(luminances.max),
                .min_display_mastering_luminance = toNits(luminances.min * 10000),
                .max_cll                         = toNits(settings.maxCLL),
                .max_fall                        = toNits(settings.maxFALL),
            },
    };
}
