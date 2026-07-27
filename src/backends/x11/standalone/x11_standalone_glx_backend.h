/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2012 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "core/outputlayer.h"
#include "openglbackend.h"
#include "openglsurfacetexture_x11.h"
#include "utils/damagejournal.h"
#include "x11eventfilter.h"

#include <epoxy/glx.h>
#include <fixx11h.h>
#include <xcb/glx.h>

#include <kwingltexture.h>
#include <kwingltexture_p.h>

#include <QHash>
#include <chrono>
#include <map>
#include <memory>

namespace KWin
{

class GlxPixmapTexturePrivate;
class VsyncMonitor;
class X11StandaloneBackend;
class GlxBackend;
class Output;
class RenderLoop;

// GLX_MESA_swap_interval
using glXSwapIntervalMESA_func = int (*)(unsigned int interval);
extern glXSwapIntervalMESA_func glXSwapIntervalMESA;

class FBConfigInfo
{
public:
    GLXFBConfig fbconfig;
    int bind_texture_format;
    int texture_targets;
    int y_inverted;
    int mipmap;
};

// ------------------------------------------------------------------

class SwapEventFilter : public X11EventFilter
{
public:
    SwapEventFilter(xcb_drawable_t drawable, xcb_glx_drawable_t glxDrawable, RenderLoop *renderLoop);
    bool event(xcb_generic_event_t *event) override;

private:
    xcb_drawable_t m_drawable;
    xcb_glx_drawable_t m_glxDrawable;
    RenderLoop *m_renderLoop;
};

/**
 * One GlxLayer is created per output. Each layer owns its own child window of the
 * shared X composite overlay window, its own GLX drawable and framebuffer, and its
 * own presentation feedback (swap event filter or vsync monitor) that drives the
 * render loop of that specific output. This is what allows outputs with different
 * refresh rates to be paced independently on X11.
 */
class GlxLayer : public OutputLayer
{
public:
    GlxLayer(GlxBackend *backend, Output *output);
    ~GlxLayer() override;

    std::optional<OutputLayerBeginFrameInfo> beginFrame() override;
    bool endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion) override;
    // KWIN_X11_UNREDIRECT_FULLSCREEN: when a fullscreen opaque window covers this whole
    // output with nothing on top, unredirect it so the X server scans it out / page-flips
    // it directly, and hide this output's overlay child so it shows through. Returns true
    // if the output is now handled by direct (unredirected) scanout instead of compositing.
    bool scanout(SurfaceItem *surfaceItem) override;

    /**
     * Makes sure the child window, GLX drawable and framebuffer for this output
     * exist. Must be called with a valid GLX context. Returns @c false on failure.
     */
    bool ensureResources();
    void present();
    Output *output() const;
    GLXWindow glxWindow() const;

private:
    void vblank(std::chrono::nanoseconds timestamp);
    void updateSize();

    // If this output was handed over to unredirected direct scanout, take it back:
    // re-redirect the window and re-map the overlay child so we can composite again.
    void exitScanoutIfActive();

    GlxBackend *const m_backend;
    Output *const m_output;
    ::Window m_window = None;
    GLXWindow m_glxWindow = None;
    std::unique_ptr<GLFramebuffer> m_fbo;
    DamageJournal m_damageJournal;
    QRegion m_lastRenderedRegion;
    int m_bufferAge = 0;
    std::unique_ptr<SwapEventFilter> m_swapEventFilter;
    std::unique_ptr<VsyncMonitor> m_vsyncMonitor;

    // Unredirect-fullscreen (direct scanout) state.
    bool m_scanoutActive = false;
    xcb_window_t m_scanoutWindow = XCB_WINDOW_NONE;
};

/**
 * @brief OpenGL Backend using GLX over an X overlay window.
 */
class GlxBackend : public OpenGLBackend
{
    Q_OBJECT

public:
    GlxBackend(Display *display, X11StandaloneBackend *backend);
    ~GlxBackend() override;
    std::unique_ptr<SurfaceTexture> createSurfaceTextureX11(SurfacePixmapX11 *pixmap) override;
    void present(Output *output) override;
    bool makeCurrent() override;
    void doneCurrent() override;
    OverlayWindow *overlayWindow() const override;
    void init() override;
    OutputLayer *primaryLayer(Output *output) override;

    Display *display() const
    {
        return m_x11Display;
    }

private:
    bool initBuffer();
    bool checkVersion();
    void initExtensions();
    bool initRenderingContext();
    bool initFbConfig();
    void initVisualDepthHashTable();
    void setSwapInterval(GLXWindow drawable, int interval);
    void screenGeometryChanged();
    void removeLayer(Output *output);

    /**
     * Makes the GLX context current on the drawable of @p layer (or the bootstrap
     * drawable if @p layer is @c nullptr), tracking it as the active layer so that
     * subsequent makeCurrent() calls during the same frame target the same output.
     */
    bool makeCurrentForLayer(GlxLayer *layer);

    int visualDepth(xcb_visualid_t visual) const;
    const FBConfigInfo &infoForVisual(xcb_visualid_t visual);

    /**
     * @brief The OverlayWindow used by this Backend.
     */
    std::unique_ptr<OverlayWindow> m_overlayWindow;
    // A child window of the overlay that exists only to give the shared GLX context
    // a stable drawable for context creation and for makeCurrent() calls that are
    // not tied to a specific output (e.g. texture uploads). It is never mapped.
    ::Window m_bootstrapWindow = None;
    GLXWindow m_bootstrapGlxWindow = None;
    GLXFBConfig fbconfig;
    GLXContext ctx;
    QHash<xcb_visualid_t, FBConfigInfo> m_fbconfigHash;
    QHash<xcb_visualid_t, int> m_visualDepthHash;
    // One layer (child window + drawable + framebuffer + presentation feedback) per output.
    std::map<Output *, std::unique_ptr<GlxLayer>> m_layers;
    GlxLayer *m_currentLayer = nullptr;
    bool m_haveMESACopySubBuffer = false;
    bool m_haveMESASwapControl = false;
    bool m_haveEXTSwapControl = false;
    bool m_haveSGISwapControl = false;
    bool m_useSwapEvents = false;
    bool m_forceSoftwareVsync = false;
    Display *m_x11Display;
    X11StandaloneBackend *m_backend;
    friend class GlxPixmapTexturePrivate;
    friend class GlxLayer;
};

class GlxPixmapTexture final : public GLTexture
{
public:
    explicit GlxPixmapTexture(GlxBackend *backend);

    bool create(SurfacePixmapX11 *texture);

private:
    Q_DECLARE_PRIVATE(GlxPixmapTexture)
};

class GlxPixmapTexturePrivate final : public GLTexturePrivate
{
public:
    GlxPixmapTexturePrivate(GlxPixmapTexture *texture, GlxBackend *backend);
    ~GlxPixmapTexturePrivate() override;

    bool create(SurfacePixmapX11 *texture);

protected:
    void onDamage() override;

private:
    GlxBackend *m_backend;
    GlxPixmapTexture *q;
    GLXPixmap m_glxPixmap;
};

class GlxSurfaceTextureX11 final : public OpenGLSurfaceTextureX11
{
public:
    GlxSurfaceTextureX11(GlxBackend *backend, SurfacePixmapX11 *pixmap);

    bool create() override;
    void update(const QRegion &region) override;
};

} // namespace
