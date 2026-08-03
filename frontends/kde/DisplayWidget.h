/*
 * DisplayWidget: the emulator video widget for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QOpenGLWidget>

#include <vector>

#include "msxsession.h"

class DisplayWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(msxsession *session, QWidget *parent = nullptr);

protected:
    void paintGL() override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void onFrameSwapped();
    void forwardKey(const QKeyEvent *event, int down);
    QRectF destRect() const;

    msxsession *m_session;
    /* The session writes tightly-packed XRGB8888 rows, so the frame is kept
     * in a plain buffer and QImage is only ever a view over it -- QImage's
     * own stride is an implementation detail we must not depend on. On a
     * little-endian host XRGB8888 is exactly QImage::Format_RGB32. Unlike
     * the sibling repos' DisplayWidget, the size here is FIXED
     * (MSXSESSION_FB_WIDTH x MSXSESSION_FB_HEIGHT) -- openMSX's own
     * getLinePtr640_480() scaling normalises every source video mode before
     * msxsession ever sees a frame. */
    std::vector<uint32_t> m_frame;
    /* uint64_t, not quint64: the session takes a uint64_t* and the two are
     * distinct types here (unsigned long vs unsigned long long). */
    uint64_t m_serial = 0;
};
