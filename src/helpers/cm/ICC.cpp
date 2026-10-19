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

static constexpr cmsTagSignature makeSig(char a, char b, char c, char d) {
    return sc<cmsTagSignature>(sc<uint32_t>(a) << 24 | sc<uint32_t>(b) << 16 | sc<uint32_t>(c) << 8 | sc<uint32_t>(d));
}

static constexpr cmsTagSignature VCGT_SIG = makeSig('v', 'c', 'g', 't');

//

static std::expected<std::optional<SVCGTTable16>, std::string> readVCGT16(cmsHPROFILE prof) {
    if (!cmsIsTag(prof, VCGT_SIG))
        return std::nullopt;

    cmsUInt32Number n = cmsReadRawTag(prof, VCGT_SIG, nullptr, 0);
    if (n < 8 + 4 + 2 + 2 + 2 + 2) // header + type + table header
        return std::unexpected("Malformed vcgt tag");

    std::vector<uint8_t> raw(n);
    if (cmsReadRawTag(prof, VCGT_SIG, raw.data(), n) != n)
        return std::unexpected("Malformed vcgt tag");

    // raw layout:
    // 0 ... 3: 'vcgt'
    // 4 ... 7: reserved
    // 8 ... 11: gammaType (0 = table)
    uint32_t gammaType = bigEndianU32(raw.data() + 8);
    if (gammaType != 0)
        return std::unexpected("VCGT formula type is not supported by Hyprland");

    SVCGTTable16 table;
    table.channels  = bigEndianU16(raw.data() + 12);
    table.entries   = bigEndianU16(raw.data() + 14);
    table.entrySize = bigEndianU16(raw.data() + 16);
    // raw+18: reserved u16

    LOG(Log::DEBUG, "readVCGT16: table has {} channels, {} entries, and entry size of {}", table.channels, table.entries, table.entrySize);

    if (table.channels != 3 || table.entrySize != 2 || table.entries == 0)
        return std::unexpected("invalid vcgt table size");

    size_t tableBytes = (size_t)table.channels * (size_t)table.entries * (size_t)table.entrySize;

    // VCGT is a piece of shit and some absolute fucking mongoloid idiots
    // decided it'd be great to have both 18 and 20
    // FUCK YOU
    size_t tableOff = 20;

    auto   readTable = [&] -> void {
        for (int c = 0; c < 3; ++c) {
            table.ch[c].resize(table.entries);
            for (uint16_t i = 0; i < table.entries; ++i) {
                const uint8_t* p = raw.data() + tableOff + sc<ptrdiff_t>((c * table.entries + i) * 2);
                table.ch[c][i]   = bigEndianU16(p); // 0 ... 65535
            }
        }
    };

    if (raw.size() < tableOff + tableBytes) {
        tableOff = 18;

        if (raw.size() < tableOff + tableBytes) {
            LOG(Log::ERR, "readVCGT16: table is too short, tag is invalid");
            return std::unexpected("table is too short");
        }

        LOG(Log::DEBUG, "readVCGT16: table is too short, but off = 18 fits. Attempting offset = 18");

        readTable();
    } else {
        readTable();

        // if the table's last entry is suspiciously low, we more than likely read an 18 as a 20.
        if (table.ch[0][table.entries - 1] < 30000) {
            LOG(Log::DEBUG, "readVCGT16: table is likely offset 18 not 20, re-reading");

            tableOff = 18;

            readTable();
        }
    }

    if (table.ch[0][table.entries - 1] < 30000) {
        LOG(Log::ERR, "readVCGT16: table is malformed, last value of a gamma ramp can't be {}", table.ch[0][table.entries - 1]);
        return std::unexpected("invalid table values");
    }

    LOG(Log::DEBUG, "readVCGT16: red channel: [{}, {}, ... {}, {}]", table.ch[0][0], table.ch[0][1], table.ch[0][table.entries - 2], table.ch[0][table.entries - 1]);
    LOG(Log::DEBUG, "readVCGT16: green channel: [{}, {}, ... {}, {}]", table.ch[1][0], table.ch[1][1], table.ch[1][table.entries - 2], table.ch[1][table.entries - 1]);
    LOG(Log::DEBUG, "readVCGT16: blue channel: [{}, {}, ... {}, {}]", table.ch[2][0], table.ch[2][1], table.ch[2][table.entries - 2], table.ch[2][table.entries - 1]);

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

// The LUT is indexed by sRGB-encoded values: a uniform grid in linear light leaves the shadows with a few
// samples only, and an sRGB output profile then maps to the identity grid.
static UniqueProfile createSRGBProfile() {
    cmsCIExyYTRIPLE prim{};
    // sRGB / Rec.709 primaries
    prim.Red.x   = 0.6400;
    prim.Red.y   = 0.3300;
    prim.Red.Y   = 1.0;
    prim.Green.x = 0.3000;
    prim.Green.y = 0.6000;
    prim.Green.Y = 1.0;
    prim.Blue.x  = 0.1500;
    prim.Blue.y  = 0.0600;
    prim.Blue.Y  = 1.0;

    cmsCIExyY wp{};
    wp.x = 0.3127;
    wp.y = 0.3290;
    wp.Y = 1.0; // D65

    // IEC 61966-2-1 piecewise sRGB, the same curve as tfInvSRGB in cm_helpers.glsl
    const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    cmsToneCurve*          srgb      = cmsBuildParametricToneCurve(nullptr, 4, params);
    cmsToneCurve*          curves[3] = {srgb, srgb, srgb};

    cmsHPROFILE            p = cmsCreateRGBProfile(&wp, &prim, curves);

    cmsFreeToneCurve(srgb);
    return UniqueProfile{p};
}

std::expected<void, std::string> NColorManagement::iccBuildLUTData(cmsHPROFILE profile, SImageDescription& image) {
    UniqueProfile src = createSRGBProfile();
    if (!src)
        return std::unexpected("Failed to create sRGB profile");

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
    static auto     PVCGTENABLED = CConfigValue<Config::INTEGER>("render:icc_vcgt_enabled");

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
        auto vcgtRes = readVCGT16(prof);
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