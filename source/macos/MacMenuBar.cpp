#include "MacMenuBar.h"

#ifdef Q_OS_MACOS

#include <QAction>
#include <QDesktopServices>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QUrl>
#include <QtDebug>

#include "AboutDialog.h"
#include "../ControlPanelDialog.h"
#include "../MainWindow.h"
#include "../core/ShortcutManager.h"

// ============================================================================
// Singleton
// ============================================================================

MacMenuBar* MacMenuBar::s_instance = nullptr;

MacMenuBar* MacMenuBar::instance()
{
    if (!s_instance) {
        s_instance = new MacMenuBar();
    }
    return s_instance;
}

// ============================================================================
// Construction
// ============================================================================

MacMenuBar::MacMenuBar(QObject* parent) : QObject(parent)
{
    // Parent-less QMenuBar: Qt promotes it to the macOS system menu bar that
    // is shared by every window in the app and stays visible even when no
    // window is open (per QA Q6.5.3).
    //
    // Lifecycle: both this MacMenuBar and m_menuBar are intentionally
    // parent-less. The singleton lives for the entire QApplication lifetime
    // and is reclaimed by the OS at process exit. Do NOT add a destructor
    // that deletes m_menuBar — Qt requires the parent-less QMenuBar to
    // outlive the application's last window for the system menu bar to
    // remain visible (Qt 6 docs: "You can create a custom default menu bar
    // by creating a parentless QMenuBar.").
    m_menuBar = new QMenuBar(nullptr);
    m_menuBar->setNativeMenuBar(true);  // explicit (default true on macOS)

    // Top-level menus, display order. The first menu is the "App menu" —
    // Qt auto-titles it from CFBundleName (packaged build via compile-mac.sh)
    // or applicationName() (dev build). Adding it with an empty title lets
    // Qt manage the title; the actions inside are what we author.
    m_appMenu      = m_menuBar->addMenu(QString());
    m_fileMenu     = m_menuBar->addMenu(tr("&File"));
    m_editMenu     = m_menuBar->addMenu(tr("&Edit"));
    m_viewMenu     = m_menuBar->addMenu(tr("&View"));
    m_documentMenu = m_menuBar->addMenu(tr("&Document"));
    m_toolsMenu    = m_menuBar->addMenu(tr("&Tools"));
    m_ocrMenu      = m_menuBar->addMenu(tr("&OCR"));
    m_windowMenu   = m_menuBar->addMenu(tr("&Window"));
    m_helpMenu     = m_menuBar->addMenu(tr("&Help"));

    buildAppMenu();
    buildWindowMenu();
    populateFileMenu();      // MAC.3
    populateHelpMenu();      // MAC.3
    populateEditMenu();      // MAC.4
    populateDocumentMenu();  // MAC.4
}

// ============================================================================
// App menu — About + Settings (Quit/Hide/Services auto-provided by Qt)
// ============================================================================

void MacMenuBar::buildAppMenu()
{
    // About SpeedyNote — opens the standalone AboutDialog. Qt's AboutRole
    // moves this to the top of the App menu regardless of insertion order.
    QAction* aboutAction = new QAction(tr("About SpeedyNote"), this);
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, []() {
        AboutDialog dlg(MainWindow::activeMainWindow());
        dlg.exec();
    });
    m_appMenu->addAction(aboutAction);

    // Settings… — reuses the registry's app.settings QAction. The Cmd+,
    // shortcut comes from ShortcutManager::setMacosDefault (MAC.1). Qt's
    // PreferencesRole moves the item into the App menu and renames it
    // "Settings…".
    //
    // MAC.3: the triggered() handler is wired centrally in
    // MainWindow::wireQActionDispatchers() (with activeMainWindow() dispatch).
    // We just set MenuRole and add the action to the menu here — no connect.
    QAction* settingsAction = ShortcutManager::instance()->action("app.settings");
    if (!settingsAction) {
        // Loud warning rather than silent skip: a missing app.settings would
        // mean the registry was tampered with (the action is unconditionally
        // registered in ShortcutManager::registerDefaults). Surfacing this in
        // the log is much easier to debug than a quietly-missing menu item.
        qWarning() << "[MacMenuBar] app.settings action missing from registry; "
                      "Settings menu item will not be added.";
        return;
    }
    settingsAction->setMenuRole(QAction::PreferencesRole);
    m_appMenu->addAction(settingsAction);

    // Quit / Hide SpeedyNote / Hide Others / Show All / Services submenu are
    // contributed automatically by Qt on macOS. Per QA Q3.3, About Qt is
    // intentionally NOT exposed.
}

// ============================================================================
// Window menu — Minimize/Zoom auto-provided by Qt on macOS
// ============================================================================

void MacMenuBar::buildWindowMenu()
{
    // Qt 6 contributes Minimize / Zoom / "Bring All to Front" / open-windows
    // list automatically for the macOS Window menu when the menu bar is
    // installed as the system menu bar. Tab navigation (Next Tab / Previous
    // Tab) is added in MAC.6.
    //
    // If a future Qt version stops auto-providing these, add them manually
    // with QAction::WindowMenuRole.
}

// ============================================================================
// MAC.3: File menu — New/Open/Save/Save As/Relink PDF/Export/Close
// ============================================================================

void MacMenuBar::populateFileMenu()
{
    auto* sm = ShortcutManager::instance();

    // Skip-on-null helper: avoids cascading qWarnings if a registry id is
    // mistyped (sm->action() warns once; QWidget::insertAction would warn
    // again on null). Kept local so the populate methods stay self-contained.
    auto add = [sm](QMenu* menu, const QString& id) {
        if (auto* a = sm->action(id)) menu->addAction(a);
    };

    // Group 1: New
    add(m_fileMenu, "file.new_paged");
    add(m_fileMenu, "file.new_edgeless");
    m_fileMenu->addSeparator();

    // Group 2: Open (Open Recent submenu deferred — see QA Q4.1)
    add(m_fileMenu, "file.open_pdf");
    add(m_fileMenu, "file.open_notebook");
    m_fileMenu->addSeparator();

    // Group 3: Save / Save As
    add(m_fileMenu, "file.save");
    add(m_fileMenu, "file.save_as");
    m_fileMenu->addSeparator();

    // Group 4: Relink PDF — not in ShortcutManager (no shortcut). Owned by
    // MacMenuBar; dispatches to the active MainWindow's existing handler.
    // Per-document text/enable sync (the overflow-menu version flips between
    // "Relink PDF..." and "Link PDF..." based on doc state and disables when
    // there is no doc) is intentionally deferred — the menu item stays as
    // "Relink PDF..." and is always enabled. The underlying dialog handles
    // both link and relink scenarios, so clicking on a doc with no PDF
    // reference just opens the link dialog.
    QAction* relink = m_fileMenu->addAction(tr("Relink PDF..."));
    connect(relink, &QAction::triggered, this, []() {
        if (auto* mw = MainWindow::activeMainWindow()) {
            // showPdfRelinkDialog is a private slot. Invoke via the meta-object
            // system so MacMenuBar (a platform subsystem outside MainWindow's
            // class boundary) can trigger it without expanding the public API.
            QMetaObject::invokeMethod(mw, "showPdfRelinkDialog",
                                      Qt::DirectConnection,
                                      Q_ARG(DocumentViewport*, mw->currentViewport()));
        }
    });
    m_fileMenu->addSeparator();

    // Group 5: Export / Share
    add(m_fileMenu, "file.export_pdf");
    add(m_fileMenu, "file.export");
    m_fileMenu->addSeparator();

    // Group 6: Close Tab (Quit auto-provided by Qt in App menu via QuitRole)
    add(m_fileMenu, "file.close_tab");
}

// ============================================================================
// MAC.3: Help menu — Keyboard Shortcuts + GitHub links
// ============================================================================

void MacMenuBar::populateHelpMenu()
{
    if (auto* a = ShortcutManager::instance()->action("app.keyboard_shortcuts")) {
        m_helpMenu->addAction(a);
    }
    m_helpMenu->addSeparator();

    QAction* visit = m_helpMenu->addAction(tr("Visit GitHub"));
    connect(visit, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/alpha-liu-01/SpeedyNote"));
    });

    QAction* report = m_helpMenu->addAction(tr("Report a Bug..."));
    connect(report, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/alpha-liu-01/SpeedyNote/issues/new"));
    });

    // The macOS system Help-menu search field is auto-injected at the top by
    // Qt + AppKit once the Help menu has at least one item. It enables
    // Cmd+Shift+/ to fuzzy-find any menu item app-wide.
}

// ============================================================================
// MAC.4: Edit menu — Undo/Redo + Cut/Copy/Paste/Delete + Find
// ============================================================================

void MacMenuBar::populateEditMenu()
{
    auto* sm = ShortcutManager::instance();
    auto add = [sm](QMenu* menu, const QString& id) {
        if (auto* a = sm->action(id)) menu->addAction(a);
    };

    // Group 1: Undo / Redo
    // edit.redo_alt (Cmd+Y) is intentionally omitted — it is the alternate
    // Redo binding only and is wired via the dispatcher so the keyboard
    // shortcut still fires; surfacing two Redo items in the menu would be
    // redundant and confusing.
    add(m_editMenu, "edit.undo");
    add(m_editMenu, "edit.redo");
    m_editMenu->addSeparator();

    // Group 2: Cut / Copy / Paste / Delete
    add(m_editMenu, "edit.cut");
    add(m_editMenu, "edit.copy");
    add(m_editMenu, "edit.paste");
    add(m_editMenu, "edit.delete");
    m_editMenu->addSeparator();

    // Group 3: Find
    // edit.select_all and edit.deselect are registered in ShortcutManager but
    // have no handlers anywhere today; intentionally not surfaced here. Add
    // them when the underlying feature lands.
    add(m_editMenu, "app.find");
    add(m_editMenu, "app.find_next");
    add(m_editMenu, "app.find_prev");

    // Qt + macOS may auto-inject "Start Dictation…" / "Emoji & Symbols" near
    // the bottom of the Edit menu once it has items; that is intended OS
    // behavior, not something we control.
}

// ============================================================================
// MAC.4: Document menu — Add / Insert / Delete page (PagedOnly)
// ============================================================================

void MacMenuBar::populateDocumentMenu()
{
    auto* sm = ShortcutManager::instance();
    auto add = [sm](QMenu* menu, const QString& id) {
        if (auto* a = sm->action(id)) menu->addAction(a);
    };

    // All three are PagedOnly. ShortcutManager::setActiveDocumentScope()
    // (plumbed in MAC.1; called from MainWindow's tab/viewport-change paths)
    // automatically toggles QAction::setEnabled() on each PagedOnly action so
    // the Document menu greys out atomically when the active tab is edgeless.
    add(m_documentMenu, "document.add_page");
    add(m_documentMenu, "document.insert_page");
    add(m_documentMenu, "document.delete_page");

    // Per QA Q4.4: navigation.go_to_page lives in the View menu (MAC.5),
    // not duplicated here.
}

#endif // Q_OS_MACOS
