/*
 * Debugger window (Qt6 Widgets) over the Z80 / VDP debugger engine, via
 * core/include/msxdebug.h and msxvidtab.h. Mirrors the GNOME one tab for tab,
 * and follows the INTV KDE debugger (the family's newest): twelve tabs with
 * the editable register strip under every one.
 *
 * Tabs: the Console (the engine's command line, with a history and Tab
 * completion), the Disassembly (click a line to select it for Run to Cursor;
 * click its left edge, double-click it or press F9 for its breakpoint),
 * Memory (the 64K the CPU sees, Go to, Poke, search), Breakpoints (execute,
 * read, write, access, port IN and OUT, with an expression condition),
 * Watches, Cart & I/O (the slots, mapper, PPI, keyboard, joysticks and the
 * memory map), Trace (live) and History (what ran before the stop), Video
 * (VideoTab over the core's msxvidtab.h), Sound, Symbols and FujiNet (the
 * FujiBus packets on the cartridge's socket). The toolbar carries the
 * stepping controls, symbol loading, Save and Pop Out; the family's keys
 * apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7
 * frame, F9 breakpoint, F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches, which takes the breakpoints out of openMSX
 * and lets the machine run on.
 *
 * A short timer polls the engine's stop snapshot (msxdebug_set_stop_callback
 * fires off the UI thread, so it is not used) and refreshes the tabs showing
 * when it moved; while the machine runs, the live tabs refresh twice a
 * second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "../help/HelpWindow.h"
#include "Accent.h"
#include "VideoTab.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

extern "C" {
#include "msxmemtools.h"
}

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 256
#define TRACE_MAX 512
#define CONSOLE_LINES 5000
#define HIST_FETCH 4000         /* ring entries scanned for a Trace / History view */
#define HIST_SHOWN 400          /* matching lines shown */

namespace {

enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY,
       TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names MSX_DEBUGGER_TAB and MSX_POPOUT take, and their
 * labels (which they take too); the Help context of each is its name. */
const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "cart", "Cart & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "fujinet", "FujiNet" },
};

/* The older names of the tabs that were folded in. */
const struct { const char *name; int tab; } kTabAliases[] = {
    { "cpu", TAB_MEMORY }, { "registers", TAB_MEMORY }, { "vdp", TAB_VIDEO }, { "io", TAB_CART }, { "slots", TAB_CART },
};

/* The register entries, in the order shown, and the msxdebug_reg each sets.
 * Pairs show four digits, I and R two, IM bare. */
const struct { const char *name; int id, digits; } kRegs[15] = {
    { "AF", MSX_REG_AF, 4 },   { "BC", MSX_REG_BC, 4 },   { "DE", MSX_REG_DE, 4 },   { "HL", MSX_REG_HL, 4 },
    { "IX", MSX_REG_IX, 4 },   { "IY", MSX_REG_IY, 4 },   { "SP", MSX_REG_SP, 4 },   { "PC", MSX_REG_PC, 4 },
    { "AF'", MSX_REG_AF2, 4 }, { "BC'", MSX_REG_BC2, 4 }, { "DE'", MSX_REG_DE2, 4 }, { "HL'", MSX_REG_HL2, 4 },
    { "I", MSX_REG_I, 2 },     { "R", MSX_REG_R, 2 },     { "IM", MSX_REG_IM, 0 },
};

/* The flags: their bit in F. */
const struct { const char *name; int mask; } kFlags[6] = {
    { "S", 0x80 }, { "Z", 0x40 }, { "H", 0x10 }, { "PV", 0x04 }, { "N", 0x02 }, { "C", 0x01 },
};

const QString kGray = QStringLiteral("color: gray;");

/* The accent: the Run button while stopped, the PC's line, a marked row. On a
 * dark theme the light variant with black ink (Accent.h). */
QTextCharFormat accentFormat(const QWidget *wid)
{
    QTextCharFormat fmt;
    fmt.setBackground(msxAccentFill(wid));
    fmt.setForeground(msxAccentFillInk(wid));
    return fmt;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A whole line of a view in a format: the accent fill, a mark. */
QTextEdit::ExtraSelection lineSelection(QPlainTextEdit *v, int line, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    s.cursor = QTextCursor(v->document()->findBlockByNumber(line));
    s.format = fmt;
    s.format.setProperty(QTextFormat::FullWidthSelection, true);
    return s;
}

/* A status line that elides instead of widening the window. */
class ElidedLabel : public QLabel {
public:
    ElidedLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        p.setPen(palette().color(foregroundRole()));
        p.drawText(r, (alignment() & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter,
                   fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
    }
};

/* MSX_DEBUGGER_TAB and MSX_POPOUT: a page number, or a tab's name or label
 * (any case), or the start of one, or of a word of a label ("memory"), or an
 * older name (cpu, vdp, io ...). */
int tabFromEnv(const char *variable = "MSX_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(variable).trimmed();
    if (tab.isEmpty()) return -1;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (const auto &alias : kTabAliases)
        if (tab.compare(QString::fromUtf8(alias.name), Qt::CaseInsensitive) == 0) return alias.tab;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The label a tab shows: "&" starts a mnemonic in tab text, so it is doubled. */
QString tabLabel(int tab)
{
    return QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&"));
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

/* A tone as a note: "A4 (+3 cents)". */
QString noteName(double hertz)
{
    static const char *const kNotes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (hertz < 8.0) return QStringLiteral("-");
    const double midi = 69.0 + 12.0 * std::log2(hertz / 440.0);
    const long note = std::lround(midi);
    return QStringLiteral("%1 Hz  %2%3 (%4 cents)")
        .arg(hertz, 0, 'f', 1)
        .arg(QLatin1String(kNotes[((note % 12) + 12) % 12]))
        .arg(note / 12 - 1)
        .arg(std::lround((midi - double(note)) * 100.0));
}

/* openMSX names a device with spaces as a Tcl list element: "{Main RAM}". */
QString unbrace(const char *name)
{
    QString text = QString::fromUtf8(name);
    text.remove(QLatin1Char('{')).remove(QLatin1Char('}'));
    return text;
}

QString onOff(int value) { return value ? QStringLiteral("on ") : QStringLiteral("off"); }
QString yesNo(int value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); }

/* A tab's window: closing it docks the tab again. The family's keys work in
 * it too, handed to the debugger. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
    std::function<bool(QKeyEvent *)> onKey;
protected:
    void closeEvent(QCloseEvent *evt) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(evt);
    }
    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(evt))) {
            evt->accept();
            return true;
        }
        return QWidget::event(evt);
    }
    void keyPressEvent(QKeyEvent *evt) override
    {
        if (!onKey || !onKey(evt)) QWidget::keyPressEvent(evt);
    }
};

QPointer<DebuggerWindow> s_win;

} // namespace

void DebuggerWindow::showFor(QWidget *parent, msxsession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showEvent attaches, which stops the machine */
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
}

void DebuggerWindow::toggleFor(QWidget *parent, msxsession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(msxsession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(msxsession_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1150, 800);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    QWidget *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = buildConsole();
    pages[TAB_DISASM] = buildDisasm();
    pages[TAB_MEMORY] = buildMemory();
    pages[TAB_BREAKS] = buildBreaks();
    pages[TAB_WATCHES] = buildWatches();
    pages[TAB_CART] = buildCart();
    pages[TAB_TRACE] = buildTrace(0);
    pages[TAB_HISTORY] = buildTrace(1);
    pages[TAB_VIDEO] = buildVideo();
    pages[TAB_SOUND] = buildSound();
    pages[TAB_SYMBOLS] = buildSymbols();
    pages[TAB_FUJINET] = buildFujinet();
    for (int idx = 0; idx < TAB_COUNT; ++idx) {
        pages[idx]->setProperty("tab", idx);     /* each page keeps its identity when tabs move */
        m_pages[idx] = pages[idx];
        m_tabs->addTab(pages[idx], tabLabel(idx));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts (msxhelp.h), one per tab */
    auto setHelpContext = [this]() {
        const int page = currentTab();
        if (page >= 0 && page < TAB_COUNT)
            setProperty("msxHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].name)));
    };
    setHelpContext();
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { msxHelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        setHelpContext();
        /* the view has no size until it shows: fill it now */
        m_slowNow = true;
        refreshTab(currentTab(), false);
        m_slowNow = false;
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    m_envWatch = qgetenv("MSX_WATCH");
    m_envWatchBreak = qgetenv("MSX_WATCH_BREAK");
    m_envMemGoto = qgetenv("MSX_MEM_GOTO");
    m_envHistory = qEnvironmentVariableIsSet("MSX_HISTORY");
    const int popTab = tabFromEnv("MSX_POPOUT");
    if (popTab >= 0) popOut(popTab);     /* for captures: peel a tab off at the start */

    m_slowTimer.setSingleShot(true);
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { refreshSlow(); });
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_timer.start(100);
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"), [this] { stepAnd(msxdebug_step); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a CALL, HALT or block instruction"),
        [this] { stepAnd(msxdebug_step_over); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On to the RET"),
        [this] { stepAnd(msxdebug_step_out); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Frame (Shift+F7)"), QStringLiteral("On to the next VDP interrupt"),
        [this] { stepAnd(msxdebug_frame); });
    h->addStretch(1);

    auto *sym = new QPushButton(QStringLiteral("Symbols"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from a .sym file (HHHH NAME per line)"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Load symbols…"), this, [this] { loadSymbols(); });
    symMenu->addAction(QStringLiteral("Clear loaded symbols"), this, [this] {
        msxdebug_clear_symbols(m_dbg);
        refreshSymbols(true);
        resync();
    });
    sym->setMenu(symMenu);
    h->addWidget(sym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    menu->addAction(QStringLiteral("Disassembly ($0000-$FFFF as mapped)"), this,
                    [this] { save("dis", QStringLiteral("Disassembly"), QStringLiteral("disassembly.asm")); });
    menu->addAction(QStringLiteral("Memory (the 64K the CPU sees)"), this,
                    [this] { save("mem", QStringLiteral("Memory"), QStringLiteral("memory.bin")); });
    menu->addAction(QStringLiteral("VRAM"), this,
                    [this] { save("vram", QStringLiteral("VRAM"), QStringLiteral("vram.bin")); });
    menu->addAction(QStringLiteral("CPU registers"), this,
                    [this] { save("regs", QStringLiteral("CPU registers"), QStringLiteral("registers.txt")); });
    saveBtn->setMenu(menu);
    h->addWidget(saveBtn);
    add(QStringLiteral("Pop Out"), QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"),
        [this] { popOut(currentTab()); });
    h->addWidget(msxHelpButton(QString()));

    /* the status line sits in the status bar: the toolbar has no room for it */
    m_status = new ElidedLabel;
    m_status->setAlignment(Qt::AlignLeft);
    statusBar()->addWidget(m_status, 1);
    return bar;
}

/* The Z80's registers, under every tab. Enter sets one (only while stopped);
 * a flag or IFF is a check box. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *box = new QGroupBox(QStringLiteral("Registers"));
    auto *regs = new QGridLayout(box);
    regs->setVerticalSpacing(2);
    regs->setHorizontalSpacing(8);
    /* the main set on the first row, the shadow set and the rest on the second */
    for (int idx = 0; idx < 15; ++idx) {
        const int row = idx < 8 ? 0 : 1, col = 2 * (idx < 8 ? idx : idx - 8);
        auto *nameLabel = new QLabel(QString::fromUtf8(kRegs[idx].name));
        nameLabel->setStyleSheet(kGray);
        regs->addWidget(nameLabel, row, col, Qt::AlignRight);
        m_reg[idx] = new QLineEdit;
        setMono(m_reg[idx]);
        m_reg[idx]->setMaxLength(24);
        setDigits(m_reg[idx], kRegs[idx].digits ? kRegs[idx].digits : 2);
        m_reg[idx]->setToolTip(QStringLiteral("Enter sets it (a bare number is hex; an expression such as hl + 1 works; "
                                            "only while stopped)"));
        connect(m_reg[idx], &QLineEdit::returnPressed, this, [this, idx] { setReg(idx); });
        regs->addWidget(m_reg[idx], row, col + 1);
    }

    auto *flags = new QHBoxLayout;
    for (int idx = 0; idx < 6; ++idx) {
        m_flag[idx] = new QCheckBox(QString::fromUtf8(kFlags[idx].name));
        m_flag[idx]->setToolTip(QStringLiteral("Flag %1 of F (only while stopped)").arg(QString::fromUtf8(kFlags[idx].name)));
        connect(m_flag[idx], &QCheckBox::clicked, this, [this, idx](bool enabled) { setFlag(kFlags[idx].mask, enabled); });
        flags->addWidget(m_flag[idx]);
    }
    flags->addSpacing(16);
    for (int idx = 0; idx < 2; ++idx) {
        m_iff[idx] = new QCheckBox(QStringLiteral("IFF%1").arg(idx + 1));
        connect(m_iff[idx], &QCheckBox::clicked, this, [this, idx](bool enabled) { setIff(idx, enabled); });
        flags->addWidget(m_iff[idx]);
    }
    m_haltNote = new QLabel;
    m_haltNote->setStyleSheet(kGray);
    flags->addSpacing(16);
    flags->addWidget(m_haltNote);
    flags->addStretch(1);
    regs->addLayout(flags, 2, 0, 1, 16);
    regs->setColumnStretch(16, 1);
    return box;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command; anything else is a Tcl "
                                          "command for openMSX. Numbers: $C000 or 0xC000 for hex, bare digits are "
                                          "decimal.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, bpa, print, mem, poke, runto, disasm, "
                                               "watch, find, video, cart, slotmap ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

/* The dump: Go to, a page back or on, Poke, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$C000, label or lo-hi"), 22);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *arrow = new QToolButton;
        arrow->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        arrow->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(arrow, &QToolButton::clicked, this, [this, dir] {
            m_memTop = qBound(0, m_memTop + dir * MEM_ROWS * 16, 0x10000 - MEM_ROWS * 16);
            refreshMem();
        });
        nav->addWidget(arrow);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("address or label"), 16);
    m_memVal = smallEntry(QStringLiteral("value"), 10);
    m_memVal->setToolTip(QStringLiteral("A byte: hex, or an expression. Only while stopped; ROM ignores it."));
    auto *pokeBtn = new QPushButton(QStringLiteral("Poke"));
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(pokeBtn, &QPushButton::clicked, this, [this] { memPoke(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(m_memVal);
    nav->addWidget(pokeBtn);
    nav->addStretch(1);
    layout->addLayout(nav);

    auto *search = new QHBoxLayout;
    search->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex bytes"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (3E 01) or text, one byte per character"));
    search->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    search->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    search->addWidget(next);
    search->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    search->addWidget(m_srchNote);
    layout->addLayout(search);

    layout->addWidget(titleLabel(QStringLiteral("Memory (as the CPU sees it through the slot map)")));
    m_mem = monoView();
    layout->addWidget(m_mem, 1);
    return page;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    auto *top = new QHBoxLayout;
    top->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                            "access. Breakpoints are kept while the debugger is closed, but only "
                                            "act while it is open.")), 1);
    v->addLayout(top);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Type"), QStringLiteral("Range"),
                                           QStringLiteral("Condition"), QStringLiteral("Hits"), QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        msxdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("Access"), QStringLiteral("In"), QStringLiteral("Out") });
    m_bpType->setToolTip(QStringLiteral("Access is a read or a write of the range; In and Out take a port ($00-$FF)"));
    m_bpStart = smallEntry(QStringLiteral("start ($4000 or label)"), 22);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional): value != old, a == 5, [$C000] > 3"));
    m_bpCond->setToolTip(QStringLiteral(
        "An expression: numbers ($C000), the Z80 registers (a hl ix af' ...), flags (zf cf ...), labels, [addr] "
        "(a byte) and {addr} (a word), frame, cycles, vline, and for an access value, addr and old (the byte a "
        "write replaced). An openMSX Tcl expression ([reg A] == 5) works too. Empty stops every time."));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clearHits, &QPushButton::clicked, this, [this] {
        msxdebug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        msxdebug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(kGray);
    v->addWidget(m_bpStop);
    return w;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QHBoxLayout;
    form->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or first-last"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(); });
    form->addWidget(m_wtEntry, 1);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(); });
    form->addWidget(add);
    layout->addLayout(form);

    m_wtList = new QListWidget;
    setMono(m_wtList);
    layout->addWidget(m_wtList, 1);

    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *mem = new QPushButton(QStringLiteral("Memory"));
    mem->setToolTip(QStringLiteral("Show the selected watch in the Memory tab"));
    connect(mem, &QPushButton::clicked, this, [this] {
        uint16_t first, last;
        if (m_wtList->currentRow() >= 0 && msxwatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) {
            showTab(TAB_MEMORY);
            showInMemory(first, last);
        }
    });
    auto *brk = new QPushButton(QStringLiteral("Break on Change"));
    brk->setToolTip(QStringLiteral("A write breakpoint on it with the condition value != old"));
    connect(brk, &QPushButton::clicked, this, [this] {
        uint16_t first, last;
        if (m_wtList->currentRow() < 0 || !msxwatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) return;
        char err[200] = "";
        if (msxdebug_breakpoint_add(m_dbg, MSXDEBUG_BP_WRITE, first, last, "value != old", err, sizeof err) >= 0)
            message(QStringLiteral("Breaking when it changes"));
        else
            message(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        syncLater();
    });
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_wtList->currentRow() >= 0) {
            msxwatch_remove(m_wtList->currentRow());
            syncLater();
        }
    });
    foot->addWidget(mem);
    foot->addWidget(brk);
    foot->addWidget(remove);
    layout->addLayout(foot);
    return page;
}

/* The machine above (cart, memory map), the I/O below (slots, mapper, PPI,
 * keyboard, joysticks). */
QWidget *DebuggerWindow::buildCart()
{
    m_cartText = monoView();
    m_ioText = monoView();
    m_cartText->setLineWrapMode(QPlainTextEdit::WidgetWidth);      /* the device list is long */
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_cartText);
    split->addWidget(m_ioText);
    split->setChildrenCollapsible(false);
    split->setSizes({ 300, 300 });
    return split;
}

/* Trace (0) and History (1): a Record switch, a filter, a note and the text.
 * Trace follows the machine live; History is what ran before the stop. */
QWidget *DebuggerWindow::buildTrace(int which)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;

    m_hRec[which] = new QPushButton(QStringLiteral("Record"));
    m_hRec[which]->setCheckable(true);
    m_hRec[which]->setStyleSheet(QStringLiteral("QPushButton:checked { background: %1; color: %2; }")
                                     .arg(msxAccentFill(m_hRec[which]).name(), msxAccentFillInk(m_hRec[which]).name()));
    m_hRec[which]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    connect(m_hRec[which], &QPushButton::toggled, this, [this, which](bool enabled) {
        if (m_hSync) return;
        if (which == 0) msxdebug_trace_enable(m_dbg, enabled);
        else msxdebug_history_enable(m_dbg, enabled);
        syncLater();
    });
    m_hFilter[which] = new QComboBox;
    m_hFilter[which]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                                 QStringLiteral("In range") });
    m_hRange[which] = new QLineEdit;
    m_hRange[which]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    auto changed = [this] {
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    };
    connect(m_hFilter[which], &QComboBox::currentIndexChanged, this, changed);
    connect(m_hRange[which], &QLineEdit::returnPressed, this, changed);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this, which] {
        if (which == 0) msxdebug_trace_clear(m_dbg);
        else msxdebug_history_clear(m_dbg);
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    });
    m_hNote[which] = new QLabel;
    m_hNote[which]->setStyleSheet(kGray);
    m_hNote[which]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[which]);
    bar->addWidget(m_hFilter[which]);
    bar->addWidget(m_hRange[which], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[which], 1);
    layout->addLayout(bar);
    m_hText[which] = monoView();
    layout->addWidget(m_hText[which], 1);
    return page;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_dbg);
    return m_video;
}

QWidget *DebuggerWindow::buildSound()
{
    m_soundText = monoView();
    return m_soundText;
}

QWidget *DebuggerWindow::buildSymbols()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(false); });
    bar->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    bar->addWidget(m_symCount);
    layout->addLayout(bar);
    m_symList = new QListWidget;
    setMono(m_symList);
    m_symList->setToolTip(QStringLiteral("Enter or double-click shows it in the Memory tab"));
    connect(m_symList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const unsigned addr = item->data(Qt::UserRole).toUInt();
        showTab(TAB_MEMORY);
        showInMemory(uint16_t(addr), uint16_t(addr));
    });
    layout->addWidget(m_symList, 1);
    return page;
}

/* The packets left (a note and Clear over them), the cartridge's link and
 * counts right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        msxdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("FujiNet cartridge")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus(const msxdebug_snapshot &snap)
{
    const bool stopped = snap.paused != 0;
    const QString text = stopped ? QStringLiteral("Stopped: %1").arg(stripControl(snap.text)) : QStringLiteral("Running");
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    /* the action on offer wears the accent: Run while stopped */
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: %2;")
                                          .arg(msxAccentFill(m_runBtn).name(), msxAccentFillInk(m_runBtn).name())
                                    : QString());
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshRegs()
{
    msxdebug_cpu c;
    msxdebug_cpu_get(m_dbg, &c);
    for (int i = 0; i < 15; ++i) {
        int val = 0;
        switch (kRegs[i].id) {
        case MSX_REG_AF: val = c.af; break;     case MSX_REG_BC: val = c.bc; break;
        case MSX_REG_DE: val = c.de; break;     case MSX_REG_HL: val = c.hl; break;
        case MSX_REG_IX: val = c.ix; break;     case MSX_REG_IY: val = c.iy; break;
        case MSX_REG_SP: val = c.sp; break;     case MSX_REG_PC: val = c.pc; break;
        case MSX_REG_AF2: val = c.af2; break;   case MSX_REG_BC2: val = c.bc2; break;
        case MSX_REG_DE2: val = c.de2; break;   case MSX_REG_HL2: val = c.hl2; break;
        case MSX_REG_I: val = c.i; break;       case MSX_REG_R: val = c.r; break;
        default: val = c.im; break;
        }
        setEntry(m_reg[i], kRegs[i].digits ? hx((unsigned)val & (kRegs[i].digits == 4 ? 0xFFFF : 0xFF), kRegs[i].digits)
                                           : QString::number(val));
    }
    const int flags[6] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
    for (int i = 0; i < 6; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_iff[0]->setChecked(c.iff1 != 0);
    m_iff[1]->setChecked(c.iff2 != 0);
    m_haltNote->setText(c.halted ? QStringLiteral("HALT") : QString());
}

/* The dump from m_memTop, each row labelled once with the region it is in (the
 * map as of the last refreshSlow); a highlighted range (Go to, a watch, a
 * search hit) in the accent. */
void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    memset(mem, 0, sizeof mem);
    msxdebug_read(m_dbg, (uint16_t)m_memTop, mem, (int)sizeof mem);

    QString t;
    t.reserve(MEM_ROWS * 90);
    t += QStringLiteral("       ");
    for (int col = 0; col < 16; ++col) t += hx((unsigned)col, 1) + QStringLiteral("  ");
    char region[64];
    QString lastRegion;
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *r = mem + row * 16;
        const unsigned addr = (unsigned)(m_memTop + row * 16);
        t += QStringLiteral("\n$%1: ").arg(hx(addr, 4));
        for (int col = 0; col < 16; ++col) t += hx(r[col], 2) + QLatin1Char(' ');
        t += QLatin1Char(' ');
        for (int col = 0; col < 16; ++col) t += QLatin1Char(r[col] >= 0x20 && r[col] < 0x7F ? (char)r[col] : '.');
        if (!m_regions.empty() && msxmem_region_name(m_regions.data(), (int)m_regions.size(), (uint16_t)addr, region, sizeof region) > 0) {
            const QString name = unbrace(region);
            if (name != lastRegion) t += QStringLiteral("  ; ") + name;
            lastRegion = name;
        }
    }
    setText(m_mem, t);

    /* the bytes Go to found stay marked across refreshes */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_memHl) {
        for (int row = 0; row < MEM_ROWS; ++row) {
            const QTextBlock block = m_mem->document()->findBlockByNumber(1 + row);
            if (!block.isValid()) break;
            for (int col = 0; col < 16; ++col) {
                const int addr = m_memTop + row * 16 + col;
                if (addr < m_memHlLo || addr > m_memHlHi) continue;
                QTextEdit::ExtraSelection mark;
                mark.cursor = QTextCursor(m_mem->document());
                mark.cursor.setPosition(block.position() + 7 + col * 3);
                mark.cursor.setPosition(block.position() + 7 + col * 3 + 2, QTextCursor::KeepAnchor);
                mark.format = accentFormat(m_mem);
                sel << mark;
            }
        }
    }
    m_mem->setExtraSelections(sel);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static msxdebug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        msxdebug_cpu c;
        msxdebug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)msxdebug_row_address(m_dbg, (uint16_t)c.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int n = msxdebug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);
    m_lineAddr.clear();
    QString text;
    char buf[256];
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        char label[48];
        snprintf(label, sizeof label, "%.*s", labelWidth, lines[i].label);
        /* the gutter: a breakpoint dot, then the PC's arrow */
        snprintf(buf, sizeof buf, "%c %04X  %-11.11s %-*s %-22s", lines[i].is_pc ? '>' : ' ', lines[i].address,
                 lines[i].bytes, labelWidth, label, lines[i].disasm);
        if (i) text += QLatin1Char('\n');
        text += lines[i].has_breakpoint ? QStringLiteral("●") : QStringLiteral(" ");
        text += QString::fromUtf8(buf);
        if (lines[i].comment[0]) text += QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    QList<QTextEdit::ExtraSelection> sel;
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            if (lines[i].breakpoint_enabled) {
                s.format.setForeground(QColor(0xe0, 0x1b, 0x24));
                s.format.setFontWeight(QFont::Bold);
            } else {
                s.format.setForeground(QColor(0x9a, 0x99, 0x96));
            }
            sel << s;
        }
        /* the selected line underlined */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format.setFontUnderline(true);
            s.format.setFontWeight(QFont::Bold);
            sel << s;
        }
    }
    /* the PC's line in the accent colour, last so its ink wins */
    if (pcLine >= 0) {
        QTextCharFormat f = accentFormat(m_disasm);
        if (lines[pcLine].address == m_selected) f.setFontUnderline(true);
        sel << lineSelection(m_disasm, pcLine, f);
    }
    m_disasm->setExtraSelections(sel);
}

void DebuggerWindow::refreshBps()
{
    static msxdebug_breakpoint bps[BP_MAX];
    const int n = msxdebug_breakpoint_list(m_dbg, bps, BP_MAX);

    /* what stopped the machine, in the core's words */
    msxdebug_snapshot snap;
    msxdebug_stop_snapshot(m_dbg, &snap);
    m_bpStop->setText(snap.paused ? QStringLiteral("Stopped: %1").arg(stripControl(snap.text)) : QStringLiteral("Running."));

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int idx = 0; idx < n; ++idx)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[idx].id).arg(bps[idx].enabled).arg(bps[idx].hits)
                   .arg(bps[idx].type).arg(bps[idx].start).arg(bps[idx].end).arg(QString::fromUtf8(bps[idx].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int idx = 0; idx < n; ++idx) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[idx].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[idx].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(idx, 0, on);
        QStringList type;
        if (bps[idx].type == MSXDEBUG_BP_ACCESS) {
            type << QStringLiteral("access");
        } else {
            if (bps[idx].type & MSXDEBUG_BP_EXEC) type << QStringLiteral("exec");
            if (bps[idx].type & MSXDEBUG_BP_READ) type << QStringLiteral("read");
            if (bps[idx].type & MSXDEBUG_BP_WRITE) type << QStringLiteral("write");
            if (bps[idx].type & MSXDEBUG_BP_IN) type << QStringLiteral("in");
            if (bps[idx].type & MSXDEBUG_BP_OUT) type << QStringLiteral("out");
        }
        auto *typeItem = new QTableWidgetItem(type.join(QLatin1Char('/')));
        char desc[256];
        msxbp_describe(m_dbg, &bps[idx], desc, sizeof desc);
        typeItem->setToolTip(stripControl(desc));
        m_bpTable->setItem(idx, 1, typeItem);
        /* a port range for in/out is two digits */
        const int digits = (bps[idx].type & (MSXDEBUG_BP_IN | MSXDEBUG_BP_OUT)) ? 2 : 4;
        QString range = QStringLiteral("$") + hx(bps[idx].start, digits);
        if (bps[idx].end != bps[idx].start) range += QStringLiteral("-$") + hx(bps[idx].end, digits);
        m_bpTable->setItem(idx, 2, new QTableWidgetItem(range));
        m_bpTable->setItem(idx, 3, new QTableWidgetItem(stripControl(bps[idx].condition)));
        m_bpTable->setItem(idx, 4, new QTableWidgetItem(QString::number(bps[idx].hits)));
        for (int col = 1; col < 5; ++col) m_bpTable->item(idx, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int bpid = bps[idx].id;
        connect(rm, &QToolButton::clicked, this, [this, bpid] {
            msxdebug_breakpoint_remove(m_dbg, bpid);
            syncLater();
        });
        m_bpTable->setCellWidget(idx, 5, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

void DebuggerWindow::refreshWatches()
{
    const int count = msxwatch_count();
    const int keep = m_wtList->currentRow();
    QStringList rows;
    for (int idx = 0; idx < count; ++idx) {
        char line[256];
        msxwatch_format(m_dbg, idx, line, sizeof line);
        rows << stripControl(line);
    }
    /* unchanged: leave the list alone (a click in it must not be lost) */
    bool same = m_wtList->count() == count;
    for (int idx = 0; same && idx < count; ++idx) same = m_wtList->item(idx)->text() == rows[idx];
    if (same) return;
    m_wtList->clear();
    m_wtList->addItems(rows);
    if (keep >= 0 && keep < count) m_wtList->setCurrentRow(keep);
}

/* The machine, its memory map and its I/O, from the structured getters (valid
 * while stopped; a running machine shows the last stop). */
void DebuggerWindow::refreshCart()
{
    static msxdebug_io ioinfo;
    msxdebug_cart cart;
    msxdebug_region map[16];
    QString top, bottom;

    msxdebug_snapshot snap;
    msxdebug_stop_snapshot(m_dbg, &snap);
    m_cartSerial = snap.serial;
    msxdebug_cart_info(m_dbg, &cart);
    top += QStringLiteral("Machine\n");
    top += QStringLiteral("  %1 (%2), Z80 at %3 Hz\n").arg(QString::fromUtf8(cart.machine), QString::fromUtf8(cart.type)).arg(cart.z80_hz);
    top += QStringLiteral("  FujiNet cartridge: %1\n")
               .arg(!cart.fujinet ? QStringLiteral("not in a slot")
                                  : QStringLiteral("in a slot, link %1%2")
                                        .arg(cart.fujinet_link ? QStringLiteral("up") : QStringLiteral("down"),
                                             cart.fujinet_external ? QStringLiteral(" (external FujiNet)") : QString()));
    top += QStringLiteral("  Sound chips: PSG, SCC %1, MSX-MUSIC %2, MSX-AUDIO %3     Memory mapper: %4\n")
               .arg(yesNo(cart.scc), yesNo(cart.music), yesNo(cart.audio), yesNo(cart.mapper));
    top += QStringLiteral("  Devices: %1\n").arg(unbrace(cart.devices));
    top += QStringLiteral("\nMemory map\n");
    const int regions = msxdebug_memory_map(m_dbg, map, 16);
    if (!regions) top += QStringLiteral("  Stop the machine to read its memory map.\n");
    for (int idx = 0; idx < regions; ++idx)
        top += QStringLiteral("  %1-%2  %3%4  %5\n")
                   .arg(hx(map[idx].lo, 4), hx(map[idx].hi, 4))
                   .arg(map[idx].readable ? QLatin1Char('r') : QLatin1Char('-'))
                   .arg(map[idx].writable ? QLatin1Char('w') : QLatin1Char('-'))
                   .arg(unbrace(map[idx].name));
    setText(m_cartText, stripControl(top.toUtf8().constData()));

    if (!msxdebug_io_get(m_dbg, &ioinfo) || !ioinfo.valid) {
        setText(m_ioText, QStringLiteral("No machine running."));
        return;
    }
    bottom += QStringLiteral("Selected slots (CPU pages)\n");
    for (int page = 0; page < 4; ++page) {
        const QString slot = ioinfo.page_subslot[page] < 0
                                 ? QString::number(ioinfo.page_slot[page])
                                 : QStringLiteral("%1-%2").arg(ioinfo.page_slot[page]).arg(ioinfo.page_subslot[page]);
        bottom += QStringLiteral("  page %1 ($%2): slot %3  %4\n").arg(page).arg(hx(unsigned(page) * 0x4000, 4))
                      .arg(slot, unbrace(ioinfo.page_device[page]));
    }
    for (int slot = 0; slot < 4; ++slot)
        if (ioinfo.expanded[slot])
            bottom += QStringLiteral("  slot %1 is expanded%2\n").arg(slot)
                          .arg(ioinfo.subslot_reg[slot] >= 0 ? QStringLiteral(", sub-slot register $%1").arg(hx(unsigned(ioinfo.subslot_reg[slot]), 2))
                                                             : QString());
    bottom += QStringLiteral("\nPPI  $A8 slot select %1   $A9 keyboard column %2   $AA %3   $AB control %4\n")
                  .arg(hx(ioinfo.ppi[0], 2), hx(ioinfo.ppi[1], 2), hx(ioinfo.ppi[2], 2), hx(ioinfo.ppi[3], 2));
    if (ioinfo.mapper)
        bottom += QStringLiteral("Memory mapper ($FC-$FF): %1 %2 %3 %4\n").arg(hx(ioinfo.mapper_reg[0], 2), hx(ioinfo.mapper_reg[1], 2),
                                                                              hx(ioinfo.mapper_reg[2], 2), hx(ioinfo.mapper_reg[3], 2));
    for (int idx = 0; idx < ioinfo.rom_mappers; ++idx) {
        bottom += QStringLiteral("ROM mapper %1: blocks at $0000-$E000:").arg(unbrace(ioinfo.rom[idx].device));
        for (int win = 0; win < 8; ++win)
            bottom += ioinfo.rom[idx].block[win] == 255 ? QStringLiteral(" --") : QStringLiteral(" %1").arg(hx(ioinfo.rom[idx].block[win], 2));
        bottom += QLatin1Char('\n');
    }
    bottom += QStringLiteral("\nKeyboard matrix (0 = pressed), %1 rows:").arg(ioinfo.keymatrix_rows);
    for (int row = 0; row < ioinfo.keymatrix_rows && row < 16; ++row)
        bottom += QStringLiteral(" %1").arg(hx(ioinfo.keymatrix[row], 2));
    bottom += QStringLiteral("\nJoystick ports (bit set = not pressed): A %1   B %2\n")
                  .arg(hx(ioinfo.joyport[0], 2), hx(ioinfo.joyport[1], 2));
    bottom += QStringLiteral("\nopenMSX slotmap\n%1\n").arg(QString::fromUtf8(ioinfo.slotmap));
    setText(m_ioText, stripControl(bottom.toUtf8().constData()));
}

/* Trace (which 0: the newest instructions, live) and History (1: what ran
 * before the stop). Both read the one ring; the filter and the note are the
 * tab's own. */
void DebuggerWindow::refreshTrace(int which, bool force)
{
    static msxdebug_trace buf[HIST_FETCH];
    const bool live = which == 0;
    const bool stopped = msxdebug_is_stopped(m_dbg) != 0;
    const bool traceOn = msxdebug_trace_enabled(m_dbg) != 0, histOn = msxdebug_history_enabled(m_dbg) != 0;
    const bool recording = live ? traceOn : histOn;
    const uint64_t total = live ? msxdebug_trace_total(m_dbg) : msxdebug_history_count(m_dbg);
    uint16_t first = 0, last = 0xFFFF;
    int filter = MSXDEBUG_TRACE_ALL;

    m_hSync = true;      /* the two Record buttons follow the engine */
    m_hRec[0]->setChecked(traceOn);
    m_hRec[1]->setChecked(histOn);
    m_hSync = false;

    if (!force && total == m_hSeen[which] && (live || stopped)) return;
    m_hSeen[which] = total;
    if (!live && !stopped) {
        m_hNote[which]->setText(QStringLiteral("Stop the machine to read its history."));
        return;
    }
    switch (m_hFilter[which]->currentIndex()) {
    case 1: filter = MSXDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (msxsym_parse_range(m_dbg, m_hRange[which]->text().toUtf8().constData(), &first, &last)) filter = MSXDEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    QList<int> hit;
    int fetched = 0;
    if (live) {
        fetched = msxdebug_trace_read(m_dbg, buf, HIST_FETCH);                /* newest first */
        for (int idx = 0; idx < fetched && hit.size() < HIST_SHOWN; ++idx)
            if (msxdebug_trace_match(&buf[idx], filter, first, last)) hit.append(idx);
    } else {
        fetched = msxdebug_history_copy(m_dbg, buf, HIST_FETCH);              /* oldest first */
        for (int idx = fetched - 1; idx >= 0 && hit.size() < HIST_SHOWN; --idx)   /* the newest matches, scanning back */
            if (msxdebug_history_match(&buf[idx], filter, first, last)) hit.append(idx);
    }
    QString text;
    if (!recording && fetched == 0)
        text = QStringLiteral("%1 is off. Press Record, then let the machine run.").arg(live ? QStringLiteral("Trace") : QStringLiteral("History"));
    for (int pos = hit.size() - 1; pos >= 0; --pos) {      /* oldest first */
        char line[320];
        if (live) msxdebug_trace_format(&buf[hit[pos]], line, sizeof line);
        else msxdebug_history_format(&buf[hit[pos]], line, sizeof line);
        text += stripControl(line);
        text += QLatin1Char('\n');
    }
    setText(m_hText[which], text, true);
    m_hNote[which]->setText(QStringLiteral("%1%2 instructions recorded, last %3 shown")
                                .arg(recording ? QStringLiteral("Recording. ") : QString()).arg(total).arg(hit.size()));
}

void DebuggerWindow::refreshVideo(bool force)
{
    if (m_video) m_video->refresh(force || m_videoForce);
    m_videoForce = false;
}

/* The PSG, and the SCC, MSX-MUSIC and MSX-AUDIO chips the machine has, read
 * straight out of openMSX (stopped only). */
void DebuggerWindow::refreshSound()
{
    static msxdebug_sound snd;
    if (!msxdebug_is_stopped(m_dbg)) {
        setText(m_soundText, QStringLiteral("Stop the machine to read the sound chips."));
        return;
    }
    QString text;
    if (!msxdebug_sound_get(m_dbg, &snd)) {
        setText(m_soundText, QStringLiteral("No machine running."));
        return;
    }
    if (snd.psg.present) {
        const msxdebug_psg &psg = snd.psg;
        text += QStringLiteral("PSG, AY-3-8910 ($A0-$A2)\n");
        for (int voice = 0; voice < 3; ++voice)
            text += QStringLiteral("  %1  period %2  tone %3  noise %4  level %5%6  %7\n")
                     .arg(QLatin1Char(char('A' + voice)))
                     .arg(psg.period[voice], 4)
                     .arg(onOff(psg.tone_on[voice]), onOff(psg.noise_on[voice]))
                     .arg(psg.level[voice], 2)
                     .arg(psg.envelope[voice] ? QStringLiteral(" (envelope)") : QString())
                     .arg(psg.tone_on[voice] && psg.period[voice] > 0 ? noteName(psg.freq[voice]) : QStringLiteral("-"));
        text += QStringLiteral("  Noise period %1   Envelope period %2, shape %3\n")
                 .arg(psg.noise_period).arg(psg.env_period).arg(hx(unsigned(psg.env_shape), 1));
        text += QStringLiteral("  Registers:");
        for (int reg = 0; reg < 16; ++reg) text += QStringLiteral(" R%1=%2").arg(hx(unsigned(reg), 1), hx(psg.reg[reg], 2));
        text += QStringLiteral("\n  (the mixer, R7, is active-low: a set bit turns that voice off; R14/R15 are the joystick ports)\n");
    }
    if (snd.scc.present) {
        const msxdebug_scc &scc = snd.scc;
        text += QStringLiteral("\nSCC, %1\n").arg(QString::fromUtf8(scc.name));
        for (int chan = 0; chan < 5; ++chan) {
            text += QStringLiteral("  %1  period %2  volume %3  %4  %5\n  wave:")
                     .arg(chan + 1).arg(scc.period[chan], 4).arg(scc.volume[chan], 2)
                     .arg(scc.enabled[chan] ? QStringLiteral("on ") : QStringLiteral("off"))
                     .arg(scc.enabled[chan] && scc.period[chan] > 0 ? noteName(scc.freq[chan]) : QStringLiteral("-"));
            for (int pos = 0; pos < 32; ++pos) text += QStringLiteral(" %1").arg(hx(scc.wave[chan][pos], 2));
            text += QLatin1Char('\n');
        }
    }
    if (snd.opll.present) {
        const msxdebug_opll &opll = snd.opll;
        text += QStringLiteral("\nMSX-MUSIC, %1%2\n").arg(QString::fromUtf8(opll.name),
                                                    opll.rhythm ? QStringLiteral("  (rhythm mode)") : QString());
        for (int chan = 0; chan < 9; ++chan)
            text += QStringLiteral("  %1  F-number %2  block %3  key %4  instrument %5  volume %6  %7\n")
                     .arg(chan + 1).arg(opll.fnum[chan], 3).arg(opll.block[chan]).arg(onOff(opll.key_on[chan]))
                     .arg(opll.instrument[chan], 2).arg(opll.volume[chan], 2)
                     .arg(opll.key_on[chan] ? noteName(opll.freq[chan]) : QStringLiteral("-"));
    }
    if (snd.y8950.present) {
        const msxdebug_y8950 &y8950 = snd.y8950;
        text += QStringLiteral("\nMSX-AUDIO, %1\n  Registers:").arg(QString::fromUtf8(y8950.name));
        for (int reg = 0; reg < 256; ++reg) {
            if (reg % 16 == 0) text += QStringLiteral("\n  %1:").arg(hx(unsigned(reg), 2));
            text += QStringLiteral(" %1").arg(hx(y8950.reg[reg], 2));
        }
        text += QLatin1Char('\n');
    }
    QStringList absent;
    if (!snd.scc.present) absent << QStringLiteral("SCC");
    if (!snd.opll.present) absent << QStringLiteral("MSX-MUSIC");
    if (!snd.y8950.present) absent << QStringLiteral("MSX-AUDIO");
    if (!absent.isEmpty()) text += QStringLiteral("\nNot on this machine: %1\n").arg(absent.join(QStringLiteral(", ")));
    setText(m_soundText, text);
}

/* Only rebuilt when the list or the filter changed: a real game .sym can be
 * a few hundred entries. force: whatever the tab shows, on first open too. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const int total = msxdebug_symbols_count(m_dbg);
    const QString filter = m_symFilter->text();
    if (!force && total == m_symTotal && filter == m_symShownFilter) return;
    m_symTotal = total;
    m_symShownFilter = filter;
    std::vector<int> found((size_t)qMax(total, 1));
    const int shown = msxdebug_symbols_filter(m_dbg, filter.toUtf8().constData(), found.data(), (int)found.size());
    m_symList->clear();
    for (int idx = 0; idx < shown; ++idx) {
        uint16_t addr;
        char name[96];
        if (!msxdebug_symbols_get(m_dbg, found[(size_t)idx], &addr, name, sizeof name)) continue;
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(hx(addr, 4), QString::fromUtf8(name)));
        item->setData(Qt::UserRole, addr);
        m_symList->addItem(item);
    }
    m_symCount->setText(QStringLiteral("%1 symbol%2%3").arg(shown).arg(shown == 1 ? QString() : QStringLiteral("s"))
                            .arg(filter.isEmpty() ? QString() : QStringLiteral(" (filtered)")));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static msx_fuji_packet p[TRACE_MAX];
    static char state[2048];
    const int n = msxdebug_fujinet_trace(m_dbg, p, TRACE_MAX);
    if (msxsession_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("External FujiNet %1, cartridge %2.")
                                .arg(QString::fromUtf8(msxsession_fujinet_endpoint(m_session)))
                                .arg(msxsession_fujinet_link(m_session) ? QStringLiteral("connected")
                                                                        : QStringLiteral("not connected")));
    else if (!msxsession_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is off (disabled, or its runtime did not start)."));
    else
        m_fujiNote->setText(msxsession_fujinet_link(m_session)
                                ? QStringLiteral("FujiNet is on, cartridge connected.")
                                : QStringLiteral("FujiNet is on, cartridge not connected."));
    state[0] = '\0';
    msxdebug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshTab(int tab, bool live)
{
    switch (tab) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: refreshMem(); if (!live) scheduleSlow(!m_regions.empty()); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(); break;
    case TAB_CART: if (!live) scheduleSlow(m_cartSerial != ~0ull); break;
    case TAB_TRACE: refreshTrace(0, !live); break;
    case TAB_HISTORY: if (!live) refreshTrace(1, true); break;
    case TAB_VIDEO: refreshVideo(!live); break;
    case TAB_SOUND: if (!live) refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(!live); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* What costs openMSX most of a second to answer (the memory map, the whole of
 * Cart & I/O): read for the stop that is current. While stepping the old
 * answer stays until the stepping pauses (scheduleSlow); a tab just opened
 * does not wait. */
void DebuggerWindow::refreshSlow()
{
    msxdebug_snapshot snap;
    msxdebug_stop_snapshot(m_dbg, &snap);
    if (!snap.paused) return;
    if (tabVisible(TAB_MEMORY) && (m_regions.empty() || m_regionSerial != snap.serial)) {
        msxdebug_region map[16];
        const int count = msxdebug_memory_map(m_dbg, map, 16);
        m_regions.assign(map, map + qMax(0, count));
        m_regionSerial = snap.serial;
        refreshMem();
    }
    if (tabVisible(TAB_CART) && m_cartSerial != snap.serial) refreshCart();
}

void DebuggerWindow::scheduleSlow(bool haveOld)
{
    if (haveOld && !m_slowNow) m_slowTimer.start(500);       /* restarts: it fires when the stops stop */
    else refreshSlow();
}

/* Every tab that is on screen (the one showing, and any peeled off). */
void DebuggerWindow::refreshAll()
{
    msxdebug_snapshot snap;
    msxdebug_stop_snapshot(m_dbg, &snap);
    refreshStatus(snap);
    refreshRegs();
    for (int tab = 0; tab < TAB_COUNT; ++tab)
        if (tabVisible(tab)) refreshTab(tab, false);
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then the tabs showing follow. */
void DebuggerWindow::resync()
{
    msxdebug_snapshot snap;
    msxdebug_stop_snapshot(m_dbg, &snap);
    /* a step is its own answer; anything else that stopped the machine is news */
    if (snap.paused && snap.serial != m_seenSerial && snap.stop.reason != MSXDEBUG_STOP_STEP && snap.text[0])
        consoleAppend(QStringLiteral("— %1\n").arg(stripControl(snap.text)));
    m_wasStopped = snap.paused != 0;
    m_seenSerial = snap.serial;
    m_seenGen = msxdebug_generation(m_dbg);
    m_runningTicks = 0;
    refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !msxdebug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop only through the stop snapshot (its stop
 * callback fires off the UI thread), so this polls it: a refresh when it
 * moved, and the live values while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    msxdebug_snapshot snap;
    msxdebug_stop_snapshot(m_dbg, &snap);
    const unsigned gen = msxdebug_generation(m_dbg);
    const bool stopped = snap.paused != 0;
    if (snap.serial != m_seenSerial || gen != m_seenGen || stopped != m_wasStopped) {
        resync();
        return;
    }
    if (stopped) return;
    ++m_runningTicks;
    /* a running machine publishes a frame, and the ring a record, all the time */
    if (m_runningTicks % 2 == 0 && tabVisible(TAB_VIDEO)) refreshVideo(false);
    if (m_runningTicks % 4 == 0 && tabVisible(TAB_TRACE)) refreshTrace(0, false);
    if (m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(snap);
        refreshRegs();
        for (int tab = 0; tab < TAB_COUNT; ++tab)
            if (tabVisible(tab) && tab != TAB_VIDEO && tab != TAB_TRACE) refreshTab(tab, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    msxdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int n = msxdebug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (msxdebug_is_stopped(m_dbg)) {
        msxdebug_resume(m_dbg);
        resync();
    } else {
        msxdebug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(void (*fn)(msxdebug *))
{
    fn(m_dbg);
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    msxdebug_run_to(m_dbg, (uint16_t)m_selected);
    settleAndSync();
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    msxdebug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* A number the way the prompt takes it: a bare token of hex digits is hex (as
 * the register and value entries always were), anything else goes to the
 * engine's expression evaluator ($C000, 49152, a label, hl + 1 ...). */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]{1,8}$"));
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (why) *why = QStringLiteral("Type a value");
        return false;
    }
    if (hexOnly.match(trimmed).hasMatch()) {
        *out = trimmed.toLong(nullptr, 16);
        return true;
    }
    char err[160] = "";
    if (msxdebug_eval(m_dbg, trimmed.toUtf8().constData(), out, err, int(sizeof err)) == 0) return true;
    if (why) *why = stripControl(err);
    return false;
}

/* A label, or a number as numberOf reads it. */
bool DebuggerWindow::addressOf(const QString &text, long *out)
{
    const int lbl = msxdebug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (lbl >= 0) { *out = lbl; return true; }
    return numberOf(text, out, nullptr);
}

/* One bulk write of every register: the entry changes the engine's copy, the
 * engine says if it will not take it (running, IM above 2). */
void DebuggerWindow::applyCpu(const msxdebug_cpu &cpu, const QString &okMessage)
{
    char err[160] = "";
    if (msxdebug_regs_set(m_dbg, &cpu, err, sizeof err)) message(okMessage);
    else message(err[0] ? stripControl(err) : QStringLiteral("The registers could not be written"));
}

/* Edits go in only while the machine is stopped. */
void DebuggerWindow::setReg(int idx)
{
    long val = 0;
    QString why;
    QLineEdit *edit = m_reg[idx];
    if (!msxdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
    } else if (!numberOf(edit->text(), &val, &why)) {
        message(why);
    } else {
        msxdebug_cpu cpu;
        msxdebug_cpu_get(m_dbg, &cpu);
        const int word = (int)(val & 0xFFFF), byte = (int)(val & 0xFF);
        switch (kRegs[idx].id) {
        case MSX_REG_AF: cpu.af = word; break;     case MSX_REG_BC: cpu.bc = word; break;
        case MSX_REG_DE: cpu.de = word; break;     case MSX_REG_HL: cpu.hl = word; break;
        case MSX_REG_IX: cpu.ix = word; break;     case MSX_REG_IY: cpu.iy = word; break;
        case MSX_REG_SP: cpu.sp = word; break;     case MSX_REG_PC: cpu.pc = word; break;
        case MSX_REG_AF2: cpu.af2 = word; break;   case MSX_REG_BC2: cpu.bc2 = word; break;
        case MSX_REG_DE2: cpu.de2 = word; break;   case MSX_REG_HL2: cpu.hl2 = word; break;
        case MSX_REG_I: cpu.i = byte; break;       case MSX_REG_R: cpu.r = byte; break;
        default: cpu.im = (int)val; break;
        }
        /* a CPU in HALT stays halted when its PC is written */
        applyCpu(cpu, kRegs[idx].id == MSX_REG_PC
                          ? QStringLiteral("PC set (a CPU in HALT stays halted until an interrupt: run a Frame first)")
                          : QStringLiteral("%1 set").arg(QString::fromUtf8(kRegs[idx].name)));
    }
    edit->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int mask, bool enabled)
{
    if (!msxdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
    } else {
        msxdebug_cpu cpu;
        msxdebug_cpu_get(m_dbg, &cpu);
        cpu.af = enabled ? (cpu.af | mask) : (cpu.af & ~mask);
        applyCpu(cpu, QStringLiteral("Flag set"));
    }
    resync();
}

void DebuggerWindow::setIff(int which, bool enabled)
{
    if (!msxdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
    } else {
        msxdebug_cpu cpu;
        msxdebug_cpu_get(m_dbg, &cpu);
        (which ? cpu.iff2 : cpu.iff1) = enabled ? 1 : 0;
        applyCpu(cpu, QStringLiteral("IFF%1 set").arg(which + 1));
    }
    resync();
}

/* Shows a range in the Memory tab, highlighted, on a row boundary and a
 * little above the middle. */
void DebuggerWindow::showInMemory(uint16_t first, uint16_t last)
{
    m_memHl = true;
    m_memHlLo = first;
    m_memHlHi = last;
    m_memTop = qBound(0, (int)((first > 0x100 ? first - 0x100 : 0) & ~0xF), 0x10000 - MEM_ROWS * 16);
    refreshMem();
    const int line = 1 + (first - m_memTop) / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* Go to: an address, a label, an expression or lo-hi. */
void DebuggerWindow::memGoto()
{
    uint16_t first, last;
    if (msxsym_parse_range(m_dbg, m_memGoto->text().toUtf8().constData(), &first, &last)) {
        showInMemory(first, last);
    } else {
        m_memHl = false;
        refreshMem();
        message(QStringLiteral("Not an address ($C000, a label or first-last)"));
    }
}

void DebuggerWindow::memPoke()
{
    long addr, val;
    QString why;
    if (!msxdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    if (!addressOf(m_memAddr->text(), &addr) || addr < 0 || addr > 0xFFFF) {
        message(QStringLiteral("Not an address"));
        return;
    }
    if (!numberOf(m_memVal->text(), &val, &why) || val < 0 || val > 0xFF) {
        message(why.isEmpty() ? QStringLiteral("The value is a byte ($00-$FF)") : why);
        return;
    }
    msxdebug_write(m_dbg, (uint16_t)addr, (uint8_t)val);
    message(QStringLiteral("Wrote $%1 to $%2").arg(hx((unsigned)val, 2), hx((unsigned)addr, 4)));
    resync();
}

/* Find Next / Previous: from the highlighted bytes, or the top of the dump. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[MSXSEARCH_MAX], mask;
    int count = 0, flags = 0;
    uint16_t found;
    if (!msxsearch_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &count, &mask, &flags)) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (3E 01) or some text."));
        return;
    }
    if (!msxdebug_is_stopped(m_dbg)) {
        m_srchNote->setText(QStringLiteral("Stop the machine to search its memory."));
        return;
    }
    const uint16_t from = m_memHl ? (backward ? m_memHlLo : uint16_t(m_memHlLo + 1)) : (uint16_t)m_memTop;
    const int hit = msxdebug_search_next(m_dbg, pat, count, mask, flags, from, backward ? 1 : 0, &found);
    if (hit > 0) {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hx(found, 4)));
        showInMemory(found, uint16_t(found + count - 1));
    } else {
        m_srchNote->setText(hit == 0 ? QStringLiteral("Not found.") : QStringLiteral("Stop the machine to search its memory."));
    }
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a) || a < 0 || a > 0xFFFF) {
        message(QStringLiteral("No such address or label"));
        return;
    }
    m_followPc->setChecked(false);
    m_selected = (int)a;
    m_disasmTop = (uint16_t)msxdebug_row_address(m_dbg, (uint16_t)a, -(visibleRows() / 3));
    refreshDisasm();
}

/* The type takes an address or label; in and out a port. The end is
 * optional; the condition is an expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    static const int types[6] = { MSXDEBUG_BP_EXEC, MSXDEBUG_BP_READ, MSXDEBUG_BP_WRITE, MSXDEBUG_BP_ACCESS,
                                  MSXDEBUG_BP_IN, MSXDEBUG_BP_OUT };
    const int sel = qBound(0, m_bpType->currentIndex(), 5);
    const bool port = sel >= 4;
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    long a, b;
    const long limit = port ? 0xFF : 0xFFFF;
    if (!addressOf(start, &a) || a < 0 || a > limit) {
        m_bpMsg->setText(port ? QStringLiteral("A port is $00-$FF") : QStringLiteral("Not an address ($4000, or a label)"));
        return;
    }
    b = a;
    if (!end.isEmpty() && (!addressOf(end, &b) || b < a || b > limit)) {
        m_bpMsg->setText(QStringLiteral("The end is not an address at or after the start"));
        return;
    }
    char err[256] = "";
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (msxdebug_breakpoint_add(m_dbg, types[sel], (uint16_t)a, (uint16_t)b, cond.constData(), err, sizeof err) < 0) {
        m_bpMsg->setText(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

void DebuggerWindow::addWatch()
{
    if (msxwatch_add(m_wtEntry->text().toUtf8().constData()) < 0) {
        message(QStringLiteral("Type an address, label, expression or lo-hi (the list holds %1)").arg(MSXWATCH_MAX));
        return;
    }
    m_wtEntry->clear();
    resync();
}

void DebuggerWindow::loadSymbols()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load Symbols"), QString(),
        QStringLiteral("Symbol files (*.sym *.map *.noi *.txt);;All files (*)"));
    if (path.isEmpty()) return;
    char msg[512] = "";
    msxdebug_load_symbols(m_dbg, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    msxdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs peeled off into windows of their own --------------------------------- */

/* A page's place in the tab widget is not its identity once tabs can be
 * moved into windows of their own: each carries its original index. */
int DebuggerWindow::currentTab() const
{
    QWidget *page = m_tabs->currentWidget();
    return page ? page->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* Brings a tab forward, wherever it lives. */
void DebuggerWindow::showTab(int tab)
{
    if (m_popped[tab]) {
        m_popped[tab]->activateWindow();
        m_popped[tab]->raise();
    } else {
        m_tabs->setCurrentWidget(m_pages[tab]);
    }
}

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;   /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("Debugger – %1").arg(QString::fromUtf8(kTabs[tab].label)));
    win->setProperty("msxHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].name)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch();
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    win->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    win->onKey = [this](QKeyEvent *evt) { return handleKey(evt); };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { msxHelpFor(win); });
    win->show();
    resync();
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int before = 0; before < tab; before++)   /* back where it was in the order */
        if (!m_popped[before]) pos++;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, tabLabel(tab));
    m_tabs->setCurrentIndex(pos);
    resync();
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)msxdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

/* The family's keys, from this window or one of its peeled-off tabs. */
bool DebuggerWindow::handleKey(QKeyEvent *evt)
{
    if (!isDebuggerKey(evt)) return false;
    const bool shift = evt->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (evt->isAutoRepeat() && (evt->key() == Qt::Key_F5 || evt->key() == Qt::Key_F9 || evt->key() == Qt::Key_F12)) return true;
    switch (evt->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? msxdebug_frame : msxdebug_step); break;
    case Qt::Key_F8: stepAnd(shift ? msxdebug_step_out : msxdebug_step_over); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
    return true;
}

void DebuggerWindow::keyPressEvent(QKeyEvent *evt)
{
    if (!handleKey(evt)) QMainWindow::keyPressEvent(evt);
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    if (e->spontaneous()) return;
    msxdebug_attach(m_dbg);
    m_wasStopped = false;
    m_seenSerial = ~0ull;
    for (QWidget *win : m_popped)
        if (win) win->show();
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        firstShow();
        settleAndSync();
    });
}

/* The scripted-capture variables that touch the machine (read at construction),
 * applied once the engine is attached: MSX_WATCH adds a watch, MSX_WATCH_BREAK a write-when-changed
 * breakpoint on it, MSX_MEM_GOTO shows it in Memory, MSX_HISTORY records from
 * the start. (MSX_DEBUGGER_TAB and MSX_POPOUT are read at construction.) */
void DebuggerWindow::firstShow()
{
    if (m_shownOnce) return;
    m_shownOnce = true;
    uint16_t first, last;
    if (!m_envWatch.isEmpty()) msxwatch_add(m_envWatch.constData());
    if (!m_envWatchBreak.isEmpty() && msxsym_parse_range(m_dbg, m_envWatchBreak.constData(), &first, &last)) {
        char err[200] = "";
        msxdebug_breakpoint_add(m_dbg, MSXDEBUG_BP_WRITE, first, last, "value != old", err, sizeof err);
    }
    if (!m_envMemGoto.isEmpty() && msxsym_parse_range(m_dbg, m_envMemGoto.constData(), &first, &last)) {
        showInMemory(first, last);
    }
    if (m_envHistory) msxdebug_history_enable(m_dbg, 1);   /* record from the start */
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a detach. The peeled-off tabs go with it. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    if (!e->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        msxdebug_detach(m_dbg);
    }
    QMainWindow::hideEvent(e);
}
