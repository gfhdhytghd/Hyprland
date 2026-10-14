#include "ColorManagement.hpp"
#include "../../macros.hpp"
#include "../Color.hpp"
#include "../TransferFunction.hpp"
#include "../../render/Renderer.hpp"
#include "render/types.hpp"
#include <hyprutils/memory/UniquePtr.hpp>
#include <map>
#include <vector>
#include <numeric>
#include <numbers>

using namespace NColorManagement;
using namespace NTransferFunction;

namespace NColorManagement {
    // expected to be small
    static std::vector<UP<const CPrimaries>>                       knownPrimaries;
    static std::vector<UP<const CImageDescription>>                knownDescriptions;
    static std::map<std::pair<uint, uint>, Hyprgraphics::CMatrix3> primariesConversion;
}

const SPCPRimaries& NColorManagement::getPrimaries(ePrimaries name) {
    switch (name) {
        case CM_PRIMARIES_SRGB: return NColorPrimaries::BT709;
        case CM_PRIMARIES_BT2020: return NColorPrimaries::BT2020;
        case CM_PRIMARIES_PAL_M: return NColorPrimaries::PAL_M;
        case CM_PRIMARIES_PAL: return NColorPrimaries::PAL;
        case CM_PRIMARIES_NTSC: return NColorPrimaries::NTSC;
        case CM_PRIMARIES_GENERIC_FILM: return NColorPrimaries::GENERIC_FILM;
        case CM_PRIMARIES_CIE1931_XYZ: return NColorPrimaries::CIE1931_XYZ;
        case CM_PRIMARIES_DCI_P3: return NColorPrimaries::DCI_P3;
        case CM_PRIMARIES_DISPLAY_P3: return NColorPrimaries::DISPLAY_P3;
        case CM_PRIMARIES_ADOBE_RGB: return NColorPrimaries::ADOBE_RGB;
        default: return NColorPrimaries::DEFAULT_PRIMARIES;
    }
}

CPrimaries::CPrimaries(const SPCPRimaries& primaries, const uint32_t primariesId) : m_id(primariesId), m_primaries(primaries) {
    const auto matrix = rgbToXYZFromPrimaries(m_primaries);
    if (!matrix) {
        m_primaries2XYZ = Hyprgraphics::CMatrix3::identity();
        return;
    }
    const auto values = matrix->getMatrix();
    m_primaries2XYZ   = Hyprgraphics::CMatrix3({{{values[0], values[1], values[2]}, {values[3], values[4], values[5]}, {values[6], values[7], values[8]}}});
}

WP<const CPrimaries> CPrimaries::from(const SPCPRimaries& primaries) {
    for (const auto& known : knownPrimaries) {
        if (known->value() == primaries)
            return known;
    }

    knownPrimaries.emplace_back(CUniquePointer(new CPrimaries(primaries, knownPrimaries.size() + 1)));
    return knownPrimaries.back();
}

WP<const CPrimaries> CPrimaries::from(const ePrimaries name) {
    return from(getPrimaries(name));
}

WP<const CPrimaries> CPrimaries::from(const uint32_t primariesId) {
    ASSERT(primariesId <= knownPrimaries.size());
    return knownPrimaries[primariesId - 1];
}

const SPCPRimaries& CPrimaries::value() const {
    return m_primaries;
}

uint CPrimaries::id() const {
    return m_id;
}

const Hyprgraphics::CMatrix3& CPrimaries::toXYZ() const {
    return m_primaries2XYZ;
}

const Hyprgraphics::CMatrix3& CPrimaries::convertMatrix(const WP<const CPrimaries> dst) const {
    const auto cacheKey = std::pair{m_id, dst->m_id};
    if (!primariesConversion.contains(cacheKey))
        primariesConversion.emplace(cacheKey, dst->toXYZ().invert() * Hyprgraphics::adaptWhite(m_primaries.white, dst->m_primaries.white) * toXYZ());

    return primariesConversion[cacheKey];
}

CImageDescription::CImageDescription(const SImageDescription& imageDescription, const uint64_t imageDescriptionId) :
    m_id(imageDescriptionId), m_imageDescription(imageDescription) {
    m_primariesId = CPrimaries::from(m_imageDescription.getPrimaries())->id();
}

PImageDescription CImageDescription::from(const SImageDescription& imageDescription) {
    auto description = imageDescription;
    // PQ's primary volume is its encoding range, not the mastering display peak.
    if (description.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ)
        description.luminances.max = std::round(description.luminances.min + HDR_MAX_LUMINANCE);
    for (const auto& known : knownDescriptions) {
        if (known->value() == description)
            return known;
    }

    knownDescriptions.emplace_back(UP<CImageDescription>(new CImageDescription(description, knownDescriptions.size() + 1)));
    return knownDescriptions.back();
}

PImageDescription CImageDescription::from(const uint64_t imageDescriptionId) {
    ASSERT(imageDescriptionId <= knownDescriptions.size());
    return knownDescriptions[imageDescriptionId - 1];
}

PImageDescription CImageDescription::with(const SImageDescription::SPCLuminances& luminances) const {
    auto desc       = m_imageDescription;
    desc.luminances = luminances;
    return CImageDescription::from(desc);
}

const SImageDescription& CImageDescription::value() const {
    return m_imageDescription;
}

uint64_t CImageDescription::id() const {
    return m_id;
}

WP<const CPrimaries> CImageDescription::getPrimaries() const {
    return CPrimaries::from(m_primariesId);
}

bool CImageDescription::needsCM(WP<const CImageDescription> target) const {
    if (m_id == target->m_id)
        return false;

    const auto  sourcePeak      = m_imageDescription.maxCLL > 0 ?
        m_imageDescription.maxCLL :
        (m_imageDescription.masteringLuminances.max > 0 ? m_imageDescription.masteringLuminances.max : m_imageDescription.luminances.max);
    const auto& destination     = target->m_imageDescription;
    const auto  destinationPeak = destination.masteringLuminances.max > 0 ? destination.masteringLuminances.max : destination.luminances.max;

    return sourcePeak >= destinationPeak * 1.01f || m_imageDescription.icc.present || target->m_imageDescription.icc.present // TODO compare ICC
        || m_imageDescription.isWindows != target->m_imageDescription.isWindows || m_imageDescription.isInternal != target->m_imageDescription.isInternal ||
        m_imageDescription.transferFunction != target->m_imageDescription.transferFunction //
        //|| m_imageDescription.transferFunctionPower != target->m_imageDescription.transferFunctionPower // TODO unsupported
        || m_imageDescription.getPrimaries() != target->m_imageDescription.getPrimaries() //
        // || m_imageDescription.masteringPrimaries != target->m_imageDescription.masteringPrimaries // TODO unused
        || m_imageDescription.luminances != target->m_imageDescription.luminances //
        // || m_imageDescription.masteringLuminances != target->m_imageDescription.masteringLuminances // TODO unused
        ;
}

static Mat3x3 diag3(const std::array<float, 3>& s) {
    return Mat3x3{std::array<float, 9>{s[0], 0, 0, 0, s[1], 0, 0, 0, s[2]}};
}

static std::optional<Mat3x3> invertMat3(const Mat3x3& m) {
    const auto   ARR = m.getMatrix();
    const double a = ARR[0], b = ARR[1], c = ARR[2];
    const double d = ARR[3], e = ARR[4], f = ARR[5];
    const double g = ARR[6], h = ARR[7], i = ARR[8];

    const double A = (e * i - f * h);
    const double B = -(d * i - f * g);
    const double C = (d * h - e * g);
    const double D = -(b * i - c * h);
    const double E = (a * i - c * g);
    const double F = -(a * h - b * g);
    const double G = (b * f - c * e);
    const double H = -(a * f - c * d);
    const double I = (a * e - b * d);

    const double det = a * A + b * B + c * C;
    if (std::abs(det) < 1e-18)
        return std::nullopt;

    const double invDet = 1.0 / det;
    Mat3x3       inv{std::array<float, 9>{
        A * invDet,
        D * invDet,
        G * invDet, //
        B * invDet,
        E * invDet,
        H * invDet, //
        C * invDet,
        F * invDet,
        I * invDet, //
    }};
    return inv;
}

static std::array<float, 3> matByVec(const Mat3x3& M, const std::array<float, 3>& v) {
    const auto ARR = M.getMatrix();
    return {ARR[0] * v[0] + ARR[1] * v[1] + ARR[2] * v[2], ARR[3] * v[0] + ARR[4] * v[1] + ARR[5] * v[2], ARR[6] * v[0] + ARR[7] * v[1] + ARR[8] * v[2]};
}

std::optional<Mat3x3> NColorManagement::rgbToXYZFromPrimaries(SPCPRimaries pr) {
    // Homogeneous primary coordinates also represent XYZ basis primaries with y=0.
    const auto primary = [](Hyprgraphics::CColor::xy xy) { return Hyprgraphics::CColor::XYZ{.x = xy.x, .y = xy.y, .z = 1.0 - xy.x - xy.y}; };
    const auto R       = primary(pr.red);
    const auto G       = primary(pr.green);
    const auto B       = primary(pr.blue);
    if (pr.white.y <= 0)
        return std::nullopt;
    const auto W = Hyprgraphics::xy2xyz(pr.white);

    // P has columns R,G,B
    Mat3x3 P{std::array<float, 9>{R.x, G.x, B.x, R.y, G.y, B.y, R.z, G.z, B.z}};

    auto   invP = invertMat3(P);
    if (!invP)
        return std::nullopt;

    const auto S = matByVec(*invP, {W.x, W.y, W.z});
    // White must be a positive mix of the primaries, otherwise a channel collapses and the matrix is singular.
    if (std::ranges::any_of(S, [](float scale) { return !std::isfinite(scale) || scale <= 0; }))
        return std::nullopt;

    P.multiply(diag3(S)); // RGB->XYZ

    return P;
}

Mat3x3 NColorManagement::adaptBradford(Hyprgraphics::CColor::xy srcW, Hyprgraphics::CColor::xy dstW) {
    static const Mat3x3        Bradford{std::array<float, 9>{0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f, -0.0685f, 1.0296f}};
    static const Mat3x3        BradfordInv = invertMat3(Bradford).value();

    const auto                 srcXYZ = Hyprgraphics::xy2xyz(srcW);
    const auto                 dstXYZ = Hyprgraphics::xy2xyz(dstW);

    const auto                 srcLMS = matByVec(Bradford, {srcXYZ.x, srcXYZ.y, srcXYZ.z});
    const auto                 dstLMS = matByVec(Bradford, {dstXYZ.x, dstXYZ.y, dstXYZ.z});

    const std::array<float, 3> scale{dstLMS[0] / srcLMS[0], dstLMS[1] / srcLMS[1], dstLMS[2] / srcLMS[2]};

    Mat3x3                     result = BradfordInv;
    result.multiply(diag3(scale)).multiply(Bradford);

    return result;
}

float SImageDescription::encodingMinLuminance() const {
    return transferFunction == CM_TRANSFER_FUNCTION_LINEAR || (transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR && (isWindows || isInternal)) ? 0.0f : luminances.min;
}

float SImageDescription::encodingMaxLuminance() const {
    if (transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ)
        return luminances.min + HDR_MAX_LUMINANCE;
    if (transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR && (isWindows || isInternal))
        return SDR_MAX_LUMINANCE;
    return luminances.max;
}

// sRGB constants
#define SRGB_POW   2.4
#define SRGB_CUT   0.0031308
#define SRGB_SCALE 12.92
#define SRGB_ALPHA 1.055

#define BT709_POW   (1.0 / 0.45)
#define BT709_CUT   0.018053968510807
#define BT709_SCALE 4.5
#define BT709_ALPHA (1.0 + 5.5 * BT709_CUT)

// See http://car.france3.mars.free.fr/HD/INA-%2026%20jan%2006/SMPTE%20normes%20et%20confs/s240m.pdf
#define ST240_POW   (1.0 / 0.45)
#define ST240_CUT   0.0228
#define ST240_SCALE 4.0
#define ST240_ALPHA 1.1115

#define ST428_POW   2.6
#define ST428_SCALE (52.37 / 48.0)

// PQ constants
#define PQ_M1     0.1593017578125
#define PQ_M2     78.84375
#define PQ_INV_M1 (1.0 / PQ_M1)
#define PQ_INV_M2 (1.0 / PQ_M2)
#define PQ_C1     0.8359375
#define PQ_C2     18.8515625
#define PQ_C3     18.6875

// HLG constants
#define HLG_D_CUT (1.0 / 12.0)
#define HLG_E_CUT 0.5
#define HLG_A     0.17883277
#define HLG_B     0.28466892
#define HLG_C     0.55991073

static inline int sign(double value) {
    return value < 0 ? -1 : 1;
}

// The primary source for these transfer functions is https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1361-0-199802-W!!PDF-E.pdf
static RGBAColor tfInvPQ(RGBAColor color) {
    for (uint i = 0; i <= 2; i++) {
        const double E = pow(std::clamp(color.v[i], 0.0, 1.0), PQ_INV_M2);
        color.v[i]     = pow((std::max(E - PQ_C1, 0.0)) / (PQ_C2 - PQ_C3 * E), PQ_INV_M1);
    }
    return color;
}

static constexpr std::array<double, 3> BT2020_LUMINANCE = {0.2627, 0.6780, 0.0593};

// BT.2100 HLG system gamma for a display with nominal peak luminance Lw; BT.2390 extends it outside 400-2000 cd/m².
static double hlgGamma(double peak) {
    if (peak >= 400.0 && peak <= 2000.0)
        return 1.2 + 0.42 * std::log10(peak / 1000.0);
    return 1.2 * std::pow(1.111, std::log2(peak / 1000.0));
}

// BT.2100 HLG black level lift for a display with black luminance range.min and nominal peak range.max.
static double hlgBeta(Render::STFRange range, double nominalPeak) {
    return std::sqrt(3.0 * std::pow(std::max(range.min, 0.0f) / range.max, 1.0 / hlgGamma(nominalPeak)));
}

// BT.2100 HLG EOTF: signal -> display light, normalized to range like the other transfer functions.
// The system gamma follows the nominal peak of the display the signal was graded for, which is range.max unless the range was rescaled.
static RGBAColor tfInvHLG(RGBAColor color, Render::STFRange range, const std::array<double, 3>& coefficients, double nominalPeak) {
    const double beta = hlgBeta(range, nominalPeak);
    for (uint i = 0; i <= 2; i++) {
        const double signal = std::max((1.0 - beta) * color.v[i] + beta, 0.0);
        color.v[i]          = signal <= HLG_E_CUT ? signal * signal / 3.0 : (exp((signal - HLG_C) / HLG_A) + HLG_B) / 12.0;
    }
    const double Ys = std::max(coefficients[0] * color.c.r + coefficients[1] * color.c.g + coefficients[2] * color.c.b, 0.0);
    // Scene black stays display black for any system gamma, including gamma < 1 where pow(0, gamma - 1) diverges.
    const double scale = Ys > 0 ? range.max * std::pow(Ys, hlgGamma(nominalPeak) - 1.0) : 0.0;
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = (color.v[i] * scale - range.min) / (range.max - range.min);
    }
    return color;
}

// Many transfer functions (including sRGB) follow the same pattern: a linear
// segment for small values and a power function for larger values. The
// following function implements this pattern from which sRGB, BT.709, and
// others can be derived by plugging in the right constants.
static RGBAColor tfInvLinPow(RGBAColor color, float gamma, float thres, float scale, float alpha) {
    for (uint i = 0; i <= 2; i++) {
        const bool isLow = color.v[i] <= thres * scale;
        color.v[i]       = isLow ? color.v[i] / scale : pow((color.v[i] + alpha - 1.0) / alpha, gamma);
    }
    return color;
}

static RGBAColor tfInvSRGB(RGBAColor color) {
    return tfInvLinPow(color, SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
}

static RGBAColor tfInvExtSRGB(RGBAColor color) {
    // EXT sRGB is the sRGB transfer function mirrored around 0.
    const auto absColor = tfInvSRGB({{.r = abs(color.c.r), .g = abs(color.c.g), .b = abs(color.c.b), .a = color.c.a}});
    return {{
        .r = absColor.c.r * sign(color.c.r),
        .g = absColor.c.g * sign(color.c.g),
        .b = absColor.c.b * sign(color.c.b),
        .a = absColor.c.a,
    }};
}

static RGBAColor tfInvBT1886(RGBAColor color, Render::STFRange range = {.min = 0.01f, .max = 100.0f}) {
    const double black = std::pow(std::max(range.min, 0.0f), 1.0 / 2.4);
    const double white = std::pow(range.max, 1.0 / 2.4);
    for (uint i = 0; i < 3; ++i)
        color.v[i] = (std::pow(std::max(color.v[i] * (white - black) + black, 0.0), 2.4) - range.min) / (range.max - range.min);
    return color;
}

static RGBAColor tfInvXVYCC(RGBAColor color) {
    // The inverse transfer function for XVYCC is the BT.709 transfer function mirrored around 0,
    // same as what EXT sRGB is to sRGB.
    const auto absColor = tfInvLinPow({{.r = abs(color.c.r), .g = abs(color.c.g), .b = abs(color.c.b), .a = color.c.a}}, BT709_POW, BT709_CUT, BT709_SCALE, BT709_ALPHA);
    return {{
        .r = absColor.c.r * sign(color.c.r),
        .g = absColor.c.g * sign(color.c.g),
        .b = absColor.c.b * sign(color.c.b),
        .a = absColor.c.a,
    }};
}

static RGBAColor tfInvST240(RGBAColor color) {
    return tfInvLinPow(color, ST240_POW, ST240_CUT, ST240_SCALE, ST240_ALPHA);
}

// Forward transfer functions corresponding to the inverse functions above.
static RGBAColor tfPQ(RGBAColor color) {
    for (uint i = 0; i <= 2; i++) {
        const double E = pow(std::clamp(color.v[i], 0.0, 1.0), PQ_M1);
        color.v[i]     = pow((PQ_C1 + PQ_C2 * E) / (1.0 + PQ_C3 * E), PQ_M2);
    }
    return color;
}

// Inverse BT.2100 HLG EOTF: normalized display light -> signal.
static RGBAColor tfHLG(RGBAColor color, Render::STFRange range, const std::array<double, 3>& coefficients) {
    const double gamma = hlgGamma(range.max);
    const double beta  = hlgBeta(range, range.max);
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = std::max(color.v[i] * (range.max - range.min) + range.min, 0.0) / range.max;
    }
    const double Yd    = std::max(coefficients[0] * color.c.r + coefficients[1] * color.c.g + coefficients[2] * color.c.b, 0.0);
    const double scale = Yd > 0 ? std::pow(Yd, (1.0 - gamma) / gamma) : 0.0;
    for (uint i = 0; i <= 2; i++) {
        const double scene = color.v[i] * scale;
        const double E     = scene <= HLG_D_CUT ? sqrt(scene * 3.0) : HLG_A * log(std::max(12.0 * scene - HLG_B, 0.0001)) + HLG_C;
        color.v[i]         = (E - beta) / (1.0 - beta);
    }
    return color;
}

static RGBAColor tfLinPow(RGBAColor color, float gamma, float thres, float scale, float alpha) {
    for (uint i = 0; i <= 2; i++) {
        const bool isLow = color.v[i] <= thres;
        color.v[i]       = isLow ? color.v[i] * scale : pow(color.v[i], 1.0 / gamma) * alpha - (alpha - 1.0);
    }
    return color;
}

static RGBAColor tfSRGB(RGBAColor color) {
    return tfLinPow(color, SRGB_POW, SRGB_CUT, SRGB_SCALE, SRGB_ALPHA);
}

static RGBAColor tfExtSRGB(RGBAColor color) {
    // EXT sRGB is the sRGB transfer function mirrored around 0.
    const auto absColor = tfSRGB({{.r = abs(color.c.r), .g = abs(color.c.g), .b = abs(color.c.b), .a = color.c.a}});
    return {{
        .r = absColor.c.r * sign(color.c.r),
        .g = absColor.c.g * sign(color.c.g),
        .b = absColor.c.b * sign(color.c.b),
        .a = absColor.c.a,
    }};
}

static RGBAColor tfBT1886(RGBAColor color, Render::STFRange range = {.min = 0.01f, .max = 100.0f}) {
    const double black = std::pow(std::max(range.min, 0.0f), 1.0 / 2.4);
    const double white = std::pow(range.max, 1.0 / 2.4);
    for (uint i = 0; i < 3; ++i)
        color.v[i] = (std::pow(std::max(color.v[i] * (range.max - range.min) + range.min, 0.0), 1.0 / 2.4) - black) / (white - black);
    return color;
}

static RGBAColor tfXVYCC(RGBAColor color) {
    // The transfer function for XVYCC is the BT.709 transfer function mirrored around 0,
    // same as what EXT sRGB is to sRGB.
    const auto absColor = tfLinPow({{.r = abs(color.c.r), .g = abs(color.c.g), .b = abs(color.c.b), .a = color.c.a}}, BT709_POW, BT709_CUT, BT709_SCALE, BT709_ALPHA);
    return {{
        .r = absColor.c.r * sign(color.c.r),
        .g = absColor.c.g * sign(color.c.g),
        .b = absColor.c.b * sign(color.c.b),
        .a = absColor.c.a,
    }};
}

static RGBAColor tfST240(RGBAColor color) {
    return tfLinPow(color, ST240_POW, ST240_CUT, ST240_SCALE, ST240_ALPHA);
}

static RGBAColor tfST428(RGBAColor color) {
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = pow(std::max(color.v[i], 0.0), ST428_POW) * ST428_SCALE;
    }
    return color;
}

static RGBAColor tfInvST428(RGBAColor color) {
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = pow(std::max(color.v[i], 0.0) / ST428_SCALE, 1.0 / ST428_POW);
    }
    return color;
}

static RGBAColor tfGamma(RGBAColor color, float gamma) {
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = pow(std::max(color.v[i], 0.0), gamma);
    }
    return color;
}

static RGBAColor tfLog(RGBAColor color, float mult) {
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = color.v[i] <= 0 ? 0.0 : exp((color.v[i] - 1.0) * mult * std::numbers::ln10);
    }
    return color;
}

static RGBAColor tfInvLog(RGBAColor color, float mult, float min) {
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = color.v[i] <= min ? 0.0 : 1.0 + log(color.v[i]) / std::numbers::ln10 / mult;
    }
    return color;
}

static RGBAColor toLinearRGB(RGBAColor color, eTransferFunction tf) {
    switch (tf) {
        case CM_TRANSFER_FUNCTION_LINEAR: return color;
        case CM_TRANSFER_FUNCTION_EXT_LINEAR: return color;
        case CM_TRANSFER_FUNCTION_ST2084_PQ: return tfInvPQ(color);
        case CM_TRANSFER_FUNCTION_GAMMA22: return tfGamma(color, 2.2);
        case CM_TRANSFER_FUNCTION_GAMMA28: return tfGamma(color, 2.8);
        case CM_TRANSFER_FUNCTION_HLG: return tfInvHLG(color, {.min = HDR_MIN_LUMINANCE, .max = HLG_MAX_LUMINANCE}, BT2020_LUMINANCE, HLG_MAX_LUMINANCE);
        case CM_TRANSFER_FUNCTION_EXT_SRGB: return tfInvExtSRGB(color);
        case CM_TRANSFER_FUNCTION_BT1886: return tfInvBT1886(color);
        case CM_TRANSFER_FUNCTION_ST240: return tfInvST240(color);
        case CM_TRANSFER_FUNCTION_LOG_100: return tfLog(color, 2.0);
        case CM_TRANSFER_FUNCTION_LOG_316: return tfLog(color, 2.5);
        case CM_TRANSFER_FUNCTION_XVYCC: return tfInvXVYCC(color);
        case CM_TRANSFER_FUNCTION_ST428: return tfST428(color);
        case CM_TRANSFER_FUNCTION_SRGB:
        default: return tfInvSRGB(color);
    }
}

static RGBAColor fromLinearRGB(RGBAColor color, eTransferFunction tf) {
    switch (tf) {
        case CM_TRANSFER_FUNCTION_EXT_LINEAR: return color;
        case CM_TRANSFER_FUNCTION_ST2084_PQ: return tfPQ(color);
        case CM_TRANSFER_FUNCTION_GAMMA22: return tfGamma(color, 1.0 / 2.2);
        case CM_TRANSFER_FUNCTION_GAMMA28: return tfGamma(color, 1.0 / 2.8);
        case CM_TRANSFER_FUNCTION_HLG: return tfHLG(color, {.min = HDR_MIN_LUMINANCE, .max = HLG_MAX_LUMINANCE}, BT2020_LUMINANCE);
        case CM_TRANSFER_FUNCTION_EXT_SRGB: return tfExtSRGB(color);
        case CM_TRANSFER_FUNCTION_BT1886: return tfBT1886(color);
        case CM_TRANSFER_FUNCTION_ST240: return tfST240(color);
        case CM_TRANSFER_FUNCTION_LOG_100: return tfInvLog(color, 2.0, 0.01);
        case CM_TRANSFER_FUNCTION_LOG_316: return tfInvLog(color, 2.5, sqrt(10.0) / 1000.0);
        case CM_TRANSFER_FUNCTION_XVYCC: return tfXVYCC(color);
        case CM_TRANSFER_FUNCTION_ST428: return tfInvST428(color);
        case CM_TRANSFER_FUNCTION_SRGB:
        default: return tfSRGB(color);
    }
}

static RGBAColor toNit(RGBAColor color, Render::STFRange range) {
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = color.v[i] * (range.max - range.min) + range.min;
    }
    return color;
}

static RGBAColor fromLinearNit(RGBAColor color, eTransferFunction tf, Render::STFRange range, const std::array<double, 3>& coefficients) {
    if (tf == CM_TRANSFER_FUNCTION_LINEAR)
        return color;

    for (uint i = 0; i <= 2; i++) {
        color.v[i] = (color.v[i] - range.min * color.c.a) / (range.max - range.min);
    }
    color /= std::max(color.c.a, 0.001);
    color = tf == CM_TRANSFER_FUNCTION_BT1886 ? tfBT1886(color, range) : (tf == CM_TRANSFER_FUNCTION_HLG ? tfHLG(color, range, coefficients) : fromLinearRGB(color, tf));
    color *= color.c.a;
    return color;
}

static RGBAColor saturate(RGBAColor color, std::array<std::array<double, 3>, 3> primaries, float saturation) {
    if (saturation == 1.0)
        return color;

    std::array<double, 3> colorArr   = {color.v[0], color.v[1], color.v[2]};
    std::array<double, 3> brightness = {primaries[1][0], primaries[1][1], primaries[1][2]};
    const auto            Y          = std::inner_product(colorArr.begin(), colorArr.end(), brightness.begin(), 0.0);
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = (color.v[i] * saturation) + (Y * (1.0 - saturation));
    }
    return color;
}

// CPU counterparts of tonemap.glsl and mapToGamut in cm_helpers.glsl, keep them in sync.
static double pqEncode(double luminance) {
    const double E = std::pow(std::clamp(luminance / HDR_MAX_LUMINANCE, 0.0, 1.0), PQ_M1);
    return std::pow((PQ_C1 + PQ_C2 * E) / (1.0 + PQ_C3 * E), PQ_M2);
}

static double pqDecode(double signal) {
    const double E = std::pow(std::clamp(signal, 0.0, 1.0), PQ_INV_M2);
    return std::pow(std::max(E - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * E), PQ_INV_M1) * HDR_MAX_LUMINANCE;
}

// ITU-R BT.2390 EETF on a PQ-encoded intensity, mapping source black/peak onto target black/peak.
static double bt2390EETF(double signal, double srcMin, double srcMax, double dstMin, double dstMax) {
    const double srcBlack = pqEncode(srcMin);
    const double srcRange = pqEncode(srcMax) - srcBlack;
    const double minLum   = (pqEncode(dstMin) - srcBlack) / srcRange;
    const double maxLum   = (pqEncode(dstMax) - srcBlack) / srcRange;
    const double E1       = std::clamp((signal - srcBlack) / srcRange, 0.0, 1.0);
    const double KS       = 1.5 * maxLum - 0.5;
    double       E2       = E1;
    if (E1 >= KS) {
        const double T  = (E1 - KS) / (1.0 - KS);
        const double T2 = T * T;
        const double T3 = T2 * T;
        E2              = (2.0 * T3 - 3.0 * T2 + 1.0) * KS + (T3 - 2.0 * T2 + T) * (1.0 - KS) + (-2.0 * T3 + 3.0 * T2) * maxLum;
    }
    const double E3 = E2 + minLum * std::pow(1.0 - E2, 4.0);
    return E3 * srcRange + srcBlack;
}

static RGBAColor limitLuminance(RGBAColor color, const std::array<double, 3>& luminanceCoefficients, double peak) {
    const double luminance = luminanceCoefficients[0] * color.c.r + luminanceCoefficients[1] * color.c.g + luminanceCoefficients[2] * color.c.b;
    if (luminance > peak) {
        for (uint i = 0; i <= 2; i++) {
            color.v[i] *= peak / luminance;
        }
    }
    return color;
}

static RGBAColor tonemap(RGBAColor color, const std::array<std::array<double, 3>, 3>& dstXYZ, const Render::SCMSettings& settings) {
    const auto& luminanceCoefficients = dstXYZ[1];
    if (settings.maxLuminance < settings.dstMaxLuminance * 1.01 || settings.tonemapMode == 2)
        return limitLuminance(color, luminanceCoefficients, settings.dstMaxLuminance);

    // BT.2100 Table 7 RGB -> LMS (integers / 4096) composed with D65 XYZ -> BT.2020 RGB.
    static const Hyprgraphics::CMatrix3 XYZ_TO_LMS{{{
        {0.3592832590121218, 0.6976051147779497, -0.0358915932320289},
        {-0.19208084637049927, 1.1004767970374318, 0.07537486585191187},
        {0.00707978446074772, 0.07483966621863658, 0.8433265453898765},
    }}};
    static const Hyprgraphics::CMatrix3 LMS_TO_XYZ = Hyprgraphics::CMatrix3{XYZ_TO_LMS}.invert();
    const Hyprgraphics::CMatrix3        rgbToXYZ{dstXYZ};
    const auto                          toLMS   = XYZ_TO_LMS * rgbToXYZ;
    const auto                          fromLMS = Hyprgraphics::CMatrix3{rgbToXYZ}.invert() * LMS_TO_XYZ;

    const auto                          lmsLinear = toLMS * Hyprgraphics::CColor::XYZ{.x = color.c.r, .y = color.c.g, .z = color.c.b};
    const double                        L = pqEncode(lmsLinear.x), M = pqEncode(lmsLinear.y), S = pqEncode(lmsLinear.z);
    const double                        I1 = 0.5 * L + 0.5 * M;
    double                              Ct = (6610.0 * L - 13613.0 * M + 7003.0 * S) / 4096.0;
    double                              Cp = (17933.0 * L - 17390.0 * M - 543.0 * S) / 4096.0;
    double                              I2 = I1;
    if (settings.tonemapMode == 3) {
        // Alternative curve suggested by LionHeartP, see USE_ALT_TONEMAP in tonemap.glsl.
        const double luminance = pqDecode(I1);
        const double kneeRatio = std::clamp(settings.dstMaxLuminance / settings.maxLuminance, 0.5f, 0.85f);
        const double knee      = settings.dstMaxLuminance * kneeRatio;
        double       mapped    = luminance;
        if (luminance > knee) {
            const double a = (settings.maxLuminance - settings.dstMaxLuminance) / ((settings.maxLuminance - knee) * (settings.dstMaxLuminance - knee));
            mapped         = knee + (luminance - knee) / (1.0 + a * (luminance - knee));
        }
        I2 = pqEncode(mapped);
    } else
        I2 = bt2390EETF(I1, settings.srcMinLuminance, settings.maxLuminance, settings.dstTFRange.min, settings.dstMaxLuminance);
    if (settings.tonemapMode != 3 && I1 > 0 && I2 > 0) {
        Ct *= std::min(I1 / I2, I2 / I1);
        Cp *= std::min(I1 / I2, I2 / I1);
    }

    const double L2  = I2 + 0.0086090370379327566 * Ct + 0.11102962500302595656 * Cp;
    const double M2  = I2 - 0.0086090370379327566 * Ct - 0.11102962500302595656 * Cp;
    const double S2  = I2 + 0.560031335710679118 * Ct - 0.32062717498731885185 * Cp;
    const auto   rgb = fromLMS * Hyprgraphics::CColor::XYZ{.x = pqDecode(L2), .y = pqDecode(M2), .z = pqDecode(S2)};
    color.c.r        = rgb.x;
    color.c.g        = rgb.y;
    color.c.b        = rgb.z;
    return limitLuminance(color, luminanceCoefficients, settings.dstMaxLuminance);
}

// Hue-preserving gamut clipping (BT.2407): desaturate towards the achromatic color of the same luminance.
static RGBAColor mapToGamut(RGBAColor color, const std::array<double, 3>& luminanceCoefficients, Render::STFRange range) {
    if (std::ranges::all_of(std::array{color.c.r, color.c.g, color.c.b}, [&](double v) { return v >= range.min && v <= range.max; }))
        return color;

    const double Y = std::clamp(luminanceCoefficients[0] * color.c.r + luminanceCoefficients[1] * color.c.g + luminanceCoefficients[2] * color.c.b, sc<double>(range.min),
                                sc<double>(range.max));
    double       s = 1.0;
    for (uint i = 0; i <= 2; i++) {
        const double chroma = color.v[i] - Y;
        if (chroma < 0)
            s = std::min(s, (Y - range.min) / -chroma);
        else if (chroma > 0)
            s = std::min(s, (range.max - Y) / chroma);
    }
    s = std::max(s, 0.0);
    for (uint i = 0; i <= 2; i++) {
        color.v[i] = Y + s * (color.v[i] - Y);
    }
    return color;
}

bool NColorManagement::isWindowsScRGB(const SImageDescription& description) {
    return description.isWindows && description.primariesNamed == CM_PRIMARIES_SRGB && description.transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR;
}

bool NColorManagement::isHDR(const SImageDescription& description) {
    return description.transferFunction == CM_TRANSFER_FUNCTION_ST2084_PQ || description.transferFunction == CM_TRANSFER_FUNCTION_HLG || isWindowsScRGB(description) ||
        (description.transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR && description.luminances.max > description.luminances.reference);
}

float NColorManagement::referenceLuminanceScale(const SImageDescription& source, const SImageDescription& target) {
    if (source.isWindows || source.luminances.reference == 0 || target.luminances.reference == 0)
        return 1.0f;
    return sc<float>(target.luminances.reference) / source.luminances.reference;
}

// CPU counterpart of doColorManagement in cm_helpers.glsl, keep both in sync.
RGBAColor NColorManagement::convertColor(RGBAColor color, PImageDescription srcDesc, PImageDescription dstDesc, const Render::SCMSettings& settings) {
    const auto sourceLuminance = Hyprgraphics::CMatrix3{srcDesc->getPrimaries()->toXYZ()}.mat()[1];
    const auto targetLuminance = Hyprgraphics::CMatrix3{dstDesc->getPrimaries()->toXYZ()}.mat()[1];
    const auto alpha           = color.c.a;

    color /= std::max(alpha, 0.001);
    color = settings.sourceTF == CM_TRANSFER_FUNCTION_BT1886 ?
        tfInvBT1886(color, settings.srcTFRange) :
        (settings.sourceTF == CM_TRANSFER_FUNCTION_HLG ? tfInvHLG(color, settings.srcTFRange, sourceLuminance, settings.srcTFRange.max / settings.srcReferenceScale) :
                                                         toLinearRGB(color, settings.sourceTF));
    if (dstDesc->value().icc.present) {
        // color.rgb = applyIcc3DLut(color.rgb, iccLut3D, iccLutSize);
        color *= alpha;
        return color;
    }

    // settings.convertMatrix already carries the reference-white ratio for linear sources.
    const auto& M = settings.convertMatrix;
    color         = RGBAColor{{
        .r = M[0][0] * color.c.r + M[0][1] * color.c.g + M[0][2] * color.c.b,
        .g = M[1][0] * color.c.r + M[1][1] * color.c.g + M[1][2] * color.c.b,
        .b = M[2][0] * color.c.r + M[2][1] * color.c.g + M[2][2] * color.c.b,
        .a = color.c.a,
    }};
    if (settings.sourceTF != CM_TRANSFER_FUNCTION_LINEAR)
        color = toNit(color, settings.srcTFRange);
    if (settings.needsSDRmod) {
        color = saturate(color, settings.dstPrimaries2XYZ, settings.sdrSaturation);
        color *= settings.sdrBrightnessMultiplier;
    }
    if (settings.needsTonemap)
        color = tonemap(color, settings.dstPrimaries2XYZ, settings);
    if (settings.targetTF != CM_TRANSFER_FUNCTION_LINEAR && settings.targetTF != CM_TRANSFER_FUNCTION_EXT_LINEAR)
        color = mapToGamut(color, targetLuminance, settings.dstTFRange);

    color.c.a = alpha;
    for (uint i = 0; i <= 2; i++) {
        color.v[i] *= alpha;
    }
    color = fromLinearNit(color, settings.targetTF, settings.dstTFRange, targetLuminance);
    return color;
}

double NColorManagement::decodeChannel(double signal, eTransferFunction tf, Render::STFRange range) {
    RGBAColor color{{.r = signal, .g = signal, .b = signal, .a = 1}};
    color = tf == CM_TRANSFER_FUNCTION_BT1886 ? tfInvBT1886(color, range) : toLinearRGB(color, tf);
    if (tf != CM_TRANSFER_FUNCTION_LINEAR)
        color = toNit(color, range);
    return color.c.r;
}

double NColorManagement::encodeChannel(double luminance, eTransferFunction tf, Render::STFRange range) {
    return fromLinearNit(RGBAColor{{.r = luminance, .g = luminance, .b = luminance, .a = 1}}, tf, range, BT2020_LUMINANCE).c.r;
}

CHyprColor NColorManagement::convertColor(const CHyprColor& color, PImageDescription srcDesc, PImageDescription dstDesc, const Render::SCMSettings& settings) {
    const auto& converted = convertColor(RGBAColor{{.r = color.r, .g = color.g, .b = color.b, .a = color.a}}, srcDesc, dstDesc, settings);
    return CHyprColor(converted.c.r, converted.c.g, converted.c.b, converted.c.a);
}

PImageDescription NColorManagement::getDefaultImageDescription() {
    const auto TF = fromConfig();
    switch (TF) {
        case TF_AUTO:
        case TF_GAMMA22:
        case TF_FORCED_GAMMA22: return DEFAULT_GAMMA22_IMAGE_DESCRIPTION;
        case TF_DEFAULT:
        case TF_SRGB: return DEFAULT_SRGB_IMAGE_DESCRIPTION;
        default: UNREACHABLE();
    }
}
