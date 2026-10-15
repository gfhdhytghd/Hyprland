#pragma once

#include "ColorManagement.hpp"

#include <array>
#include <cstdint>
#include <chrono>
#include <optional>
#include <vector>

namespace NColorManagement {
    // Inputs for converting a buffer with the display hardware: DEGAMMA_LUT -> CTM -> GAMMA_LUT.
    struct SHardwareCMParams {
        size_t               degammaSize   = 0;
        size_t               gammaSize     = 0;
        bool                 supportsCTM   = false;
        float                sdrBrightness = 1.F; // only applied when the source is SDR and the target HDR, like the shader
        float                sdrSaturation = 1.F;
        std::optional<float> sdrBlack;                     // sdr_min_luminance, absolute source black on HDR targets
        std::array<float, 3> outputGain = {1.F, 1.F, 1.F}; // diagonal user CTM, which acts on the encoded output of composited frames
        bool                 operator==(const SHardwareCMParams&) const = default;
    };

    struct SHardwareCM {
        std::vector<uint16_t> degamma; // interleaved [r,g,b]+, source signal -> linear light normalized to the source peak
        std::array<float, 9>  ctm = {
            1, 0, 0, //
            0, 1, 0, //
            0, 0, 1, //
        }; // row-major, linear source primaries -> linear target primaries
        std::vector<uint16_t> gamma; // interleaved [r,g,b]+, linear light -> target signal, before user ramps
    };

    // Bounded per-output LRU, including failures. Cache hits are free of rebuild
    // throttling; misses fall back to composition while the CPU budget recovers.
    class CHardwareCMCache {
      public:
        WP<SHardwareCM> get(PImageDescription source, PImageDescription target, const SHardwareCMParams& params,
                            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

      private:
        struct SEntry {
            PImageDescription source;
            PImageDescription target;
            SHardwareCMParams params;
            UP<SHardwareCM>   result;
        };
        std::vector<SEntry>                   m_entries;
        std::chrono::steady_clock::time_point m_nextBuild = {};
    };

    // Builds a hardware equivalent of the shader conversion, or nothing when the hardware can't reproduce it within half a 10 bit code:
    // HLG and extended ranges have no per-channel LUT form, tone mapping and hue-preserving gamut clipping have no
    // hardware form, HDR sources would lose shadow precision in a 16 bit linear LUT, and small LUTs can't follow steep encodings.
    std::optional<SHardwareCM> buildHardwareCM(const SImageDescription& source, const SImageDescription& target, const SHardwareCMParams& params);

    // Applies a gamma ramp (gamma-control or VCGT, defined on the output signal) after base, resampled to size entries.
    // Empty base or ramp means identity; returns empty when both are.
    std::vector<uint16_t> composeGammaRamp(const std::vector<uint16_t>& base, const std::vector<uint16_t>& ramp, size_t size);
}
