#include "ui/media_services.h"

#include "commands/project_commands.h"
#include "export/proxy.h"

#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "media/thumbnail.h"
#include "model/project_json.h"
#include "ui/project_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <set>

namespace loki::ui {

namespace {

constexpr auto kCacheDirKey = "cache/directory";
constexpr int kThumbWidth = 320;
constexpr int kThumbHeight = 180;

std::filesystem::path toPath(const QString& s) {
    return std::filesystem::path(s.toStdWString());
}

QString fromPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

QString idKey(model::MediaId id) {
    return QString::fromStdString(id.toString());
}

bool hasImportableExtension(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto& list = media::importableExtensions();
    return std::find(list.begin(), list.end(), ext) != list.end();
}

} // namespace

QString defaultCacheDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("MediaCache"));
}

QString configuredCacheDirectory() {
    const QSettings settings;
    const auto value = settings.value(kCacheDirKey).toString();
    return value.isEmpty() ? defaultCacheDirectory() : value;
}

void setConfiguredCacheDirectory(const QString& directory) {
    QSettings settings;
    settings.setValue(kCacheDirKey, directory);
}

QImage toQImage(const media::VideoFrame& frame) {
    if (frame.format != media::PixelFormat::Rgba || frame.planes[0].empty()) {
        return {};
    }
    const QImage view(frame.planes[0].data(), frame.width, frame.height, frame.strides[0], QImage::Format_RGBA8888);
    return view.copy(); // detach from the frame's buffer
}

struct MediaServices::ImportJob {
    model::BinId bin;
    std::mutex mutex;
    std::vector<model::MediaItem> items;
    QStringList errors;
    std::atomic<int> remaining{0};
};

MediaServices::MediaServices(ProjectController& controller, const QString& cacheDirectory, QObject* parent)
    : QObject(parent), m_controller(controller),
      m_cache(std::make_unique<media::MediaCache>(toPath(cacheDirectory))),
      m_pool(std::make_unique<util::TaskPool>(util::TaskPool::defaultDecodeThreads(), "media")) {}

MediaServices::~MediaServices() {
    m_pool.reset(); // cancel pending work and join before the caches go away
}

QString MediaServices::cacheDirectory() const {
    return fromPath(m_cache->root());
}

void MediaServices::setCacheDirectory(const QString& directory) {
    m_cache = std::make_unique<media::MediaCache>(toPath(directory));
    m_thumbnails.clear();
    m_waveforms.clear();
}

QImage MediaServices::thumbnail(const model::MediaItem& item) {
    if (!item.info.hasVideo) {
        return {};
    }
    const auto key = idKey(item.id);
    if (const auto it = m_thumbnails.constFind(key); it != m_thumbnails.cend()) {
        return *it;
    }
    if (item.isGenerated() || m_pendingThumbnails.contains(key)) {
        return {}; // generated media (titles) have no file to thumbnail
    }
    m_pendingThumbnails.insert(key);
    const auto cacheRoot = m_cache->root();
    const auto path = item.path;
    const Rational at = item.info.isStill ? Rational() : min(Rational(1), item.info.duration / Rational(10));
    m_pool->submit(
        [this, key, cacheRoot, path, at](const util::CancelToken& cancel) {
            if (cancel.cancelled()) {
                return;
            }
            QImage image;
            const media::MediaCache cache(cacheRoot);
            const auto sourceKey = media::MediaCache::sourceKey(path);
            const auto file = sourceKey.empty() ? std::filesystem::path()
                                                : cache.entryPath("thumbnails", sourceKey, "_320.jpg");
            if (!file.empty() && std::filesystem::exists(file)) {
                image.load(fromPath(file));
            }
            if (image.isNull()) {
                try {
                    const auto frame = media::makeThumbnail(path, at, kThumbWidth, kThumbHeight);
                    image = toQImage(*frame);
                    if (!file.empty() && !image.isNull()) {
                        image.save(fromPath(file), "JPG", 85);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Thumbnail for {} failed: {}", model::pathToUtf8(path), e.what());
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, key, image] {
                    m_pendingThumbnails.remove(key);
                    m_thumbnails.insert(key, image); // null image = failed; do not retry every paint
                    emit thumbnailReady(key);
                },
                Qt::QueuedConnection);
        },
        util::TaskPriority::Background);
    return {};
}

void MediaServices::createProxies(const std::vector<model::MediaId>& ids) {
    for (const auto id : ids) {
        const auto* found = m_controller.project().findMedia(id);
        const auto key = idKey(id);
        if (found == nullptr || !exporter::canHaveProxy(*found) || m_pendingProxies.contains(key)) {
            continue;
        }
        m_pendingProxies.insert(key);
        const model::MediaItem item = *found;
        const auto output = exporter::proxyPathFor(m_cache->root(), id);
        m_pool->submit(
            [this, key, item, output](const util::CancelToken& cancel) {
                std::atomic<bool> stop{false};
                const auto result = exporter::makeProxy(item, output, &stop, [&](double p) {
                    if (cancel.cancelled()) {
                        stop = true;
                    }
                    QMetaObject::invokeMethod(this, [this, key, p] { emit proxyProgress(key, p); }, Qt::QueuedConnection);
                });
                QMetaObject::invokeMethod(
                    this,
                    [this, key, id = item.id, output, result] {
                        m_pendingProxies.remove(key);
                        if (result.ok && m_controller.project().findMedia(id) != nullptr) {
                            m_controller.execute(std::make_unique<commands::EditMediaCommand>(
                                id, "Create Proxy", [output](model::MediaItem& m) { m.proxyPath = output; }));
                        }
                        emit proxyFinished(key, result.ok, QString::fromStdString(result.error));
                    },
                    Qt::QueuedConnection);
            },
            util::TaskPriority::Background);
    }
}

bool MediaServices::isMakingProxy(model::MediaId id) const {
    return m_pendingProxies.contains(idKey(id));
}

std::shared_ptr<const media::Waveform> MediaServices::waveform(const model::MediaItem& item) {
    if (!item.info.hasAudio) {
        return nullptr;
    }
    const auto key = idKey(item.id);
    if (const auto it = m_waveforms.constFind(key); it != m_waveforms.cend()) {
        return *it;
    }
    if (item.isGenerated() || m_pendingWaveforms.contains(key)) {
        return nullptr;
    }
    m_pendingWaveforms.insert(key);
    const auto cacheRoot = m_cache->root();
    const auto path = item.path;
    m_pool->submit(
        [this, key, cacheRoot, path](const util::CancelToken& cancel) {
            std::shared_ptr<const media::Waveform> result;
            const media::MediaCache cache(cacheRoot);
            const auto sourceKey = media::MediaCache::sourceKey(path);
            const auto file = sourceKey.empty() ? std::filesystem::path() : cache.entryPath("waveforms", sourceKey, ".lkwf");
            try {
                if (!file.empty() && std::filesystem::exists(file)) {
                    result = std::make_shared<media::Waveform>(media::loadWaveform(file));
                }
            } catch (const std::exception& e) {
                spdlog::warn("Ignoring corrupt waveform cache {}: {}", model::pathToUtf8(file), e.what());
            }
            if (!result) {
                try {
                    if (auto wf = media::computeWaveform(path, 256, &cancel)) {
                        if (!file.empty()) {
                            media::saveWaveform(*wf, file);
                        }
                        result = std::make_shared<media::Waveform>(std::move(*wf));
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Waveform for {} failed: {}", model::pathToUtf8(path), e.what());
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, key, result] {
                    m_pendingWaveforms.remove(key);
                    m_waveforms.insert(key, result);
                    emit waveformReady(key);
                },
                Qt::QueuedConnection);
        },
        util::TaskPriority::Background);
    return nullptr;
}

void MediaServices::importPaths(const QStringList& paths, model::BinId bin) {
    if (paths.isEmpty()) {
        return;
    }
    ++m_importsRunning;
    auto job = std::make_shared<ImportJob>();
    job->bin = bin;

    std::set<std::filesystem::path> existing;
    for (const auto& m : m_controller.project().media()) {
        if (!m.isGenerated()) {
            existing.insert(m.path.lexically_normal());
        }
    }
    std::vector<std::filesystem::path> roots;
    for (const auto& p : paths) {
        roots.push_back(toPath(p));
    }

    // Folder enumeration is I/O too: do it on the pool, then fan out one probe per file.
    util::TaskPool* pool = m_pool.get();
    pool->submit(
        [this, pool, job, roots, existing](const util::CancelToken&) {
            std::vector<std::filesystem::path> files;
            for (const auto& root : roots) {
                std::error_code ec;
                if (std::filesystem::is_directory(root, ec)) {
                    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                        if (it->is_regular_file(ec) && hasImportableExtension(it->path())) {
                            files.push_back(it->path());
                        }
                    }
                } else {
                    files.push_back(root);
                }
            }
            std::sort(files.begin(), files.end());
            files.erase(std::unique(files.begin(), files.end()), files.end());
            std::erase_if(files, [&](const auto& f) {
                std::error_code ec;
                const auto normal = std::filesystem::absolute(f, ec).lexically_normal();
                if (existing.contains(normal)) {
                    std::lock_guard lock(job->mutex);
                    job->errors << QStringLiteral("%1: already in the project").arg(fromPath(f));
                    return true;
                }
                return false;
            });
            if (files.empty()) {
                QMetaObject::invokeMethod(this, [this, job] { finishImport(job); }, Qt::QueuedConnection);
                return;
            }
            job->remaining = static_cast<int>(files.size());
            for (const auto& file : files) {
                pool->submit(
                    [this, job, file](const util::CancelToken&) {
                        try {
                            model::MediaItem item;
                            item.id = model::MediaId::generate();
                            std::error_code ec;
                            item.path = std::filesystem::absolute(file, ec).lexically_normal();
                            item.name = model::pathToUtf8(file.stem());
                            item.bin = job->bin;
                            item.info = media::probeMedia(file);
                            std::lock_guard lock(job->mutex);
                            job->items.push_back(std::move(item));
                        } catch (const std::exception& e) {
                            std::lock_guard lock(job->mutex);
                            job->errors << QStringLiteral("%1: %2").arg(fromPath(file), QString::fromUtf8(e.what()));
                        }
                        if (--job->remaining == 0) {
                            QMetaObject::invokeMethod(this, [this, job] { finishImport(job); }, Qt::QueuedConnection);
                        }
                    },
                    util::TaskPriority::High);
            }
        },
        util::TaskPriority::High);
}

void MediaServices::finishImport(const std::shared_ptr<ImportJob>& job) {
    --m_importsRunning;
    std::vector<model::MediaItem> items;
    QStringList errors;
    {
        std::lock_guard lock(job->mutex);
        items = std::move(job->items);
        errors = job->errors;
    }
    // Stable order regardless of which probe finished first.
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    // The target bin may have been deleted meanwhile.
    const auto bin = m_controller.project().findBin(job->bin) != nullptr ? job->bin : model::BinId();
    std::vector<std::unique_ptr<commands::Command>> adds;
    for (auto& item : items) {
        item.bin = bin;
        adds.push_back(std::make_unique<commands::AddMediaCommand>(std::move(item)));
    }
    const int count = static_cast<int>(adds.size());
    if (count > 0) {
        const auto text = count == 1 ? adds.front()->text() : "Import " + std::to_string(count) + " Items";
        if (!m_controller.execute(std::make_unique<commands::MacroCommand>(text, std::move(adds)))) {
            errors << QStringLiteral("Import failed: the project rejected the new items");
        }
    }
    for (const auto& e : errors) {
        spdlog::warn("Import: {}", e.toStdString());
    }
    emit importFinished(count, errors);
}

void MediaServices::waitForIdle() {
    for (int i = 0; i < 1000; ++i) {
        m_pool->waitIdle();
        QCoreApplication::sendPostedEvents(this);
        if (m_importsRunning == 0 && m_pendingThumbnails.isEmpty() && m_pendingWaveforms.isEmpty() &&
            m_pool->pendingCount() == 0) {
            return;
        }
        QThread::msleep(1);
    }
}

} // namespace loki::ui
