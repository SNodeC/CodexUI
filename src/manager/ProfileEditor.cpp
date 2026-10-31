// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ProfileEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <optional>

namespace codexui::manager {
    namespace {
        QWidget* page(QTabWidget* tabs, const QString& name, QVBoxLayout** layout) {
            auto* area = new QScrollArea(tabs);
            area->setWidgetResizable(true);
            area->setFrameShape(QFrame::NoFrame);
            auto* widget = new QWidget(area);
            *layout = new QVBoxLayout(widget);
            (*layout)->setContentsMargins(20, 20, 20, 20);
            (*layout)->setSpacing(18);
            area->setWidget(widget);
            tabs->addTab(area, name);
            return widget;
        }
        QLineEdit* line(QFormLayout* form,
                        const QString& label,
                        QString& value,
                        QVector<std::function<void(Profile&)>>& readers,
                        const QString& placeholder = {},
                        std::optional<QFileDialog::FileMode> localSelection = std::nullopt,
                        QList<QPushButton*>* hostPaths = nullptr) {
            auto* edit = new QLineEdit(value, form->parentWidget());
            edit->setPlaceholderText(placeholder);
            edit->setAccessibleName(label);
            if (localSelection) {
                auto* row = new QWidget(form->parentWidget());
                auto* layout = new QHBoxLayout(row);
                layout->setContentsMargins(0, 0, 0, 0);
                auto* browse = new QPushButton("Browse local…", row);
                browse->setAccessibleName("Choose local " + label);
                browse->setToolTip(browse->accessibleName());
                if (hostPaths) {
                    hostPaths->push_back(browse);
                }
                layout->addWidget(edit, 1);
                layout->addWidget(browse);
                QWidget::setTabOrder(edit, browse);
                auto* caption = new QLabel(label);
                caption->setBuddy(edit);
                form->addRow(caption, row);
                QObject::connect(browse, &QPushButton::clicked, edit, [edit, label, mode = *localSelection] {
                    auto* dialog = new QFileDialog(edit, "Choose local " + label, edit->text());
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->setWindowModality(Qt::WindowModal);
                    dialog->setFileMode(mode);
                    dialog->setAcceptMode(mode == QFileDialog::AnyFile ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
                    dialog->setFilter(dialog->filter() | QDir::System);
                    dialog->setOption(QFileDialog::ShowDirsOnly, mode == QFileDialog::Directory);
                    dialog->setSupportedSchemes({"file"});
                    QObject::connect(dialog, &QFileDialog::fileSelected, edit, &QLineEdit::setText);
                    dialog->open();
                });
            } else {
                form->addRow(label, edit);
            }
            readers.push_back([edit, &value](Profile&) {
                value = edit->text().trimmed();
            });
            return edit;
        }
    } // namespace
    ProfileEditor::ProfileEditor(Profile p, QVector<Profile> others, QWidget* parent)
        : ScrollFormDialog(parent)
        , profile_(std::move(p))
        , others_(std::move(others)) {
        setWindowTitle("Configure profile");
        resize(780, 700);
        setMinimumSize(560, 440);
        auto* layout = new QVBoxLayout(this);
        auto* heading = UiStyle::makeLabel("Make room for another way of working", "panelHeader", this);
        layout->addWidget(heading);
        auto* tabs = new QTabWidget(this);
        layout->addWidget(tabs, 1);
        QVBoxLayout* content;
        QList<QPushButton*> bridgePaths, appPaths;
        auto* identity = page(tabs, "Identity", &content);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        content->addLayout(form);
        line(form, "Name", profile_.name, readers_);
        line(form, "Description", profile_.description, readers_);
        line(form, "CODEX_HOME", profile_.codexHome, readers_, "Absolute path on the app-server host", QFileDialog::Directory, &appPaths);
        auto* accent = new QComboBox(identity);
        for (const auto& key : {"blue", "green", "purple", "teal", "amber", "orange"}) {
            accent->addItem(QString::fromLatin1(key), QString::fromLatin1(key));
        }
        accent->setCurrentIndex(accent->findData(profile_.accent));
        form->addRow("Accent", accent);
        readers_.push_back([this, accent](Profile&) {
            profile_.accent = accent->currentData().toString();
        });
        auto* share = new QCheckBox("Link the existing global AGENTS.md for a locally managed home", identity);
        share->setChecked(profile_.shareInstructions);
        content->addWidget(share);
        readers_.push_back([this, share](Profile&) {
            profile_.shareInstructions = share->isChecked();
        });
        content->addWidget(UiStyle::makeLabel(
            "Creating a profile registers its configuration. Starting a locally managed backend creates its home if needed. Existing files "
            "and credentials are never overwritten. Use Setup & startup for authentication and remote preparation.",
            "meta",
            identity));
        content->addStretch();

        auto hostForm = [this](QVBoxLayout* box, Host& host, const QString& title, QString& executable, QList<QPushButton*>& paths) {
            auto* group = new QGroupBox(title, this);
            auto* f = new QFormLayout(group);
            box->addWidget(group);
            auto* ownership = new QComboBox(group);
            for (auto mode : {Ownership::Local, Ownership::Ssh, Ownership::External}) {
                ownership->addItem(ownershipLabel(mode), int(mode));
            }
            ownership->setCurrentIndex(ownership->findData(int(host.ownership)));
            f->addRow("Process ownership", ownership);
            readers_.push_back([ownership, &host](Profile&) {
                host.ownership = Ownership(ownership->currentData().toInt());
            });
            line(f, "Host / SSH alias", host.sshTarget, readers_, "Empty for this machine; user@host or SSH alias for remote");
            line(f,
                 "SSH identity file",
                 host.identityFile,
                 readers_,
                 "Optional local key path; SSH config and agent are also supported",
                 QFileDialog::ExistingFile);
            line(f, "Executable", executable, readers_, {}, QFileDialog::ExistingFile, &paths);
            line(f,
                 "Working directory",
                 host.workingDirectory,
                 readers_,
                 "Optional absolute path on this component's host",
                 QFileDialog::Directory,
                 &paths);
            auto* environment = new QPlainTextEdit(group);
            QStringList variables;
            for (auto it = host.environment.cbegin(); it != host.environment.cend(); ++it) {
                variables << it.key() + '=' + it.value();
            }
            environment->setPlainText(variables.join('\n'));
            environment->setMaximumHeight(110);
            environment->setPlaceholderText("Non-secret KEY=value entries, one per line");
            environment->setAccessibleName(title + " environment");
            f->addRow("Environment", environment);
            readers_.push_back([environment, &host](Profile&) {
                host.environment.clear();
                for (const auto& entry : environment->toPlainText().split('\n')) {
                    const auto separator = entry.indexOf('=');
                    if (!entry.trimmed().isEmpty()) {
                        host.environment.insert(separator > 0 ? entry.left(separator).trimmed() : entry,
                                                separator > 0 ? entry.mid(separator + 1) : QString{});
                    }
                }
            });
            return group;
        };
        auto endpointForm = [this, &bridgePaths, &appPaths](QVBoxLayout* box, Endpoint& e, bool provider) {
            auto* group = new QGroupBox(provider ? "Bridge ↔ app-server" : "CodexUI ↔ bridge", this);
            auto* f = new QFormLayout(group);
            box->addWidget(group);
            auto* transport = new QComboBox(group);
            const auto keys = provider ? QStringList{"stdio", "unix", "websocket-ipv4", "websocket-ipv6"} : frontendTransports();
            for (const auto& key : keys) {
                transport->addItem(transportLabel(key), key);
            }
            transport->setCurrentIndex(transport->findData(e.transport));
            f->addRow("Transport", transport);
            readers_.push_back([transport, &e](Profile&) {
                e.transport = transport->currentData().toString();
            });
            line(f, "Connect host / Bluetooth address", e.host, readers_);
            line(f, "Listener bind host", e.bindHost, readers_, "Listener-side address; independent of the client's connect host");
            auto* port = new QSpinBox(group);
            port->setRange(1, 65535);
            port->setValue(e.port);
            f->addRow("Port", port);
            readers_.push_back([port, &e](Profile&) {
                e.port = port->value();
            });
            line(f, "Unix socket", e.socketPath, readers_, {}, QFileDialog::AnyFile, provider ? &appPaths : &bridgePaths);
            line(f, "WebSocket path", e.webSocketPath, readers_);
            if (!provider) {
                line(f,
                     "Bridge TLS certificate",
                     e.certificate,
                     readers_,
                     "Path on the bridge host",
                     QFileDialog::ExistingFile,
                     &bridgePaths);
                line(f,
                     "Bridge TLS private key",
                     e.privateKey,
                     readers_,
                     "Path on the bridge host; never paste key material",
                     QFileDialog::ExistingFile,
                     &bridgePaths);
                line(f,
                     "Client CA certificate",
                     e.caCertificate,
                     readers_,
                     "Local path used by CodexUI and the readiness client",
                     QFileDialog::ExistingFile);
                line(f, "TLS server name (SNI)", e.serverName, readers_);
                auto* channel = new QSpinBox(group);
                channel->setRange(1, 30);
                channel->setValue(e.channel);
                f->addRow("RFCOMM channel", channel);
                readers_.push_back([channel, &e](Profile&) {
                    e.channel = channel->value();
                });
            }
            const auto update = [f, transport, provider] {
                const QString t = transport->currentData().toString();
                for (int row = 1; row < f->rowCount(); ++row) {
                    f->setRowVisible(row, t != "stdio");
                }
                if (t != "stdio") {
                    f->setRowVisible(1, t != "unix");
                    f->setRowVisible(2, t != "unix");
                    f->setRowVisible(3, t.contains("ipv"));
                    f->setRowVisible(4, t == "unix");
                    f->setRowVisible(5, t.contains("websocket") || t.startsWith("wss"));
                }
                if (!provider && t != "stdio") {
                    const bool secured = t.contains("tls") || t.startsWith("wss");
                    for (int i = 6; i <= 9; ++i) {
                        f->setRowVisible(i, secured);
                    }
                    f->setRowVisible(10, t.startsWith("rfcomm"));
                }
            };
            connect(transport, &QComboBox::currentIndexChanged, group, update);
            update();
            return transport;
        };
        auto* bridgePage = page(tabs, "Bridge", &content);
        auto* bridgeHost = hostForm(content, profile_.bridge, "Bridge process", profile_.bridgeExecutable, bridgePaths);
        auto* frontendTransport = endpointForm(content, profile_.frontend, false);
        content->addStretch();
        auto* serverPage = page(tabs, "App-server", &content);
        auto* provider = endpointForm(content, profile_.provider, true);
        auto* appHost =
            hostForm(content, profile_.appServer, "App-server process (non-stdio modes)", profile_.appServerExecutable, appPaths);
        auto* appInfo = UiStyle::makeLabel("Stdio: the bridge owns its child and uses CODEX_HOME from Identity. The executable below still "
                                           "selects that child. For Unix or network connections, choose who owns the separate app-server. "
                                           "SSH management requires systemd user services and a verified SSH host key.",
                                           "meta",
                                           serverPage);
        content->addWidget(appInfo);
        auto* bridgeForm = qobject_cast<QFormLayout*>(bridgeHost->layout());
        const auto updateAppHost = [appHost, provider, bridgeForm, bridgePaths, appPaths] {
            const bool stdio = provider->currentData() == "stdio";
            appHost->setTitle(stdio ? "Bridge-owned app-server executable (host follows bridge)" : "App-server process");
            auto* form = qobject_cast<QFormLayout*>(appHost->layout());
            for (int row = 0; row < form->rowCount(); ++row) {
                form->setRowVisible(row, !stdio || row == 3);
            }
            const auto local = [](QFormLayout* host) {
                return qobject_cast<QComboBox*>(host->itemAt(0, QFormLayout::FieldRole)->widget())->currentData() ==
                           int(Ownership::Local) &&
                       qobject_cast<QLineEdit*>(host->itemAt(1, QFormLayout::FieldRole)->widget())->text().trimmed().isEmpty();
            };
            for (auto* browse : bridgePaths) {
                browse->setVisible(local(bridgeForm));
            }
            for (auto* browse : appPaths) {
                browse->setVisible(local(stdio ? bridgeForm : form));
            }
        };
        connect(provider, &QComboBox::currentIndexChanged, appHost, updateAppHost);
        for (auto* host : {bridgeForm, qobject_cast<QFormLayout*>(appHost->layout())}) {
            connect(qobject_cast<QComboBox*>(host->itemAt(0, QFormLayout::FieldRole)->widget()),
                    &QComboBox::currentIndexChanged,
                    appHost,
                    updateAppHost);
            connect(qobject_cast<QLineEdit*>(host->itemAt(1, QFormLayout::FieldRole)->widget()),
                    &QLineEdit::textChanged,
                    appHost,
                    updateAppHost);
        }
        updateAppHost();
        content->addStretch();
        auto* tools = page(tabs, "Local tools", &content);
        auto* tf = new QFormLayout;
        content->addLayout(tf);
        auto* uiExecutable = line(tf, "CodexUI executable", profile_.uiExecutable, readers_, {}, QFileDialog::ExistingFile);
        auto* probeExecutable = line(tf, "Readiness client executable", profile_.probeExecutable, readers_, {}, QFileDialog::ExistingFile);
        auto* capabilities = new QLabel("Checking locally installed frontend transports…", tools);
        capabilities->setWordWrap(true);
        content->addWidget(capabilities);
        auto* discover = new QPushButton("Check installed transports", tools);
        content->addWidget(discover);
        auto* uiHelp = new QProcess(this);
        auto* probeHelp = new QProcess(this);
        uiHelp->setProcessChannelMode(QProcess::MergedChannels);
        probeHelp->setProcessChannelMode(QProcess::MergedChannels);
        auto* deadline = new QTimer(this);
        deadline->setSingleShot(true);
        deadline->setInterval(10000);
        connect(deadline, &QTimer::timeout, this, [uiHelp, probeHelp, capabilities, discover] {
            uiHelp->kill();
            probeHelp->kill();
            capabilities->setText("Tool discovery timed out. Check executable paths, then retry.");
            discover->setEnabled(true);
        });
        connect(discover, &QPushButton::clicked, this, [uiHelp, uiExecutable, discover, frontendTransport, deadline] {
            discover->setEnabled(false);
            frontendTransport->setEnabled(false);
            deadline->start();
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("QT_QPA_PLATFORM", "offscreen");
            uiHelp->setProcessEnvironment(env);
            uiHelp->start(uiExecutable->text().trimmed(), {"--config-file", "/dev/null", "--help"});
        });
        connect(uiHelp,
                &QProcess::finished,
                this,
                [uiHelp, probeHelp, probeExecutable, frontendTransport, deadline](int, QProcess::ExitStatus) {
                    if (deadline->isActive()) {
                        const auto help = uiHelp->readAllStandardOutput();
                        auto* model = qobject_cast<QStandardItemModel*>(frontendTransport->model());
                        for (int i = 0; i < frontendTransport->count(); ++i) {
                            model->item(i)->setEnabled(help.contains(("codex-ui-" + frontendTransport->itemData(i).toString()).toUtf8()));
                        }
                        probeHelp->start(probeExecutable->text().trimmed(), {"--config-file", "/dev/null", "--help"});
                    }
                });
        connect(probeHelp,
                &QProcess::finished,
                this,
                [uiHelp, probeHelp, uiExecutable, probeExecutable, frontendTransport, discover, capabilities, deadline](
                    int, QProcess::ExitStatus) {
                    if (deadline->isActive()) {
                        deadline->stop();
                        const auto help = probeHelp->readAllStandardOutput();
                        int supported = 0;
                        auto* model = qobject_cast<QStandardItemModel*>(frontendTransport->model());
                        for (int i = 0; i < frontendTransport->count(); ++i) {
                            auto* item = model->item(i);
                            item->setEnabled(item->isEnabled() &&
                                             help.contains(("codex-bridge-client-" + frontendTransport->itemData(i).toString()).toUtf8()));
                            supported += item->isEnabled();
                        }
                        const bool current = uiHelp->program() == uiExecutable->text().trimmed() &&
                                             probeHelp->program() == probeExecutable->text().trimmed();
                        frontendTransport->setEnabled(current && supported > 0);
                        discover->setEnabled(true);
                        capabilities->setText(current ? QString::number(supported) +
                                                            " transports supported by the local UI and readiness client. Bridge "
                                                            "configuration is verified on its host before startup."
                                                      : "Executable changed. Check installed transports again.");
                    }
                });
        for (auto* process : {uiHelp, probeHelp}) {
            connect(process,
                    &QProcess::errorOccurred,
                    this,
                    [process, frontendTransport, discover, capabilities, deadline](QProcess::ProcessError error) {
                        if (error == QProcess::FailedToStart) {
                            deadline->stop();
                            frontendTransport->setEnabled(false);
                            discover->setEnabled(true);
                            capabilities->setText("Tool discovery failed: " + process->errorString());
                        }
                    });
        }
        for (auto* edit : {uiExecutable, probeExecutable}) {
            connect(edit, &QLineEdit::textChanged, this, [frontendTransport, capabilities] {
                frontendTransport->setEnabled(false);
                capabilities->setText("Executable changed. Check installed transports before saving.");
            });
        }
        QTimer::singleShot(0, discover, &QPushButton::click);
        content->addWidget(
            UiStyle::makeLabel("The existing codex-bridge-client checks transport and provider readiness without maintaining conversation "
                               "state. Native SSH keys, agents and host verification are used; credentials are not stored in profiles.",
                               "meta",
                               tools));
        content->addStretch();
        Q_UNUSED(bridgePage);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this, frontendTransport] {
            for (const auto& read : readers_) {
                read(profile_);
            }
            auto errors = profile_.errors(others_);
            if (!frontendTransport->isEnabled() ||
                !(frontendTransport->model()->flags(frontendTransport->model()->index(frontendTransport->currentIndex(), 0)) &
                  Qt::ItemIsEnabled)) {
                errors << "Check installed tools and select a supported frontend transport.";
            }
            if (!errors.isEmpty()) {
                QMessageBox::warning(this, "Profile needs attention", errors.join('\n'));
            } else {
                accept();
            }
        });
    }
    ProfileEditor::~ProfileEditor() {
        // Helpers must stop before QObject destroys the widgets captured by their callbacks.
        for (auto* process : findChildren<QProcess*>()) {
            process->disconnect(this);
            process->kill();
            process->waitForFinished(1000);
        }
    }
    Profile ProfileEditor::profile() const {
        return profile_;
    }
} // namespace codexui::manager
