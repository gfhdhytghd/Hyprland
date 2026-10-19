#ifndef ALLOW_INCLUDES
#define ALLOW_INCLUDES
#extension GL_ARB_shading_language_include : enable
#endif
#ifndef CM_HELPERS_GLSL
#define CM_HELPERS_GLSL

#include "defines.h"
#include "constants.h"

uniform vec3  sourceLuminanceCoefficients;
uniform vec3  targetLuminanceCoefficients;
// Target / source reference white, already applied to the source luminance range.
uniform float sourceReferenceScale;
#if USE_TONEMAP
uniform mat3 targetXYZPrimaries;
// Source black after reference white anchoring, the BT.2390 EETF black input.
uniform float srcMinLuminance;
#endif
#if USE_MIRROR
// Target primaries -> BT.709 with target reference white scaled to SDR white.
uniform mat3  mirrorConvertMatrix;
// Peaks of the drawn source and of the target after that scaling.
uniform float mirrorMaxLuminance;
uniform float mirrorTargetMaxLuminance;

// BT.709 primaries to XYZ (D65) and its luminance row, used for the SDR unmodified copy.
const mat3 BT709toXYZ = mat3(        //
    0.4123908, 0.2126390, 0.0193308, //
    0.3575843, 0.7151687, 0.1191948, //
    0.1804808, 0.0721923, 0.9505322  //
);
const mat3 XYZtoBT709      = mat3(3.24096970749534, -0.9692435172196444, 0.05563015115599572, -1.53738281847333, 1.875967326610972, -0.2039770041610219, -0.4986107824000627,
                                  0.04155508813874787, 1.056971447040656);
const vec3 BT709_LUMINANCE = vec3(0.2126390, 0.7151687, 0.0721923);
#endif

#if USE_SDR_MOD
vec4 saturate(vec4 color, mat3 primaries, float saturation) {
    if (saturation == 1.0)
        return color;
    vec3  brightness = vec3(primaries[0][1], primaries[1][1], primaries[2][1]);
    float Y          = dot(color.rgb, brightness);
    return vec4(mix(vec3(Y), color.rgb, saturation), color[3]);
}
#endif

vec3 xy2xyz(vec2 xy) {
    if (xy.y == 0.0)
        return vec3(0.0, 0.0, 0.0);

    return vec3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y);
}

// The primary source for these transfer functions is https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1361-0-199802-W!!PDF-E.pdf
vec3 tfInvPQ(vec3 color) {
    vec3 E = pow(clamp(color.rgb, vec3(0.0), vec3(1.0)), vec3(PQ_INV_M2));
    return pow((max(E - PQ_C1, vec3(0.0))) / (PQ_C2 - PQ_C3 * E), vec3(PQ_INV_M1));
}

// BT.2100 HLG system gamma for a display with nominal peak luminance Lw; BT.2390 extends it outside 400-2000 cd/m².
float hlgGamma(float peak) {
    if (peak >= 400.0 && peak <= 2000.0)
        return 1.2 + 0.42 * log(peak / 1000.0) / log(10.0);
    return 1.2 * pow(1.111, log2(peak / 1000.0));
}

// BT.2100 HLG black level lift for a display with black luminance range.x, peak range.y and system gamma for nominalPeak.
float hlgBeta(vec2 range, float nominalPeak) {
    return sqrt(3.0 * pow(max(range.x, 0.0) / range.y, 1.0 / hlgGamma(nominalPeak)));
}

// BT.2100 HLG EOTF: signal -> display light, normalized to range like the other transfer functions.
// The system gamma follows the nominal peak of the display the signal was graded for, which is range.y unless the range was rescaled.
vec3 tfInvHLG(vec3 color, vec2 range, float nominalPeak) {
    float beta    = hlgBeta(range, nominalPeak);
    vec3  signal  = max((1.0 - beta) * color.rgb + beta, vec3(0.0));
    bvec3 isLow   = lessThanEqual(signal, vec3(HLG_E_CUT));
    vec3  lo      = signal * signal / 3.0;
    vec3  hi      = (exp((signal - HLG_C) / HLG_A) + HLG_B) / 12.0;
    vec3  scene   = mix(hi, lo, isLow);
    float Ys      = max(dot(scene, sourceLuminanceCoefficients), 0.0);
    // Scene black stays display black for any system gamma, including gamma < 1 where pow(0, gamma - 1) is undefined.
    vec3  display = Ys > 0.0 ? range.y * pow(Ys, hlgGamma(nominalPeak) - 1.0) * scene : vec3(0.0);
    return (display - range.x) / (range.y - range.x);
}

// Many transfer functions (including sRGB) follow the same pattern: a linear
// segment for small values and a power function for larger values. The
// following function implements this pattern from which sRGB, BT.709, and
// others can be derived by plugging in the right constants.
vec3 tfInvLinPow(vec3 color, float gamma, float thres, float scale, float alpha) {
    bvec3 isLow = lessThanEqual(color.rgb, vec3(thres * scale));
    vec3  lo    = color.rgb / scale;
    vec3  hi    = pow((color.rgb + alpha - 1.0) / alpha, vec3(gamma));
    return mix(hi, lo, isLow);
}

vec3 tfInvSRGB(vec3 color) {
    return tfInvLinPow(color, SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
}

vec3 tfInvExtSRGB(vec3 color) {
    // EXT sRGB is the sRGB transfer function mirrored around 0.
    return sign(color) * tfInvSRGB(abs(color));
}

vec3 tfInvBT1886(vec3 color, vec2 range) {
    float black = pow(max(range.x, 0.0), 1.0 / 2.4);
    float white = pow(range.y, 1.0 / 2.4);
    return (pow(max(color * (white - black) + black, vec3(0.0)), vec3(2.4)) - range.x) / (range.y - range.x);
}

vec3 tfInvXVYCC(vec3 color) {
    // The inverse transfer function for XVYCC is the BT.709 transfer function mirrored around 0,
    // same as what EXT sRGB is to sRGB.
    return sign(color) * tfInvLinPow(abs(color), BT709_POW, BT709_CUT, BT709_SCALE, BT709_ALPHA);
}

vec3 tfInvST240(vec3 color) {
    return tfInvLinPow(color, ST240_POW, ST240_CUT, ST240_SCALE, ST240_ALPHA);
}

// Forward transfer functions corresponding to the inverse functions above.
vec3 tfPQ(vec3 color) {
    vec3 E = pow(clamp(color.rgb, vec3(0.0), vec3(1.0)), vec3(PQ_M1));
    return pow((vec3(PQ_C1) + PQ_C2 * E) / (vec3(1.0) + PQ_C3 * E), vec3(PQ_M2));
}

// Inverse BT.2100 HLG EOTF: normalized display light -> signal.
vec3 tfHLG(vec3 color, vec2 range) {
    float gamma   = hlgGamma(range.y);
    float beta    = hlgBeta(range, range.y);
    vec3  display = max(color.rgb * (range.y - range.x) + range.x, vec3(0.0)) / range.y;
    float Yd      = max(dot(display, targetLuminanceCoefficients), 0.0);
    vec3  scene   = Yd > 0.0 ? display * pow(Yd, (1.0 - gamma) / gamma) : vec3(0.0);
    bvec3 isLow   = lessThanEqual(scene, vec3(HLG_D_CUT));
    vec3  lo      = sqrt(scene * 3.0);
    vec3  hi      = HLG_A * log(max(12.0 * scene - HLG_B, vec3(0.0001))) + HLG_C;
    return (mix(hi, lo, isLow) - beta) / (1.0 - beta);
}

vec3 tfLinPow(vec3 color, float gamma, float thres, float scale, float alpha) {
    bvec3 isLow = lessThanEqual(color.rgb, vec3(thres));
    vec3  lo    = color.rgb * scale;
    vec3  hi    = pow(color.rgb, vec3(1.0 / gamma)) * alpha - (alpha - 1.0);
    return mix(hi, lo, isLow);
}

vec3 tfSRGB(vec3 color) {
    return tfLinPow(color, SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
}

vec3 tfExtSRGB(vec3 color) {
    // EXT sRGB is the sRGB transfer function mirrored around 0.
    return sign(color) * tfSRGB(abs(color));
}

vec3 applyIcc3DLut(vec3 linearRgb01, highp sampler3D iccLut3D, float iccLutSize) {
    // the LUT is indexed by sRGB-encoded values, see buildIcc3DLut
    vec3 x = tfSRGB(clamp(linearRgb01, 0.0, 1.0));

    // Map [0..1] to texel centers to avoid edge issues
    float N     = iccLutSize;
    vec3  coord = (x * (N - 1.0) + 0.5) / N;

    return texture(iccLut3D, coord).rgb;
}

vec3 tfBT1886(vec3 color, vec2 range) {
    float black = pow(max(range.x, 0.0), 1.0 / 2.4);
    float white = pow(range.y, 1.0 / 2.4);
    return (pow(max(color * (range.y - range.x) + range.x, vec3(0.0)), vec3(1.0 / 2.4)) - black) / (white - black);
}

vec3 tfXVYCC(vec3 color) {
    // The transfer function for XVYCC is the BT.709 transfer function mirrored around 0,
    // same as what EXT sRGB is to sRGB.
    return sign(color) * tfLinPow(abs(color), BT709_POW, BT709_CUT, BT709_SCALE, BT709_ALPHA);
}

vec3 tfST240(vec3 color) {
    return tfLinPow(color, ST240_POW, ST240_CUT, ST240_SCALE, ST240_ALPHA);
}

vec3 toLinearRGB(vec3 color, int tf) {
    switch (tf) {
        case CM_TRANSFER_FUNCTION_LINEAR: return color;
        case CM_TRANSFER_FUNCTION_EXT_LINEAR: return color;
        case CM_TRANSFER_FUNCTION_ST2084_PQ: return tfInvPQ(color);
        case CM_TRANSFER_FUNCTION_GAMMA22: return pow(max(color, vec3(0.0)), vec3(2.2));
        case CM_TRANSFER_FUNCTION_GAMMA28: return pow(max(color, vec3(0.0)), vec3(2.8));
        case CM_TRANSFER_FUNCTION_HLG: return tfInvHLG(color, vec2(HDR_MIN_LUMINANCE, HLG_MAX_LUMINANCE), HLG_MAX_LUMINANCE);
        case CM_TRANSFER_FUNCTION_EXT_SRGB: return tfInvExtSRGB(color);
        case CM_TRANSFER_FUNCTION_BT1886: return tfInvBT1886(color, vec2(0.01, 100.0));
        case CM_TRANSFER_FUNCTION_ST240: return tfInvST240(color);
        case CM_TRANSFER_FUNCTION_LOG_100: return mix(exp((color - 1.0) * 2.0 * log(10.0)), vec3(0.0), lessThanEqual(color, vec3(0.0)));
        case CM_TRANSFER_FUNCTION_LOG_316: return mix(exp((color - 1.0) * 2.5 * log(10.0)), vec3(0.0), lessThanEqual(color, vec3(0.0)));
        case CM_TRANSFER_FUNCTION_XVYCC: return tfInvXVYCC(color);
        case CM_TRANSFER_FUNCTION_ST428: return pow(max(color, vec3(0.0)), vec3(ST428_POW)) * ST428_SCALE;
        case CM_TRANSFER_FUNCTION_SRGB:
        default: return tfInvSRGB(color);
    }
}

// Display-referred transfer functions depend on the source luminance range, which carries the reference white scale.
vec3 toLinearRGB(vec3 color, int tf, vec2 range) {
    if (tf == CM_TRANSFER_FUNCTION_BT1886)
        return tfInvBT1886(color, range);
    if (tf == CM_TRANSFER_FUNCTION_HLG)
        return tfInvHLG(color, range, sourceReferenceScale > 0.0 ? range.y / sourceReferenceScale : range.y);
    return toLinearRGB(color, tf);
}

vec4 toLinear(vec4 color, int tf) {
    if (tf == CM_TRANSFER_FUNCTION_EXT_LINEAR)
        return color;

    color.rgb /= max(color.a, 0.001);
    color.rgb = toLinearRGB(color.rgb, tf);
    color.rgb *= color.a;
    return color;
}

vec4 toNit(vec4 color, vec2 range) {
    color.rgb = color.rgb * (range[1] - range[0]) + range[0];
    return color;
}

vec3 fromLinearRGB(vec3 color, int tf) {
    switch (tf) {
        case CM_TRANSFER_FUNCTION_EXT_LINEAR: return color;
        case CM_TRANSFER_FUNCTION_ST2084_PQ: return tfPQ(color);
        case CM_TRANSFER_FUNCTION_GAMMA22: return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
        case CM_TRANSFER_FUNCTION_GAMMA28: return pow(max(color, vec3(0.0)), vec3(1.0 / 2.8));
        case CM_TRANSFER_FUNCTION_HLG: return tfHLG(color, vec2(HDR_MIN_LUMINANCE, HLG_MAX_LUMINANCE));
        case CM_TRANSFER_FUNCTION_EXT_SRGB: return tfExtSRGB(color);
        case CM_TRANSFER_FUNCTION_BT1886: return tfBT1886(color, vec2(0.01, 100.0));
        case CM_TRANSFER_FUNCTION_ST240: return tfST240(color);
        case CM_TRANSFER_FUNCTION_LOG_100: return mix(1.0 + log(color) / log(10.0) / 2.0, vec3(0.0), lessThanEqual(color, vec3(0.01)));
        case CM_TRANSFER_FUNCTION_LOG_316: return mix(1.0 + log(color) / log(10.0) / 2.5, vec3(0.0), lessThanEqual(color, vec3(sqrt(10.0) / 1000.0)));
        case CM_TRANSFER_FUNCTION_XVYCC: return tfXVYCC(color);
        case CM_TRANSFER_FUNCTION_ST428: return pow(max(color, vec3(0.0)) / ST428_SCALE, vec3(1.0 / ST428_POW));
        case CM_TRANSFER_FUNCTION_SRGB:
        default: return tfSRGB(color);
    }
}

vec4 fromLinear(vec4 color, int tf) {
    if (tf == CM_TRANSFER_FUNCTION_EXT_LINEAR || tf == CM_TRANSFER_FUNCTION_LINEAR)
        return color;

    color.rgb /= max(color.a, 0.001);
    color.rgb = fromLinearRGB(color.rgb, tf);
    color.rgb *= color.a;
    return color;
}

vec4 fromLinearNit(vec4 color, int tf, vec2 range) {
    if (tf == CM_TRANSFER_FUNCTION_LINEAR)
        return color;

    color.rgb = (color.rgb - range[0] * color.a) / (range[1] - range[0]); // @gulafaran
    color.rgb /= max(color.a, 0.001);
    color.rgb = tf == CM_TRANSFER_FUNCTION_BT1886 ? tfBT1886(color.rgb, range) : (tf == CM_TRANSFER_FUNCTION_HLG ? tfHLG(color.rgb, range) : fromLinearRGB(color.rgb, tf));
    color.rgb *= color.a;
    return color;
}

#if USE_TONEMAP || (USE_MIRROR && !USE_ICC)
#include "tonemap.glsl"
#endif

// Hue-preserving gamut clipping (BT.2407): out-of-range colors are desaturated towards the
// achromatic color of the same luminance, which moves them along a straight line to the white point.
vec3 mapToGamut(vec3 color, vec2 range, vec3 luminanceCoefficients) {
    if (all(greaterThanEqual(color, vec3(range.x))) && all(lessThanEqual(color, vec3(range.y))))
        return color;

    float Y      = clamp(dot(color, luminanceCoefficients), range.x, range.y);
    vec3  chroma = color - Y;
    float s      = 1.0;
    for (int i = 0; i < 3; ++i) {
        if (chroma[i] < 0.0)
            s = min(s, (Y - range.x) / -chroma[i]);
        else if (chroma[i] > 0.0)
            s = min(s, (range.y - Y) / chroma[i]);
    }
    return vec3(Y) + max(s, 0.0) * chroma;
}

#if USE_MIRROR && !USE_ICC
// Converts linear target light in nits to the BT.709 SDR unmodified copy, premultiplied and sRGB encoded.
// Highlights above SDR white are mapped by the BT.2390 EETF, then out-of-gamut colors are clipped preserving hue.
vec4 toMirror(vec3 nits, float peak, float alpha) {
    vec2 range = vec2(SDR_MIN_LUMINANCE, SDR_MAX_LUMINANCE);
    vec4 color = tonemap(vec4(mirrorConvertMatrix * nits, 1.0), BT709toXYZ, XYZtoBT709, 0.0, peak, range.y, range.x, 1);
    color.rgb  = mapToGamut(color.rgb, range, BT709_LUMINANCE);
    return fromLinearNit(vec4(color.rgb * alpha, alpha), CM_TRANSFER_FUNCTION_SRGB, range);
}

// Converts an opaque color in the target encoding, e.g. the blurred background, to the unmodified copy.
vec3 targetToMirror(vec3 color, int dstTF, vec2 dstTFRange) {
    color = dstTF == CM_TRANSFER_FUNCTION_BT1886 ? tfInvBT1886(color, dstTFRange) : toLinearRGB(color, dstTF);
    if (dstTF != CM_TRANSFER_FUNCTION_LINEAR)
        color = toNit(vec4(color, 1.0), dstTFRange).rgb;
    return toMirror(color, mirrorTargetMaxLuminance, 1.0).rgb;
}
#endif

#if USE_MIRROR
vec4[2]
#else
vec4
#endif
    doColorManagement(vec4 pixColor, float additionalAlpha, int srcTF, int dstTF, mat3 convertMatrix, vec2 srcTFRange, vec2 dstTFRange, float linearNoise
#if USE_ICC
                      ,
                      highp sampler3D iccLut3D, float iccLutSize
#else
#if USE_TONEMAP || USE_SDR_MOD
                       ,
                       mat3 dstxyz
#endif
#if USE_TONEMAP
                       ,
                       float maxLuminance, float dstMaxLuminance, float dstRefLuminance, int tonemapMode
#endif
#if USE_SDR_MOD
                       ,
                       float sdrSaturation, float sdrBrightnessMultiplier
#endif
#endif
    ) {
    float sourceAlpha = pixColor.a;
    float finalAlpha  = sourceAlpha * additionalAlpha;

    pixColor.rgb /= max(sourceAlpha, 0.001);
    pixColor.rgb = toLinearRGB(pixColor.rgb, srcTF, srcTFRange);
    pixColor.rgb += linearNoise;

#if USE_ICC
    pixColor.rgb = applyIcc3DLut(pixColor.rgb, iccLut3D, iccLutSize);
    pixColor.a   = finalAlpha;
    pixColor.rgb *= pixColor.a;
#if USE_MIRROR
    vec4 mirrorColor = pixColor;
#endif
#else
    pixColor.rgb = convertMatrix * pixColor.rgb;
    if (srcTF != CM_TRANSFER_FUNCTION_LINEAR)
        pixColor = toNit(pixColor, srcTFRange);
#if USE_MIRROR
    // The unmodified copy is a BT.709 SDR frame taken before the SDR brightness and saturation adjustments.
    vec4 mirrorColor = toMirror(pixColor.rgb, mirrorMaxLuminance, finalAlpha);
#endif
#if USE_SDR_MOD
    // SDR adjustments come before tone mapping so the adjusted signal still fits the target volume.
    pixColor = saturate(pixColor, dstxyz, sdrSaturation);
    pixColor.rgb *= sdrBrightnessMultiplier;
#endif
#if USE_TONEMAP
    pixColor = tonemap(pixColor, dstxyz, targetXYZPrimaries, srcMinLuminance, maxLuminance, dstMaxLuminance, dstTFRange.x, tonemapMode);
#endif
    // Linear targets are intermediate buffers that keep extended values; encoded targets get gamut mapped.
    if (dstTF != CM_TRANSFER_FUNCTION_LINEAR && dstTF != CM_TRANSFER_FUNCTION_EXT_LINEAR)
        pixColor.rgb = mapToGamut(pixColor.rgb, dstTFRange, targetLuminanceCoefficients);
    pixColor.a = finalAlpha;
    pixColor.rgb *= finalAlpha;
    pixColor = fromLinearNit(pixColor, dstTF, dstTFRange);
#endif

#if USE_MIRROR
    vec4[2] pixColors;
    pixColors[0] = pixColor;
    pixColors[1] = mirrorColor;
    return pixColors;
#else
    return pixColor;
#endif
}

#endif
