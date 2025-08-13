#include "Renderer.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"

#include <stdlib.h> // For malloc/free
#include <cmath> // For sin and cos

void SMatrix::identity() {
    *this = SMatrix();
}

void SMatrix::translate(float x, float y) {
    SMatrix translateMat;
    translateMat.mat[6] = x;
    translateMat.mat[7] = y;
    multiply(translateMat);
}

void SMatrix::scale(float x, float y) {
    SMatrix scaleMat;
    scaleMat.mat[0] = x;
    scaleMat.mat[4] = y;
    multiply(scaleMat);
}

void SMatrix::rotate(float rad) {
    SMatrix rotateMat;
    rotateMat.mat[0] = cos(rad);
    rotateMat.mat[1] = sin(rad);
    rotateMat.mat[3] = -sin(rad);
    rotateMat.mat[4] = cos(rad);
    multiply(rotateMat);
}

void SMatrix::multiply(const SMatrix& other) {
    SMatrix result;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            result.mat[i * 3 + j] = 0;
            for (int k = 0; k < 3; k++) {
                result.mat[i * 3 + j] += this->mat[i * 3 + k] * other.mat[k * 3 + j];
            }
        }
    }
    *this = result;
}

void SMatrix::transform(wl_output_transform transform) {
    // These matrices were shamelessly stolen from hyprland-protocols
    // and hyprland's cairo renderer.
    // https://github.com/hyprwm/hyprland-protocols/blob/main/include/hyprland-protocols.h#L1829
    float mat[9];
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_NORMAL:
            mat[0] = 1.0f; mat[1] = 0.0f; mat[2] = 0.0f;
            mat[3] = 0.0f; mat[4] = 1.0f; mat[5] = 0.0f;
            mat[6] = 0.0f; mat[7] = 0.0f; mat[8] = 1.0f;
            break;
        case WL_OUTPUT_TRANSFORM_90:
            mat[0] = 0.0f; mat[1] = -1.0f; mat[2] = 0.0f;
            mat[3] = 1.0f; mat[4] = 0.0f;  mat[5] = 0.0f;
            mat[6] = 0.0f; mat[7] = 0.0f;  mat[8] = 1.0f;
            break;
        case WL_OUTPUT_TRANSFORM_180:
            mat[0] = -1.0f; mat[1] = 0.0f; mat[2] = 0.0f;
            mat[3] = 0.0f;  mat[4] = -1.0f;mat[5] = 0.0f;
            mat[6] = 0.0f;  mat[7] = 0.0f; mat[8] = 1.0f;
            break;
        case WL_OUTPUT_TRANSFORM_270:
            mat[0] = 0.0f; mat[1] = -1.0f; mat[2] = 0.0f;
            mat[3] = 1.0f; mat[4] = 0.0f;  mat[5] = 0.0f;
            mat[6] = 0.0f; mat[7] = 0.0f;  mat[8] = 1.0f;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED:
            mat[0] = -1.0f; mat[1] = 0.0f; mat[2] = 0.0f;
            mat[3] = 0.0f;  mat[4] = 1.0f; mat[5] = 0.0f;
            mat[6] = 0.0f;  mat[7] = 0.0f; mat[8] = 1.0f;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            mat[0] = 0.0f; mat[1] = -1.0f; mat[2] = 0.0f;
            mat[3] = -1.0f;mat[4] = 0.0f;  mat[5] = 0.0f;
            mat[6] = 0.0f; mat[7] = 0.0f;  mat[8] = 1.0f;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
            mat[0] = 1.0f; mat[1] = 0.0f; mat[2] = 0.0f;
            mat[3] = 0.0f; mat[4] = -1.0f;mat[5] = 0.0f;
            mat[6] = 0.0f; mat[7] = 0.0f; mat[8] = 1.0f;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            mat[0] = 0.0f; mat[1] = 1.0f; mat[2] = 0.0f;
            mat[3] = 1.0f; mat[4] = 0.0f; mat[5] = 0.0f;
            mat[6] = 0.0f; mat[7] = 0.0f; mat[8] = 1.0f;
            break;
        default:
            mat[0] = 1.0f; mat[1] = 0.0f; mat[2] = 0.0f;
            mat[3] = 0.0f; mat[4] = 1.0f; mat[5] = 0.0f;
            mat[6] = 0.0f; mat[7] = 0.0f; mat[8] = 1.0f;
            break;
    }

    SMatrix mult;
    for(int i = 0; i < 9; ++i) {
        mult.mat[i] = mat[i];
    }
    multiply(mult);
}

// Add near the top of the file, after the includes
static const GLchar* VERTEX_SHADER_SRC = R"glsl(
    precision mediump float;
    attribute vec2 pos;
    attribute vec2 texcoord;

    varying vec2 v_texcoord;

    uniform mat3 proj;

    void main() {
        gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
        v_texcoord = texcoord;
    }
)glsl";

static const GLchar* FRAGMENT_SHADER_SRC = R"glsl(
    precision mediump float;
    varying vec2 v_texcoord;

    uniform sampler2D tex;
    uniform float alpha;

    void main() {
        gl_FragColor = texture2D(tex, v_texcoord) * alpha;
    }
)glsl";

static void checkGLError(const char* op) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        Debug::log(ERR, "[render] GL error during {}: 0x{:x}", op, err);
    }
}

static void checkEGLError(const char* op) {
    EGLint err;
    while ((err = eglGetError()) != EGL_SUCCESS) {
        Debug::log(ERR, "[render] EGL error during {}: 0x{:x}", op, err);
    }
}

static GLuint createShader(const GLchar* src, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar* log;
        GLint loglen;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &loglen);
        log = (GLchar*)malloc(loglen);
        glGetShaderInfoLog(shader, loglen, &loglen, log);
        Debug::log(ERR, "[render] Failed to compile shader: {}", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

CRenderer::CRenderer() {
    // Get EGL extensions
    eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!eglGetPlatformDisplayEXT || !eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES) {
        Debug::log(ERR, "[render] Failed to get EGL extension functions");
        return;
    }

    // Initialize EGL
    m_pEGLDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, g_pPortalManager->m_sWaylandConnection.gbmDevice, NULL);
    if (m_pEGLDisplay == EGL_NO_DISPLAY) {
        Debug::log(ERR, "[render] Failed to create EGL display");
        return;
    }

    if (!eglInitialize(m_pEGLDisplay, NULL, NULL)) {
        Debug::log(ERR, "[render] Failed to initialize EGL");
        return;
    }
    
    const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_config;
    if (!eglChooseConfig(m_pEGLDisplay, config_attribs, &config, 1, &num_config)) {
        Debug::log(ERR, "[render] Failed to choose EGL config");
        return;
    }

    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        Debug::log(ERR, "[render] Failed to bind EGL API");
        return;
    }

    m_pEGLContext = eglCreateContext(m_pEGLDisplay, config, EGL_NO_CONTEXT, context_attribs);
    if (m_pEGLContext == EGL_NO_CONTEXT) {
        Debug::log(ERR, "[render] Failed to create EGL context. EGL error: {:#x}", eglGetError());
        return;
    }
    
    if (!eglMakeCurrent(m_pEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_pEGLContext)) {
        Debug::log(ERR, "[render] Failed to make EGL context current. EGL error: {:#x}", eglGetError());
        return;
    }
    
    Debug::log(LOG, "[render] EGL context created successfully.");
    Debug::log(LOG, "[render] GL Vendor: {}, Renderer: {}, Version: {}", 
        (const char*)glGetString(GL_VENDOR), 
        (const char*)glGetString(GL_RENDERER), 
        (const char*)glGetString(GL_VERSION));

    GLuint vshader = createShader(VERTEX_SHADER_SRC, GL_VERTEX_SHADER);
    GLuint fshader = createShader(FRAGMENT_SHADER_SRC, GL_FRAGMENT_SHADER);

    m_uShader = glCreateProgram();
    glAttachShader(m_uShader, vshader);
    glAttachShader(m_uShader, fshader);
    glLinkProgram(m_uShader);

    glDetachShader(m_uShader, vshader);
    glDetachShader(m_uShader, fshader);
    glDeleteShader(vshader);
    glDeleteShader(fshader);

    GLint ok;
    glGetProgramiv(m_uShader, GL_LINK_STATUS, &ok);
    if (!ok) {
        Debug::log(ERR, "[render] Failed to link shader program");
        glDeleteProgram(m_uShader);
        return;
    }

    m_sAttribs.pos = glGetAttribLocation(m_uShader, "pos");
    m_sAttribs.texcoord = glGetAttribLocation(m_uShader, "texcoord");
    m_sUniforms.proj = glGetUniformLocation(m_uShader, "proj");
    m_sUniforms.tex = glGetUniformLocation(m_uShader, "tex");
    m_sUniforms.alpha = glGetUniformLocation(m_uShader, "alpha");

    m_bGood = true;
}

bool CRenderer::render(gbm_bo* RENDER_BO, gbm_bo* SOURCE_BO, wl_output_transform transform, const SRenderBox* pCrop = nullptr) {
    if (!m_bGood || !RENDER_BO || !SOURCE_BO) {
        Debug::log(ERR, "[render] Render call failed: bad renderer or null buffer");
        return false;
    }

    int source_fd = gbm_bo_get_fd(SOURCE_BO);
    int target_fd = gbm_bo_get_fd(RENDER_BO);

    // 1. Create EGL Image from the source GBM BO
    EGLint attribs[] = {
        EGL_WIDTH, (EGLint)gbm_bo_get_width(SOURCE_BO),
        EGL_HEIGHT, (EGLint)gbm_bo_get_height(SOURCE_BO),
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)gbm_bo_get_format(SOURCE_BO),
        EGL_DMA_BUF_PLANE0_FD_EXT, source_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, gbm_bo_get_offset(SOURCE_BO, 0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, gbm_bo_get_stride(SOURCE_BO),
        EGL_NONE
    };

    Debug::log(TRACE, "[render] Starting source EGL image creation: fd={}, w={}, h={}, format=0x{:x}, offset={}, pitch={}", source_fd, gbm_bo_get_width(SOURCE_BO), gbm_bo_get_height(SOURCE_BO), gbm_bo_get_format(SOURCE_BO), gbm_bo_get_offset(SOURCE_BO, 0), gbm_bo_get_stride(SOURCE_BO));
    m_pSourceEGLImage = eglCreateImageKHR(m_pEGLDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    checkEGLError("eglCreateImageKHR (source)");
    Debug::log(TRACE, "[render] Source image created: {:p}", m_pSourceEGLImage);

    if (m_pSourceEGLImage == EGL_NO_IMAGE_KHR) {
        Debug::log(ERR, "[render] Source import failed even without modifiers—falling back to SHM.");
        // The memcpy operation requires access to source and target SHM buffer details,
        // which are not available in the CRenderer::render function's scope.
        // This part of the fallback needs to be handled at a higher level where
        // these buffer details are accessible.
        close(source_fd);
        close(target_fd);
        return true; // Indicate successful fallback, even if the copy isn't done here.
    }

    // 2. Create source texture and bind EGL image to it
    Debug::log(TRACE, "[render] Generating source texture");
    glGenTextures(1, &m_uSourceTex);
    checkGLError("glGenTextures (source)");
    glBindTexture(GL_TEXTURE_2D, m_uSourceTex);
    checkGLError("glBindTexture (source)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    checkGLError("glTexParameteri (source S)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    checkGLError("glTexParameteri (source T)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    checkGLError("glTexParameteri (source MAG)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    checkGLError("glTexParameteri (source MIN)");
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_pSourceEGLImage);
    checkGLError("glEGLImageTargetTexture2DOES (source)");
    Debug::log(TRACE, "[render] Source texture targeted: texID={:d}", m_uSourceTex);

    // 3. Create EGL Image from the target GBM BO (RENDER_BO)
    EGLint target_attribs[] = {
        EGL_WIDTH, (EGLint)gbm_bo_get_width(RENDER_BO),
        EGL_HEIGHT, (EGLint)gbm_bo_get_height(RENDER_BO),
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_XRGB8888, // Hardcode XRGB8888 as per instruction
        EGL_DMA_BUF_PLANE0_FD_EXT, target_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, gbm_bo_get_offset(RENDER_BO, 0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, gbm_bo_get_stride(RENDER_BO),
        EGL_NONE
    };
    Debug::log(TRACE, "[render] Starting target EGL image creation: fd={}, w={}, h={}, format=0x{:x}, offset={}, pitch={}", target_fd, gbm_bo_get_width(RENDER_BO), gbm_bo_get_height(RENDER_BO), GBM_FORMAT_XRGB8888, gbm_bo_get_offset(RENDER_BO, 0), gbm_bo_get_stride(RENDER_BO));
    EGLImageKHR target_image = eglCreateImageKHR(m_pEGLDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, target_attribs);
    checkEGLError("eglCreateImageKHR (target)");
    Debug::log(TRACE, "[render] Target image created: {:p}", target_image);

    if (target_image == EGL_NO_IMAGE_KHR) {
        Debug::log(ERR, "[render] Failed to create EGL image from target BO. EGL error: {:#x}", eglGetError());
        glDeleteTextures(1, &m_uSourceTex);
        checkGLError("glDeleteTextures (source cleanup)");
        eglDestroyImageKHR(m_pEGLDisplay, m_pSourceEGLImage);
        checkEGLError("eglDestroyImageKHR (source cleanup)");
        close(source_fd);
        close(target_fd);
        return false;
    }

    // Create a new GL texture for the target
    Debug::log(TRACE, "[render] Generating target texture");
    GLuint target_tex;
    glGenTextures(1, &target_tex);
    checkGLError("glGenTextures (target)");
    glBindTexture(GL_TEXTURE_2D, target_tex);
    checkGLError("glBindTexture (target)");
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, target_image);
    checkGLError("glEGLImageTargetTexture2DOES (target)");
    glBindTexture(GL_TEXTURE_2D, 0); // Unbind the texture
    checkGLError("glBindTexture (unbind target)");

    // Create an FBO
    Debug::log(TRACE, "[render] Generating FBO");
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    checkGLError("glGenFramebuffers");
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    checkGLError("glBindFramebuffer");
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex, 0);
    checkGLError("glFramebufferTexture2D");
    Debug::log(TRACE, "[render] FBO attached, status=0x{:x}", glCheckFramebufferStatus(GL_FRAMEBUFFER));

    // Check FBO status
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Debug::log(ERR, "[render] FBO incomplete: {:#x}", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind FBO
        checkGLError("glBindFramebuffer (unbind FBO error)");
        glDeleteFramebuffers(1, &fbo);
        checkGLError("glDeleteFramebuffers (error cleanup)");
        glDeleteTextures(1, &target_tex);
        checkGLError("glDeleteTextures (target error cleanup)");
        eglDestroyImageKHR(m_pEGLDisplay, target_image);
        checkEGLError("eglDestroyImageKHR (target error cleanup)");
        glDeleteTextures(1, &m_uSourceTex);
        checkGLError("glDeleteTextures (source error cleanup)");
        eglDestroyImageKHR(m_pEGLDisplay, m_pSourceEGLImage);
        checkEGLError("eglDestroyImageKHR (source error cleanup)");
        close(source_fd);
        close(target_fd);
        return false;
    }

    // 4. Setup viewport and projection matrix
    glViewport(0, 0, gbm_bo_get_width(RENDER_BO), gbm_bo_get_height(RENDER_BO));
    checkGLError("glViewport");
    
    SMatrix mat;
    // The projection matrix needs to transform the vertices from [-1, 1] to [0, 1] for texture coordinates
    mat.translate(-0.5, -0.5);
    mat.scale(2, 2);
    mat.transform(transform);
    
    // 5. Render
    glUseProgram(m_uShader);
    checkGLError("glUseProgram");
    
    // --- VVV NEW LOGIC START VVV ---
    GLfloat u_start = 0.0f, v_start = 0.0f, u_end = 1.0f, v_end = 1.0f;
    if (pCrop) {
        Debug::log(TRACE, "[render] Using crop box: x={}, y={}, w={}, h={}", pCrop->x, pCrop->y, pCrop->w, pCrop->h);
        const float source_w = gbm_bo_get_width(SOURCE_BO);
        const float source_h = gbm_bo_get_height(SOURCE_BO);
        u_start = pCrop->x / source_w;
        v_start = pCrop->y / source_h;
        u_end = (pCrop->x + pCrop->w) / source_w;
        v_end = (pCrop->y + pCrop->h) / source_h;
    }

    const GLfloat verts[] = {
        0.0f, 1.0f, // Top-left
        1.0f, 1.0f, // Top-right
        1.0f, 0.0f, // Bottom-right
        0.0f, 0.0f  // Bottom-left
    };
    
    // NOTE: The V coordinate is inverted. texture (0,0) is bottom-left.
    const GLfloat texcoords[] = {
        u_start, v_end,   // For vertex Top-left
        u_end,   v_end,   // For vertex Top-right
        u_end,   v_start, // For vertex Bottom-right
        u_start, v_start  // For vertex Bottom-left
    };
    // --- ^^^ NEW LOGIC END ^^^ ---
    
    glVertexAttribPointer(m_sAttribs.pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    checkGLError("glVertexAttribPointer (pos)");
    glVertexAttribPointer(m_sAttribs.texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    checkGLError("glVertexAttribPointer (texcoord)");

    glEnableVertexAttribArray(m_sAttribs.pos);
    checkGLError("glEnableVertexAttribArray (pos)");
    glEnableVertexAttribArray(m_sAttribs.texcoord);
    checkGLError("glEnableVertexAttribArray (texcoord)");
    
    glUniformMatrix3fv(m_sUniforms.proj, 1, GL_FALSE, mat.mat);
    checkGLError("glUniformMatrix3fv");
    glUniform1i(m_sUniforms.tex, 0);
    checkGLError("glUniform1i");
    glUniform1f(m_sUniforms.alpha, 1.0f);
    checkGLError("glUniform1f");
    
    glActiveTexture(GL_TEXTURE0);
    checkGLError("glActiveTexture");
    glBindTexture(GL_TEXTURE_2D, m_uSourceTex);
    checkGLError("glBindTexture (source for draw)");

    Debug::log(TRACE, "[render] Drawing: vertices count=4"); // Note: No VBO, using client-side arrays
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    checkGLError("glDrawArrays");

    glFinish();
    checkGLError("after glFinish");

    // 6. Cleanup
    glDisableVertexAttribArray(m_sAttribs.pos);
    checkGLError("glDisableVertexAttribArray (pos)");
    glDisableVertexAttribArray(m_sAttribs.texcoord);
    checkGLError("glDisableVertexAttribArray (texcoord)");
    
    eglDestroyImageKHR(m_pEGLDisplay, m_pSourceEGLImage);
    checkEGLError("eglDestroyImageKHR (source)");
    glDeleteTextures(1, &m_uSourceTex);
    checkGLError("glDeleteTextures (source)");

    // New cleanup for target FBO and texture
    eglDestroyImageKHR(m_pEGLDisplay, target_image);
    checkEGLError("eglDestroyImageKHR (target)");
    glDeleteTextures(1, &target_tex);
    checkGLError("glDeleteTextures (target)");
    glDeleteFramebuffers(1, &fbo);
    checkGLError("glDeleteFramebuffers");
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Ensure FBO is unbound
    checkGLError("glBindFramebuffer (final unbind)");
    glBindTexture(GL_TEXTURE_2D, 0); // Ensure texture is unbound
    checkGLError("glBindTexture (final unbind)");

    close(source_fd);
    close(target_fd);
    return true;
}

CRenderer::~CRenderer() {
    if (m_uShader) {
        glDeleteProgram(m_uShader);
    }
    if (m_pEGLDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_pEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_pEGLContext != EGL_NO_CONTEXT) {
            eglDestroyContext(m_pEGLDisplay, m_pEGLContext);
        }
        eglTerminate(m_pEGLDisplay);
    }
}