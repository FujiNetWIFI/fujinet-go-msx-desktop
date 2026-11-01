/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"
#include "KeyForward.h"

#include <QEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(msxsession *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_image(MSXSESSION_FB_WIDTH, MSXSESSION_FB_HEIGHT, QImage::Format_RGB32)
{
    m_image.fill(Qt::black);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(MSXSESSION_FB_WIDTH / 2, MSXSESSION_FB_HEIGHT / 2);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

bool SoftDisplayWidget::event(QEvent *e)
{
    if (msxClaimShortcut(e))
        return true;
    return QWidget::event(e);
}

void SoftDisplayWidget::keyPressEvent(QKeyEvent *e) { msxKeyEvent(m_session, e, 1); }
void SoftDisplayWidget::keyReleaseEvent(QKeyEvent *e) { msxKeyEvent(m_session, e, 0); }

void SoftDisplayWidget::onFrame()
{
    if (!msxsession_copy_frame(m_session, reinterpret_cast<uint32_t *>(m_image.bits()),
                               &m_serial))
        return;
    /* The pixels are 0x00RRGGBB; Format_RGB32 needs 0xFFRRGGBB, or KWin
     * composites the zero alpha as see-through, so force alpha on. */
    quint32 *px = reinterpret_cast<quint32 *>(m_image.bits());
    const size_t n = (size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT;
    for (size_t i = 0; i < n; i++)
        px[i] |= 0xFF000000u;
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    p.drawImage(displayRect(QSizeF(size())), m_image);
}
