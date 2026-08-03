/*
 * MainWindow: menu bar over the emulator display for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QActionGroup>
#include <QMainWindow>

#include "msxsession.h"

class DisplayWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(msxsession *session, QWidget *parent = nullptr);

private:
    void buildMenus();
    void importMedia();
    void status(const QString &message);

    msxsession *m_session;
    DisplayWidget *m_display;
    QActionGroup *m_machineGroup = nullptr;
};
