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
                forgetAccount();
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
        return deletion_.has_value() || login_.has_value() || starting_ || stopping_ || current_.has_value() || !operations_.isEmpty();
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
    void ProfileRuntime::enqueue(Command c, std::function<void(int, QString)> done, int timeoutMs) {
        operations_.enqueue({std::move(c), std::move(done), timeoutMs});
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
            operationTimeout_.start(current_->timeoutMs);
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
            const QFileInfo homeInfo(profile_.codexHome);
            result = existed || (QDir().mkpath(homeInfo.absolutePath()) && QDir().mkdir(profile_.codexHome));
            if (!result) {
                fail("Cannot create CODEX_HOME: " + profile_.codexHome);
            } else {
                if (!existed) {
                    QFile::setPermissions(profile_.codexHome, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                    QFile owner(profile_.codexHome + "/.codexmanager-owner");
                    const QByteArray identity = profile_.id.toUtf8() + '\n';
                    result = owner.open(QIODevice::WriteOnly | QIODevice::NewOnly) && owner.write(identity) == identity.size();
                    if (!result) {
                        fail("Cannot record ownership of the new profile home.");
                    }
                }
                QFile owner(profile_.codexHome + "/.codexmanager-owner");
                if (QFileInfo(owner).isFile() && owner.open(QIODevice::ReadOnly) && owner.read(128).startsWith("deleting:")) {
                    result = false;
                    fail("Profile deletion is incomplete. Retry deletion before starting this home.");
                }
                if (result && profile_.shareInstructions) {
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
        if (!starting_ && !stopping_ && !login_ && !deletion_) {
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
        if (!starting_ && !stopping_ && !login_ && !deletion_) {
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
                forgetAccount();
                probeConnectionId_ = message.value("connectionId").toString();
                observer_ = message.value("role").toString() == "observer";
            } else if (kind == "bridge.controller") {
                controllerId_ = message.value("controllerConnectionId").toString();
                observer_ = !probeConnectionId_.isEmpty() && controllerId_ != probeConnectionId_;
                if (!observer_ && !probeConnectionId_.isEmpty()) {
                    if (login_ && login_->phase == LoginPhase::Claiming) {
                        const bool logout = login_->parameters.isEmpty();
                        login_->phase = logout ? LoginPhase::SigningOut : LoginPhase::Starting;
                        accountRequest(logout ? "account/logout" : "account/login/start", login_->parameters);
                        login_->parameters = {};
                    } else if (!login_) {
                        probe_.write("release\n");
                        setStatus("Releasing diagnostic control…");
                    }
                } else if (login_ && !controllerId_.isEmpty()) {
                    finishAuthentication(
                        "Another frontend owns control. Use Release control in this profile's CodexUI, or close it, then retry.");
                }
            } else if (kind == "bridge.provider") {
                const bool wasReady = ready_;
                ready_ = message.value("state").toString() == "ready";
                if (!ready_) {
                    forgetAccount();
                } else if (!wasReady) {
                    refreshAccount();
                }
                log("App-server protocol: " + message.value("state").toString());
                if (!ready_) {
                    setStatus("Bridge connected · app-server " + message.value("state").toString());
                }
            } else if (kind == "client.connection") {
                if (message.value("event").toString() == "disconnected") {
                    ready_ = false;
                    forgetAccount();
                    setStatus("Bridge disconnected");
                } else {
                    setStatus("Bridge connected · checking app-server…");
                }
            } else if (kind == "bridge.diagnostic" && login_ && message.value("connectionId").toString() == probeConnectionId_ &&
                       message.value("code").toString().startsWith("controller-")) {
                finishAuthentication("Bridge control could not be acquired. Use Release control in CodexUI, then retry.");
            } else if (kind == "appserver") {
                accountMessage(message.value("payload").toObject());
            } else if (kind == "client.error") {
                if (login_) {
                    finishAuthentication("Authentication request could not be delivered. Check the connection and retry.");
                } else {
                    log(message.value("message").toString());
                }
            }
            updateReadiness();
        }
    }
    void ProfileRuntime::updateReadiness() {
        if (ready_ && observer_) {
            readinessTimeout_.stop();
            starting_ = false;
            setStatus("Backend connected");
            if (openRequested_ && !login_ && accountState_ != AccountState::Unknown) {
                openRequested_ = false;
                if (accountState_ == AccountState::SignedOut) {
                    authenticationMessage_ = "Sign in before sending prompts. You can still open CodexUI to browse.";
                    emit changed();
                } else {
                    ensureUi();
                }
            }
        }
    }
    void ProfileRuntime::openUi() {
        if (ready_ && observer_ && !login_ && !deletion_) {
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
            forgetAccount();
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
            stopping_ = deletion_.has_value();
            setStatus(hasOwnedProcesses() ? "Some owned components could not be stopped; check logs"
                                          : "Owned components stopped; external services unchanged");
            if (deletion_) {
                if (hasOwnedProcesses()) {
                    finishDeletion("Owned services could not be verified stopped. No data deletion was started.");
                } else {
                    deleteTarget(0);
                }
            }
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
        bool timedOut = false;
        for (auto it = accountRequests_.begin(); it != accountRequests_.end();) {
            if (it->deadline.hasExpired()) {
                if (it->method == "account/read") {
                    accountState_ = AccountState::Unknown;
                    if (!login_) {
                        authenticationMessage_ = "Account status unavailable. Refresh to retry.";
                    }
                    emit changed();
                } else {
                    timedOut = true;
                }
                it = accountRequests_.erase(it);
            } else {
                ++it;
            }
        }
        if (timedOut || (login_ && login_->deadline.hasExpired())) {
            if (login_ && login_->phase == LoginPhase::Waiting) {
                cancelLogin();
            } else {
                finishAuthentication("Account request timed out; its outcome is unknown. Refresh account status before retrying.");
                accountState_ = AccountState::Unknown;
                emit changed();
            }
        }
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
    QString ProfileRuntime::accountStatus() const {
        QString result;
        switch (accountState_) {
            case AccountState::Unknown:
                result = "Account status unknown";
                break;
            case AccountState::SignedOut:
                result = "Sign-in required";
                break;
            case AccountState::SignedIn:
                result = "Signed in · " + accountIdentity_;
                break;
            case AccountState::Rejected:
                result = "Authentication rejected · sign in again";
                break;
            case AccountState::NotRequired:
                result = "Provider does not require OpenAI sign-in";
                break;
        }
        return result;
    }
    bool ProfileRuntime::authenticationAvailable() const {
        return ready_ && !stopping_ && !login_ && controllerId_.isEmpty();
    }
    void ProfileRuntime::accountRequest(const QString& method, const QJsonObject& parameters) {
        if (ready_ && !stopping_) {
            const QString id = "manager-account-" + QString::number(++accountSequence_);
            accountRequests_.insert(id, {method, QDeadlineTimer(30000)});
            const QJsonObject request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", parameters}};
            probe_.write("raw " + QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        }
    }
    void ProfileRuntime::refreshAccount() {
        const bool pending = std::any_of(accountRequests_.cbegin(), accountRequests_.cend(), [](const AccountRequest& r) {
            return r.method == "account/read";
        });
        if (!pending) {
            accountRequest("account/read", {{"refreshToken", false}});
        }
    }
    void ProfileRuntime::beginAuthentication(QJsonObject parameters) {
        if (authenticationAvailable()) {
            login_.emplace();
            login_->parameters = std::move(parameters);
            authenticationMessage_ = login_->parameters.isEmpty() ? "Signing out…" : "Starting sign-in…";
            probe_.write("acquire\n");
        } else {
            authenticationMessage_ = ready_ ? "Use Release control in this profile's CodexUI, or close it before changing authentication."
                                            : "Start or connect the profile before signing in.";
        }
        emit changed();
    }
    void ProfileRuntime::signIn(const QString& type, const QString& apiKey) {
        if (type == "chatgpt" || type == "chatgptDeviceCode" || (type == "apiKey" && !apiKey.trimmed().isEmpty())) {
            QJsonObject parameters{{"type", type}};
            if (type == "apiKey") {
                parameters.insert("apiKey", apiKey.trimmed());
            }
            beginAuthentication(std::move(parameters));
        }
    }
    void ProfileRuntime::signOut() {
        beginAuthentication({});
    }
    void ProfileRuntime::cancelLogin() {
        if (login_) {
            const bool cancelling = std::any_of(accountRequests_.cbegin(), accountRequests_.cend(), [](const AccountRequest& request) {
                return request.method == "account/login/cancel";
            });
            if (!login_->id.isEmpty() && !cancelling) {
                login_->phase = LoginPhase::Cancelling;
                login_->deadline = QDeadlineTimer(30000);
                accountRequest("account/login/cancel", {{"loginId", login_->id}});
                authenticationMessage_ = "Cancelling sign-in…";
            } else if (login_->phase == LoginPhase::Claiming) {
                finishAuthentication("Sign-in cancelled.");
            } else if (login_->phase == LoginPhase::Starting) {
                login_->phase = LoginPhase::Cancelling;
                authenticationMessage_ = "Waiting for the login ID to cancel sign-in…";
            }
            emit changed();
        }
    }
    void ProfileRuntime::finishAuthentication(const QString& message) {
        login_.reset();
        for (auto it = accountRequests_.begin(); it != accountRequests_.end();) {
            if (it->method != "account/read") {
                it = accountRequests_.erase(it);
            } else {
                ++it;
            }
        }
        authenticationMessage_ = message;
        if (controllerId_ == probeConnectionId_ && !probeConnectionId_.isEmpty()) {
            probe_.write("release\n");
        }
        emit changed();
    }
    void ProfileRuntime::forgetAccount() {
        if (controllerId_ == probeConnectionId_ && !probeConnectionId_.isEmpty()) {
            probe_.write("release\n");
        }
        if (login_) {
            authenticationMessage_ = "Connection lost during authentication. Its outcome is unknown; reconnect and refresh account status.";
        }
        login_.reset();
        accountRequests_.clear();
        accountState_ = AccountState::Unknown;
        accountIdentity_.clear();
        controllerId_.clear();
    }
    void ProfileRuntime::accountMessage(const QJsonObject& message) {
        const QString notification = message.value("method").toString();
        const auto parameters = message.value("params").toObject();
        const QString id = message.value("id").toString();
        const auto found = accountRequests_.find(id);
        if (found != accountRequests_.end()) {
            const QString method = found->method;
            accountRequests_.erase(found);
            const auto result = message.value("result").toObject();
            if (message.contains("error")) {
                const int code = message.value("error").toObject().value("code").toInt();
                const QString failure = "Account operation rejected (" + QString::number(code) +
                                        "). Try another sign-in method or use Setup & startup on the app-server host.";
                if (method == "account/read") {
                    accountState_ = AccountState::Unknown;
                    authenticationMessage_ = failure;
                } else {
                    finishAuthentication(failure);
                    refreshAccount();
                }
            } else if (method == "account/read") {
                const auto account = result.value("account").toObject();
                if (!result.value("requiresOpenaiAuth").isBool()) {
                    accountState_ = AccountState::Unknown;
                    authenticationMessage_ = "App-server returned an unsupported account response.";
                } else if (!account.isEmpty()) {
                    accountState_ = AccountState::SignedIn;
                    accountIdentity_ = account.value("email").toString(account.value("type").toString());
                    const auto plan = account.value("planType").toString();
                    if (!plan.isEmpty()) {
                        accountIdentity_ += " · " + plan;
                    }
                } else {
                    accountState_ = result.value("requiresOpenaiAuth").toBool() ? AccountState::SignedOut : AccountState::NotRequired;
                    accountIdentity_.clear();
                }
            } else if (method == "account/login/start" && login_) {
                if (result.value("type").toString() == "apiKey") {
                    finishAuthentication("API key stored by the app-server. This does not verify quota or inference access.");
                    refreshAccount();
                } else {
                    login_->id = result.value("loginId").toString();
                    login_->url = result.value("authUrl").toString(result.value("verificationUrl").toString());
                    login_->code = result.value("userCode").toString();
                    if (login_->id.isEmpty() || login_->url.isEmpty()) {
                        finishAuthentication("The app-server did not return a usable sign-in flow. Try another method.");
                    } else if (login_->phase == LoginPhase::Cancelling) {
                        cancelLogin();
                    } else {
                        login_->phase = LoginPhase::Waiting;
                        login_->deadline = QDeadlineTimer(10 * 60 * 1000);
                        authenticationMessage_ = "Waiting for you to complete sign-in in the browser…";
                        if (!login_->completion.isEmpty()) {
                            const QJsonObject completion = login_->completion;
                            accountMessage(completion);
                        }
                    }
                }
            } else if (method == "account/login/cancel" || method == "account/logout") {
                finishAuthentication(method == "account/logout" ? "Signed out." : "Sign-in cancelled; refreshing account status.");
                refreshAccount();
            }
        } else if (notification == "account/updated") {
            for (auto it = accountRequests_.begin(); it != accountRequests_.end();) {
                if (it->method == "account/read") {
                    it = accountRequests_.erase(it);
                } else {
                    ++it;
                }
            }
            refreshAccount();
        } else if (notification == "account/login/completed" && login_ && login_->id.isEmpty()) {
            login_->completion = message;
        } else if (notification == "error") {
            const auto info = parameters.value("error").toObject().value("codexErrorInfo");
            bool rejected = info.toString() == "unauthorized";
            const auto detail = info.toObject();
            for (const auto& value : detail) {
                rejected = rejected || value.toObject().value("httpStatusCode").toInt() == 401;
            }
            if (rejected) {
                accountState_ = AccountState::Rejected;
                authenticationMessage_ = "OpenAI rejected authentication. Sign in again for this profile.";
                accountRequests_.clear();
            }
        } else if (notification == "account/login/completed" && login_ && !login_->id.isEmpty() &&
                   parameters.value("loginId").toString() == login_->id) {
            finishAuthentication(parameters.value("success").toBool()
                                     ? "Sign-in completed."
                                     : "Sign-in failed, expired, or was cancelled. Retry or choose another method.");
            refreshAccount();
        }
        if (!id.isEmpty() || notification.startsWith("account/") || notification == "error") {
            emit changed();
        }
    }
    void ProfileRuntime::inspectDeletion(const QVector<Profile>& profiles, std::function<void(DeletionPlan)> done) {
        inspectDeletionTarget(DeletionPlan::forProfile(profile_, profiles), 0, std::move(done));
    }
    void ProfileRuntime::inspectDeletionTarget(DeletionPlan plan, int index, std::function<void(DeletionPlan)> done) {
        if (!plan.error.isEmpty() || index == plan.targets.size()) {
            done(std::move(plan));
        } else {
            const auto command = plan.targets[index].command("inspect");
            enqueue(command, [this, plan = std::move(plan), index, done = std::move(done)](int code, const QString& output) mutable {
                const auto response = QJsonDocument::fromJson(output.toUtf8()).object();
                if (code != 0 || !response.contains("token")) {
                    plan.error = response.value("error").toString(output.isEmpty() ? "Cannot inspect the deletion target." : output);
                } else {
                    plan.targets[index].specification.insert("expected", response.value("token"));
                }
                inspectDeletionTarget(std::move(plan), index + 1, std::move(done));
            });
        }
    }
    void ProfileRuntime::deleteData(DeletionPlan plan) {
        if (!deletion_ && !starting_ && !stopping_ && !login_) {
            const auto current = DeletionPlan::forProfile(profile_, {});
            bool matches = plan.profileId == profile_.id && plan.targets.size() == current.targets.size() && plan.error.isEmpty() &&
                           current.error.isEmpty();
            for (int i = 0; matches && i < plan.targets.size(); ++i) {
                const auto& expected = plan.targets[i].specification;
                const auto& actual = current.targets[i].specification;
                matches = expected.value("path") == actual.value("path") && expected.value("kind") == actual.value("kind") &&
                          expected.value("profile") == actual.value("profile") && expected.contains("expected") &&
                          plan.targets[i].host.key() == current.targets[i].host.key();
            }
            if (matches) {
                deletion_ = std::move(plan);
                stop();
            } else {
                emit deletionFinished(false, "Deletion preview is no longer valid. Inspect and confirm the profile again.");
            }
        } else {
            emit deletionFinished(false, "Finish the current profile operation before deleting it.");
        }
    }
    void ProfileRuntime::deleteTarget(int index) {
        if (index == deletion_->targets.size()) {
            finishDeletion({});
        } else {
            const auto target = deletion_->targets[index];
            setStatus("Deleting " + target.specification.value("path").toString() + " on " + target.host.label() + "…");
            enqueue(
                target.command("delete"),
                [this, index](int code, const QString& output) {
                    const auto response = QJsonDocument::fromJson(output.toUtf8()).object();
                    if (code != 0 || !response.contains("deleted")) {
                        finishDeletion(
                            "Cleanup did not complete. The registration is retained; earlier targets or files may already be deleted. " +
                            output);
                    } else {
                        log("Deleted " + response.value("deleted").toString());
                        deleteTarget(index + 1);
                    }
                },
                300000);
        }
    }
    void ProfileRuntime::finishDeletion(const QString& error) {
        deletion_.reset();
        stopping_ = false;
        setStatus(error.isEmpty() ? "Profile data deleted" : error);
        emit deletionFinished(error.isEmpty(), error);
    }
} // namespace codexui::manager
