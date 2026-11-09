#pragma once

#include <array>
#include <string_view>

namespace loki::ui {

// Identifies every dockable panel. The object name is persisted in workspace layouts,
// so existing values must never be renamed.
enum class PanelId {
    Project,
    SourceMonitor,
    ProgramMonitor,
    Timeline,
    EffectControls,
    AudioMeters,
    AudioMixer,
};

inline constexpr std::array kAllPanels = {
    PanelId::Project,        PanelId::SourceMonitor,  PanelId::ProgramMonitor,
    PanelId::Timeline,       PanelId::EffectControls, PanelId::AudioMeters,
    PanelId::AudioMixer,
};

// Stable, non-translatable identifier used as QObject::objectName and in saved layouts.
constexpr std::string_view panelObjectName(PanelId id) noexcept {
    switch (id) {
    case PanelId::Project: return "panel.project";
    case PanelId::SourceMonitor: return "panel.sourceMonitor";
    case PanelId::ProgramMonitor: return "panel.programMonitor";
    case PanelId::Timeline: return "panel.timeline";
    case PanelId::EffectControls: return "panel.effectControls";
    case PanelId::AudioMeters: return "panel.audioMeters";
    case PanelId::AudioMixer: return "panel.audioMixer";
    }
    return "panel.unknown";
}

// User-visible English title (translations come through Qt's tr() at the UI layer).
constexpr std::string_view panelTitle(PanelId id) noexcept {
    switch (id) {
    case PanelId::Project: return "Project";
    case PanelId::SourceMonitor: return "Source";
    case PanelId::ProgramMonitor: return "Program";
    case PanelId::Timeline: return "Timeline";
    case PanelId::EffectControls: return "Effect Controls";
    case PanelId::AudioMeters: return "Audio Meters";
    case PanelId::AudioMixer: return "Audio Track Mixer";
    }
    return "Unknown";
}

} // namespace loki::ui
