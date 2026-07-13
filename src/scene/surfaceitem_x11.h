/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "scene/surfaceitem.h"

#include <xcb/damage.h>
#include <xcb/xfixes.h>

namespace KWin
{

class Deleted;

/**
 * The SurfaceItemX11 class represents an X11 surface in the scene.
 */
class KWIN_EXPORT SurfaceItemX11 : public SurfaceItem
{
    Q_OBJECT

public:
    explicit SurfaceItemX11(Window *window, Scene *scene, Item *parent = nullptr);
    ~SurfaceItemX11() override;

    Window *window() const;

    void preprocess() override;

    void processDamage();
    bool fetchDamage();
    void waitForDamage();
    void destroyDamage();

    QVector<QRectF> shape() const override;
    QRegion opaque() const override;

    // True while the window (an X11Window) has a valid _X_DENSITY_PIXMAP published -
    // an auxiliary Pixmap, owned and sized by the client itself, that stands in for
    // the window's own contents when density-negotiating. The window's own X11
    // geometry never changes for this - window management/decoration are entirely
    // unaware density negotiation exists. Public so SurfacePixmapX11::create() (a
    // sibling class in this file) can query it to decide what to capture.
    bool hasAuxiliaryPixmap() const;

private Q_SLOTS:
    void handleBufferGeometryChanged(Window *window, const QRectF &old);
    void handleGeometryShapeChanged();
    void handleWindowClosed(Window *original, Deleted *deleted);

protected:
    std::unique_ptr<SurfacePixmap> createPixmap() override;

private:
    // The density the client rendered its content at (X11Window::densityScale(), 1.0
    // for windows that aren't an X11Window - e.g. unmanaged/override-redirect - or
    // that never negotiated a density). Only meaningful - and only applied to
    // surfaceToBufferMatrix - while hasAuxiliaryPixmap(): the item's own size() is
    // always just the window's normal (buffer) size, since the window itself never
    // resizes for density; surfaceToBufferMatrix maps that onto the denser auxiliary
    // pixmap for UV sampling in SurfaceItem::buildQuads(), the same role
    // SurfaceItemWayland's buffer_scale matrix plays. Without an auxiliary pixmap the
    // matrix is identity and everything behaves exactly as it did before density
    // negotiation existed.
    qreal densityScale() const;
    void updateDensityGeometry();

    Window *m_window;
    xcb_damage_damage_t m_damageHandle = XCB_NONE;
    xcb_xfixes_fetch_region_cookie_t m_damageCookie;
    bool m_isDamaged = false;
    bool m_havePendingDamageRegion = false;
};

class KWIN_EXPORT SurfacePixmapX11 final : public SurfacePixmap
{
    Q_OBJECT

public:
    explicit SurfacePixmapX11(SurfaceItemX11 *item, QObject *parent = nullptr);
    ~SurfacePixmapX11() override;

    xcb_pixmap_t pixmap() const;
    xcb_visualid_t visual() const;

    void create() override;
    bool isValid() const override;

private:
    SurfaceItemX11 *m_item;
    xcb_pixmap_t m_pixmap = XCB_PIXMAP_NONE;
    // False when m_pixmap is the client's own auxiliary pixmap (see
    // hasAuxiliaryPixmap()) - the client owns and frees that one, not us.
    bool m_ownsPixmap = true;
};

} // namespace KWaylandServer
