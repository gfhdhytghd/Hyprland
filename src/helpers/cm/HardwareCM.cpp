#include "HardwareCM.hpp"
#include "../../render/types.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

using namespace NColorManagement;

// Display-referred transfer functions with a per-channel form and no extended range.
static bool hardwareSourceTF(eTransferFunction tf) {
    switch (tf) {
        case CM_TRANSFER_FUNCTION_SRGB:
        case CM_TRANSFER_FUNCTION_GAMMA22:
        case CM_TRANSFER_FUNCTION_GAMMA28:
        case CM_TRANSFER_FUNCTION_BT1886:
        case CM_TRANSFER_FUNCTION_ST240: return true;
        default: return false;
    }
}

static bool hardwareTargetTF(eTransferFunction tf) {
    return hardwareSourceTF(tf) || tf == CM_TRANSFER_FUNCTION_ST2084_PQ;
}

static uint16_t toLutEntry(double value) {
    return sc<uint16_t>(std::lround(std::clamp(value, 0.0, 1.0) * UINT16_MAX));
}

static std::vector<uint16_t> buildLut(size_t size, const std::function<double(double)>& fn) {
    std::vector<uint16_t> lut(size * 3);
    for (size_t i = 0; i < size; ++i) {
        const auto entry = toLutEntry(fn(sc<double>(i) / sc<double>(size - 1)));
        lut[(i * 3)]     = entry;
        lut[(i * 3) + 1] = entry;
        lut[(i * 3) + 2] = entry;
    }
    return lut;
}

static double sampleRamp(const std::vector<uint16_t>& ramp, size_t channel, double x) {
    const size_t entries  = ramp.size() / 3;
    const double position = std::clamp(x, 0.0, 1.0) * sc<double>(entries - 1);
    const size_t index    = std::min(sc<size_t>(position), entries - 2);
    const double fraction = position - sc<double>(index);
    return ((ramp[(index * 3) + channel] * (1.0 - fraction)) + (ramp[((index + 1) * 3) + channel] * fraction)) / UINT16_MAX;
}

// The KMS pipeline: per-channel interpolated LUTs around a CTM whose output is clamped to the LUT domain.
static std::array<double, 3> runHardware(const SHardwareCM& hw, const std::array<double, 3>& signal) {
    std::array<double, 3> linear = {};
    for (size_t i = 0; i < 3; ++i)
        linear[i] = sampleRamp(hw.degamma, i, signal[i]);
    std::array<double, 3> out = {};
    for (size_t row = 0; row < 3; ++row) {
        const double converted = (hw.ctm[row * 3] * linear[0]) + (hw.ctm[(row * 3) + 1] * linear[1]) + (hw.ctm[(row * 3) + 2] * linear[2]);
        out[row]               = sampleRamp(hw.gamma, row, std::clamp(converted, 0.0, 1.0));
    }
    return out;
}

// 8 bit ramps of grey, the primaries and secondaries, and a coarse cube: the regions where LUT spacing, interpolation and clipping differ most.
static std::vector<std::array<double, 3>> verificationSamples() {
    std::vector<std::array<double, 3>> samples;
    for (int i = 0; i <= 255; ++i) {
        const double v = i / 255.0;
        for (const auto& mask : std::array<std::array<double, 3>, 7>{{{1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1}}})
            samples.push_back({v * mask[0], v * mask[1], v * mask[2]});
    }
    for (int r = 0; r <= 8; ++r)
        for (int g = 0; g <= 8; ++g)
            for (int b = 0; b <= 8; ++b)
                samples.push_back({r / 8.0, g / 8.0, b / 8.0});
    return samples;
}

std::optional<SHardwareCM> NColorManagement::buildHardwareCM(const SImageDescription& source, const SImageDescription& target, const SHardwareCMParams& params) {
    if (params.degammaSize < 2 || params.gammaSize < 2 || !params.supportsCTM || target.icc.present)
        return std::nullopt;
    if (!hardwareSourceTF(source.transferFunction) || !hardwareTargetTF(target.transferFunction))
        return std::nullopt;

    // Same anchoring as IHyprRenderer::getCMSettings.
    const bool sdrToHDR = (source.transferFunction == CM_TRANSFER_FUNCTION_SRGB || source.transferFunction == CM_TRANSFER_FUNCTION_GAMMA22) &&
        target.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ;
    const float      referenceScale = referenceLuminanceScale(source, target);
    const float      sdrBrightness  = sdrToHDR && params.sdrBrightness > 0 ? params.sdrBrightness : 1.F;
    const float      sdrSaturation  = sdrToHDR && params.sdrSaturation > 0 ? params.sdrSaturation : 1.F;

    Render::STFRange sourceRange = {.min = source.encodingMinLuminance() * referenceScale, .max = source.encodingMaxLuminance() * referenceScale};
    if (sdrToHDR && params.sdrBlack.has_value() && *params.sdrBlack >= 0)
        sourceRange.min = *params.sdrBlack;
    const Render::STFRange targetRange = {.min = target.encodingMinLuminance(), .max = target.encodingMaxLuminance()};

    // Linear light between the LUTs is normalized to the brightest decoded value, which the CTM can't exceed below.
    const double peak = sourceRange.max * sdrBrightness;
    if (!(peak > 0) || sourceRange.max <= sourceRange.min || targetRange.max <= targetRange.min || peak > targetRange.max)
        return std::nullopt;

    // Tone mapping is a cross-channel operation, so the hardware path is only exact when the shader wouldn't tone map.
    const float sourcePeak   = source.maxCLL > 0 ? source.maxCLL : (source.masteringLuminances.max > 0 ? source.masteringLuminances.max : source.luminances.max);
    const float targetPeak   = target.masteringLuminances.max > 0 ? target.masteringLuminances.max : (target.luminances.max > 0 ? target.luminances.max : 10000);
    const float maxLuminance = referenceScale * sourcePeak * sdrBrightness;
    if (maxLuminance >= targetPeak * 1.01)
        return std::nullopt;

    // CTM = saturation (in target primaries, as the shader applies it) * primaries conversion.
    const auto  conversion = Hyprgraphics::CMatrix3{CPrimaries::from(source.getPrimaries())->convertMatrix(CPrimaries::from(target.getPrimaries()))}.mat();
    const auto  luminance  = Hyprgraphics::CMatrix3{CPrimaries::from(target.getPrimaries())->toXYZ()}.mat()[1];

    SHardwareCM result;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 3; ++col) {
            double value = 0;
            for (size_t k = 0; k < 3; ++k) {
                const double saturation = (sdrSaturation * (row == k ? 1.0 : 0.0)) + ((1.0 - sdrSaturation) * luminance[k]);
                value += saturation * conversion[k][col];
            }
            // A negative coefficient produces out-of-gamut colors that KMS clips per channel, unlike the shader's hue-preserving clip.
            if (value < -1e-6)
                return std::nullopt;
            result.ctm[(row * 3) + col] = sc<float>(std::max(value, 0.0));
        }
    }

    const auto sourceTF = source.transferFunction;
    const auto targetTF = target.transferFunction;
    result.degamma      = buildLut(params.degammaSize, [&](double signal) { return decodeChannel(signal, sourceTF, sourceRange) * sdrBrightness / peak; });
    result.gamma        = buildLut(params.gammaSize, [&](double light) { return encodeChannel(light * peak, targetTF, targetRange); });

    // A diagonal user CTM scales the encoded output on composited frames, the same as folding it into the encoding.
    for (size_t i = 0; i < params.gammaSize; ++i)
        for (size_t channel = 0; channel < 3; ++channel)
            result.gamma[(i * 3) + channel] = toLutEntry(params.outputGain[channel] * result.gamma[(i * 3) + channel] / UINT16_MAX);

    // LUT spacing is uniform in linear light, which can't follow steep encodings near black with few entries, and per-channel
    // clipping differs from the shader's gamut mapping. Accept the pipeline only if it reproduces the shader within half a 10 bit code.
    Render::SCMSettings settings{
        .sourceTF                = sourceTF,
        .targetTF                = targetTF,
        .srcTFRange              = sourceRange,
        .dstTFRange              = targetRange,
        .srcReferenceScale       = referenceScale,
        .convertMatrix           = conversion,
        .needsSDRmod             = sdrToHDR && (sdrBrightness != 1.F || sdrSaturation != 1.F),
        .sdrSaturation           = sdrSaturation,
        .sdrBrightnessMultiplier = sdrBrightness,
    };
    settings.dstPrimaries2XYZ = Hyprgraphics::CMatrix3{CPrimaries::from(target.getPrimaries())->toXYZ()}.mat();
    const auto SOURCE         = CImageDescription::from(source);
    const auto TARGET         = CImageDescription::from(target);
    for (const auto& signal : verificationSamples()) {
        const auto expected = convertColor(RGBAColor{{.r = signal[0], .g = signal[1], .b = signal[2], .a = 1}}, SOURCE, TARGET, settings);
        const auto actual   = runHardware(result, signal);
        for (size_t i = 0; i < 3; ++i) {
            if (std::abs(actual[i] - std::clamp(params.outputGain[i] * expected.v[i], 0.0, 1.0)) > 0.5 / 1023.0)
                return std::nullopt;
        }
    }

    return result;
}

std::vector<uint16_t> NColorManagement::composeGammaRamp(const std::vector<uint16_t>& base, const std::vector<uint16_t>& ramp, size_t size) {
    const bool hasBase = base.size() >= 6 && base.size() % 3 == 0;
    const bool hasRamp = ramp.size() >= 6 && ramp.size() % 3 == 0;
    if (size < 2 || (!hasBase && !hasRamp))
        return {};

    std::vector<uint16_t> result(size * 3);
    for (size_t i = 0; i < size; ++i) {
        const double x = sc<double>(i) / sc<double>(size - 1);
        for (size_t channel = 0; channel < 3; ++channel) {
            const double signal       = hasBase ? sampleRamp(base, channel, x) : x;
            result[(i * 3) + channel] = toLutEntry(hasRamp ? sampleRamp(ramp, channel, signal) : signal);
        }
    }
    return result;
}

WP<SHardwareCM> CHardwareCMCache::get(PImageDescription source, PImageDescription target, const SHardwareCMParams& params, std::chrono::steady_clock::time_point now) {
    if (!source || !target)
        return {};

    const auto it =
        std::ranges::find_if(m_entries, [&](const auto& entry) { return entry.source->id() == source->id() && entry.target->id() == target->id() && entry.params == params; });
    if (it != m_entries.end()) {
        std::rotate(it, it + 1, m_entries.end());
        return m_entries.back().result;
    }
    if (now < m_nextBuild)
        return {};

    const auto start   = std::chrono::steady_clock::now();
    auto       result  = buildHardwareCM(source->value(), target->value(), params);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // At most ten admissions/second and about 1% sustained rebuild CPU time.
    // Charge actual cost, including failed builds, even if one build exceeds the
    // allowance. There is no per-frame reset that clients can exploit.
    m_nextBuild = now + std::max(std::chrono::steady_clock::duration(std::chrono::milliseconds(100)), elapsed * 100);

    constexpr size_t CAPACITY = 8;
    if (m_entries.size() == CAPACITY)
        m_entries.erase(m_entries.begin());
    m_entries.push_back({.source = std::move(source), .target = std::move(target), .params = params, .result = result ? makeUnique<SHardwareCM>(std::move(*result)) : nullptr});
    return m_entries.back().result;
}
