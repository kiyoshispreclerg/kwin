/*
    SPDX-FileCopyrightText: 2010, 2012 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "x11_standalone_egl_backend.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "core/overlaywindow.h"
#include "core/renderloop.h"
#include "core/renderloop_p.h"
#include "kwinglplatform.h"
#include "options.h"
#include "scene/surfaceitem_x11.h"
#include "scene/workspacescene.h"
#include "softwarevsyncmonitor.h"
#include "utils/xcbutils.h"
#include "workspace.h"
#include "x11_standalone_backend.h"
#include "x11_standalone_logging.h"
#include "x11_standalone_overlaywindow.h"

#include <QOpenGLContext>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtPlatformHeaders/QEGLNativeContext>
#endif

namespace KWin
{

EglLayer::EglLayer(EglBackend *backend, Output *output)
    : m_backend(backend)
    , m_output(output)
{
    connect(output, &Output::geometryChanged, this, &EglLayer::updateSize);
}

EglLayer::~EglLayer()
{
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_backend->eglDisplay(), m_surface);
    }
    if (m_window != None) {
        xcb_destroy_window(connection(), m_window);
    }
}

Output *EglLayer::output() const
{
    return m_output;
}

EGLSurface EglLayer::surface() const
{
    return m_surface;
}

bool EglLayer::ensureResources()
{
    if (m_surface != EGL_NO_SURFACE && m_fbo) {
        return true;
    }

    xcb_connection_t *const c = connection();

    if (m_window == None) {
        const QRect geometry = m_output->geometry();
        const QSize size = m_output->pixelSize();

        // The EGLConfig was matched against the root window's own visual in
        // initBufferConfigs(), so a plain copy-from-parent child of the (root-screen)
        // overlay window already has a compatible visual - no explicit visual/colormap
        // lookup is needed here, unlike the GLX backend's GLXFBConfig-derived visual.
        m_window = xcb_generate_id(c);
        xcb_create_window(c, XCB_COPY_FROM_PARENT, m_window, m_backend->overlayWindow()->window(),
                          geometry.x(), geometry.y(), size.width(), size.height(), 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);

        m_backend->overlayWindow()->setup(m_window);
        // Map the child window explicitly. OverlayWindow::show() only maps the
        // overlay's subwindows once, so layers created after the first present
        // (e.g. a second output) would otherwise stay unmapped and show black.
        xcb_map_window(c, m_window);
    }

    if (m_surface == EGL_NO_SURFACE) {
        m_surface = m_backend->createLayerSurface(m_window);
        if (m_surface == EGL_NO_SURFACE) {
            return false;
        }
        // Without eglPostSubBufferNV, the compositor relies on the back buffer being
        // preserved across eglSwapBuffers() to draw partial updates onto it, exactly
        // like the single-surface path in EglOnXBackend::init() - each output's own
        // surface needs this set individually.
        m_backend->configureSurfaceSwapBehavior(m_surface);
    }

    // The framebuffer requires the context to be current on this surface.
    if (!m_backend->makeCurrentForLayer(this)) {
        return false;
    }

    m_fbo = std::make_unique<GLFramebuffer>(0, m_output->pixelSize());

    // There is no reliable way to determine when eglSwapBuffers()/eglPostSubBufferNV()
    // completes for a given surface, so fall back to a per-output software vblank timer
    // (mirrors the GLX backend's fallback path for the same reason).
    m_vsyncMonitor = SoftwareVsyncMonitor::create();
    RenderLoop *renderLoop = m_output->renderLoop();
    m_vsyncMonitor->setRefreshRate(renderLoop->refreshRate());
    connect(renderLoop, &RenderLoop::refreshRateChanged, this, [this]() {
        m_vsyncMonitor->setRefreshRate(m_output->renderLoop()->refreshRate());
    });
    connect(m_vsyncMonitor.get(), &VsyncMonitor::vblankOccurred, this, &EglLayer::vblank);

    return true;
}

void EglLayer::updateSize()
{
    if (m_window == None) {
        return;
    }
    const QRect geometry = m_output->geometry();
    const QSize size = m_output->pixelSize();
    m_backend->makeCurrentForLayer(this);
    const uint32_t values[] = {
        uint32_t(geometry.x()), uint32_t(geometry.y()), uint32_t(size.width()), uint32_t(size.height())};
    xcb_configure_window(connection(), m_window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    Xcb::sync();
    m_bufferAge = 0;
    m_fbo = std::make_unique<GLFramebuffer>(0, size);
}

std::optional<OutputLayerBeginFrameInfo> EglLayer::beginFrame()
{
    if (!ensureResources()) {
        return std::nullopt;
    }

    m_backend->makeCurrentForLayer(this);

    QRegion repaint;
    if (m_backend->supportsBufferAge()) {
        repaint = m_damageJournal.accumulate(m_bufferAge, infiniteRegion());
    }

    eglWaitNative(EGL_CORE_NATIVE_ENGINE);

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_fbo.get()),
        .repaint = repaint,
    };
}

bool EglLayer::endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion)
{
    if (m_backend->supportsBufferAge()) {
        m_damageJournal.add(damagedRegion);
    }
    m_lastRenderedRegion = renderedRegion;
    return true;
}

void EglLayer::present()
{
    if (m_surface == EGL_NO_SURFACE) {
        return;
    }

    m_backend->makeCurrentForLayer(this);

    // There is no reliable way to determine when eglSwapBuffers()/eglPostSubBufferNV()
    // completes, so assume the frame will be presented at the next vblank.
    m_vsyncMonitor->arm();

    const QRect displayRect(QPoint(0, 0), m_output->pixelSize());
    const QRegion displayRegion(displayRect);

    QRegion effectiveRenderedRegion = m_lastRenderedRegion;
    if (!GLPlatform::instance()->isGLES()) {
        if (!m_backend->supportsBufferAge() && options->glPreferBufferSwap() == Options::CopyFrontBuffer && m_lastRenderedRegion != displayRegion) {
            glReadBuffer(GL_FRONT);
            m_backend->copyPixels(displayRegion - m_lastRenderedRegion, displayRect.size());
            glReadBuffer(GL_BACK);
            effectiveRenderedRegion = displayRegion;
        }
    }

    const bool fullRepaint = m_backend->supportsBufferAge() || !m_backend->havePostSubBuffer() || (effectiveRenderedRegion == displayRegion);
    if (fullRepaint) {
        eglSwapBuffers(m_backend->eglDisplay(), m_surface);
        if (m_backend->supportsBufferAge()) {
            eglQuerySurface(m_backend->eglDisplay(), m_surface, EGL_BUFFER_AGE_EXT, &m_bufferAge);
        }
    } else {
        for (const QRect &r : effectiveRenderedRegion) {
            eglPostSubBufferNV(m_backend->eglDisplay(), m_surface, r.left(), displayRect.height() - r.bottom() - 1, r.width(), r.height());
        }
    }

    if (m_backend->overlayWindow()->window()) { // show the window only after the first pass,
        m_backend->overlayWindow()->show(); // since that pass may take long
    }
}

void EglLayer::vblank(std::chrono::nanoseconds timestamp)
{
    RenderLoopPrivate::get(m_output->renderLoop())->notifyFrameCompleted(timestamp);
}

EglBackend::EglBackend(Display *display, X11StandaloneBackend *backend)
    : EglOnXBackend(kwinApp()->x11Connection(), display, kwinApp()->x11RootWindow())
    , m_backend(backend)
    , m_overlayWindow(std::make_unique<OverlayWindowX11>())
{
    connect(workspace(), &Workspace::geometryChanged, this, &EglBackend::screenGeometryChanged);
}

EglBackend::~EglBackend()
{
    // No completion events will be received for in-flight frames, this may lock the
    // render loops. We need to ensure that they are back to their initial state if
    // the render backend is about to be destroyed.
    for (const auto &[output, layer] : m_layers) {
        RenderLoopPrivate::get(output->renderLoop())->invalidate();
    }
    // Destroy the per-output layers (and their X/EGL resources) before the context.
    m_currentLayer = nullptr;
    m_layers.clear();

    if (isFailed() && m_overlayWindow) {
        m_overlayWindow->destroy();
    }
    // Make sure the context is current on a surface that still exists (the per-output
    // surfaces were just destroyed) before cleaning up shared GL resources.
    if (m_bootstrapSurface != EGL_NO_SURFACE) {
        makeContextCurrent(m_bootstrapSurface);
    }
    cleanup();

    if (m_bootstrapWindow != None) {
        xcb_destroy_window(connection(), m_bootstrapWindow);
    }

    if (m_overlayWindow && m_overlayWindow->window()) {
        m_overlayWindow->destroy();
    }
}

std::unique_ptr<SurfaceTexture> EglBackend::createSurfaceTextureX11(SurfacePixmapX11 *texture)
{
    return std::make_unique<EglSurfaceTextureX11>(this, texture);
}

void EglBackend::init()
{
    QOpenGLContext *qtShareContext = QOpenGLContext::globalShareContext();
    EGLDisplay shareDisplay = EGL_NO_DISPLAY;
    EGLContext shareContext = EGL_NO_CONTEXT;
    if (qtShareContext) {
        qDebug(KWIN_X11STANDALONE) << "Global share context format:" << qtShareContext->format();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        const QVariant nativeHandle = qtShareContext->nativeHandle();
        if (!nativeHandle.canConvert<QEGLNativeContext>()) {
            setFailed(QStringLiteral("Invalid QOpenGLContext::globalShareContext()"));
            return;
        } else {
            QEGLNativeContext handle = qvariant_cast<QEGLNativeContext>(nativeHandle);
            shareContext = handle.context();
            shareDisplay = handle.display();
        }
#else
        const auto nativeHandle = qtShareContext->nativeInterface<QNativeInterface::QEGLContext>();
        if (nativeHandle) {
            shareContext = nativeHandle->nativeContext();
            shareDisplay = nativeHandle->display();
        } else {
            setFailed(QStringLiteral("Invalid QOpenGLContext::globalShareContext()"));
            return;
        }
#endif
    }
    if (shareContext == EGL_NO_CONTEXT) {
        setFailed(QStringLiteral("QOpenGLContext::globalShareContext() is required"));
        return;
    }

    kwinApp()->outputBackend()->setSceneEglDisplay(shareDisplay);
    kwinApp()->outputBackend()->setSceneEglGlobalShareContext(shareContext);
    EglOnXBackend::init();

    // Per-output layers are created lazily; make sure they are torn down when an
    // output goes away.
    connect(m_backend, &OutputBackend::outputRemoved, this, &EglBackend::removeLayer);
}

bool EglBackend::createSurfaces()
{
    if (!m_overlayWindow) {
        return false;
    }

    if (!m_overlayWindow->create()) {
        qCCritical(KWIN_X11STANDALONE) << "Could not get overlay window";
        return false;
    }
    // Shape the overlay window to cover the whole X screen. The per-output child
    // windows that are actually rendered into are created later, on demand, in
    // EglLayer::ensureResources().
    m_overlayWindow->setup(XCB_WINDOW_NONE);

    // A small, never-mapped child window of the overlay. It only exists to give the
    // shared EGL context a stable surface for context/config setup and for
    // makeCurrent() calls that are not tied to a specific output.
    xcb_connection_t *const c = connection();
    m_bootstrapWindow = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, m_bootstrapWindow, m_overlayWindow->window(),
                      0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);

    m_bootstrapSurface = createSurface(m_bootstrapWindow);
    if (m_bootstrapSurface == EGL_NO_SURFACE) {
        return false;
    }
    setSurface(m_bootstrapSurface);
    return true;
}

void EglBackend::screenGeometryChanged()
{
    // The overlay window covers the whole X screen; keep it in sync with the union
    // of all outputs. The per-output child windows track their own outputs' geometry
    // independently (see EglLayer::updateSize()).
    overlayWindow()->resize(workspace()->geometry().size());
    Xcb::sync();
}

void EglBackend::present(Output *output)
{
    auto it = m_layers.find(output);
    if (it != m_layers.end()) {
        it->second->present();
    }
}

bool EglBackend::makeCurrent()
{
    EGLSurface surface = m_bootstrapSurface;
    if (m_currentLayer && m_currentLayer->surface() != EGL_NO_SURFACE) {
        surface = m_currentLayer->surface();
    }
    return makeContextCurrent(surface);
}

bool EglBackend::makeCurrentForLayer(EglLayer *layer)
{
    m_currentLayer = layer;
    return makeCurrent();
}

OverlayWindow *EglBackend::overlayWindow() const
{
    return m_overlayWindow.get();
}

OutputLayer *EglBackend::primaryLayer(Output *output)
{
    std::unique_ptr<EglLayer> &layer = m_layers[output];
    if (!layer) {
        layer = std::make_unique<EglLayer>(this, output);
    }
    // Remember which output is about to be composited so that makeCurrent() targets
    // the right surface during this frame.
    m_currentLayer = layer.get();
    return layer.get();
}

void EglBackend::removeLayer(Output *output)
{
    auto it = m_layers.find(output);
    if (it == m_layers.end()) {
        return;
    }
    if (m_currentLayer == it->second.get()) {
        m_currentLayer = nullptr;
    }
    // Make sure the context is not current on a surface that is about to be destroyed.
    makeContextCurrent(m_bootstrapSurface);
    m_layers.erase(it);
}

EglSurfaceTextureX11::EglSurfaceTextureX11(EglBackend *backend, SurfacePixmapX11 *texture)
    : OpenGLSurfaceTextureX11(backend, texture)
{
}

bool EglSurfaceTextureX11::create()
{
    auto texture = std::make_unique<EglPixmapTexture>(static_cast<EglBackend *>(m_backend));
    if (texture->create(m_pixmap)) {
        m_texture = std::move(texture);
        return true;
    } else {
        return false;
    }
}

void EglSurfaceTextureX11::update(const QRegion &region)
{
    // mipmaps need to be updated
    m_texture->setDirty();
}

EglPixmapTexture::EglPixmapTexture(EglBackend *backend)
    : GLTexture(*new EglPixmapTexturePrivate(this, backend))
{
}

bool EglPixmapTexture::create(SurfacePixmapX11 *texture)
{
    Q_D(EglPixmapTexture);
    return d->create(texture);
}

EglPixmapTexturePrivate::EglPixmapTexturePrivate(EglPixmapTexture *texture, EglBackend *backend)
    : q(texture)
    , m_backend(backend)
{
    m_target = GL_TEXTURE_2D;
}

EglPixmapTexturePrivate::~EglPixmapTexturePrivate()
{
    if (m_image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(m_backend->eglDisplay(), m_image);
    }
}

bool EglPixmapTexturePrivate::create(SurfacePixmapX11 *pixmap)
{
    const xcb_pixmap_t nativePixmap = pixmap->pixmap();
    if (nativePixmap == XCB_NONE) {
        return false;
    }

    glGenTextures(1, &m_texture);
    q->setWrapMode(GL_CLAMP_TO_EDGE);
    q->setFilter(GL_LINEAR);
    q->bind();
    const EGLint attribs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE};
    m_image = eglCreateImageKHR(m_backend->eglDisplay(),
                                EGL_NO_CONTEXT,
                                EGL_NATIVE_PIXMAP_KHR,
                                reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(nativePixmap)),
                                attribs);

    if (EGL_NO_IMAGE_KHR == m_image) {
        qCDebug(KWIN_X11STANDALONE) << "failed to create egl image";
        q->unbind();
        return false;
    }
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(m_image));
    q->unbind();
    q->setYInverted(true);
    m_size = pixmap->size();
    updateMatrix();
    return true;
}

void EglPixmapTexturePrivate::onDamage()
{
    if (options->isGlStrictBinding()) {
        // This is just implemented to be consistent with
        // the example in mesa/demos/src/egl/opengles1/texture_from_pixmap.c
        eglWaitNative(EGL_CORE_NATIVE_ENGINE);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(m_image));
    }
    GLTexturePrivate::onDamage();
}

} // namespace KWin
