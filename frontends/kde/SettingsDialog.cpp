/*
 * Preferences dialog for the KDE/Qt frontend: use the built-in FujiNet, or
 * connect to an external one at host:port.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(msxsession *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Preferences"));

    auto *fujinetBox = new QGroupBox(QStringLiteral("FujiNet"), this);
    auto *fujinetForm = new QFormLayout(fujinetBox);
    m_external = new QCheckBox(this);
    m_external->setChecked(
        msxsession_get_int(session, "fujinet_external", 0) != 0);
    fujinetForm->addRow(QStringLiteral("Use external FujiNet"), m_external);
    m_externalAddr = new QLineEdit(
        QString::fromUtf8(
            msxsession_get_str(session, "fujinet_external_addr", "")),
        this);
    m_externalAddr->setPlaceholderText(QStringLiteral("127.0.0.1:65505"));
    m_externalAddr->setEnabled(m_external->isChecked());
    connect(m_external, &QCheckBox::toggled, m_externalAddr,
            &QWidget::setEnabled);
    fujinetForm->addRow(QStringLiteral("Address:port"), m_externalAddr);
    auto *hint = new QLabel(
        QStringLiteral("Skips the built-in FujiNet and connects to another "
                       "instance that is already running. Use its [BOIP] "
                       "listen port (65505 by default). The session "
                       "restarts when you press OK."),
        this);
    hint->setWordWrap(true);
    fujinetForm->addRow(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(fujinetBox);
    layout->addWidget(buttons);
}

void SettingsDialog::accept()
{
    const QByteArray addr = m_externalAddr->text().trimmed().toUtf8();
    if (m_external->isChecked()) {
        char host[128];
        int port;
        if (msxsession_parse_hostport(addr.constData(), host, sizeof host,
                                      &port) != 0) {
            QMessageBox::warning(
                this, QStringLiteral("External FujiNet"),
                QStringLiteral("\"%1\" is not an address:port. Example: "
                               "127.0.0.1:65505")
                    .arg(m_externalAddr->text()));
            m_externalAddr->setFocus();
            return;
        }
    }

    const int external = m_external->isChecked() ? 1 : 0;
    if (msxsession_get_int(m_session, "fujinet_external", 0) != external) {
        msxsession_set_int(m_session, "fujinet_external", external);
        m_dirty = true;
    }
    if (QString::fromUtf8(
            msxsession_get_str(m_session, "fujinet_external_addr", "")) !=
        QString::fromUtf8(addr)) {
        msxsession_set_str(m_session, "fujinet_external_addr",
                           addr.constData());
        m_dirty = true;
    }
    QDialog::accept();
}
