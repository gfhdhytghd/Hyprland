#include <render/ShaderLoader.hpp>
#include <Compositor.hpp>
#include <config/supplementary/jeremy/Jeremy.hpp>
#include <config/ConfigValue.hpp>
#include <config/lua/ConfigManager.hpp>
#include <config/shared/inotify/ConfigWatcher.hpp>
#include "ShaderCompiler.hpp"
#include <gtest/gtest.h>

using namespace Render;

TEST(GLES2Conversion, VersionAndExtensions) {
    const auto source = toGLES2("#version 300 es\n#extension GL_ARB_shading_language_include : enable\n#extension GL_OES_EGL_image_external_essl3 : require\n", true);
    ASSERT_TRUE(source);
    EXPECT_EQ(*source, "#version 100\n#extension GL_OES_EGL_image_external : require\n");
}

TEST(GLES2Conversion, VertexAttributesAndVaryings) {
    const auto source = toGLES2("in vec2 pos;\nout vec2 uv;\n", false);
    ASSERT_TRUE(source);
    EXPECT_EQ(*source, "attribute vec2 pos;\nvarying vec2 uv;\n");
}

TEST(GLES2Conversion, FragmentOutputPrecisionAndSampling) {
    const auto source = toGLES2("precision highp float;\nin vec2 uv;\nlayout(location = 0) out vec4 color;\nvoid main() { color = texture(tex, uv); }\n", true);
    ASSERT_TRUE(source);
    EXPECT_NE(source->find("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif"), std::string::npos);
    EXPECT_NE(source->find("varying vec2 uv;"), std::string::npos);
    EXPECT_NE(source->find("#define color gl_FragColor"), std::string::npos);
    EXPECT_NE(source->find("texture2D(tex, uv)"), std::string::npos);
}

TEST(GLES2Conversion, FragmentOutputsWithTrailingComments) {
    const auto line = toGLES2("layout(location = 0) out vec4 fragColor; // output\n", true);
    ASSERT_TRUE(line);
    EXPECT_EQ(*line, "#define fragColor gl_FragColor\n");

    const auto block = toGLES2("out vec4 c; /* output */\n", true);
    ASSERT_TRUE(block);
    EXPECT_EQ(*block, "#define c gl_FragColor\n");
}

TEST(GLES2Conversion, BlockCommentsPreserveTokensAndDirectives) {
    const auto source = toGLES2("#if 1 /* first\nsecond */ && 1\nout/* type */vec4 c;\n#endif\n", true);
    ASSERT_TRUE(source);
    EXPECT_EQ(*source, "#if 1   && 1\n#define c gl_FragColor\n#endif\n");
}

TEST(GLES2Conversion, RejectsMRTAndIntegerOutputs) {
    EXPECT_FALSE(toGLES2("layout(location = 0) out vec4 color;\nlayout(location = 1) out vec4 mirror;\n", true));
    EXPECT_FALSE(toGLES2("out uvec4 color;\n", true));
}

TEST(GLES2Conversion, SurfaceKeepsGuardedBlurAndExternalBranches) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    const auto    source = loader.process("surface.frag", {{"LEGACY_GLES2", "1"}, {"USE_BLUR", "1"}, {"USE_DISCARD", "1"}, {"USE_BLUR_MATTE", "1"}, {"USE_EXTERNAL", "1"}});

    EXPECT_TRUE(source.contains("#if USE_BLUR\nuniform vec2"));
    EXPECT_TRUE(source.contains("uniform sampler2D blurredBG;"));
    EXPECT_TRUE(source.contains("#if USE_BLUR_MATTE\nuniform sampler2D blurAlphaMatte;"));
    EXPECT_TRUE(source.contains("#if USE_EXTERNAL\nuniform samplerExternalOES tex;\n#else\nuniform sampler2D tex;"));
    EXPECT_TRUE(source.contains("#if LEGACY_GLES2 && USE_BLUR && USE_DISCARD\n    float legacyBlurMask ="));
    EXPECT_TRUE(source.contains("pixBlurAlphaMask *= legacyBlurMask;"));
    EXPECT_TRUE(source.contains("float pixBlurAlphaMask = legacyBlurMask * blurAlpha;"));
}

class CGLES2ShaderTest : public testing::Test {
  protected:
    void SetUp() override {
        m_previousCompositor = std::move(g_pCompositor);
        g_pCompositor        = makeUnique<CCompositor>(true);
        Config::Supplementary::Jeremy::flushCachedCfgPath();
        m_previousWatcher = std::move(Config::watcher());
        Config::watcher() = makeUnique<Config::CConfigWatcher>();
        m_previousConfig  = std::move(Config::mgr());
        Config::mgr()     = makeUnique<Config::Lua::CConfigManager>();
        CConfigValueBase::flushCaches();
        if (!m_compiler.initialize(2))
            GTEST_SKIP() << "Surfaceless EGL with OpenGL ES 2 is unavailable";
    }

    void TearDown() override {
        Config::mgr()     = std::move(m_previousConfig);
        Config::watcher() = std::move(m_previousWatcher);
        if (Config::mgr())
            CConfigValueBase::flushCaches();
        g_pCompositor = std::move(m_previousCompositor);
        Config::Supplementary::Jeremy::flushCachedCfgPath();
    }

    void validate(const std::string& source, GLenum stage) {
        ASSERT_TRUE(source.starts_with("#version 100\n")) << source;
        EXPECT_TRUE(m_compiler.compile(source, stage));
    }

    void validateBlurBlend(ShaderFeatureFlags textureFeatures) {
        CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
        for (ShaderFeatureFlags base = 0; base < 16; ++base) {
            for (const ShaderFeatureFlags blur :
                 std::array<ShaderFeatureFlags, 7>{SH_FEAT_BLUR_ALPHA_MASK, SH_FEAT_BLUR, SH_FEAT_BLUR | SH_FEAT_BLUR_ALPHA_MASK, SH_FEAT_BLUR | SH_FEAT_BLUR_MATTE,
                                                   SH_FEAT_BLUR | SH_FEAT_BLUR_ALPHA_MASK | SH_FEAT_BLUR_MATTE, SH_FEAT_BLUR | SH_FEAT_BLUR_PROPORTIONAL,
                                                   SH_FEAT_BLUR | SH_FEAT_BLUR_MATTE | SH_FEAT_BLUR_PROPORTIONAL}) {
                const auto features = sc<ShaderFeatureFlags>(base | blur | textureFeatures);
                SCOPED_TRACE("surface.frag features=" + std::to_string(features));
                const auto source = loader.getVariantSource(SH_FRAG_SURFACE, {.features = features});
                EXPECT_NE(source.find(blur & SH_FEAT_BLUR ? "#define USE_BLUR 1\n" : "#define USE_BLUR 0\n"), std::string::npos);
                EXPECT_NE(source.find(blur & SH_FEAT_BLUR_MATTE ? "#define USE_BLUR_MATTE 1\n" : "#define USE_BLUR_MATTE 0\n"), std::string::npos);
                EXPECT_NE(source.find(textureFeatures & SH_FEAT_EXTERNAL ? "#define USE_EXTERNAL 1\n" : "#define USE_EXTERNAL 0\n"), std::string::npos);
                validate(source, GL_FRAGMENT_SHADER);
            }
        }
    }

    CShaderCompiler m_compiler;

  private:
    UP<CCompositor>            m_previousCompositor;
    UP<Config::IConfigManager> m_previousConfig;
    UP<Config::CConfigWatcher> m_previousWatcher;
};

TEST_F(CGLES2ShaderTest, AllLegacyVariantsCompile) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    validate(loader.process("tex300.vert"), GL_VERTEX_SHADER);
    for (const auto frag : {SH_FRAG_QUAD, SH_FRAG_PASSTHRURGBA, SH_FRAG_MATTE, SH_FRAG_SHADOW, SH_FRAG_INNER_GLOW, SH_FRAG_SURFACE, SH_FRAG_BORDER1, SH_FRAG_GLITCH}) {
        for (ShaderFeatureFlags features = 0; features < 16; ++features) {
            SCOPED_TRACE(FRAG_SHADERS[frag] + " features=" + std::to_string(features));
            validate(loader.getVariantSource(frag, {.features = features}), GL_FRAGMENT_SHADER);
        }
    }
}

TEST_F(CGLES2ShaderTest, KawaseVariantsCompile) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    for (const auto frag : {SH_FRAG_BLUR1, SH_FRAG_BLUR2, SH_FRAG_BLURPREPARE, SH_FRAG_BLURFINISH}) {
        SCOPED_TRACE(FRAG_SHADERS[frag]);
        validate(loader.getVariantSource(frag, {}), GL_FRAGMENT_SHADER);
    }
}

TEST_F(CGLES2ShaderTest, BlurBlendVariantsCompile) {
    validateBlurBlend(0);
}

TEST_F(CGLES2ShaderTest, ExternalVariantsCompile) {
    if (!m_compiler.supportsExtension("GL_OES_EGL_image_external"))
        GTEST_SKIP() << "GL_OES_EGL_image_external is unavailable";

    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    for (ShaderFeatureFlags base = 0; base < 16; ++base) {
        SCOPED_TRACE("surface.frag external features=" + std::to_string(base));
        validate(loader.getVariantSource(SH_FRAG_SURFACE, {.features = sc<ShaderFeatureFlags>(base | SH_FEAT_EXTERNAL)}), GL_FRAGMENT_SHADER);
    }
    validateBlurBlend(SH_FEAT_EXTERNAL);
}

TEST_F(CGLES2ShaderTest, UnsupportedFeaturesFallBack) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    const auto    features = SH_FEAT_RGBA | SH_FEAT_BLUR | SH_FEAT_BLUR_ALPHA_MASK | SH_FEAT_BLUR_MATTE | SH_FEAT_ROUNDING;
    const auto    fallback = loader.getVariantSource(SH_FRAG_SURFACE, {.features = sc<ShaderFeatureFlags>(features)});
    EXPECT_EQ(loader.getVariantSource(SH_FRAG_SURFACE, {.features = sc<ShaderFeatureFlags>(features | SH_FEAT_MOTION_BLUR | SH_FEAT_CM | SH_FEAT_ICC | SH_FEAT_MIRROR)}), fallback);
    validate(fallback, GL_FRAGMENT_SHADER);
}
