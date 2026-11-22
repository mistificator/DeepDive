#pragma once
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QString>
#include <QVector3D>
#include <QVector4D>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>
#include "GpuCapabilities.h"
#include "WorldGenerator.h"

class DeepDiveEngine;
class QOpenGLFunctions_4_0_Core;

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
};

class DeepDiveRender : protected QOpenGLFunctions_3_3_Core {
public:
    DeepDiveRender() = default;
    ~DeepDiveRender() = default;

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

    // Procedural texture generation & world geometry upload
    static std::vector<uint8_t> generateRockTextureData(int size);
    void uploadRockTexture(const std::vector<uint8_t>& rgba, int size);
    void uploadWorld(const WorldData& world);

    // Frame execution
    void resize(int physW, int physH);
    void clearScreen(float r, float g, float b);
    void render(const DeepDiveEngine& engine, int physW, int physH, GLuint targetFramebuffer);

    // Runtime render toggles
    void togglePostProcess();
    void toggleShadows();
    void toggleWireframe();
    void toggleTessellation();

    // Telemetry & render state queries
    const FrameRenderStats& stats() const { return m_stats; }
    const QString& glRendererInfo() const { return m_glRendererInfo; }
    int glMajorVersion() const { return m_glMajorVersion; }
    int glMinorVersion() const { return m_glMinorVersion; }
    int maxTessLevel() const { return m_maxTessLevel; }
    int activeSamples() const { return m_activeSamples; }
    bool shadowsEnabled() const { return m_shadowsEnabled; }
    bool postProcessEnabled() const { return m_postProcessEnabled; }
    bool wireframeMode() const { return m_wireframeMode; }
    bool tessellationSupported() const { return m_tessSupported; }
    bool tessellationEnabled() const { return m_tessSupported && m_tessEnabled; }

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

    bool linkTessProgram(QOpenGLShaderProgram& prog,
                         const char* vertSrc,
                         const char* tcsSrc,
                         const char* tesSrc,
                         const char* fragSrc);

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

    void gatherAndUploadInstProps(const WorldData& world,
                                  const std::vector<std::pair<int, float>>& chunks,
                                  bool forShadow);
    void drawInstPropOne(const PropLodBuffers& buf, size_t count, GLenum primitiveMode, bool countTris);
    void drawInstPropBatches(bool forShadow, bool countTris, bool skipTessRocks = false);
    void cullAndUploadFauna(const DeepDiveEngine& engine, const FrustumPlanes& frustum);

    // Optional OpenGL 4.0+ tessellation interface (owned by QOpenGLContext)
    QOpenGLFunctions_4_0_Core* m_gl4 = nullptr;
    int m_glMajorVersion = 3;
    int m_glMinorVersion = 3;
    int m_maxTessLevel = 0;
    bool m_gl4Ready = false;
    bool m_envTessReady = false;
    bool m_instPropTessReady = false;
    bool m_waterTessReady = false;
    bool m_tessSupported = false;
    bool m_tessEnabled = false;

    // GPU capabilities & settings
    QString m_glRendererInfo;
    int m_activeSamples = 8;
    bool m_postProcessEnabled = true;
    bool m_shadowsEnabled = true;
    bool m_wireframeMode = false;
    FrameRenderStats m_stats;

    // Hysteresis LOD state
    std::vector<ChunkLodState> m_chunkLodStates;
    std::vector<uint8_t> m_fishLodState;
    std::vector<uint8_t> m_mantaLodState;
    std::vector<uint8_t> m_anglerLodState;
    uint8_t m_whaleLodState = 0;

    // OpenGL 3.3 core shader programs
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

    // Optional OpenGL 4.0+ hardware tessellation programs
    QOpenGLShaderProgram m_envTessShader;
    QOpenGLShaderProgram m_instPropTessShader;
    QOpenGLShaderProgram m_waterTessShader;

    // Textures & static/dynamic geometry buffers
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
};
