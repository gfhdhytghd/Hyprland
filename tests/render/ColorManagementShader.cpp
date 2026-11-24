#include <render/ShaderLoader.hpp>
#include <render/CMSettings.hpp>
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
