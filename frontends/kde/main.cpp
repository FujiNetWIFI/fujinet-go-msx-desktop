/*
 * FujiNet Go MSX -- KDE/Qt frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>

#include <cstdio>

#include "MainWindow.h"
#include "msxsession.h"

int main(int argc, char *argv[])
{
    /* Vsync'd swaps for the display widget -- frameSwapped is what feeds
     * the session's (advisory) vsync notification. Shared contexts for
     * WebEngine. */
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FujiNet Go MSX"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.msx.kde"));
#ifdef MSX_ICON_FILE
    /* Dev-tree fallback; installed runs resolve the themed icon. */
    app.setWindowIcon(QIcon::fromTheme(
        QStringLiteral("online.fujinet.go.msx.kde"),
        QIcon(QStringLiteral(MSX_ICON_FILE))));
#endif

    msxsession *session = msxsession_new(nullptr);
    if (!session) {
        fprintf(stderr, "fatal: could not create the session\n");
        return 1;
    }
    msxsession_start_opts opts;
    msxsession_default_opts(session, &opts);
    if (msxsession_start(session, &opts) != 0)
        fprintf(stderr, "session start: %s\n",
                msxsession_last_error(session));

    MainWindow win(session);
    win.show();
    const int rc = app.exec();

    msxsession_free(session);
    return rc;
}
