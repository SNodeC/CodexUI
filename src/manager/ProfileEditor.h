// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "codex/ui/UiStyle.h"
#include "manager/Profile.h"

#include <functional>

class QLineEdit;
class QCheckBox;

namespace codexui::manager {
    class ProfileEditor final : public UiStyle::ScrollFormDialog {
    public:
        ProfileEditor(Profile profile, QVector<Profile> others, QWidget* parent = nullptr);
        ~ProfileEditor() override;
        [[nodiscard]] Profile profile() const;

    private:
        Profile profile_;
        QVector<Profile> others_;
        QVector<std::function<void(Profile&)>> readers_;
    };
} // namespace codexui::manager
