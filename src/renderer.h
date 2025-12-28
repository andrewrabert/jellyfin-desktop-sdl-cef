#pragma once

#include <GL/glew.h>
#include <cstdint>

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(int width, int height);

    // Update CEF overlay texture
    void updateOverlayTexture(const void* buffer, int width, int height);

    // Render video texture (opaque)
    void renderVideo(GLuint video_texture);

    // Render CEF overlay with alpha
    void renderOverlay(float alpha);

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint overlay_texture_ = 0;
    GLuint shader_program_ = 0;
    GLint alpha_uniform_ = -1;
    int tex_width_ = 0;
    int tex_height_ = 0;

    GLuint compileShader(GLenum type, const char* source);
    GLuint createShaderProgram();
};
