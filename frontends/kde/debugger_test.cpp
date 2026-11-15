/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform.
 *
 * Without a machine (openMSX or its C-BIOS not available, as in CI; or
 * DBG_TEST_NO_MACHINE=1) only what needs none is checked: the twelve tabs and
 * their order, the MSX_DEBUGGER_TAB names and the older aliases, the Help
 * contexts, the minimum height, pop-out and dock, the Symbols tab filling on
 * first open. With a machine it is stopped (the boot's idle loop is a HALT, so
 * it runs to the next frame first) and a small program of NOP-padded
 * instructions is written into RAM at $C000 and stepped through the window:
 * the register strip, the Poke bar, the console, a breakpoint with an
 * expression condition, a watch, the Video and Sound tabs, the Cart & I/O text
 * and the env hooks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyleHints>
#include <QTabWidget>
#include <QTemporaryDir>

extern "C" {
#include "msxdebug.h"
#include "msxmemtools.h"
#include "msxsession.h"
}
#include "debugger/DebuggerWindow.h"
#include "debugger/VideoTab.h"

namespace {

/* In RAM at $C000, NOP-padded so a backward disassembly guess is stable:
 * NOP x3, LD HL,$1234, NOP, LD ($C100),HL, NOP x3. */
const uint8_t kProg[] = { 0x00, 0x00, 0x00, 0x21, 0x34, 0x12, 0x00, 0x22, 0x00, 0xC1, 0x00, 0x00, 0x00 };
const int kStart = 0xC000;
const int kLoad = 0xC003;

QStringList expectedTabs()
{
    return { QStringLiteral("Console"), QStringLiteral("Disassembly"), QStringLiteral("Memory"),
             QStringLiteral("Breakpoints"), QStringLiteral("Watches"), QStringLiteral("Cart & I/O"),
             QStringLiteral("Trace"), QStringLiteral("History"), QStringLiteral("Video"),
             QStringLiteral("Sound"), QStringLiteral("Symbols"), QStringLiteral("FujiNet") };
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    msxsession *m_s = nullptr;
    msxdebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    bool m_machine = false;

    static QMainWindow *findWindow()
    {
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle() == QLatin1String("Debugger")) return qobject_cast<QMainWindow *>(top);
        return nullptr;
    }
    /* the window is kept when closed: hide it (which detaches the engine) and
     * delete it, so showFor() builds a fresh one that reads the env hooks */
    void discardWindow()
    {
        if (m_win) {
            m_win->hide();
            m_win->deleteLater();
        }
        m_win = nullptr;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void openWindow()
    {
        DebuggerWindow::showFor(nullptr, m_s);
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
    }
    QString tabText(int idx) const { return m_tabs->tabText(idx).replace(QStringLiteral("&&"), QStringLiteral("&")); }
    QPushButton *button(const QString &start)
    {
        for (QPushButton *btn : m_win->findChildren<QPushButton *>())
            if (btn->text().startsWith(start)) return btn;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->placeholderText().startsWith(placeholderStart)) return edit;
        return nullptr;
    }
    QPushButton *addButton(int tab)   /* the tab's own Add */
    {
        for (QPushButton *btn : m_tabs->widget(tab)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Add")) return btn;
        return nullptr;
    }
    QList<QLineEdit *> regFields()
    {
        QList<QLineEdit *> out;
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->toolTip().startsWith(QLatin1String("Enter sets it"))) out << edit;
        return out;
    }
    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(msxdebug_is_stopped(m_d), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Run (F5)")) != nullptr, 3000);
    }
    int readPc()
    {
        msxdebug_cpu cpu;
        msxdebug_cpu_get(m_d, &cpu);
        return cpu.pc;
    }
    /* Page 3 is RAM only once the BIOS has set its slots up, and the boot's
     * idle loop is a HALT that keeps a CPU halted when its PC is written: run
     * to the next frame first. Interrupts are off so none lands in a step. */
    bool loadProgram()
    {
        uint8_t back[sizeof kProg];
        for (size_t idx = 0; idx < sizeof kProg; ++idx) msxdebug_write(m_d, uint16_t(kStart + idx), kProg[idx]);
        if (msxdebug_read(m_d, kStart, back, int(sizeof kProg)) != int(sizeof kProg) || memcmp(back, kProg, sizeof kProg)) return false;
        msxdebug_cpu_set(m_d, MSX_REG_SP, 0xF000);
        msxdebug_cpu_set(m_d, MSX_REG_IFF1, 0);
        msxdebug_cpu_set(m_d, MSX_REG_IFF2, 0);
        msxdebug_cpu_set(m_d, MSX_REG_PC, kStart);
        return readPc() == kStart;
    }
    QString consoleText()
    {
        auto *view = m_tabs->widget(0)->findChild<QPlainTextEdit *>();
        return view ? view->toPlainText() : QString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_DARK")) QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
        msxsession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        m_s = msxsession_new(&paths);
        QVERIFY(m_s);
        m_d = msxsession_debugger(m_s);
        QVERIFY(m_d);

        if (!qEnvironmentVariableIsSet("DBG_TEST_NO_MACHINE")) {
            msxsession_start_opts opts;
            msxsession_default_opts(m_s, &opts);
            opts.enable_fujinet = 0;
            opts.machine = MSXSESSION_MACHINE_MSX;
            m_machine = msxsession_start(m_s, &opts) == 0;
            if (m_machine) {
                /* the BIOS needs a couple of seconds to set its slots up */
                for (int waited = 0; waited < 15000 && msxsession_frame_count(m_s) < 100; waited += 50) QTest::qWait(50);
                m_machine = msxsession_frame_count(m_s) >= 100;
            }
        }

        /* the window opens on the tab MSX_DEBUGGER_TAB names (the older "vdp" is the Video tab) */
        qputenv("MSX_DEBUGGER_TAB", "vdp");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("MSX_DEBUGGER_TAB");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
    }

    void cleanupTestCase()
    {
        if (m_win) m_win->hide();
        QCoreApplication::processEvents();
        if (m_s) {
            msxsession_stop(m_s);
            msxsession_free(m_s);
        }
    }

    void tabsInOrder()
    {
        QStringList names;
        for (int idx = 0; idx < m_tabs->count(); ++idx) names << tabText(idx);
        QCOMPARE(names, expectedTabs());
        QCOMPARE(m_tabs->currentIndex(), 8);   /* "vdp" is the Video tab */
        QVERIFY(m_win->findChild<VideoTab *>() != nullptr);
    }

    void tabsByName()
    {
        /* each name the env hook takes, in a window of its own */
        discardWindow();
        struct { const char *name; int index; } cases[] = {
            { "cpu", 2 }, { "registers", 2 }, { "io", 5 }, { "slots", 5 }, { "CART", 5 }, { "3", 3 },
            { "Sound", 9 }, { "console", 0 }, { "hist", 7 }, { "sym", 10 } };
        for (const auto &one : cases) {
            qputenv("MSX_DEBUGGER_TAB", one.name);
            DebuggerWindow::showFor(nullptr, m_s);
            qunsetenv("MSX_DEBUGGER_TAB");
            QMainWindow *win = findWindow();
            QVERIFY(win);
            QVERIFY2(win->findChild<QTabWidget *>()->currentIndex() == one.index, one.name);
            m_win = win;
            discardWindow();
        }
        openWindow();
    }

    /* DBG_TEST_SHOTS=<dir>: a PNG of every tab (DBG_TEST_DARK=1: in the dark scheme) */
    void shots()
    {
        const QString dir = qEnvironmentVariable("DBG_TEST_SHOTS");
        if (dir.isEmpty()) QSKIP("DBG_TEST_SHOTS not set");
        for (int idx = 0; idx < m_tabs->count(); ++idx) {
            m_tabs->setCurrentIndex(idx);
            QTest::qWait(250);
            QVERIFY(m_win->grab().save(QStringLiteral("%1/tab%2.png").arg(dir).arg(idx)));
        }
    }

    void helpContexts()
    {
        static const char *const kContexts[] = { "console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                                 "trace", "history", "video", "sound", "symbols", "fujinet" };
        for (int idx = 0; idx < 12; ++idx) {
            m_tabs->setCurrentIndex(idx);
            QCOMPARE(m_win->property("msxHelpContext").toString(),
                     QStringLiteral("debugger/%1").arg(QLatin1String(kContexts[idx])));
        }
    }

    void fitsASmallScreen()
    {
        QVERIFY2(m_win->minimumSizeHint().height() <= 800,
                 qPrintable(QStringLiteral("minimum height %1").arg(m_win->minimumSizeHint().height())));
    }

    /* the built-in BIOS names fill the list the first time the tab opens */
    void symbolsFillOnFirstOpen()
    {
        m_tabs->setCurrentIndex(10);
        QListWidget *list = m_tabs->widget(10)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(list->count() > 0, 2000);
        QLineEdit *filter = m_tabs->widget(10)->findChild<QLineEdit *>();
        QVERIFY(filter);
        const int all = list->count();
        filter->setText(QStringLiteral("CHPUT"));
        QVERIFY(list->count() > 0 && list->count() < all);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("CHPUT")));
        filter->clear();
        QCOMPARE(list->count(), all);
    }

    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(9);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        pop->click();
        QCOMPARE(m_tabs->count(), 11);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        for (QPushButton *btn : window->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Dock")) btn->click();
        QCOMPARE(m_tabs->count(), 12);
        QCOMPARE(tabText(9), QStringLiteral("Sound"));
    }

    void consoleAndRegisters()
    {
        if (!m_machine) QSKIP("needs a machine (openMSX and C-BIOS)");
        if (!msxdebug_is_stopped(m_d)) msxdebug_stop(m_d);
        waitStopped();
        msxdebug_frame(m_d);       /* leave the boot's HALT */
        waitStopped();
        QVERIFY2(loadProgram(), "page 3 is not RAM: the BIOS has not set up its slots");

        /* the console runs the core's prompt */
        m_tabs->setCurrentIndex(0);
        QLineEdit *prompt = entry(QStringLiteral("command"));
        QVERIFY(prompt);
        prompt->setText(QStringLiteral("print $10 + 2"));
        QTest::keyClick(prompt, Qt::Key_Return);
        QTRY_VERIFY(consoleText().contains(QStringLiteral("$0012")));
        prompt->setText(QStringLiteral("regs"));
        QTest::keyClick(prompt, Qt::Key_Return);
        QVERIFY(consoleText().contains(QStringLiteral("HL")));

        /* step the NOPs, then the LD HL, with the toolbar button */
        QCOMPARE(readPc(), kStart);
        QPushButton *step = button(QStringLiteral("Step ("));
        QVERIFY(step);
        for (int idx = 1; idx <= 3; ++idx) {
            step->click();
            QTRY_COMPARE(readPc(), kStart + idx);
        }
        QCOMPARE(readPc(), kLoad);
        step->click();
        QTRY_COMPARE(readPc(), kLoad + 3);
        msxdebug_cpu cpu;
        msxdebug_cpu_get(m_d, &cpu);
        QCOMPARE(cpu.hl, 0x1234);

        /* the strip: HL takes hex, DE an expression, only while stopped */
        const QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 15);
        regs[3]->setText(QStringLiteral("abcd"));
        QTest::keyClick(regs[3], Qt::Key_Return);
        msxdebug_cpu_get(m_d, &cpu);
        QCOMPARE(cpu.hl, 0xABCD);
        regs[2]->setText(QStringLiteral("hl + 1"));
        QTest::keyClick(regs[2], Qt::Key_Return);
        msxdebug_cpu_get(m_d, &cpu);
        QCOMPARE(cpu.de, 0xABCE);
        /* IM above 2 is refused by the engine and leaves it alone */
        const int imBefore = cpu.im;
        regs[14]->setText(QStringLiteral("7"));
        QTest::keyClick(regs[14], Qt::Key_Return);
        msxdebug_cpu_get(m_d, &cpu);
        QCOMPARE(cpu.im, imBefore);
        /* a flag check box and IFF1 */
        QCheckBox *zero = nullptr, *iff1 = nullptr;
        for (QCheckBox *box : m_win->findChildren<QCheckBox *>()) {
            if (box->text() == QLatin1String("Z")) zero = box;
            if (box->text() == QLatin1String("IFF1")) iff1 = box;
        }
        QVERIFY(zero && iff1);
        msxdebug_cpu_get(m_d, &cpu);
        zero->click();
        const int zeroNow = cpu.zf;
        msxdebug_cpu_get(m_d, &cpu);
        QCOMPARE(cpu.zf, zeroNow ? 0 : 1);
        iff1->click();
        msxdebug_cpu_get(m_d, &cpu);
        QCOMPARE(cpu.iff1, 1);
        iff1->click();
        msxdebug_cpu_get(m_d, &cpu);
        QCOMPARE(cpu.iff1, 0);
        QVERIFY(loadProgram());
    }

    void pokeAndSearch()
    {
        if (!m_machine) QSKIP("needs a machine (openMSX and C-BIOS)");
        QVERIFY(msxdebug_is_stopped(m_d));
        m_tabs->setCurrentIndex(2);
        QLineEdit *addr = entry(QStringLiteral("address or label"));
        QLineEdit *val = entry(QStringLiteral("value"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("$C200"));
        val->setText(QStringLiteral("$5A"));
        QTest::keyClick(val, Qt::Key_Return);
        uint8_t byte = 0;
        msxdebug_read(m_d, 0xC200, &byte, 1);
        QCOMPARE(int(byte), 0x5A);
        /* a value that is not a byte is refused */
        val->setText(QStringLiteral("$1FF"));
        QTest::keyClick(val, Qt::Key_Return);
        msxdebug_read(m_d, 0xC200, &byte, 1);
        QCOMPARE(int(byte), 0x5A);
        /* Go to marks the row, and search finds the program's LD HL */
        QLineEdit *gotoBox = entry(QStringLiteral("$C000, label"));
        QVERIFY(gotoBox);
        gotoBox->setText(QStringLiteral("$C200"));
        QTest::keyClick(gotoBox, Qt::Key_Return);
        QVERIFY(!m_tabs->widget(2)->findChildren<QPlainTextEdit *>().isEmpty());
        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        find->setText(QStringLiteral("21 34 12 00 22"));
        QTest::keyClick(find, Qt::Key_Return);
        QLabel *note = nullptr;
        for (QLabel *label : m_tabs->widget(2)->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("Found")) || label->text().startsWith(QLatin1String("Not found"))) note = label;
        QVERIFY(note);
        QVERIFY2(note->text().contains(QStringLiteral("C003")), qPrintable(note->text()));
    }

    void breakpointWithCondition()
    {
        if (!m_machine) QSKIP("needs a machine (openMSX and C-BIOS)");
        msxdebug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(3);
        QLineEdit *addr = entry(QStringLiteral("start ("));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(addr && cond);
        addr->setText(QStringLiteral("$C100"));
        cond->setText(QStringLiteral("value == $34"));
        QComboBox *type = m_tabs->widget(3)->findChild<QComboBox *>();
        QVERIFY(type);
        QCOMPARE(type->itemText(3), QStringLiteral("Access"));
        type->setCurrentIndex(2);   /* Write */
        addButton(3)->click();
        msxdebug_breakpoint bps[4];
        QCOMPARE(msxdebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(bps[0].type, int(MSXDEBUG_BP_WRITE));
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value == $34"));

        /* a condition that does not parse is refused */
        addr->setText(QStringLiteral("$C101"));
        cond->setText(QStringLiteral("value =="));
        addButton(3)->click();
        QCOMPARE(msxdebug_breakpoint_list(m_d, bps, 4), 1);

        /* an Access entry is one row */
        addr->setText(QStringLiteral("$C300"));
        cond->clear();
        type->setCurrentIndex(3);
        addButton(3)->click();
        QCOMPARE(msxdebug_breakpoint_list(m_d, bps, 4), 2);
        QCOMPARE(bps[1].type, int(MSXDEBUG_BP_ACCESS));
        msxdebug_breakpoint_remove(m_d, bps[1].id);

        /* run the program: it stops after the store, the byte in the core's words */
        QVERIFY(loadProgram());
        msxdebug_resume(m_d);
        QTRY_VERIFY_WITH_TIMEOUT(msxdebug_is_stopped(m_d), 4000);
        msxdebug_snapshot snap;
        msxdebug_stop_snapshot(m_d, &snap);
        QVERIFY2(QString::fromUtf8(snap.text).contains(QStringLiteral("$34")), snap.text);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Run (F5)")) != nullptr, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("$34")), 2000);
        msxdebug_breakpoint_clear(m_d);
    }

    void watchesAndBreakOnChange()
    {
        if (!m_machine) QSKIP("needs a machine (openMSX and C-BIOS)");
        QVERIFY(msxdebug_is_stopped(m_d));
        msxwatch_clear();
        m_tabs->setCurrentIndex(4);
        QLineEdit *entryBox = m_tabs->widget(4)->findChild<QLineEdit *>();
        QVERIFY(entryBox);
        entryBox->setText(QStringLiteral("$C100"));
        QTest::keyClick(entryBox, Qt::Key_Return);
        QCOMPARE(msxwatch_count(), 1);
        QListWidget *list = m_tabs->widget(4)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("C100")));
        list->setCurrentRow(0);
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Break on Change")) btn->click();
        msxdebug_breakpoint bps[4];
        QTRY_COMPARE(msxdebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        msxdebug_breakpoint_clear(m_d);
        msxwatch_clear();
    }

    void traceAndHistory()
    {
        if (!m_machine) QSKIP("needs a machine (openMSX and C-BIOS)");
        QVERIFY(msxdebug_is_stopped(m_d));
        m_tabs->setCurrentIndex(7);
        QPushButton *record = nullptr;
        for (QPushButton *btn : m_tabs->widget(7)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Record")) record = btn;
        QVERIFY(record);
        record->click();
        QVERIFY(msxdebug_history_enabled(m_d));
        QVERIFY(loadProgram());
        for (int idx = 0; idx < 4; ++idx) {
            msxdebug_step(m_d);
            QTest::qWait(60);
        }
        QTRY_VERIFY_WITH_TIMEOUT(msxdebug_history_count(m_d) >= 4, 3000);
        m_tabs->setCurrentIndex(0);
        m_tabs->setCurrentIndex(7);
        auto *text = m_tabs->widget(7)->findChild<QPlainTextEdit *>();
        QVERIFY(text);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("C00")), 3000);
        record->click();
        QVERIFY(!msxdebug_history_enabled(m_d));
        msxdebug_history_clear(m_d);
    }

    void videoSoundAndCart()
    {
        if (!m_machine) QSKIP("needs a machine (openMSX and C-BIOS)");
        QVERIFY(msxdebug_is_stopped(m_d));
        m_tabs->setCurrentIndex(8);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        video->refresh(true);
        QVERIFY(video->findChild<QComboBox *>() != nullptr);
        m_tabs->setCurrentIndex(9);
        auto soundText = [this] {
            auto *view = qobject_cast<QPlainTextEdit *>(m_tabs->widget(9));
            return view ? view->toPlainText() : QString();
        };
        QTRY_VERIFY2_WITH_TIMEOUT(soundText().contains(QStringLiteral("PSG")), qPrintable(soundText()), 3000);
        m_tabs->setCurrentIndex(5);
        QString ioText;
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(5)->findChildren<QPlainTextEdit *>().isEmpty(), 2000);
        for (QPlainTextEdit *view : m_tabs->widget(5)->findChildren<QPlainTextEdit *>()) ioText += view->toPlainText();
        QVERIFY2(ioText.contains(QStringLiteral("Memory map")), qPrintable(ioText));
        QVERIFY(ioText.contains(QStringLiteral("Selected slots")));
        QVERIFY(ioText.contains(QStringLiteral("PPI")));
    }

    /* MSX_WATCH, MSX_WATCH_BREAK, MSX_MEM_GOTO and MSX_HISTORY act in a fresh window */
    void envHooks()
    {
        if (!m_machine) QSKIP("needs a machine (openMSX and C-BIOS)");
        msxdebug_breakpoint_clear(m_d);
        msxwatch_clear();
        discardWindow();
        qputenv("MSX_DEBUGGER_TAB", "watches");
        qputenv("MSX_WATCH", "$C300");
        qputenv("MSX_WATCH_BREAK", "$C300");
        qputenv("MSX_MEM_GOTO", "$C300");
        qputenv("MSX_HISTORY", "1");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("MSX_DEBUGGER_TAB");
        qunsetenv("MSX_WATCH");
        qunsetenv("MSX_WATCH_BREAK");
        qunsetenv("MSX_MEM_GOTO");
        qunsetenv("MSX_HISTORY");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(msxwatch_count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(msxdebug_history_enabled(m_d), 3000);
        msxdebug_breakpoint bps[4];
        QTRY_COMPARE_WITH_TIMEOUT(msxdebug_breakpoint_list(m_d, bps, 4), 1, 3000);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        /* MEM_GOTO put its address in the Memory view (the tab stays the one asked for) */
        QCOMPARE(m_tabs->currentIndex(), 4);
        m_tabs->setCurrentIndex(2);
        auto *dump = m_tabs->widget(2)->findChildren<QPlainTextEdit *>().last();
        QTRY_VERIFY_WITH_TIMEOUT(dump->toPlainText().contains(QStringLiteral("$C300:")), 3000);
        msxdebug_history_enable(m_d, 0);
        msxdebug_breakpoint_clear(m_d);
        msxwatch_clear();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
