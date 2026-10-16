// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "manager/Profile.h"

namespace codexui::manager {
    struct Command {
        QString program;
        QStringList arguments;
        QMap<QString, QString> environment;
        QString workingDirectory;
        [[nodiscard]] QString shell() const;
    };
    struct Component {
        QString name;
        Host host;
        QString unit;
        Command command;
        [[nodiscard]] Command configurationCheck() const;
        [[nodiscard]] bool acceptsConfiguration(int code, const QString& output) const;
    };
    struct LaunchPlan {
        QVector<Component> backends;
        Component ui;
        Command probe;
        [[nodiscard]] static LaunchPlan forProfile(const Profile& profile);
        [[nodiscard]] QString instructions(const Profile& profile) const;
    };
    [[nodiscard]] Command onHost(const Host& host, Command command);
    [[nodiscard]] Command serviceCommand(const Component& component, const QString& action);
    [[nodiscard]] Command serviceStart(const Component& component);
    [[nodiscard]] Command serviceLogs(const Component& component);
} // namespace codexui::manager
