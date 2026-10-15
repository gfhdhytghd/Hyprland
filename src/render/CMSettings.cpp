#include "CMSettings.hpp"

#include <algorithm>

using namespace Render;
using namespace NColorManagement;

SCMSettings CCMSettingsCache::get(PImageDescription imageDescription, PImageDescription targetImageDescription, const SCMSettingsInputs& inputs) {
    const auto srcId = imageDescription->id();
    const auto dstId = targetImageDescription->id();
    for (const auto& entry : m_entries) {
        if (entry.sourceId == srcId && entry.targetId == dstId && entry.inputs == inputs)
            return entry.settings;
    }

    const auto& source = imageDescription->value();
    const auto& target = targetImageDescription->value();
    // Reference whites are anchored for every conversion, including SDR content on HDR outputs.
    const float referenceScale  = referenceLuminanceScale(source, target);
    const float sdrBrightness   = inputs.needsSDRmod && inputs.sdrBrightness > 0 ? inputs.sdrBrightness : 1.0f;
    const float sourcePeak      = source.maxCLL > 0 ? source.maxCLL : (source.masteringLuminances.max > 0 ? source.masteringLuminances.max : source.luminances.max);
    const float maxLuminance    = referenceScale * sourcePeak * sdrBrightness;
    const auto  dstMaxLuminance = target.masteringLuminances.max > 0 ? target.masteringLuminances.max : (target.luminances.max > 0 ? target.luminances.max : 10000);

    auto        matrix = imageDescription->getPrimaries()->convertMatrix(targetImageDescription->getPrimaries());
    auto        toXYZ  = targetImageDescription->getPrimaries()->toXYZ();

    const bool  needsMod = inputs.needsSDRmod && ((inputs.sdrSaturation > 0 && inputs.sdrSaturation != 1.0f) || (inputs.sdrBrightness > 0 && inputs.sdrBrightness != 1.0f));

    const bool  needsTonemap = maxLuminance >= dstMaxLuminance * 1.01;

    auto        result = SCMSettings{
        .sourceTF          = inputs.sourceTF,
        .targetTF          = targetImageDescription->value().transferFunction,
        .srcTFRange        = {.min = source.encodingMinLuminance() * referenceScale, .max = source.encodingMaxLuminance() * referenceScale},
        .dstTFRange        = {.min = target.encodingMinLuminance(), .max = target.encodingMaxLuminance()},
        .srcReferenceScale = referenceScale,
        .srcRefLuminance   = imageDescription->value().luminances.reference,
        .dstRefLuminance   = targetImageDescription->value().luminances.reference,
        .convertMatrix     = matrix.mat(),

        .needsTonemap            = inputs.tonemapMode != 0 && needsTonemap,
        .tonemapMode             = inputs.tonemapMode,
        .maxLuminance            = needsTonemap && inputs.tonemapMode == 2 ? dstMaxLuminance : maxLuminance,
        .dstMaxLuminance         = dstMaxLuminance,
        .srcMaxLuminance         = referenceScale * sourcePeak,
        .srcMinLuminance         = referenceScale * (source.masteringLuminances.max > 0 ? source.masteringLuminances.min : source.encodingMinLuminance()),
        .dstPrimaries2XYZ        = toXYZ.mat(),
        .needsSDRmod             = needsMod,
        .sdrSaturation           = inputs.needsSDRmod && inputs.sdrSaturation > 0 ? inputs.sdrSaturation : 1.0f,
        .sdrBrightnessMultiplier = sdrBrightness,
    };

    // sdr_min_luminance sets the absolute SDR black level on HDR outputs.
    if (inputs.needsSDRmod && inputs.sdrMinLuminance >= 0) {
        result.srcTFRange.min  = inputs.sdrMinLuminance;
        result.srcMinLuminance = inputs.sdrMinLuminance;
    }

    // Linear sources skip toNit, so their pixels take the reference-white ratio through the conversion matrix.
    if (inputs.sourceTF == CM_TRANSFER_FUNCTION_LINEAR)
        for (auto& row : result.convertMatrix)
            for (auto& value : row)
                value *= referenceScale;

    // Bound even a long render session with many unique client descriptions.
    constexpr size_t CAPACITY = 64;
    if (m_entries.size() == CAPACITY)
        m_entries.erase(m_entries.begin());
    m_entries.push_back({.sourceId = srcId, .targetId = dstId, .inputs = inputs, .settings = result});
    return result;
}

void CCMSettingsCache::clear() {
    m_entries.clear();
}
