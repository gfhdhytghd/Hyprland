#ifndef ALLOW_INCLUDES
#define ALLOW_INCLUDES
#extension GL_ARB_shading_language_include : enable
#endif
#include "constants.h"

// BT.2100 Table 7 RGB -> LMS (integers / 4096) composed with D65 XYZ -> BT.2020 RGB, so it applies to XYZ.
const mat3 XYZtoLMS = mat3(                                         //
    0.3592832590121218, -0.19208084637049927, 0.00707978446074772,  //
    0.6976051147779497, 1.1004767970374318, 0.07483966621863658,    //
    -0.0358915932320289, 0.07537486585191187, 0.8433265453898765    //
);
//const mat3 LMStoXYZ = inverse(XYZtoLMS);
const mat3 LMStoXYZ = mat3(                                         //
    2.0701522183894223, 0.36473852097480713, -0.04974720753581203,  //
    -1.3263473389671556, 0.6805660249472276, -0.04926096669661379,  //
    0.20665104762940512, -0.04530454592203474, 1.1880659249923042   //
);

// const mat3 ICtCpPQ = transpose(mat3(
//     2048.0, 2048.0, 0.0,
//     6610.0, -13613.0, 7003.0,
//     17933.0, -17390.0, -543.0
// ) / 4096.0);
const mat3 ICtCpPQ = mat3(                //
    0.5, 1.61376953125, 4.378173828125,   //
    0.5, -3.323486328125, -4.24560546875, //
    0.0, 1.709716796875, -0.132568359375  //
);
//const mat3 ICtCpPQInv = inverse(ICtCpPQ);
const mat3 ICtCpPQInv = mat3(                                                //
    1.0, 1.0, 1.0,                                                           //
    0.0086090370379327566, -0.0086090370379327566, 0.560031335710679118,     //
    0.11102962500302595656, -0.11102962500302595656, -0.32062717498731885185 //
);

// Tone mapping uses PQ ICtCp after conversion to absolute display light, including for HLG sources.

vec4 limitLuminance(vec4 color, mat3 dstXYZ, float peak) {
    float luminance = (dstXYZ * color.rgb).y;
    if (luminance > peak)
        color.rgb *= peak / luminance;
    return color;
}

float pqEncode(float luminance) {
    float E = pow(clamp(luminance / HDR_MAX_LUMINANCE, 0.0, 1.0), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * E) / (1.0 + PQ_C3 * E), PQ_M2);
}

float pqDecode(float signal) {
    float E = pow(clamp(signal, 0.0, 1.0), PQ_INV_M2);
    return pow(max(E - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * E), PQ_INV_M1) * HDR_MAX_LUMINANCE;
}

// ITU-R BT.2390 EETF on a PQ-encoded intensity, mapping source black/peak onto target black/peak.
float bt2390EETF(float signal, float srcMin, float srcMax, float dstMin, float dstMax) {
    float srcBlack = pqEncode(srcMin);
    float srcRange = pqEncode(srcMax) - srcBlack;
    float minLum   = (pqEncode(dstMin) - srcBlack) / srcRange;
    float maxLum   = (pqEncode(dstMax) - srcBlack) / srcRange;
    float E1       = clamp((signal - srcBlack) / srcRange, 0.0, 1.0);
    float KS       = 1.5 * maxLum - 0.5;
    float E2       = E1;
    if (E1 >= KS) {
        float T  = (E1 - KS) / (1.0 - KS);
        float T2 = T * T;
        float T3 = T2 * T;
        E2       = (2.0 * T3 - 3.0 * T2 + 1.0) * KS + (T3 - 2.0 * T2 + T) * (1.0 - KS) + (-2.0 * T3 + 3.0 * T2) * maxLum;
    }
    float E3 = E2 + minLum * pow(1.0 - E2, 4.0);
    return E3 * srcRange + srcBlack;
}

vec4 tonemap(vec4 color, mat3 dstXYZ, float srcMinLuminance, float maxLuminance, float dstMaxLuminance, float dstMinLuminance, int tonemapMode) {
    if (maxLuminance < dstMaxLuminance * 1.01 || tonemapMode == 2)
        return limitLuminance(color, dstXYZ, dstMaxLuminance);

    mat3  toLMS   = XYZtoLMS * dstXYZ;
    mat3  fromLMS = inverse(dstXYZ) * LMStoXYZ;

    vec3  lms   = fromLinear(vec4((toLMS * color.rgb) / HDR_MAX_LUMINANCE, 1.0), CM_TRANSFER_FUNCTION_ST2084_PQ).rgb;
    vec3  ICtCp = ICtCpPQ * lms;
    float I1    = ICtCp[0];

#if USE_ALT_TONEMAP
    // suggested by LionHeartP
    float luminance    = pqDecode(I1);
    float newLuminance = luminance;
    float kneeRatio    = clamp(dstMaxLuminance / maxLuminance, 0.5, 0.85);
    float knee         = dstMaxLuminance * kneeRatio;
    if (luminance > knee) {
        float alpha  = (maxLuminance - dstMaxLuminance) / ((maxLuminance - knee) * (dstMaxLuminance - knee));
        newLuminance = knee + (luminance - knee) / (1.0 + alpha * (luminance - knee));
    }
    ICtCp[0] = pqEncode(newLuminance);
#else
    ICtCp[0] = bt2390EETF(I1, srcMinLuminance, maxLuminance, dstMinLuminance, dstMaxLuminance);
    // BT.2390 keeps the saturation of mapped colors by scaling chroma with the intensity change.
    if (I1 > 0.0 && ICtCp[0] > 0.0)
        ICtCp.yz *= min(I1 / ICtCp[0], ICtCp[0] / I1);
#endif

    color = vec4(fromLMS * toLinear(vec4(ICtCpPQInv * ICtCp, 1.0), CM_TRANSFER_FUNCTION_ST2084_PQ).rgb * HDR_MAX_LUMINANCE, color[3]);

    return limitLuminance(color, dstXYZ, dstMaxLuminance);
}
