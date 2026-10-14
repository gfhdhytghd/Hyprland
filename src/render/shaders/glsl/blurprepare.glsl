#ifndef ALLOW_INCLUDES
#define ALLOW_INCLUDES
#extension GL_ARB_shading_language_include : enable
#endif

#include "defines.h"

#if USE_CM
#include "cm_helpers.glsl"
#endif

#include "gain.glsl"

vec4 blurPrepare(vec4 pixColor, float contrast, float brightness
#if USE_CM
                 ,
                 int sourceTF, int targetTF, mat3 convertMatrix, vec2 srcTFRange, vec2 dstTFRange, float srcRefLuminance, float sdrBrightnessMultiplier, float sdrSaturation
#endif
) {
#if USE_CM
    float alpha = pixColor.a;
    pixColor.rgb /= max(alpha, 0.001);
    pixColor.rgb = sourceTF == CM_TRANSFER_FUNCTION_BT1886 ? tfInvBT1886(pixColor.rgb, srcTFRange) : toLinearRGB(pixColor.rgb, sourceTF);
    pixColor     = toNit(pixColor, srcTFRange);
    pixColor.rgb /= max(sdrBrightnessMultiplier, 0.001);
    float luminance = dot(pixColor.rgb, sourceLuminanceCoefficients);
    pixColor.rgb    = mix(vec3(luminance), pixColor.rgb, 1.0 / max(sdrSaturation, 0.001));
    pixColor.rgb    = convertMatrix * pixColor.rgb;
    pixColor.rgb *= alpha;
    pixColor = fromLinearNit(pixColor, targetTF, dstTFRange);
#endif

    // contrast
    if (contrast != 1.0)
        pixColor.rgb = gain(pixColor.rgb, contrast);

    // brightness
    pixColor.rgb *= max(1.0, brightness);

    return pixColor;
}
