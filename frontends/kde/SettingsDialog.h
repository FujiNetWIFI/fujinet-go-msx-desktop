/*
 * Preferences dialog for the KDE/Qt frontend: the FujiNet section. Writes
 * the same shared settings store as the GNOME frontend; the caller restarts
 * the session when accept() reports a change.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include "msxsession.h"

class QCheckBox;
class QLineEdit;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(msxsession *session, QWidget *parent = nullptr);

    /* True after accept() when the FujiNet options changed. */
    bool sessionDirty() const { return m_dirty; }

    void accept() override;

private:
    msxsession *m_session;
    QCheckBox *m_external;
    QLineEdit *m_externalAddr;
    bool m_dirty = false;
};
