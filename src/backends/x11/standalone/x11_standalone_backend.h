/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <config-kwin.h>

#include "core/outputbackend.h"
#include <kwin_export.h>

#include <QObject>

#include <memory>

#include <X11/Xlib-xcb.h>
#include <fixx11h.h>

namespace KWin
{
class RenderLoop;
class XInputIntegration;
class WindowSelector;
class X11EventFilter;
class X11Output;
class X11Keyboard;
class Edge;
class ScreenEdges;
class Outline;
class OutlineVisual;
class Compositor;
class WorkspaceScene;
class Window;
class ImageCursorSource;

class KWIN_EXPORT X11StandaloneBackend : public OutputBackend
{
    Q_OBJECT

public:
    X11StandaloneBackend(QObject *parent = nullptr);
    ~X11StandaloneBackend() override;
    bool initialize() override;

    std::unique_ptr<OpenGLBackend> createOpenGLBackend() override;
    QVector<CompositingType> supportedCompositors() const override;

    void initOutputs();
    void scheduleUpdateOutputs();
    void updateOutputs();

    std::unique_ptr<Edge> createScreenEdge(ScreenEdges *parent);
    void createPlatformCursor(QObject *parent = nullptr);
    void startInteractiveWindowSelection(std::function<void(KWin::Window *)> callback, const QByteArray &cursorName = QByteArray());
    void startInteractivePositionSelection(std::function<void(const QPoint &)> callback);
    PlatformCursorImage cursorImage() const;
    std::unique_ptr<OutlineVisual> createOutline(Outline *outline);
    void createEffectsHandler(Compositor *compositor, WorkspaceScene *scene);

    X11Keyboard *keyboard() const;
    RenderLoop *renderLoop() const;
    Outputs outputs() const override;

private:
    /**
     * Tests whether GLX is supported and returns @c true
     * in case KWin is compiled with OpenGL support and GLX
     * is available.
     *
     * If KWin is compiled with OpenGL ES or without OpenGL at
     * all, @c false is returned.
     * @returns @c true if GLX is available, @c false otherwise and if not build with OpenGL support.
     */
    static bool hasGlx();

    X11Output *findX11Output(const QString &name) const;
    template<typename T>
    void doUpdateOutputs();
    void updateRefreshRate();
    void updateCursor();
    // Feeds the X server's actual cursor image (via XFixes) into an ImageCursorSource
    // registered on Cursors::self()->mouse(), so KWin's own compositor-drawn cursor
    // (used on X11 only while the pointer is over a scaled output - see
    // X11Output::setCursor()/moveCursor()) has real pixels to show instead of nothing.
    // Without this, Cursors::self()->mouse()->source() is never populated on X11 - the
    // whole CursorImage/CursorSource pipeline in pointer_input.cpp only tracks Wayland
    // surface cursors and KWin's own effect/decoration/move-resize cursors, none of
    // which apply to a plain X11 client window.
    void updateCursorImage();
    // Registers, per CRTC, a cursor confinement box via the X-INPUT-SCALE X
    // extension. X-INPUT-SCALE keeps the pointer inside whichever sub-rectangle
    // of a CRTC's physical scanout KWin is actually treating as the logical/
    // active area for that output (Output::geometry(), the same box used to
    // clamp override-redirect popups - see events.cpp) when that's smaller than
    // the CRTC's full physical pixel size. Window geometry, hit-testing and
    // RandR queries are untouched - this only stops the pointer from wandering
    // into scanout pixels nothing is drawn into (the device-only dead zone that
    // motivated the cursor-hide-by-default change). No-op if the extension is
    // absent (KWin keeps working normally).
    void updateInputScale();

#if HAVE_X11_XINPUT
    std::unique_ptr<XInputIntegration> m_xinputIntegration;
#endif
    std::unique_ptr<QTimer> m_updateOutputsTimer;
    Display *m_x11Display;
    std::unique_ptr<WindowSelector> m_windowSelector;
    std::unique_ptr<X11EventFilter> m_screenEdgesFilter;
    std::unique_ptr<X11EventFilter> m_randrEventFilter;
    std::unique_ptr<X11Keyboard> m_keyboard;
    std::unique_ptr<RenderLoop> m_renderLoop;
    QVector<Output *> m_outputs;
    uint8_t m_inputScaleOpcode = 0; // 0 = unknown/absent; queried lazily
    bool m_inputScaleChecked = false;
    std::unique_ptr<ImageCursorSource> m_cursorSource;
    bool m_updatingCursorImage = false;
    bool m_nativeCursorHidden = false;
};

}
