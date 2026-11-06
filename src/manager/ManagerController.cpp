// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ManagerController.h"

#include <QJsonArray>
#include <QTimer>
#include <algorithm>

namespace codexui::manager {
    ManagerController::ManagerController(QString path, QObject* parent)
        : QObject(parent)
        , store_(std::move(path)) {
        profiles_ = store_.load(&error_);
        reconcile();
    }
    const QVector<Profile>& ManagerController::profiles() const {
        return profiles_;
    }
    const QMap<QString, ProfileRuntime*>& ManagerController::runtimes() const {
        return runtimes_;
    }
    ProfileRuntime* ManagerController::runtime(const QString& id) const {
        return runtimes_.value(id);
    }
    QString ManagerController::storePath() const {
        return store_.filePath();
    }
    QString ManagerController::error() const {
        return error_;
    }
    int ManagerController::revision() const {
        return revision_;
    }

    QString ManagerController::replace(const QVector<Profile>& profiles, int expectedRevision) {
        QString error = error_;
        if (error.isEmpty() && expectedRevision != revision_) {
            error = "Profiles changed in another interface. Reload before saving.";
        }
        for (auto it = runtimes_.cbegin(); error.isEmpty() && it != runtimes_.cend(); ++it) {
            auto replacement = std::find_if(profiles.cbegin(), profiles.cend(), [&](const Profile& p) {
                return p.id == it.key();
            });
            if ((replacement == profiles.cend() || replacement->toJson() != it.value()->profile().toJson()) &&
                (it.value()->busy() || it.value()->hasOwnedProcesses())) {
                error = "Stop owned components and verify their status before changing or removing this profile.";
            }
        }
        if (error.isEmpty() && store_.save(profiles, &error)) {
            profiles_ = profiles;
            ++revision_;
            reconcile();
            emit profilesChanged();
        }
        return error;
    }
    QString ManagerController::remove(const QString& id, int expectedRevision) {
        auto next = profiles_;
        next.erase(std::remove_if(next.begin(),
                                  next.end(),
                                  [&](const Profile& p) {
                                      return p.id == id;
                                  }),
                   next.end());
        return replace(next, expectedRevision);
    }
    void ManagerController::reconcile() {
        for (auto it = runtimes_.begin(); it != runtimes_.end();) {
            const auto match = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const Profile& p) {
                return p.id == it.key() && p.toJson() == it.value()->profile().toJson();
            });
            if (match == profiles_.cend()) {
                delete it.value();
                it = runtimes_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& p : profiles_) {
            if (!runtime(p.id)) {
                auto* r = new ProfileRuntime(p, this);
                runtimes_.insert(p.id, r);
                connect(r, &ProfileRuntime::changed, this, [this, id = p.id] {
                    emit changed(id);
                });
                connect(r, &ProfileRuntime::logAdded, this, [this, id = p.id] {
                    emit changed(id);
                });
                connect(
                    r,
                    &ProfileRuntime::deletionFinished,
                    this,
                    [this, id = p.id](bool success, const QString& message) {
                        QString error = message;
                        if (success) {
                            error = remove(id, revision_);
                        }
                        emit deletionFinished(id, success && error.isEmpty(), error);
                    },
                    Qt::QueuedConnection);
                QTimer::singleShot(0, r, &ProfileRuntime::refresh);
            }
        }
    }
    QString ManagerController::deleteData(const QString& id, const QString& name, bool exclusive, int expectedRevision) {
        auto* r = runtime(id);
        QString error;
        if (!r || expectedRevision != revision_) {
            error = "Profile changed or was removed. Reload the deletion preview.";
        } else if (!exclusive || name != r->profile().name || r->busy()) {
            error = "Confirm the exact profile name and exclusive use, and finish any current operation.";
        } else {
            // Inspect again at execution; a browser never supplies trusted paths or a deletion plan.
            r->inspectDeletion(profiles_, [this, r, expectedRevision](DeletionPlan plan) {
                if (expectedRevision != revision_) {
                    plan.error = "Profiles changed. Reload the preview.";
                }
                r->deleteData(std::move(plan));
            });
        }
        return error;
    }
    QJsonObject ManagerController::snapshot(const QString& id) const {
        QJsonArray profiles;
        QJsonObject detail;
        for (const auto& p : profiles_) {
            auto* r = runtime(p.id);
            profiles.append(QJsonObject{{"id", p.id},
                                        {"name", p.name},
                                        {"description", p.description},
                                        {"accent", accentColor(p.accent)},
                                        {"location", p.bridge.label()},
                                        {"status", r->status()},
                                        {"account", r->accountStatus()},
                                        {"ready", r->ready()},
                                        {"busy", r->busy()},
                                        {"browserUrl", p.browserUrl()},
                                        {"editable", !r->busy() && !r->hasOwnedProcesses()}});
            if (p.id == id) {
                QJsonObject components;
                const auto states = r->components();
                for (auto it = states.cbegin(); it != states.cend(); ++it) {
                    components.insert(it.key(), it.value());
                }
                const auto plan = DeletionPlan::forProfile(p, profiles_);
                detail = {{"configuration", p.toJson()},
                          {"components", components},
                          {"logs", r->logs()},
                          {"instructions", LaunchPlan::forProfile(p).instructions(p)},
                          {"deletionPreview", plan.description()},
                          {"deletionInstructions", plan.manualInstructions()},
                          {"authenticationAvailable", r->authenticationAvailable()},
                          {"authenticating", r->authenticating()},
                          {"authenticationMessage", r->authenticationMessage()},
                          {"loginUrl", r->loginUrl()},
                          {"loginCode", r->loginCode()}};
            }
        }
        return {{"profiles", profiles},
                {"detail", detail},
                {"revision", revision_},
                {"error", error_},
                {"transports",
                 QJsonObject{{"frontend", QJsonArray::fromStringList(frontendTransports())},
                             {"provider", QJsonArray::fromStringList(providerTransports())},
                             {"browser", QJsonArray::fromStringList(browserTransports())}}}};
    }
    QJsonObject ManagerController::request(const QString& action, const QJsonObject& body) {
        const auto id = body.value("id").toString();
        auto* r = runtime(id);
        const int revision = body.value("revision").toInt(-1);
        QJsonObject result;
        QString error;
        if (action == "state") {
            result = snapshot(id);
        } else if (action == "new") {
            result = {{"configuration", Profile::create().toJson()}, {"revision", revision_}};
        } else if (action == "save" || action == "import") {
            auto next = profiles_;
            const auto incoming = action == "save" ? QJsonArray{body.value("configuration")} : body.value("profiles").toArray();
            if (incoming.isEmpty() || (action == "import" && body.value("version").toInt() != 1)) {
                error = "Invalid profile document.";
            }
            for (auto it = incoming.cbegin(); error.isEmpty() && it != incoming.cend(); ++it) {
                const auto p = Profile::fromJson(it->toObject());
                auto old = std::find_if(next.begin(), next.end(), [&](const Profile& x) {
                    return x.id == p.id;
                });
                if (old != next.end() && action == "import") {
                    error = "An imported profile ID already exists.";
                } else if (old != next.end()) {
                    *old = p;
                } else {
                    next.push_back(p);
                }
            }
            if (error.isEmpty()) {
                error = replace(next, revision);
            }
        } else if (!r) {
            error = "Profile not found.";
        } else if (action == "remove") {
            error = remove(id, revision);
        } else if (action == "delete") {
            error = deleteData(id, body.value("name").toString(), body.value("exclusive").toBool(), revision);
        } else if (action == "start") {
            r->start(false);
        } else if (action == "native") {
            r->openUi();
        } else if (action == "stop") {
            r->stop();
        } else if (action == "refresh") {
            r->refresh();
            r->readLogs();
            r->refreshAccount();
        } else if (action == "signin") {
            r->signIn(body.value("method").toString(), body.value("key").toString());
        } else if (action == "signout") {
            r->signOut();
        } else if (action == "cancel") {
            r->cancelLogin();
        } else {
            error = "Unknown manager operation.";
        }
        if (!error.isEmpty()) {
            result = {{"error", error}};
        }
        return result;
    }
} // namespace codexui::manager
