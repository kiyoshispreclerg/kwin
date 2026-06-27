# Rounded (shaped) decorations without compositing — KWin ⇄ decoration contract

This fork can clip a decorated X11 window to a rounded / non‑rectangular silhouette
while **compositing is OFF**, using the X Shape extension (the mechanism the old
KDecoration1 themes used via the removed `setMask()`).

It rounds the **whole window** (decoration borders **and** client contents) because
the shape is applied to the frame window (`frameId()`), which contains both.

> Limitations (inherent to running without a compositor):
> - corners are **aliased** (X Shape is 1 bit per pixel — no anti‑aliasing);
> - **no drop shadow** and **no translucency** (those need alpha blending);
> - only applies on **X11** and only while **uncomposited** (with compositing on,
>   the shape stays rectangular and the decoration's own alpha channel rounds the
>   corners as usual).

## KWin side (already implemented here)

`X11Window::updateDecorationCornerShape()` (in `src/x11window.cpp`) runs whenever the
decoration is (re)created, the frame is resized, compositing is toggled, or the config
changes. It applies a bounding shape only when **all** of these hold: decorated, not
client‑shaped, not maximized, not fullscreen, and compositing is OFF.

It picks the shape from, in order of priority:

1. **`decorationShapeMask`** — `QRegion`, the decoration's exact opaque silhouette
   (advanced/optional override).
2. **`decorationCornerRadius`** — `int` (logical px) + **`decorationRoundedCorners`**
   — `int` bitmask of which corners to round. KWin builds the rounded rectangle.
3. **kwinrc fallback** (testing only): `[Windows] CornerRadius` (int) +
   `[Windows] CornerRadiusCorners` (int bitmask, default all corners).

All coordinates/sizes are **logical pixels**; KWin converts to native (`Xcb::toXNative`).

### Corner bitmask

| Corner | Bit |
|---|---|
| Top‑left | `1` |
| Top‑right | `2` |
| Bottom‑left | `4` |
| Bottom‑right | `8` |

All four = `15`. Top two only = `1 | 2 = 3`.

### Quick test without touching Klassy

```ini
# ~/.config/kwinrc
[Windows]
CornerRadius=12
CornerRadiusCorners=15   # optional; 3 = only the top two
```

Apply with `qdbus org.kde.KWin /KWin reconfigure` (or restart KWin) **while
uncomposited**. Changing the value and reconfiguring updates it live now.

## Klassy side (what you need to add)

Klassy is an external `KDecoration2` plugin. Two changes:

### 1. Tell KWin what you want (the API)

On your `Decoration` subclass, set these dynamic properties whenever the radius / corner
choice changes (e.g. at the end of `recalculateBorders()` / `reconfigure()`):

```cpp
// radius in logical px; 0 (or not set) disables rounding
setProperty("decorationCornerRadius", cornerRadius());

// which corners to round: TopLeft=1, TopRight=2, BottomLeft=4, BottomRight=8
// all four = 15; "only the two top corners" = 3
int corners = roundBottomCornersToo() ? 15 : 3;
setProperty("decorationRoundedCorners", corners);
```

That's it — KWin builds the rounded rectangle from these. Setting them unconditionally
is fine; KWin only consumes them while uncomposited.

> KWin re-reads on decoration (re)creation, frame resize, compositing toggle and config
> change. Changing the radius through Klassy's settings already reconfigures/recreates
> the decoration, so it is picked up. (If you ever need a push update without any of
> those, emit a signal and we can `connect` it on the KWin side.)

**Advanced (optional):** if your shape is not a plain rounded rectangle, skip the two
properties above and instead publish the exact silhouette as a `QRegion`
(decoration‑local coords, `0,0 … size()`):

```cpp
QPainterPath path; path.addRoundedRect(QRectF(QPointF(0,0), size()), r, r);
setProperty("decorationShapeMask", QVariant::fromValue(QRegion(path.toFillPolygon().toPolygon())));
```

### 2. Don't simplify when there is no alpha channel

Today Klassy (like Breeze) draws a plain opaque rectangle when
`settings()->isAlphaChannelSupported()` is `false`. With KWin now clipping the frame,
keep drawing the **full** decoration in that case:

- draw the title bar, the 4‑side borders and the thin state‑colored outline as usual;
- paint the corners with the **same radius/corners** you reported, so the painted
  outline lines up with the XShape cut;
- accept that the result is opaque + aliased + shadowless (expected without a compositor).

Concretely: find the branch(es) gated on `isAlphaChannelSupported()` that switch to the
square/minimal look and make the uncomposited path render the rounded frame + outline
instead.

## Summary of the API surface

| Direction | Name | Type | Meaning |
|---|---|---|---|
| Klassy → KWin | property `decorationCornerRadius` | `int` | radius in logical px; ≤0 = off |
| Klassy → KWin | property `decorationRoundedCorners` | `int` | corner bitmask (default 15) |
| Klassy → KWin | property `decorationShapeMask` | `QRegion` | exact silhouette (optional override) |
| user → KWin (fallback) | `kwinrc [Windows] CornerRadius` | `int` | radius in logical px |
| user → KWin (fallback) | `kwinrc [Windows] CornerRadiusCorners` | `int` | corner bitmask |

No change to the `kdecoration` library API is required — these are plain Qt dynamic
properties on the decoration object.
