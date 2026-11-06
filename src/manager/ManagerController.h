// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "manager/ProfileRuntime.h"

namespace codexui::manager {
    // Qt-thread authority shared by the window and the HTTP adapter.
    class ManagerController final : public QObject {
        Q_OBJECT
    public:
        explicit ManagerController(QString path, QObject* parent = nullptr);
        [[nodiscard]] const QVector<Profile>& profiles() const;
        [[nodiscard]] const QMap<QString, ProfileRuntime*>& runtimes() const;
        [[nodiscard]] ProfileRuntime* runtime(const QString& id) const;
        [[nodiscard]] QString storePath() const;
        [[nodiscard]] QString error() const;
        [[nodiscard]] int revision() const;
        QString replace(const QVector<Profile>& profiles, int expectedRevision);
        QString remove(const QString& id, int expectedRevision);
        QString deleteData(const QString& id, const QString& name, bool exclusive, int expectedRevision);
        QJsonObject request(const QString& action, const QJsonObject& body);
    signals:
        void profilesChanged();
        void changed(const QString& id);
        void deletionFinished(const QString& id, bool success, const QString& error);

    private:
        QJsonObject snapshot(const QString& id) const;
        void reconcile();
        ProfileStore store_;
        QVector<Profile> profiles_;
        QMap<QString, ProfileRuntime*> runtimes_;
        QString error_;
        int revision_ = 0;
    };
} // namespace codexui::manager
