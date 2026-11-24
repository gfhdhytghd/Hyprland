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
