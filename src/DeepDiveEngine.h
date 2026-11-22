#pragma once
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QString>
#include <QVector3D>
#include <QVector4D>
#include <array>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>
#include "GpuCapabilities.h"
#include "WorldGenerator.h"

struct FrustumPlanes {
    std::array<QVector4D, 6> planes; // xyz: inward normal, w: plane offset

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

struct EngineInput {
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
    float moveForward = 0.0f;
    float moveRight = 0.0f;
    float moveUp = 0.0f;
    bool sprint = false;
};

struct EngineStepEvents {
    bool playerCollisionImpact = false;
    float collisionImpactSpeed = 0.0f;
    bool whaleCalled = false;
};

class DeepDiveEngine : protected QOpenGLFunctions_3_3_Core {
public:
    DeepDiveEngine();
    ~DeepDiveEngine() = default;

    // Lifecycle & stepped GPU initialization (caller ensures current GL context)
    void initializeGLContext();
    void initBaseResources(const std::vector<AdapterCapability>& adapters);
    void compileShadowShaders();
    void compileEnvironmentShaders();
    void compileWaterAndLightShaders();
    void compilePostProcessShaders();
    void initRenderBuffers(int physW, int physH);
    void refreshGpuCapabilities(const std::vector<AdapterCapability>& adapters);
    void cleanup();

    // Procedural asset generation & GPU upload
    static std::vector<uint8_t> generateRockTextureData(int size);
    void uploadRockTexture(const std::vector<uint8_t>& rgba, int size);
    void uploadWorld(WorldData&& world, uint32_t seed);

    // Frame execution
    void resize(int physW, int physH);
    void clearScreen(float r, float g, float b);
    EngineStepEvents update(float dt, const EngineInput& input);
    void render(int physW, int physH, GLuint targetFramebuffer);

    // Runtime world & rendering toggles
    void cycleBiome();
    void togglePostProcess();
    void toggleShadows();
    void toggleWireframe();
    void summonWhaleNearPlayer();

    // State queries for HUD & audio
    const FrameRenderStats& stats() const { return m_stats; }
    const QString& glRendererInfo() const { return m_glRendererInfo; }
    int activeSamples() const { return m_activeSamples; }
    bool shadowsEnabled() const { return m_shadowsEnabled; }
    bool postProcessEnabled() const { return m_postProcessEnabled; }
    bool wireframeMode() const { return m_wireframeMode; }
    QVector3D cameraPosition() const { return m_camPos; }
    float cameraSpeed() const { return m_camVel.length(); }
    int audioBiomeIndex() const { return (m_paletteIndex == 0) ? 0 : (m_paletteIndex - 1); }
    float diveDepth() const;
    float radialDistance() const;
    const char* biomeName() const;

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

    enum class WhaleState {
        Cruising,
        AvoidingTurn
    };

    void queryOpenGLCapabilities(const std::vector<AdapterCapability>& adapters);
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
    void cullAndUploadFauna(const FrustumPlanes& frustum);

    void spawnWhalePass(bool nearPlayer);
    float sampleWhaleClearance(float x, float z) const;
    float measureWhaleFreeDist(float x, float z, float yaw, float maxDist) const;
    QVector3D pickWhaleOpenWaypoint(const QVector3D& nearAnchor, float minDist, float maxDist) const;
    void updateWhaleNavigation(float dt, bool& whaleCalledOut);
    void updateFaunaAgents(float dt);
    void updateAnglerAgents(float dt);
    QVector3D computeAnglerLureWorldPos(const FaunaAgent& a) const;

    QVector3D cameraForward() const;
    QVector3D cameraRight() const;

    // GPU info & render settings
    QString m_glRendererInfo;
    int m_activeSamples = 8;
    bool m_postProcessEnabled = true;
    bool m_shadowsEnabled = true;
    bool m_wireframeMode = false;

    // World data & telemetry
    WorldData m_world;
    uint32_t m_seed = 42u;
    int m_paletteIndex = 0; // 0 = Tropical Reef (Vibrant) by default
    FrameRenderStats m_stats;
    bool m_wasPlayerColliding = false;
    float m_time = 0.0f;

    std::vector<ChunkLodState> m_chunkLodStates;
    std::vector<uint8_t> m_fishLodState;
    std::vector<uint8_t> m_mantaLodState;
    std::vector<uint8_t> m_anglerLodState;
    uint8_t m_whaleLodState = 0;

    // Shaders
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

    // Textures & geometry buffers
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

    // Shadow map FBO
    static constexpr int SHADOW_MAP_RES = 2048;
    GLuint m_shadowFBO = 0;
    GLuint m_shadowDepthTex = 0;

    // MSAA HDR & Bloom FBOs
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

    // Autonomous humpback whale state
    std::mt19937 m_rng;
    FaunaAgent m_whale;
    QVector3D m_whaleWaypoint{0.0f, 20.5f, 0.0f};
    float m_whaleWaypointTimer = 0.0f;
    float m_whaleSongTimer = 7.0f;

    WhaleState m_whaleState = WhaleState::Cruising;
    float m_whaleTurnDir = 1.0f;
    float m_whaleAvoidTimer = 0.0f;
    float m_whaleAvoidAccumYaw = 0.0f;
    float m_whaleCurrentYawRate = 0.0f;
    QVector3D m_whaleProgressAnchor{0.0f, 20.5f, 0.0f};
    float m_whaleProgressTimer = 0.0f;
    float m_whaleStuckAccum = 0.0f;

    bool m_whaleSurfacing = false;
    float m_whaleSurfaceCooldown = 8.0f;
    float m_whaleSurfaceTimer = 0.0f;
    float m_whaleSurfaceBlend = 0.0f;

    // Diver camera state
    QVector3D m_camPos{0.0f, -6.0f, 20.0f};
    QVector3D m_camVel{0.0f, 0.0f, 0.0f};
    float m_yaw = 0.0f;
    float m_pitch = 0.06f;
};
