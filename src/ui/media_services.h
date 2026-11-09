#pragma once

#include "media/media_cache.h"
#include "media/video_frame.h"
#include "media/waveform.h"
#include "model/ids.h"
#include "model/types.h"
#include "util/task_pool.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <memory>

namespace loki::ui {

class ProjectController;

// Shared background services for the UI: worker pool, disk media cache, thumbnails, waveforms and
// media import. Everything slow runs on the pool; results come back as signals on the UI thread.
class MediaServices final : public QObject {
    Q_OBJECT

public:
    MediaServices(ProjectController& controller, const QString& cacheDirectory, QObject* parent = nullptr);
    ~MediaServices() override;

    MediaServices(const MediaServices&) = delete;
    MediaServices& operator=(const MediaServices&) = delete;
    MediaServices(MediaServices&&) = delete;
    MediaServices& operator=(MediaServices&&) = delete;

    [[nodiscard]] util::TaskPool& pool() noexcept { return *m_pool; }
    [[nodiscard]] QString cacheDirectory() const;
    void setCacheDirectory(const QString& directory);

    // Returns the cached thumbnail or a null image; in the latter case one is generated in the
    // background and thumbnailReady(mediaId) is emitted when available.
    [[nodiscard]] QImage thumbnail(const model::MediaItem& item);
    // Same pattern for waveforms (audio media only).
    [[nodiscard]] std::shared_ptr<const media::Waveform> waveform(const model::MediaItem& item);

    // Imports files and folders (recursively) into `bin`: probes in the background, then adds
    // everything as one undoable step. Files already in the project are skipped.
    void importPaths(const QStringList& paths, model::BinId bin);
    [[nodiscard]] bool isImporting() const noexcept { return m_importsRunning > 0; }

    // Creates 720p proxies (D24) in the background (cache folder/proxies). When one is ready the
    // media item's proxyPath is set through an undoable command.
    void createProxies(const std::vector<model::MediaId>& ids);
    [[nodiscard]] bool isMakingProxy(model::MediaId id) const;

    // Blocks until all background work finished and results were delivered (tests, shutdown).
    void waitForIdle();

signals:
    void thumbnailReady(const QString& mediaId);
    void waveformReady(const QString& mediaId);
    // imported: number of items added; errors: "<file>: <reason>" lines.
    void importFinished(int imported, const QStringList& errors);
    void proxyProgress(const QString& mediaId, double progress);
    void proxyFinished(const QString& mediaId, bool ok, const QString& error);

private:
    struct ImportJob;
    void finishImport(const std::shared_ptr<ImportJob>& job);

    ProjectController& m_controller;
    std::unique_ptr<media::MediaCache> m_cache;
    QHash<QString, QImage> m_thumbnails;
    QSet<QString> m_pendingThumbnails;
    QHash<QString, std::shared_ptr<const media::Waveform>> m_waveforms;
    QSet<QString> m_pendingWaveforms;
    int m_importsRunning = 0;
    QSet<QString> m_pendingProxies;
    std::unique_ptr<util::TaskPool> m_pool; // declared last: destroyed (joined) first
};

// Default media cache folder (%LOCALAPPDATA%/Loki/MediaCache) and the configured one.
[[nodiscard]] QString defaultCacheDirectory();
[[nodiscard]] QString configuredCacheDirectory();
void setConfiguredCacheDirectory(const QString& directory);

// QImage copy of an RGBA frame.
[[nodiscard]] QImage toQImage(const media::VideoFrame& frame);

} // namespace loki::ui
