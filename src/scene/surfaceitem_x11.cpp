/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/surfaceitem_x11.h"
#include "composite.h"
#include "core/renderbackend.h"
#include "deleted.h"
#include "unmanaged.h"
#include "x11syncmanager.h"
#include "x11window.h"

namespace KWin
{

// Density negotiation (X-DENSITY.md) is read from both managed X11 windows and
// override-redirect ones (Unmanaged - popups/menus/tooltips): a density-aware
// toolkit publishes _X_DENSITY_SCALE/_X_DENSITY_PIXMAP on either the same way, so a
// popup can be just as sharp as its parent window on a scaled output. The two
// classes don't share a common base with these accessors (X11Window also has the
// write side - setDensityRequestScale() - which nothing currently drives for
// popups), so these two helpers are the single place that knows to check both.
static xcb_pixmap_t windowDensityPixmap(const Window *window)
{
    if (auto *x11Window = qobject_cast<const X11Window *>(window)) {
        return x11Window->densityPixmap();
    }
    if (auto *unmanaged = qobject_cast<const Unmanaged *>(window)) {
        return unmanaged->densityPixmap();
    }
    return XCB_PIXMAP_NONE;
}

static qreal windowDensityScale(const Window *window)
{
    if (auto *x11Window = qobject_cast<const X11Window *>(window)) {
        return x11Window->densityScale();
    }
    if (auto *unmanaged = qobject_cast<const Unmanaged *>(window)) {
        return unmanaged->densityScale();
    }
    return 1.0;
}

SurfaceItemX11::SurfaceItemX11(Window *window, Scene *scene, Item *parent)
    : SurfaceItem(scene, parent)
    , m_window(window)
{
    connect(window, &Window::bufferGeometryChanged,
            this, &SurfaceItemX11::handleBufferGeometryChanged);
    connect(window, &Window::geometryShapeChanged,
            this, &SurfaceItemX11::handleGeometryShapeChanged);
    connect(window, &Window::windowClosed,
            this, &SurfaceItemX11::handleWindowClosed);

    m_damageHandle = xcb_generate_id(kwinApp()->x11Connection());
    xcb_damage_create(kwinApp()->x11Connection(), m_damageHandle, window->frameId(),
                      XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);

    // With unmanaged windows there is a race condition between the client painting the window
    // and us setting up damage tracking.  If the client wins we won't get a damage event even
    // though the window has been painted.  To avoid this we mark the whole window as damaged
    // immediately after creating the damage object.
    if (window->isUnmanaged()) {
        m_isDamaged = true;
    }

    if (X11Window *x11Window = qobject_cast<X11Window *>(window)) {
        connect(x11Window, &X11Window::densityScaleChanged,
                this, &SurfaceItemX11::updateDensityGeometry);
        connect(x11Window, &X11Window::densityPixmapChanged,
                this, &SurfaceItemX11::updateDensityGeometry);
    } else if (Unmanaged *unmanaged = qobject_cast<Unmanaged *>(window)) {
        connect(unmanaged, &Unmanaged::densityScaleChanged,
                this, &SurfaceItemX11::updateDensityGeometry);
        connect(unmanaged, &Unmanaged::densityPixmapChanged,
                this, &SurfaceItemX11::updateDensityGeometry);
    }

    updateDensityGeometry();
}

SurfaceItemX11::~SurfaceItemX11()
{
    // destroyDamage() will be called by the associated Window.
}

Window *SurfaceItemX11::window() const
{
    return m_window;
}

void SurfaceItemX11::handleWindowClosed(Window *original, Deleted *deleted)
{
    m_window = deleted;
}

void SurfaceItemX11::preprocess()
{
    if (!damage().isEmpty()) {
        X11Compositor *compositor = X11Compositor::self();
        if (X11SyncManager *syncManager = compositor->syncManager()) {
            syncManager->insertWait();
        }
    }
    SurfaceItem::preprocess();
}

void SurfaceItemX11::processDamage()
{
    m_isDamaged = true;
    scheduleFrame();
}

bool SurfaceItemX11::fetchDamage()
{
    if (!m_isDamaged) {
        return false;
    }

    if (m_damageHandle == XCB_NONE) {
        return true;
    }

    xcb_xfixes_region_t region = xcb_generate_id(kwinApp()->x11Connection());
    xcb_xfixes_create_region(kwinApp()->x11Connection(), region, 0, nullptr);
    xcb_damage_subtract(kwinApp()->x11Connection(), m_damageHandle, 0, region);

    m_damageCookie = xcb_xfixes_fetch_region_unchecked(kwinApp()->x11Connection(), region);
    xcb_xfixes_destroy_region(kwinApp()->x11Connection(), region);

    m_havePendingDamageRegion = true;

    return true;
}

void SurfaceItemX11::waitForDamage()
{
    if (!m_havePendingDamageRegion) {
        return;
    }
    m_havePendingDamageRegion = false;

    xcb_xfixes_fetch_region_reply_t *reply =
        xcb_xfixes_fetch_region_reply(kwinApp()->x11Connection(), m_damageCookie, nullptr);
    if (!reply) {
        qCDebug(KWIN_CORE) << "Failed to check damage region";
        return;
    }

    const int rectCount = xcb_xfixes_fetch_region_rectangles_length(reply);
    QRegion region;

    if (rectCount > 1 && rectCount < 16) {
        xcb_rectangle_t *rects = xcb_xfixes_fetch_region_rectangles(reply);

        QVector<QRect> qtRects;
        qtRects.reserve(rectCount);

        for (int i = 0; i < rectCount; ++i) {
            qtRects << QRect(rects[i].x, rects[i].y, rects[i].width, rects[i].height);
        }
        region.setRects(qtRects.constData(), rectCount);
    } else {
        region = QRect(reply->extents.x, reply->extents.y, reply->extents.width, reply->extents.height);
    }
    free(reply);

    addDamage(region);
    m_isDamaged = false;
}

void SurfaceItemX11::destroyDamage()
{
    if (m_damageHandle != XCB_NONE) {
        m_isDamaged = false;
        xcb_damage_destroy(kwinApp()->x11Connection(), m_damageHandle);
        m_damageHandle = XCB_NONE;
    }
    if (m_auxDamageHandle != XCB_NONE) {
        xcb_damage_destroy(kwinApp()->x11Connection(), m_auxDamageHandle);
        m_auxDamageHandle = XCB_NONE;
        m_auxDamagePixmap = XCB_PIXMAP_NONE;
    }
}

void SurfaceItemX11::syncAuxiliaryDamage()
{
    const xcb_pixmap_t auxPixmap = windowDensityPixmap(m_window);

    if (auxPixmap == m_auxDamagePixmap) {
        return; // already watching the right thing (possibly nothing)
    }

    if (m_auxDamageHandle != XCB_NONE) {
        xcb_damage_destroy(kwinApp()->x11Connection(), m_auxDamageHandle);
        m_auxDamageHandle = XCB_NONE;
    }

    m_auxDamagePixmap = auxPixmap;

    if (auxPixmap != XCB_PIXMAP_NONE) {
        m_auxDamageHandle = xcb_generate_id(kwinApp()->x11Connection());
        xcb_damage_create(kwinApp()->x11Connection(), m_auxDamageHandle, auxPixmap,
                          XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
    }
}

void SurfaceItemX11::processAuxiliaryDamage()
{
    // The auxiliary pixmap isn't a window, so it doesn't share the frame's damage
    // accounting/pipelining (fetchDamage()/waitForDamage() above assume the frame);
    // subtract synchronously right here instead - this only happens while density
    // negotiation is actually active (not on every regular window repaint), so the
    // extra round-trip is an acceptable tradeoff for reusing far less code. parts=None
    // discards the precise sub-region (we don't need it: unlike the frame, we have no
    // cheaper "is this actually visible/opaque" tracking to feed it into), just
    // acknowledges the damage so the server keeps reporting further changes.
    if (m_auxDamageHandle == XCB_NONE) {
        return;
    }
    xcb_damage_subtract(kwinApp()->x11Connection(), m_auxDamageHandle, XCB_NONE, XCB_NONE);

    discardPixmap();
    addDamage(boundingRect().toAlignedRect());
    scheduleRepaint(boundingRect());
}

void SurfaceItemX11::handleBufferGeometryChanged(Window *window, const QRectF &old)
{
    if (window->bufferGeometry().size() != old.size()) {
        discardPixmap();
    }
    updateDensityGeometry();
}

qreal SurfaceItemX11::densityScale() const
{
    return windowDensityScale(m_window);
}

bool SurfaceItemX11::hasAuxiliaryPixmap() const
{
    return windowDensityPixmap(m_window) != XCB_PIXMAP_NONE;
}

void SurfaceItemX11::updateDensityGeometry()
{
    // The window's own X11 geometry never changes for density negotiation - it stays
    // exactly what window management/decoration would give a normal, non-density-aware
    // window - so the item's own size is always just that, no dividing/adjusting.
    setSize(m_window->bufferGeometry().size());

    // Only while an auxiliary pixmap is actually published (see hasAuxiliaryPixmap())
    // does surfaceToBufferMatrix need to do anything: it maps this item's normal
    // (logical) size onto that denser buffer for UV sampling in
    // SurfaceItem::buildQuads(), the same role SurfaceItemWayland's buffer_scale
    // matrix plays. Without one, stay identity - the frame is captured 1:1 like any
    // ordinary window, exactly as before density negotiation existed.
    QMatrix4x4 matrix;
    if (hasAuxiliaryPixmap()) {
        const qreal density = densityScale();
        matrix.scale(density, density);
        // shape()'s clipped rects - the quad vertex positions buildQuads() feeds
        // through this matrix - are in FRAME-relative logical coordinates (they come
        // from clientGeometry().translated(-bufferGeometry().topLeft())). The
        // auxiliary pixmap, however, only contains the CLIENT's own content (nothing
        // decoration-related), so its pixel (0,0) is the CLIENT's own origin - offset
        // from the frame's by the decoration border. Subtract that offset before
        // scaling, or sampling starts short of the buffer's real origin (missing a
        // border-sized margin at the top/left) and overshoots the same amount past
        // its far edge (bottom/right). (Matrix calls compose so the LAST one here
        // runs FIRST on a point: translate then scale, i.e. density * (v - clientOffset).)
        const QPointF clientOffset = m_window->clientGeometry().topLeft() - m_window->bufferGeometry().topLeft();
        matrix.translate(-clientOffset.x(), -clientOffset.y());
    }
    setSurfaceToBufferMatrix(matrix);

    // hasAuxiliaryPixmap() flipping changes what SurfacePixmapX11::create() captures
    // (the auxiliary pixmap vs. the window's own frame - see there); discard
    // unconditionally rather than relying on a paired geometry change to have done it
    // already (a client can publish/withdraw _X_DENSITY_PIXMAP without ever touching
    // its own window geometry, since that's the whole point of this scheme).
    discardPixmap();
    discardQuads();

    // (Re)point our auxiliary XDamage object (if any) at whatever pixmap is now
    // current - see syncAuxiliaryDamage()/processAuxiliaryDamage(). Without this, a
    // brand new auxiliary pixmap would never get its own damage tracked at all, and
    // discardPixmap() above only takes effect on the *next* scheduled repaint - so
    // explicitly schedule one now, or a client publishing/updating _X_DENSITY_PIXMAP
    // with nothing else prompting a repaint would just sit there never redrawn.
    syncAuxiliaryDamage();
    scheduleRepaint(boundingRect());
}

void SurfaceItemX11::handleGeometryShapeChanged()
{
    scheduleRepaint(boundingRect());
    discardQuads();
}

QVector<QRectF> SurfaceItemX11::shape() const
{
    const QRectF clipRect = m_window->clientGeometry().translated(-m_window->bufferGeometry().topLeft());
    QVector<QRectF> shape = m_window->shapeRegion();
    // bounded to clipRect
    for (QRectF &shapePart : shape) {
        shapePart = shapePart.intersected(clipRect);
    }
    return shape;
}

QRegion SurfaceItemX11::opaque() const
{
    QRegion shapeRegion;
    for (const QRectF &shapePart : shape()) {
        shapeRegion |= shapePart.toRect();
    }
    if (!m_window->hasAlpha()) {
        return shapeRegion;
    } else {
        return m_window->opaqueRegion() & shapeRegion;
    }
    return QRegion();
}

std::unique_ptr<SurfacePixmap> SurfaceItemX11::createPixmap()
{
    return std::make_unique<SurfacePixmapX11>(this);
}

SurfacePixmapX11::SurfacePixmapX11(SurfaceItemX11 *item, QObject *parent)
    : SurfacePixmap(Compositor::self()->backend()->createSurfaceTextureX11(this), parent)
    , m_item(item)
{
}

SurfacePixmapX11::~SurfacePixmapX11()
{
    // Never free the client's own auxiliary pixmap (see SurfaceItemX11::
    // hasAuxiliaryPixmap()) - we don't own it, the client does.
    if (m_pixmap != XCB_PIXMAP_NONE && m_ownsPixmap) {
        xcb_free_pixmap(kwinApp()->x11Connection(), m_pixmap);
    }
}

bool SurfacePixmapX11::isValid() const
{
    return m_pixmap != XCB_PIXMAP_NONE;
}

xcb_pixmap_t SurfacePixmapX11::pixmap() const
{
    return m_pixmap;
}

xcb_visualid_t SurfacePixmapX11::visual() const
{
    return m_item->window()->visual();
}

void SurfacePixmapX11::create()
{
    const Window *window = m_item->window();
    if (window->isDeleted()) {
        return;
    }

    xcb_connection_t *connection = kwinApp()->x11Connection();

    // Density negotiation, auxiliary-pixmap scheme: if the client published a valid
    // _X_DENSITY_PIXMAP, use it directly instead of capturing the window - it is
    // already exactly the content we want, at whatever (denser) size the client drew
    // it at, and needs no Composite/NameWindowPixmap dance since the client created
    // and owns the pixmap itself (see SurfaceItemX11::hasAuxiliaryPixmap() and
    // windowDensityPixmap() - checked for both managed windows and override-redirect
    // popups/menus/tooltips). We only borrow the XID; the client frees it, not us
    // (see the destructor, m_ownsPixmap).
    if (const xcb_pixmap_t auxPixmap = windowDensityPixmap(window); auxPixmap != XCB_PIXMAP_NONE) {
        Xcb::WindowGeometry auxGeometry(auxPixmap);
        if (!auxGeometry || auxGeometry.size().isEmpty()) {
            qCDebug(KWIN_CORE, "Failed to use _X_DENSITY_PIXMAP 0x%x for window 0x%x: invalid or empty",
                    auxPixmap, window->window());
        } else {
            m_pixmap = auxPixmap;
            m_ownsPixmap = false;
            m_hasAlphaChannel = window->hasAlpha();
            m_size = auxGeometry.size().toSize();
            return;
        }
    }

    XServerGrabber grabber;
    xcb_window_t frame = window->frameId();
    xcb_pixmap_t pixmap = xcb_generate_id(connection);
    xcb_void_cookie_t namePixmapCookie = xcb_composite_name_window_pixmap_checked(connection,
                                                                                  frame,
                                                                                  pixmap);
    Xcb::WindowAttributes windowAttributes(frame);
    Xcb::WindowGeometry windowGeometry(frame);
    if (xcb_generic_error_t *error = xcb_request_check(connection, namePixmapCookie)) {
        qCDebug(KWIN_CORE, "Failed to create window pixmap for window 0x%x (error code %d)",
                window->window(), error->error_code);
        free(error);
        return;
    }
    // check that the received pixmap is valid and actually matches what we
    // know about the window (i.e. size)
    if (!windowAttributes || windowAttributes->map_state != XCB_MAP_STATE_VIEWABLE) {
        qCDebug(KWIN_CORE, "Failed to create window pixmap for window 0x%x (not viewable)",
                window->window());
        xcb_free_pixmap(connection, pixmap);
        return;
    }
    const QRectF bufferGeometry = window->bufferGeometry();
    if (windowGeometry.size() != bufferGeometry.size()) {
        qCDebug(KWIN_CORE, "Failed to create window pixmap for window 0x%x: window size (%fx%f) != buffer size (%fx%f)", window->window(),
                windowGeometry.size().width(), windowGeometry.size().height(), bufferGeometry.width(), bufferGeometry.height());
        xcb_free_pixmap(connection, pixmap);
        return;
    }

    m_pixmap = pixmap;
    m_hasAlphaChannel = window->hasAlpha();
    // this class is only used on X11 where the logical size and
    // device pixel size is guaranteed to be the same and we can convert safely
    m_size = bufferGeometry.size().toSize();
}

} // namespace KWin
