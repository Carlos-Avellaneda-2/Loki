#pragma once

#include "model/ids.h"

#include <QSet>
#include <QString>
#include <QWidget>

class QAction;
class QLabel;
class QListView;
class QStackedWidget;
class QTreeView;

namespace loki::ui {

class MediaServices;
class ProjectController;
class ProjectItemModel;

// Premiere-style Project panel: bins and media in a list (with metadata columns) or icon view
// (thumbnails); import by dialog or drag & drop; rename/new bin/delete through commands.
class ProjectPanel final : public QWidget {
    Q_OBJECT

public:
    ProjectPanel(ProjectController& controller, MediaServices& services, QWidget* parent = nullptr);
    ~ProjectPanel() override;

    ProjectPanel(const ProjectPanel&) = delete;
    ProjectPanel& operator=(const ProjectPanel&) = delete;
    ProjectPanel(ProjectPanel&&) = delete;
    ProjectPanel& operator=(ProjectPanel&&) = delete;

    [[nodiscard]] ProjectItemModel* itemModel() const noexcept { return m_model; }
    [[nodiscard]] QAction* importAction() const noexcept { return m_importAction; }

    // Media items selected in the panel, in view order.
    [[nodiscard]] std::vector<model::MediaId> selectedMedia() const;

    void setIconView(bool icons);
    [[nodiscard]] bool isIconView() const;

signals:
    // Double-click on a media item (opens it in the Source Monitor).
    void mediaActivated(loki::model::MediaId id);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void importWithDialog();
    void newBin();
    void deleteSelection();
    void activate(const QModelIndex& index);
    [[nodiscard]] QModelIndex currentIndex() const;
    void updateStatus();
    void rememberExpanded();
    void restoreExpanded();

    ProjectController& m_controller;
    MediaServices& m_services;
    ProjectItemModel* m_model = nullptr;
    QStackedWidget* m_stack = nullptr;
    QTreeView* m_tree = nullptr;
    QListView* m_icons = nullptr;
    QLabel* m_status = nullptr;
    QAction* m_importAction = nullptr;
    QAction* m_newBinAction = nullptr;
    QAction* m_deleteAction = nullptr;
    QAction* m_listViewAction = nullptr;
    QAction* m_iconViewAction = nullptr;
    QAction* m_upAction = nullptr;
    QSet<QString> m_expandedBins;
};

} // namespace loki::ui
