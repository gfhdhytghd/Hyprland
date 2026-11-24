#include <render/ShaderLoader.hpp>
#include <render/types.hpp>
#include <render/transformer/Transformer.hpp>
#include <hyprgraphics/egl/Egl.hpp>
#include <helpers/cm/ColorManagement.hpp>
#include "ShaderCompiler.hpp"
#include <gtest/gtest.h>

using namespace Render;
using namespace NColorManagement;

static std::map<std::string, std::string> surfaceDefines() {
    std::map<std::string, std::string> defines;
    for (const auto name : {"RGBA", "DISCARD", "TINT", "ROUNDING", "CM", "TONEMAP", "SDR_MOD", "BLUR", "ICC", "MIRROR", "MOTION_BLUR", "BLUR_ALPHA_MASK", "BLUR_MATTE",
                            "ALT_TONEMAP", "BLUR_PROPORTIONAL", "EXTERNAL"})
        defines[std::string("USE_") + name] = "0";
    return defines;
}

TEST(ColorManagementShader, SurfaceAndBlurVariantsCompile) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3))
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 is unavailable";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    for (const auto fragment : {SH_FRAG_SURFACE, SH_FRAG_BLURPREPARE, SH_FRAG_BLURFINISH, SH_FRAG_PRISMFINISH, SH_FRAG_BORDER1, SH_FRAG_INNER_GLOW}) {
        for (const auto tf : {CM_TRANSFER_FUNCTION_BT1886, CM_TRANSFER_FUNCTION_EXT_LINEAR, CM_TRANSFER_FUNCTION_ST2084_PQ, CM_TRANSFER_FUNCTION_HLG}) {
            for (const auto extra :
                 {0, sc<int>(SH_FEAT_SDR_MOD), sc<int>(SH_FEAT_MIRROR), sc<int>(SH_FEAT_TONEMAP | SH_FEAT_MIRROR), sc<int>(SH_FEAT_TONEMAP | SH_FEAT_SDR_MOD | SH_FEAT_MIRROR),
                  sc<int>(SH_FEAT_ICC | SH_FEAT_MIRROR), sc<int>(SH_FEAT_TONEMAP | SH_FEAT_SDR_MOD | SH_FEAT_ALT_TONEMAP)}) {
                if (fragment == SH_FRAG_BLURPREPARE && extra != 0)
                    continue;
                if (fragment != SH_FRAG_SURFACE && fragment != SH_FRAG_BORDER1 && fragment != SH_FRAG_INNER_GLOW && (extra & (SH_FEAT_TONEMAP | SH_FEAT_ICC | SH_FEAT_MIRROR)))
                    continue;
                auto defines               = surfaceDefines();
                defines["USE_CM"]          = "1";
                defines["USE_TONEMAP"]     = extra & SH_FEAT_TONEMAP ? "1" : "0";
                defines["USE_SDR_MOD"]     = extra & SH_FEAT_SDR_MOD ? "1" : "0";
                defines["USE_MIRROR"]      = extra & SH_FEAT_MIRROR ? "1" : "0";
                defines["USE_ICC"]         = extra & SH_FEAT_ICC ? "1" : "0";
                defines["USE_ALT_TONEMAP"] = extra & SH_FEAT_ALT_TONEMAP ? "1" : "0";
                defines["SOURCE_TF"]       = std::to_string(sc<int>(tf));
                defines["TARGET_TF"]       = std::to_string(sc<int>(tf));
                SCOPED_TRACE(FRAG_SHADERS[fragment] + " TF=" + std::to_string(sc<int>(tf)) + " features=" + std::to_string(extra));
                EXPECT_TRUE(compiler.compile(loader.process(FRAG_SHADERS[fragment], defines), GL_FRAGMENT_SHADER));
            }
        }
    }
}

TEST(ColorManagementShader, SurfaceBlurCompositingVariantsCompile) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3))
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 is unavailable";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    // Bits: CM, ICC, MIRROR, BLUR_MATTE, BLUR_ALPHA_MASK, DISCARD, BLUR_PROPORTIONAL.
    for (int variant = 0; variant < (1 << 7); ++variant) {
        const bool cm = variant & 1;
        if (!cm && (variant & 2))
            continue;
        auto defines                     = surfaceDefines();
        defines["USE_BLUR"]              = "1";
        defines["USE_ROUNDING"]          = "1";
        defines["USE_CM"]                = cm ? "1" : "0";
        defines["USE_ICC"]               = variant & 2 ? "1" : "0";
        defines["USE_MIRROR"]            = variant & 4 ? "1" : "0";
        defines["USE_BLUR_MATTE"]        = variant & 8 ? "1" : "0";
        defines["USE_BLUR_ALPHA_MASK"]   = variant & 16 ? "1" : "0";
        defines["USE_DISCARD"]           = variant & 32 ? "1" : "0";
        defines["USE_BLUR_PROPORTIONAL"] = variant & 64 ? "1" : "0";
        defines["SOURCE_TF"]             = std::to_string(sc<int>(CM_TRANSFER_FUNCTION_ST2084_PQ));
        defines["TARGET_TF"]             = std::to_string(sc<int>(CM_TRANSFER_FUNCTION_ST2084_PQ));
        SCOPED_TRACE(variant);
        EXPECT_TRUE(compiler.compile(loader.process(FRAG_SHADERS[SH_FRAG_SURFACE], defines), GL_FRAGMENT_SHADER));
    }
}

TEST(ColorManagementShader, ExternalSurfaceCompilesWithMotionBlurAndColorManagement) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3))
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 is unavailable";
    if (!compiler.supportsExtension("GL_OES_EGL_image_external_essl3"))
        GTEST_SKIP() << "GL_OES_EGL_image_external_essl3 is unavailable";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    auto          defines      = surfaceDefines();
    defines["USE_EXTERNAL"]    = "1";
    defines["USE_MOTION_BLUR"] = "1";
    defines["USE_RGBA"]        = "1";
    defines["USE_CM"]          = "1";
    defines["SOURCE_TF"]       = std::to_string(sc<int>(CM_TRANSFER_FUNCTION_ST2084_PQ));
    defines["TARGET_TF"]       = std::to_string(sc<int>(CM_TRANSFER_FUNCTION_BT1886));
    EXPECT_TRUE(compiler.compile(loader.process(FRAG_SHADERS[SH_FRAG_SURFACE], defines), GL_FRAGMENT_SHADER));
}

static void setColorManagementUniforms(GLuint program, Render::STFRange source, Render::STFRange target) {
    static constexpr std::array<GLfloat, 9> IDENTITY = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    glUniformMatrix3fv(glGetUniformLocation(program, "convertMatrix"), 1, GL_FALSE, IDENTITY.data());
    glUniform2f(glGetUniformLocation(program, "srcTFRange"), source.min, source.max);
    glUniform2f(glGetUniformLocation(program, "dstTFRange"), target.min, target.max);
    glUniform3f(glGetUniformLocation(program, "sourceLuminanceCoefficients"), 0.2126F, 0.7152F, 0.0722F);
    glUniform3f(glGetUniformLocation(program, "targetLuminanceCoefficients"), 0.2126F, 0.7152F, 0.0722F);
    glUniform1f(glGetUniformLocation(program, "sourceReferenceScale"), 1.F);
    glUniform1f(glGetUniformLocation(program, "sdrSaturation"), 1.F);
    glUniform1f(glGetUniformLocation(program, "sdrBrightnessMultiplier"), 1.F);
}

static std::map<std::string, std::string> cmDefines(eTransferFunction source, eTransferFunction target) {
    auto defines         = surfaceDefines();
    defines["USE_CM"]    = "1";
    defines["SOURCE_TF"] = std::to_string(sc<int>(source));
    defines["TARGET_TF"] = std::to_string(sc<int>(target));
    return defines;
}

static void expectRGBA(const RGBA& actual, const RGBA& expected, float tolerance) {
    for (size_t i = 0; i < 4; ++i)
        EXPECT_NEAR(actual[i], expected[i], tolerance) << "channel " << i;
}

// Regression: the material finishes read sourceTF / targetTF as uniforms that were never set, so they skipped all conversion.
TEST(ColorManagementShader, MaterialFinishesUseTheVariantTransferFunctions) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3) || !compiler.canDraw())
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 and float render targets is unavailable";

    CShaderLoader          loader(SHADER_INCLUDES, FRAG_SHADERS);
    const Render::STFRange SOURCE  = {.min = 0.F, .max = 203.F};
    const Render::STFRange TARGET  = {.min = 0.005F, .max = 10000.005F};
    auto                   defines = cmDefines(CM_TRANSFER_FUNCTION_SRGB, CM_TRANSFER_FUNCTION_ST2084_PQ);
    defines["USE_SDR_MOD"]         = "1";
    const float expected           = encodeChannel(decodeChannel(0.4, CM_TRANSFER_FUNCTION_SRGB, SOURCE), CM_TRANSFER_FUNCTION_ST2084_PQ, TARGET);

    for (const auto fragment : {SH_FRAG_BLURFINISH, SH_FRAG_FROSTFINISH, SH_FRAG_RIPPLEFINISH, SH_FRAG_DROPSFINISH, SH_FRAG_WATERFINISH, SH_FRAG_FLUIDJARFINISH,
                                SH_FRAG_PRISMFINISH, SH_FRAG_HEATSHIMMERFINISH, SH_FRAG_ACRYLICFINISH, SH_FRAG_AURORAFINISH, SH_FRAG_HAZEFINISH}) {
        SCOPED_TRACE(FRAG_SHADERS[fragment]);
        RGBA result = {};
        ASSERT_TRUE(compiler.draw(
            {
                .fragment = loader.process(FRAG_SHADERS[fragment], defines),
                .uniforms =
                    [&](GLuint program) {
                        EXPECT_EQ(glGetUniformLocation(program, "sourceTF"), -1);
                        EXPECT_EQ(glGetUniformLocation(program, "targetTF"), -1);
                        setColorManagementUniforms(program, SOURCE, TARGET);
                        glUniform1i(glGetUniformLocation(program, "tex"), 0);
                        glUniform1f(glGetUniformLocation(program, "brightness"), 1.F);
                        glUniform1f(glGetUniformLocation(program, "glassSize"), 40.F);
                    },
                .textures = {{0.4F, 0.4F, 0.4F, 1.F}},
            },
            result));
        // Frost with no refraction or roughness only applies the finish conversion.
        if (fragment == SH_FRAG_BLURFINISH || fragment == SH_FRAG_FROSTFINISH)
            expectRGBA(result, {expected, expected, expected, 1.F}, 1e-4F);
    }
}

static RGBA drawHalfCoveredWhiteShadow(CShaderCompiler& compiler, const std::map<std::string, std::string>& defines, Render::STFRange source, Render::STFRange target) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    RGBA          result = {};
    EXPECT_TRUE(compiler.draw(
        {
            .fragment = loader.process(FRAG_SHADERS[SH_FRAG_SHADOW], defines),
            .uniforms =
                [&](GLuint program) {
                    setColorManagementUniforms(program, source, target);
                    // One white OkLab stop, like a single configured colour. The pixel is at the centre of a 100 px box with a 100 px
                    // linear falloff, so its coverage is 0.5.
                    glUniform4f(glGetUniformLocation(program, "gradient"), 1.F, 0.F, 0.F, 1.F);
                    glUniform1i(glGetUniformLocation(program, "gradientLength"), 1);
                    glUniform1i(glGetUniformLocation(program, "gradient2Length"), 0);
                    glUniform1f(glGetUniformLocation(program, "alpha"), 1.F);
                    glUniform2f(glGetUniformLocation(program, "topLeft"), 0.F, 0.F);
                    glUniform2f(glGetUniformLocation(program, "bottomRight"), 100.F, 100.F);
                    glUniform2f(glGetUniformLocation(program, "fullSize"), 100.F, 100.F);
                    glUniform2f(glGetUniformLocation(program, "windowTopLeft"), -1.F, -1.F);
                    glUniform2f(glGetUniformLocation(program, "windowBottomRight"), -1.F, -1.F);
                    glUniform1f(glGetUniformLocation(program, "range"), 100.F);
                    glUniform1f(glGetUniformLocation(program, "shadowPower"), 1.F);
                    glUniform1f(glGetUniformLocation(program, "roundingPower"), 2.F);
                },
            .color = {1.F, 1.F, 1.F, 1.F},
        },
        result));
    return result;
}

// Regression: the shadow replaced its converted colour with the sRGB gradient sample and premultiplied in optical space.
TEST(ColorManagementShader, ShadowFalloffIsConvertedAndPremultipliedInTheTargetEncoding) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3) || !compiler.canDraw())
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 and float render targets is unavailable";

    // Matching sRGB work buffer: the colour stays sRGB and is premultiplied there, like rectangles and borders.
    auto plain      = cmDefines(CM_TRANSFER_FUNCTION_SRGB, CM_TRANSFER_FUNCTION_SRGB);
    plain["USE_CM"] = "0";
    expectRGBA(drawHalfCoveredWhiteShadow(compiler, plain, {.min = 0.F, .max = 80.F}, {.min = 0.F, .max = 80.F}), {0.5F, 0.5F, 0.5F, 0.5F}, 1e-4F);

    // FP16 linear work buffer, 1.0 = 80 cd/m² and SDR white anchored at 203 cd/m².
    const float LINEAR = 0.5F * 203.F / 80.F;
    expectRGBA(drawHalfCoveredWhiteShadow(compiler, cmDefines(CM_TRANSFER_FUNCTION_SRGB, CM_TRANSFER_FUNCTION_EXT_LINEAR), {.min = 0.F, .max = 203.F}, {.min = 0.F, .max = 80.F}),
               {LINEAR, LINEAR, LINEAR, 0.5F}, 1e-4F);

    // PQ work buffer: premultiplied in PQ code values.
    const Render::STFRange PQ_RANGE = {.min = 0.005F, .max = 10000.005F};
    const float            PQ       = 0.5F * encodeChannel(203.0, CM_TRANSFER_FUNCTION_ST2084_PQ, PQ_RANGE);
    expectRGBA(drawHalfCoveredWhiteShadow(compiler, cmDefines(CM_TRANSFER_FUNCTION_SRGB, CM_TRANSFER_FUNCTION_ST2084_PQ), {.min = 0.F, .max = 203.F}, PQ_RANGE), {PQ, PQ, PQ, 0.5F},
               1e-4F);
}

// Regression: the transformed-window blur matte was read from red, which holds colour-managed display light.
TEST(ColorManagementShader, BlurMatteCoverageIsReadFromAlpha) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3) || !compiler.canDraw())
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 and float render targets is unavailable";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    auto          defines     = surfaceDefines();
    defines["USE_RGBA"]       = "1";
    defines["USE_BLUR"]       = "1";
    defines["USE_BLUR_MATTE"] = "1";
    RGBA result               = {};
    // A transparent window pixel, the blurred background and a fully covered matte whose RGB is PQ(203 cd/m²).
    ASSERT_TRUE(compiler.draw(
        {
            .fragment = loader.process(FRAG_SHADERS[SH_FRAG_SURFACE], defines),
            .uniforms =
                [](GLuint program) {
                    glUniform1i(glGetUniformLocation(program, "tex"), 0);
                    glUniform1i(glGetUniformLocation(program, "blurredBG"), 1);
                    glUniform1i(glGetUniformLocation(program, "blurAlphaMatte"), 2);
                    glUniform1f(glGetUniformLocation(program, "alpha"), 1.F);
                    glUniform1f(glGetUniformLocation(program, "blurAlpha"), 1.F);
                    glUniform2f(glGetUniformLocation(program, "uvSize"), 1.F, 1.F);
                    glUniform2f(glGetUniformLocation(program, "uvOffset"), 0.F, 0.F);
                },
            .textures = {{0.F, 0.F, 0.F, 0.F}, {0.3F, 0.3F, 0.3F, 1.F}, {0.58069F, 0.58069F, 0.58069F, 1.F}},
        },
        result));
    // Textures are sampled at the default sampler precision, which may be FP16.
    expectRGBA(result, {0.3F, 0.3F, 0.3F, 1.F}, 2e-4F);
}

// Regression: decode and encode each divided by max(alpha, 0.001), attenuating FP16 pixels with a smaller alpha.
TEST(ColorManagementShader, ConversionKeepsColourBelowTheOldAlphaFloor) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3) || !compiler.canDraw())
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 and float render targets is unavailable";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    auto          defines = cmDefines(CM_TRANSFER_FUNCTION_EXT_LINEAR, CM_TRANSFER_FUNCTION_EXT_LINEAR);
    defines["USE_RGBA"]   = "1";
    const float A         = 1.F / 2048.F;
    RGBA        result    = {};
    ASSERT_TRUE(compiler.draw(
        {
            .fragment = loader.process(FRAG_SHADERS[SH_FRAG_SURFACE], defines),
            .uniforms =
                [](GLuint program) {
                    setColorManagementUniforms(program, {.min = 0.F, .max = 80.F}, {.min = 0.F, .max = 80.F});
                    glUniform1i(glGetUniformLocation(program, "tex"), 0);
                    glUniform1f(glGetUniformLocation(program, "alpha"), 1.F);
                },
            .textures = {{A, A, A, A}},
        },
        result));
    expectRGBA(result, {A, A, A, A}, A * 1e-4F);
}

static float drawTintedSurface(CShaderCompiler& compiler, eTransferFunction tf, Render::STFRange range, float signal) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    auto          defines = cmDefines(tf, tf);
    defines["USE_RGBA"]   = "1";
    defines["USE_TINT"]   = "1";
    RGBA result           = {};
    EXPECT_TRUE(compiler.draw(
        {
            .fragment = loader.process(FRAG_SHADERS[SH_FRAG_SURFACE], defines),
            .uniforms =
                [&](GLuint program) {
                    setColorManagementUniforms(program, range, range);
                    glUniform1i(glGetUniformLocation(program, "tex"), 0);
                    glUniform1f(glGetUniformLocation(program, "alpha"), 1.F);
                    glUniform3f(glGetUniformLocation(program, "tint"), 0.5F, 0.5F, 0.5F);
                },
            .textures = {{signal, signal, signal, 1.F}},
        },
        result));
    EXPECT_FLOAT_EQ(result[3], 1.F);
    return result[0];
}

// Regression: window dimming multiplied PQ code values, so a 0.5 dim left 5 instead of 50 of 100 cd/m².
TEST(ColorManagementShader, TintScalesDisplayLightOnHDREncodings) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3) || !compiler.canDraw())
        GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 and float render targets is unavailable";

    const Render::STFRange PQ_RANGE = {.min = 0.005F, .max = 10000.005F};
    const float            dimmed   = drawTintedSurface(compiler, CM_TRANSFER_FUNCTION_ST2084_PQ, PQ_RANGE, encodeChannel(100.0, CM_TRANSFER_FUNCTION_ST2084_PQ, PQ_RANGE));
    // The tolerance covers FP16 texture sampling, about 0.1 cd/m² per step at this level. Code value multiplication gave 5.45 cd/m².
    EXPECT_NEAR(decodeChannel(dimmed, CM_TRANSFER_FUNCTION_ST2084_PQ, PQ_RANGE), 50.0, 0.5);

    // HLG takes the same display light path: half the light is a smaller, but not half, signal.
    const float hlg = drawTintedSurface(compiler, CM_TRANSFER_FUNCTION_HLG, {.min = 0.005F, .max = 1000.F}, 0.75F);
    EXPECT_GT(hlg, 0.375F);
    EXPECT_LT(hlg, 0.75F);

    // SDR encodings keep multiplying code values.
    EXPECT_NEAR(drawTintedSurface(compiler, CM_TRANSFER_FUNCTION_SRGB, {.min = 0.F, .max = 80.F}, 0.6F), 0.3F, 2e-4F);
}

TEST(ColorManagementShader, HLGRedTintUsesTargetPrimaries) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3) || !compiler.canDraw())
        GTEST_SKIP() << "Requires surfaceless GLES 3 with float render targets";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    const auto    fragment = std::string{"#version 300 es\n#define ALLOW_INCLUDES\nprecision highp float;\n"} + loader.process("cm_helpers.glsl", surfaceDefines()) +
        "out vec4 result; void main() { result = applyTint(vec4(0.75,0,0,1),vec3(0.5),CM_TRANSFER_FUNCTION_HLG,vec2(0,1000)); }";
    RGBA result{};
    ASSERT_TRUE(compiler.draw({.fragment = fragment,
                               .uniforms =
                                   [](GLuint program) {
                                       glUniform3f(glGetUniformLocation(program, "sourceLuminanceCoefficients"), 0.2126F, 0.7152F, 0.0722F);
                                       glUniform3f(glGetUniformLocation(program, "targetLuminanceCoefficients"), 0.2627F, 0.6780F, 0.0593F);
                                   }},
                              result));
    expectRGBA(result, {0.63239658F, 0, 0, 1}, 1e-4F);
}

TEST(ColorManagementShader, RoundedMatteKeepsFractionalCoverageThroughTransformAndMotionBlur) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3) || !compiler.canDraw())
        GTEST_SKIP() << "Requires surfaceless GLES 3 with float render targets";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    auto          defines   = surfaceDefines();
    defines["USE_ROUNDING"] = "1";
    const auto format       = Hyprgraphics::Egl::getPixelFormatFromDRM(WINDOW_BLUR_MATTE_FORMAT);
    ASSERT_NE(format, nullptr);
    const GLenum storage = format->glInternalFormat ? format->glInternalFormat : GL_RGBA8;
    RGBA         edge{};
    ASSERT_TRUE(compiler.draw(
        {
            .fragment = loader.process(FRAG_SHADERS[SH_FRAG_QUAD], defines),
            .uniforms =
                [](GLuint program) {
                    // Put the fragment centre exactly on a circular corner of radius two.
                    const float origin = std::sqrt(2.F) - 1.75F;
                    glUniform2f(glGetUniformLocation(program, "topLeft"), origin, origin);
                    glUniform2f(glGetUniformLocation(program, "fullSize"), 4.F, 4.F);
                    glUniform1f(glGetUniformLocation(program, "radius"), 2.F);
                    glUniform1f(glGetUniformLocation(program, "roundingPower"), 2.F);
                },
            .targetFormat = storage,
            .color        = {1, 1, 1, 1},
        },
        edge));
    EXPECT_NEAR(edge[3], 0.5F, 1.F / 255.F);

    defines             = surfaceDefines();
    defines["USE_RGBA"] = "1";
    RGBA transformed{};
    ASSERT_TRUE(compiler.draw(
        {
            .fragment     = loader.process(FRAG_SHADERS[SH_FRAG_SURFACE], defines),
            .uniforms     = [](GLuint program) { glUniform1f(glGetUniformLocation(program, "alpha"), 1.F); },
            .textures     = {edge},
            .targetFormat = storage,
        },
        transformed));

    defines["USE_BLUR"]        = "1";
    defines["USE_BLUR_MATTE"]  = "1";
    defines["USE_MOTION_BLUR"] = "1";
    RGBA result{};
    ASSERT_TRUE(compiler.draw(
        {
            .fragment = loader.process(FRAG_SHADERS[SH_FRAG_SURFACE], defines),
            .uniforms =
                [](GLuint program) {
                    glUniform1i(glGetUniformLocation(program, "blurredBG"), 1);
                    glUniform1i(glGetUniformLocation(program, "blurAlphaMatte"), 2);
                    glUniform1f(glGetUniformLocation(program, "alpha"), 1.F);
                    glUniform1f(glGetUniformLocation(program, "blurAlpha"), 1.F);
                    for (const auto name : {"motionPrevBox", "motionCurrBox", "motionSourceBox"})
                        glUniform4f(glGetUniformLocation(program, name), 0, 0, 1, 1);
                    glUniform2f(glGetUniformLocation(program, "motionSourceTexSize"), 1, 1);
                    glUniform1i(glGetUniformLocation(program, "motionSamples"), 4);
                },
            .textures = {{0, 0, 0, 0}, {1, 1, 1, 1}, transformed},
        },
        result));
    expectRGBA(result, {0.5F, 0.5F, 0.5F, 0.5F}, 1.F / 255.F);
}
