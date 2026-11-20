#include "ShaderCompiler.hpp"

#include <EGL/eglext.h>
#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Hyprutils::Memory::rc;

CShaderCompiler::~CShaderCompiler() {
    if (m_display == EGL_NO_DISPLAY)
        return;

    if (m_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(m_display, m_context);
    }
    if (m_surface != EGL_NO_SURFACE)
        eglDestroySurface(m_display, m_surface);
    eglTerminate(m_display);
}

bool CShaderCompiler::initialize(int version) {
    const auto getPlatformDisplay = rc<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!getPlatformDisplay)
        return false;

    m_display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API))
        return false;

    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, version == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig    config        = nullptr;
    EGLint       configCount   = 0;
    if (!eglChooseConfig(m_display, configAttrs, &config, 1, &configCount) || configCount == 0)
        return false;

    const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
    m_context                   = eglCreateContext(m_display, config, EGL_NO_CONTEXT, contextAttrs);
    if (m_context == EGL_NO_CONTEXT)
        return false;

    const EGLint surfaceAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    m_surface                   = eglCreatePbufferSurface(m_display, config, surfaceAttrs);
    return m_surface != EGL_NO_SURFACE && eglMakeCurrent(m_display, m_surface, m_surface, m_context);
}

bool CShaderCompiler::supportsExtension(const char* extension) const {
    const auto extensions = rc<const char*>(glGetString(GL_EXTENSIONS));
    return extensions && (std::string(" ") + extensions + " ").contains(std::string(" ") + extension + " ");
}

testing::AssertionResult CShaderCompiler::compile(const std::string& source, GLenum stage) const {
    if (source.empty())
        return testing::AssertionFailure() << "Shader source is empty";

    const auto shader = glCreateShader(stage);
    if (!shader)
        return testing::AssertionFailure() << "glCreateShader failed";

    const Hyprutils::Utils::CScopeGuard cleanup([shader] { glDeleteShader(shader); });
    const auto                          text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return testing::AssertionSuccess();

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 0)
        return testing::AssertionFailure() << "Shader compilation failed without an info log";

    std::string log(length, '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    return testing::AssertionFailure() << log.c_str();
}
