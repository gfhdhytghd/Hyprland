#include <render/ShaderLoader.hpp>
#include <render/shaders/Shaders.hpp>
#include <glslang/Public/resource_limits_c.h>
#include <gtest/gtest.h>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <memory>

using namespace Render;
using namespace NColorManagement;

TEST(ColorManagementShader, SurfaceAndBlurVariantsCompile) {
    ASSERT_TRUE(glslang_initialize_process());
    const Hyprutils::Utils::CScopeGuard finalize([] { glslang_finalize_process(); });
    std::vector<std::string>            includes;
    for (const auto& [name, source] : SHADERS)
        if (name.ends_with(".h") || name.ends_with(".glsl"))
            includes.emplace_back(name);
    std::array<std::string, SH_FRAG_LAST> fragments;
    fragments.fill("surface.frag");
    fragments[SH_FRAG_BLURPREPARE] = "blurprepare.frag";
    fragments[SH_FRAG_BLURFINISH]  = "blurfinish.frag";
    fragments[SH_FRAG_PRISMFINISH] = "prismfinish.frag";
    CShaderLoader loader(includes, fragments);
    for (const auto fragment : {SH_FRAG_SURFACE, SH_FRAG_BLURPREPARE, SH_FRAG_BLURFINISH, SH_FRAG_PRISMFINISH}) {
        for (const auto tf : {CM_TRANSFER_FUNCTION_BT1886, CM_TRANSFER_FUNCTION_EXT_LINEAR, CM_TRANSFER_FUNCTION_ST2084_PQ, CM_TRANSFER_FUNCTION_HLG}) {
            for (const auto extra : {0, sc<int>(SH_FEAT_SDR_MOD), sc<int>(SH_FEAT_TONEMAP | SH_FEAT_MIRROR), sc<int>(SH_FEAT_TONEMAP | SH_FEAT_SDR_MOD | SH_FEAT_ALT_TONEMAP)}) {
                if (fragment == SH_FRAG_BLURPREPARE && extra != 0)
                    continue;
                if (fragment != SH_FRAG_SURFACE && (extra & SH_FEAT_TONEMAP))
                    continue;
                const auto source = loader.getVariantSource(fragment, {.features = sc<ShaderFeatureFlags>(SH_FEAT_CM | extra), .sourceTF = tf, .targetTF = tf});
                ASSERT_FALSE(source.empty());
                const glslang_input_t input{
                    .language        = GLSLANG_SOURCE_GLSL,
                    .stage           = GLSLANG_STAGE_FRAGMENT,
                    .client          = GLSLANG_CLIENT_NONE,
                    .target_language = GLSLANG_TARGET_NONE,
                    .code            = source.c_str(),
                    .default_version = 300,
                    .default_profile = GLSLANG_ES_PROFILE,
                    .messages        = GLSLANG_MSG_DEFAULT_BIT,
                    .resource        = glslang_default_resource(),
                };
                const std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)> shader(glslang_shader_create(&input), glslang_shader_delete);
                ASSERT_TRUE(shader);
                ASSERT_TRUE(glslang_shader_preprocess(shader.get(), &input)) << glslang_shader_get_info_log(shader.get());
                EXPECT_TRUE(glslang_shader_parse(shader.get(), &input))
                    << "fragment=" << fragment << " TF=" << tf << " features=" << extra << " " << glslang_shader_get_info_log(shader.get());
            }
        }
    }
}
