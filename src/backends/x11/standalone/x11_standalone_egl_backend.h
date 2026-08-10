/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "../common/x11_common_egl_backend.h"
#include "core/outputlayer.h"
#include "openglsurfacetexture_x11.h"
#include "utils/damagejournal.h"

#include <kwingltexture.h>
#include <kwingltexture_p.h>

#include <chrono>
#include <map>
#include <memory>

namespace KWin
{

class EglPixmapTexturePrivate;
class SoftwareVsyncMonitor;
class X11StandaloneBackend;
class EglBackend;
class Output;
class RenderLoop;

/**
 * One EglLayer is created per output. Each layer owns its own child window of the
 * shared X composite overlay window, its own EGLSurface and framebuffer, and its own
 * software vsync monitor that drives the render loop of that specific output. This is
 * what allows outputs with different refresh rates to be paced independently on X11,
 * mirroring the GlxLayer design in x11_standalone_glx_backend.cpp.
 */
class EglLayer : public OutputLayer
{
public:
    EglLayer(EglBackend *backend, Output *output);
    ~EglLayer() override;

    std::optional<OutputLayerBeginFrameInfo> beginFrame() override;
    bool endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion) override;

    /**
     * Makes sure the child window, EGLSurface and framebuffer for this output exist.
     * Must be called with a valid EGL context. Returns @c false on failure.
     */
    bool ensureResources();
    void present();
    Output *output() const;
    EGLSurface surface() const;

private:
    void vblank(std::chrono::nanoseconds timestamp);
    void updateSize();

    EglBackend *const m_backend;
    Output *const m_output;
    ::Window m_window = None;
    EGLSurface m_surface = EGL_NO_SURFACE;
    std::unique_ptr<GLFramebuffer> m_fbo;
    DamageJournal m_damageJournal;
    QRegion m_lastRenderedRegion;
    int m_bufferAge = 0;
    std::unique_ptr<SoftwareVsyncMonitor> m_vsyncMonitor;
};

class EglBackend : public EglOnXBackend
{
    Q_OBJECT

public:
    EglBackend(Display *display, X11StandaloneBackend *platform);
    ~EglBackend() override;

    void init() override;

    std::unique_ptr<SurfaceTexture> createSurfaceTextureX11(SurfacePixmapX11 *texture) override;
    void present(Output *output) override;
    bool makeCurrent() override;
    OverlayWindow *overlayWindow() const override;
    OutputLayer *primaryLayer(Output *output) override;

protected:
    bool createSurfaces() override;

private:
    void screenGeometryChanged();
    void removeLayer(Output *output);

    /**
     * Makes the EGL context current on the surface of @p layer (or the bootstrap
     * surface if @p layer is @c nullptr), tracking it as the active layer so that
     * subsequent makeCurrent() calls during the same frame target the same output.
     */
    bool makeCurrentForLayer(EglLayer *layer);

    // Thin wrappers around EglOnXBackend's protected members so EglLayer (a friend of
    // this class, but not of EglOnXBackend) can create and configure its own surface.
    EGLSurface createLayerSurface(xcb_window_t window)
    {
        return createSurface(window);
    }
    bool havePostSubBuffer() const
    {
        return EglOnXBackend::havePostSubBuffer();
    }
    // Without eglPostSubBufferNV, eglSwapBuffers() does not preserve the back buffer,
    // so partial repaints would lose whatever was drawn in previous frames. Ask EGL to
    // preserve it instead, at the cost of vsync/performance (see EglOnXBackend::init(),
    // which does the same for the bootstrap surface).
    void configureSurfaceSwapBehavior(EGLSurface surface)
    {
        if (!havePostSubBuffer()) {
            eglSurfaceAttrib(eglDisplay(), surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
        }
    }

    X11StandaloneBackend *m_backend;
    std::unique_ptr<OverlayWindow> m_overlayWindow;
    // A child window of the overlay that exists only to give the shared EGL context a
    // stable surface for context/config setup and for makeCurrent() calls that are not
    // tied to a specific output (e.g. texture uploads). It is never mapped.
    ::Window m_bootstrapWindow = None;
    EGLSurface m_bootstrapSurface = EGL_NO_SURFACE;
    // One layer (child window + surface + framebuffer + vsync monitor) per output.
    std::map<Output *, std::unique_ptr<EglLayer>> m_layers;
    EglLayer *m_currentLayer = nullptr;
    friend class EglLayer;
    friend class EglPixmapTexturePrivate;
};

class EglPixmapTexture : public GLTexture
{
public:
    explicit EglPixmapTexture(EglBackend *backend);

    bool create(SurfacePixmapX11 *texture);

private:
    Q_DECLARE_PRIVATE(EglPixmapTexture)
};

class EglPixmapTexturePrivate : public GLTexturePrivate
{
public:
    EglPixmapTexturePrivate(EglPixmapTexture *texture, EglBackend *backend);
    ~EglPixmapTexturePrivate() override;

    bool create(SurfacePixmapX11 *texture);

protected:
    void onDamage() override;

private:
    EglPixmapTexture *q;
    EglBackend *m_backend;
    EGLImageKHR m_image = EGL_NO_IMAGE_KHR;
};

class EglSurfaceTextureX11 : public OpenGLSurfaceTextureX11
{
public:
    EglSurfaceTextureX11(EglBackend *backend, SurfacePixmapX11 *texture);

    bool create() override;
    void update(const QRegion &region) override;
};

} // namespace KWin
