#include <QApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QStringList>
#include <QSurfaceFormat>
#include "AppIcon.h"
#include "DeepDiveWidget.h"
#include "version.h"

#ifdef _WIN32
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {
bool probeOpenGLCoreVersion(int major, int minor) {
    QSurfaceFormat probeFmt;
    probeFmt.setVersion(major, minor);
    probeFmt.setProfile(QSurfaceFormat::CoreProfile);
    probeFmt.setRenderableType(QSurfaceFormat::OpenGL);

    QOpenGLContext ctx;
    ctx.setFormat(probeFmt);
    if (!ctx.create()) {
        return false;
    }

    QOffscreenSurface surface;
    surface.setFormat(ctx.format());
    surface.create();
    if (!surface.isValid()) {
        return false;
    }

    const bool ok = ctx.makeCurrent(&surface);
    const QSurfaceFormat actual = ctx.format();
    if (ok) {
        ctx.doneCurrent();
    }

    return ok && (actual.majorVersion() > major ||
                  (actual.majorVersion() == major && actual.minorVersion() >= minor));
}
} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_OPENGL", "desktop");
    qputenv("__NV_PRIME_RENDER_OFFLOAD", "1");
    qputenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
    qputenv("SHIM_MCCOMPAT", "0x800000001");

    QApplication app(argc, argv);
    app.setApplicationName("DeepDive");
    app.setApplicationVersion(DEEPDIVE_VERSION_STRING);
    app.setDesktopFileName("deepdive");

#if !defined(Q_OS_MACOS)
    const QIcon appIcon = AppIcon::createAppIcon();
    app.setWindowIcon(appIcon);
#endif

    const bool forceGl33 = app.arguments().contains("--gl33");
    int glMajor = 3;
    int glMinor = 3;

    if (!forceGl33) {
        if (probeOpenGLCoreVersion(4, 1)) {
            glMajor = 4;
            glMinor = 1;
        } else if (probeOpenGLCoreVersion(4, 0)) {
            glMajor = 4;
            glMinor = 0;
        }
    }

    QSurfaceFormat fmt;
    fmt.setVersion(glMajor, glMinor);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(1);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(8);
    QSurfaceFormat::setDefaultFormat(fmt);

    DeepDiveWidget window;
#if !defined(Q_OS_MACOS)
    window.setWindowIcon(appIcon);
#endif
    window.setWindowTitle(
        QString("Deep Dive v%1 — Procedural Underwater World (Qt6 / OpenGL %2.%3)")
            .arg(DEEPDIVE_VERSION_STRING)
            .arg(glMajor)
            .arg(glMinor)
        );
    window.resize(1440, 810);
    window.showMaximized();

    return app.exec();
}
