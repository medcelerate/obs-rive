// backend_gl.cpp
//
// Linux Rive renderer backend. Creates a headless EGL context with a 1x1
// PBuffer surface, then renders Rive into an offscreen GL texture. CPU
// readback is via glReadPixels into a heap-allocated row buffer (GL writes
// bottom-up; we flip on the way out so OBS receives top-down BGRA, matching
// the macOS / Windows backends).
//
// EGL is required at link time (-lEGL -lGL on Linux).

#include "IBackend.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstring>
#include <string>
#include <vector>

// Rive's GL renderer expects glad-loaded entry points; pull in the loader and
// the GLES3-flavored headers it ships. texture.hpp must come before
// render_context_impl.hpp so rcp<rive::gpu::Texture>'s dtor has the full
// type (same trick we use in backend_d3d11.cpp).
#include "rive/renderer/gl/gles3.hpp"
#include "rive/renderer/texture.hpp"
#include "rive/renderer/gl/render_context_gl_impl.hpp"
#include "rive/renderer/gl/render_target_gl.hpp"
#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"

namespace obsrive {

class GLBackend : public IBackend {
public:
    ~GLBackend() override
    {
        // Make sure we own a current context for the teardown of Rive's GL
        // resources (RenderContext destructor will issue glDelete* calls).
        if (mEglDpy != EGL_NO_DISPLAY && mEglCtx != EGL_NO_CONTEXT) {
            eglMakeCurrent(mEglDpy, mEglSurface, mEglSurface, mEglCtx);
        }
        if (mReadFbo) { glDeleteFramebuffers(1, &mReadFbo); mReadFbo = 0; }
        if (mTex)     { glDeleteTextures(1, &mTex);         mTex = 0;     }
        mRenderTarget.reset();
        if (mRenderContext) {
            mRenderContext->releaseResources();
            mRenderContext.reset();
        }
        if (mEglDpy != EGL_NO_DISPLAY) {
            eglMakeCurrent(mEglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (mEglCtx     != EGL_NO_CONTEXT) eglDestroyContext(mEglDpy, mEglCtx);
            if (mEglSurface != EGL_NO_SURFACE) eglDestroySurface(mEglDpy, mEglSurface);
            eglTerminate(mEglDpy);
        }
    }

    bool init(std::string& err) override
    {
        mEglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (mEglDpy == EGL_NO_DISPLAY) { err = "eglGetDisplay failed."; return false; }

        EGLint major = 0, minor = 0;
        if (!eglInitialize(mEglDpy, &major, &minor)) {
            err = "eglInitialize failed.";
            mEglDpy = EGL_NO_DISPLAY;
            return false;
        }

        if (!eglBindAPI(EGL_OPENGL_API)) {
            err = "eglBindAPI(OPENGL) failed.";
            return false;
        }

        const EGLint cfgAttribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_NONE
        };
        EGLConfig cfg;
        EGLint nCfg = 0;
        if (!eglChooseConfig(mEglDpy, cfgAttribs, &cfg, 1, &nCfg) || nCfg < 1) {
            err = "eglChooseConfig failed.";
            return false;
        }

        const EGLint pbAttribs[] = {
            EGL_WIDTH,  1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        mEglSurface = eglCreatePbufferSurface(mEglDpy, cfg, pbAttribs);
        if (mEglSurface == EGL_NO_SURFACE) {
            err = "eglCreatePbufferSurface failed.";
            return false;
        }

        // Desktop GL 4.5 - the version Rive's PLS renderer targets.
        const EGLint ctxAttribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 4,
            EGL_CONTEXT_MINOR_VERSION, 5,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        mEglCtx = eglCreateContext(mEglDpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
        if (mEglCtx == EGL_NO_CONTEXT) {
            err = "eglCreateContext failed.";
            return false;
        }

        if (!eglMakeCurrent(mEglDpy, mEglSurface, mEglSurface, mEglCtx)) {
            err = "eglMakeCurrent failed.";
            return false;
        }

        // Rive's GL backend needs glad-loaded function pointers.
        if (!gladLoadCustomLoader(
                reinterpret_cast<GLADloadfunc>(eglGetProcAddress))) {
            err = "gladLoadCustomLoader failed.";
            return false;
        }

        rive::gpu::RenderContextGLImpl::ContextOptions opts;
        mRenderContext = rive::gpu::RenderContextGLImpl::MakeContext(opts);
        if (!mRenderContext) {
            err = "Failed to create Rive GL render context.";
            return false;
        }
        return true;
    }

    rive::Factory*            factory()       override { return mRenderContext.get(); }
    rive::gpu::RenderContext* renderContext() override { return mRenderContext.get(); }

    bool ensureRenderTarget(uint32_t w, uint32_t h, std::string& err) override
    {
        if (w == 0 || h == 0) { err = "Render target has zero size."; return false; }
        if (mTex && mW == w && mH == h && mRenderTarget) return true;

        if (mReadFbo) { glDeleteFramebuffers(1, &mReadFbo); mReadFbo = 0; }
        if (mTex)     { glDeleteTextures(1, &mTex);         mTex = 0;     }

        glGenTextures(1, &mTex);
        glBindTexture(GL_TEXTURE_2D, mTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Readback FBO that aliases the offscreen texture as color
        // attachment 0, so we can glReadPixels into client memory.
        // (glGetTexImage isn't available in Rive's glad subset.)
        glGenFramebuffers(1, &mReadFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, mReadFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, mTex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            err = "Readback FBO incomplete (status=" + std::to_string(status) + ")";
            return false;
        }

        auto rt = rive::make_rcp<rive::gpu::TextureRenderTargetGL>(w, h);
        rt->setTargetTexture(mTex);
        mRenderTarget = std::move(rt);

        mReadBuf.resize((size_t)w * h * 4);
        mW = w;
        mH = h;
        return true;
    }

    bool renderAndReadback(const rive::gpu::RenderContext::FrameDescriptor& fd,
                           const std::function<void(rive::Renderer*)>&      draw,
                           void*                                            dst,
                           std::string&                                     err) override
    {
        if (!mRenderContext || !mRenderTarget) {
            err = "GL backend not initialized.";
            return false;
        }

        mRenderContext->beginFrame(fd);
        rive::RiveRenderer renderer(mRenderContext.get());
        draw(&renderer);

        rive::gpu::RenderContext::FlushResources flush;
        flush.renderTarget = mRenderTarget.get();
        flush.externalCommandBuffer = nullptr;
        mRenderContext->flush(flush);

        // Read pixels: GL returns RGBA, OBS / our cross-platform contract is
        // BGRA top-down. Swap R/B and Y-flip on the fly.
        glBindFramebuffer(GL_FRAMEBUFFER, mReadFbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, (GLsizei)mW, (GLsizei)mH,
                     GL_RGBA, GL_UNSIGNED_BYTE, mReadBuf.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const size_t rowBytes = (size_t)mW * 4;
        uint8_t* d = (uint8_t*)dst;
        for (uint32_t y = 0; y < mH; ++y) {
            const uint8_t* src = mReadBuf.data() + (size_t)(mH - 1 - y) * rowBytes;
            for (uint32_t x = 0; x < mW; ++x) {
                d[x * 4 + 0] = src[x * 4 + 2]; // B
                d[x * 4 + 1] = src[x * 4 + 1]; // G
                d[x * 4 + 2] = src[x * 4 + 0]; // R
                d[x * 4 + 3] = src[x * 4 + 3]; // A
            }
            d += rowBytes;
        }
        return true;
    }

private:
    EGLDisplay mEglDpy = EGL_NO_DISPLAY;
    EGLSurface mEglSurface = EGL_NO_SURFACE;
    EGLContext mEglCtx = EGL_NO_CONTEXT;

    GLuint                                    mTex = 0;
    GLuint                                    mReadFbo = 0;
    uint32_t                                  mW = 0, mH = 0;
    std::vector<uint8_t>                      mReadBuf;
    std::unique_ptr<rive::gpu::RenderContext> mRenderContext;
    rive::rcp<rive::gpu::RenderTargetGL>      mRenderTarget;
};

std::unique_ptr<IBackend> CreateBackend()
{
    return std::make_unique<GLBackend>();
}

} // namespace obsrive
