/*
 * The Help window (Ctrl+Shift+H): the topics of data/help/help.md, built in
 * (msxhelp.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QAbstractButton>
#include <QString>
#include <QWidget>

/* Shows the Help at id: a heading's slug or a context ("debugger/cpu",
 * "prefs/Machine"), or "?words" to search; empty stays where it was. home
 * is the main window it belongs to. */
void msxHelpShow(QWidget *home, const QString &id);

/* Opens the Help for what window shows: its "msxHelpContext" property
 * (or, in the Help, finds). */
void msxHelpFor(QWidget *window);

/* A Help button: opens context, or with an empty one its window's. */
QAbstractButton *msxHelpButton(const QString &context);
