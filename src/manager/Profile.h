// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QVector>

namespace codexui::manager {

    enum class Ownership { Local, Ssh, External };

    struct Host {
        Ownership ownership = Ownership::Local;
        QString sshTarget;
        QString identityFile;
        QString workingDirectory;
        QMap<QString, QString> environment;
        [[nodiscard]] QString label() const;
        [[nodiscard]] QString key() const;
    };

    struct Endpoint {
        QString transport = QStringLiteral("unix");
        QString host = QStringLiteral("127.0.0.1");
        QString bindHost = QStringLiteral("127.0.0.1");
        int port = 4500;
        QString socketPath;
        QString webSocketPath = QStringLiteral("/codex");
        QString certificate;
        QString privateKey;
        QString caCertificate;
        QString serverName;
        int channel = 1;
    };

    struct Profile {
        QString id;
        QString name;
        QString description;
        QString accent = QStringLiteral("blue");
        QString codexHome;
        QString uiExecutable = QStringLiteral("codex-ui");
        QString bridgeExecutable = QStringLiteral("codex-bridge");
        QString appServerExecutable = QStringLiteral("codex");
        QString probeExecutable = QStringLiteral("codex-bridge-client");
        Host bridge;
        Host appServer;
        Endpoint frontend;
        Endpoint provider;
        bool shareInstructions = false;

        [[nodiscard]] static Profile create(QString name = {});
        [[nodiscard]] QJsonObject toJson() const;
        [[nodiscard]] static Profile fromJson(const QJsonObject& object);
        [[nodiscard]] QStringList errors(const QVector<Profile>& others = {}) const;
        [[nodiscard]] QString runtimeSocket() const;
        [[nodiscard]] QString settingsDirectory() const;
        [[nodiscard]] QString unit(const QString& component) const;
    };

    [[nodiscard]] QStringList frontendTransports();
    [[nodiscard]] QString transportLabel(const QString& transport);
    [[nodiscard]] QString ownershipLabel(Ownership ownership);
    [[nodiscard]] QString accentColor(const QString& accent);
    [[nodiscard]] QString quote(const QString& argument);

    class ProfileStore {
    public:
        explicit ProfileStore(QString filePath);
        [[nodiscard]] QVector<Profile> load(QString* error) const;
        [[nodiscard]] bool save(const QVector<Profile>& profiles, QString* error) const;
        [[nodiscard]] QString filePath() const {
            return path;
        }

    private:
        QString path;
    };

} // namespace codexui::manager
