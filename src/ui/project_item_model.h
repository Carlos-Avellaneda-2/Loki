#pragma once

#include "model/ids.h"

#include <QAbstractItemModel>
#include <QStringList>

#include <memory>
#include <unordered_map>
#include <vector>

namespace loki::ui {

class MediaServices;
class ProjectController;

// Tree of bins and media for the Project panel. Reads the model through ProjectController and
// edits it only with commands (rename, move to bin, import by drop).
class ProjectItemModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column { Name, FrameRate, Duration, VideoInfo, VideoCodec, AudioInfo, ColumnCount };
    enum Role { ItemIdRole = Qt::UserRole + 1, IsBinRole };
    static constexpr auto kMimeType = "application/x-loki-project-items";

    ProjectItemModel(ProjectController& controller, MediaServices& services, QObject* parent = nullptr);
    ~ProjectItemModel() override;

    ProjectItemModel(const ProjectItemModel&) = delete;
    ProjectItemModel& operator=(const ProjectItemModel&) = delete;
    ProjectItemModel(ProjectItemModel&&) = delete;
    ProjectItemModel& operator=(ProjectItemModel&&) = delete;

    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;

    [[nodiscard]] bool isBin(const QModelIndex& index) const;
    [[nodiscard]] model::BinId binId(const QModelIndex& index) const;     // null if not a bin
    [[nodiscard]] model::MediaId mediaId(const QModelIndex& index) const; // null if not media
    [[nodiscard]] QModelIndex indexOfBin(model::BinId id) const;
    [[nodiscard]] QModelIndex indexOfMedia(model::MediaId id) const;
    // Bin that should receive new items for a selection (the bin itself, or the media's bin).
    [[nodiscard]] model::BinId targetBin(const QModelIndex& index) const;

    // Encodes media ids for drag & drop to other panels (Source Monitor, Timeline).
    [[nodiscard]] static std::vector<model::MediaId> decodeMediaIds(const QMimeData* data);

private:
    struct Node;
    void rebuild();
    [[nodiscard]] Node* nodeOf(const QModelIndex& index) const;
    [[nodiscard]] QModelIndex indexOfNode(const Node* node, int column = 0) const;

    ProjectController& m_controller;
    MediaServices& m_services;
    std::unique_ptr<Node> m_root;
    std::unordered_map<Uuid, Node*> m_nodes;
};

} // namespace loki::ui
