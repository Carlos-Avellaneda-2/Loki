// loki_bench: measures the non-functional criteria on this machine and writes a Markdown report.
//   loki_bench [report.md] [--seconds N]
// Playback (real audio device = master clock, preview composited with the real GL compositor),
// A/V sync, scrub latency, export speed and a soak run for memory growth.
#include "export/export_job.h"
#include "export/proxy.h"
#include "media/media_probe.h"
#include "model/effect_registry.h"
#include "model/project.h"
#include "playback/playback_engine.h"
#include "render/gl_compositor.h"
#include "util/logging.h"

#include "media_generator.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>

namespace {

using namespace loki;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

double privateMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
}

model::MediaItem mediaItem(const std::filesystem::path& path) {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = path.stem().string();
    m.path = path;
    m.info = media::probeMedia(path);
    return m;
}

// `videoTracks` layers (the upper ones scaled and semi-transparent, like picture-in-picture) and
// `audioTracks` copies of the sound, all `seconds` long, 1080p30.
std::shared_ptr<playback::SequenceSnapshot> makeSequence(const model::MediaItem& media, int videoTracks,
                                                         int audioTracks, int seconds) {
    auto s = std::make_shared<playback::SequenceSnapshot>();
    s->sequence = model::makeDefaultSequence("Bench", {}, static_cast<std::size_t>(videoTracks),
                                             static_cast<std::size_t>(audioTracks));
    s->sequence.settings.width = 1920;
    s->sequence.settings.height = 1080;
    s->sequence.settings.frameRate = Rational(30);
    s->media[media.id] = media;
    for (int v = 0; v < videoTracks; ++v) {
        model::Clip c;
        c.id = model::ClipId::generate();
        c.media = media.id;
        c.duration = Rational(seconds);
        c.sourceIn = Rational(v, 3);
        if (v > 0) {
            auto motion = model::makeEffect(model::kMotionEffect, media.info, s->sequence.settings);
            for (auto& p : motion.parameters) {
                if (p.key == "scale") {
                    p.value = 45;
                } else if (p.key == "positionX") {
                    p.value = 480.0 * v;
                }
            }
            c.effects.push_back(motion);
            auto opacity = model::makeEffect(model::kOpacityEffect, media.info, s->sequence.settings);
            opacity.parameters[0].value = 80;
            c.effects.push_back(opacity);
        }
        s->sequence.videoTracks[static_cast<std::size_t>(v)].clips.push_back(c);
    }
    for (int a = 0; a < audioTracks; ++a) {
        model::Clip c;
        c.id = model::ClipId::generate();
        c.media = media.id;
        c.duration = Rational(seconds);
        c.sourceIn = Rational(a, 4);
        s->sequence.audioTracks[static_cast<std::size_t>(a)].clips.push_back(c);
    }
    s->generation = 1;
    return s;
}

struct PlaybackResult {
    std::string name;
    std::uint64_t shown = 0;
    std::uint64_t dropped = 0;
    std::uint64_t underruns = 0;
    double maxLagFrames = 0;
    double avgRenderMs = 0;
    std::string hwAccel;
    bool audioDevice = false;
};

PlaybackResult measurePlayback(const std::string& name, std::shared_ptr<playback::SequenceSnapshot> seq,
                               render::GlCompositor& compositor, int seconds, int maxWidth, int maxHeight) {
    PlaybackResult r;
    r.name = name;
    playback::PlaybackEngine engine;
    r.audioDevice = engine.hasAudioOutput();
    engine.setQuality(maxWidth, maxHeight);
    engine.openSequence(seq);
    engine.waitIdle(10000);
    engine.play();
    const auto start = Clock::now();
    const Rational frame(1, 30);
    double renderTotal = 0;
    int renders = 0;
    Rational lastTime(-1);
    while (msSince(start) < seconds * 1000.0) {
        std::this_thread::sleep_for(8ms);
        (void)engine.currentFrame();
        const auto picture = engine.currentPicture();
        if (picture && picture->composite && picture->time != lastTime) {
            lastTime = picture->time;
            const auto t0 = Clock::now();
            const int w = maxWidth > 0 ? maxWidth : 1920;
            compositor.render(*picture->composite, w, w * 9 / 16);
            QOpenGLContext::currentContext()->functions()->glFinish();
            renderTotal += msSince(t0);
            ++renders;
            // Picture shown vs the audio clock, in frames (should stay below 1).
            const double lag = std::abs(((engine.position() - picture->time) / frame).toDouble());
            if (msSince(start) > 500) { // ignore the first half second (prime)
                r.maxLagFrames = std::max(r.maxLagFrames, lag);
            }
        }
    }
    engine.pause();
    engine.waitIdle();
    const auto stats = engine.stats();
    r.shown = stats.framesShown;
    r.dropped = stats.framesDropped;
    r.underruns = stats.audioUnderruns;
    r.hwAccel = stats.hwAccel.empty() ? "software" : stats.hwAccel;
    r.avgRenderMs = renders > 0 ? renderTotal / renders : 0;
    return r;
}

struct ScrubResult {
    double avgMs = 0;
    double maxMs = 0;
    int samples = 0;
};

// random = jumps anywhere (worst case); otherwise small steps like dragging the playhead.
ScrubResult measureScrub(std::shared_ptr<playback::SequenceSnapshot> seq, int seconds, bool random) {
    ScrubResult r;
    playback::PlaybackEngine engine;
    engine.setQuality(960, 540); // the monitor size in a typical layout
    engine.openSequence(seq);
    engine.waitIdle(10000);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> pick(0, seconds * 30 - 1);
    // Warm-up: the first seek opens the decoders (hardware decoder start-up), like opening a project.
    engine.seek(Rational(1));
    engine.waitIdle(10000);
    (void)engine.currentFrame();
    double total = 0;
    int position = 45;
    for (int i = 0; i < 20; ++i) {
        position = random ? pick(rng) : std::clamp(position + ((i % 5 == 4) ? -4 : 3), 0, seconds * 30 - 1);
        const Rational target(position, 30);
        const auto t0 = Clock::now();
        engine.seek(target);
        while (msSince(t0) < 2000) {
            (void)engine.currentFrame();
            if (engine.currentFrameTime() == target) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        const double ms = msSince(t0);
        std::cout << "  seek " << (random ? "random" : "step") << (seq->useProxies ? " proxy" : " orig") << " frame "
                  << position << ": " << ms << " ms\n";
        total += ms;
        r.maxMs = std::max(r.maxMs, ms);
        ++r.samples;
    }
    r.avgMs = total / r.samples;
    return r;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    util::initLogging({});
    std::filesystem::path report = "PERFORMANCE.md";
    int seconds = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::max(3, std::atoi(argv[++i]));
        } else {
            report = arg;
        }
    }

    const auto dir = std::filesystem::temp_directory_path() / "loki_bench";
    std::filesystem::create_directories(dir);
    const auto file = dir / "bench_1080p30.mp4";
    if (!std::filesystem::exists(file)) {
        std::cout << "Generating 1080p30 test media..." << std::endl;
        test::VideoSpec spec;
        spec.width = 1920;
        spec.height = 1080;
        spec.frameRate = Rational(30);
        spec.frames = (seconds + 2) * 30;
        spec.gop = 30;
        spec.codec = "libopenh264";
        auto partial = file;
        partial.replace_extension(".part.mp4");
        test::generateVideo(partial, spec);
        std::filesystem::rename(partial, file); // an interrupted run never leaves a broken file
    }
    auto media = mediaItem(file);
    const auto proxyFile = dir / "bench_1080p30_proxy_gop5.mp4";
    if (!std::filesystem::exists(proxyFile)) {
        std::cout << "Creating the 720p proxy..." << std::endl;
        (void)exporter::makeProxy(media, proxyFile);
    }
    auto proxied = media;
    proxied.proxyPath = proxyFile;

    QOffscreenSurface surface;
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    surface.setFormat(format);
    surface.create();
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create() || !context.makeCurrent(&surface)) {
        std::cerr << "No OpenGL context\n";
        return 1;
    }
    render::GlCompositor compositor;
    compositor.initialize();

    std::cout << "Playback 2V+2A full..." << std::endl;
    const auto full22 = measurePlayback("2V+2A, 1080p30, Full", makeSequence(media, 2, 2, seconds), compositor, seconds, 0, 0);
    std::cout << "Playback 2V+2A 1/2..." << std::endl;
    const auto half22 = measurePlayback("2V+2A, 1080p30, 1/2", makeSequence(media, 2, 2, seconds), compositor, seconds, 960, 540);
    std::cout << "Playback 3V+4A 1/2..." << std::endl;
    const auto half34 = measurePlayback("3V+4A, 1080p30, 1/2", makeSequence(media, 3, 4, seconds), compositor, seconds, 960, 540);
    std::cout << "Scrubbing..." << std::endl;
    const auto scrubRandom = measureScrub(makeSequence(media, 2, 2, seconds), seconds, true);
    const auto scrubSteps = measureScrub(makeSequence(media, 2, 2, seconds), seconds, false);
    auto proxySeq = makeSequence(proxied, 2, 2, seconds);
    proxySeq->useProxies = true;
    const auto scrubProxyRandom = measureScrub(proxySeq, seconds, true);
    const auto scrubProxySteps = measureScrub(proxySeq, seconds, false);
    compositor.release();
    context.doneCurrent();

    std::cout << "Export..." << std::endl;
    exporter::ExportRequest request;
    request.snapshot = makeSequence(media, 2, 2, std::min(seconds, 5));
    request.out = Rational(std::min(seconds, 5));
    request.file = dir / "bench_export.mp4";
    request.preset = *exporter::findExportPreset("h264-1080p30");
    std::vector<std::unique_ptr<exporter::FrameRenderer>> renderers;
    renderers.push_back(exporter::makeGlRenderer(&surface));
    renderers.push_back(exporter::makeCpuRenderer());
    const auto exported = exporter::ExportJob(request, std::move(renderers)).run();

    std::cout << "Soak (memory)..." << std::endl;
    double memoryAt5 = 0;
    const auto soakSeq = makeSequence(media, 2, 2, 3);
    for (int i = 0; i < 30; ++i) {
        playback::PlaybackEngine engine;
        engine.openSequence(soakSeq);
        engine.play();
        std::this_thread::sleep_for(150ms);
        engine.seek(Rational(i % 3));
        engine.waitIdle();
        if (i == 4) {
            memoryAt5 = privateMb();
        }
    }
    const double memoryGrowth = privateMb() - memoryAt5;

    std::ostringstream md;
    const auto yesNo = [](bool ok) { return ok ? "OK" : "**NO**"; };
    md << "# Rendimiento medido (loki_bench)\n\n";
    md << "Máquina: i5-7200U (2C/4T), Intel HD 620 + GeForce 920MX. Medio: H.264 1080p30 sintético. "
       << "Duración de cada prueba: " << seconds << " s.\n\n";
    md << "## Reproducción (reloj de audio maestro, composición GL incluida)\n\n";
    md << "| Secuencia | Mostrados | Perdidos | Cortes de audio | Desfase A/V máx. (fotogramas) | Render GL medio | Cumple |\n";
    md << "|---|---|---|---|---|---|---|\n";
    for (const auto* r : {&full22, &half22, &half34}) {
        char lag[32];
        std::snprintf(lag, sizeof lag, "%.2f", r->maxLagFrames);
        char ren[32];
        std::snprintf(ren, sizeof ren, "%.1f ms", r->avgRenderMs);
        md << "| " << r->name << " | " << r->shown << " | " << r->dropped << " | " << r->underruns << " | " << lag
           << " | " << ren << " | " << yesNo(r->dropped == 0 && r->maxLagFrames < 1.0) << " |\n";
    }
    if (!full22.audioDevice) {
        md << "\nSin dispositivo de audio: el reloj fue el de pared.\n";
    }
    char buf[256];
    md << "\n## Scrubbing (calidad 1/2, 2V+2A, 20 saltos; objetivo < 100 ms)\n\n";
    md << "| Medios | Saltos | Medio | Máximo | Cumple |\n|---|---|---|---|---|\n";
    const std::tuple<const char*, const char*, const ScrubResult*> scrubs[] = {
        {"Originales (GOP 30)", "aleatorios", &scrubRandom},
        {"Originales (GOP 30)", "cortos (arrastre)", &scrubSteps},
        {"Proxies 720p (GOP 5)", "aleatorios", &scrubProxyRandom},
        {"Proxies 720p (GOP 5)", "cortos (arrastre)", &scrubProxySteps},
    };
    for (const auto& [kind, steps, sc] : scrubs) {
        std::snprintf(buf, sizeof buf, "| %s | %s | %.0f ms | %.0f ms | %s |\n", kind, steps, sc->avgMs, sc->maxMs,
                      yesNo(sc->maxMs < 100.0));
        md << buf;
    }
    const double exportFps = exported.seconds > 0 ? static_cast<double>(exported.frames) / exported.seconds : 0;
    std::snprintf(buf, sizeof buf,
                  "\n## Exportación\n\n2V+2A, %lld fotogramas 1080p30 a H.264: %.1f s (%.1f fps, %.2fx tiempo real), %s + %s.\n",
                  static_cast<long long>(exported.frames), exported.seconds, exportFps, exportFps / 30.0,
                  exported.renderer.c_str(), exported.videoEncoder.c_str());
    md << buf;
    std::snprintf(buf, sizeof buf,
                  "\n## Memoria (30 ciclos abrir/reproducir/saltar/cerrar)\n\nCrecimiento de memoria privada entre el ciclo 5 y el 30: %.1f MB (objetivo < 20 MB): %s\n",
                  memoryGrowth, yesNo(memoryGrowth < 20.0));
    md << buf;
    md << "\nUI congelada > 50 ms: la mide `ui::StallDetector` dentro de la app (aviso en el log). Fugas: tests con "
          "ASan en debug (la suite completa corre en el preset `windows-msvc-asan`).\n";

    std::ofstream(report) << md.str();
    std::cout << md.str();
    return 0;
}
