// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "manager/ProfileRuntime.h"

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
    public:
        explicit ManagerWindow(QString storePath, QWidget* parent = nullptr);
        ~ManagerWindow() override;

    protected:
        void closeEvent(QCloseEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;

    private:
        void rebuild();
        void reflow();
        void createOrEdit(const QString& id = {});
        void remove(const QString& id);
        void showProfile(const QString& id);
        void updateProfile(const QString& id);
        void updateLogs();
        bool persist(const QVector<Profile>& profiles);
        void importProfiles();
        void exportProfile(const QString& id);
        void quitManager();
        [[nodiscard]] ProfileRuntime* runtime(const QString& id) const;

        ProfileStore store_;
        QVector<Profile> profiles_;
        QMap<QString, ProfileRuntime*> runtimes_;
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
        bool storeValid_ = true;
    };
} // namespace codexui::manager
