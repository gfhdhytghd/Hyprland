#pragma once

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <vector>

using RGBA = std::array<float, 4>;

// One fragment shader invocation, drawn over a 1x1 float target.
struct SShaderDraw {
    std::string fragment;
    // Called with the program in use, to set uniforms.
    std::function<void(GLuint program)> uniforms;
    // 1x1 RGBA32F textures bound to units 0, 1, ...
    std::vector<RGBA> textures;
    // Destination before the draw, and premultiplied source-over (ONE, ONE_MINUS_SRC_ALPHA) when blend is set.
    RGBA   clear        = {0.F, 0.F, 0.F, 0.F};
    bool   blend        = false;
    GLenum targetFormat = GL_RGBA32F;
    // v_color of the vertex stage.
    RGBA color = {0.F, 0.F, 0.F, 0.F};
};

class CShaderCompiler {
  public:
    ~CShaderCompiler();

    bool                     initialize(int version);
    bool                     supportsExtension(const char* extension) const;
    testing::AssertionResult compile(const std::string& source, GLenum stage) const;
    // Needs a GLES 3 context with EXT_color_buffer_float, see canDraw.
    bool                     canDraw() const;
    testing::AssertionResult draw(const SShaderDraw& draw, RGBA& result) const;

  private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
};
