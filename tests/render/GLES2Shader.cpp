#include <render/ShaderLoader.hpp>
#include <Compositor.hpp>
#include <config/supplementary/jeremy/Jeremy.hpp>
#include <config/ConfigValue.hpp>
#include <config/lua/ConfigManager.hpp>
#include <config/shared/inotify/ConfigWatcher.hpp>
#include <glslang/Public/resource_limits_c.h>
#include <gtest/gtest.h>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <memory>

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

TEST(GLES2Conversion, RejectsMRTAndIntegerOutputs) {
    EXPECT_FALSE(toGLES2("layout(location = 0) out vec4 color;\nlayout(location = 1) out vec4 mirror;\n", true));
    EXPECT_FALSE(toGLES2("out uvec4 color;\n", true));
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
        ASSERT_TRUE(glslang_initialize_process());
    }

    void TearDown() override {
        glslang_finalize_process();
        Config::mgr()     = std::move(m_previousConfig);
        Config::watcher() = std::move(m_previousWatcher);
        if (Config::mgr())
            CConfigValueBase::flushCaches();
        g_pCompositor = std::move(m_previousCompositor);
        Config::Supplementary::Jeremy::flushCachedCfgPath();
    }

    void validate(const std::string& source, glslang_stage_t stage) {
        ASSERT_TRUE(source.starts_with("#version 100\n")) << source;
        const glslang_input_t input{
            .language        = GLSLANG_SOURCE_GLSL,
            .stage           = stage,
            .client          = GLSLANG_CLIENT_NONE,
            .target_language = GLSLANG_TARGET_NONE,
            .code            = source.c_str(),
            .default_version = 100,
            .default_profile = GLSLANG_ES_PROFILE,
            .messages        = GLSLANG_MSG_DEFAULT_BIT,
            .resource        = glslang_default_resource(),
        };
        const std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)> shader(glslang_shader_create(&input), glslang_shader_delete);
        ASSERT_TRUE(shader);
        ASSERT_TRUE(glslang_shader_preprocess(shader.get(), &input)) << glslang_shader_get_info_log(shader.get());
        EXPECT_TRUE(glslang_shader_parse(shader.get(), &input)) << glslang_shader_get_info_log(shader.get());
    }

  private:
    UP<CCompositor>            m_previousCompositor;
    UP<Config::IConfigManager> m_previousConfig;
    UP<Config::CConfigWatcher> m_previousWatcher;
};

TEST_F(CGLES2ShaderTest, AllLegacyVariantsCompile) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    validate(loader.process("tex300.vert"), GLSLANG_STAGE_VERTEX);
    for (const auto frag : {SH_FRAG_QUAD, SH_FRAG_PASSTHRURGBA, SH_FRAG_MATTE, SH_FRAG_EXT, SH_FRAG_SHADOW, SH_FRAG_INNER_GLOW, SH_FRAG_SURFACE, SH_FRAG_BORDER1, SH_FRAG_GLITCH}) {
        for (ShaderFeatureFlags features = 0; features < 16; ++features) {
            SCOPED_TRACE(FRAG_SHADERS[frag] + " features=" + std::to_string(features));
            validate(loader.getVariantSource(frag, {.features = features}), GLSLANG_STAGE_FRAGMENT);
        }
    }
}

TEST_F(CGLES2ShaderTest, KawaseVariantsCompile) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    for (const auto frag : {SH_FRAG_BLUR1, SH_FRAG_BLUR2, SH_FRAG_BLURPREPARE, SH_FRAG_BLURFINISH}) {
        SCOPED_TRACE(FRAG_SHADERS[frag]);
        validate(loader.getVariantSource(frag, {}), GLSLANG_STAGE_FRAGMENT);
    }
}

TEST_F(CGLES2ShaderTest, BlurBlendVariantsCompile) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    for (const auto frag : {SH_FRAG_SURFACE, SH_FRAG_EXT}) {
        for (ShaderFeatureFlags base = 0; base < 16; ++base) {
            for (const ShaderFeatureFlags blur :
                 std::array<ShaderFeatureFlags, 7>{SH_FEAT_BLUR_ALPHA_MASK, SH_FEAT_BLUR, SH_FEAT_BLUR | SH_FEAT_BLUR_ALPHA_MASK, SH_FEAT_BLUR | SH_FEAT_BLUR_MATTE,
                                                   SH_FEAT_BLUR | SH_FEAT_BLUR_ALPHA_MASK | SH_FEAT_BLUR_MATTE, SH_FEAT_BLUR | SH_FEAT_BLUR_PROPORTIONAL,
                                                   SH_FEAT_BLUR | SH_FEAT_BLUR_MATTE | SH_FEAT_BLUR_PROPORTIONAL}) {
                SCOPED_TRACE(FRAG_SHADERS[frag] + " features=" + std::to_string(base | blur));
                const auto source = loader.getVariantSource(frag, {.features = sc<ShaderFeatureFlags>(base | blur)});
                EXPECT_EQ(source.find("uniform sampler2D blurredBG") != std::string::npos, (blur & SH_FEAT_BLUR) != 0);
                EXPECT_EQ(source.find("uniform samplerExternalOES tex") != std::string::npos, frag == SH_FRAG_EXT);
                EXPECT_EQ(source.find("legacyBlurMask") != std::string::npos, (base & SH_FEAT_DISCARD) != 0 && (blur & SH_FEAT_BLUR) != 0);
                EXPECT_EQ(source.find("uniform sampler2D blurAlphaMatte") != std::string::npos, (blur & SH_FEAT_BLUR_MATTE) != 0);
                validate(source, GLSLANG_STAGE_FRAGMENT);
            }
        }
    }
}

TEST_F(CGLES2ShaderTest, UnsupportedFeaturesFallBack) {
    CShaderLoader loader(SHADER_INCLUDES, FRAG_SHADERS, "", true);
    const auto    features = SH_FEAT_RGBA | SH_FEAT_BLUR | SH_FEAT_BLUR_ALPHA_MASK | SH_FEAT_BLUR_MATTE | SH_FEAT_ROUNDING;
    const auto    fallback = loader.getVariantSource(SH_FRAG_SURFACE, {.features = sc<ShaderFeatureFlags>(features)});
    EXPECT_EQ(loader.getVariantSource(SH_FRAG_SURFACE, {.features = sc<ShaderFeatureFlags>(features | SH_FEAT_MOTION_BLUR | SH_FEAT_CM | SH_FEAT_ICC | SH_FEAT_MIRROR)}), fallback);
    EXPECT_EQ(fallback.find("textureSize"), std::string::npos);
    validate(fallback, GLSLANG_STAGE_FRAGMENT);
}
