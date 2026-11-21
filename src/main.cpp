#include <QApplication>
#include <QSurfaceFormat>
#include "GpuCapabilities.h"
#include "DeepDiveWidget.h"
#include "version.h"

#ifdef _WIN32
// Аппаратные флаги для драйверов NVIDIA Optimus и AMD PowerXpress:
// предписывают ОС и драйверу создавать OpenGL-контекст на дискретной видеокарте (RTX 5070)
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char* argv[]) {
    // Переменные окружения для выбора дискретного GPU до инициализации OpenGL-библиотек
    qputenv("QT_OPENGL", "desktop");
    qputenv("__NV_PRIME_RENDER_OFFLOAD", "1");
    qputenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
    qputenv("SHIM_MCCOMPAT", "0x800000001");

    // Опрос доступных видеоадаптеров и ранжирование по capabilities
    auto adapters = GpuUtils::enumerateAndSelectBestGpu();

    QApplication app(argc, argv);
    app.setApplicationName("DeepDive");
    app.setApplicationVersion(DEEPDIVE_VERSION_STRING);

    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(1);    // Включаем вертикальную синхронизацию (VSync ON)
    fmt.setDepthBufferSize(24);
    fmt.setSamples(8);         // Запрашиваем 8x MSAA
    QSurfaceFormat::setDefaultFormat(fmt);

    DeepDiveWidget window(adapters);
    window.setWindowTitle(
        QString("Deep Dive v%1 — Procedural Underwater World (Qt6 / OpenGL 3.3)")
            .arg(DEEPDIVE_VERSION_STRING)
        );
    window.resize(1440, 810);
    window.show();

    return app.exec();
}
