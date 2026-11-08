// GoogleTest entry point for tests that need a QApplication. Runs headless (offscreen QPA)
// unless QT_QPA_PLATFORM is already set by the environment.
#include <QApplication>
#include <QtGlobal>

#include <gtest/gtest.h>

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
