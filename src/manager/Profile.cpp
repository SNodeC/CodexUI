// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/Profile.h"

#include "codex/ui/UiStyle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace codexui::manager {
    namespace {
        QJsonObject hostJson(const Host& host) {
            QJsonObject env;
            for (auto it = host.environment.cbegin(); it != host.environment.cend(); ++it) {
                env.insert(it.key(), it.value());
            }
            return {{"ownership", int(host.ownership)},
                    {"sshTarget", host.sshTarget},
                    {"identityFile", host.identityFile},
                    {"workingDirectory", host.workingDirectory},
                    {"environment", env}};
        }
        Host readHost(const QJsonObject& json) {
            Host host;
            host.ownership = Ownership(json.value("ownership").toInt());
            host.sshTarget = json.value("sshTarget").toString();
            host.identityFile = json.value("identityFile").toString();
            host.workingDirectory = json.value("workingDirectory").toString();
            const auto env = json.value("environment").toObject();
            for (auto it = env.begin(); it != env.end(); ++it) {
                host.environment.insert(it.key(), it.value().toString());
            }
            return host;
        }
        QJsonObject endpointJson(const Endpoint& e) {
            return {{"transport", e.transport},
                    {"host", e.host},
                    {"bindHost", e.bindHost},
                    {"port", e.port},
                    {"socketPath", e.socketPath},
                    {"webSocketPath", e.webSocketPath},
                    {"certificate", e.certificate},
                    {"privateKey", e.privateKey},
                    {"caCertificate", e.caCertificate},
                    {"serverName", e.serverName},
                    {"channel", e.channel}};
        }
        Endpoint readEndpoint(const QJsonObject& j) {
            Endpoint e;
            e.transport = j.value("transport").toString("unix");
            e.host = j.value("host").toString("127.0.0.1");
            e.bindHost = j.value("bindHost").toString("127.0.0.1");
            e.port = j.value("port").toInt(4500);
            e.socketPath = j.value("socketPath").toString();
            e.webSocketPath = j.value("webSocketPath").toString("/codex");
            e.certificate = j.value("certificate").toString();
            e.privateKey = j.value("privateKey").toString();
            e.caCertificate = j.value("caCertificate").toString();
            e.serverName = j.value("serverName").toString();
            e.channel = j.value("channel").toInt(1);
            return e;
        }
        QString effectivePath(const Host& host, const QString& path) {
            QString result = QDir::cleanPath(path);
            if (host.key() == "local") {
                const QFileInfo file(path);
                const QString canonical = file.canonicalFilePath();
                if (!canonical.isEmpty()) {
                    result = canonical;
                } else if (!file.canonicalPath().isEmpty()) {
                    result = file.canonicalPath() + '/' + file.fileName();
                }
            }
            return result;
        }
        bool listenersConflict(const Host& a, const Endpoint& x, const Host& b, const Endpoint& y) {
            bool result = false;
            if (a.key() == b.key() && x.transport != "stdio" && y.transport != "stdio") {
                if (x.transport == "unix" || y.transport == "unix") {
                    result = x.transport == y.transport && effectivePath(a, x.socketPath) == effectivePath(b, y.socketPath);
                } else if (x.transport.startsWith("rfcomm") || y.transport.startsWith("rfcomm")) {
                    result = x.transport.startsWith("rfcomm") && y.transport.startsWith("rfcomm") && x.channel == y.channel &&
                             (x.bindHost == y.bindHost || x.bindHost == "00:00:00:00:00:00" || y.bindHost == "00:00:00:00:00:00");
                } else if (x.port == y.port) {
                    const QHostAddress left(x.bindHost), right(y.bindHost);
                    const bool sameFamily = x.transport.contains("ipv6") == y.transport.contains("ipv6");
                    const bool wildcard = left == QHostAddress::AnyIPv4 || left == QHostAddress::AnyIPv6 ||
                                          right == QHostAddress::AnyIPv4 || right == QHostAddress::AnyIPv6;
                    result = (sameFamily && (wildcard || x.bindHost.compare(y.bindHost, Qt::CaseInsensitive) == 0 ||
                                             (!left.isNull() && left == right))) ||
                             (!sameFamily && (left == QHostAddress::AnyIPv6 || right == QHostAddress::AnyIPv6));
                }
            }
            return result;
        }
    } // namespace

    QString ownershipLabel(Ownership mode) {
        QString result = QStringLiteral("Invalid ownership");
        switch (mode) {
            case Ownership::Local:
                result = QStringLiteral("Managed locally");
                break;
            case Ownership::Ssh:
                result = QStringLiteral("Managed over SSH");
                break;
            case Ownership::External:
                result = QStringLiteral("Started externally");
                break;
        }
        return result;
    }
    QString Host::label() const {
        return sshTarget.isEmpty() ? QStringLiteral("This machine") : sshTarget;
    }
    QString Host::key() const {
        return sshTarget.trimmed().isEmpty() ? QStringLiteral("local") : sshTarget.trimmed().toLower();
    }
    QString quote(const QString& argument) {
        QString escaped = argument;
        escaped.replace('\'', QStringLiteral("'\\''"));
        return '\'' + escaped + '\'';
    }
    QStringList frontendTransports() {
        return {"unix",
                "ipv4",
                "ipv6",
                "tls-ipv4",
                "tls-ipv6",
                "websocket-ipv4",
                "websocket-ipv6",
                "wss-ipv4",
                "wss-ipv6",
                "rfcomm",
                "rfcomm-tls"};
    }
    QString transportLabel(const QString& t) {
        static const QMap<QString, QString> labels{{"stdio", "Stdio · bridge-owned child"},
                                                   {"unix", "Unix-domain socket"},
                                                   {"ipv4", "IPv4 · legacy stream"},
                                                   {"ipv6", "IPv6 · legacy stream"},
                                                   {"tls-ipv4", "IPv4 · TLS stream"},
                                                   {"tls-ipv6", "IPv6 · TLS stream"},
                                                   {"websocket-ipv4", "IPv4 · WebSocket"},
                                                   {"websocket-ipv6", "IPv6 · WebSocket"},
                                                   {"wss-ipv4", "IPv4 · secure WebSocket"},
                                                   {"wss-ipv6", "IPv6 · secure WebSocket"},
                                                   {"rfcomm", "Bluetooth · RFCOMM"},
                                                   {"rfcomm-tls", "Bluetooth · RFCOMM TLS"}};
        return labels.value(t, t);
    }
    QString accentColor(const QString& accent) {
        using namespace UiStyle;
        static const QMap<QString, QString> colors{{"blue", Palette::blue},
                                                   {"green", Palette::green},
                                                   {"purple", Palette::purple},
                                                   {"teal", Palette::teal},
                                                   {"amber", Palette::amber},
                                                   {"orange", Palette::orange}};
        return colors.value(accent, Palette::blue);
    }
    Profile Profile::create(QString name) {
        Profile p;
        const QString siblingUi = QCoreApplication::applicationDirPath() + "/codex-ui";
        if (QFileInfo(siblingUi).isExecutable()) {
            p.uiExecutable = siblingUi;
        }
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.name = name.isEmpty() ? QStringLiteral("New profile") : std::move(name);
        p.codexHome = QDir::homePath() + "/.codex-" + p.id.left(8);
        p.frontend.socketPath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/codexmanager-" + p.id + ".sock";
        p.provider.transport = "stdio";
        p.provider.port = 4501;
        p.provider.webSocketPath = "/";
        p.provider.socketPath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/codexmanager-" + p.id + "-app.sock";
        return p;
    }
    QString Profile::runtimeSocket() const {
        return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/codexmanager-" + id + "-ui.sock";
    }
    QString Profile::settingsDirectory() const {
        return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/profiles/" + id;
    }
    QString Profile::unit(const QString& component) const {
        return "codexmanager-" + id + '-' + component + ".service";
    }
    QJsonObject Profile::toJson() const {
        return {{"id", id},
                {"name", name},
                {"description", description},
                {"accent", accent},
                {"codexHome", codexHome},
                {"uiExecutable", uiExecutable},
                {"bridgeExecutable", bridgeExecutable},
                {"appServerExecutable", appServerExecutable},
                {"probeExecutable", probeExecutable},
                {"bridge", hostJson(bridge)},
                {"appServer", hostJson(appServer)},
                {"frontend", endpointJson(frontend)},
                {"provider", endpointJson(provider)},
                {"shareInstructions", shareInstructions}};
    }
    Profile Profile::fromJson(const QJsonObject& j) {
        Profile p;
        p.id = j.value("id").toString();
        p.name = j.value("name").toString();
        p.description = j.value("description").toString();
        p.accent = j.value("accent").toString("blue");
        p.codexHome = j.value("codexHome").toString();
        p.uiExecutable = j.value("uiExecutable").toString("codex-ui");
        p.bridgeExecutable = j.value("bridgeExecutable").toString("codex-bridge");
        p.appServerExecutable = j.value("appServerExecutable").toString("codex");
        p.probeExecutable = j.value("probeExecutable").toString("codex-bridge-client");
        p.bridge = readHost(j.value("bridge").toObject());
        p.appServer = readHost(j.value("appServer").toObject());
        p.frontend = readEndpoint(j.value("frontend").toObject());
        p.provider = readEndpoint(j.value("provider").toObject());
        p.shareInstructions = j.value("shareInstructions").toBool();
        return p;
    }
    QStringList Profile::errors(const QVector<Profile>& others) const {
        QStringList errors;
        if (!QRegularExpression("^[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}$").match(id).hasMatch()) {
            errors << "Profile ID is invalid.";
        }
        if (name.trimmed().isEmpty()) {
            errors << "Enter a profile name.";
        }
        if (!QDir::isAbsolutePath(codexHome)) {
            errors << "CODEX_HOME must be an absolute path on the app-server host.";
        }
        if (!frontendTransports().contains(frontend.transport)) {
            errors << "Unsupported frontend transport.";
        }
        if (!QStringList{"stdio", "unix", "websocket-ipv4", "websocket-ipv6"}.contains(provider.transport)) {
            errors << "Unsupported app-server transport.";
        }
        for (const auto* host : {&bridge, &appServer}) {
            if (host == &bridge || provider.transport != "stdio") {
                if (host->ownership != Ownership::Local && host->ownership != Ownership::Ssh && host->ownership != Ownership::External) {
                    errors << "Invalid ownership mode.";
                }
                if (host->ownership == Ownership::Ssh && (host->sshTarget.isEmpty() || host->sshTarget.startsWith('-') ||
                                                          host->sshTarget.contains(QRegularExpression("[\\s\\x00-\\x1f]")))) {
                    errors << "SSH management requires a valid SSH host alias or user@host.";
                }
                if (host->ownership == Ownership::Local && !host->sshTarget.isEmpty()) {
                    errors << "A locally managed component cannot have a remote host.";
                }
                if (!host->workingDirectory.isEmpty() && !QDir::isAbsolutePath(host->workingDirectory)) {
                    errors << "Working directories must be absolute host paths.";
                }
                for (auto i = host->environment.cbegin(); i != host->environment.cend(); ++i) {
                    if (!QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(i.key()).hasMatch()) {
                        errors << "Invalid environment variable name.";
                    }
                    if (i.key() == "CODEX_HOME" || i.key().startsWith("CODEXUI_")) {
                        errors << "Profile identity and CODEX_HOME are configured by the profile, not its environment overrides.";
                    }
                    if (i.key().contains(QRegularExpression("TOKEN|PASSWORD|SECRET|API_KEY", QRegularExpression::CaseInsensitiveOption))) {
                        errors << "Keep credentials in host authentication/environment stores, not profile metadata.";
                    }
                }
            }
        }
        if (frontend.transport == "unix" && !bridge.sshTarget.isEmpty()) {
            errors << "CodexUI cannot access a remote Unix socket directly; select a network frontend or provide a local tunnel endpoint.";
        }
        if (provider.transport.startsWith("websocket") && appServer.ownership == Ownership::Local) {
            errors << "Network app-servers must be started externally or explicitly managed over SSH.";
        }
        if (provider.transport == "unix" && appServer.key() != bridge.key()) {
            errors << "Unix app-server and bridge must be on the same host.";
        }
        if (provider.transport != "stdio" && listenersConflict(bridge, frontend, appServer, provider)) {
            errors << "Bridge and app-server listeners must be distinct.";
        }
        if (provider.transport.contains("websocket") && provider.webSocketPath != "/") {
            errors << "The current app-server supports the WebSocket path / only.";
        }
        for (const auto* e : {&frontend, &provider}) {
            if (e->transport == "unix" && (!QDir::isAbsolutePath(e->socketPath) || e->socketPath.toUtf8().size() > 100)) {
                errors << "Unix socket paths must be absolute and at most 100 bytes.";
            }
            if (e->transport.contains("ipv") && (e->port < 1 || e->port > 65535 || e->host.isEmpty())) {
                errors << "Network endpoints require a host and port from 1 to 65535.";
            }
            if (e->webSocketPath.isEmpty() || !e->webSocketPath.startsWith('/')) {
                errors << "WebSocket paths must start with /.";
            }
            if (e->transport.startsWith("rfcomm") && (e->channel < 1 || e->channel > 30)) {
                errors << "RFCOMM channels must be from 1 to 30.";
            }
        }
        if (frontend.transport.contains("tls") || frontend.transport.startsWith("wss")) {
            if (bridge.ownership != Ownership::External && (frontend.certificate.isEmpty() || frontend.privateKey.isEmpty())) {
                errors << "A managed TLS bridge requires a certificate and private-key path on its host.";
            }
            if (frontend.caCertificate.isEmpty()) {
                errors << "Configure the local CA certificate used to verify the bridge.";
            }
        }
        if (uiExecutable.isEmpty() || bridgeExecutable.isEmpty() || appServerExecutable.isEmpty() || probeExecutable.isEmpty()) {
            errors << "All executable fields must be set.";
        }
        for (const Profile& p : others) {
            if (p.id != id) {
                const auto& serverHost = provider.transport == "stdio" ? bridge : appServer;
                const auto& otherHost = p.provider.transport == "stdio" ? p.bridge : p.appServer;
                if (serverHost.key() == otherHost.key() && effectivePath(serverHost, codexHome) == effectivePath(otherHost, p.codexHome)) {
                    errors << "Another profile already uses this host and CODEX_HOME.";
                }
                if (provider.transport != "stdio" && listenersConflict(appServer, provider, p.bridge, p.frontend)) {
                    errors << "App-server listener conflicts with another bridge listener.";
                }
                if (p.provider.transport != "stdio" && listenersConflict(bridge, frontend, p.appServer, p.provider)) {
                    errors << "Bridge listener conflicts with another app-server listener.";
                }
                if (listenersConflict(bridge, frontend, p.bridge, p.frontend)) {
                    errors << "Another profile already uses this bridge listener.";
                }
                if (provider.transport != "stdio" && p.provider.transport != "stdio" &&
                    listenersConflict(appServer, provider, p.appServer, p.provider)) {
                    errors << "Another profile already uses this app-server listener.";
                }
            }
        }
        errors.removeDuplicates();
        return errors;
    }

    ProfileStore::ProfileStore(QString filePath)
        : path(std::move(filePath)) {
    }
    QVector<Profile> ProfileStore::load(QString* error) const {
        error->clear();
        QFile f(path);
        QVector<Profile> result;
        if (f.exists()) {
            if (!f.open(QIODevice::ReadOnly)) {
                *error = f.errorString();
            } else {
                QJsonParseError parse;
                const auto document = QJsonDocument::fromJson(f.readAll(), &parse);
                if (parse.error != QJsonParseError::NoError || !document.isObject() || document.object().value("version").toInt() != 1 ||
                    !document.object().value("profiles").isArray()) {
                    *error = "Invalid or unsupported profile-store format; the file was not modified.";
                } else {
                    const auto array = document.object().value("profiles").toArray();
                    for (auto it = array.cbegin(); error->isEmpty() && it != array.cend(); ++it) {
                        Profile profile = Profile::fromJson(it->toObject());
                        if (!profile.errors(result).isEmpty() || std::any_of(result.cbegin(), result.cend(), [&](const Profile& p) {
                                return p.id == profile.id;
                            })) {
                            *error = "Invalid or duplicate stored profile: " + profile.name;
                        } else {
                            result.push_back(std::move(profile));
                        }
                    }
                }
            }
        }
        if (!error->isEmpty()) {
            result.clear();
        }
        return result;
    }
    bool ProfileStore::save(const QVector<Profile>& profiles, QString* error) const {
        error->clear();
        QVector<Profile> checked;
        QJsonArray array;
        for (auto it = profiles.cbegin(); error->isEmpty() && it != profiles.cend(); ++it) {
            const Profile& profile = *it;
            if (!profile.errors(checked).isEmpty() || std::any_of(checked.cbegin(), checked.cend(), [&](const Profile& p) {
                    return p.id == profile.id;
                })) {
                *error = "Invalid or duplicate profile: " + profile.name;
            } else {
                checked.push_back(profile);
                array.append(profile.toJson());
            }
        }
        if (error->isEmpty()) {
            if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
                *error = "Cannot create profile-store directory.";
            } else {
                QSaveFile f(path);
                if (!f.open(QIODevice::WriteOnly)) {
                    *error = f.errorString();
                } else {
                    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
                    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"profiles", array}}).toJson();
                    if (f.write(bytes) != bytes.size() || !f.commit()) {
                        *error = f.errorString();
                    }
                }
            }
        }
        return error->isEmpty();
    }
} // namespace codexui::manager
