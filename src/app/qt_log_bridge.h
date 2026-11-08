#pragma once

namespace loki::app {

// Routes qDebug/qWarning/qCritical/qFatal output into spdlog.
void installQtLogBridge();

} // namespace loki::app
