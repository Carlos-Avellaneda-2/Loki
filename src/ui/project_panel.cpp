#include "ui/project_panel.h"

#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"
#include "ui/project_item_model.h"

#include <QAction>
#include <QActionGroup>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QStackedWidget>
#include <QStyle>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace loki::ui {

ProjectPanel::ProjectPanel(ProjectController& controller, MediaServices& services, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_services(services) {
    setAcceptDrops(true);
    m_model = new ProjectItemModel(controller, services, this);

    auto* toolbar = new QToolBar(this);
    toolbar->setIconSize(QSize(16, 16));
    m_importAction = toolbar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Import..."));
    m_importAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    m_importAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_newBinAction = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder), tr("New Bin"));
    m_deleteAction = toolbar->addAction(style()->standardIcon(QStyle::SP_TrashIcon), tr("Delete"));
    m_deleteAction->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
    m_deleteAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(m_deleteAction);
    addAction(m_importAction);
    toolbar->addSeparator();
    auto* viewGroup = new QActionGroup(this);
    m_listViewAction = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("List View"));
    m_iconViewAction = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogListView), tr("Icon View"));
    for (auto* a : {m_listViewAction, m_iconViewAction}) {
        a->setCheckable(true);
        viewGroup->addAction(a);
    }
    m_listViewAction->setChecked(true);
    m_upAction = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogToParent), tr("Up"));
    m_upAction->setEnabled(false);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_model);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setIconSize(QSize(32, 18));
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(ProjectItemModel::Name, 220);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

    m_icons = new QListView(this);
    m_icons->setModel(m_model);
    m_icons->setViewMode(QListView::IconMode);
    m_icons->setIconSize(QSize(160, 90));
    m_icons->setGridSize(QSize(176, 124));
    m_icons->setResizeMode(QListView::Adjust);
    m_icons->setMovement(QListView::Static);
    m_icons->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_icons->setDragDropMode(QAbstractItemView::DragDrop);
    m_icons->setDefaultDropAction(Qt::MoveAction);
    m_icons->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_icons->setWordWrap(true);
    m_icons->setContextMenuPolicy(Qt::CustomContextMenu);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_icons);

    m_status = new QLabel(this);
    m_status->setContentsMargins(6, 2, 6, 2);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_status);

    connect(m_importAction, &QAction::triggered, this, &ProjectPanel::importWithDialog);
    connect(m_newBinAction, &QAction::triggered, this, &ProjectPanel::newBin);
    connect(m_deleteAction, &QAction::triggered, this, &ProjectPanel::deleteSelection);
    connect(m_listViewAction, &QAction::triggered, this, [this] { setIconView(false); });
    connect(m_iconViewAction, &QAction::triggered, this, [this] { setIconView(true); });
    connect(m_upAction, &QAction::triggered, this, [this] {
        const auto root = m_icons->rootIndex();
        m_icons->setRootIndex(root.parent());
        m_upAction->setEnabled(root.parent().isValid());
    });
    connect(m_tree, &QTreeView::doubleClicked, this, &ProjectPanel::activate);
    connect(m_icons, &QListView::doubleClicked, this, &ProjectPanel::activate);

    const auto contextMenu = [this](QAbstractItemView* view, const QPoint& pos) {
        QMenu menu(this);
        const auto idx = view->indexAt(pos);
        menu.addAction(m_importAction);
        menu.addAction(m_newBinAction);
        if (idx.isValid()) {
            menu.addSeparator();
            menu.addAction(tr("Rename"), this, [view, idx] { view->edit(idx.siblingAtColumn(0)); });
            menu.addAction(m_deleteAction);
        }
        menu.exec(view->viewport()->mapToGlobal(pos));
    };
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this, contextMenu](const QPoint& p) { contextMenu(m_tree, p); });
    connect(m_icons, &QWidget::customContextMenuRequested, this, [this, contextMenu](const QPoint& p) { contextMenu(m_icons, p); });

    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &ProjectPanel::rememberExpanded);
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        restoreExpanded();
        m_icons->setRootIndex({});
        m_upAction->setEnabled(false);
        updateStatus();
    });
    connect(&m_services, &MediaServices::importFinished, this, [this](int imported, const QStringList& errors) {
        updateStatus();
        if (!errors.isEmpty()) {
            QMessageBox box(QMessageBox::Warning, tr("Import"),
                            tr("%n item(s) imported. Some files could not be imported:", nullptr, imported),
                            QMessageBox::Ok, this);
            box.setDetailedText(errors.join(QLatin1Char('\n')));
            box.exec();
        }
    });
    updateStatus();
}

ProjectPanel::~ProjectPanel() = default;

void ProjectPanel::setIconView(bool icons) {
    m_stack->setCurrentWidget(icons ? static_cast<QWidget*>(m_icons) : m_tree);
    (icons ? m_iconViewAction : m_listViewAction)->setChecked(true);
    m_upAction->setVisible(icons);
}

bool ProjectPanel::isIconView() const {
    return m_stack->currentWidget() == m_icons;
}

QModelIndex ProjectPanel::currentIndex() const {
    const auto* view = isIconView() ? static_cast<QAbstractItemView*>(m_icons) : m_tree;
    const auto idx = view->currentIndex();
    if (!idx.isValid() && isIconView()) {
        return m_icons->rootIndex();
    }
    return idx.siblingAtColumn(0);
}

void ProjectPanel::importWithDialog() {
    QStringList patterns;
    for (const auto& ext : media::importableExtensions()) {
        patterns << QStringLiteral("*") + QString::fromStdString(ext);
    }
    const auto filter = tr("Media Files (%1);;All Files (*)").arg(patterns.join(QLatin1Char(' ')));
    const auto files = QFileDialog::getOpenFileNames(this, tr("Import"), QString(), filter);
    if (!files.isEmpty()) {
        m_services.importPaths(files, m_model->targetBin(currentIndex()));
        m_status->setText(tr("Importing %n file(s)...", nullptr, static_cast<int>(files.size())));
    }
}

void ProjectPanel::newBin() {
    model::Bin bin;
    bin.name = tr("Bin %1").arg(m_controller.project().bins().size() + 1).toStdString();
    bin.parent = m_model->targetBin(currentIndex());
    auto command = std::make_unique<commands::AddBinCommand>(bin);
    const auto id = command->binId();
    if (m_controller.execute(std::move(command))) {
        const auto idx = m_model->indexOfBin(id);
        auto* view = isIconView() ? static_cast<QAbstractItemView*>(m_icons) : m_tree;
        if (idx.parent().isValid() && view == m_tree) {
            m_tree->expand(idx.parent());
        }
        view->setCurrentIndex(idx);
        view->edit(idx);
    }
}

void ProjectPanel::deleteSelection() {
    const auto* view = isIconView() ? static_cast<QAbstractItemView*>(m_icons) : m_tree;
    std::vector<model::BinId> bins;
    std::vector<model::MediaId> media;
    for (const auto& idx : view->selectionModel()->selectedRows(0)) {
        if (m_model->isBin(idx)) {
            bins.push_back(m_model->binId(idx));
        } else if (auto id = m_model->mediaId(idx)) {
            media.push_back(id);
        }
    }
    if (bins.empty() && media.empty()) {
        return;
    }
    const auto& project = m_controller.project();
    std::size_t usedClips = 0;
    for (const auto& m : project.media()) {
        const bool selected = std::find(media.begin(), media.end(), m.id) != media.end() ||
                              std::any_of(bins.begin(), bins.end(), [&](model::BinId b) {
                                  return !m.bin.isNull() && project.binIsWithin(m.bin, b);
                              });
        if (selected) {
            usedClips += project.clipsUsingMedia(m.id).size();
        }
    }
    if (usedClips > 0 &&
        QMessageBox::question(this, tr("Delete"),
                              tr("The selected media is used by %n clip(s) in sequences. Delete it and those clips?",
                                 nullptr, static_cast<int>(usedClips))) != QMessageBox::Yes) {
        return;
    }
    std::vector<std::unique_ptr<commands::Command>> removals;
    for (const auto id : media) {
        // Media inside a selected bin is removed with the bin.
        const auto* m = project.findMedia(id);
        const bool inSelectedBin = m != nullptr && std::any_of(bins.begin(), bins.end(), [&](model::BinId b) {
            return !m->bin.isNull() && project.binIsWithin(m->bin, b);
        });
        if (!inSelectedBin) {
            removals.push_back(std::make_unique<commands::RemoveMediaCommand>(id));
        }
    }
    for (const auto id : bins) {
        const auto* b = project.findBin(id);
        const bool nested = b != nullptr && !b->parent.isNull() && std::any_of(bins.begin(), bins.end(), [&](model::BinId o) {
            return o != id && project.binIsWithin(b->parent, o);
        });
        if (!nested) {
            removals.push_back(commands::removeBinRecursive(project, id));
        }
    }
    m_controller.execute(std::make_unique<commands::MacroCommand>("Delete", std::move(removals)));
}

void ProjectPanel::activate(const QModelIndex& index) {
    const auto idx = index.siblingAtColumn(0);
    if (m_model->isBin(idx)) {
        if (isIconView()) {
            m_icons->setRootIndex(idx);
            m_upAction->setEnabled(true);
        }
        return;
    }
    if (const auto id = m_model->mediaId(idx)) {
        emit mediaActivated(id);
    }
}

void ProjectPanel::updateStatus() {
    const auto& project = m_controller.project();
    if (m_services.isImporting()) {
        m_status->setText(tr("Importing..."));
        return;
    }
    m_status->setText(tr("%n item(s)", nullptr, static_cast<int>(project.media().size())));
}

void ProjectPanel::rememberExpanded() {
    m_expandedBins.clear();
    const std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < m_model->rowCount(parent); ++r) {
            const auto idx = m_model->index(r, 0, parent);
            if (m_model->isBin(idx) && m_tree->isExpanded(idx)) {
                m_expandedBins.insert(idx.data(ProjectItemModel::ItemIdRole).toString());
                walk(idx);
            }
        }
    };
    walk({});
}

void ProjectPanel::restoreExpanded() {
    for (const auto& id : m_expandedBins) {
        if (auto bin = model::BinId::parse(id.toStdString())) {
            m_tree->expand(m_model->indexOfBin(*bin));
        }
    }
}

void ProjectPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void ProjectPanel::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const auto& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths << url.toLocalFile();
        }
    }
    if (!paths.isEmpty()) {
        m_services.importPaths(paths, {});
        event->acceptProposedAction();
        updateStatus();
    }
}

} // namespace loki::ui
