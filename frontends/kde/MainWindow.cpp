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
#include "FujiNetWindows.h"
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
    resize(960, 780);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);
    statusBar();

    buildMenus();
    m_display->setFocus();
}

void MainWindow::status(const QString &message)
{
    statusBar()->showMessage(message, 6000);
}

void MainWindow::showDebugger()
{
    DebuggerWindow::showFor(this, m_session);
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
            m_display->setFocus();
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
    fujinet->addAction(QStringLiteral("&Configuration…"), this,
                       [this] { fujinet_config_show(this, m_session); });
    fujinet->addAction(QStringLiteral("Console &Log…"), this,
                       [this] { fujinet_log_show(this, m_session); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12),
                    this, &MainWindow::showDebugger);

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
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
