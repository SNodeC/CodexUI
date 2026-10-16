// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ProfileRuntime.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

namespace codexui::manager {
    namespace {
        QString redact(QString text) {
            text.replace(QRegularExpression("(Bearer\\s+)[A-Za-z0-9._~+/-]+", QRegularExpression::CaseInsensitiveOption), "\\1[redacted]");
            text.replace(QRegularExpression("(sk-|gh[opusr]_)[A-Za-z0-9_-]{12,}"), "[redacted]");
            return text;
        }

    } // namespace
    ProfileRuntime::ProfileRuntime(Profile profile, QObject* parent)
        : QObject(parent)
        , profile_(std::move(profile))
        , plan_(LaunchPlan::forProfile(profile_)) {
        for (const auto& c : plan_.backends) {
            componentStates_[c.name] = c.host.ownership == Ownership::External ? State::External : State::Unknown;
        }
        componentStates_["CodexUI"] = State::Unknown;
        operation_.setProcessChannelMode(QProcess::MergedChannels);
        operationTimeout_.setSingleShot(true);
        operationTimeout_.setInterval(20000);
        connect(&operationTimeout_, &QTimer::timeout, this, [this] {
            log("Management command timed out; process status is unknown.");
            operation_.kill();
        });
        connect(&operation_, &QProcess::readyReadStandardOutput, this, [this] {
            operationOutput_.append(operation_.readAllStandardOutput());
            if (operationOutput_.size() > 131072) {
                operationOutput_.truncate(131072);
            }
        });
        connect(&operation_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            finishOperation(code);
        });
        connect(&operation_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                operationOutput_ += operation_.errorString().toUtf8();
                finishOperation(127);
            }
        });
        connect(&probe_, &QProcess::readyReadStandardOutput, this, &ProfileRuntime::readProbe);
        connect(&probe_, &QProcess::readyReadStandardError, this, [this] {
            log(QString::fromUtf8(probe_.readAllStandardError()));
        });
        connect(&probe_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                fail("Readiness client could not start: " + probe_.errorString());
            }
        });
        connect(&probe_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            if (!stopping_) {
                readinessTimeout_.stop();
                ready_ = false;
                starting_ = false;
                setStatus("Connection monitor stopped (" + QString::number(code) + ")");
            }
        });
        readinessTimeout_.setSingleShot(true);
        readinessTimeout_.setInterval(35000);
        connect(&readinessTimeout_, &QTimer::timeout, this, [this] {
            if (!ready()) {
                fail("Bridge protocol is not ready. Check Setup & startup and component logs.");
            }
        });
        refreshTimer_.setInterval(5000);
        connect(&refreshTimer_, &QTimer::timeout, this, &ProfileRuntime::refresh);
        refreshTimer_.start();
    }
    ProfileRuntime::~ProfileRuntime() {
        // Only helper clients are direct children. Backend/UI user services survive manager exit.
        refreshTimer_.stop();
        readinessTimeout_.stop();
        operationTimeout_.stop();
        probe_.disconnect(this);
        operation_.disconnect(this);
        probe_.kill();
        probe_.waitForFinished(1000);
        operation_.kill();
        operation_.waitForFinished(1000);
    }
    bool ProfileRuntime::busy() const {
        return starting_ || stopping_ || current_.has_value() || !operations_.isEmpty();
    }
    ProfileRuntime::State ProfileRuntime::serviceState(const Component& c, int code, const QString& output) {
        State result = State::Unknown;
        const QString description = "Description=CodexManager " + c.unit;
        const QStringList lines = output.split('\n');
        if ((code == 0 || code == 1 || code == 4) && lines.contains("LoadState=not-found")) {
            result = State::Inactive;
        } else if ((code == 0 || code == 4) && lines.contains("ActiveState=inactive")) {
            result = State::Inactive;
        } else if (code == 0 && (lines.contains("ActiveState=active") || lines.contains("ActiveState=activating") ||
                                 lines.contains("ActiveState=deactivating"))) {
            result = State::Running;
        } else if (code == 0 && lines.contains("ActiveState=failed")) {
            result = State::Inactive;
        }
        const auto descriptions = lines.filter("Description=");
        if (result != State::Unknown && !lines.contains("LoadState=not-found") && !descriptions.isEmpty() && !lines.contains(description)) {
            result = State::Foreign;
        }
        return result;
    }
    QMap<QString, QString> ProfileRuntime::components() const {
        QMap<QString, QString> result;
        for (auto it = componentStates_.cbegin(); it != componentStates_.cend(); ++it) {
            switch (it.value()) {
                case State::Unknown:
                    result[it.key()] = "Status unknown / supervision unavailable";
                    break;
                case State::Inactive:
                    result[it.key()] = "Stopped";
                    break;
                case State::Running:
                    result[it.key()] = "Running";
                    break;
                case State::Starting:
                    result[it.key()] = "Starting";
                    break;
                case State::Foreign:
                    result[it.key()] = "Service identity conflict";
                    break;
                case State::External:
                    result[it.key()] = "Externally managed";
                    break;
            }
        }
        return result;
    }
    bool ProfileRuntime::hasOwnedProcesses() const {
        return std::any_of(componentStates_.cbegin(), componentStates_.cend(), [](State s) {
            return s != State::Inactive && s != State::External;
        });
    }
    QVector<Component> ProfileRuntime::ownedComponents() const {
        QVector<Component> result;
        for (const auto& c : plan_.backends) {
            if (c.host.ownership != Ownership::External) {
                result.push_back(c);
            }
        }
        result.push_back(plan_.ui);
        return result;
    }
    void ProfileRuntime::setStatus(QString text) {
        status_ = std::move(text);
        emit changed();
    }
    void ProfileRuntime::log(QString text) {
        text = redact(text.trimmed());
        if (!text.isEmpty()) {
            const QString line = QDateTime::currentDateTime().toString("HH:mm:ss  ") + text;
            logs_ += line + '\n';
            if (logs_.size() > 262144) {
                logs_ = logs_.right(196608);
            }
            emit logAdded(line);
        }
    }
    void ProfileRuntime::fail(const QString& message) {
        starting_ = false;
        openRequested_ = false;
        log(message);
        setStatus(message);
    }
    void ProfileRuntime::enqueue(Command c, std::function<void(int, QString)> done) {
        operations_.enqueue({std::move(c), std::move(done)});
        runNext();
    }
    void ProfileRuntime::launch(const Component& c, std::function<void(int, QString)> done) {
        enqueue(c.configurationCheck(), [this, c, done = std::move(done)](int code, const QString& output) {
            const bool supported = c.acceptsConfiguration(code, output);
            if (!supported) {
                done(126, "Installed component rejected the selected configuration: " + output);
            } else if (!stopping_) {
                log(c.name + ": installed configuration verified");
                enqueue(serviceStart(c), done);
            }
        });
    }
    void ProfileRuntime::runNext() {
        if (!current_ && !operations_.isEmpty()) {
            current_ = operations_.dequeue();
            operationOutput_.clear();
            auto environment = QProcessEnvironment::systemEnvironment();
            for (auto i = current_->command.environment.cbegin(); i != current_->command.environment.cend(); ++i) {
                environment.insert(i.key(), i.value());
            }
            operation_.setProcessEnvironment(environment);
            operation_.setWorkingDirectory(current_->command.workingDirectory);
            operationTimeout_.start();
            operation_.start(current_->command.program, current_->command.arguments);
        }
    }
    void ProfileRuntime::finishOperation(int code) {
        if (current_) {
            if (!operationTimeout_.isActive()) {
                code = 124;
            }
            operationTimeout_.stop();
            operationOutput_ += operation_.readAllStandardOutput();
            operationOutput_.truncate(131072);
            auto done = std::move(current_->done);
            current_.reset();
            const QString output = QString::fromUtf8(operationOutput_);
            done(code, output);
            QTimer::singleShot(0, this, &ProfileRuntime::runNext);
        }
    }
    bool ProfileRuntime::prepareHome() {
        bool result = true;
        const auto& host = profile_.provider.transport == "stdio" ? profile_.bridge : profile_.appServer;
        if (host.ownership == Ownership::Local) {
            const bool existed = QDir(profile_.codexHome).exists();
            result = QDir().mkpath(profile_.codexHome);
            if (!result) {
                fail("Cannot create CODEX_HOME: " + profile_.codexHome);
            } else {
                if (!existed) {
                    QFile::setPermissions(profile_.codexHome, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                }
                if (profile_.shareInstructions) {
                    const QString source = QDir::homePath() + "/.codex/AGENTS.md";
                    const QString target = profile_.codexHome + "/AGENTS.md";
                    if (!QFileInfo::exists(target) && QFileInfo::exists(source) && !QFile::link(source, target)) {
                        result = false;
                        fail("Could not link shared instructions; existing instructions were preserved.");
                    }
                }
            }
        }
        return result;
    }
    void ProfileRuntime::start(bool openUi) {
        if (!starting_ && !stopping_) {
            const auto errors = profile_.errors();
            if (!errors.isEmpty()) {
                fail(errors.join('\n'));
            } else if (prepareHome()) {
                starting_ = true;
                openRequested_ = openUi;
                setStatus("Checking managed components…");
                ensureComponent(0);
            }
        }
    }
    void ProfileRuntime::ensureComponent(int index) {
        if (starting_ && !stopping_) {
            if (index >= plan_.backends.size()) {
                beginProbe();
            } else {
                const auto c = plan_.backends[index];
                if (c.host.ownership == Ownership::External) {
                    componentStates_[c.name] = State::External;
                    ensureComponent(index + 1);
                } else {
                    setStatus("Checking " + c.name.toLower() + " on " + c.host.label() + "…");
                    enqueue(serviceCommand(c, "show"), [this, c, index](int code, const QString& output) {
                        if (starting_ && !stopping_) {
                            const auto state = serviceState(c, code, output);
                            componentStates_[c.name] = state;
                            if (state == State::Unknown || state == State::Foreign) {
                                fail(c.name + ": " +
                                     (code == 255 ? "SSH supervision unavailable. " : "Cannot safely determine owned service status. ") +
                                     output);
                            } else if (state == State::Running) {
                                ensureComponent(index + 1);
                            } else {
                                componentStates_[c.name] = State::Starting;
                                setStatus("Starting " + c.name.toLower() + " on " + c.host.label() + "…");
                                launch(c, [this, c, index](int exitCode, const QString& out) {
                                    if (starting_ && !stopping_) {
                                        log(c.name + ": " + out);
                                        componentStates_[c.name] = exitCode == 0 ? State::Running : State::Unknown;
                                        if (exitCode != 0) {
                                            fail("Could not start " + c.name + ". Check systemd user-service access and Setup & startup.");
                                        } else {
                                            ensureComponent(index + 1);
                                        }
                                    }
                                });
                            }
                        }
                    });
                }
            }
        }
    }
    void ProfileRuntime::check() {
        if (!starting_ && !stopping_) {
            openRequested_ = false;
            starting_ = true;
            beginProbe();
        }
    }
    void ProfileRuntime::beginProbe() {
        if (probe_.state() != QProcess::NotRunning) {
            if (ready()) {
                updateReadiness();
            } else {
                setStatus("Waiting for bridge protocol…");
                readinessTimeout_.start();
            }
        } else {
            observer_ = false;
            ready_ = false;
            probeConnectionId_.clear();
            probeOutput_.clear();
            setStatus("Connecting to bridge…");
            probe_.start(plan_.probe.program, plan_.probe.arguments);
            readinessTimeout_.start();
        }
    }
    void ProfileRuntime::readProbe() {
        probeOutput_.append(probe_.readAllStandardOutput());
        if (stopping_) {
            probeOutput_.clear();
        }
        if (probeOutput_.size() > 1048576) {
            probe_.kill();
            probeOutput_.clear();
            fail("Readiness output exceeded its bounded buffer.");
        }
        while (probeOutput_.contains('\n')) {
            const qsizetype newline = probeOutput_.indexOf('\n');
            const auto line = probeOutput_.first(newline);
            probeOutput_.remove(0, newline + 1);
            const auto message = QJsonDocument::fromJson(line).object();
            if (message.isEmpty() && !line.trimmed().isEmpty()) {
                log(QString::fromUtf8(line));
            }
            const auto kind = message.value("kind").toString();
            if (kind == "bridge.connection" && message.value("event").toString() == "opened") {
                probeConnectionId_ = message.value("connectionId").toString();
                observer_ = message.value("role").toString() == "observer";
            } else if (kind == "bridge.controller") {
                observer_ = !probeConnectionId_.isEmpty() && message.value("controllerConnectionId").toString() != probeConnectionId_;
                if (!observer_ && !probeConnectionId_.isEmpty()) {
                    probe_.write("release\n");
                    setStatus("Releasing diagnostic control…");
                }
            } else if (kind == "bridge.provider") {
                ready_ = message.value("state").toString() == "ready";
                log("App-server protocol: " + message.value("state").toString());
                if (!ready_) {
                    setStatus("Bridge connected · app-server " + message.value("state").toString());
                }
            } else if (kind == "client.connection") {
                if (message.value("event").toString() == "disconnected") {
                    ready_ = false;
                    setStatus("Bridge disconnected");
                } else {
                    setStatus("Bridge connected · checking app-server…");
                }
            } else if (kind == "client.error") {
                log(message.value("message").toString());
            }
            updateReadiness();
        }
    }
    void ProfileRuntime::updateReadiness() {
        if (ready_ && observer_) {
            readinessTimeout_.stop();
            starting_ = false;
            setStatus("Ready");
            if (openRequested_) {
                openRequested_ = false;
                ensureUi();
            }
        }
    }
    void ProfileRuntime::openUi() {
        if (ready_ && observer_) {
            ensureUi();
        } else {
            start(true);
        }
    }
    void ProfileRuntime::ensureUi() {
        if (componentStates_.value("CodexUI") != State::Starting) {
            componentStates_["CodexUI"] = State::Starting;
            auto* socket = new QLocalSocket(this);
            connect(socket, &QLocalSocket::connected, this, [this, socket] {
                socket->write("activate\n");
                socket->flush();
                socket->disconnectFromServer();
                socket->deleteLater();
                componentStates_["CodexUI"] = State::Running;
                emit changed();
            });
            connect(socket, &QLocalSocket::errorOccurred, this, [this, socket](QLocalSocket::LocalSocketError) {
                socket->deleteLater();
                const auto c = plan_.ui;
                enqueue(serviceCommand(c, "show"), [this, c](int code, const QString& out) {
                    if (!stopping_) {
                        const auto state = serviceState(c, code, out);
                        componentStates_[c.name] = state;
                        if (state == State::Unknown || state == State::Foreign) {
                            fail("Cannot safely launch CodexUI: " + out);
                        } else if (state == State::Running) {
                            componentStates_["CodexUI"] = State::Running;
                            log("CodexUI is already running; activation endpoint is not yet available.");
                        } else {
                            QDir().mkpath(profile_.settingsDirectory());
                            launch(c, [this](int code, const QString& output) {
                                if (!stopping_) {
                                    if (code != 0) {
                                        componentStates_["CodexUI"] = State::Unknown;
                                        fail("Could not launch CodexUI: " + output);
                                    } else {
                                        componentStates_["CodexUI"] = State::Running;
                                        log("CodexUI opened");
                                        emit changed();
                                    }
                                }
                            });
                        }
                    }
                });
            });
            socket->connectToServer(profile_.runtimeSocket());
        }
    }
    void ProfileRuntime::stop() {
        if (!stopping_) {
            stopping_ = true;
            starting_ = false;
            openRequested_ = false;
            ready_ = false;
            operations_.clear();
            readinessTimeout_.stop();
            if (probe_.state() != QProcess::NotRunning) {
                probe_.write("quit\n");
                probe_.terminate();
            }
            setStatus("Stopping owned components…");
            stopComponent(ownedComponents().size() - 1);
        }
    }
    void ProfileRuntime::stopComponent(int index) {
        if (index < 0) {
            stopping_ = false;
            setStatus(hasOwnedProcesses() ? "Some owned components could not be stopped; check logs"
                                          : "Owned components stopped; external services unchanged");
        } else {
            const auto c = ownedComponents()[index];
            enqueue(serviceCommand(c, "show"), [this, c, index](int code, const QString& out) {
                const auto state = serviceState(c, code, out);
                componentStates_[c.name] = state;
                if (state == State::Running) {
                    enqueue(serviceCommand(c, "stop"), [this, c, index](int result, const QString& output) {
                        componentStates_[c.name] = result == 0 ? State::Inactive : State::Unknown;
                        if (result != 0) {
                            log(c.name + ": " + output);
                        }
                        stopComponent(index - 1);
                    });
                } else {
                    if (state != State::Inactive) {
                        log("Refusing to stop unverified service " + c.unit + ": " + out);
                    }
                    stopComponent(index - 1);
                }
            });
        }
    }
    void ProfileRuntime::refresh() {
        if (!busy()) {
            for (const auto& c : ownedComponents()) {
                enqueue(serviceCommand(c, "show"), [this, c](int code, const QString& output) {
                    componentStates_[c.name] = serviceState(c, code, output);
                    emit changed();
                });
            }
        }
    }
    void ProfileRuntime::readLogs() {
        for (const auto& c : ownedComponents()) {
            enqueue(serviceLogs(c), [this, c](int, const QString& output) {
                log(c.name + " journal:\n" + output);
            });
        }
    }
} // namespace codexui::manager
