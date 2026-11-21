#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QTimer>
#include <QElapsedTimer>
#include <QSet>
#include <QMatrix4x4>
#include <array>
#include <vector>
#include <random>
#include <utility>
#include "GpuCapabilities.h"
#include "GamepadInput.h"
#include "WorldGenerator.h"
#include "UnderwaterAudio.h"

struct FrustumPlanes {
    std::array<QVector4D, 6> planes; // xyz: нормаль внутрь, w: смещение

    void extractFromMatrix(const QMatrix4x4& m);
    bool intersectsSphere(const QVector3D& center, float radius) const;
};

struct FrameRenderStats {
    int visibleChunks = 0;
    int totalChunks = 0;
    int lodCounts[4] = {0, 0, 0, 0};
    int drawnTriangles = 0;
    int visibleInstProps = 0;
    int visibleFishLod0 = 0;
    int visibleFishLod1 = 0;
    int visibleMantaLod0 = 0;
    int visibleMantaLod1 = 0;
    int visibleAnglers = 0;
    bool playerColliding = false;
};

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
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct FaunaLodBuffers {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLuint instVbo = 0;
        GLsizei indexCount = 0;
        GLsizei maxInstances = 0;
    };

    struct PropLodBuffers {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLuint instVbo = 0;
        GLsizei indexCount = 0;
        GLsizei maxInstances = 0;
    };

    struct ChunkLodState {
        int terrainLod = 0;
        std::array<int, PROP_ARCHETYPE_COUNT> propLod{}; // 0=LOD0, 1=LOD1, 2=culled
        bool detailVisible = true;
    };

    void queryOpenGLCapabilities();
    void createProceduralRockTexture();
    void createShadowMapBuffer();
    void createWaterSurfaceMesh();
    void resizePostProcessBuffers(int physW, int physH);
    void destroyPostProcessBuffers();

    void setupFaunaLodBuffers(FaunaLodBuffers& buf,
                              const std::vector<FaunaVertex>& verts,
                              const std::vector<uint32_t>& inds,
                              size_t maxInstCount);
    void deleteFaunaLodBuffers(FaunaLodBuffers& buf);

    void setupPropLodBuffers(PropLodBuffers& buf,
                             const std::vector<EnvVertex>& verts,
                             const std::vector<uint32_t>& inds,
                             size_t initialMaxInst);
    void ensurePropInstCapacity(PropLodBuffers& buf, size_t needed);
    void deletePropLodBuffers(PropLodBuffers& buf);

    void gatherAndUploadInstProps(const std::vector<std::pair<int, float>>& chunks, bool forShadow);
    void drawInstPropBatches(bool forShadow, bool countTris);

    void setMouseCapture(bool enable);
    void rebuildWorld(uint32_t seed);
    void spawnWhalePass(bool nearPlayer);
    float sampleWhaleClearance(float x, float z) const;
    float measureWhaleFreeDist(float x, float z, float yaw, float maxDist) const;
    QVector3D pickWhaleOpenWaypoint(const QVector3D& nearAnchor, float minDist, float maxDist) const;
    void updateWhaleNavigation(float dt);
    void updateFaunaAgents(float dt);
    void updateAnglerAgents(float dt);
    void updateSimulation(float dt);
    void cullAndUploadFauna(const FrustumPlanes& frustum);
    QVector3D computeAnglerLureWorldPos(const FaunaAgent& a) const;

    QVector3D cameraForward() const;
    QVector3D cameraRight() const;

    std::vector<AdapterCapability> m_adapters;
    QString m_glRendererInfo;
    int m_activeSamples = 8;
    bool m_vsyncEnabled = true;
    bool m_postProcessEnabled = true;
    bool m_shadowsEnabled = true;
    bool m_wireframeMode = false;
    bool m_hudVisible = true;

    WorldData m_world;
    uint32_t m_seed = 42u;
    int m_paletteIndex = 0; // 0 = Tropical Reef (Vibrant) по умолчанию
    FrameRenderStats m_stats;

    std::vector<ChunkLodState> m_chunkLodStates;
    std::vector<uint8_t> m_fishLodState;
    std::vector<uint8_t> m_mantaLodState;
    std::vector<uint8_t> m_anglerLodState;
    uint8_t m_whaleLodState = 0;

    UnderwaterAudioEngine* m_audioEngine = nullptr;
    GamepadInput m_gamepad;
    bool m_gamepadConnected = false;
    bool m_gamepadIsXInput = false;
    bool m_wasPlayerColliding = false;

    QOpenGLShaderProgram m_shadowEnvShader;
    QOpenGLShaderProgram m_shadowInstPropShader;
    QOpenGLShaderProgram m_shadowFaunaShader;
    QOpenGLShaderProgram m_envShader;
    QOpenGLShaderProgram m_instPropShader;
    QOpenGLShaderProgram m_faunaShader;
    QOpenGLShaderProgram m_waterShader;
    QOpenGLShaderProgram m_skyShader;
    QOpenGLShaderProgram m_rayShader;
    QOpenGLShaderProgram m_particleShader;
    QOpenGLShaderProgram m_scatterBlurShader;
    QOpenGLShaderProgram m_postProcessShader;

    GLuint m_rockTex = 0;

    GLuint m_terrainVAO = 0, m_terrainVBO = 0, m_terrainEBO = 0;
    GLuint m_propVAO = 0, m_propVBO = 0, m_propEBO = 0;

    std::array<PropLodBuffers, PROP_ARCHETYPE_COUNT> m_propLod0{};
    std::array<PropLodBuffers, PROP_ARCHETYPE_COUNT> m_propLod1{};
    std::array<std::vector<PropGpuInstance>, PROP_ARCHETYPE_COUNT> m_visPropsLod0;
    std::array<std::vector<PropGpuInstance>, PROP_ARCHETYPE_COUNT> m_visPropsLod1;

    FaunaLodBuffers m_fishLod0, m_fishLod1;
    FaunaLodBuffers m_mantaLod0, m_mantaLod1;
    FaunaLodBuffers m_whaleLod0, m_whaleLod1;
    FaunaLodBuffers m_anglerLod0, m_anglerLod1;

    std::vector<FaunaGpuInstance> m_visFishLod0, m_visFishLod1;
    std::vector<FaunaGpuInstance> m_visMantaLod0, m_visMantaLod1;
    std::vector<FaunaGpuInstance> m_visWhaleLod0, m_visWhaleLod1;
    std::vector<FaunaGpuInstance> m_visAnglerLod0, m_visAnglerLod1;

    static constexpr int MAX_LURE_LIGHTS = 8;
    std::array<QVector3D, MAX_LURE_LIGHTS> m_lurePositions{};
    int m_lureCount = 0;

    GLuint m_waterVAO = 0, m_waterVBO = 0, m_waterEBO = 0;
    GLsizei m_waterIndexCount = 0;

    GLuint m_skyVAO = 0, m_skyVBO = 0;
    GLuint m_rayVAO = 0, m_rayVBO = 0, m_rayEBO = 0, m_rayInstVBO = 0;
    GLsizei m_rayIndexCount = 0;
    GLsizei m_rayInstanceCount = 0;
    GLuint m_particleVAO = 0, m_particleVBO = 0;

    static constexpr int SHADOW_MAP_RES = 2048;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowDepthTex = 0;

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

    enum class WhaleState {
        Cruising,
        AvoidingTurn
    };

    // Автономный горбатый кит
    std::mt19937 m_rng;
    FaunaAgent m_whale;
    QVector3D m_whaleWaypoint{0.0f, 20.5f, 0.0f};
    float m_whaleWaypointTimer = 0.0f;
    float m_whaleSongTimer = 7.0f;

    // Уверенный обход с фиксацией стороны разворота (без дрожания)
    WhaleState m_whaleState = WhaleState::Cruising;
    float m_whaleTurnDir = 1.0f;
    float m_whaleAvoidTimer = 0.0f;
    float m_whaleAvoidAccumYaw = 0.0f;
    float m_whaleCurrentYawRate = 0.0f;
    QVector3D m_whaleProgressAnchor{0.0f, 20.5f, 0.0f};
    float m_whaleProgressTimer = 0.0f;
    float m_whaleStuckAccum = 0.0f;

    // Периодическое всплытие (в воде остаётся только нижняя половина тела)
    bool  m_whaleSurfacing = false;
    float m_whaleSurfaceCooldown = 8.0f;
    float m_whaleSurfaceTimer = 0.0f;
    float m_whaleSurfaceBlend = 0.0f;

    QVector3D m_camPos{0.0f, -6.0f, 20.0f};
    QVector3D m_camVel{0.0f, 0.0f, 0.0f};
    float m_yaw = 0.0f;
    float m_pitch = 0.06f;

    // Свободный обзор мышью без зажатой ЛКМ
    bool m_mouseCaptured = false;
    bool m_userReleasedMouse = false;
    bool m_skipNextMouseDelta = false;
    QSet<int> m_pressedKeys;

    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastFrameNs = 0;
    float m_time = 0.0f;
    float m_fps = 60.0f;
};
