/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "x11_standalone_backend.h"

#include "atoms.h"
#include "composite.h"
#include "core/session.h"
#include "cursor.h"
#include "cursorsource.h"
#include "x11_standalone_cursor.h"
#include "x11_standalone_edge.h"
#include "x11_standalone_placeholderoutput.h"
#include "x11_standalone_windowselector.h"
#include <kwinconfig.h>
#if HAVE_EPOXY_GLX
#include "x11_standalone_glx_backend.h"
#endif
#if HAVE_X11_XINPUT
#include "x11_standalone_xinputintegration.h"
#endif
#include "core/renderloop.h"
#include "keyboard_input.h"
#include "options.h"
#include "utils/c_ptr.h"
#include "utils/edid.h"
#include "utils/xcbutils.h"
#include "window.h"
#include "workspace.h"
#include "x11_standalone_effects.h"
#include "x11_standalone_egl_backend.h"
#include "x11_standalone_keyboard.h"
#include "x11_standalone_logging.h"
#include "x11_standalone_non_composited_outline.h"
#include "x11_standalone_output.h"
#include "x11_standalone_screenedges_filter.h"
#include "xkb.h"

#include "../common/kwinxrenderutils.h"

#include <KConfigGroup>
#include <KLocalizedString>

#include <QOpenGLContext>
#include <QThread>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <private/qtx11extras_p.h>
#else
#include <QX11Info>
#endif

#include <span>

#include <xcb/xcbext.h>

namespace KWin
{

class XrandrEventFilter : public X11EventFilter
{
public:
    explicit XrandrEventFilter(X11StandaloneBackend *backend);

    bool event(xcb_generic_event_t *event) override;

private:
    X11StandaloneBackend *m_backend;
};

XrandrEventFilter::XrandrEventFilter(X11StandaloneBackend *backend)
    : X11EventFilter(QVector<int>{
          Xcb::Extensions::self()->randrNotifyEvent(),
          Xcb::Extensions::self()->randrOutputNotifyEvent(),
      })
    , m_backend(backend)
{
}

bool XrandrEventFilter::event(xcb_generic_event_t *event)
{
    // let's try to gather a few XRandR events, unlikely that there is just one
    m_backend->scheduleUpdateOutputs();

    // Only ScreenChangeNotify carries the screen dimensions; RRNotify (e.g. an
    // output-property/DPI change) just triggers a reconfigure above.
    if ((event->response_type & ~0x80) != Xcb::Extensions::self()->randrNotifyEvent()) {
        return false;
    }

    // update default screen
    auto *xrrEvent = reinterpret_cast<xcb_randr_screen_change_notify_event_t *>(event);
    xcb_screen_t *screen = Xcb::defaultScreen();
    if (xrrEvent->rotation & (XCB_RANDR_ROTATION_ROTATE_90 | XCB_RANDR_ROTATION_ROTATE_270)) {
        screen->width_in_pixels = xrrEvent->height;
        screen->height_in_pixels = xrrEvent->width;
        screen->width_in_millimeters = xrrEvent->mheight;
        screen->height_in_millimeters = xrrEvent->mwidth;
    } else {
        screen->width_in_pixels = xrrEvent->width;
        screen->height_in_pixels = xrrEvent->height;
        screen->width_in_millimeters = xrrEvent->mwidth;
        screen->height_in_millimeters = xrrEvent->mheight;
    }

    return false;
}

X11StandaloneBackend::X11StandaloneBackend(QObject *parent)
    : OutputBackend(parent)
    , m_updateOutputsTimer(std::make_unique<QTimer>())
    , m_x11Display(QX11Info::display())
    , m_renderLoop(std::make_unique<RenderLoop>())
{
#if HAVE_X11_XINPUT
    if (!qEnvironmentVariableIsSet("KWIN_NO_XI2")) {
        m_xinputIntegration = std::make_unique<XInputIntegration>(m_x11Display, this);
        m_xinputIntegration->init();
        if (!m_xinputIntegration->hasXinput()) {
            m_xinputIntegration.reset();
        } else {
            connect(kwinApp(), &Application::workspaceCreated, m_xinputIntegration.get(), &XInputIntegration::startListening);
        }
    }
#endif

    m_updateOutputsTimer->setSingleShot(true);
    connect(m_updateOutputsTimer.get(), &QTimer::timeout, this, &X11StandaloneBackend::updateOutputs);

    // Per-output scaling only applies while compositing (see doUpdateOutputs), so
    // re-query the outputs whenever compositing is toggled. The Compositor exists by
    // the time the workspace is created.
    connect(kwinApp(), &Application::workspaceCreated, this, [this]() {
        if (Compositor *compositor = Compositor::self()) {
            connect(compositor, &Compositor::compositingToggled, this, &X11StandaloneBackend::scheduleUpdateOutputs);
        }
    });

    // Feed the X server's real cursor image (see updateCursorImage()) into the mouse
    // Cursor's source, so the compositor has real pixels to draw while it is showing
    // its own cursor on a scaled output (see X11Output::setCursor()/moveCursor() and
    // Compositor::addOutput()). Deferred to workspaceCreated because createPlatformCursor()
    // (which constructs the X11Cursor registered as Cursors::self()->mouse()) isn't
    // guaranteed to have run yet at this point in startup.
    connect(kwinApp(), &Application::workspaceCreated, this, [this]() {
        Cursor *mouse = Cursors::self()->mouse();
        if (!mouse) {
            return;
        }
        m_cursorSource = std::make_unique<ImageCursorSource>();
        mouse->setSource(m_cursorSource.get());
        mouse->startCursorTracking();
        connect(mouse, &Cursor::cursorChanged, this, &X11StandaloneBackend::updateCursorImage);
        updateCursorImage();
    });

    m_keyboard = std::make_unique<X11Keyboard>();
}

X11StandaloneBackend::~X11StandaloneBackend()
{
    if (sceneEglDisplay() != EGL_NO_DISPLAY) {
        eglTerminate(sceneEglDisplay());
    }
    XRenderUtils::cleanup();
}

bool X11StandaloneBackend::initialize()
{
    if (!QX11Info::isPlatformX11()) {
        return false;
    }
    XRenderUtils::init(kwinApp()->x11Connection(), kwinApp()->x11RootWindow());
    initOutputs();

    if (Xcb::Extensions::self()->isRandrAvailable()) {
        m_randrEventFilter = std::make_unique<XrandrEventFilter>(this);
    }
    connect(Cursors::self(), &Cursors::hiddenChanged, this, &X11StandaloneBackend::updateCursor);
    // The native cursor is hidden/shown depending on the scale of the output the
    // pointer is currently over (see updateCursor()), so react to crossing outputs too.
    connect(Cursors::self(), &Cursors::positionChanged, this, &X11StandaloneBackend::updateCursor);
    return true;
}

std::unique_ptr<OpenGLBackend> X11StandaloneBackend::createOpenGLBackend()
{
    switch (options->glPlatformInterface()) {
#if HAVE_EPOXY_GLX
    case GlxPlatformInterface:
        if (hasGlx()) {
            return std::make_unique<GlxBackend>(m_x11Display, this);
        } else {
            qCWarning(KWIN_X11STANDALONE) << "Glx not available, trying EGL instead.";
            // no break, needs fall-through
            Q_FALLTHROUGH();
        }
#endif
    case EglPlatformInterface:
        return std::make_unique<EglBackend>(m_x11Display, this);
    default:
        // no backend available
        return nullptr;
    }
}

std::unique_ptr<Edge> X11StandaloneBackend::createScreenEdge(ScreenEdges *edges)
{
    if (!m_screenEdgesFilter) {
        m_screenEdgesFilter = std::make_unique<ScreenEdgesFilter>();
    }
    return std::make_unique<WindowBasedEdge>(edges);
}

void X11StandaloneBackend::createPlatformCursor(QObject *parent)
{
#if HAVE_X11_XINPUT
    auto c = new X11Cursor(parent, m_xinputIntegration != nullptr);
    if (m_xinputIntegration) {
        m_xinputIntegration->setCursor(c);
        // we know we have xkb already
        auto xkb = input()->keyboard()->xkb();
        xkb->setConfig(kwinApp()->kxkbConfig());
        xkb->reconfigure();
    }
#else
    new X11Cursor(parent, false);
#endif
}

bool X11StandaloneBackend::hasGlx()
{
    return Xcb::Extensions::self()->hasGlx();
}

PlatformCursorImage X11StandaloneBackend::cursorImage() const
{
    auto c = kwinApp()->x11Connection();
    UniqueCPtr<xcb_xfixes_get_cursor_image_reply_t> cursor(
        xcb_xfixes_get_cursor_image_reply(c,
                                          xcb_xfixes_get_cursor_image_unchecked(c),
                                          nullptr));
    if (!cursor) {
        return PlatformCursorImage();
    }

    QImage qcursorimg((uchar *)xcb_xfixes_get_cursor_image_cursor_image(cursor.get()), cursor->width, cursor->height,
                      QImage::Format_ARGB32_Premultiplied);
    // deep copy of image as the data is going to be freed
    return PlatformCursorImage(qcursorimg.copy(), QPoint(cursor->xhot, cursor->yhot));
}

void X11StandaloneBackend::updateCursorImage()
{
    // Re-entrancy guard: Cursor::setSource() wires CursorSource::changed() back into
    // Cursor::cursorChanged (the very signal driving this slot), so m_cursorSource->update()
    // below would otherwise immediately re-trigger this same function - infinite recursion.
    if (!m_cursorSource || m_updatingCursorImage) {
        return;
    }
    m_updatingCursorImage = true;
    const PlatformCursorImage platformImage = cursorImage();
    if (!platformImage.isNull()) {
        m_cursorSource->update(platformImage.image(), platformImage.hotSpot());
    }
    m_updatingCursorImage = false;
}

void X11StandaloneBackend::updateCursor()
{
    // XFixes cursor visibility is scoped to the X Screen, not to a CRTC/output, so it
    // can't be hidden on just one monitor. Instead, hide/show it dynamically as the
    // pointer crosses in and out of a scaled output (mirroring how e.g. the zoom
    // effect hides the native cursor while it draws its own): fast/native everywhere
    // by default, composited (see X11Output::setCursor/moveCursor and
    // Compositor::addOutput()) only while actually over a scaled output.
    bool hide = Cursors::self()->isCursorHidden();
    if (!hide && workspace()) {
        if (Output *output = workspace()->outputAt(Cursors::self()->mouse()->pos())) {
            hide = !qFuzzyCompare(output->scale(), 1.0);
        }
    }
    // This runs on every pointer motion (Cursors::positionChanged), so only actually
    // send an XCB request when the hidden state changes, not on every single move -
    // outputAt() itself is a cheap O(output count) loop, but a protocol round-trip
    // per mouse-move event would not be.
    if (hide == m_nativeCursorHidden) {
        return;
    }
    m_nativeCursorHidden = hide;
    if (hide) {
        xcb_xfixes_hide_cursor(kwinApp()->x11Connection(), kwinApp()->x11RootWindow());
    } else {
        xcb_xfixes_show_cursor(kwinApp()->x11Connection(), kwinApp()->x11RootWindow());
    }
}

void X11StandaloneBackend::startInteractiveWindowSelection(std::function<void(KWin::Window *)> callback, const QByteArray &cursorName)
{
    if (!m_windowSelector) {
        m_windowSelector = std::make_unique<WindowSelector>();
    }
    m_windowSelector->start(callback, cursorName);
}

void X11StandaloneBackend::startInteractivePositionSelection(std::function<void(const QPoint &)> callback)
{
    if (!m_windowSelector) {
        m_windowSelector = std::make_unique<WindowSelector>();
    }
    m_windowSelector->start(callback);
}

std::unique_ptr<OutlineVisual> X11StandaloneBackend::createOutline(Outline *outline)
{
    return std::make_unique<NonCompositedOutlineVisual>(outline);
}

void X11StandaloneBackend::createEffectsHandler(Compositor *compositor, WorkspaceScene *scene)
{
    new EffectsHandlerImplX11(compositor, scene);
}

QVector<CompositingType> X11StandaloneBackend::supportedCompositors() const
{
    QVector<CompositingType> compositors;
#if HAVE_EPOXY_GLX
    compositors << OpenGLCompositing;
#endif
    compositors << NoCompositing;
    return compositors;
}

void X11StandaloneBackend::initOutputs()
{
    doUpdateOutputs<Xcb::RandR::ScreenResources>();
    updateRefreshRate();
}

void X11StandaloneBackend::scheduleUpdateOutputs()
{
    m_updateOutputsTimer->start();
}

void X11StandaloneBackend::updateOutputs()
{
    doUpdateOutputs<Xcb::RandR::CurrentResources>();
    updateRefreshRate();
}

// Experimental per-output scale for X11, driven by KWIN_X11_OUTPUT_SCALE.
// Accepts either a single factor for all outputs ("2") or per-output pairs
// ("DP-1:2,HDMI-1:1.5"). This is the foundational plumbing for per-output scaling;
// coherent window sizing and input come in later stages.
static qreal x11OutputScale(const QString &name)
{
    static const QString env = qEnvironmentVariable("KWIN_X11_OUTPUT_SCALE");
    if (env.isEmpty()) {
        return 1.0;
    }
    bool ok = false;
    const qreal all = env.toDouble(&ok);
    if (ok) {
        return all > 0 ? all : 1.0;
    }
    const auto pairs = env.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const auto kv = pair.split(QLatin1Char(':'));
        if (kv.size() == 2 && kv[0] == name) {
            const qreal scale = kv[1].toDouble(&ok);
            return (ok && scale > 0) ? scale : 1.0;
        }
    }
    return 1.0;
}

template<typename T>
void X11StandaloneBackend::doUpdateOutputs()
{
    QVector<Output *> changed;
    QVector<Output *> added;
    QVector<Output *> removed = m_outputs;

    if (Xcb::Extensions::self()->isRandrAvailable()) {
        T resources(rootWindow());
        if (!resources.isNull()) {

            std::span crtcs(resources.crtcs(), resources->num_crtcs);
            for (auto crtc : crtcs) {
                Xcb::RandR::CrtcInfo info(crtc, resources->config_timestamp);

                const QRect geometry = info.rect();
                if (!geometry.isValid()) {
                    continue;
                }

                float refreshRate = -1.0f;

                for (auto mode : std::span(resources.modes(), resources->num_modes)) {
                    if (info->mode == mode.id) {
                        if (mode.htotal != 0 && mode.vtotal != 0) { // BUG 313996
                            // refresh rate calculation - WTF was wikipedia 1998 when I needed it?
                            int dotclock = mode.dot_clock,
                                vtotal = mode.vtotal;
                            if (mode.mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) {
                                dotclock *= 2;
                            }
                            if (mode.mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) {
                                vtotal *= 2;
                            }
                            refreshRate = dotclock / float(mode.htotal * vtotal);
                        }
                        break; // found mode
                    }
                }

                for (auto xcbOutput : std::span(info.outputs(), info->num_outputs)) {
                    Xcb::RandR::OutputInfo outputInfo(xcbOutput, resources->config_timestamp);
                    if (outputInfo->crtc != crtc) {
                        continue;
                    }

                    X11Output *output = findX11Output(outputInfo.name());
                    if (output) {
                        changed.append(output);
                        removed.removeOne(output);
                    } else {
                        output = new X11Output(this);
                        added.append(output);
                    }

                    // TODO: Perhaps the output has to save the inherited gamma ramp and
                    // restore it during tear down. Currently neither standalone x11 nor
                    // drm platform do this.
                    Xcb::RandR::CrtcGamma gamma(crtc);

                    output->setCrtc(crtc);
                    output->setGammaRampSize(gamma.isNull() ? 0 : gamma->size);
                    auto it = std::find(crtcs.begin(), crtcs.end(), crtc);
                    int crtcIndex = std::distance(crtcs.begin(), it);
                    output->setXineramaNumber(crtcIndex);

                    QSize physicalSize(outputInfo->mm_width, outputInfo->mm_height);
                    switch (info->rotation) {
                    case XCB_RANDR_ROTATION_ROTATE_0:
                    case XCB_RANDR_ROTATION_ROTATE_180:
                        break;
                    case XCB_RANDR_ROTATION_ROTATE_90:
                    case XCB_RANDR_ROTATION_ROTATE_270:
                        physicalSize.transpose();
                        break;
                    case XCB_RANDR_ROTATION_REFLECT_X:
                    case XCB_RANDR_ROTATION_REFLECT_Y:
                        break;
                    }

                    X11Output::Information information{
                        .name = outputInfo.name(),
                        .physicalSize = physicalSize,
                    };

                    auto edidProperty = Xcb::RandR::OutputProperty(xcbOutput, atoms->edid, XCB_ATOM_INTEGER, 0, 100, false, false);
                    bool ok;
                    if (auto data = edidProperty.toByteArray(&ok); ok && !data.isEmpty()) {
                        if (auto edid = Edid(data, edidProperty.data()->num_items); edid.isValid()) {
                            information.manufacturer = edid.manufacturerString();
                            information.model = edid.monitorName();
                            information.serialNumber = edid.serialNumber();
                            information.edid = data;
                        }
                    }

                    // Per-output DPI from the RandR "DPI" property (robust to INTEGER
                    // or CARDINAL). Drives the per-output scale: scale = dpi/96, so a
                    // 96 dpi output is 1:1 and a 192 dpi one is 2x. Falls back to the
                    // KWIN_X11_OUTPUT_SCALE env when no DPI property is present.
                    int dpiValue = 0;
                    if (atoms->dpi.isValid()) {
                        auto readDpi = [&](xcb_atom_t type) -> int {
                            auto prop = Xcb::RandR::OutputProperty(xcbOutput, atoms->dpi, type, 0, 1, false, false);
                            bool dpiOk = false;
                            const int32_t value = prop.value<int32_t>(0, &dpiOk);
                            return (dpiOk && value > 0) ? value : 0;
                        };
                        dpiValue = readDpi(XCB_ATOM_INTEGER);
                        if (dpiValue == 0) {
                            dpiValue = readDpi(XCB_ATOM_CARDINAL);
                        }
                    }

                    auto mode = std::make_shared<OutputMode>(geometry.size(), refreshRate * 1000);

                    X11Output::State state = output->m_state;
                    state.modes = {mode};
                    state.currentMode = mode;
                    state.position = geometry.topLeft();
                    state.dpi = dpiValue;
                    // Per-output scaling only makes sense while compositing: the
                    // compositor is what draws each logical output onto the larger
                    // physical panel. Without a compositor there is no scaling, so keep
                    // the output at its native size (scale 1) and use the whole panel.
                    if (Compositor::compositing()) {
                        state.scale = dpiValue > 0 ? dpiValue / 96.0 : x11OutputScale(outputInfo.name());
                    } else {
                        state.scale = 1.0;
                    }

                    output->setInformation(information);
                    output->setState(state);
                    break;
                }
            }
        }
    }

    // The workspace handles having no outputs poorly. If the last output is about to be
    // removed, create a dummy output to avoid crashing.
    if (changed.isEmpty() && added.isEmpty()) {
        auto dummyOutput = new X11PlaceholderOutput(this);
        m_outputs << dummyOutput;
        Q_EMIT outputAdded(dummyOutput);
        dummyOutput->updateEnabled(true);
    }

    // Process new outputs. Note new outputs must be introduced before removing any other outputs.
    for (Output *output : std::as_const(added)) {
        m_outputs.append(output);
        Q_EMIT outputAdded(output);
        if (auto placeholderOutput = qobject_cast<X11PlaceholderOutput *>(output)) {
            placeholderOutput->updateEnabled(true);
        } else if (auto nativeOutput = qobject_cast<X11Output *>(output)) {
            nativeOutput->updateEnabled(true);
        }
    }

    // Outputs have to be removed last to avoid the case where there are no enabled outputs.
    for (Output *output : std::as_const(removed)) {
        m_outputs.removeOne(output);
        if (auto placeholderOutput = qobject_cast<X11PlaceholderOutput *>(output)) {
            placeholderOutput->updateEnabled(false);
        } else if (auto nativeOutput = qobject_cast<X11Output *>(output)) {
            nativeOutput->updateEnabled(false);
        }
        Q_EMIT outputRemoved(output);
        output->unref();
    }

    // Make sure that the position of an output in m_outputs matches its xinerama index, there
    // are X11 protocols that use xinerama indices to identify outputs.
    std::sort(m_outputs.begin(), m_outputs.end(), [](const Output *a, const Output *b) {
        const auto xa = qobject_cast<const X11Output *>(a);
        if (!xa) {
            return false;
        }
        const auto xb = qobject_cast<const X11Output *>(b);
        if (!xb) {
            return true;
        }
        return xa->xineramaNumber() < xb->xineramaNumber();
    });

    updateInputScale();
    // A runtime DPI change may have (de)scaled the output currently under the
    // pointer without the pointer itself moving; re-evaluate native vs composited.
    updateCursor();

    Q_EMIT outputsQueried();
}

void X11StandaloneBackend::updateInputScale()
{
    // X-INPUT-SCALE wire protocol, mirrored locally so we don't depend on the
    // server's headers (no libXext/libxcb binding exists yet - see
    // Xext/inputscale/inputscaleproto.h in the Xlibre fork). Coordinates are
    // desktop-absolute, the same convention RandR itself uses for crtc->x/y -
    // no matrix, no separate "logical space".
    static constexpr uint8_t X_XISSetCrtcConfine = 1;
    static constexpr uint8_t X_XISResetCrtcConfine = 3;

    xcb_connection_t *c = kwinApp()->x11Connection();
    if (!c) {
        return;
    }

    if (!m_inputScaleChecked) {
        m_inputScaleChecked = true;
        const char name[] = "X-INPUT-SCALE";
        UniqueCPtr<xcb_query_extension_reply_t> reply(
            xcb_query_extension_reply(c, xcb_query_extension(c, sizeof(name) - 1, name), nullptr));
        if (reply && reply->present) {
            m_inputScaleOpcode = reply->major_opcode;
            qCWarning(KWIN_X11STANDALONE) << "X-INPUT-SCALE: extension present, opcode" << m_inputScaleOpcode;
        } else {
            qCWarning(KWIN_X11STANDALONE) << "X-INPUT-SCALE: extension NOT available; pointer can wander into unused scanout pixels on scaled outputs";
        }
    }
    if (!m_inputScaleOpcode) {
        return; // extension absent: KWin keeps working normally, pointer just isn't confined
    }

    auto send = [c](uint8_t opcode, const void *req, size_t len) {
        struct iovec parts[4];
        xcb_protocol_request_t r;
        r.count = 2;
        r.ext = nullptr;
        r.opcode = opcode;
        r.isvoid = 1;
        parts[2].iov_base = const_cast<void *>(req);
        parts[2].iov_len = len;
        parts[3].iov_base = nullptr;
        parts[3].iov_len = -len & 3;
        xcb_send_request(c, 0, parts + 2, &r);
    };

    for (Output *output : std::as_const(m_outputs)) {
        auto *x11Output = qobject_cast<X11Output *>(output);
        if (!x11Output || x11Output->crtc() == XCB_NONE) {
            continue;
        }

        // Output::geometry() = QRect(position, pixelSize()/scale()) - the same logical/
        // active box already used to clamp override-redirect popups (events.cpp). At
        // scale 1 this equals the CRTC's full physical pixel size, so there is nothing
        // to confine; explicitly reset in that case rather than setting a same-size box,
        // since a stale confinement from a previous larger scale must not linger.
        const QRect box = x11Output->geometry();
        if (box.size() == x11Output->pixelSize()) {
            struct
            {
                uint8_t reqType;
                uint8_t xisReqType;
                uint16_t length;
                uint32_t crtc;
            } req = {};
            req.xisReqType = X_XISResetCrtcConfine;
            req.crtc = x11Output->crtc();
            send(m_inputScaleOpcode, &req, sizeof(req));
            continue;
        }

        struct
        {
            uint8_t reqType;
            uint8_t xisReqType;
            uint16_t length;
            uint32_t crtc;
            int16_t x;
            int16_t y;
            uint16_t width;
            uint16_t height;
        } req = {};
        req.xisReqType = X_XISSetCrtcConfine;
        req.crtc = x11Output->crtc();
        req.x = box.x();
        req.y = box.y();
        req.width = box.width();
        req.height = box.height();
        qCWarning(KWIN_X11STANDALONE) << "X-INPUT-SCALE: confine crtc" << x11Output->crtc()
                                      << "to" << box << "physical" << x11Output->pixelSize();
        send(m_inputScaleOpcode, &req, sizeof(req));
    }
    xcb_flush(c);
}

X11Output *X11StandaloneBackend::findX11Output(const QString &name) const
{
    for (Output *output : m_outputs) {
        if (output->name() == name) {
            return qobject_cast<X11Output *>(output);
        }
    }
    return nullptr;
}

Outputs X11StandaloneBackend::outputs() const
{
    return m_outputs;
}

X11Keyboard *X11StandaloneBackend::keyboard() const
{
    return m_keyboard.get();
}

RenderLoop *X11StandaloneBackend::renderLoop() const
{
    return m_renderLoop.get();
}

static bool refreshRate_compare(const Output *first, const Output *smallest)
{
    return first->refreshRate() < smallest->refreshRate();
}

static int currentRefreshRate()
{
    static const int refreshRate = qEnvironmentVariableIntValue("KWIN_X11_REFRESH_RATE");
    if (refreshRate) {
        return refreshRate;
    }

    const QVector<Output *> outputs = kwinApp()->outputBackend()->outputs();
    if (outputs.isEmpty()) {
        return 60000;
    }

    static const QString syncDisplayDevice = qEnvironmentVariable("__GL_SYNC_DISPLAY_DEVICE");
    if (!syncDisplayDevice.isEmpty()) {
        for (const Output *output : outputs) {
            if (output->name() == syncDisplayDevice) {
                return output->refreshRate();
            }
        }
    }

    auto syncIt = std::min_element(outputs.begin(), outputs.end(), refreshRate_compare);
    return (*syncIt)->refreshRate();
}

void X11StandaloneBackend::updateRefreshRate()
{
    // Each output drives its own render loop at its own refresh rate, so that
    // outputs with different refresh rates (e.g. 60 Hz + 144 Hz) can be paced
    // independently instead of all being locked to the lowest common rate.
    for (Output *output : std::as_const(m_outputs)) {
        RenderLoop *loop = output->renderLoop();
        if (!loop) {
            continue;
        }
        int refreshRate = output->refreshRate();
        if (refreshRate <= 0) {
            qCWarning(KWIN_X11STANDALONE) << "Bogus refresh rate" << refreshRate << "on" << output->name();
            refreshRate = 60000;
        }
        loop->setRefreshRate(refreshRate);
    }

    // Transitional: the GLX/EGL backends still feed a single, backend-wide render
    // loop until per-output presentation feedback is wired up. Keep it on the
    // lowest common refresh rate to preserve the previous behavior meanwhile.
    int refreshRate = currentRefreshRate();
    if (refreshRate <= 0) {
        refreshRate = 60000;
    }
    m_renderLoop->setRefreshRate(refreshRate);
}

}
