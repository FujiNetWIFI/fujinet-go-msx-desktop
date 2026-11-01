/*
 * The software display: the fallback for a machine with no usable GPU
 * device. A QWidget that scales with QPainter, fed by FrameFeed like the GPU
 * one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QWidget>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "msxsession.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(msxsession *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    msxsession *m_session;
    FrameFeed *m_feed;
    QImage m_image;            /* Format_RGB32, 640x480 */
    uint64_t m_serial = 0;
};
