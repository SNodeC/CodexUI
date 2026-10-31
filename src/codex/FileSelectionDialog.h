// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#ifndef CODEXUI_CODEX_FILESELECTIONDIALOG_H
#define CODEXUI_CODEX_FILESELECTIONDIALOG_H
#include <QDialog>
#include <QStringList>
#include <QThreadPool>
#include <functional>
#include <nlohmann/json.hpp>
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace codexui::codex {
    class FileSelectionDialog final : public QDialog {
    public:
        using Filesystem = std::function<void(QObject*, std::string, nlohmann::json, std::function<void(nlohmann::json, std::string)>)>;
        using Access = std::function<Filesystem()>;
        explicit FileSelectionDialog(QString initialDirectory,
                                     QWidget* parent = nullptr,
                                     QString targetTitle = {},
                                     QStringList choices = {},
                                     Filesystem filesystem = {},
                                     bool files = false);
        ~FileSelectionDialog() override {
            pool.waitForDone();
        }
        [[nodiscard]] QString selectedDirectory() const;
        [[nodiscard]] QStringList selectedFiles() const;
        [[nodiscard]] bool localFiles() const;
        [[nodiscard]] QString uploadDirectory() const;

        [[nodiscard]] Filesystem access() const {
            return filesystem;
        }

    private:
        void loadDirectory();
        void activateEntry(QTreeWidgetItem* entry);
        void updateActions();
        QComboBox* source = nullptr;
        QLineEdit* location;
        QTreeWidget* entries;
        QLabel* status;
        QPushButton* acceptButton;
        Filesystem filesystem;
        QString serverDirectory, localDirectory;
        bool files, busy = false, showHidden = false;
        std::uint64_t request = 0;
        QThreadPool pool;
    };
} // namespace codexui::codex
#endif
