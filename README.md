# KWin (personal fork)

> **This is an unofficial personal fork of KWin.** It is **not affiliated
> with, endorsed by, or supported by KDE** or the KWin development team.
> It is not based on a current KWin release and does not receive upstream
> security or bug fixes — treat it as an experimental research codebase,
> not something to run as your daily driver. See
> [What this fork adds](#what-this-fork-adds) below for what's actually
> going on here; everything past that point (contributing, mailing lists,
> bug tracker) describes upstream KWin, not this repository.

KWin is an easy to use, but flexible, composited Window Manager for Xorg windowing systems (Wayland, X11) on Linux. Its primary usage is in conjunction with a Desktop Shell (e.g. KDE Plasma Desktop). KWin is designed to go out of the way; users should not notice that they use a window manager at all. Nevertheless KWin provides a steep learning curve for advanced features, which are available, if they do not conflict with the primary mission. KWin does not have a dedicated targeted user group, but follows the targeted user group of the Desktop Shell using KWin as it's window manager.

## KWin is not...

 * a standalone window manager (c.f. openbox, i3) and does not provide any functionality belonging to a Desktop Shell.
 * a replacement for window managers designed for use with a specific Desktop Shell (e.g. GNOME Shell)
 * a minimalistic window manager
 * designed for use without compositing or for X11 network transparency, though both are possible.

## What this fork adds

This fork is a personal, in-progress experiment pushing X11 support well past what's normally considered feasible for that windowing system — mostly by pairing KWin with a matching personal fork of the X server (XLibre) that adds a couple of small, narrowly-scoped extensions. The headline pieces:

 * **A render loop per output**, so mixed-refresh-rate multi-monitor setups (a 144Hz panel next to a 60Hz one) each actually run at their own rate on X11, instead of the whole session being pinned to the slowest/fastest screen.

This is exploratory, single-developer-plus-AI work, not a
polished feature. It's genuinely experimental — things break, some corners
are half-finished, and the matching XLibre changes are required for most of
it to do anything at all.

---

The sections below are preserved from upstream KWin and describe the **original project**, not this fork — there is no mailing list, IRC channel, or bug tracker for this repository to report issues against.

# Contributing to KWin

Please refer to the [contributing document](CONTRIBUTING.md) for everything you need to know to get started contributing to KWin.

# Contacting KWin development team

 * mailing list: [kwin@kde.org](https://mail.kde.org/mailman/listinfo/kwin)
 * IRC: #kde-kwin on irc.libera.chat

# Support
## Application Developer
If you are an application developer having questions regarding windowing systems (either X11 or Wayland) please do not hesitate to contact us. Preferable through our mailing list. Ideally subscribe to the mailing list, so that your mail doesn't get stuck in the moderation queue.

## End user
Please contact the support channels of your Linux distribution for user support. The KWin development team does not provide end user support.

# Reporting bugs

Please use [KDE's bugtracker](https://bugs.kde.org) and report for [product KWin](https://bugs.kde.org/enter_bug.cgi?product=kwin) — **for upstream KWin only**. Bugs in this fork's own experimental changes are not KDE's to triage; they're specific to this repository and the paired Xlibre fork.

## Guidelines for new features

A new Feature can only be added to KWin if:

 * it does not violate the primary missions as stated at the start of this document
 * it does not introduce instabilities
 * it is maintained, that is bugs are fixed in a timely manner (second next minor release) if it is not a corner case.
 * it works together with all existing features
 * it supports both single and multi screen (xrandr)
 * it adds a significant advantage
 * it is feature complete, that is supports at least all useful features from competitive implementations
 * it is not a special case for a small user group
 * it does not increase code complexity significantly
 * it does not affect KWin's license (GPLv2+)

All new added features are under probation, that is if any of the non-functional requirements as listed above do not hold true in the next two feature releases, the added feature will be removed again.

The same non functional requirements hold true for any kind of plugins (effects, scripts, etc.). It is suggested to use scripted plugins and distribute them separately.
