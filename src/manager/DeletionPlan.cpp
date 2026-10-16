// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/DeletionPlan.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

static QString deletionScript() {
    Q_INIT_RESOURCE(deletion);
    QFile file(":/manager/DeleteProfileData.py");
    const bool opened = file.open(QIODevice::ReadOnly);
    return opened ? QString::fromUtf8(file.readAll()) : QString{};
}

namespace codexui::manager {
    Command DeletionTarget::command(const QString& action) const {
        auto parameters = specification;
        parameters.insert("action", action);
        return onHost(
            host,
            {"python3", {"-c", deletionScript(), QString::fromUtf8(QJsonDocument(parameters).toJson(QJsonDocument::Compact))}, {}, {}});
    }
    DeletionPlan DeletionPlan::forProfile(const Profile& p, const QVector<Profile>& others) {
        DeletionPlan result;
        result.profileId = p.id;
        const Host host = p.provider.transport == "stdio" ? p.bridge : p.appServer;
        QJsonArray shared;
        QJsonArray localHomes;
        for (const auto& other : others) {
            const auto& otherHost = other.provider.transport == "stdio" ? other.bridge : other.appServer;
            if (other.id != p.id && host.key() == otherHost.key()) {
                shared.append(other.codexHome);
            }
            if (otherHost.key() == Host{}.key()) {
                localHomes.append(other.codexHome);
            }
            if (host.key() == Host{}.key()) {
                shared.append(other.settingsDirectory());
            }
        }
        if (host.key() == Host{}.key()) {
            shared.append(p.settingsDirectory());
            localHomes.append(p.codexHome);
        }
        result.targets = {{host, {{"path", p.codexHome}, {"profile", p.id}, {"kind", "home"}, {"others", shared}}},
                          {Host{}, {{"path", p.settingsDirectory()}, {"profile", p.id}, {"kind", "settings"}, {"others", localHomes}}}};
        if (!p.errors().isEmpty()) {
            result.error = p.errors().join('\n');
        } else if (host.ownership == Ownership::External && host.sshTarget.isEmpty()) {
            result.error =
                "The app-server host is externally managed without an SSH target. Follow the manual instructions on its actual host.";
        }
        return result;
    }
    QString DeletionPlan::description() const {
        QString result = "Permanently delete:\n";
        for (const auto& target : targets) {
            result += "\n" +
                      (target.host.ownership == Ownership::External && target.host.sshTarget.isEmpty()
                           ? QString("External app-server host (run there manually)")
                           : target.host.label()) +
                      "\n" + target.specification.value("path").toString() + "\n";
        }
        result +=
            "\nThe manager will stop its owned components first. Conversations, configuration, locally stored credentials and manager "
            "settings will be lost. External services must be stopped by their owner. The registration is removed only after cleanup "
            "succeeds. "
            "Deleting files does not revoke OpenAI authorization or erase credentials in an external OS keyring.";
        return result;
    }
    QString DeletionPlan::manualInstructions() const {
        QString result = "MANUAL CLEANUP\nStop all processes using this home first. Python 3 and Linux /proc are required on each host. "
                         "Each guarded command previews its exact target and asks you to type that directory path. Protected, shared, "
                         "symlinked or unowned homes are refused.\n\n";
        for (auto target : targets) {
            result += (target.host.ownership == Ownership::External && target.host.sshTarget.isEmpty()
                           ? QString("External app-server host (run there manually)")
                           : target.host.label()) +
                      " — run directly on this target host:\n";
            target.host = Host{};
            result += target.command("manual").shell() + "\n\n";
        }
        result += "After both commands succeed, use Remove from manager to remove the registration. For an unmarked existing home, use "
                  "registration removal unless you have independently established exclusive ownership; see the profile-manager guide.";
        return result;
    }
} // namespace codexui::manager
