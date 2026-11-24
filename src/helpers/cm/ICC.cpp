#include "ICC.hpp"
#include <bit>
#include <cmath>
#include "../math/Math.hpp"
#include <cstddef>
#include <fstream>

#include "../../debug/log/Logger.hpp"
#include "../../render/Texture.hpp"
#include "../../render/Renderer.hpp"

#include <lcms2.h>

using namespace NColorManagement;

static std::vector<uint8_t> readBinary(const std::filesystem::path& file) {
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs.good())
        return {};

    ifs.seekg(0, std::ios::end);
    size_t len = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    if (len <= 0)
        return {};

    std::vector<uint8_t> buf;
    buf.resize(len);
    ifs.read(reinterpret_cast<char*>(buf.data()), len);

    return buf;
}

static uint16_t bigEndianU16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static uint32_t bigEndianU32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

std::expected<std::optional<SVCGTTable16>, std::string> NColorManagement::iccReadVCGT(cmsHPROFILE profile) {
    if (!cmsIsTag(profile, cmsSigVcgtTag))
        return std::nullopt;

    const auto size = cmsReadRawTag(profile, cmsSigVcgtTag, nullptr, 0);
    // Three channels of at most 65535 two-byte entries, plus the header and alignment.
    if (size < 12 || size > 20 + 3 * 65535 * 2 + 3)
        return std::unexpected("Invalid vcgt tag size");

    std::vector<uint8_t> raw(size);
    if (cmsReadRawTag(profile, cmsSigVcgtTag, raw.data(), size) != size || bigEndianU32(raw.data()) != cmsSigVcgtType)
        return std::unexpected("Invalid vcgt tag header");

    SVCGTTable16 table{.channels = 3, .entrySize = 2};
    const auto   type = bigEndianU32(raw.data() + 8);
    if (type == 1) {
        if (size < 48)
            return std::unexpected("Truncated vcgt formula");

        // Per-channel signed 16.16 triples. Sample in the same normalized input domain as the KMS ramp.
        table.entries = 4096;
        for (size_t c = 0; c < table.ch.size(); ++c) {
            const auto   fixed = [&](size_t field) { return std::bit_cast<int32_t>(bigEndianU32(raw.data() + 12 + c * 12 + field * 4)) / 65536.0; };
            const double gamma = fixed(0), low = fixed(1), high = fixed(2);
            if (gamma <= 0 || low < 0 || high > 1 || low >= high)
                return std::unexpected("Invalid vcgt formula range or exponent");
            table.ch[c].resize(table.entries);
            for (size_t i = 0; i < table.entries; ++i) {
                const auto value = std::lerp(low, high, std::pow(sc<double>(i) / (table.entries - 1), gamma));
                table.ch[c][i]   = sc<uint16_t>(std::lround(value * 65535));
            }
        }
        return table;
    }

    if (type != 0 || size < 18)
        return std::unexpected("Invalid vcgt table header");

    const auto channels = bigEndianU16(raw.data() + 12);
    table.entries       = bigEndianU16(raw.data() + 14);
    const auto width    = bigEndianU16(raw.data() + 16);
    if ((channels != 1 && channels != 3) || (width != 1 && width != 2) || table.entries < 2)
        return std::unexpected("Invalid vcgt table dimensions");

    const size_t bytes = sc<size_t>(channels) * table.entries * width;
    if (size < 18 + bytes)
        return std::unexpected("Truncated vcgt table");

    // The declared size may include zero padding to a four-byte boundary. External ICC alignment is not part of the tag.
    const auto matchesLength = [&](size_t offset) {
        const size_t end = offset + bytes;
        return size == end || (size == ((end + 3) & ~size_t{3}) && std::ranges::all_of(std::span(raw).subspan(end), [](uint8_t byte) { return byte == 0; }));
    };
    const bool standard = matchesLength(18);
    const bool legacy   = matchesLength(20) && bigEndianU16(raw.data() + 18) == 0;
    if (standard == legacy)
        return std::unexpected("Invalid or ambiguous vcgt table layout");

    const size_t offset = legacy ? 20 : 18;
    for (size_t c = 0; c < table.ch.size(); ++c) {
        table.ch[c].resize(table.entries);
        const auto channel = offset + (channels == 1 ? 0 : c) * table.entries * width;
        for (size_t i = 0; i < table.entries; ++i) {
            const auto pos = channel + i * width;
            table.ch[c][i] = width == 1 ? raw[pos] * 257 : bigEndianU16(raw.data() + pos);
        }
    }
    return table;
}

struct CmsProfileDeleter {
    void operator()(cmsHPROFILE p) const {
        if (p)
            cmsCloseProfile(p);
    }
};
struct CmsTransformDeleter {
    void operator()(cmsHTRANSFORM t) const {
        if (t)
            cmsDeleteTransform(t);
    }
};

using UniqueProfile   = std::unique_ptr<std::remove_pointer_t<cmsHPROFILE>, CmsProfileDeleter>;
using UniqueTransform = std::unique_ptr<std::remove_pointer_t<cmsHTRANSFORM>, CmsTransformDeleter>;

// Sample the display's linear RGB cube with a perceptual shaper to retain shadow precision.
static UniqueProfile createLUTInputProfile(const SPCPRimaries& domain) {
    const cmsCIExyYTRIPLE prim{{domain.red.x, domain.red.y, 1}, {domain.green.x, domain.green.y, 1}, {domain.blue.x, domain.blue.y, 1}};
    const cmsCIExyY       wp{domain.white.x, domain.white.y, 1};

    // IEC 61966-2-1 piecewise sRGB, the same curve as tfInvSRGB in cm_helpers.glsl
    const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    cmsToneCurve*          srgb      = cmsBuildParametricToneCurve(nullptr, 4, params);
    if (!srgb)
        return {};
    cmsToneCurve* curves[3] = {srgb, srgb, srgb};

    cmsHPROFILE   p = cmsCreateRGBProfile(&wp, &prim, curves);

    cmsFreeToneCurve(srgb);
    return UniqueProfile{p};
}

std::expected<void, std::string> NColorManagement::iccBuildLUTData(cmsHPROFILE profile, SImageDescription& image) {
    UniqueProfile src = createLUTInputProfile(image.getPrimaries());
    if (!src)
        return std::unexpected("Failed to create ICC LUT input profile");

    // Rendering intent: RELATIVE_COLORIMETRIC is common for displays; add BPC to be safe.
    const int             intent = INTENT_RELATIVE_COLORIMETRIC;
    const cmsUInt32Number flags  = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC; // good quality precalc in LCMS

    // float->float transform (sRGB-encoded input, encoded output in dst device space)
    UniqueTransform xform{cmsCreateTransform(src.get(), TYPE_RGB_FLT, profile, TYPE_RGB_FLT, intent, flags)};
    if (!xform)
        return std::unexpected("Failed to create ICC transform");

    LOG(Log::DEBUG, "Building a {}³ 3D LUT", image.icc.lutSize);

    image.icc.present = true;
    image.icc.lutDataPacked.resize(image.icc.lutSize * image.icc.lutSize * image.icc.lutSize * 3);

    auto idx = [&image](int r, int g, int b) -> size_t {
        //
        return ((size_t)b * image.icc.lutSize * image.icc.lutSize + (size_t)g * image.icc.lutSize + (size_t)r) * 3;
    };

    for (size_t bz = 0; bz < image.icc.lutSize; ++bz) {
        for (size_t gy = 0; gy < image.icc.lutSize; ++gy) {
            for (size_t rx = 0; rx < image.icc.lutSize; ++rx) {
                float in[3] = {
                    rx / float(image.icc.lutSize - 1),
                    gy / float(image.icc.lutSize - 1),
                    bz / float(image.icc.lutSize - 1),
                };
                float outRGB[3];
                cmsDoTransform(xform.get(), in, outRGB, 1);

                outRGB[0] = std::clamp(outRGB[0], 0.F, 1.F);
                outRGB[1] = std::clamp(outRGB[1], 0.F, 1.F);
                outRGB[2] = std::clamp(outRGB[2], 0.F, 1.F);

                const size_t o                 = idx(rx, gy, bz);
                image.icc.lutDataPacked[o + 0] = outRGB[0];
                image.icc.lutDataPacked[o + 1] = outRGB[1];
                image.icc.lutDataPacked[o + 2] = outRGB[2];
            }
        }
    }

    LOG(Log::DEBUG, "3D LUT constructed, size {}", image.icc.lutDataPacked.size());

    return {};
}

static std::expected<void, std::string> buildIcc3DLut(cmsHPROFILE profile, SImageDescription& image) {
    if (const auto RET = iccBuildLUTData(profile, image); !RET)
        return RET;

    image.icc.lutTexture = g_pHyprRenderer->createTexture(image.icc.lutDataPacked, image.icc.lutSize);

    return {};
}

static cmsCIExyY buildPrimary(UniqueTransform& xform, std::array<float, 3> rgb) {
    float xyz_data[3] = {};
    cmsDoTransform(xform.get(), rgb.data(), xyz_data, 1);

    const cmsCIEXYZ xyz{xyz_data[0], xyz_data[1], xyz_data[2]};
    cmsCIExyY       xyY{};

    cmsXYZ2xyY(&xyY, &xyz);
    return xyY;
}

static std::optional<SPCPRimaries> nativePrimaries(cmsHPROFILE profile) {
    std::array<cmsCIEXYZ, 4> xyz{};
    const std::array         tags = {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag, cmsSigMediaWhitePointTag};
    for (size_t i = 0; i < tags.size(); ++i) {
        const auto value = sc<const cmsCIEXYZ*>(cmsReadTag(profile, tags[i]));
        if (!value)
            return std::nullopt;
        xyz[i] = *value;
    }

    if (cmsIsTag(profile, cmsSigChromaticAdaptationTag)) {
        // chad is an s15Fixed16ArrayType containing the row-major native-to-PCS matrix.
        std::array<uint8_t, 44> raw{};
        if (cmsReadRawTag(profile, cmsSigChromaticAdaptationTag, raw.data(), raw.size()) != raw.size() || bigEndianU32(raw.data()) != cmsSigS15Fixed16ArrayType)
            return std::nullopt;
        std::array<std::array<double, 3>, 3> matrix{};
        for (size_t row = 0; row < 3; ++row)
            for (size_t col = 0; col < 3; ++col)
                matrix[row][col] = std::bit_cast<int32_t>(bigEndianU32(raw.data() + 8 + (row * 3 + col) * 4)) / 65536.0;
        const auto&  m = matrix;
        const double det =
            m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (!std::isfinite(det) || std::abs(det) < 1e-10)
            return std::nullopt;
        const auto inverse = Hyprgraphics::CMatrix3{matrix}.invert();
        // v4 (and most v2) display profiles store a PCS-relative D50 wtpt; some v2 profiles (e.g. ArgyllCMS) store the native white and only adapt the colorants.
        cmsCIExyY whiteXY{};
        cmsCIExyY d50XY{};
        cmsXYZ2xyY(&whiteXY, &xyz[3]);
        cmsXYZ2xyY(&d50XY, cmsD50_XYZ());
        const bool whiteIsPCS = std::abs(whiteXY.x - d50XY.x) < 1e-3 && std::abs(whiteXY.y - d50XY.y) < 1e-3;
        for (size_t i = 0; i < xyz.size(); ++i) {
            if (i == 3 && !whiteIsPCS)
                continue;
            const auto& value  = xyz[i];
            const auto  native = inverse * Hyprgraphics::CColor::XYZ{.x = value.X, .y = value.Y, .z = value.Z};
            xyz[i]             = {native.x, native.y, native.z};
        }
    } else {
        // Older profiles may store native media white without a chad tag, but colorants are PCS-relative.
        for (size_t i = 0; i < 3; ++i) {
            cmsCIEXYZ native{};
            if (!cmsAdaptToIlluminant(&native, cmsD50_XYZ(), &xyz[3], &xyz[i]))
                return std::nullopt;
            xyz[i] = native;
        }
    }

    std::array<Hyprgraphics::CColor::xy, 4> xy{};
    for (size_t i = 0; i < xyz.size(); ++i) {
        const auto& value = xyz[i];
        if (!std::isfinite(value.X) || !std::isfinite(value.Y) || !std::isfinite(value.Z) || value.X + value.Y + value.Z <= 0)
            return std::nullopt;
        cmsCIExyY result{};
        cmsXYZ2xyY(&result, &value);
        xy[i] = {.x = result.x, .y = result.y};
    }
    return SPCPRimaries{.red = xy[0], .green = xy[1], .blue = xy[2], .white = xy[3]};
}

std::expected<SPCPRimaries, std::string> NColorManagement::iccPrimaries(cmsHPROFILE profile) {
    if (const auto native = nativePrimaries(profile))
        return *native;

    const int             intent = INTENT_RELATIVE_COLORIMETRIC;
    const cmsUInt32Number flags  = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC; // good quality precalc in LCMS

    // float->float transform
    UniqueProfile xyzProfile{cmsCreateXYZProfile()};
    if (!xyzProfile)
        return std::unexpected("Failed to create XYZ profile");
    UniqueTransform xform{cmsCreateTransform(profile, TYPE_RGB_FLT, xyzProfile.get(), TYPE_XYZ_FLT, intent, flags)};
    if (!xform)
        return std::unexpected("Failed to create ICC transform");

    LOG(Log::DEBUG, "Building RGB Primaries");

    const cmsCIExyY redXY   = buildPrimary(xform, {1.0f, 0.0f, 0.0f});
    const cmsCIExyY greenXY = buildPrimary(xform, {0.0f, 1.0f, 0.0f});
    const cmsCIExyY blueXY  = buildPrimary(xform, {0.0f, 0.0f, 1.0f});
    const cmsCIExyY whiteXY = buildPrimary(xform, {1.0f, 1.0f, 1.0f});

    return SPCPRimaries{
        .red   = {.x = redXY.x, .y = redXY.y},
        .green = {.x = greenXY.x, .y = greenXY.y},
        .blue  = {.x = blueXY.x, .y = blueXY.y},
        .white = {.x = whiteXY.x, .y = whiteXY.y},
    };
}

std::expected<SImageDescription, std::string> SImageDescription::fromICC(const std::filesystem::path& file, SP<const CImageDescription> previous) {
    static auto     PVCGTENABLED = CConfigValue<Config::BOOL>("render:icc_vcgt_enabled");

    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
        return std::unexpected("Invalid file");

    SImageDescription image;
    image.rawICC = readBinary(file);

    if (image.rawICC.empty())
        return std::unexpected("Failed to read file");

    UniqueProfile profile{cmsOpenProfileFromMem(image.rawICC.data(), image.rawICC.size())};
    const auto    prof = profile.get();
    if (!prof)
        return std::unexpected("CMS failed to open icc file");

    // only handle RGB (typical display profiles)
    if (cmsGetColorSpace(prof) != cmsSigRgbData)
        return std::unexpected("Only RGB display profiles are supported");

    LOG(Log::DEBUG, "============= Begin ICC load =============");
    LOG(Log::DEBUG, "ICC size: {} bytes", image.rawICC.size());

    const auto primaries = iccPrimaries(prof);
    if (!primaries)
        return std::unexpected(primaries.error());
    image.primaries   = *primaries;
    image.icc.present = true;

    if (*PVCGTENABLED) {
        auto vcgtRes = iccReadVCGT(prof);
        if (!vcgtRes)
            return std::unexpected(vcgtRes.error());

        image.icc.vcgt = *vcgtRes;

        if (!*vcgtRes)
            LOG(Log::DEBUG, "ICC profile has no VCGT data");
    } else
        LOG(Log::DEBUG, "Skipping VCGT load, disabled by config");

    // Compare content and effective settings before constructing or uploading the expensive LUT.
    if (previous && image == previous->value())
        return previous->value();

    if (const auto RET = buildIcc3DLut(prof, image); !RET)
        return std::unexpected(RET.error());

    LOG(Log::DEBUG, "============= End ICC load =============");

    return image;
}