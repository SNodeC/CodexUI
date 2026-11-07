// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "manager/ManagerController.h"

#include <QMainWindow>

class QCloseEvent;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;
class QSystemTrayIcon;
class QTabWidget;
class QToolButton;

namespace codexui::manager {
    class ManagerWindow final : public QMainWindow {
        Q_OBJECT
    public:
        explicit ManagerWindow(ManagerController& manager, QWidget* parent = nullptr);

    signals:
        void webSettingsRequested(const QJsonObject& request);
        void webSettingsReceived(const QJsonObject& result);

    protected:
        void closeEvent(QCloseEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;

    private:
        void showWebSettings();
        void rebuild();
        void reflow();
        void createOrEdit(const QString& id = {});
        void remove(const QString& id);
        void forgetRegistration(const QString& id, int revision);
        void showDeletion(const QString& id);
        void showProfile(const QString& id);
        void showAuthentication(const QString& id);
        void updateProfile(const QString& id);
        void updateLogs();
        void importProfiles();
        void exportProfile(const QString& id);
        void quitManager();
        [[nodiscard]] ProfileRuntime* runtime(const QString& id) const;

        ManagerController& manager_;
        QMap<QString, QWidget*> cards_;
        QStackedWidget* pages_;
        QWidget* overview_;
        QWidget* gridWidget_;
        QGridLayout* grid_ = nullptr;
        QLineEdit* search_;
        QLabel* summary_;
        QLabel* detailTitle_;
        QLabel* detailState_;
        QLabel* topology_;
        QLabel* componentDetails_;
        QLabel* technicalDetails_;
        QPlainTextEdit* configuration_;
        QPlainTextEdit* instructions_;
        QPlainTextEdit* logs_;
        QLineEdit* logFilter_;
        QToolButton* logPause_;
        QString selected_;
        QSystemTrayIcon* tray_;
    };
} // namespace codexui::manager
