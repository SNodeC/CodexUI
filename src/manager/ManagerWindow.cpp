// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ManagerWindow.h"

#include "codex/ui/UiStyle.h"
#include "manager/ProfileEditor.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace codexui::manager {
    namespace {
        QPushButton* button(const QString& text, QWidget* parent, std::function<void()> action) {
            auto* b = new QPushButton(text, parent);
            b->setCursor(Qt::PointingHandCursor);
            b->setMinimumHeight(34);
            QObject::connect(b, &QPushButton::clicked, parent, std::move(action));
            return b;
        }
        QPlainTextEdit* textView(QTabWidget* tabs, const QString& title) {
            auto* view = new QPlainTextEdit(tabs);
            view->setReadOnly(true);
            view->setLineWrapMode(QPlainTextEdit::WidgetWidth);
            view->setMaximumBlockCount(5000);
            view->setAccessibleName(title);
            tabs->addTab(view, title);
            return view;
        }
        class ProfileCard final : public QFrame {
        public:
            ProfileCard(ProfileRuntime* runtime,
                        QWidget* parent,
                        std::function<void()> detail,
                        std::function<void()> edit,
                        std::function<void()> stop,
                        std::function<void()> remove)
                : QFrame(parent)
                , runtime_(runtime) {
                const auto& p = runtime->profile();
                setObjectName("profileCard");
                setMinimumWidth(280);
                setMaximumWidth(520);
                setStyleSheet(
                    QString("QFrame#profileCard {background:%1; border:1px solid %2; border-top:4px solid %3; border-radius:14px;}")
                        .arg(UiStyle::panel, UiStyle::divider, accentColor(p.accent)));
                auto* shadow = new QGraphicsDropShadowEffect(this);
                shadow->setBlurRadius(18);
                shadow->setOffset(0, 4);
                auto shadowColor = QColor(UiStyle::primary);
                shadowColor.setAlpha(18);
                shadow->setColor(shadowColor);
                setGraphicsEffect(shadow);
                auto* layout = new QVBoxLayout(this);
                layout->setContentsMargins(22, 18, 22, 20);
                layout->setSpacing(12);
                auto* row = new QHBoxLayout;
                auto* name = UiStyle::makeLabel(p.name, "panelHeader", this);
                name->setTextInteractionFlags(Qt::NoTextInteraction);
                name->setWordWrap(true);
                name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                row->addWidget(name, 1);
                auto* menu = new QToolButton(this);
                menu->setText("···");
                menu->setAccessibleName("Actions for " + p.name);
                menu->setPopupMode(QToolButton::InstantPopup);
                auto* actions = new QMenu(menu);
                actions->addAction("Configure profile…", this, std::move(edit));
                actions->addAction("Stop owned components…", this, std::move(stop));
                actions->addSeparator();
                actions->addAction("Remove registration…", this, std::move(remove));
                menu->setMenu(actions);
                row->addWidget(menu);
                layout->addLayout(row);
                state_ = UiStyle::makeLabel(runtime->status(), "body", this);
                layout->addWidget(state_);
                auto* description =
                    UiStyle::makeLabel(p.description.isEmpty() ? "An independent Codex environment" : p.description, "meta", this);
                layout->addWidget(description);
                auto* where = UiStyle::makeLabel(p.bridge.label() + "  ·  " + transportLabel(p.frontend.transport), "meta", this);
                layout->addWidget(where);
                auto* footer = new QHBoxLayout;
                open_ = button("Start && open", this, [runtime] {
                    runtime->openUi();
                });
                open_->setObjectName("primaryButton");
                footer->addWidget(open_, 1);
                footer->addWidget(button("Details →", this, std::move(detail)));
                layout->addLayout(footer);
                update();
            }
            void update() {
                state_->setText((runtime_->ready() ? "●  " : "○  ") + runtime_->status());
                open_->setText(runtime_->ready()
                                   ? (runtime_->components().value("CodexUI").startsWith("Running") ? "Show CodexUI" : "Open CodexUI")
                               : runtime_->busy() ? "Working…"
                                                  : "Start && open");
                open_->setEnabled(!runtime_->busy());
            }

        private:
            ProfileRuntime* runtime_;
            QLabel* state_;
            QPushButton* open_;
        };
    } // namespace
    ManagerWindow::ManagerWindow(QString path, QWidget* parent)
        : QMainWindow(parent)
        , store_(std::move(path)) {
        setWindowTitle("CodexManager · Profiles");
        resize(1240, 830);
        setMinimumSize(740, 520);
        setWindowIcon(QIcon::fromTheme("codex-manager", style()->standardIcon(QStyle::SP_ComputerIcon)));
        setStyleSheet(
            UiStyle::applicationStyleSheet() +
            QString("QMainWindow {background:%1;} QLineEdit {padding:9px; border:1px solid %2; border-radius:8px;} "
                    "QPushButton {padding:7px 13px; border:1px solid %2; border-radius:7px; background:%3; color:%4;} "
                    "QPushButton:hover {background:%5;} QPushButton:focus {border:2px solid %6;} "
                    "QPushButton#primaryButton {background:%6; color:%3; border-color:%6;} "
                    "QPushButton:disabled {color:%7;} QLabel#hero {font-size:30px; font-weight:650;} "
                    "QLabel#detailTitle {font-size:26px; font-weight:650;} QTabWidget::pane {border:1px solid %2; border-radius:8px; "
                    "background:%3;} "
                    "QPlainTextEdit {border:0; background:%3; padding:14px; color:%4;} QGroupBox {padding-top:22px; margin-top:12px;}")
                .arg(UiStyle::appBackground,
                     UiStyle::divider,
                     UiStyle::panel,
                     UiStyle::primary,
                     UiStyle::hover,
                     UiStyle::primaryAction,
                     UiStyle::secondary));
        auto* central = new QWidget(this);
        setCentralWidget(central);
        auto* outer = new QVBoxLayout(central);
        outer->setContentsMargins(32, 24, 32, 24);
        outer->setSpacing(22);
        auto* bar = new QHBoxLayout;
        auto* brand = UiStyle::makeLabel("◈  CodexManager", "panelHeader", central);
        bar->addWidget(brand);
        bar->addStretch();
        bar->addWidget(button("Import…", central, [this] {
            importProfiles();
        }));
        auto* add = button("＋ New profile", central, [this] {
            createOrEdit();
        });
        add->setObjectName("primaryButton");
        bar->addWidget(add);
        auto* menuButton = new QToolButton(central);
        menuButton->setText("☰");
        menuButton->setAccessibleName("Manager menu");
        menuButton->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(menuButton);
        menu->addAction("Open profile storage", this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(store_.filePath()).absolutePath()));
        });
        menu->addSeparator();
        menu->addAction("Quit manager…", this, &ManagerWindow::quitManager);
        menuButton->setMenu(menu);
        bar->addWidget(menuButton);
        outer->addLayout(bar);
        pages_ = new QStackedWidget(central);
        outer->addWidget(pages_, 1);
        overview_ = new QWidget(pages_);
        pages_->addWidget(overview_);
        auto* overviewLayout = new QVBoxLayout(overview_);
        overviewLayout->setContentsMargins(0, 0, 0, 0);
        overviewLayout->setSpacing(16);
        auto* hero = new QLabel("Your work, in parallel.", overview_);
        hero->setObjectName("hero");
        overviewLayout->addWidget(hero);
        overviewLayout->addWidget(UiStyle::makeLabel("Independent profiles. Clear ownership. One place to start.", "meta", overview_));
        auto* searchRow = new QHBoxLayout;
        summary_ = new QLabel(overview_);
        searchRow->addWidget(summary_, 1);
        search_ = new QLineEdit(overview_);
        search_->setPlaceholderText("Search profiles…");
        search_->setAccessibleName("Search profiles");
        search_->setMaximumWidth(310);
        searchRow->addWidget(search_);
        overviewLayout->addLayout(searchRow);
        auto* area = new QScrollArea(overview_);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setStyleSheet("QScrollArea {background:transparent;}");
        gridWidget_ = new QWidget(area);
        grid_ = new QGridLayout(gridWidget_);
        grid_->setContentsMargins(8, 8, 18, 18);
        grid_->setSpacing(22);
        grid_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        area->setWidget(gridWidget_);
        overviewLayout->addWidget(area, 1);
        connect(search_, &QLineEdit::textChanged, this, [this] {
            reflow();
        });

        auto* details = new QWidget(pages_);
        pages_->addWidget(details);
        auto* dl = new QVBoxLayout(details);
        dl->setContentsMargins(0, 0, 0, 0);
        dl->setSpacing(16);
        auto* navigation = new QHBoxLayout;
        navigation->addWidget(button("← All profiles", details, [this] {
            pages_->setCurrentIndex(0);
            reflow();
        }));
        navigation->addStretch();
        navigation->addWidget(button("Configure…", details, [this] {
            createOrEdit(selected_);
        }));
        navigation->addWidget(button("Export…", details, [this] {
            exportProfile(selected_);
        }));
        dl->addLayout(navigation);
        detailTitle_ = new QLabel(details);
        detailTitle_->setObjectName("detailTitle");
        detailTitle_->setTextFormat(Qt::PlainText);
        detailTitle_->setWordWrap(true);
        detailTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        dl->addWidget(detailTitle_);
        detailState_ = UiStyle::makeLabel({}, "body", details);
        dl->addWidget(detailState_);
        auto* actions = new QHBoxLayout;
        auto* open = button("Start && open CodexUI", details, [this] {
            if (auto* r = runtime(selected_)) {
                r->openUi();
            }
        });
        open->setObjectName("primaryButton");
        actions->addWidget(open);
        actions->addWidget(button("Check readiness", details, [this] {
            if (auto* r = runtime(selected_)) {
                r->check();
            }
        }));
        actions->addWidget(button("Refresh processes", details, [this] {
            if (auto* r = runtime(selected_)) {
                r->refresh();
            }
        }));
        actions->addStretch();
        actions->addWidget(button("Stop profile…", details, [this] {
            auto* r = runtime(selected_);
            if (r &&
                QMessageBox::question(
                    this,
                    "Stop owned components?",
                    "Stop this profile's owned CodexUI, bridge and app-server services? Externally managed components are left running.") ==
                    QMessageBox::Yes) {
                r->stop();
            }
        }));
        dl->addLayout(actions);
        auto* tabs = new QTabWidget(details);
        dl->addWidget(tabs, 1);
        auto* overview = new QWidget(tabs);
        auto* ol = new QVBoxLayout(overview);
        ol->setContentsMargins(22, 22, 22, 22);
        topology_ = UiStyle::makeLabel({}, "body", overview);
        ol->addWidget(topology_);
        componentDetails_ = UiStyle::makeLabel({}, "body", overview);
        ol->addWidget(componentDetails_);
        ol->addStretch();
        auto* technical = new QToolButton(overview);
        technical->setText("Technical details");
        technical->setCheckable(true);
        technical->setArrowType(Qt::RightArrow);
        technical->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        ol->addWidget(technical);
        technicalDetails_ = UiStyle::makeLabel({}, "meta", overview);
        technicalDetails_->hide();
        ol->addWidget(technicalDetails_);
        connect(technical, &QToolButton::toggled, this, [this, technical](bool checked) {
            technical->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
            technicalDetails_->setVisible(checked);
        });
        tabs->addTab(overview, "Overview");
        configuration_ = textView(tabs, "Configuration");
        instructions_ = textView(tabs, "Setup && startup");
        auto* logPage = new QWidget(tabs);
        auto* ll = new QVBoxLayout(logPage);
        auto* logActions = new QHBoxLayout;
        logActions->addWidget(button("Read service logs", logPage, [this] {
            if (auto* r = runtime(selected_)) {
                r->readLogs();
            }
        }));
        logActions->addWidget(button("Copy", logPage, [this] {
            QApplication::clipboard()->setText(logs_->toPlainText());
        }));
        logActions->addWidget(button("Export…", logPage, [this] {
            const auto path = QFileDialog::getSaveFileName(this, "Export profile log", {}, "Text (*.log *.txt)");
            if (!path.isEmpty()) {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(logs_->toPlainText().toUtf8()) < 0 || !file.commit()) {
                    QMessageBox::warning(this, "Export failed", file.errorString());
                }
            }
        }));
        logActions->addStretch();
        auto* pause = new QToolButton(logPage);
        pause->setText("Pause updates");
        pause->setCheckable(true);
        logActions->addWidget(pause);
        ll->addLayout(logActions);
        auto* filter = new QLineEdit(logPage);
        filter->setPlaceholderText("Filter displayed log lines…");
        filter->setAccessibleName("Filter logs");
        ll->addWidget(filter);
        logs_ = new QPlainTextEdit(logPage);
        logs_->setReadOnly(true);
        logs_->setMaximumBlockCount(5000);
        logs_->setAccessibleName("Profile logs");
        ll->addWidget(logs_, 1);
        tabs->addTab(logPage, "Logs");
        logFilter_ = filter;
        logPause_ = pause;
        connect(filter, &QLineEdit::textChanged, this, &ManagerWindow::updateLogs);
        connect(pause, &QToolButton::toggled, this, &ManagerWindow::updateLogs);
        // Window-local presentation refresh; runtime remains the only live-state authority.
        connect(tabs, &QTabWidget::currentChanged, this, [this](int) {
            updateLogs();
        });

        tray_ = new QSystemTrayIcon(windowIcon(), this);
        tray_->setToolTip("CodexManager · running profiles stay independent");
        auto* trayMenu = new QMenu(this);
        trayMenu->addAction("Show CodexManager", this, [this] {
            showNormal();
            raise();
            activateWindow();
        });
        trayMenu->addAction("Quit manager…", this, &ManagerWindow::quitManager);
        tray_->setContextMenu(trayMenu);
        connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                showNormal();
                raise();
                activateWindow();
            }
        });
        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            tray_->show();
        }
        QString error;
        profiles_ = store_.load(&error);
        if (!error.isEmpty()) {
            storeValid_ = false;
            QTimer::singleShot(0, this, [this, error] {
                QMessageBox::critical(this, "Profile store could not be loaded", error + "\nThe store will not be overwritten.");
            });
        }
        rebuild();
    }
    ManagerWindow::~ManagerWindow() {
        // Runtime helpers outlive their services, but must not outlive this presentation.
        for (auto* r : runtimes_) {
            r->disconnect(this);
            delete r;
        }
        runtimes_.clear();
    }
    ProfileRuntime* ManagerWindow::runtime(const QString& id) const {
        return runtimes_.value(id, nullptr);
    }
    bool ManagerWindow::persist(const QVector<Profile>& profiles) {
        bool saved = false;
        if (!storeValid_) {
            QMessageBox::warning(this, "Store unavailable", "Resolve the profile-store loading error before saving.");
        } else {
            QString error;
            saved = store_.save(profiles, &error);
            if (saved) {
                profiles_ = profiles;
                rebuild();
            } else {
                QMessageBox::warning(this, "Cannot save profiles", error);
            }
        }
        return saved;
    }
    void ManagerWindow::rebuild() {
        for (auto* card : cards_) {
            delete card;
        }
        cards_.clear();
        QStringList retained;
        for (const auto& p : profiles_) {
            retained << p.id;
            auto* r = runtime(p.id);
            if (r && r->profile().toJson() != p.toJson()) {
                delete runtimes_.take(p.id);
                r = nullptr;
            }
            if (!r) {
                r = new ProfileRuntime(p, this);
                runtimes_.insert(p.id, r);
                QTimer::singleShot(0, r, &ProfileRuntime::refresh);
                connect(r, &ProfileRuntime::changed, this, [this, id = p.id] {
                    updateProfile(id);
                });
                connect(r, &ProfileRuntime::logAdded, this, [this, id = p.id](const QString&) {
                    if (selected_ == id) {
                        updateLogs();
                    }
                });
            }
            auto* card = new ProfileCard(
                r,
                gridWidget_,
                [this, id = p.id] {
                    showProfile(id);
                },
                [this, id = p.id] {
                    createOrEdit(id);
                },
                [this, r] {
                    if (QMessageBox::question(this, "Stop profile?", "Stop only this profile's owned components?") == QMessageBox::Yes) {
                        r->stop();
                    }
                },
                [this, id = p.id] {
                    remove(id);
                });
            cards_.insert(p.id, card);
        }
        for (const auto& id : runtimes_.keys()) {
            if (!retained.contains(id)) {
                delete runtimes_.take(id);
            }
        }
        reflow();
        updateProfile(selected_);
    }
    void ManagerWindow::reflow() {
        while (auto* item = grid_->takeAt(0)) {
            delete item;
        }
        int shown = 0;
        const int columns = std::max(1, std::min(3, (width() - 80) / 330));
        for (const auto& p : profiles_) {
            auto* card = cards_.value(p.id);
            const bool visible = (p.name + ' ' + p.description).contains(search_->text(), Qt::CaseInsensitive);
            card->setVisible(visible);
            if (visible) {
                grid_->addWidget(card, shown / columns, shown % columns);
                ++shown;
            }
        }
        if (profiles_.isEmpty()) {
            auto* empty = gridWidget_->findChild<QLabel*>("emptyProfiles");
            if (!empty) {
                empty = UiStyle::makeLabel("A fresh place to begin\nCreate a profile for your daily work, an experiment, or a remote Codex "
                                           "environment. Each can run independently.",
                                           "body",
                                           gridWidget_);
                empty->setObjectName("emptyProfiles");
            }
            empty->show();
            grid_->addWidget(empty, 0, 0);
        } else if (auto* empty = gridWidget_->findChild<QLabel*>("emptyProfiles")) {
            empty->hide();
        }
        int ready = 0;
        for (auto* r : runtimes_) {
            ready += r->ready();
        }
        summary_->setText(QString::number(profiles_.size()) + " profiles  ·  " + QString::number(ready) + " ready");
    }
    void ManagerWindow::updateProfile(const QString& id) {
        if (auto* card = static_cast<ProfileCard*>(cards_.value(id))) {
            card->update();
        }
        int ready = 0;
        for (auto* r : runtimes_) {
            ready += r->ready();
        }
        summary_->setText(QString::number(profiles_.size()) + " profiles  ·  " + QString::number(ready) + " ready");
        auto* r = runtime(id);
        if (id == selected_ && r) {
            detailState_->setText(r->status());
            QStringList details;
            const auto components = r->components();
            for (auto it = components.cbegin(); it != components.cend(); ++it) {
                details << it.key() + "    " + it.value();
            }
            componentDetails_->setText(details.join("\n\n"));
        }
    }
    void ManagerWindow::updateLogs() {
        auto* r = runtime(selected_);
        if (!logPause_->isChecked() && r) {
            QStringList lines = r->logs().split('\n');
            if (!logFilter_->text().isEmpty()) {
                lines = lines.filter(logFilter_->text(), Qt::CaseInsensitive);
            }
            const bool follow = logs_->verticalScrollBar()->value() >= logs_->verticalScrollBar()->maximum() - 2;
            const int position = logs_->verticalScrollBar()->value();
            logs_->setPlainText(lines.join('\n'));
            logs_->verticalScrollBar()->setValue(follow ? logs_->verticalScrollBar()->maximum() : position);
        }
    }
    void ManagerWindow::showProfile(const QString& id) {
        auto* r = runtime(id);
        if (r) {
            selected_ = id;
            detailTitle_->setText(r->profile().name);
            topology_->setText("CodexUI · this machine  →  Bridge · " + r->profile().bridge.label() + "  →  App-server · " +
                               (r->profile().provider.transport == "stdio" ? r->profile().bridge.label() : r->profile().appServer.label()));
            const auto& p = r->profile();
            technicalDetails_->setText("CODEX_HOME (app-server host): " + p.codexHome + "\nFrontend socket/host: " +
                                       (p.frontend.transport == "unix" ? p.frontend.socketPath : p.frontend.host) +
                                       "\nUI activation socket: " + p.runtimeSocket());
            configuration_->setPlainText("PROFILE\n" + p.name + "\n" + p.description + "\n\nCODEX_HOME\n" + p.codexHome + "\n\nBRIDGE\n" +
                                         ownershipLabel(p.bridge.ownership) + " · " + p.bridge.label() + "\n" +
                                         transportLabel(p.frontend.transport) + "\n\nAPP-SERVER\n" + transportLabel(p.provider.transport) +
                                         "\n" +
                                         (p.provider.transport == "stdio" ? "Owned by bridge" : ownershipLabel(p.appServer.ownership)) +
                                         "\n\nUse Configure to edit this profile. Paths belong to the component's host.");
            instructions_->setPlainText(LaunchPlan::forProfile(p).instructions(p));
            logs_->setPlainText(r->logs());
            pages_->setCurrentIndex(1);
            updateProfile(id);
        }
    }
    void ManagerWindow::createOrEdit(const QString& id) {
        auto* r = runtime(id);
        if (r && (r->busy() || r->hasOwnedProcesses())) {
            QMessageBox::information(this,
                                     "Profile is in use",
                                     "Stop owned components and verify their status before changing this profile's launch configuration.");
        } else {
            ProfileEditor editor(r ? r->profile() : Profile::create(), profiles_, this);
            if (editor.exec() == QDialog::Accepted) {
                auto next = profiles_;
                const auto p = editor.profile();
                bool replaced = false;
                for (auto& item : next) {
                    if (item.id == p.id) {
                        item = p;
                        replaced = true;
                    }
                }
                if (!replaced) {
                    next.push_back(p);
                }
                if (persist(next)) {
                    showProfile(p.id);
                }
            }
        }
    }
    void ManagerWindow::remove(const QString& id) {
        auto* r = runtime(id);
        if (r) {
            if (r->busy() || r->hasOwnedProcesses()) {
                QMessageBox::information(
                    this, "Profile is in use", "Stop owned components and verify their status before removing the registration.");
            } else if (QMessageBox::question(this,
                                             "Remove registration?",
                                             "Remove this profile from CodexManager? Its CODEX_HOME, conversations, authentication and "
                                             "external services will not be deleted.") == QMessageBox::Yes) {
                auto next = profiles_;
                next.erase(std::remove_if(next.begin(),
                                          next.end(),
                                          [&](const Profile& p) {
                                              return p.id == id;
                                          }),
                           next.end());
                if (persist(next)) {
                    selected_.clear();
                    pages_->setCurrentIndex(0);
                }
            }
        }
    }
    void ManagerWindow::importProfiles() {
        const auto path = QFileDialog::getOpenFileName(this, "Import profile definitions", {}, "Profiles (*.json)");
        if (!path.isEmpty()) {
            QString error;
            const auto imported = ProfileStore(path).load(&error);
            auto next = profiles_;
            for (auto it = imported.cbegin(); error.isEmpty() && it != imported.cend(); ++it) {
                if (std::any_of(next.cbegin(),
                                next.cend(),
                                [&](const Profile& x) {
                                    return x.id == it->id;
                                }) ||
                    !it->errors(next).isEmpty()) {
                    error = "A profile ID, home, or endpoint conflicts with an existing registration. No profiles were imported.";
                } else {
                    next.push_back(*it);
                }
            }
            if (error.isEmpty()) {
                persist(next);
            } else {
                QMessageBox::warning(this, "Import failed", error);
            }
        }
    }
    void ManagerWindow::exportProfile(const QString& id) {
        auto* r = runtime(id);
        if (r) {
            const auto path = QFileDialog::getSaveFileName(this, "Export profile definition", {}, "Profiles (*.json)");
            if (!path.isEmpty()) {
                QString error;
                if (!ProfileStore(path).save({r->profile()}, &error)) {
                    QMessageBox::warning(this, "Export failed", error);
                }
            }
        }
    }
    void ManagerWindow::quitManager() {
        if (QMessageBox::question(this,
                                  "Quit CodexManager?",
                                  "Quit the manager and leave profile services and CodexUI windows running? Use Stop profile first if you "
                                  "want to shut them down.") == QMessageBox::Yes) {
            qApp->quit();
        }
    }
    void ManagerWindow::closeEvent(QCloseEvent* event) {
        event->ignore();
        if (tray_->isVisible()) {
            hide();
        } else {
            showMinimized();
        }
    }
    void ManagerWindow::resizeEvent(QResizeEvent* event) {
        QMainWindow::resizeEvent(event);
        if (grid_) {
            reflow();
        }
    }
} // namespace codexui::manager
