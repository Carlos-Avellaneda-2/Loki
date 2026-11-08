#include "ui/project_item_model.h"

#include "commands/project_commands.h"
#include "model/project_json.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"
#include "util/timecode.h"

#include <QApplication>
#include <QIcon>
#include <QMimeData>
#include <QPixmap>
#include <QStyle>
#include <QUrl>

#include <algorithm>
#include <functional>

namespace loki::ui {

struct ProjectItemModel::Node {
    bool bin = false;
    Uuid id;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    [[nodiscard]] int row() const {
        if (parent == nullptr) {
            return 0;
        }
        const auto& siblings = parent->children;
        const auto it = std::find_if(siblings.begin(), siblings.end(), [this](const auto& c) { return c.get() == this; });
        return static_cast<int>(std::distance(siblings.begin(), it));
    }
};

namespace {

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString formatRate(const Rational& r) {
    if (!r.isPositive()) {
        return {};
    }
    return QString::number(r.toDouble(), 'f', r.isInteger() ? 0 : 3) + QStringLiteral(" fps");
}

QString formatDuration(const model::MediaInfo& info) {
    if (info.isStill || !info.duration.isPositive()) {
        return info.isStill ? QStringLiteral("Still") : QString();
    }
    const Rational rate = info.frameRate.isPositive() ? info.frameRate : Rational(30);
    const auto frames = timeToFrames(info.duration, rate, Rounding::Nearest);
    return qs(formatTimecode(frames, rate, supportsDropFrame(rate)));
}

} // namespace

ProjectItemModel::ProjectItemModel(ProjectController& controller, MediaServices& services, QObject* parent)
    : QAbstractItemModel(parent), m_controller(controller), m_services(services), m_root(std::make_unique<Node>()) {
    rebuild();
    connect(&m_controller, &ProjectController::projectReset, this, [this] {
        beginResetModel();
        rebuild();
        endResetModel();
    });
    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        switch (e.kind) {
        case model::ChangeKind::MediaAdded:
        case model::ChangeKind::MediaRemoved:
        case model::ChangeKind::MediaChanged:
        case model::ChangeKind::BinAdded:
        case model::ChangeKind::BinRemoved:
        case model::ChangeKind::BinChanged:
            // Bins/media change rarely and in small numbers: a full rebuild keeps this simple.
            beginResetModel();
            rebuild();
            endResetModel();
            break;
        default: break;
        }
    });
    connect(&m_services, &MediaServices::thumbnailReady, this, [this](const QString& id) {
        const auto parsed = model::MediaId::parse(id.toStdString());
        if (parsed) {
            const auto idx = indexOfMedia(*parsed);
            if (idx.isValid()) {
                emit dataChanged(idx, idx, {Qt::DecorationRole});
            }
        }
    });
}

ProjectItemModel::~ProjectItemModel() = default;

void ProjectItemModel::rebuild() {
    m_root = std::make_unique<Node>();
    m_nodes.clear();
    const auto& project = m_controller.project();

    std::unordered_map<Uuid, Node*> bins;
    for (const auto& b : project.bins()) {
        auto node = std::make_unique<Node>();
        node->bin = true;
        node->id = b.id.uuid();
        bins[node->id] = node.get();
        m_nodes[node->id] = node.get();
        node.release(); // re-owned below once parents are known
    }
    const auto ownerOf = [&](const Uuid& parent) -> Node* {
        const auto it = bins.find(parent);
        return it != bins.end() ? it->second : m_root.get();
    };
    for (const auto& b : project.bins()) {
        Node* node = bins[b.id.uuid()];
        Node* owner = ownerOf(b.parent.uuid());
        node->parent = owner;
        owner->children.emplace_back(node);
    }
    for (const auto& m : project.media()) {
        auto node = std::make_unique<Node>();
        node->id = m.id.uuid();
        Node* owner = ownerOf(m.bin.uuid());
        node->parent = owner;
        m_nodes[node->id] = node.get();
        owner->children.push_back(std::move(node));
    }
    // Bins first, then alphabetical (case-insensitive), like Premiere's list view.
    const auto nameOf = [&](const Node& n) -> std::string {
        if (n.bin) {
            return project.findBin(model::BinId(n.id))->name;
        }
        return project.findMedia(model::MediaId(n.id))->name;
    };
    const std::function<void(Node&)> sortTree = [&](Node& n) {
        std::stable_sort(n.children.begin(), n.children.end(), [&](const auto& a, const auto& b) {
            if (a->bin != b->bin) {
                return a->bin;
            }
            return QString::compare(qs(nameOf(*a)), qs(nameOf(*b)), Qt::CaseInsensitive) < 0;
        });
        for (auto& c : n.children) {
            sortTree(*c);
        }
    };
    sortTree(*m_root);
}

ProjectItemModel::Node* ProjectItemModel::nodeOf(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : m_root.get();
}

QModelIndex ProjectItemModel::indexOfNode(const Node* node, int column) const {
    if (node == nullptr || node == m_root.get()) {
        return {};
    }
    return createIndex(node->row(), column, const_cast<Node*>(node));
}

QModelIndex ProjectItemModel::index(int row, int column, const QModelIndex& parent) const {
    const Node* p = nodeOf(parent);
    if (row < 0 || column < 0 || column >= ColumnCount || row >= static_cast<int>(p->children.size())) {
        return {};
    }
    return createIndex(row, column, p->children[static_cast<std::size_t>(row)].get());
}

QModelIndex ProjectItemModel::parent(const QModelIndex& child) const {
    const Node* n = nodeOf(child);
    return n == m_root.get() ? QModelIndex() : indexOfNode(n->parent);
}

int ProjectItemModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() && parent.column() != 0) {
        return 0;
    }
    return static_cast<int>(nodeOf(parent)->children.size());
}

int ProjectItemModel::columnCount(const QModelIndex&) const {
    return ColumnCount;
}

QVariant ProjectItemModel::data(const QModelIndex& index, int role) const {
    const Node* n = nodeOf(index);
    if (n == m_root.get()) {
        return {};
    }
    const auto& project = m_controller.project();
    if (role == ItemIdRole) {
        return QString::fromStdString(n->id.toString());
    }
    if (role == IsBinRole) {
        return n->bin;
    }
    if (n->bin) {
        const auto* bin = project.findBin(model::BinId(n->id));
        if (bin == nullptr) {
            return {};
        }
        if (index.column() == Name && (role == Qt::DisplayRole || role == Qt::EditRole)) {
            return qs(bin->name);
        }
        if (index.column() == Name && role == Qt::DecorationRole) {
            return QApplication::style()->standardIcon(QStyle::SP_DirIcon);
        }
        return {};
    }

    const auto* media = project.findMedia(model::MediaId(n->id));
    if (media == nullptr) {
        return {};
    }
    const auto& info = media->info;
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case Name: return qs(media->name);
        case FrameRate: return formatRate(info.frameRate);
        case Duration: return formatDuration(info);
        case VideoInfo: return info.hasVideo ? QStringLiteral("%1 x %2").arg(info.width).arg(info.height) : QString();
        case VideoCodec: return qs(info.videoCodec);
        case AudioInfo:
            return info.hasAudio ? QStringLiteral("%1 Hz - %2 ch").arg(info.sampleRate).arg(info.audioChannels)
                                 : QString();
        default: return {};
        }
    }
    if (role == Qt::DecorationRole && index.column() == Name) {
        const QImage thumb = m_services.thumbnail(*media);
        if (!thumb.isNull()) {
            return QIcon(QPixmap::fromImage(thumb));
        }
        return QApplication::style()->standardIcon(info.hasVideo ? QStyle::SP_FileIcon : QStyle::SP_MediaVolume);
    }
    if (role == Qt::ToolTipRole) {
        return QString::fromStdWString(media->path.wstring());
    }
    return {};
}

QVariant ProjectItemModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case Name: return tr("Name");
    case FrameRate: return tr("Frame Rate");
    case Duration: return tr("Media Duration");
    case VideoInfo: return tr("Video Info");
    case VideoCodec: return tr("Video Codec");
    case AudioInfo: return tr("Audio Info");
    default: return {};
    }
}

Qt::ItemFlags ProjectItemModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::ItemIsDropEnabled;
    }
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == Name) {
        f |= Qt::ItemIsEditable;
    }
    if (nodeOf(index)->bin) {
        f |= Qt::ItemIsDropEnabled;
    }
    return f;
}

bool ProjectItemModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || index.column() != Name) {
        return false;
    }
    const auto name = value.toString().trimmed().toStdString();
    if (name.empty()) {
        return false;
    }
    const Node* n = nodeOf(index);
    return n->bin ? m_controller.execute(commands::renameBin(model::BinId(n->id), name))
                  : m_controller.execute(commands::renameMedia(model::MediaId(n->id), name));
}

QStringList ProjectItemModel::mimeTypes() const {
    return {QString::fromLatin1(kMimeType), QStringLiteral("text/uri-list")};
}

QMimeData* ProjectItemModel::mimeData(const QModelIndexList& indexes) const {
    QStringList entries;
    for (const auto& idx : indexes) {
        if (idx.column() != Name) {
            continue;
        }
        const Node* n = nodeOf(idx);
        entries << (n->bin ? QStringLiteral("bin:") : QStringLiteral("media:")) + QString::fromStdString(n->id.toString());
    }
    auto* mime = new QMimeData();
    mime->setData(QString::fromLatin1(kMimeType), entries.join(QLatin1Char('\n')).toUtf8());
    return mime;
}

Qt::DropActions ProjectItemModel::supportedDropActions() const {
    return Qt::CopyAction | Qt::MoveAction;
}

Qt::DropActions ProjectItemModel::supportedDragActions() const {
    return Qt::CopyAction | Qt::MoveAction;
}

bool ProjectItemModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const {
    if (parent.isValid() && !nodeOf(parent)->bin) {
        return false;
    }
    return data->hasUrls() || data->hasFormat(QString::fromLatin1(kMimeType));
}

bool ProjectItemModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                    const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    const auto target = parent.isValid() ? model::BinId(nodeOf(parent)->id) : model::BinId();
    if (data->hasUrls()) {
        QStringList paths;
        for (const auto& url : data->urls()) {
            if (url.isLocalFile()) {
                paths << url.toLocalFile();
            }
        }
        m_services.importPaths(paths, target);
        return !paths.isEmpty();
    }
    // Internal move into `target`.
    std::vector<std::unique_ptr<commands::Command>> moves;
    const auto entries = QString::fromUtf8(data->data(QString::fromLatin1(kMimeType))).split(QLatin1Char('\n'));
    for (const auto& entry : entries) {
        if (entry.startsWith(QStringLiteral("bin:"))) {
            if (auto id = model::BinId::parse(entry.mid(4).toStdString()); id && *id != target) {
                moves.push_back(commands::moveBin(*id, target));
            }
        } else if (entry.startsWith(QStringLiteral("media:"))) {
            if (auto id = model::MediaId::parse(entry.mid(6).toStdString())) {
                moves.push_back(commands::moveMediaToBin(*id, target));
            }
        }
    }
    if (moves.empty()) {
        return false;
    }
    m_controller.execute(std::make_unique<commands::MacroCommand>("Move to Bin", std::move(moves)));
    return false; // we already moved through commands; the view must not remove rows itself
}

bool ProjectItemModel::isBin(const QModelIndex& index) const {
    return index.isValid() && nodeOf(index)->bin;
}

model::BinId ProjectItemModel::binId(const QModelIndex& index) const {
    return isBin(index) ? model::BinId(nodeOf(index)->id) : model::BinId();
}

model::MediaId ProjectItemModel::mediaId(const QModelIndex& index) const {
    return index.isValid() && !nodeOf(index)->bin ? model::MediaId(nodeOf(index)->id) : model::MediaId();
}

QModelIndex ProjectItemModel::indexOfBin(model::BinId id) const {
    const auto it = m_nodes.find(id.uuid());
    return it == m_nodes.end() ? QModelIndex() : indexOfNode(it->second);
}

QModelIndex ProjectItemModel::indexOfMedia(model::MediaId id) const {
    const auto it = m_nodes.find(id.uuid());
    return it == m_nodes.end() ? QModelIndex() : indexOfNode(it->second);
}

model::BinId ProjectItemModel::targetBin(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {};
    }
    const Node* n = nodeOf(index);
    if (n->bin) {
        return model::BinId(n->id);
    }
    return n->parent != m_root.get() ? model::BinId(n->parent->id) : model::BinId();
}

std::vector<model::MediaId> ProjectItemModel::decodeMediaIds(const QMimeData* data) {
    std::vector<model::MediaId> ids;
    if (data == nullptr || !data->hasFormat(QString::fromLatin1(kMimeType))) {
        return ids;
    }
    for (const auto& entry : QString::fromUtf8(data->data(QString::fromLatin1(kMimeType))).split(QLatin1Char('\n'))) {
        if (entry.startsWith(QStringLiteral("media:"))) {
            if (auto id = model::MediaId::parse(entry.mid(6).toStdString())) {
                ids.push_back(*id);
            }
        }
    }
    return ids;
}

} // namespace loki::ui
