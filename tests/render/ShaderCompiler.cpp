#include "ShaderCompiler.hpp"

#include <EGL/eglext.h>
#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>

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

static GLuint compileShader(const std::string& source, GLenum stage, std::string& error) {
    const auto shader = glCreateShader(stage);
    const auto text   = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    error.assign(std::max(length, 1), '\0');
    glGetShaderInfoLog(shader, length, nullptr, error.data());
    glDeleteShader(shader);
    return 0;
}

bool CShaderCompiler::canDraw() const {
    return m_context != EGL_NO_CONTEXT && supportsExtension("GL_EXT_color_buffer_float");
}

testing::AssertionResult CShaderCompiler::draw(const SShaderDraw& draw, RGBA& result) const {
    // A full-target triangle; v_texcoord is the target centre for the single pixel.
    static constexpr const char* VERTEX = R"(#version 300 es
uniform vec4 color;
out vec4 v_color;
out vec2 v_texcoord;
out vec2 v_texcoordMatte;
void main() {
    vec2 p          = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position     = vec4(p, 0.0, 1.0);
    v_color         = color;
    v_texcoord      = p * 0.5 + 0.5;
    v_texcoordMatte = v_texcoord;
}
)";

    std::string                  error;
    const GLuint                 vertex = compileShader(VERTEX, GL_VERTEX_SHADER, error);
    if (!vertex)
        return testing::AssertionFailure() << "vertex: " << error.c_str();
    const GLuint fragment = compileShader(draw.fragment, GL_FRAGMENT_SHADER, error);
    if (!fragment) {
        glDeleteShader(vertex);
        return testing::AssertionFailure() << "fragment: " << error.c_str();
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    std::vector<GLuint>                 textures(draw.textures.size(), 0);
    GLuint                              target = 0, framebuffer = 0, vao = 0;
    const Hyprutils::Utils::CScopeGuard cleanup([&] {
        glDeleteProgram(program);
        if (!textures.empty())
            glDeleteTextures(textures.size(), textures.data());
        glDeleteTextures(1, &target);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteVertexArrays(1, &vao);
    });

    GLint                               linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        error.assign(std::max(length, 1), '\0');
        glGetProgramInfoLog(program, length, nullptr, error.data());
        return testing::AssertionFailure() << "link: " << error.c_str();
    }

    if (!textures.empty())
        glGenTextures(textures.size(), textures.data());
    for (size_t i = 0; i < textures.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, draw.textures[i].data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    glGenTextures(1, &target);
    glActiveTexture(GL_TEXTURE0 + textures.size());
    glBindTexture(GL_TEXTURE_2D, target);
    glTexStorage2D(GL_TEXTURE_2D, 1, draw.targetFormat, 1, 1);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return testing::AssertionFailure() << "Incomplete framebuffer for GL format " << draw.targetFormat;

    glViewport(0, 0, 1, 1);
    glClearBufferfv(GL_COLOR, 0, draw.clear.data());
    if (draw.blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else
        glDisable(GL_BLEND);

    glUseProgram(program);
    glUniform4fv(glGetUniformLocation(program, "color"), 1, draw.color.data());
    for (size_t i = 0; i < textures.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
    }
    if (draw.uniforms)
        draw.uniforms(program);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (draw.targetFormat == GL_RGBA32F)
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, result.data());
    else {
        std::array<GLubyte, 4> bytes{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
        for (size_t i = 0; i < bytes.size(); ++i)
            result[i] = bytes[i] / 255.F;
    }
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (const auto glError = glGetError(); glError != GL_NO_ERROR)
        return testing::AssertionFailure() << "GL error " << glError;
    return testing::AssertionSuccess();
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
