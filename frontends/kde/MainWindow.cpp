/*
 * MainWindow: menu bar over the emulator display. Plain Qt6 Widgets, no KDE
 * Frameworks -- Breeze arrives through the platform theme, and staying
 * framework-free keeps this frontend reusable elsewhere.
 *
 * Machine is a checkable radio group: switching is live
 * (msxsession_set_machine -- no restart, unlike the sibling repos), so the
 * menu is the readout of what is selected right now.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

#include <iterator>

#include "DisplayWidget.h"
#include "SoftDisplayWidget.h"
#include "FujiNetWindows.h"
#include "help/HelpWindow.h"
#include "SettingsDialog.h"
#include "debugger/DebuggerWindow.h"

#ifndef MSX_VERSION_STRING
#define MSX_VERSION_STRING "0.0.0"
#endif

namespace {
struct MachineEntry { const char *label; const char *id; };
constexpr MachineEntry kMachines[] = {
    { "MSX",   MSXSESSION_MACHINE_MSX },
    { "MSX2",  MSXSESSION_MACHINE_MSX2 },
    { "MSX2+", MSXSESSION_MACHINE_MSX2P },
};
} // namespace

MainWindow::MainWindow(msxsession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go MSX"));
    setProperty("msxHelpContext", QStringLiteral("main"));
    resize(960, 780);

    auto *gpu = new DisplayWidget(session, this);
    m_display = gpu;
    setCentralWidget(gpu);
    /* No usable GPU device: fall back to painting in software */
    connect(gpu, &QRhiWidget::renderFailed, this, &MainWindow::useSoftwareDisplay,
            Qt::QueuedConnection);
    statusBar();

    buildMenus();
    m_display->widget()->setFocus();
}

void MainWindow::useSoftwareDisplay()
{
    if (dynamic_cast<SoftDisplayWidget *>(m_display))
        return;
    auto *soft = new SoftDisplayWidget(m_session, this);
    m_display = soft;
    setCentralWidget(soft);   /* deletes the GPU display, which stops its feed */
    soft->setFocus();
}

void MainWindow::status(const QString &message)
{
    statusBar()->showMessage(message, 6000);
}

void MainWindow::showDebugger()
{
    /* F12 / View > Debugger: shows it, or hides it when it is up. */
    DebuggerWindow::toggleFor(this, m_session);
}

void MainWindow::restartSession()
{
    msxsession_start_opts opts;
    msxsession_stop(m_session);
    msxsession_default_opts(m_session, &opts);
    if (msxsession_start(m_session, &opts) != 0)
        status(QString::fromUtf8(msxsession_last_error(m_session)));
}

void MainWindow::showPreferences()
{
    SettingsDialog dialog(m_session, this);
    if (dialog.exec() == QDialog::Accepted && dialog.sessionDirty()) {
        restartSession();
        status(QStringLiteral("FujiNet options applied (session restarted)"));
    }
    m_display->widget()->setFocus();
}

void MainWindow::showFujiNetConfig()
{
    if (msxsession_fujinet_external(m_session)) {
        status(QStringLiteral("FujiNet is external: use its own web UI"));
        return;
    }
    fujinet_config_show(this, m_session);
}

void MainWindow::showFujiNetLog()
{
    if (msxsession_fujinet_external(m_session)) {
        status(QStringLiteral("FujiNet is external: see its own console"));
        return;
    }
    fujinet_log_show(this, m_session);
}

void MainWindow::importMedia()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Disk, Cassette or Cartridge"), QString(),
        QStringLiteral("MSX disk, cassette or cartridge images "
                       "(*.dsk *.cas *.rom);;All files (*)"));
    if (path.isEmpty())
        return;

    char dest[1024];
    if (msxsession_import_media(m_session, path.toUtf8().constData(), dest,
                                sizeof(dest)) != 0) {
        status(QString::fromUtf8(msxsession_last_error(m_session)));
        return;
    }
    status(QStringLiteral("Copied to the FujiNet SD folder. Mount it from "
                          "FujiNet Configuration."));
}

void MainWindow::buildMenus()
{
    QMenu *machineMenu = menuBar()->addMenu(QStringLiteral("&Machine"));

    m_machineGroup = new QActionGroup(this);
    m_machineGroup->setExclusive(true);
    const QByteArray current = msxsession_machine(m_session);
    for (size_t i = 0; i < std::size(kMachines); i++) {
        QAction *act = machineMenu->addAction(QString::fromUtf8(kMachines[i].label));
        act->setCheckable(true);
        act->setChecked(current == kMachines[i].id);
        m_machineGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, i] {
            msxsession_set_machine(m_session, kMachines[i].id);
            status(QStringLiteral("%1 (live switch)")
                       .arg(QString::fromUtf8(kMachines[i].label)));
            m_display->widget()->setFocus();
        });
    }

    machineMenu->addSeparator();
    machineMenu->addAction(QStringLiteral("&Reset"), this, [this] {
        msxsession_reset(m_session);
        status(QStringLiteral("Reset"));
    });
    machineMenu->addSeparator();
    machineMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, qApp,
                           &QApplication::quit);

    QMenu *media = menuBar()->addMenu(QStringLiteral("Me&dia"));
    media->addAction(QStringLiteral("&Import Disk, Cassette or Cartridge…"),
                     this, &MainWindow::importMedia);

    QMenu *fujinet = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fujinet->addAction(QStringLiteral("&Preferences…"), this,
                       &MainWindow::showPreferences);
    fujinet->addAction(QStringLiteral("&Configuration…"), this,
                       &MainWindow::showFujiNetConfig);
    fujinet->addAction(QStringLiteral("Console &Log…"), this,
                       &MainWindow::showFujiNetLog);

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12),
                    this, &MainWindow::showDebugger);

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    {
        /* works from every window of the app (the Debugger has the keyboard
         * too) */
        QAction *helpAct = help->addAction(QStringLiteral("FujiNet Go MSX &Help"));
        helpAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
        helpAct->setShortcutContext(Qt::ApplicationShortcut);
        addAction(helpAct);
        connect(helpAct, &QAction::triggered, this, [this] {
            msxHelpFor(QApplication::activeWindow() ? QApplication::activeWindow() : this);
        });
    }
    help->addSeparator();
    help->addAction(QStringLiteral("&About"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("FujiNet Go MSX"),
            QStringLiteral(
                "<b>FujiNet Go MSX</b> %1<br><br>"
                "Self-contained MSX / MSX2 / MSX2+ with built-in "
                "FujiNet.<br>"
                "Emulation by openMSX; FujiNet runs in-process over "
                "BOIP.<br><br>"
                "Copyright © 2026 Thomas Cherryhomes<br>"
                "GPL-3.0-or-later &middot; "
                "<a href=\"https://fujinet.online/\">fujinet.online</a>")
                .arg(QStringLiteral(MSX_VERSION_STRING)));
    });
}
