/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/output.h"

#include <memory>

namespace KWin
{

class X11StandaloneBackend;

class X11PlaceholderOutput : public Output
{
    Q_OBJECT

public:
    explicit X11PlaceholderOutput(X11StandaloneBackend *backend, QObject *parent = nullptr);
    ~X11PlaceholderOutput() override;

    RenderLoop *renderLoop() const override;

    void updateEnabled(bool enabled);

private:
    X11StandaloneBackend *m_backend;
    std::unique_ptr<RenderLoop> m_loop;
};

} // namespace KWin
