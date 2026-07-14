/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2019 Roman Gilg <subdiff@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"
#include <kwin_export.h>

#include <QObject>
#include <QRect>

#include <memory>

#include <xcb/randr.h>

namespace KWin
{

class X11StandaloneBackend;

/**
 * X11 output representation
 */
class KWIN_EXPORT X11Output : public Output
{
    Q_OBJECT

public:
    explicit X11Output(X11StandaloneBackend *backend, QObject *parent = nullptr);
    ~X11Output() override;

    void updateEnabled(bool enabled);

    RenderLoop *renderLoop() const override;

    int xineramaNumber() const;
    void setXineramaNumber(int number);

    bool setGammaRamp(const std::shared_ptr<ColorTransformation> &transformation) override;

    // X11 has no per-output hardware cursor plane API, so these don't move any
    // cursor themselves; they just tell the compositor (see Compositor::addOutput())
    // whether the X server's own (fast, native) cursor is usable on THIS output.
    // It is, as long as this output isn't scaled - a scaled output's logical space
    // diverges from the physical space the X server's cursor is drawn in (see
    // docs/x11-per-output-scaling-extension.md), so there the compositor falls back
    // to compositing its own cursor, scaled and positioned correctly like any other
    // per-output content. X11StandaloneBackend::updateCursor() hides the native
    // cursor (globally - XFixes hide/show has no per-CRTC granularity) exactly while
    // the pointer is over such an output, and shows it again once the pointer moves
    // back to a native-scale one.
    bool setCursor(CursorSource *source) override;
    bool moveCursor(const QPoint &position) override;

    xcb_randr_crtc_t crtc() const
    {
        return m_crtc;
    }

private:
    void setCrtc(xcb_randr_crtc_t crtc);
    void setGammaRampSize(int size);

    X11StandaloneBackend *m_backend;
    std::unique_ptr<RenderLoop> m_loop;
    xcb_randr_crtc_t m_crtc = XCB_NONE;
    int m_gammaRampSize;
    int m_xineramaNumber = 0;

    friend class X11StandaloneBackend;
};

}
