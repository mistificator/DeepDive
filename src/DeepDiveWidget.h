#pragma once
#include <QColor>
#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QSet>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include "GamepadInput.h"
#include "GpuCapabilities.h"
#include "UnderwaterAudio.h"

class DeepDiveEngine;
class DeepDiveRender;
class QLabel;
class QProgressBar;

class DeepDiveWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit DeepDiveWidget(std::vector<AdapterCapability> adapters = {}, QWidget* parent = nullptr);
    ~DeepDiveWidget() override;

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct PendingWorldLoad;

    void applySystemColorSchemeBackground();
    void createHudOverlay();
    void updateHudGeometry();
    void setHudVisible(bool visible);
    void updateHudOverlay();
    void createLoadingOverlay();
    void setLoadingOverlayVisible(bool visible, const QString& text = {});
    void toggleFullscreen();
    void setMouseCapture(bool enable);
    void beginInitialStartup();
    void runNextInitStep();
    void startAsyncWorldLoad(uint32_t seed, bool generateRockTex, bool probeAdapters = false);
    void tryFinishLoading();

    std::unique_ptr<DeepDiveEngine> m_engine;
    std::unique_ptr<DeepDiveRender> m_render;
    std::vector<AdapterCapability> m_adapters;
    uint32_t m_seed = 42u;

    QColor m_windowBgColor{18, 22, 28};
    bool m_hudVisible = false;
    bool m_wasMaximizedBeforeFullscreen = false;

    // HUD overlay widget (composited by Qt without invoking QPainter inside paintGL)
    QLabel* m_hudLabel = nullptr;
    qint64 m_lastHudUpdateNs = 0;

    // Loading overlay (indeterminate progress bar) and background generation thread
    QWidget* m_loadingOverlay = nullptr;
    QLabel* m_loadingLabel = nullptr;
    QProgressBar* m_loadingBar = nullptr;
    std::thread m_loadThread;
    std::atomic<bool> m_destroying{false};
    bool m_isLoading = false;
    bool m_worldReady = false;
    bool m_glContextReady = false;
    bool m_startupScheduled = false;
    bool m_glInitComplete = false;
    int m_initStep = 0;
    std::unique_ptr<PendingWorldLoad> m_pendingLoad;

    // Application subsystems (audio & input)
    UnderwaterAudioEngine* m_audioEngine = nullptr;
    GamepadInput m_gamepad;
    bool m_gamepadConnected = false;
    bool m_gamepadIsXInput = false;

    // Mouse capture and keyboard state
    bool m_mouseCaptured = false;
    bool m_userReleasedMouse = false;
    bool m_skipNextMouseDelta = false;
    float m_pendingMouseYawDelta = 0.0f;
    float m_pendingMousePitchDelta = 0.0f;
    QSet<int> m_pressedKeys;

    // Main loop timing and FPS counter
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastFrameNs = 0;
    qint64 m_fpsWindowStartNs = 0;
    int m_fpsFrameCount = 0;
    float m_fps = 60.0f;
};
