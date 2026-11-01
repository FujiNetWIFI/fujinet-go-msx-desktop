/*
 * FrameFeed -- tells the GUI thread when the session has a new frame.
 *
 * Qt Widgets has no frame clock to hang a tick on, and a self-sustaining
 * frameSwapped->update() loop redraws at the monitor rate whether or not the
 * picture moved. The session calls back from the emulator thread after each
 * new frame; this turns that into a queued signal on the GUI thread.
 *
 *  - Coalescing: at most one signal is in flight, so a GUI thread that falls
 *    behind sees one wakeup, not a backlog; the frame it copies is the
 *    newest anyway (the slot is latest-wins).
 *  - Idle: while inactive (the window is hidden or minimised) the callback
 *    is unregistered altogether, so a minimised emulator costs the GUI
 *    thread no wakeups. Becoming active again signals once, to catch up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <atomic>

#include "msxsession.h"

class FrameFeed : public QObject {
    Q_OBJECT
public:
    explicit FrameFeed(msxsession *session, QObject *parent = nullptr);
    ~FrameFeed() override;

    void setActive(bool active);

signals:
    void frameReady();

private:
    static void onFrame(void *self);
    void deliver();

    msxsession *m_session;
    std::atomic<bool> m_pending{false};
    bool m_active = false;
};
