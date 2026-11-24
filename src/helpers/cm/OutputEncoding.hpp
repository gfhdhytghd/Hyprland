#pragma once

#include "../CMType.hpp"
#include "ColorManagement.hpp"

#include <cstdint>
#include <functional>
#include <optional>

// Per-output colour mode decisions. They are pure so that the image description, the connector signalling, the preferred
// description offered to clients, direct scanout admission and the shader bypass all follow one decision table.
namespace NColorManagement {
    // What the output and the renderer can do, independent of the monitor rule. Snapshot once per frame.
    struct SOutputColorCapabilities {
        bool shaderCM           = true;  // render:cm_enabled, and a renderer with CM shaders. Without them pixels reach the output unconverted.
        bool enabled10bit       = false; // the output runs a 10 bpc format
        bool bt2020Signal       = false; // CMonitor::canSignalBT2020: a DRM connector listing BT2020_RGB, or a backend without colorimetry signalling
        bool wideColor          = false; // CMonitor::supportsWideColor: BT.2020 can be signalled and the sink (or supports_wide_color = 1) accepts it
        bool hdr                = false; // CMonitor::supportsHDR
        bool edidChromaticities = false; // the EDID lists the panel primaries
    };

    // Why the output doesn't encode as configured, or as auto HDR would.
    enum eCMFallback : uint8_t {
        CM_FALLBACK_NONE = 0,
        CM_FALLBACK_CAPABILITY,            // the output can't signal the preset at all (no BT2020_RGB, no HDR support)
        CM_FALLBACK_NO_SHADER_CM,          // render:cm_enabled = false: PQ without the shader conversion into it
        CM_FALLBACK_AUTO_HDR_NO_SHADER_CM, // the same for the PQ preset auto HDR would switch to for the fullscreen HDR content
    };

    struct SConfiguredCMType {
        NCMType::eCMType cmType   = NCMType::CM_SRGB;
        eCMFallback      fallback = CM_FALLBACK_NONE;

        bool             operator==(const SConfiguredCMType&) const = default;
    };

    enum eFullscreenContent : uint8_t {
        FS_CONTENT_NONE = 0, // no fullscreen window, or one without a committed image description
        FS_CONTENT_SDR,
        FS_CONTENT_HDR, // see isHDR(const SImageDescription&)
    };

    struct SOutputEncodingInputs {
        NCMType::eCMType         rule       = NCMType::CM_SRGB; // the cm of the last successfully applied monitor rule, before capability fallbacks
        bool                     iccProfile = false;            // the installed output description is an ICC profile, which overrides cm
        SOutputColorCapabilities caps;
        int64_t                  autoHDR    = 0;     // render:cm_auto_hdr, 0 off, 1 hdr, 2 hdredid
        bool                     fullscreen = false; // a fullscreen window is presented on the output
        bool                     noAutoHDR  = false; // the fullscreen window's no_auto_hdr rule
        eFullscreenContent       content    = FS_CONTENT_NONE;
    };

    struct SOutputEncoding {
        bool             icc      = false;            // the ICC profile defines the encoding, cmType is unused
        NCMType::eCMType baseline = NCMType::CM_SRGB; // the configured preset after capability fallbacks
        NCMType::eCMType cmType   = NCMType::CM_SRGB; // the preset the output encodes with
        bool             autoHDR  = false;            // cmType is an HDR preset selected by auto HDR
        eCMFallback      fallback = CM_FALLBACK_NONE; // why baseline differs from the configured preset, or why auto HDR didn't switch

        // PQ encoding, HDR10 metadata and BT.2020 colorimetry
        bool hdr() const;
        bool operator==(const SOutputEncoding&) const = default;
    };

    // The preset a monitor rule's cm resolves to. Presets the output or the renderer can't honour fall back to sRGB, which
    // is what unconverted pixels and default colorimetry mean.
    SConfiguredCMType resolveConfiguredCMType(NCMType::eCMType rule, const SOutputColorCapabilities& caps);

    // The HDR preset auto HDR switches to while the fullscreen window shows HDR content, if auto HDR may act on this output.
    std::optional<NCMType::eCMType> autoHDRCMType(const SOutputEncodingInputs& inputs);
    // Whether auto HDR would act on this output with shader CM, ignoring the window's no_auto_hdr rule and content: only then do the
    // rule and the content matter, and the surface tree has to be searched for the content's description.
    bool            autoHDRApplicable(const SOutputEncodingInputs& inputs);

    SOutputEncoding resolveOutputEncoding(const SOutputEncodingInputs& inputs);

    // Whether the output description has to be rebuilt for the encoding. appliedCMType is the preset the current description was built from.
    bool               outputEncodingNeedsApply(const SOutputEncoding& encoding, std::optional<NCMType::eCMType> appliedCMType, bool descriptionIsICC);

    eFullscreenContent classifyFullscreenContent(const PImageDescription& description);

    // The brightest light the description's content can contain, and the brightest light the target can show.
    float contentPeakLuminance(const SImageDescription& description);
    float displayPeakLuminance(const SImageDescription& description);
    // The one boundary between content that fits the target and content the shader tone maps. Shader CM (CCMSettingsCache),
    // CImageDescription::needsCM, direct scanout admission and the CTM bypass all use it.
    bool peakNeedsToneMapping(float sourcePeak, float targetPeak);
    // Content brighter than the target needs tone mapping, which neither a CTM nor the sink is asked to do.
    bool contentFitsDisplayPeak(const SImageDescription& source, const SImageDescription& target);
    // PQ (or HLG) pixels with the target's transfer function and luminances that need no tone mapping. Direct scanout additionally needs
    // matching primaries or a CTM that converts them.
    bool hdrPassthroughCompatible(const SImageDescription& source, const SImageDescription& target);
    // PQ pixels that shader CM passes to the target unchanged: same transfer function, luminances and primaries, no tone mapping. A frame
    // that shows only such a surface carries the surface's own mastering metadata.
    bool compositeReproducesSurface(const SImageDescription& surface, const SImageDescription& target);

    // Whether the KMS CTM alone converts source pixels to the target encoding. The CTM runs between the (unset) degamma and gamma LUTs,
    // so it can only convert primaries of linear values. compatibleTF is the caller's transfer function equivalence, see isCompatibleTF.
    bool ctmConvertsExactly(const SImageDescription& source, const SImageDescription& target, bool compatibleTF, bool supportsCTM);

    // The fullscreen CTM makes the shaders skip every conversion of composited frames. That is only valid when the frame goes nowhere but
    // through the CTM to the display: a copy for capture or mirrors would hold unconverted pixels.
    bool fullscreenCTMAllowed(eNoShader policy, bool scanout, bool frameCopied);

    // Whether a draw skips shader CM because the monitor's fullscreen CTM converts it. Only the output frame itself goes through the
    // CTM, captures and other render targets keep their conversion.
    bool shaderCMBypassed(bool monitorBypass, bool normalRenderMode, bool explicitScene);

    // Every input the fullscreen CTM decision depends on, evaluated. A changed member requires a new decision.
    struct SFullscreenCTMInputs {
        eNoShader policy           = CM_NS_IGNORE; // render:non_shader_cm
        bool      fullscreen       = false;        // a fullscreen window is presented
        uint64_t  source           = 0;            // its description id
        bool      solitarySource   = false;        // the source is the solitary resource: the buffer scanout submits, the pixels a solitary frame shows
        uint64_t  target           = 0;            // the output description id
        bool      interop          = false;        // render:non_shader_cm_interop keeps the user CTM for this window (content type included)
        bool      userCTM          = false;        // a non-identity user CTM is set
        bool      needsCM          = false;        // the fullscreen description differs from the output description
        bool      convertsToOutput = false;        // the CTM converts it into the output encoding, see CMonitor::canNoShaderCM
        bool      scanout          = false;        // the previous frame was a direct scanout
        bool      convertsToWork   = false;        // composited: the CTM also converts it into the work buffer encoding the frame is drawn in
        bool      coversFrame      = false;        // composited: the fullscreen surface is the only thing drawn (solitary, no software cursor)
        bool      frameCopied      = false;        // composited frames are copied for capture, mirrors or the unmodified copy

        bool      operator==(const SFullscreenCTMInputs&) const = default;
    };

    enum eFullscreenCTM : uint8_t {
        FS_CTM_RESTORE = 0, // the user CTM applies, shader CM converts everything
        FS_CTM_CONVERT,     // the fullscreen CTM converts the frame, the shaders skip their conversion
        FS_CTM_IDENTITY,    // the user CTM is suppressed (non_shader_cm_interop), shader CM converts everything
    };

    eFullscreenCTM decideFullscreenCTM(const SFullscreenCTMInputs& inputs);
    // The decision when the conversion matrix can't be staged
    eFullscreenCTM fullscreenCTMWithoutConversion(const SFullscreenCTMInputs& inputs);

    // Direct scanout admission, colour part: whether the buffer would reach the display in another encoding than the signalled one.
    struct SScanoutCMState {
        eNoShader policy           = CM_NS_IGNORE;
        bool      outputSignalsHDR = false; // the staged metadata signals PQ, see CMonitor::inHDR
        bool      compatibleTF     = false; // the buffer's transfer function matches the output's, see isCompatibleTF
        bool      hardwareCM       = false; // the display hardware converts this SDR buffer exactly, see CMonitor::hardwareCMFor
        bool      needsCM          = false; // the fullscreen description differs from the output description
        bool      ctmConverts      = false; // the CTM converts the fullscreen description into the output encoding
    };
    bool scanoutBlockedByCM(const SImageDescription& surface, const SImageDescription& output, const SScanoutCMState& state);

    // HDR metadata staged on the output before drawing. The renderer applies the decision; scanout admission stages the surface's own
    // metadata afterwards, see CMonitor::attemptDirectScanout.
    struct SHDRMetadataInputs {
        bool hdrSupported      = false; // CMonitor::supportsHDR
        bool wantHDR           = false; // SOutputEncoding::hdr
        bool stagedHDR         = false; // the staged metadata signals PQ
        bool initialized       = false; // a first frame already staged metadata (CMonitor::m_needsHDRupdate)
        bool scanoutCandidate  = false; // the previous frame was a scanout and the solitary surface is HDR: admission of its buffer follows
        bool fromSurface       = false; // the staged metadata is a scanout surface's
        bool surfaceWindow     = false; // ... of the presented fullscreen window
        bool surfaceComposites = false; // the composited frame shows only the solitary surface, see compositeReproducesSurface
    };

    enum eHDRMetadataSource : uint8_t {
        HDR_METADATA_KEEP = 0, // leave the staged metadata
        HDR_METADATA_OUTPUT,   // the output description's (SDR metadata without wantHDR)
        HDR_METADATA_SURFACE,  // the fullscreen surface's mastering metadata, content light levels unknown
    };

    struct SHDRMetadataDecision {
        eHDRMetadataSource source      = HDR_METADATA_KEEP;
        bool               force       = false; // stage even if equal: a previous DRM master may have left other metadata
        bool               keepSurface = false; // the staged metadata stays attributed to the scanout surface
        bool               initialized = true;

        bool               operator==(const SHDRMetadataDecision&) const = default;
    };

    // outputMetadataStaged: whether the staged metadata equals selectHDRMetadata(output, wantHDR). Evaluated only when the decision depends on it.
    SHDRMetadataDecision decideHDRMetadata(const SHDRMetadataInputs& inputs, const std::function<bool()>& outputMetadataStaged);
}
