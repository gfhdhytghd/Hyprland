#include "OutputEncoding.hpp"

using namespace NColorManagement;

bool SOutputEncoding::hdr() const {
    return !icc && NCMType::isHDR(cmType);
}

SConfiguredCMType NColorManagement::resolveConfiguredCMType(NCMType::eCMType rule, const SOutputColorCapabilities& caps) {
    switch (rule) {
        case NCMType::CM_AUTO: return {.cmType = caps.enabled10bit && caps.wideColor ? NCMType::CM_WIDE : NCMType::CM_SRGB};
        case NCMType::CM_EDID: return {.cmType = caps.edidChromaticities ? NCMType::CM_EDID : NCMType::CM_SRGB};
        // BT.2020 pixels the connector can't signal would be shown with the display's default colorimetry.
        case NCMType::CM_WIDE:
            return caps.bt2020Signal ? SConfiguredCMType{.cmType = NCMType::CM_WIDE} : SConfiguredCMType{.cmType = NCMType::CM_SRGB, .fallback = CM_FALLBACK_CAPABILITY};
        case NCMType::CM_HDR:
        case NCMType::CM_HDR_EDID:
            if (!caps.hdr)
                return {.cmType = NCMType::CM_SRGB, .fallback = CM_FALLBACK_CAPABILITY};
            // PQ for unconverted sRGB pixels
            if (!caps.shaderCM)
                return {.cmType = NCMType::CM_SRGB, .fallback = CM_FALLBACK_NO_SHADER_CM};
            return {.cmType = rule};
        // SDR presets keep their encoding without shader CM (owner decision 2026-11-24 for wide and auto, see ICC-03)
        default: return {.cmType = rule};
    }
}

std::optional<NCMType::eCMType> NColorManagement::autoHDRCMType(const SOutputEncodingInputs& inputs) {
    // An ICC profile describes the calibrated SDR output and has no HDR counterpart to switch to.
    if (inputs.iccProfile || !inputs.autoHDR || !inputs.fullscreen || inputs.noAutoHDR || !inputs.caps.shaderCM || !inputs.caps.hdr)
        return std::nullopt;

    // A configured HDR preset already is the HDR mode, auto HDR neither switches to nor away from it.
    if (NCMType::isHDR(resolveConfiguredCMType(inputs.rule, inputs.caps).cmType))
        return std::nullopt;

    return inputs.autoHDR == 2 ? NCMType::CM_HDR_EDID : NCMType::CM_HDR;
}

bool NColorManagement::autoHDRApplicable(const SOutputEncodingInputs& inputs) {
    auto withShaderCM          = inputs;
    withShaderCM.caps.shaderCM = true;
    withShaderCM.noAutoHDR     = false;
    return autoHDRCMType(withShaderCM).has_value();
}

SOutputEncoding NColorManagement::resolveOutputEncoding(const SOutputEncodingInputs& inputs) {
    if (inputs.iccProfile)
        return {.icc = true};

    const auto      CONFIGURED = resolveConfiguredCMType(inputs.rule, inputs.caps);

    SOutputEncoding encoding;
    encoding.baseline = CONFIGURED.cmType;
    encoding.cmType   = CONFIGURED.cmType;
    encoding.fallback = CONFIGURED.fallback;

    if (inputs.content != FS_CONTENT_HDR)
        return encoding;

    if (const auto AUTO = autoHDRCMType(inputs); AUTO.has_value()) {
        encoding.cmType  = *AUTO;
        encoding.autoHDR = true;
    } else if (encoding.fallback == CM_FALLBACK_NONE && !inputs.caps.shaderCM && !inputs.noAutoHDR && autoHDRApplicable(inputs))
        encoding.fallback = CM_FALLBACK_AUTO_HDR_NO_SHADER_CM; // PQ for unconverted pixels, see resolveConfiguredCMType

    return encoding;
}

bool NColorManagement::outputEncodingNeedsApply(const SOutputEncoding& encoding, std::optional<NCMType::eCMType> appliedCMType, bool descriptionIsICC) {
    // The ICC description is installed by the monitor rule, presets never replace it.
    if (encoding.icc)
        return false;

    return descriptionIsICC || appliedCMType != encoding.cmType;
}

eFullscreenContent NColorManagement::classifyFullscreenContent(const PImageDescription& description) {
    if (!description)
        return FS_CONTENT_NONE;

    return isHDR(description->value()) ? FS_CONTENT_HDR : FS_CONTENT_SDR;
}

float NColorManagement::contentPeakLuminance(const SImageDescription& description) {
    if (description.maxCLL > 0)
        return description.maxCLL;
    if (description.masteringLuminances.max > 0)
        return description.masteringLuminances.max;
    return description.luminances.max;
}

float NColorManagement::displayPeakLuminance(const SImageDescription& description) {
    return description.masteringLuminances.max > 0 ? description.masteringLuminances.max : description.luminances.max;
}

bool NColorManagement::peakNeedsToneMapping(float sourcePeak, float targetPeak) {
    // 1% tolerance against rounded metadata, evaluated in float like the shaders (tonemap.glsl)
    return sourcePeak >= targetPeak * 1.01F;
}

bool NColorManagement::contentFitsDisplayPeak(const SImageDescription& source, const SImageDescription& target) {
    return !peakNeedsToneMapping(contentPeakLuminance(source), displayPeakLuminance(target));
}

bool NColorManagement::hdrPassthroughCompatible(const SImageDescription& source, const SImageDescription& target) {
    return source.transferFunction == target.transferFunction && source.luminances == target.luminances && contentFitsDisplayPeak(source, target);
}

bool NColorManagement::compositeReproducesSurface(const SImageDescription& surface, const SImageDescription& target) {
    return surface.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ && hdrPassthroughCompatible(surface, target) && surface.getPrimaries() == target.getPrimaries();
}

bool NColorManagement::ctmConvertsExactly(const SImageDescription& source, const SImageDescription& target, bool compatibleTF, bool supportsCTM) {
    const auto linear       = [](eTransferFunction tf) { return tf == CM_TRANSFER_FUNCTION_LINEAR || tf == CM_TRANSFER_FUNCTION_EXT_LINEAR; };
    const bool SAME_GAMUT   = source.getPrimaries() == target.getPrimaries();
    const bool PRIMARIES_OK = SAME_GAMUT || (linear(source.transferFunction) && linear(target.transferFunction) && supportsCTM);

    return compatibleTF                                                   //
        && source.transferFunctionPower == target.transferFunctionPower   //
        && source.luminances == target.luminances                         //
        && source.encodingMinLuminance() == target.encodingMinLuminance() //
        && source.encodingMaxLuminance() == target.encodingMaxLuminance() //
        && contentFitsDisplayPeak(source, target) && PRIMARIES_OK;
}

bool NColorManagement::fullscreenCTMAllowed(eNoShader policy, bool scanout, bool frameCopied) {
    if (frameCopied)
        return false;

    switch (policy) {
        case CM_NS_ALWAYS: return true;
        case CM_NS_ONDEMAND: return scanout;
        default: return false;
    }
}

bool NColorManagement::shaderCMBypassed(bool monitorBypass, bool normalRenderMode, bool explicitScene) {
    return monitorBypass && normalRenderMode && !explicitScene;
}

eFullscreenCTM NColorManagement::fullscreenCTMWithoutConversion(const SFullscreenCTMInputs& inputs) {
    if (!inputs.fullscreen || inputs.policy == CM_NS_IGNORE)
        return FS_CTM_RESTORE;

    // only suppresses the composited user CTM (hyprsunset), the content still needs shader CM
    return !inputs.interop && inputs.userCTM ? FS_CTM_IDENTITY : FS_CTM_RESTORE;
}

eFullscreenCTM NColorManagement::decideFullscreenCTM(const SFullscreenCTMInputs& inputs) {
    if (!inputs.fullscreen || inputs.policy == CM_NS_IGNORE)
        return FS_CTM_RESTORE;

    // The CTM converts the source's pixels only. A direct scanout submits the solitary resource alone. A composited frame skips every shader
    // conversion, so every draw must already be in the encoding the CTM converts, and the work buffer must hold it unconverted.
    const bool WHOLE_FRAME = inputs.solitarySource && (inputs.scanout || (inputs.coversFrame && inputs.convertsToWork));

    if (inputs.needsCM && inputs.convertsToOutput && WHOLE_FRAME && fullscreenCTMAllowed(inputs.policy, inputs.scanout, inputs.frameCopied))
        return FS_CTM_CONVERT;

    return fullscreenCTMWithoutConversion(inputs);
}

bool NColorManagement::scanoutBlockedByCM(const SImageDescription& surface, const SImageDescription& output, const SScanoutCMState& state) {
    const bool SURFACE_IS_HDR = isHDR(surface);

    // These encodings require conversion to the output's PQ signalling.
    if (surface.transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR || surface.transferFunction == CM_TRANSFER_FUNCTION_HLG)
        return true;

    // The buffer must already use the encoding that is signalled to the display, even with non_shader_cm = ignore.
    if (!state.compatibleTF || SURFACE_IS_HDR != state.outputSignalsHDR)
        return SURFACE_IS_HDR || !state.hardwareCM;

    if (state.policy == CM_NS_IGNORE)
        return false;

    // SDR that needs CM while neither the CTM nor the display hardware converts it
    if (!SURFACE_IS_HDR)
        return state.needsCM && !state.ctmConverts && !state.hardwareCM;

    // Matching primaries need no CTM, but content brighter than the display still needs tone mapping.
    return !state.outputSignalsHDR || !hdrPassthroughCompatible(surface, output) || (surface.getPrimaries() != output.getPrimaries() && !state.ctmConverts);
}

SHDRMetadataDecision NColorManagement::decideHDRMetadata(const SHDRMetadataInputs& inputs, const std::function<bool()>& outputMetadataStaged) {
    if (!inputs.hdrSupported) {
        // HDR support was withdrawn while HDR was signalled (e.g. supports_hdr = -1), or a previous DRM master left HDR metadata behind:
        // the PQ metadata takes precedence over Colorspace in KMS, so it has to be cleared.
        if (!inputs.initialized || !outputMetadataStaged())
            return {.source = HDR_METADATA_OUTPUT, .force = true};
        return {.keepSurface = inputs.fromSurface};
    }

    if (inputs.scanoutCandidate) {
        // Admission of the next buffer stages its metadata, see CMonitor::attemptDirectScanout. Until then the output keeps what it
        // last committed, if that was this window's admitted surface. Never stage a candidate's metadata before admission.
        if (inputs.fromSurface && inputs.surfaceWindow && inputs.stagedHDR)
            return {.keepSurface = true, .initialized = false};
        return {.source = outputMetadataStaged() ? HDR_METADATA_KEEP : HDR_METADATA_OUTPUT, .initialized = false};
    }

    // Preserve the scanout surface's mastering metadata while the composited frame still shows that surface alone and unchanged. Anything
    // else (tone mapping, a gamut conversion, another draw) no longer has that mastering volume.
    if (inputs.fromSurface && inputs.wantHDR && inputs.stagedHDR && inputs.surfaceWindow && inputs.surfaceComposites)
        return {.source = HDR_METADATA_SURFACE, .keepSurface = true};

    // Composited frames have no known content light levels (CTA-861.3: 0 = unknown).
    // The first frame always stages SDR: a previous DRM master (e.g. a compositor that crashed in HDR) may have left HDR metadata on the
    // connector that the zero-initialized output state can't know about.
    const bool INITIAL_SDR = !inputs.initialized && !inputs.wantHDR;
    const bool HDR_CHANGED = inputs.stagedHDR != inputs.wantHDR;
    if (!HDR_CHANGED && !INITIAL_SDR && !inputs.fromSurface && (!inputs.wantHDR || outputMetadataStaged()))
        return {};

    if (INITIAL_SDR || !outputMetadataStaged())
        return {.source = HDR_METADATA_OUTPUT, .force = INITIAL_SDR};
    return {};
}
