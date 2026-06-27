# Task: make Klassy keep rounded corners when compositing is OFF

> Self-contained brief. Copy this file into the **Klassy** repository
> (https://github.com/paulmcauley/klassy, branch compatible with KWin/Plasma 5.27)
> so a future session can implement it there without needing prior context.

## Goal

When the X11 compositor is disabled, Klassy currently falls back to a flat opaque
square titlebar (no rounded corners, no side borders, no colored outline). Make Klassy
keep drawing its full decoration (rounded frame + 4-side borders + thin state-colored
outline) in that mode, and tell KWin how to clip the window to the rounded silhouette.

The matching KWin side is **already implemented in a KWin 5.27 fork** (see "KWin
contract" below). Klassy just has to (1) publish its corner radius/corners and (2) stop
simplifying when there is no alpha channel.

## Background (why the look disappears uncomposited)

- A decoration's rounded/translucent corners need **alpha blending**, which only a
  compositor provides. Uncomposited, a window is an opaque rectangle on the X server.
- KDecoration2 tells the decoration this via `settings()->isAlphaChannelSupported()`.
  When it is `false`, Breeze/Klassy switch to a simplified opaque square to avoid black
  corners.
- The genuine way to get rounded corners without a compositor is the **X Shape**
  extension: KWin clips the frame window to a rounded region. Result is **aliased**
  (no AA) and still has **no shadow/translucency** — that is expected and unavoidable.

## KWin contract (already done on the KWin side)

The KWin fork reads these from the decoration via **Qt dynamic properties** on the
`KDecoration2::Decoration` object (no kdecoration library API change needed), and applies
an XShape bounding shape to the frame while uncomposited:

| Property | Type | Meaning |
|---|---|---|
| `decorationCornerRadius` | `int` | radius in **logical px**; `<=0` or unset = no rounding |
| `decorationRoundedCorners` | `int` | corner bitmask, default all four |
| `decorationShapeMask` | `QRegion` | exact silhouette, decoration-local coords (optional, overrides the two above) |

Corner bitmask: `TopLeft=1`, `TopRight=2`, `BottomLeft=4`, `BottomRight=8`. All = `15`;
"only the two top corners" = `3`.

KWin re-reads these on decoration (re)creation, frame resize, compositing toggle and
config change. Changing settings in Klassy's KCM reconfigures/recreates the decoration,
so updates are picked up automatically.

## What to change in Klassy

Klassy is a C++ KDecoration2 plugin (a Breeze fork). The decoration class lives under
`kdecoration/` (look for the subclass of `KDecoration2::Decoration`, e.g.
`breezedecoration.cpp` / class `Decoration`, and its `recalculateBorders()`,
`reconfigure()`, `paint()` methods and the corner-radius setting in `breezesettingsdata`
/ the KCM config).

### 1. Publish radius + corners to KWin

In the decoration class, whenever the radius or corner choice is (re)computed (end of
`recalculateBorders()` / `reconfigure()`), set:

```cpp
// radius Klassy already uses to paint rounded corners, in logical px
setProperty("decorationCornerRadius", m_internalSettings->cornerRadius());

// which corners Klassy rounds (Klassy can round all four or only the top two)
int corners = roundsBottomCorners() ? 15 : 3;   // map to your existing option
setProperty("decorationRoundedCorners", corners);
```

Setting them unconditionally is fine — KWin only uses them while uncomposited.

### 2. Don't simplify when `isAlphaChannelSupported()` is false

Find the code paths gated on `settings()->isAlphaChannelSupported()` (in `paint()` /
border/shadow logic) that switch to the square/minimal look, and make the uncomposited
path still draw:

- the title bar and all **4 side borders**,
- the thin **state-colored outline** (active/inactive/accent — unchanged logic),
- the corners painted with the **same radius/corners** reported in step 1, so the
  painted outline lines up with KWin's XShape cut.

Drop only what is physically impossible uncomposited: the **drop shadow** and any
**translucency/blur** (those require a compositor).

## Acceptance test

1. Build/install Klassy; use it as the decoration.
2. Disable compositing (e.g. Alt+Shift+F12 on X11, or run KWin uncomposited).
3. Windows should show Klassy's rounded silhouette + 4 borders + colored outline
   following the rounded edge (aliased corners, no shadow — expected).
4. Changing corner radius / "round bottom corners" in Klassy's settings should update
   live (KWin re-reads on reconfigure).

If KWin does not clip, verify the properties are actually set on the Decoration object
(e.g. log `property("decorationCornerRadius")`), and that the KWin fork with the
contract above is running.
