#pragma once

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <gtest/gtest.h>

class CShaderCompiler {
  public:
    ~CShaderCompiler();

    bool                     initialize(int version);
    bool                     supportsExtension(const char* extension) const;
    testing::AssertionResult compile(const std::string& source, GLenum stage) const;

  private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
};
