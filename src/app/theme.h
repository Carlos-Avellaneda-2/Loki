#pragma once

class QApplication;
class QPalette;

namespace loki::app {

// Premiere-like dark palette (Fusion style). Exposed separately for tests.
[[nodiscard]] QPalette darkPalette();

// Applies the Fusion style, the dark palette and the dark stylesheet to the application.
void applyDarkTheme(QApplication& app);

} // namespace loki::app
