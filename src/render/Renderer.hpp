#pragma once

#include "../includes.hpp" // For SP/WP
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <wayland-server-protocol.h> // For wl_output_transform enum

struct SMatrix {
    float mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    void identity();
    void translate(float x, float y);
    void scale(float x, float y);
    void rotate(float rad);
    void multiply(const SMatrix& other);
    void transform(wl_output_transform transform);
};

class CRenderer {
public:
    CRenderer();
    ~CRenderer();

    bool render(gbm_bo* RENDER_BO, gbm_bo* SOURCE_BO, wl_output_transform transform);

    EGLImageKHR m_pSourceEGLImage = EGL_NO_IMAGE_KHR;
    GLuint m_uSourceTex = 0;
    GLuint m_uRenderTex = 0;
    GLuint m_uFramebuffer = 0;

    bool m_bGood = false;

private:
    // EGL/OpenGL state
    EGLDisplay m_pEGLDisplay = EGL_NO_DISPLAY;
    EGLContext m_pEGLContext = EGL_NO_CONTEXT;

    // GL state
    GLuint m_uShader            = 0;
    struct {
        GLint proj     = -1;
        GLint tex      = -1;
        GLint alpha    = -1;
    } m_sUniforms;
    struct {
        GLint pos       = -1;
        GLint texcoord  = -1;
    } m_sAttribs;

    // EGL extensions
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = nullptr;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
};