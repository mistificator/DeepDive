#include "DeepDiveWidget.h"
#include "AppIcon.h"
#include "DeepDiveEngine.h"
#include "DeepDiveRender.h"
#include "WorldGenerator.h"
#include <QCursor>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPalette>
#include <QProgressBar>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyleHints>
#include <QtGlobal>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {

int mapSet1ScanCodeToQtKey(quint32 sc) {
    switch (sc) {
    case 0x29: return Qt::Key_QuoteLeft; // ` / ~ / ё
    case 0x0C: return Qt::Key_Minus;     // - / _
    case 0x0D: return Qt::Key_Equal;     // = / +
    case 0x11: return Qt::Key_W;
    case 0x13: return Qt::Key_R;
    case 0x14: return Qt::Key_T;
    case 0x19: return Qt::Key_P;
    case 0x1E: return Qt::Key_A;
    case 0x1F: return Qt::Key_S;
    case 0x20: return Qt::Key_D;
    case 0x21: return Qt::Key_F;
    case 0x25: return Qt::Key_K;
    case 0x26: return Qt::Key_L;
    case 0x2E: return Qt::Key_C;
    case 0x30: return Qt::Key_B;
    case 0x32: return Qt::Key_M;
    default:   return 0;
    }
}

int resolvePhysicalKey(const QKeyEvent* event) {
    const int k = event->key();

    // Pass through non-character special keys (Escape, Enter, Shift, Ctrl, Alt,
    // arrows, F-keys, media/volume keys) as-is, while allowing dead keys and
    // Qt::Key_unknown to be resolved by hardware scan code.
    const bool isDeadKey = (k >= 0x01001250 && k <= 0x0100126F);
    if (k == Qt::Key_Space ||
        (k >= Qt::Key_Escape && k != Qt::Key_unknown && !isDeadKey)) {
        return k;
    }

#if defined(Q_OS_WIN)
    // Hardware PS/2 Set 1 scan code is independent of the active keyboard layout
    if (const int mapped = mapSet1ScanCodeToQtKey(event->nativeScanCode())) {
        return mapped;
    }
    // Fallback for virtual/RDP keyboards that omit hardware scan codes
    const quint32 vk = event->nativeVirtualKey();
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<int>(vk); // Matches Qt::Key_A .. Qt::Key_Z
    }
    switch (vk) {
    case 0xC0: return Qt::Key_QuoteLeft; // VK_OEM_3 (` / ~ / ё)
    case 0xBB: return Qt::Key_Equal;     // VK_OEM_PLUS (= / +)
    case 0xBD: return Qt::Key_Minus;     // VK_OEM_MINUS (- / _)
    case 0x6B: return Qt::Key_Plus;      // VK_ADD (Numpad +)
    case 0x6D: return Qt::Key_Minus;     // VK_SUBTRACT (Numpad -)
    default:   break;
    }
#elif defined(Q_OS_LINUX)
    // X11 and Wayland (xkbcommon) report evdev + 8; evdev matches Set 1
    const quint32 sc = event->nativeScanCode();
    if (sc >= 8u) {
        if (const int mapped = mapSet1ScanCodeToQtKey(sc - 8u)) {
            return mapped;
        }
    }
#elif defined(Q_OS_MACOS)
    // Physical ANSI keycodes (Carbon/HIToolbox), layout-independent
    switch (event->nativeVirtualKey()) {
    case 0x32: return Qt::Key_QuoteLeft; // kVK_ANSI_Grave
    case 0x1B: return Qt::Key_Minus;     // kVK_ANSI_Minus
    case 0x18: return Qt::Key_Equal;     // kVK_ANSI_Equal
    case 0x0D: return Qt::Key_W;         // kVK_ANSI_W
    case 0x0F: return Qt::Key_R;         // kVK_ANSI_R
    case 0x11: return Qt::Key_T;         // kVK_ANSI_T
    case 0x23: return Qt::Key_P;         // kVK_ANSI_P
    case 0x00: return Qt::Key_A;         // kVK_ANSI_A
    case 0x01: return Qt::Key_S;         // kVK_ANSI_S
    case 0x02: return Qt::Key_D;         // kVK_ANSI_D
    case 0x03: return Qt::Key_F;         // kVK_ANSI_F
    case 0x28: return Qt::Key_K;         // kVK_ANSI_K
    case 0x25: return Qt::Key_L;         // kVK_ANSI_L
    case 0x08: return Qt::Key_C;         // kVK_ANSI_C
    case 0x0B: return Qt::Key_B;         // kVK_ANSI_B
    case 0x2E: return Qt::Key_M;         // kVK_ANSI_M
    default:   break;
    }
#endif

    return k;
}

} // namespace

struct DeepDiveWidget::PendingWorldLoad {
    std::vector<AdapterCapability> adapters;
    std::vector<uint8_t> rockRgba;
    WorldData world;
    uint32_t seed = 42u;
    bool generateRockTex = false;
};

DeepDiveWidget::DeepDiveWidget(std::vector<AdapterCapability> adapters, QWidget* parent)
    : QOpenGLWidget(parent),
    m_engine(std::make_unique<DeepDiveEngine>()),
    m_render(std::make_unique<DeepDiveRender>()),
    m_adapters(std::move(adapters)) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setMouseTracking(true);
    setMinimumSize(1280, 720);

    applySystemColorSchemeBackground();
    createHudOverlay();
    createLoadingOverlay();

    connect(&m_timer, &QTimer::timeout, this, QOverload<>::of(&DeepDiveWidget::update));
    m_clock.start();
}

DeepDiveWidget::~DeepDiveWidget() {
    m_destroying.store(true);
    m_timer.stop();
    if (m_loadThread.joinable()) {
        m_loadThread.join();
    }
    m_pendingLoad.reset();

    setMouseCapture(false);
    if (!m_glContextReady || !context()) {
        return;
    }

    makeCurrent();
    if (m_render) {
        m_render->cleanup();
    }
    doneCurrent();
}

void DeepDiveWidget::applySystemColorSchemeBackground() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    const bool isDark = (scheme == Qt::ColorScheme::Dark) ||
                        (scheme == Qt::ColorScheme::Unknown &&
                         palette().color(QPalette::Window).lightness() < 128);
#else
    const bool isDark = palette().color(QPalette::Window).lightness() < 128;
#endif

    m_windowBgColor = isDark ? QColor(18, 22, 28) : QColor(242, 244, 248);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, m_windowBgColor);
    pal.setColor(QPalette::Base, m_windowBgColor);
    setPalette(pal);
    setAutoFillBackground(true);
}

void DeepDiveWidget::createHudOverlay() {
    m_hudLabel = new QLabel(this);
    m_hudLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_hudLabel->setAttribute(Qt::WA_StyledBackground, true);
    m_hudLabel->setFocusPolicy(Qt::NoFocus);
    m_hudLabel->setTextFormat(Qt::PlainText);
    m_hudLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_hudLabel->setStyleSheet(
        "QLabel {"
        "  background-color: rgba(5, 22, 32, 165);"
        "  color: rgba(210, 245, 255, 230);"
        "  font-family: 'Consolas', monospace;"
        "  font-size: 10pt;"
        "  font-weight: bold;"
        "  padding: 4px 10px;"
        "}"
        );
    updateHudGeometry();
    m_hudLabel->hide();
}

void DeepDiveWidget::updateHudGeometry() {
    if (!m_hudLabel) return;
    const int hudW = std::max(600, width() - 28);
    m_hudLabel->setGeometry(14, 14, hudW, 68);
}

void DeepDiveWidget::setHudVisible(bool visible) {
    m_hudVisible = visible;
    if (!m_hudLabel) return;

    if (m_hudVisible && m_worldReady && !m_isLoading) {
        updateHudGeometry();
        m_lastHudUpdateNs = 0;
        updateHudOverlay();
        m_hudLabel->show();
        m_hudLabel->raise();
        if (m_loadingOverlay && m_loadingOverlay->isVisible()) {
            m_loadingOverlay->raise();
        }
    } else {
        m_hudLabel->hide();
    }
}

void DeepDiveWidget::updateHudOverlay() {
    if (!m_hudLabel) return;

    const QString audioStatus = (m_audioEngine && !m_audioEngine->isMuted())
                                    ? QString("ON (%1%)").arg(qRound(m_audioEngine->volume() * 100.0f))
                                    : QString("MUTED");
    const QString padStatus = m_gamepadConnected
                                  ? (m_gamepadIsXInput ? "XInput" : "HID")
                                  : "OFF";
    const char* tessStatus = !m_render->tessellationSupported()
                                 ? "N/A"
                                 : (m_render->tessellationEnabled() ? "ON" : "OFF");

    const QString depthOrAlt = m_engine->isPlayerAirborne()
                                   ? QString("Alt: +%1m").arg(qRound(std::max(0.0f, m_engine->surfaceAltitude())))
                                   : QString("Depth: %1m").arg(qRound(m_engine->diveDepth()));

    QString statusTag;
    if (m_engine->bulletTimeFactor() > 0.15f) {
        statusTag = QString("[BULLET-TIME %1x]").arg(m_engine->timeScale(), 0, 'f', 2);
    } else if (m_engine->isPlayerColliding()) {
        statusTag = "[COLLISION]";
    }

    const FrameRenderStats& st = m_render->stats();

    const QString hud = QString(
                            "GPU: %1 | GL %2.%3 | %4x MSAA | Tess: %5 | Shadows: %6 | PostFX: %7 | Audio: %8 | Pad: %9 | FPS: %10\n"
                            "Chunks: %11/%12 (LOD0:%13 LOD1:%14 LOD2:%15 LOD3:%16) | Props: %17 | Tris: %18k | Fish: %19+%20 | Mantas: %21+%22 | Anglers: %23 | Gulls: %24\n"
                            "Cam: %25 | %26 | R: %27m %28 | Seed: %29 | Biome: %30 | [C/L3] Cam [`] HUD [F/R3] Wire [T] Tess [B] Biome [K] Whale [Alt+Enter] FS"
                            )
                            .arg(m_render->glRendererInfo())
                            .arg(m_render->glMajorVersion())
                            .arg(m_render->glMinorVersion())
                            .arg(m_render->activeSamples())
                            .arg(tessStatus)
                            .arg(m_render->shadowsEnabled() ? "ON" : "OFF")
                            .arg(m_render->postProcessEnabled() ? "ON" : "OFF")
                            .arg(audioStatus)
                            .arg(padStatus)
                            .arg(qRound(m_fps))
                            .arg(st.visibleChunks)
                            .arg(st.totalChunks)
                            .arg(st.lodCounts[0])
                            .arg(st.lodCounts[1])
                            .arg(st.lodCounts[2])
                            .arg(st.lodCounts[3])
                            .arg(st.visibleInstProps)
                            .arg(st.drawnTriangles / 1000.0, 0, 'f', 1)
                            .arg(st.visibleFishLod0)
                            .arg(st.visibleFishLod1)
                            .arg(st.visibleMantaLod0)
                            .arg(st.visibleMantaLod1)
                            .arg(st.visibleAnglers)
                            .arg(st.visibleSeagulls)
                            .arg(m_engine->cameraModeName())
                            .arg(depthOrAlt)
                            .arg(qRound(m_engine->radialDistance()))
                            .arg(statusTag)
                            .arg(m_seed)
                            .arg(m_engine->biomeName());

    m_hudLabel->setText(hud);
}

void DeepDiveWidget::createLoadingOverlay() {
    m_loadingOverlay = new QWidget(this);
    m_loadingOverlay->setAttribute(Qt::WA_StyledBackground, true);
    m_loadingOverlay->setStyleSheet(
        "QWidget { background-color: rgb(4, 18, 28); }"
        "QLabel { color: rgb(205, 242, 255); font-family: 'Segoe UI'; background: transparent; }"
        "QProgressBar { border: 1px solid rgba(110, 215, 235, 110); border-radius: 6px; "
        "               background-color: rgba(8, 36, 52, 220); height: 14px; }"
        "QProgressBar::chunk { background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "                      stop:0 rgb(30, 175, 195), stop:1 rgb(115, 240, 225)); border-radius: 5px; }"
        );

    auto* layout = new QVBoxLayout(m_loadingOverlay);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(14);

    m_loadingIconLabel = new QLabel(m_loadingOverlay);
    m_loadingIconLabel->setAlignment(Qt::AlignCenter);
    m_loadingIconLabel->setFixedSize(112, 112);
    updateLoadingIconPixmap();

    auto* title = new QLabel("Deep Dive", m_loadingOverlay);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size: 26px; font-weight: 600; letter-spacing: 2px;");

    m_loadingLabel = new QLabel("Preparing underwater world...", m_loadingOverlay);
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    m_loadingLabel->setStyleSheet("font-size: 13px; color: rgba(190, 235, 248, 200);");

    m_loadingBar = new QProgressBar(m_loadingOverlay);
    m_loadingBar->setRange(0, 0);
    m_loadingBar->setTextVisible(false);
    m_loadingBar->setFixedWidth(360);

    layout->addWidget(m_loadingIconLabel, 0, Qt::AlignCenter);
    layout->addWidget(title);
    layout->addWidget(m_loadingLabel);
    layout->addWidget(m_loadingBar, 0, Qt::AlignCenter);

    m_loadingOverlay->setGeometry(rect());
    m_loadingOverlay->show();
    m_loadingOverlay->raise();
}

void DeepDiveWidget::updateLoadingIconPixmap() {
    if (!m_loadingIconLabel) return;
    m_loadingIconLabel->setPixmap(AppIcon::createPixmap(112, devicePixelRatioF()));
}

void DeepDiveWidget::setLoadingOverlayVisible(bool visible, const QString& text) {
    if (!m_loadingOverlay) return;
    if (!text.isEmpty() && m_loadingLabel) {
        m_loadingLabel->setText(text);
    }
    m_loadingOverlay->setGeometry(rect());
    m_loadingOverlay->setVisible(visible);
    if (visible) {
        updateLoadingIconPixmap();
        m_loadingOverlay->raise();
    }
}

void DeepDiveWidget::toggleFullscreen() {
    QWidget* win = window();
    if (!win) return;

    const bool wasCaptured = m_mouseCaptured;
    if (wasCaptured) {
        m_skipNextMouseDelta = true;
    }

    if (win->isFullScreen()) {
        if (m_wasMaximizedBeforeFullscreen) {
            win->showMaximized();
        } else {
            win->showNormal();
        }
    } else {
        m_wasMaximizedBeforeFullscreen = win->isMaximized();
        win->showFullScreen();
    }

    if (wasCaptured) {
        QTimer::singleShot(0, this, [this]() {
            if (m_mouseCaptured && isActiveWindow()) {
                m_skipNextMouseDelta = true;
                QCursor::setPos(mapToGlobal(QPoint(width() / 2, height() / 2)));
            }
        });
    }
}

void DeepDiveWidget::setMouseCapture(bool enable) {
    if (enable) {
        if (!isVisible() || width() < 4 || height() < 4) return;
        m_mouseCaptured = true;
        m_skipNextMouseDelta = true;
        setCursor(Qt::BlankCursor);
        QCursor::setPos(mapToGlobal(QPoint(width() / 2, height() / 2)));
    } else {
        m_mouseCaptured = false;
        m_skipNextMouseDelta = false;
        unsetCursor();
    }
}

void DeepDiveWidget::initializeGL() {
    m_render->initializeGLContext();
    m_glContextReady = true;
    setAutoFillBackground(false);

    // Release the first frame so the window and progress bar appear immediately,
    // then run stepped shader compilation and background world generation.
    if (!m_startupScheduled) {
        m_startupScheduled = true;
        QTimer::singleShot(30, this, &DeepDiveWidget::beginInitialStartup);
    }
}

void DeepDiveWidget::beginInitialStartup() {
    if (m_destroying.load()) return;

    startAsyncWorldLoad(m_seed, true, m_adapters.empty());

    m_initStep = 0;
    QTimer::singleShot(0, this, &DeepDiveWidget::runNextInitStep);
}

void DeepDiveWidget::runNextInitStep() {
    if (m_destroying.load() || !m_glContextReady) return;

    auto setStatus = [this](const QString& text) {
        if (m_loadingLabel && m_isLoading) {
            m_loadingLabel->setText(text);
        }
    };

    switch (m_initStep) {
    case 0:
        setStatus("Initializing OpenGL...");
        makeCurrent();
        m_render->initBaseResources(m_adapters);
        doneCurrent();
        break;

    case 1:
        setStatus("Compiling shadow shaders...");
        makeCurrent();
        m_render->compileShadowShaders();
        doneCurrent();
        break;

    case 2:
        setStatus("Compiling environment shaders...");
        makeCurrent();
        m_render->compileEnvironmentShaders();
        doneCurrent();
        break;

    case 3:
        setStatus("Compiling water and lighting shaders...");
        makeCurrent();
        m_render->compileWaterAndLightShaders();
        doneCurrent();
        break;

    case 4:
        setStatus("Compiling post-processing shaders...");
        makeCurrent();
        m_render->compilePostProcessShaders();
        doneCurrent();
        break;

    case 5: {
        setStatus("Preparing render buffers...");
        makeCurrent();
        const qreal dpr = devicePixelRatioF();
        m_render->initRenderBuffers(int(width() * dpr), int(height() * dpr));
        doneCurrent();
        break;
    }

    case 6:
        setStatus("Initializing audio system...");
        if (!m_audioEngine) {
            m_audioEngine = new UnderwaterAudioEngine(this);
        }
        break;

    case 7:
        m_glInitComplete = true;
        if (!m_pendingLoad) {
            setStatus("Generating underwater world and textures...");
        }
        tryFinishLoading();
        return;
    }

    ++m_initStep;
    QTimer::singleShot(10, this, &DeepDiveWidget::runNextInitStep);
}

void DeepDiveWidget::startAsyncWorldLoad(uint32_t seed, bool generateRockTex, bool probeAdapters) {
    if (m_isLoading) return;
    if (m_loadThread.joinable()) {
        m_loadThread.join();
    }

    m_isLoading = true;
    m_pendingLoad.reset();
    m_timer.stop();
    m_pressedKeys.clear();
    m_pendingMouseYawDelta = 0.0f;
    m_pendingMousePitchDelta = 0.0f;
    setMouseCapture(false);
    if (m_hudLabel) {
        m_hudLabel->hide();
    }
    setLoadingOverlayVisible(true, generateRockTex
                                       ? "Generating underwater world and textures..."
                                       : "Regenerating underwater world...");

    m_loadThread = std::thread([this, seed, generateRockTex, probeAdapters]() {
        auto payload = std::make_shared<PendingWorldLoad>();
        payload->seed = seed;
        payload->generateRockTex = generateRockTex;

        if (probeAdapters && !m_destroying.load()) {
            payload->adapters = GpuUtils::enumerateAndSelectBestGpu();
        }
        if (m_destroying.load()) return;

        if (generateRockTex && !m_destroying.load()) {
            payload->rockRgba = DeepDiveRender::generateRockTextureData(1024);
        }
        if (m_destroying.load()) return;

        payload->world = WorldGenerator::generate(seed);
        if (m_destroying.load()) return;

        QMetaObject::invokeMethod(this, [this, payload]() {
            if (m_destroying.load()) return;
            m_pendingLoad = std::make_unique<PendingWorldLoad>(std::move(*payload));
            tryFinishLoading();
        }, Qt::QueuedConnection);
    });
}

void DeepDiveWidget::tryFinishLoading() {
    if (m_destroying.load() || !m_glInitComplete || !m_pendingLoad) {
        return;
    }

    auto pending = std::move(m_pendingLoad);
    if (!pending->adapters.empty()) {
        m_adapters = std::move(pending->adapters);
    }

    m_engine->setWorld(std::move(pending->world), pending->seed);

    makeCurrent();
    if (!m_adapters.empty()) {
        m_render->refreshGpuCapabilities(m_adapters);
    }
    if (pending->generateRockTex && !pending->rockRgba.empty()) {
        m_render->uploadRockTexture(pending->rockRgba, 1024);
    }
    m_render->uploadWorld(m_engine->world());
    doneCurrent();

    m_worldReady = true;
    m_isLoading = false;
    m_lastFrameNs = 0;
    m_fpsWindowStartNs = 0;
    m_fpsFrameCount = 0;
    m_lastHudUpdateNs = 0;

    setLoadingOverlayVisible(false);
    if (m_hudVisible) {
        setHudVisible(true);
    }
    m_timer.start(0);
    if (!m_userReleasedMouse && isActiveWindow()) {
        setMouseCapture(true);
    }
    update();
}

void DeepDiveWidget::resizeGL(int w, int h) {
    if (!m_glInitComplete) return;
    const qreal dpr = devicePixelRatioF();
    m_render->resize(int(w * dpr), int(h * dpr));
}

void DeepDiveWidget::showEvent(QShowEvent* event) {
    QOpenGLWidget::showEvent(event);
    updateHudGeometry();
    if (m_loadingOverlay) {
        m_loadingOverlay->setGeometry(rect());
        if (m_loadingOverlay->isVisible()) {
            updateLoadingIconPixmap();
            m_loadingOverlay->raise();
        }
    }
}

void DeepDiveWidget::resizeEvent(QResizeEvent* event) {
    QOpenGLWidget::resizeEvent(event);
    updateHudGeometry();
    if (m_loadingOverlay) {
        m_loadingOverlay->setGeometry(rect());
    }
}

void DeepDiveWidget::paintGL() {
    if (!m_worldReady || m_isLoading) {
        if (m_glContextReady) {
            m_render->clearScreen(static_cast<float>(m_windowBgColor.redF()),
                                  static_cast<float>(m_windowBgColor.greenF()),
                                  static_cast<float>(m_windowBgColor.blueF()));
        }
        return;
    }

    const qint64 nowNs = m_clock.nsecsElapsed();
    const float rawDt = (m_lastFrameNs == 0) ? 0.016f : (nowNs - m_lastFrameNs) * 1e-9f;
    m_lastFrameNs = nowNs;

    if (m_fpsWindowStartNs == 0) {
        m_fpsWindowStartNs = nowNs;
    }
    ++m_fpsFrameCount;
    const double elapsedSec = (nowNs - m_fpsWindowStartNs) * 1e-9;
    if (elapsedSec >= 0.5) {
        m_fps = static_cast<float>(m_fpsFrameCount / elapsedSec);
        m_fpsFrameCount = 0;
        m_fpsWindowStartNs = nowNs;
    }

    const float dt = std::min(rawDt, 0.05f);

    // Poll gamepad and dispatch discrete controller actions
    const GamepadState gp = m_gamepad.poll(dt);
    m_gamepadConnected = gp.connected;
    m_gamepadIsXInput = gp.isXInput;

    if (gp.connected) {
        if (gp.cycleCamera)     { m_engine->cycleCameraMode();   m_lastHudUpdateNs = 0; }
        if (gp.toggleBiome)     { m_engine->cycleBiome();        m_lastHudUpdateNs = 0; }
        if (gp.togglePostFx)    { m_render->togglePostProcess(); m_lastHudUpdateNs = 0; }
        if (gp.toggleShadows)   { m_render->toggleShadows();     m_lastHudUpdateNs = 0; }
        if (gp.toggleHud)       { setHudVisible(!m_hudVisible); }
        if (gp.toggleWireframe) { m_render->toggleWireframe();   m_lastHudUpdateNs = 0; }
        if (gp.toggleMute && m_audioEngine) { m_audioEngine->toggleMute();         m_lastHudUpdateNs = 0; }
        if (gp.volumeUp   && m_audioEngine) { m_audioEngine->adjustVolume(0.05f);  m_lastHudUpdateNs = 0; }
        if (gp.volumeDown && m_audioEngine) { m_audioEngine->adjustVolume(-0.05f); m_lastHudUpdateNs = 0; }
        if (gp.summonWhale) {
            m_engine->summonWhaleNearPlayer();
            if (m_audioEngine) m_audioEngine->triggerWhaleCall();
            m_gamepad.triggerRumble(0.42f, 0.14f, 0.35f);
        }
        if (gp.regenWorld) {
            m_seed += 17u;
            startAsyncWorldLoad(m_seed, false);
            m_render->clearScreen(static_cast<float>(m_windowBgColor.redF()),
                                  static_cast<float>(m_windowBgColor.greenF()),
                                  static_cast<float>(m_windowBgColor.blueF()));
            return;
        }
    }

    // Gather continuous movement and look inputs for the simulation engine
    EngineInput input;
    input.yawDelta = m_pendingMouseYawDelta;
    input.pitchDelta = m_pendingMousePitchDelta;
    m_pendingMouseYawDelta = 0.0f;
    m_pendingMousePitchDelta = 0.0f;

    if (m_pressedKeys.contains(Qt::Key_W))       input.moveForward += 1.0f;
    if (m_pressedKeys.contains(Qt::Key_S))       input.moveForward -= 1.0f;
    if (m_pressedKeys.contains(Qt::Key_D))       input.moveRight   += 1.0f;
    if (m_pressedKeys.contains(Qt::Key_A))       input.moveRight   -= 1.0f;
    if (m_pressedKeys.contains(Qt::Key_Space))   input.moveUp      += 1.0f;
    if (m_pressedKeys.contains(Qt::Key_Control)) input.moveUp      -= 1.0f;
    input.sprint = m_pressedKeys.contains(Qt::Key_Shift);

    if (gp.connected) {
        input.yawDelta    += gp.lookX * 2.15f * dt;
        input.pitchDelta  += gp.lookY * 1.65f * dt;
        input.moveForward += gp.moveY;
        input.moveRight   += gp.moveX;
        input.moveUp      += gp.vertical;
        input.sprint       = input.sprint || gp.sprint;
    }

    // Step underwater & airborne simulation
    const EngineStepEvents events = m_engine->update(dt, input);

    // Tactile rumble feedback for collisions and water-surface breaching/splashdown
    if (gp.connected) {
        if (events.breachedSurface) {
            const float s = std::clamp(events.splashIntensity, 0.45f, 1.0f);
            m_gamepad.triggerRumble(0.28f * s, 0.58f * s, 0.18f);
        } else if (events.splashedDown) {
            const float s = std::clamp(events.splashIntensity, 0.40f, 1.0f);
            m_gamepad.triggerRumble(0.64f * s, 0.30f * s, 0.22f);
        } else if (events.playerCollisionImpact && events.collisionImpactSpeed > 1.8f) {
            const float intensity = std::clamp(events.collisionImpactSpeed / 16.0f, 0.15f, 0.60f);
            m_gamepad.triggerRumble(intensity, intensity * 0.45f, 0.14f);
        }
    }

    // Dual-medium audio state (camera-driven so 3rd-person water crossings feel exact)
    if (m_audioEngine) {
        m_audioEngine->updateState(m_engine->cameraPosition().y(),
                                   m_engine->cameraSpeed(),
                                   m_engine->audioBiomeIndex(),
                                   m_engine->isCameraAboveWater(),
                                   m_engine->timeScale());

        if (events.breachedSurface) {
            m_audioEngine->triggerSplash(true, events.splashIntensity);
        } else if (events.splashedDown) {
            m_audioEngine->triggerSplash(false, events.splashIntensity);
        } else if (events.cameraCrossedSurface) {
            m_audioEngine->triggerSplash(events.cameraEnteredAir, events.splashIntensity * 0.65f);
        }

        if (events.whaleCalled) {
            m_audioEngine->triggerWhaleCall();
        }
    }

    // Render current engine state
    const qreal dpr = devicePixelRatioF();
    const int physW = std::max(64, int(width() * dpr));
    const int physH = std::max(64, int(height() * dpr));
    m_render->render(*m_engine, physW, physH, defaultFramebufferObject());

    if (m_hudVisible && m_hudLabel) {
        if (!m_hudLabel->isVisible()) {
            updateHudGeometry();
            m_hudLabel->show();
            m_lastHudUpdateNs = 0;
        }
        if (m_lastHudUpdateNs == 0 || (nowNs - m_lastHudUpdateNs) >= 100'000'000LL) {
            m_lastHudUpdateNs = nowNs;
            updateHudOverlay();
        }
    }
}

void DeepDiveWidget::keyPressEvent(QKeyEvent* event) {
    const int key = resolvePhysicalKey(event);

    if ((event->modifiers() & Qt::AltModifier) &&
        (key == Qt::Key_Return || key == Qt::Key_Enter)) {
        if (!event->isAutoRepeat()) {
            toggleFullscreen();
        }
        event->accept();
        return;
    }

    if (key == Qt::Key_Escape) {
        if (!event->isAutoRepeat()) {
            if (m_mouseCaptured) {
                m_userReleasedMouse = true;
                setMouseCapture(false);
            } else {
                close();
            }
        }
        event->accept();
        return;
    }

    if (!m_worldReady || m_isLoading) {
        return;
    }

    m_pressedKeys.insert(key);

    if (key == Qt::Key_Plus || key == Qt::Key_Equal) {
        if (m_audioEngine) {
            m_audioEngine->adjustVolume(0.05f);
            m_lastHudUpdateNs = 0;
        }
        event->accept();
        return;
    }
    if (key == Qt::Key_Minus) {
        if (m_audioEngine) {
            m_audioEngine->adjustVolume(-0.05f);
            m_lastHudUpdateNs = 0;
        }
        event->accept();
        return;
    }

    if (event->isAutoRepeat()) {
        return;
    }

    const QString txt = event->text();
    const bool isHudToggle = (key == Qt::Key_QuoteLeft ||
                              key == Qt::Key_AsciiTilde ||
                              txt == QLatin1String("`") ||
                              txt == QLatin1String("~") ||
                              (!txt.isEmpty() && (txt[0] == QChar(0x0451) || txt[0] == QChar(0x0401))));
    if (isHudToggle) {
        setHudVisible(!m_hudVisible);
        return;
    }

    if (key == Qt::Key_C) {
        m_engine->cycleCameraMode();
        m_lastHudUpdateNs = 0;
    } else if (key == Qt::Key_B) {
        m_engine->cycleBiome();
        m_lastHudUpdateNs = 0;
    } else if (key == Qt::Key_R) {
        m_seed += 17u;
        startAsyncWorldLoad(m_seed, false);
    } else if (key == Qt::Key_P) {
        m_render->togglePostProcess();
        m_lastHudUpdateNs = 0;
    } else if (key == Qt::Key_L) {
        m_render->toggleShadows();
        m_lastHudUpdateNs = 0;
    } else if (key == Qt::Key_F) {
        m_render->toggleWireframe();
        m_lastHudUpdateNs = 0;
    } else if (key == Qt::Key_T) {
        m_render->toggleTessellation();
        m_lastHudUpdateNs = 0;
    } else if (key == Qt::Key_M) {
        if (m_audioEngine) {
            m_audioEngine->toggleMute();
            m_lastHudUpdateNs = 0;
        }
    } else if (key == Qt::Key_K) {
        m_engine->summonWhaleNearPlayer();
        if (m_audioEngine) m_audioEngine->triggerWhaleCall();
    }
}

void DeepDiveWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }
    m_pressedKeys.remove(resolvePhysicalKey(event));
}

void DeepDiveWidget::mousePressEvent(QMouseEvent* event) {
    if (!m_worldReady || m_isLoading) return;
    if (event->button() == Qt::LeftButton && !m_mouseCaptured) {
        m_userReleasedMouse = false;
        setMouseCapture(true);
    }
}

void DeepDiveWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_worldReady || m_isLoading) return;
    if (!m_mouseCaptured) {
        if (!m_userReleasedMouse && isActiveWindow()) {
            setMouseCapture(true);
        }
        return;
    }

    const QPoint center(width() / 2, height() / 2);
    const QPoint delta = event->pos() - center;

    if (m_skipNextMouseDelta) {
        m_skipNextMouseDelta = false;
        if (!delta.isNull()) {
            QCursor::setPos(mapToGlobal(center));
        }
        return;
    }

    if (delta.isNull()) return;

    m_pendingMouseYawDelta += delta.x() * 0.0028f;
    m_pendingMousePitchDelta -= delta.y() * 0.0028f;

    QCursor::setPos(mapToGlobal(center));
}

void DeepDiveWidget::mouseReleaseEvent(QMouseEvent* event) {
    QOpenGLWidget::mouseReleaseEvent(event);
}

void DeepDiveWidget::focusInEvent(QFocusEvent* event) {
    QOpenGLWidget::focusInEvent(event);
    if (m_worldReady && !m_isLoading && !m_userReleasedMouse) {
        setMouseCapture(true);
    }
}

void DeepDiveWidget::focusOutEvent(QFocusEvent* event) {
    QOpenGLWidget::focusOutEvent(event);
    m_pressedKeys.clear();
    m_pendingMouseYawDelta = 0.0f;
    m_pendingMousePitchDelta = 0.0f;
    setMouseCapture(false);
}
