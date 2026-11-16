#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QTimer>
#include <QElapsedTimer>
#include <QSet>
#include <QMatrix4x4>
#include "GpuCapabilities.h"
#include "WorldGenerator.h"
#include "UnderwaterAudio.h"

class DeepDiveWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit DeepDiveWidget(std::vector<AdapterCapability> adapters = {}, QWidget* parent = nullptr);
    ~DeepDiveWidget() override;

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void queryOpenGLCapabilities();
    void createProceduralRockTexture();
    void createShadowMapBuffer();
    void resizePostProcessBuffers(int physW, int physH);
    void destroyPostProcessBuffers();
    void rebuildWorld(uint32_t seed);
    void spawnWhalePass(bool nearPlayer);
    void updateSimulation(float dt);
    QVector3D cameraForward() const;
    QVector3D cameraRight() const;

    std::vector<AdapterCapability> m_adapters;
    QString m_glRendererInfo;
    int m_activeSamples = 8;
    bool m_vsyncEnabled = true;
    bool m_postProcessEnabled = true;
    bool m_shadowsEnabled = true;

    WorldData m_world;
    uint32_t m_seed = 42u;
    int m_paletteIndex = 0;

    UnderwaterAudioEngine* m_audioEngine = nullptr;

    // Шейдеры
    QOpenGLShaderProgram m_shadowEnvShader;
    QOpenGLShaderProgram m_shadowFaunaShader;
    QOpenGLShaderProgram m_envShader;
    QOpenGLShaderProgram m_faunaShader;
    QOpenGLShaderProgram m_skyShader;
    QOpenGLShaderProgram m_rayShader;
    QOpenGLShaderProgram m_particleShader;
    QOpenGLShaderProgram m_scatterBlurShader;
    QOpenGLShaderProgram m_postProcessShader;

    GLuint m_rockTex = 0;
    GLuint m_envVAO = 0, m_envVBO = 0, m_envEBO = 0;
    GLuint m_fishVAO = 0, m_fishVBO = 0, m_fishEBO = 0, m_fishInstVBO = 0;
    GLuint m_mantaVAO = 0, m_mantaVBO = 0, m_mantaEBO = 0, m_mantaInstVBO = 0;
    GLuint m_whaleVAO = 0, m_whaleVBO = 0, m_whaleEBO = 0, m_whaleInstVBO = 0;
    GLuint m_skyVAO = 0, m_skyVBO = 0;
    GLuint m_rayVAO = 0, m_rayVBO = 0;
    GLuint m_particleVAO = 0, m_particleVBO = 0;

    // Буфер карты теней (Directional Shadow Map)
    static constexpr int SHADOW_MAP_RES = 2048;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowDepthTex = 0;

    // Буферы MSAA и Постпродакшена
    int m_fbWidth = 0;
    int m_fbHeight = 0;
    GLuint m_msaaFBO = 0;
    GLuint m_msaaColorRBO = 0;
    GLuint m_msaaDepthRBO = 0;

    GLuint m_resolveFBO = 0;
    GLuint m_sceneColorTex = 0;
    GLuint m_sceneDepthTex = 0;

    GLuint m_bloomFBO[2] = {0, 0};
    GLuint m_bloomTex[2] = {0, 0};

    // Навигация проплывающего кита
    QVector3D m_whaleStartPos{-85.0f, 11.5f, -35.0f};
    QVector3D m_whaleDir{0.92f, 0.0f, 0.39f};
    float m_whaleTravelDist = 0.0f;
    float m_whaleMaxDist = 180.0f;
    float m_whaleSpeed = 3.6f;
    float m_whaleCooldown = 0.0f;
    bool  m_whaleActive = true;
    bool  m_whaleSongTriggered = false;

    QVector3D m_camPos{0.0f, -8.0f, 18.0f};
    QVector3D m_camVel{0.0f, 0.0f, 0.0f};
    float m_yaw = 0.0f;
    float m_pitch = 0.08f;
    bool m_mouseLooking = false;
    QPoint m_lastMousePos;
    QSet<int> m_pressedKeys;

    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastFrameNs = 0;
    float m_time = 0.0f;
    float m_fps = 60.0f;
};
