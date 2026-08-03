/*
 * DisplayWidget: paints the latest emulator frame with QPainter over a
 * QOpenGLWidget, letterboxed to 4:3 (the fixed 640x480 buffer already is
 * 4:3, so no per-mode aspect table is needed here -- see DisplayWidget.h).
 * The frameSwapped signal (vsync-aligned with the default swap interval of
 * 1) feeds the session's (advisory-only, see msxsession.h) vsync
 * notification and schedules the next repaint, forming a self-sustaining
 * present loop.
 *
 * Key events are translated to the X11 keysyms msx_key_from_event expects
 * (core/src/input_map.c), so this frontend and the GNOME one share one
 * mapping table -- the numeric XK_* values here are exactly the ones that
 * file hardcodes, deliberately kept in sync. Both press and release are
 * forwarded: openMSX's Keyboard tracks held keys, so a missed release
 * leaves that key down in the matrix.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QKeyEvent>
#include <QPainter>

#include <ctime>

namespace {

qint64 monotonic_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (qint64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Qt key code -> X11/xkb keysym, for the keys msx_key_from_event resolves
 * by keysym rather than by character. Values match
 * core/src/input_map.c's XK_* table exactly. */
quint32 keysymForKey(const QKeyEvent *event)
{
    /* F1..F9. F10, F11 and F12 are the frontend's own (menu, fullscreen,
     * debugger) and never reach here -- event() lets those three through as
     * shortcuts instead of claiming them. */
    if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F9)
        return 0xFFBE + (quint32)(event->key() - Qt::Key_F1);

    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:     return 0xFF0D;
    case Qt::Key_Escape:    return 0xFF1B;
    case Qt::Key_Tab:       return 0xFF09;
    case Qt::Key_Backspace: return 0xFF08;
    case Qt::Key_Delete:    return 0xFFFF;
    case Qt::Key_Insert:    return 0xFF63;
    case Qt::Key_Home:      return 0xFF50;
    case Qt::Key_End:       return 0xFF57;
    case Qt::Key_PageUp:    return 0xFF55;
    case Qt::Key_PageDown:  return 0xFF56;
    case Qt::Key_Left:      return 0xFF51;
    case Qt::Key_Up:        return 0xFF52;
    case Qt::Key_Right:     return 0xFF53;
    case Qt::Key_Down:      return 0xFF54;
    /* Modifiers are real MSX keys, so forwarded rather than swallowed. Qt
     * reports left and right as the same key code for Shift/Control/Alt, so
     * both land on the left one -- input_map.c's table has no right-hand
     * equivalent to distinguish anyway for the keys this needs. */
    case Qt::Key_Shift:     return 0xFFE1; /* Shift_L */
    case Qt::Key_Control:   return 0xFFE3; /* Control_L */
    case Qt::Key_Alt:       return 0xFFE9; /* Alt_L */
    case Qt::Key_AltGr:     return 0xFFEA; /* Alt_R */
    case Qt::Key_Meta:      return 0xFFEB; /* Super_L */
    case Qt::Key_CapsLock:  return 0xFFE5;
    default:                return 0;
    }
}

} // namespace

DisplayWidget::DisplayWidget(msxsession *session, QWidget *parent)
    : QOpenGLWidget(parent),
      m_session(session),
      m_frame((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT, 0)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(MSXSESSION_FB_WIDTH / 2, MSXSESSION_FB_HEIGHT / 2);
    connect(this, &QOpenGLWidget::frameSwapped, this,
            &DisplayWidget::onFrameSwapped, Qt::QueuedConnection);
}

void DisplayWidget::onFrameSwapped()
{
    msxsession_notify_vsync(m_session, monotonic_ns());
    update();
}

void DisplayWidget::showEvent(QShowEvent *event)
{
    QOpenGLWidget::showEvent(event);
    update(); /* kick the frameSwapped/update loop */
}

QRectF DisplayWidget::destRect() const
{
    const qreal w = width(), h = height();
    constexpr qreal aspect = (qreal)MSXSESSION_FB_WIDTH / MSXSESSION_FB_HEIGHT;
    qreal dw, dh;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }
    return QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

void DisplayWidget::paintGL()
{
    uint64_t serial = m_serial;

    if (msxsession_copy_frame(m_session, m_frame.data(), &serial))
        m_serial = serial;

    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    /* A view over the packed buffer: rows are MSXSESSION_FB_WIDTH pixels
     * apart, always -- no per-frame geometry to track. */
    const QImage frame((const uchar *)m_frame.data(), MSXSESSION_FB_WIDTH,
                       MSXSESSION_FB_HEIGHT, MSXSESSION_FB_WIDTH * 4,
                       QImage::Format_RGB32);
    p.drawImage(destRect(), frame);
}

bool DisplayWidget::event(QEvent *event)
{
    /* Claim every key except the reserved chords (F10 menu, F11 fullscreen,
     * F12 debugger) while focused, so menu accelerators cannot shadow what
     * the MSX should receive. Alt in particular must NOT reach the menu
     * bar: it is a real key on an MSX keyboard. */
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() != Qt::Key_F10 && ke->key() != Qt::Key_F11 &&
            ke->key() != Qt::Key_F12) {
            event->accept();
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

void DisplayWidget::forwardKey(const QKeyEvent *event, int down)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    quint32 unicode = 0;
    if (!event->text().isEmpty())
        unicode = event->text().at(0).unicode();

    quint32 keysym = keysymForKey(event);
    if (keysym == 0) {
        /* Printable: Qt gives no text for a Ctrl combo, so fall back to the
         * unshifted letter, which is what the keysym contract wants. */
        if (ctrl && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
            keysym = 'a' + (quint32)(event->key() - Qt::Key_A);
        else
            keysym = unicode;
    }
    if (keysym == 0)
        return;

    msxsession_key(m_session, down, keysym, unicode, ctrl ? 1 : 0,
                  shift ? 1 : 0);
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        /* openMSX's Keyboard holds a key down until it is released, and the
         * MSX BIOS does its own auto-repeat. A host repeat storm would just
         * be redundant press events on an already-held key. */
        event->accept();
        return;
    }
    forwardKey(event, 1);
    event->accept();
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    forwardKey(event, 0);
    event->accept();
}
