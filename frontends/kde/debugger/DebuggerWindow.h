/*
 * Debugger window for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>

#include "msxdebug.h"
#include "msxsession.h"
#include "msxvdp.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows (creating on first use) the debugger for the session. */
    static void showFor(QWidget *parent, msxsession *session);

signals:
    void stopped(int reason, quint16 pc);

private:
    DebuggerWindow(QWidget *parent, msxsession *session);
    ~DebuggerWindow() override;

    void buildUi();
    void refreshAll();
    void refreshDisasm();
    void refreshRegs();
    void refreshMem();
    void refreshBps();
    void refreshVdp();
    void onStopped(int reason, quint16 pc);
    void pauseContinue();
    bool parseAddr(const QString &text, quint16 *out);
    void vramSubmit();
    void vramPage(int delta);

    msxsession *m_session;
    msxdebug *m_dbg;
    msxvdp_chip m_chip;

    QLabel *m_status;
    QPushButton *m_pauseBtn;
    QPlainTextEdit *m_disasm;
    quint16 m_disasmBase = 0;
    bool m_followPc = true;

    QLineEdit *m_regEdit[8];
    QLabel *m_flags;

    QLineEdit *m_memAddr;
    QPlainTextEdit *m_memView;
    quint16 m_memBase = 0;

    QLineEdit *m_bpEntry;
    QPlainTextEdit *m_bpView;

    QLabel *m_nt, *m_pat, *m_spr, *m_pal;
    QComboBox *m_patBank;
    QPlainTextEdit *m_spriteInfo;
    QPlainTextEdit *m_vdpState;

    QLineEdit *m_vramEntry;
    QLabel *m_vramStatus;
    QPlainTextEdit *m_vramView;
    quint32 m_vramBase = 0;

    QTimer *m_tick;
};
