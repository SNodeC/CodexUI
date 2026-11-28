// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "manager/LaunchPlan.h"
#include "manager/Profile.h"

#include <qcontainerfwd.h>
#include <qjsonobject.h>
#include <qlist.h>
#include <qstring.h>

namespace codexui::manager {
    struct DeletionTarget {
        Host host;
        QJsonObject specification;
        [[nodiscard]] Command command(const QString& action) const;
    };
    struct DeletionPlan {
        QString profileId;
        QVector<DeletionTarget> targets;
        QString error;
        [[nodiscard]] static DeletionPlan forProfile(const Profile& profile, const QVector<Profile>& others);
        [[nodiscard]] QString description() const;
        [[nodiscard]] QString manualInstructions() const;
    };
} // namespace codexui::manager
