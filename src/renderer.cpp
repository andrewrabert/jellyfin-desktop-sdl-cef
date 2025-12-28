#include "renderer.h"
#include <iostream>

static const char* vertex_shader_source = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

static const char* fragment_shader_source = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;
uniform sampler2D textureSampler;
uniform float alpha;
void main() {
    vec4 color = texture(textureSampler, TexCoord);
    FragColor = vec4(color.rgb, color.a * alpha);
}
)";

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (overlay_texture_) glDeleteTextures(1, &overlay_texture_);
    if (shader_program_) glDeleteProgram(shader_program_);
}

bool Renderer::init(int width, int height) {
    tex_width_ = width;
    tex_height_ = height;

    shader_program_ = createShaderProgram();
    if (!shader_program_) return false;

    alpha_uniform_ = glGetUniformLocation(shader_program_, "alpha");

    // Fullscreen quad (position + texcoord)
    float vertices[] = {
        // pos      // tex
        -1.0f,  1.0f,  0.0f, 0.0f,  // top-left
        -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right
        -1.0f,  1.0f,  0.0f, 0.0f,  // top-left
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right
         1.0f,  1.0f,  1.0f, 0.0f,  // top-right
    };

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Create overlay texture (for CEF)
    glGenTextures(1, &overlay_texture_);
    glBindTexture(GL_TEXTURE_2D, overlay_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

    glBindVertexArray(0);
    return true;
}

void Renderer::updateOverlayTexture(const void* buffer, int width, int height) {
    glBindTexture(GL_TEXTURE_2D, overlay_texture_);
    if (width != tex_width_ || height != tex_height_) {
        tex_width_ = width;
        tex_height_ = height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, buffer);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, buffer);
    }
}

void Renderer::renderVideo(GLuint video_texture) {
    glDisable(GL_BLEND);

    glUseProgram(shader_program_);
    glUniform1f(alpha_uniform_, 1.0f);  // Opaque

    glBindVertexArray(vao_);
    glBindTexture(GL_TEXTURE_2D, video_texture);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void Renderer::renderOverlay(float alpha) {
    if (alpha <= 0.0f) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(shader_program_);
    glUniform1f(alpha_uniform_, alpha);

    glBindVertexArray(vao_);
    glBindTexture(GL_TEXTURE_2D, overlay_texture_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

GLuint Renderer::compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "Shader compile error: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint Renderer::createShaderProgram() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (!vs || !fs) return 0;

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::cerr << "Shader link error: " << log << std::endl;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}
