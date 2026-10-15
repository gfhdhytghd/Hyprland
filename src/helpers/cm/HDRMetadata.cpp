#include "ColorManagement.hpp"
#include <drm_mode.h>
#include <algorithm>
#include <cmath>

using namespace NColorManagement;

static const hdr_output_metadata NO_HDR_METADATA = {.hdmi_metadata_type1 = hdr_metadata_infoframe{.eotf = 0}};

bool                             NColorManagement::isPhysicalDisplayChromaticities(const SPCPRimaries& primaries) {
    // Inside the xy unit triangle, which contains the spectrum locus, and spanning a color space with a positive white mix.
    for (const auto& xy : {primaries.red, primaries.green, primaries.blue, primaries.white}) {
        if (!std::isfinite(xy.x) || !std::isfinite(xy.y) || xy.x < 0 || xy.y < 0 || xy.x + xy.y > 1)
            return false;
    }
    return rgbToXYZFromPrimaries(primaries).has_value();
}

hdr_output_metadata NColorManagement::createHDRMetadata(const SImageDescription& settings) {
    uint8_t eotf = 0;
    switch (settings.transferFunction) {
        case CM_TRANSFER_FUNCTION_GAMMA22:
        case CM_TRANSFER_FUNCTION_SRGB: eotf = 0; break; // used to send primaries and luminances to AQ. ignored for now
        case CM_TRANSFER_FUNCTION_ST2084_PQ: eotf = 2; break;
        // Linear content is never sent to the display; it is composited into the output's PQ encoding.
        // case CM_TRANSFER_FUNCTION_HLG: eotf = 3; break; TODO check display capabilities first
        default: return NO_HDR_METADATA; // empty metadata for SDR
    }

    const auto toNits  = [](double value) { return sc<uint16_t>(std::clamp(std::round(value), 0.0, 65535.0)); };
    const auto to16Bit = [](float value) { return sc<uint16_t>(std::clamp(std::round(value * 50000.0), 0.0, 50000.0)); };

    // CTA-861.3: zero fields mean unknown. The primary volume is the encoding range, not a mastering display, and chromaticities
    // above 50000 (1.0) are reserved, so primaries that can't be a display are sent as unknown.
    const auto colorimetry = isPhysicalDisplayChromaticities(settings.masteringPrimaries) ? settings.masteringPrimaries : SPCPRimaries{};
    const auto luminances  = settings.masteringLuminances.max > 0 ? settings.masteringLuminances : SImageDescription::SPCMasteringLuminances{.min = 0, .max = 0};

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
