#pragma once

// gl_utils.h — shared GLSL compile/link helpers.
//
// Both engine.cpp (for the fallback triangle) and shader_pipeline.cpp (for
// post-process shaders) need to compile and link GLSL programs.  Keeping one
// copy here prevents the two from drifting apart if error-reporting ever changes.
//
// static inline: each including translation unit gets its own copy of the
// generated machine code.  For two small functions this is a fine trade-off.

#include <glad/glad.h>
#include <cstdio>
#include <initializer_list>
#include <utility>

// Compile one GLSL shader stage.
// type: GL_VERTEX_SHADER or GL_FRAGMENT_SHADER.
// Returns 0 and prints the driver error log to stderr on failure.
static inline GLuint gl_compile_shader(GLenum type, const char* src) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        fprintf(stderr, "gl_compile_shader (%s):\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

// Link a vertex + fragment shader into a program.
// On success: deletes vert and frag (they're no longer needed) and returns prog.
// On failure: returns 0 and prints the driver link log to stderr.
//
// attribs: optional list of {slot, name} pairs passed to glBindAttribLocation
// before linking.  In GL 3.1 / GLSL 1.40, layout(location=N) is not available
// in vertex shaders, so we use this to pin attribute slots explicitly instead.
// The slot numbers must match the glVertexAttribPointer calls in the VAO setup.
static inline GLuint gl_link_program(GLuint vert, GLuint frag,
    std::initializer_list<std::pair<GLuint, const char*>> attribs = {})
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    for (const auto& [loc, name] : attribs)
        glBindAttribLocation(prog, loc, name);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "gl_link_program:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }

    // Individual stage objects are reference-counted by the driver.
    // Calling glDeleteShader here marks them for deletion — they'll be freed
    // once the program is deleted too.
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}
