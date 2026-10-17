#include <render/Shader.hpp>
#include <gtest/gtest.h>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <set>

struct SShaderFailureState {
    bool             enabled = false;
    std::set<GLuint> shaders, programs;
    GLuint           nextID        = 0;
    GLenum           failingStage  = GL_NONE;
    GLuint           failingShader = 0;
};

static SShaderFailureState state;

// Linker wrappers inject failures only during this test; other tests use real GL.
extern "C" GLuint __real_glCreateShader(GLenum type);
extern "C" GLuint __wrap_glCreateShader(GLenum type) {
    if (!state.enabled)
        return __real_glCreateShader(type);
    const auto id = ++state.nextID;
    state.shaders.insert(id);
    if (type == state.failingStage)
        state.failingShader = id;
    return id;
}

extern "C" void __real_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* source, const GLint* length);
extern "C" void __wrap_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* source, const GLint* length) {
    if (!state.enabled)
        return __real_glShaderSource(shader, count, source, length);
    ;
}

extern "C" void __real_glCompileShader(GLuint shader);
extern "C" void __wrap_glCompileShader(GLuint shader) {
    if (!state.enabled)
        return __real_glCompileShader(shader);
    ;
}

extern "C" void __real_glGetShaderiv(GLuint shader, GLenum pname, GLint* value);
extern "C" void __wrap_glGetShaderiv(GLuint shader, GLenum pname, GLint* value) {
    if (!state.enabled)
        return __real_glGetShaderiv(shader, pname, value);
    *value = pname == GL_COMPILE_STATUS ? (shader != state.failingShader) : 0;
}

extern "C" void __real_glGetShaderInfoLog(GLuint shader, GLsizei size, GLsizei* length, GLchar* log);
extern "C" void __wrap_glGetShaderInfoLog(GLuint shader, GLsizei size, GLsizei* length, GLchar* log) {
    if (!state.enabled)
        return __real_glGetShaderInfoLog(shader, size, length, log);
    ;
}

extern "C" void __real_glDeleteShader(GLuint shader);
extern "C" void __wrap_glDeleteShader(GLuint shader) {
    if (!state.enabled)
        return __real_glDeleteShader(shader);
    EXPECT_EQ(state.shaders.erase(shader), 1U);
}

extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    if (!state.enabled)
        return __real_glCreateProgram();
    const auto id = ++state.nextID;
    state.programs.insert(id);
    return id;
}

extern "C" void __real_glAttachShader(GLuint program, GLuint shader);
extern "C" void __wrap_glAttachShader(GLuint program, GLuint shader) {
    if (!state.enabled)
        return __real_glAttachShader(program, shader);
    ;
}

extern "C" void __real_glDetachShader(GLuint program, GLuint shader);
extern "C" void __wrap_glDetachShader(GLuint program, GLuint shader) {
    if (!state.enabled)
        return __real_glDetachShader(program, shader);
    ;
}

extern "C" void __real_glLinkProgram(GLuint program);
extern "C" void __wrap_glLinkProgram(GLuint program) {
    if (!state.enabled)
        return __real_glLinkProgram(program);
    ;
}

extern "C" void __real_glGetProgramiv(GLuint program, GLenum pname, GLint* value);
extern "C" void __wrap_glGetProgramiv(GLuint program, GLenum pname, GLint* value) {
    if (!state.enabled)
        return __real_glGetProgramiv(program, pname, value);
    *value = 0;
}

extern "C" void __real_glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei* length, GLchar* log);
extern "C" void __wrap_glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei* length, GLchar* log) {
    if (!state.enabled)
        return __real_glGetProgramInfoLog(program, size, length, log);
    ;
}

extern "C" void __real_glDeleteProgram(GLuint program);
extern "C" void __wrap_glDeleteProgram(GLuint program) {
    if (!state.enabled)
        return __real_glDeleteProgram(program);
    EXPECT_EQ(state.programs.erase(program), 1U);
}

TEST(ShaderFailure, ReleasesObjectsOnEveryDynamicFailure) {
    state.enabled = true;
    const Hyprutils::Utils::CScopeGuard restore([] { state = {}; });
    for (const auto stage : {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_NONE}) {
        SCOPED_TRACE(stage);
        state.nextID        = 0;
        state.failingShader = 0;
        state.failingStage  = stage;
        CShader shader;
        EXPECT_FALSE(shader.createProgram("vertex", "fragment", true, true));
        EXPECT_EQ(shader.program(), 0U);
        EXPECT_TRUE(state.shaders.empty());
        EXPECT_TRUE(state.programs.empty());
        EXPECT_EQ(state.nextID, stage == GL_VERTEX_SHADER ? 1U : stage == GL_FRAGMENT_SHADER ? 2U : 3U);
    }
}
