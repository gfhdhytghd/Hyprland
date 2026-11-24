#include <Compositor.hpp>
#include <config/ConfigValue.hpp>
#include <config/lua/ConfigManager.hpp>
#include <config/shared/inotify/ConfigWatcher.hpp>
#include <config/supplementary/jeremy/Jeremy.hpp>
#include <render/OpenGL.hpp>
#include <render/gl/GLTexture.hpp>
#include <render/CMSettings.hpp>
#include <helpers/cm/ICC.hpp>
#include <render/ShaderLoader.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
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

TEST(ColorManagementShader, NarrowBT1886TransferRetainsSignal) {
    CShaderCompiler compiler;
    if (!compiler.initialize(3))
        GTEST_SKIP() << "Surfaceless EGL is unavailable";
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 3 || (major == 3 && minor == 0))
        GTEST_SKIP() << "This numerical shader check requires OpenGL ES 3.1";
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
    const auto    source  = std::string("#version 310 es\n#define ALLOW_INCLUDES\nprecision highp float;\n") + loader.process("cm_helpers.glsl", surfaceDefines()) + R"(
        layout(local_size_x=1) in;
        layout(std430,binding=0) buffer Result { vec4 values; };
        uniform vec2 srcTFRange;
        void main() {
            vec2 range = vec2(srcTFRange.x, tfRangeSpans.x);
            float linear = tfInvBT1886(vec3(0.5), range).r;
            vec4 nits = doColorManagement(vec4(0.5, 0.5, 0.5, 1.0), 1.0, CM_TRANSFER_FUNCTION_BT1886,
                CM_TRANSFER_FUNCTION_LINEAR, mat3(1.0), srcTFRange, vec2(0.0, 80.0), 0.0);
            values = vec4(linear, tfBT1886(vec3(linear), range).r, nits.r, range.y);
        })";
    const auto    code    = source.c_str();
    const GLuint  program = glCreateShaderProgramv(GL_COMPUTE_SHADER, 1, &code);
    const Hyprutils::Utils::CScopeGuard cleanupProgram([program] { glDeleteProgram(program); });
    GLint                               linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE) << compiler.compile(source, GL_COMPUTE_SHADER).message();
    glUseProgram(program);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    const Hyprutils::Utils::CScopeGuard cleanupBuffer([buffer] { glDeleteBuffers(1, &buffer); });
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 4 * sizeof(float), nullptr, GL_DYNAMIC_READ);
    auto description             = DEFAULT_SRGB_IMAGE_DESCRIPTION->value();
    description.transferFunction = CM_TRANSFER_FUNCTION_BT1886;
    description.luminances       = {.min = 4095.9999, .max = 4096, .reference = 4096};
    CCMSettingsCache cache;
    const auto       range     = cache.get(CImageDescription::from(description), DEFAULT_SRGB_IMAGE_DESCRIPTION, {.sourceTF = description.transferFunction}).srcTFRange;
    const auto [minimum, span] = range.shaderMinAndSpan();
    glUniform2f(glGetUniformLocation(program, "srcTFRange"), minimum, range.max);
    glUniform2f(glGetUniformLocation(program, "tfRangeSpans"), span, 80.F);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    const auto values = sc<const float*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 4 * sizeof(float), GL_MAP_READ_BIT));
    ASSERT_NE(values, nullptr);
    EXPECT_NEAR(values[0], 0.5, 1e-6);
    EXPECT_NEAR(values[1], 0.5, 1e-6);
    EXPECT_FLOAT_EQ(values[2], 80);
    EXPECT_FLOAT_EQ(values[3], 1.953125e-6f);
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
}

namespace Render::GL {
    // Supply only the GLES entry points used by CShader. CShaderCompiler owns the EGL context.
    CHyprOpenGLImpl::CHyprOpenGLImpl(STestContext) : m_currentProgram(0) {
        m_proc.glGenVertexArrays    = glGenVertexArrays;
        m_proc.glBindVertexArray    = glBindVertexArray;
        m_proc.glDeleteVertexArrays = glDeleteVertexArrays;
    }

    class CICCSurfaceTest : public ::testing::Test {
      protected:
        void SetUp() override {
            if (!m_compiler.initialize(3))
                GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 3 is unavailable";
            m_previousCompositor                = std::move(g_pCompositor);
            m_previousConfig                    = std::move(Config::mgr());
            m_previousWatcher                   = std::move(Config::watcher());
            g_pCompositor                       = makeUnique<CCompositor>(true);
            g_pCompositor->m_explicitConfigPath = "icc-surface-test.lua";
            Config::Supplementary::Jeremy::flushCachedCfgPath();
            Config::watcher() = makeUnique<Config::CConfigWatcher>();
            Config::mgr()     = makeUnique<Config::Lua::CConfigManager>();
            CConfigValueBase::flushCaches();
            m_previous    = std::move(g_pHyprOpenGL);
            g_pHyprOpenGL = UP<CHyprOpenGLImpl>{new CHyprOpenGLImpl(CHyprOpenGLImpl::STestContext{})};
        }
        void TearDown() override {
            if (IsSkipped())
                return;
            g_pHyprOpenGL     = std::move(m_previous);
            Config::mgr()     = std::move(m_previousConfig);
            Config::watcher() = std::move(m_previousWatcher);
            if (Config::mgr())
                CConfigValueBase::flushCaches();
            g_pCompositor = std::move(m_previousCompositor);
            Config::Supplementary::Jeremy::flushCachedCfgPath();
        }
        void bindCM(const SP<CShader>& shader, PImageDescription target) {
            const auto settings = CCMSettingsCache{}.get(LINEAR_IMAGE_DESCRIPTION, target, {.sourceTF = CM_TRANSFER_FUNCTION_EXT_LINEAR});
            g_pHyprOpenGL->passCMUniforms(shader, LINEAR_IMAGE_DESCRIPTION, target, false, -1, settings);
        }
        UP<CCompositor>            m_previousCompositor;
        UP<Config::IConfigManager> m_previousConfig;
        UP<Config::CConfigWatcher> m_previousWatcher;
        CShaderCompiler            m_compiler;
        UP<CHyprOpenGLImpl>        m_previous;
    };

    static auto rgbProfile(const cmsCIExyYTRIPLE& primaries, double gamma) {
        const cmsCIExyY white{0.3127, 0.329, 1};
        const auto      curve    = cmsBuildGamma(nullptr, gamma);
        cmsToneCurve*   curves[] = {curve, curve, curve};
        const auto      profile  = cmsCreateRGBProfile(&white, &primaries, curves);
        cmsFreeToneCurve(curve);
        return std::unique_ptr<void, decltype(&cmsCloseProfile)>{profile, cmsCloseProfile};
    }

    TEST_F(CICCSurfaceTest, MatchesLittleCMSOnP3Output) {
        const auto p3    = rgbProfile({{0.68, 0.32, 1}, {0.265, 0.69, 1}, {0.15, 0.06, 1}}, 2.2);
        const auto bt709 = rgbProfile({{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}}, 1);
        ASSERT_TRUE(p3 && bt709);
        const std::unique_ptr<void, decltype(&cmsDeleteTransform)> direct{
            cmsCreateTransform(bt709.get(), TYPE_RGB_FLT, p3.get(), TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC),
            cmsDeleteTransform};
        ASSERT_TRUE(direct);
        const auto primaries = iccPrimaries(p3.get());
        ASSERT_TRUE(primaries);
        SImageDescription target{.primaries = *primaries, .icc = {.present = true}};
        ASSERT_TRUE(iccBuildLUTData(p3.get(), target));
        target.icc.lutTexture = makeShared<CGLTexture>(target.icc.lutDataPacked, target.icc.lutSize);

        CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS);
        const auto    shader = makeShared<CShader>();
        const auto    fragment =
            loader.getVariantSource(SH_FRAG_SURFACE, {.features = SH_FEAT_CM | SH_FEAT_ICC, .sourceTF = CM_TRANSFER_FUNCTION_EXT_LINEAR, .targetTF = target.transferFunction});
        ASSERT_TRUE(shader->createProgram(loader.process("tex300.vert"), fragment, true, true));
        glUseProgram(shader->program());
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, false, {2, 0, 0, 0, 2, 0, -1, -1, 1});
        bindCM(shader, CImageDescription::from(target));
        shader->setUniformFloat(SHADER_ALPHA, 1);
        shader->setUniformInt(SHADER_TEX, 0);

        const std::array<std::array<float, 4>, 4> pixels{{
            {1, 0, 0, 1},
            {1.224940f, -0.042057f, -0.019638f, 1},
            {0.00242f, 0.00242f, 0.00242f, 1},
            {0.61f, 0.35f, 0.42f, 1},
        }};
        GLuint                                    input = 0, output = 0, framebuffer = 0;
        glGenTextures(1, &input);
        glBindTexture(GL_TEXTURE_2D, input);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixels.size(), 1, 0, GL_RGBA, GL_FLOAT, pixels.data());
        glGenRenderbuffers(1, &output);
        glBindRenderbuffer(GL_RENDERBUFFER, output);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, pixels.size(), 1);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, output);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        glViewport(0, 0, pixels.size(), 1);
        glDisable(GL_DITHER);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        std::array<std::array<uint8_t, 4>, pixels.size()> actual{};
        glReadPixels(0, 0, pixels.size(), 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        for (size_t i = 0; i < pixels.size(); ++i) {
            SCOPED_TRACE(i);
            std::array<float, 3> expected{};
            cmsDoTransform(direct.get(), pixels[i].data(), expected.data(), 1);
            for (size_t channel = 0; channel < expected.size(); ++channel)
                EXPECT_NEAR(actual[i][channel], 255 * std::clamp(expected[channel], 0.f, 1.f), 1.5);
            EXPECT_EQ(actual[i][3], 255);
        }
    }
}

static void setColorManagementUniforms(GLuint program, Render::STFRange source, Render::STFRange target) {
    static constexpr std::array<GLfloat, 9> IDENTITY = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    glUniformMatrix3fv(glGetUniformLocation(program, "convertMatrix"), 1, GL_FALSE, IDENTITY.data());
    glUniform2f(glGetUniformLocation(program, "srcTFRange"), source.min, source.max);
    glUniform2f(glGetUniformLocation(program, "dstTFRange"), target.min, target.max);
    glUniform2f(glGetUniformLocation(program, "tfRangeSpans"), source.shaderMinAndSpan()[1], target.shaderMinAndSpan()[1]);
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
                                       glUniform2f(glGetUniformLocation(program, "tfRangeSpans"), 1000.F, 1000.F);
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
