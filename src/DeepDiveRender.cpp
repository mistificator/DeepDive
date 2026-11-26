#include "DeepDiveRender.h"
#include "DeepDiveEngine.h"
#include "Noise.h"
#include "ShadersAtmosphere.h"
#include "ShadersGeometry.h"
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_0_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>
#include <QVector2D>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>

namespace {
constexpr float TWO_PI = 6.28318530717958647692f;

inline float distanceToAABB(const QVector3D& p, const QVector3D& bmin, const QVector3D& bmax) {
    const float cx = ProcMath::clamp(p.x(), bmin.x(), bmax.x());
    const float cy = ProcMath::clamp(p.y(), bmin.y(), bmax.y());
    const float cz = ProcMath::clamp(p.z(), bmin.z(), bmax.z());
    return (p - QVector3D(cx, cy, cz)).length();
}

inline FaunaGpuInstance packFaunaInstance(const FaunaAgent& a) {
    FaunaGpuInstance gpu;
    gpu.posScale  = QVector4D(a.pos.x(), a.pos.y(), a.pos.z(), a.scale);
    gpu.rotPhase  = QVector4D(a.yaw, a.phase, a.type, a.bank);
    gpu.pitchTint = QVector4D(a.pitch, a.tint.x(), a.tint.y(), a.tint.z());
    return gpu;
}

inline int updateLodWithHysteresis(int currentLod, float dist,
                                   const float* thresholds, int maxLod, float band) {
    while (currentLod < maxLod && dist > thresholds[currentLod] + band) {
        ++currentLod;
    }
    while (currentLod > 0 && dist < thresholds[currentLod - 1] - band) {
        --currentLod;
    }
    return currentLod;
}

inline uint8_t updateBinaryLod(uint8_t currentLod, float dist, float threshold, float band) {
    if (currentLod == 0 && dist > threshold + band) return 1;
    if (currentLod != 0 && dist < threshold - band) return 0;
    return currentLod;
}

inline bool isTessellatedPropArchetype(int archetype) {
    switch (archetype) {
    case PROP_BOULDER_CHUNK:
    case PROP_CLIFF_CRAG:
    case PROP_KELP_COLUMN:
    case PROP_BARREL_SPONGE:
    case PROP_CORAL_BUSH:
    case PROP_TABLE_FAN:
    case PROP_ANEMONE_PEN:
        return true;
    case PROP_GROUND_CARPET:
    case PROP_CLIFF_GRASS:
    default:
        return false;
    }
}

inline float cloudRemap(float v, float lo1, float hi1, float lo2, float hi2) {
    return lo2 + (v - lo1) * (hi2 - lo2) / std::max(hi1 - lo1, 1e-5f);
}

// 3D tileable gradient Perlin noise (C2 quintic fade, power-of-two period)
inline float tileablePerlin3D(float u, float v, float w, int period, uint32_t seed) {
    const float x = u * static_cast<float>(period);
    const float y = v * static_cast<float>(period);
    const float z = w * static_cast<float>(period);
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);
    const float fz = z - std::floor(z);

    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const float uz = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);

    const int mask = period - 1;
    auto gradDot = [&](int dx, int dy, int dz) -> float {
        const uint32_t h = ProcMath::hashInt(
                               static_cast<uint32_t>((ix + dx) & mask),
                               static_cast<uint32_t>((iy + dy) & mask),
                               static_cast<uint32_t>((iz + dz) & mask), seed) & 15u;
        const float gx = fx - static_cast<float>(dx);
        const float gy = fy - static_cast<float>(dy);
        const float gz = fz - static_cast<float>(dz);
        const float a = (h < 8u) ? gx : gy;
        const float b = (h < 4u) ? gy : ((h == 12u || h == 14u) ? gx : gz);
        return ((h & 1u) ? -a : a) + ((h & 2u) ? -b : b);
    };

    const float nx00 = ProcMath::mix(gradDot(0, 0, 0), gradDot(1, 0, 0), ux);
    const float nx10 = ProcMath::mix(gradDot(0, 1, 0), gradDot(1, 1, 0), ux);
    const float nx01 = ProcMath::mix(gradDot(0, 0, 1), gradDot(1, 0, 1), ux);
    const float nx11 = ProcMath::mix(gradDot(0, 1, 1), gradDot(1, 1, 1), ux);

    const float nxy0 = ProcMath::mix(nx00, nx10, uy);
    const float nxy1 = ProcMath::mix(nx01, nx11, uy);
    return ProcMath::clamp(ProcMath::mix(nxy0, nxy1, uz) * 0.72f + 0.5f, 0.0f, 1.0f);
}

// 2D tileable inverted Worley noise (1 - F1): distinct rounded cumulus clusters
inline float tileableWorley2D(float u, float v, int period, uint32_t seed) {
    const float x = u * static_cast<float>(period);
    const float y = v * static_cast<float>(period);
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);

    const int mask = period - 1;
    float minD2 = 10.0f;

    for (int dy = -1; dy <= 1; ++dy) {
        const int cy = (iy + dy) & mask;
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = (ix + dx) & mask;
            const float rx = static_cast<float>(dx) + ProcMath::hash3(cx, cy, 3, seed) - fx;
            const float ry = static_cast<float>(dy) + ProcMath::hash3(cx, cy, 7, seed + 29u) - fy;
            const float d2 = rx * rx + ry * ry;
            if (d2 < minD2) minD2 = d2;
        }
    }
    return ProcMath::clamp(1.0f - std::sqrt(minD2), 0.0f, 1.0f);
}

// 3D tileable inverted Worley noise (1 - F1): billowing cauliflower lobes
inline float tileableWorley3D(float u, float v, float w, int period, uint32_t seed) {
    const float x = u * static_cast<float>(period);
    const float y = v * static_cast<float>(period);
    const float z = w * static_cast<float>(period);
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);
    const float fz = z - std::floor(z);

    const int mask = period - 1;
    float minD2 = 10.0f;

    for (int dz = -1; dz <= 1; ++dz) {
        const int cz = (iz + dz) & mask;
        for (int dy = -1; dy <= 1; ++dy) {
            const int cy = (iy + dy) & mask;
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = (ix + dx) & mask;
                const float rx = static_cast<float>(dx) + ProcMath::hash3(cx, cy, cz, seed)       - fx;
                const float ry = static_cast<float>(dy) + ProcMath::hash3(cx, cy, cz, seed + 19u) - fy;
                const float rz = static_cast<float>(dz) + ProcMath::hash3(cx, cy, cz, seed + 43u) - fz;
                const float d2 = rx * rx + ry * ry + rz * rz;
                if (d2 < minD2) minD2 = d2;
            }
        }
    }
    return ProcMath::clamp(1.0f - std::sqrt(minD2), 0.0f, 1.0f);
}

// LOD transition thresholds for each of the 9 instanced prop archetypes (LOD0 -> LOD1 -> Cull)
struct PropLodConfig {
    float lod0Dist;
    float maxDist;
    float hysteresis;
    bool  castShadow;
    float shadowLod0Dist;
    float shadowMaxDist;
};

constexpr std::array<PropLodConfig, PROP_ARCHETYPE_COUNT> PROP_LOD_CFG = {{
    // PROP_BOULDER_CHUNK
    {225.0f, 420.0f, 10.0f, true,  105.0f, 155.0f},
    // PROP_CLIFF_CRAG
    {235.0f, 480.0f, 10.0f, true,  105.0f, 155.0f},
    // PROP_KELP_COLUMN
    {135.0f, 330.0f,  8.0f, true,   85.0f, 150.0f},
    // PROP_BARREL_SPONGE
    {115.0f, 225.0f,  8.0f, true,   60.0f,  95.0f},
    // PROP_GROUND_CARPET
    {100.0f, 195.0f,  8.0f, false,   0.0f,   0.0f},
    // PROP_CORAL_BUSH
    {125.0f, 240.0f,  8.0f, true,   65.0f, 105.0f},
    // PROP_TABLE_FAN
    {140.0f, 265.0f,  8.0f, true,   75.0f, 115.0f},
    // PROP_ANEMONE_PEN
    {120.0f, 230.0f,  8.0f, false,   0.0f,   0.0f},
    // PROP_CLIFF_GRASS
    {130.0f, 540.0f, 12.0f, false,   0.0f,   0.0f}
}};
} // namespace

// ============================================================================
// FRUSTUM CULLING
// ============================================================================

void FrustumPlanes::extractFromMatrix(const QMatrix4x4& m) {
    const QVector4D r0 = m.row(0);
    const QVector4D r1 = m.row(1);
    const QVector4D r2 = m.row(2);
    const QVector4D r3 = m.row(3);

    planes[0] = r3 + r0; // Left
    planes[1] = r3 - r0; // Right
    planes[2] = r3 + r1; // Bottom
    planes[3] = r3 - r1; // Top
    planes[4] = r3 + r2; // Near
    planes[5] = r3 - r2; // Far

    for (auto& pl : planes) {
        const float len = pl.toVector3D().length();
        if (len > 1e-6f) {
            pl /= len;
        }
    }
}

bool FrustumPlanes::intersectsSphere(const QVector3D& center, float radius) const {
    for (const auto& pl : planes) {
        const float dist = pl.x() * center.x() + pl.y() * center.y() + pl.z() * center.z() + pl.w();
        if (dist < -radius) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// LIFECYCLE & GPU INITIALIZATION
// ============================================================================

void DeepDiveRender::initializeGLContext() {
    initializeOpenGLFunctions();
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_PROGRAM_POINT_SIZE);

    m_gl4 = nullptr;
    m_gl4Ready = false;
    m_envTessReady = false;
    m_instPropTessReady = false;
    m_faunaTessReady = false;
    m_waterTessReady = false;
    m_tessSupported = false;
    m_tessEnabled = false;
    m_maxTessLevel = 0;

    if (QOpenGLContext* ctx = QOpenGLContext::currentContext()) {
        const QSurfaceFormat fmt = ctx->format();
        m_glMajorVersion = fmt.majorVersion();
        m_glMinorVersion = fmt.minorVersion();

        if (m_glMajorVersion >= 4) {
            m_gl4 = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_0_Core>(ctx);
            if (m_gl4 && m_gl4->initializeOpenGLFunctions()) {
                GLint maxTess = 0;
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTess);
                glGetError();
                m_maxTessLevel = static_cast<int>(maxTess);
                m_gl4Ready = (m_maxTessLevel >= 4);
            } else {
                m_gl4 = nullptr;
            }
        }
    }
}

bool DeepDiveRender::linkTessProgram(QOpenGLShaderProgram& prog,
                                     const char* vertSrc,
                                     const char* tcsSrc,
                                     const char* tesSrc,
                                     const char* fragSrc) {
    prog.removeAllShaders();
    const bool ok =
        prog.addShaderFromSourceCode(QOpenGLShader::Vertex, vertSrc) &&
        prog.addShaderFromSourceCode(QOpenGLShader::TessellationControl, tcsSrc) &&
        prog.addShaderFromSourceCode(QOpenGLShader::TessellationEvaluation, tesSrc) &&
        prog.addShaderFromSourceCode(QOpenGLShader::Fragment, fragSrc) &&
        prog.link();
    if (!ok) {
        prog.removeAllShaders();
    }
    return ok;
}

void DeepDiveRender::initBaseResources(const std::vector<AdapterCapability>& adapters) {
    queryOpenGLCapabilities(adapters);
    createShadowMapBuffer();

    const float skyQuad[] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f
    };
    glGenVertexArrays(1, &m_skyVAO);
    glGenBuffers(1, &m_skyVBO);
    glBindVertexArray(m_skyVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_skyVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyQuad), skyQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

    glGenVertexArrays(1, &m_terrainVAO);
    glGenBuffers(1, &m_terrainVBO);
    glGenBuffers(1, &m_terrainEBO);

    glGenVertexArrays(1, &m_propVAO);
    glGenBuffers(1, &m_propVBO);
    glGenBuffers(1, &m_propEBO);

    glGenVertexArrays(1, &m_rayVAO);
    glGenBuffers(1, &m_rayVBO);
    glGenBuffers(1, &m_rayEBO);
    glGenBuffers(1, &m_rayInstVBO);

    glGenVertexArrays(1, &m_particleVAO);
    glGenBuffers(1, &m_particleVBO);

    // Dynamic vertex buffer for 3D airborne breach spray, splashdown crowns & whale blow spout
    m_sprayMaxVertices = DeepDiveEngine::MAX_SPRAY_PARTICLES;
    glGenVertexArrays(1, &m_sprayVAO);
    glGenBuffers(1, &m_sprayVBO);
    glBindVertexArray(m_sprayVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_sprayVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<size_t>(m_sprayMaxVertices) * sizeof(SprayGpuVertex),
                 nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SprayGpuVertex),
                          reinterpret_cast<void*>(offsetof(SprayGpuVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(SprayGpuVertex),
                          reinterpret_cast<void*>(offsetof(SprayGpuVertex, colorSize)));
    glBindVertexArray(0);
}

void DeepDiveRender::compileShadowShaders() {
    m_shadowEnvShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ShadowEnvVert);
    m_shadowEnvShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ShadowDepthFrag);
    m_shadowEnvShader.link();

    m_shadowInstPropShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ShadowInstPropVert);
    m_shadowInstPropShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ShadowDepthFrag);
    m_shadowInstPropShader.link();

    m_shadowFaunaShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ShadowFaunaVert);
    m_shadowFaunaShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ShadowDepthFrag);
    m_shadowFaunaShader.link();
}

void DeepDiveRender::compileEnvironmentShaders() {
    m_envShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::EnvironmentVert);
    m_envShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::EnvironmentFrag);
    m_envShader.link();

    m_instPropShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::InstPropVert);
    m_instPropShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::EnvironmentFrag);
    m_instPropShader.link();

    m_faunaShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::FaunaVert);
    m_faunaShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::FaunaFrag);
    m_faunaShader.link();

    if (m_gl4Ready) {
        m_envTessReady = linkTessProgram(
            m_envTessShader,
            Shaders::EnvTessVert,
            Shaders::EnvTessControl,
            Shaders::EnvTessEval,
            Shaders::EnvironmentFrag400);

        m_instPropTessReady = linkTessProgram(
            m_instPropTessShader,
            Shaders::InstPropTessVert,
            Shaders::InstPropTessControl,
            Shaders::InstPropTessEval,
            Shaders::EnvironmentFrag400);

        m_faunaTessReady = linkTessProgram(
            m_faunaTessShader,
            Shaders::FaunaTessVert,
            Shaders::FaunaTessControl,
            Shaders::FaunaTessEval,
            Shaders::FaunaFrag400);
    }
}

void DeepDiveRender::compileWaterAndLightShaders() {
    m_waterShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::WaterSurfaceVert);
    m_waterShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::WaterSurfaceFrag);
    m_waterShader.link();

    m_skyShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::SkyboxVert);
    m_skyShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::SkyboxFrag);
    m_skyShader.link();

    m_rayShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::TranslucentVert);
    m_rayShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::TranslucentFrag);
    m_rayShader.link();

    if (m_gl4Ready) {
        m_waterTessReady = linkTessProgram(
            m_waterTessShader,
            Shaders::WaterTessVert,
            Shaders::WaterTessControl,
            Shaders::WaterTessEval,
            Shaders::WaterSurfaceFrag400);

        // Enable hardware tessellation by default once all GL 4.0 tess stages link cleanly
        m_tessSupported = m_envTessReady && m_instPropTessReady && m_faunaTessReady && m_waterTessReady;
        m_tessEnabled = m_tessSupported;
    }
}

void DeepDiveRender::compilePostProcessShaders() {
    m_particleShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ParticleVert);
    m_particleShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ParticleFrag);
    m_particleShader.link();

    m_sprayShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::SprayVert);
    m_sprayShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::SprayFrag);
    m_sprayShader.link();

    m_scatterBlurShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_scatterBlurShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ScatterBlurFrag);
    m_scatterBlurShader.link();

    m_hdrBloomDownShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_hdrBloomDownShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::HdrBloomDownFrag);
    m_hdrBloomDownShader.link();

    m_hdrBloomUpShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_hdrBloomUpShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::HdrBloomUpFrag);
    m_hdrBloomUpShader.link();

    m_autoExposureShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_autoExposureShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::AutoExposureFrag);
    m_autoExposureShader.link();

    m_postProcessShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_postProcessShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::PostProcessFrag);
    m_postProcessShader.link();
}

void DeepDiveRender::initRenderBuffers(int physW, int physH) {
    createWaterSurfaceMesh();
    createCloudNoiseTextures();
    resizePostProcessBuffers(physW, physH);
}

void DeepDiveRender::refreshGpuCapabilities(const std::vector<AdapterCapability>& adapters) {
    queryOpenGLCapabilities(adapters);
}

void DeepDiveRender::cleanup() {
    m_gl4 = nullptr;
    m_gl4Ready = false;
    m_envTessReady = false;
    m_instPropTessReady = false;
    m_faunaTessReady = false;
    m_waterTessReady = false;
    m_tessSupported = false;
    m_tessEnabled = false;

    destroyPostProcessBuffers();

    if (m_shadowFBO) { glDeleteFramebuffers(1, &m_shadowFBO); m_shadowFBO = 0; }
    if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex); m_shadowDepthTex = 0; }
    if (m_rockTex) { glDeleteTextures(1, &m_rockTex); m_rockTex = 0; }
    if (m_cloudShapeTex)   { glDeleteTextures(1, &m_cloudShapeTex);   m_cloudShapeTex = 0; }
    if (m_cloudDetailTex)  { glDeleteTextures(1, &m_cloudDetailTex);  m_cloudDetailTex = 0; }
    if (m_cloudWeatherTex) { glDeleteTextures(1, &m_cloudWeatherTex); m_cloudWeatherTex = 0; }

    if (m_terrainVAO) { glDeleteVertexArrays(1, &m_terrainVAO); m_terrainVAO = 0; }
    if (m_terrainVBO) { glDeleteBuffers(1, &m_terrainVBO); m_terrainVBO = 0; }
    if (m_terrainEBO) { glDeleteBuffers(1, &m_terrainEBO); m_terrainEBO = 0; }

    if (m_propVAO) { glDeleteVertexArrays(1, &m_propVAO); m_propVAO = 0; }
    if (m_propVBO) { glDeleteBuffers(1, &m_propVBO); m_propVBO = 0; }
    if (m_propEBO) { glDeleteBuffers(1, &m_propEBO); m_propEBO = 0; }

    for (int i = 0; i < PROP_ARCHETYPE_COUNT; ++i) {
        deletePropLodBuffers(m_propLod0[i]);
        deletePropLodBuffers(m_propLod1[i]);
    }

    deleteFaunaLodBuffers(m_fishLod0);
    deleteFaunaLodBuffers(m_fishLod1);
    deleteFaunaLodBuffers(m_mantaLod0);
    deleteFaunaLodBuffers(m_mantaLod1);
    deleteFaunaLodBuffers(m_whaleLod0);
    deleteFaunaLodBuffers(m_whaleLod1);
    deleteFaunaLodBuffers(m_anglerLod0);
    deleteFaunaLodBuffers(m_anglerLod1);
    deleteFaunaLodBuffers(m_seagullLod0);
    deleteFaunaLodBuffers(m_seagullLod1);

    if (m_waterVAO) { glDeleteVertexArrays(1, &m_waterVAO); m_waterVAO = 0; }
    if (m_waterVBO) { glDeleteBuffers(1, &m_waterVBO); m_waterVBO = 0; }
    if (m_waterEBO) { glDeleteBuffers(1, &m_waterEBO); m_waterEBO = 0; }

    if (m_skyVAO) { glDeleteVertexArrays(1, &m_skyVAO); m_skyVAO = 0; }
    if (m_skyVBO) { glDeleteBuffers(1, &m_skyVBO); m_skyVBO = 0; }

    if (m_rayVAO) { glDeleteVertexArrays(1, &m_rayVAO); m_rayVAO = 0; }
    if (m_rayVBO) { glDeleteBuffers(1, &m_rayVBO); m_rayVBO = 0; }
    if (m_rayEBO) { glDeleteBuffers(1, &m_rayEBO); m_rayEBO = 0; }
    if (m_rayInstVBO) { glDeleteBuffers(1, &m_rayInstVBO); m_rayInstVBO = 0; }

    if (m_particleVAO) { glDeleteVertexArrays(1, &m_particleVAO); m_particleVAO = 0; }
    if (m_particleVBO) { glDeleteBuffers(1, &m_particleVBO); m_particleVBO = 0; }

    if (m_sprayVAO) { glDeleteVertexArrays(1, &m_sprayVAO); m_sprayVAO = 0; }
    if (m_sprayVBO) { glDeleteBuffers(1, &m_sprayVBO); m_sprayVBO = 0; }
    m_sprayMaxVertices = 0;
}

void DeepDiveRender::queryOpenGLCapabilities(const std::vector<AdapterCapability>& adapters) {
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* vendor   = reinterpret_cast<const char*>(glGetString(GL_VENDOR));

    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    m_activeSamples = std::min(8, static_cast<int>(maxSamples));

    constexpr GLenum GL_GPU_MEM_TOTAL_NVX = 0x9048;
    GLint totalMemKB = 0;
    glGetIntegerv(GL_GPU_MEM_TOTAL_NVX, &totalMemKB);
    glGetError();

    QString vramStr;
    if (totalMemKB > 0) {
        vramStr = QString(" (%1 MB VRAM)").arg(totalMemKB / 1024);
    } else if (!adapters.empty()) {
        vramStr = QString(" (%1 MB VRAM)").arg(adapters.front().dedicatedVramMB);
    }

    m_glRendererInfo = QString("%1 [%2]%3")
                           .arg(renderer ? renderer : "Unknown GPU")
                           .arg(vendor ? vendor : "Unknown Vendor")
                           .arg(vramStr);
}

void DeepDiveRender::createShadowMapBuffer() {
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    glGenFramebuffers(1, &m_shadowFBO);
    glGenTextures(1, &m_shadowDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 SHADOW_MAP_RES, SHADOW_MAP_RES, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
}

void DeepDiveRender::createWaterSurfaceMesh() {
    constexpr int RINGS = 88;
    constexpr int SECTORS = 96;
    constexpr float RADIUS = 1650.0f;

    std::vector<QVector2D> verts;
    verts.reserve(1 + RINGS * SECTORS);
    verts.emplace_back(0.0f, 0.0f);

    for (int r = 1; r <= RINGS; ++r) {
        const float t = static_cast<float>(r) / static_cast<float>(RINGS);
        const float rad = RADIUS * std::pow(t, 2.75f);
        for (int s = 0; s < SECTORS; ++s) {
            const float ang = (TWO_PI * static_cast<float>(s)) / static_cast<float>(SECTORS);
            verts.emplace_back(std::cos(ang) * rad, std::sin(ang) * rad);
        }
    }

    std::vector<uint32_t> inds;
    inds.reserve(SECTORS * 3 + (RINGS - 1) * SECTORS * 6);

    for (int s = 0; s < SECTORS; ++s) {
        const uint32_t i1 = 1 + static_cast<uint32_t>(s);
        const uint32_t i2 = 1 + static_cast<uint32_t>((s + 1) % SECTORS);
        inds.insert(inds.end(), {0u, i1, i2});
    }

    for (int r = 1; r < RINGS; ++r) {
        const uint32_t ringA = 1 + static_cast<uint32_t>((r - 1) * SECTORS);
        const uint32_t ringB = 1 + static_cast<uint32_t>(r * SECTORS);
        for (int s = 0; s < SECTORS; ++s) {
            const uint32_t sNext = static_cast<uint32_t>((s + 1) % SECTORS);
            const uint32_t a0 = ringA + s;
            const uint32_t a1 = ringA + sNext;
            const uint32_t b0 = ringB + s;
            const uint32_t b1 = ringB + sNext;
            inds.insert(inds.end(), {a0, b0, a1, a1, b0, b1});
        }
    }

    m_waterIndexCount = static_cast<GLsizei>(inds.size());

    glGenVertexArrays(1, &m_waterVAO);
    glGenBuffers(1, &m_waterVBO);
    glGenBuffers(1, &m_waterEBO);

    glBindVertexArray(m_waterVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_waterVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(QVector2D), verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_waterEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t), inds.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(QVector2D), nullptr);
    glBindVertexArray(0);
}

void DeepDiveRender::createCloudNoiseTextures() {
    const unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());

    // 1. Low-frequency 3D Base Shape Texture (64^3 RGBA8):
    //    R = Perlin-Worley, G/B/A = 3 contrast-boosted octaves of inverted Worley (billowing cumulus lobes)
    {
        constexpr int N = 64;
        std::vector<uint8_t> vol(static_cast<size_t>(N) * N * N * 4);
        std::vector<std::thread> workers;
        workers.reserve(numThreads);

        for (unsigned int t = 0; t < numThreads; ++t) {
            const int zStart = (t * N) / numThreads;
            const int zEnd   = ((t + 1) * N) / numThreads;
            workers.emplace_back([&, zStart, zEnd]() {
                for (int z = zStart; z < zEnd; ++z) {
                    const float w = (static_cast<float>(z) + 0.5f) / N;
                    for (int y = 0; y < N; ++y) {
                        const float v = (static_cast<float>(y) + 0.5f) / N;
                        for (int x = 0; x < N; ++x) {
                            const float u = (static_cast<float>(x) + 0.5f) / N;

                            const float pFbm = tileablePerlin3D(u, v, w,  4, 6101u) * 0.55f
                                               + tileablePerlin3D(u, v, w,  8, 6133u) * 0.30f
                                               + tileablePerlin3D(u, v, w, 16, 6173u) * 0.15f;

                            // Stretch Worley 3D dynamic range to full [0..1] for bold cauliflower domes
                            const float w4  = ProcMath::clamp(cloudRemap(tileableWorley3D(u, v, w,  4, 6203u), 0.18f, 0.85f, 0.0f, 1.0f), 0.0f, 1.0f);
                            const float w8  = ProcMath::clamp(cloudRemap(tileableWorley3D(u, v, w,  8, 6247u), 0.18f, 0.85f, 0.0f, 1.0f), 0.0f, 1.0f);
                            const float w16 = ProcMath::clamp(cloudRemap(tileableWorley3D(u, v, w, 16, 6287u), 0.18f, 0.85f, 0.0f, 1.0f), 0.0f, 1.0f);

                            const float wFbm = w4 * 0.625f + w8 * 0.25f + w16 * 0.125f;
                            const float perlinWorley = ProcMath::clamp(
                                cloudRemap(pFbm, (1.0f - wFbm) * 0.32f, 0.86f, 0.0f, 1.0f), 0.0f, 1.0f);

                            const size_t idx = (static_cast<size_t>(z) * N * N + static_cast<size_t>(y) * N + x) * 4;
                            vol[idx + 0] = static_cast<uint8_t>(perlinWorley * 255.0f);
                            vol[idx + 1] = static_cast<uint8_t>(w4  * 255.0f);
                            vol[idx + 2] = static_cast<uint8_t>(w8  * 255.0f);
                            vol[idx + 3] = static_cast<uint8_t>(w16 * 255.0f);
                        }
                    }
                }
            });
        }
        for (auto& th : workers) th.join();

        if (m_cloudShapeTex == 0) glGenTextures(1, &m_cloudShapeTex);
        glBindTexture(GL_TEXTURE_3D, m_cloudShapeTex);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, N, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol.data());
        glGenerateMipmap(GL_TEXTURE_3D);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // 2. High-frequency 3D Detail Erosion Texture (32^3 RGBA8):
    //    R/G/B = 3 crisp octaves of fine inverted Worley noise
    {
        constexpr int N = 32;
        std::vector<uint8_t> vol(static_cast<size_t>(N) * N * N * 4);
        std::vector<std::thread> workers;
        workers.reserve(numThreads);

        for (unsigned int t = 0; t < numThreads; ++t) {
            const int zStart = (t * N) / numThreads;
            const int zEnd   = ((t + 1) * N) / numThreads;
            workers.emplace_back([&, zStart, zEnd]() {
                for (int z = zStart; z < zEnd; ++z) {
                    const float w = (static_cast<float>(z) + 0.5f) / N;
                    for (int y = 0; y < N; ++y) {
                        const float v = (static_cast<float>(y) + 0.5f) / N;
                        for (int x = 0; x < N; ++x) {
                            const float u = (static_cast<float>(x) + 0.5f) / N;
                            const float d4  = ProcMath::clamp(cloudRemap(tileableWorley3D(u, v, w,  4, 6421u), 0.18f, 0.85f, 0.0f, 1.0f), 0.0f, 1.0f);
                            const float d8  = ProcMath::clamp(cloudRemap(tileableWorley3D(u, v, w,  8, 6469u), 0.18f, 0.85f, 0.0f, 1.0f), 0.0f, 1.0f);
                            const float d16 = ProcMath::clamp(cloudRemap(tileableWorley3D(u, v, w, 16, 6521u), 0.18f, 0.85f, 0.0f, 1.0f), 0.0f, 1.0f);

                            const size_t idx = (static_cast<size_t>(z) * N * N + static_cast<size_t>(y) * N + x) * 4;
                            vol[idx + 0] = static_cast<uint8_t>(d4  * 255.0f);
                            vol[idx + 1] = static_cast<uint8_t>(d8  * 255.0f);
                            vol[idx + 2] = static_cast<uint8_t>(d16 * 255.0f);
                            vol[idx + 3] = 255u;
                        }
                    }
                }
            });
        }
        for (auto& th : workers) th.join();

        if (m_cloudDetailTex == 0) glGenTextures(1, &m_cloudDetailTex);
        glBindTexture(GL_TEXTURE_3D, m_cloudDetailTex);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, N, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol.data());
        glGenerateMipmap(GL_TEXTURE_3D);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // 3. 2D Tileable Weather Map (256^2 RGBA8):
    //    R = sparse cumulus coverage (~25% sky), G = tower height (tall cores, tapered rims), B = cirrus, A = density
    {
        constexpr int N = 256;
        std::vector<uint8_t> map(static_cast<size_t>(N) * N * 4);
        std::vector<std::thread> workers;
        workers.reserve(numThreads);

        for (unsigned int t = 0; t < numThreads; ++t) {
            const int yStart = (t * N) / numThreads;
            const int yEnd   = ((t + 1) * N) / numThreads;
            workers.emplace_back([&, yStart, yEnd]() {
                for (int y = yStart; y < yEnd; ++y) {
                    const float v = (static_cast<float>(y) + 0.5f) / N;
                    for (int x = 0; x < N; ++x) {
                        const float u = (static_cast<float>(x) + 0.5f) / N;

                        const float wu = ProcMath::fract(u + (tileablePerlin3D(u, v, 0.1f, 4, 7001u) - 0.5f) * 0.12f + 1.0f);
                        const float wv = ProcMath::fract(v + (tileablePerlin3D(u, v, 0.7f, 4, 7013u) - 0.5f) * 0.12f + 1.0f);

                        // True 2D Worley islands gated by macro Perlin -> crisp scattered cumulus groups
                        const float pMacro = tileablePerlin3D(wu, wv, 0.25f, 4, 7103u) * 0.65f
                                             + tileablePerlin3D(wu, wv, 0.25f, 8, 7127u) * 0.35f;
                        const float wCell  = tileableWorley2D(wu, wv, 8,  7187u) * 0.68f
                                            + tileableWorley2D(wu, wv, 16, 7193u) * 0.32f;
                        const float covRaw = pMacro * 0.44f + wCell * 0.56f;
                        const float coverage = ProcMath::smoothstep(0.55f, 0.80f, covRaw);

                        // Tall billowing towers at the core of each cloud, lower rounded shoulders at edges
                        const float typeNoise = tileablePerlin3D(wu, wv, 0.65f, 4, 7211u) * 0.55f
                                                + tileableWorley2D(wu, wv, 8, 7237u) * 0.45f;
                        const float cloudType = ProcMath::clamp(coverage * 0.62f + typeNoise * 0.42f, 0.15f, 1.0f);

                        // Faint high-altitude cirrus wisps
                        const float cu = ProcMath::fract(u + (tileablePerlin3D(u, v, 0.3f, 8, 7307u) - 0.5f) * 0.20f + 1.0f);
                        const float cv = ProcMath::fract(v + (tileablePerlin3D(u, v, 0.8f, 4, 7321u) - 0.5f) * 0.10f + 1.0f);
                        const float cirrusRaw = tileablePerlin3D(cu, cv, 0.5f,  8, 7333u) * 0.55f
                                                + tileablePerlin3D(cu, cv, 0.5f, 16, 7349u) * 0.30f
                                                + tileablePerlin3D(cu, cv, 0.5f, 32, 7369u) * 0.15f;
                        const float cirrus = ProcMath::smoothstep(0.66f, 0.88f, cirrusRaw);

                        const float moisture = 0.72f + 0.28f * tileablePerlin3D(wu, wv, 0.9f, 8, 7411u);

                        const size_t idx = (static_cast<size_t>(y) * N + x) * 4;
                        map[idx + 0] = static_cast<uint8_t>(ProcMath::clamp(coverage,  0.0f, 1.0f) * 255.0f);
                        map[idx + 1] = static_cast<uint8_t>(ProcMath::clamp(cloudType, 0.0f, 1.0f) * 255.0f);
                        map[idx + 2] = static_cast<uint8_t>(ProcMath::clamp(cirrus,    0.0f, 1.0f) * 255.0f);
                        map[idx + 3] = static_cast<uint8_t>(ProcMath::clamp(moisture,  0.0f, 1.0f) * 255.0f);
                    }
                }
            });
        }
        for (auto& th : workers) th.join();

        if (m_cloudWeatherTex == 0) glGenTextures(1, &m_cloudWeatherTex);
        glBindTexture(GL_TEXTURE_2D, m_cloudWeatherTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, map.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    glBindTexture(GL_TEXTURE_3D, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

std::vector<uint8_t> DeepDiveRender::generateRockTextureData(int size) {
    const int N = size;
    std::vector<float> heightMap(static_cast<size_t>(N) * N);
    std::vector<float> cavityMap(static_cast<size_t>(N) * N);

    const unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (unsigned int t = 0; t < numThreads; ++t) {
        const int yStart = (t * N) / numThreads;
        const int yEnd   = ((t + 1) * N) / numThreads;
        workers.emplace_back([&, yStart, yEnd]() {
            for (int y = yStart; y < yEnd; ++y) {
                const float v = static_cast<float>(y) / N;
                for (int x = 0; x < N; ++x) {
                    const float u = static_cast<float>(x) / N;

                    const float wU = (ProcMath::tileableNoise2D(u, v, 4, 113u) - 0.5f) * 0.14f
                                     + (ProcMath::tileableNoise2D(u, v, 8, 127u) - 0.5f) * 0.06f;
                    const float wV = (ProcMath::tileableNoise2D(u, v, 4, 131u) - 0.5f) * 0.14f
                                     + (ProcMath::tileableNoise2D(u, v, 8, 137u) - 0.5f) * 0.06f;
                    const float wu = ProcMath::fract(u + wU + 1.0f);
                    const float wv = ProcMath::fract(v + wV + 1.0f);

                    float fbmVal = 0.0f;
                    fbmVal += 0.30f * ProcMath::tileableNoise2D(wu, wv, 8,   101u);
                    fbmVal += 0.24f * ProcMath::tileableNoise2D(wu, wv, 16,  202u);
                    fbmVal += 0.18f * ProcMath::tileableNoise2D(u,  v,  32,  303u);
                    fbmVal += 0.13f * ProcMath::tileableNoise2D(u,  v,  64,  404u);
                    fbmVal += 0.09f * ProcMath::tileableNoise2D(u,  v,  128, 505u);
                    fbmVal += 0.06f * ProcMath::tileableNoise2D(u,  v,  256, 606u);

                    const float grain = ProcMath::tileableNoise2D(wu, wv, 12, 653u) * 0.65f
                                        + ProcMath::tileableNoise2D(u,  v,  48, 677u) * 0.35f;

                    const float vor1 = ProcMath::tileableVoronoi2D(wu, wv, 4,  707u);
                    const float vor2 = ProcMath::tileableVoronoi2D(wu, wv, 12, 808u);
                    const float vor3 = ProcMath::tileableVoronoi2D(u,  v,  32, 909u);
                    const float crackMain = ProcMath::smoothstep(0.015f, 0.14f, vor1);
                    const float crackMed  = ProcMath::smoothstep(0.015f, 0.16f, vor2);
                    const float crackFine = ProcMath::smoothstep(0.02f,  0.18f, vor3);

                    const float cavity = ProcMath::clamp(crackMain * 0.52f + crackMed * 0.30f + crackFine * 0.18f, 0.0f, 1.0f);
                    const float h = ProcMath::clamp(fbmVal * 0.56f + grain * 0.16f + cavity * 0.28f, 0.0f, 1.0f);

                    const size_t idx = static_cast<size_t>(y) * N + x;
                    heightMap[idx] = h;
                    cavityMap[idx] = cavity;
                }
            }
        });
    }
    for (auto& th : workers) th.join();

    std::vector<uint8_t> rgba(static_cast<size_t>(N) * N * 4);
    const int mask = N - 1;
    const float normalStrength = 8.2f;

    for (int y = 0; y < N; ++y) {
        const int yU = (y - 1) & mask;
        const int yD = (y + 1) & mask;
        for (int x = 0; x < N; ++x) {
            const int xL = (x - 1) & mask;
            const int xR = (x + 1) & mask;

            const float hTL = heightMap[static_cast<size_t>(yU) * N + xL];
            const float hT  = heightMap[static_cast<size_t>(yU) * N + x ];
            const float hTR = heightMap[static_cast<size_t>(yU) * N + xR];
            const float hL  = heightMap[static_cast<size_t>(y ) * N + xL];
            const float hR  = heightMap[static_cast<size_t>(y ) * N + xR];
            const float hBL = heightMap[static_cast<size_t>(yD) * N + xL];
            const float hB  = heightMap[static_cast<size_t>(yD) * N + x ];
            const float hBR = heightMap[static_cast<size_t>(yD) * N + xR];

            const float dx = ((hTL + 2.0f * hL + hBL) - (hTR + 2.0f * hR + hBR)) * 0.25f;
            const float dy = ((hTL + 2.0f * hT + hTR) - (hBL + 2.0f * hB + hBR)) * 0.25f;

            QVector3D norm(dx * normalStrength, dy * normalStrength, 1.0f);
            norm.normalize();

            const size_t pIdx = static_cast<size_t>(y) * N + x;
            const size_t idx  = pIdx * 4;
            rgba[idx + 0] = static_cast<uint8_t>(ProcMath::clamp(norm.x() * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[idx + 1] = static_cast<uint8_t>(ProcMath::clamp(norm.y() * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[idx + 2] = static_cast<uint8_t>(cavityMap[pIdx] * 255.0f); // B = cavity / cracks
            rgba[idx + 3] = static_cast<uint8_t>(heightMap[pIdx] * 255.0f); // A = height
        }
    }

    return rgba;
}

void DeepDiveRender::uploadRockTexture(const std::vector<uint8_t>& rgba, int size) {
    if (m_rockTex == 0) {
        glGenTextures(1, &m_rockTex);
    }
    glBindTexture(GL_TEXTURE_2D, m_rockTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_VAL = 0x84FF;
    constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_EXT_VAL = 0x84FE;
    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_VAL, &maxAniso);
    if (maxAniso > 1.0f) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT_VAL, std::min(maxAniso, 16.0f));
    }
}

void DeepDiveRender::destroyPostProcessBuffers() {
    if (m_msaaFBO) glDeleteFramebuffers(1, &m_msaaFBO);
    if (m_msaaColorRBO) glDeleteRenderbuffers(1, &m_msaaColorRBO);
    if (m_msaaDepthRBO) glDeleteRenderbuffers(1, &m_msaaDepthRBO);

    if (m_resolveFBO) glDeleteFramebuffers(1, &m_resolveFBO);
    if (m_sceneColorTex) glDeleteTextures(1, &m_sceneColorTex);
    if (m_sceneDepthTex) glDeleteTextures(1, &m_sceneDepthTex);

    if (m_bloomFBO[0]) glDeleteFramebuffers(2, m_bloomFBO);
    if (m_bloomTex[0]) glDeleteTextures(2, m_bloomTex);

    if (m_hdrBloomFBO[0]) glDeleteFramebuffers(HDR_BLOOM_MIPS, m_hdrBloomFBO);
    if (m_hdrBloomTex[0]) glDeleteTextures(HDR_BLOOM_MIPS, m_hdrBloomTex);

    if (m_exposureFBO[0]) glDeleteFramebuffers(2, m_exposureFBO);
    if (m_exposureTex[0]) glDeleteTextures(2, m_exposureTex);

    m_msaaFBO = m_msaaColorRBO = m_msaaDepthRBO = 0;
    m_resolveFBO = m_sceneColorTex = m_sceneDepthTex = 0;
    m_bloomFBO[0] = m_bloomFBO[1] = m_bloomTex[0] = m_bloomTex[1] = 0;
    for (int i = 0; i < HDR_BLOOM_MIPS; ++i) {
        m_hdrBloomFBO[i] = m_hdrBloomTex[i] = 0;
        m_hdrBloomW[i] = m_hdrBloomH[i] = 0;
    }
    m_exposureFBO[0] = m_exposureFBO[1] = 0;
    m_exposureTex[0] = m_exposureTex[1] = 0;
    m_exposureIdx = 0;
    m_exposureInitialized = false;
    m_fbWidth = m_fbHeight = 0;
}

void DeepDiveRender::resize(int physW, int physH) {
    resizePostProcessBuffers(physW, physH);
}

void DeepDiveRender::resizePostProcessBuffers(int physW, int physH) {
    physW = std::max(64, physW);
    physH = std::max(64, physH);
    if (physW == m_fbWidth && physH == m_fbHeight && m_msaaFBO != 0) return;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    destroyPostProcessBuffers();
    m_fbWidth = physW;
    m_fbHeight = physH;

    glGenFramebuffers(1, &m_msaaFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);

    glGenRenderbuffers(1, &m_msaaColorRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_msaaColorRBO);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_activeSamples, GL_RGBA16F, physW, physH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_msaaColorRBO);

    glGenRenderbuffers(1, &m_msaaDepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_msaaDepthRBO);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_activeSamples, GL_DEPTH_COMPONENT24, physW, physH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_msaaDepthRBO);

    glGenFramebuffers(1, &m_resolveFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_resolveFBO);

    glGenTextures(1, &m_sceneColorTex);
    glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, physW, physH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneColorTex, 0);

    glGenTextures(1, &m_sceneDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, physW, physH, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_sceneDepthTex, 0);

    // Underwater half-res scatter blur buffers
    const int halfW = std::max(32, physW / 2);
    const int halfH = std::max(32, physH / 2);
    glGenFramebuffers(2, m_bloomFBO);
    glGenTextures(2, m_bloomTex);
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[i]);
        glBindTexture(GL_TEXTURE_2D, m_bloomTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, halfW, halfH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_bloomTex[i], 0);
    }

    // Above-water 5-cascade HDR bloom pyramid
    glGenFramebuffers(HDR_BLOOM_MIPS, m_hdrBloomFBO);
    glGenTextures(HDR_BLOOM_MIPS, m_hdrBloomTex);
    int mipW = physW;
    int mipH = physH;
    for (int i = 0; i < HDR_BLOOM_MIPS; ++i) {
        mipW = std::max(8, mipW / 2);
        mipH = std::max(8, mipH / 2);
        m_hdrBloomW[i] = mipW;
        m_hdrBloomH[i] = mipH;

        glBindFramebuffer(GL_FRAMEBUFFER, m_hdrBloomFBO[i]);
        glBindTexture(GL_TEXTURE_2D, m_hdrBloomTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, mipW, mipH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_hdrBloomTex[i], 0);
    }

    // 1x1 GPU ping-pong auto-exposure textures
    glGenFramebuffers(2, m_exposureFBO);
    glGenTextures(2, m_exposureTex);
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_exposureFBO[i]);
        glBindTexture(GL_TEXTURE_2D, m_exposureTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 1, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_exposureTex[i], 0);
    }
    m_exposureIdx = 0;
    m_exposureInitialized = false;

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
}

void DeepDiveRender::clearScreen(float r, float g, float b) {
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// ============================================================================
// BUFFER SETUP & WORLD UPLOAD
// ============================================================================

void DeepDiveRender::setupFaunaLodBuffers(FaunaLodBuffers& buf,
                                          const std::vector<FaunaVertex>& verts,
                                          const std::vector<uint32_t>& inds,
                                          size_t maxInstCount) {
    if (buf.vao == 0) {
        glGenVertexArrays(1, &buf.vao);
        glGenBuffers(1, &buf.vbo);
        glGenBuffers(1, &buf.ebo);
        glGenBuffers(1, &buf.instVbo);
    }

    buf.indexCount   = static_cast<GLsizei>(inds.size());
    buf.maxInstances = static_cast<GLsizei>(std::max<size_t>(1, maxInstCount));

    glBindVertexArray(buf.vao);

    glBindBuffer(GL_ARRAY_BUFFER, buf.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(FaunaVertex),
                 verts.empty() ? nullptr : verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t),
                 inds.empty() ? nullptr : inds.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FaunaVertex),
                          reinterpret_cast<void*>(offsetof(FaunaVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(FaunaVertex),
                          reinterpret_cast<void*>(offsetof(FaunaVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(FaunaVertex),
                          reinterpret_cast<void*>(offsetof(FaunaVertex, color)));

    glBindBuffer(GL_ARRAY_BUFFER, buf.instVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<size_t>(buf.maxInstances) * sizeof(FaunaGpuInstance),
                 nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(FaunaGpuInstance),
                          reinterpret_cast<void*>(offsetof(FaunaGpuInstance, posScale)));
    glVertexAttribDivisor(3, 1);

    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(FaunaGpuInstance),
                          reinterpret_cast<void*>(offsetof(FaunaGpuInstance, rotPhase)));
    glVertexAttribDivisor(4, 1);

    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(FaunaGpuInstance),
                          reinterpret_cast<void*>(offsetof(FaunaGpuInstance, pitchTint)));
    glVertexAttribDivisor(5, 1);

    glBindVertexArray(0);
}

void DeepDiveRender::deleteFaunaLodBuffers(FaunaLodBuffers& buf) {
    if (buf.vao) glDeleteVertexArrays(1, &buf.vao);
    if (buf.vbo) glDeleteBuffers(1, &buf.vbo);
    if (buf.ebo) glDeleteBuffers(1, &buf.ebo);
    if (buf.instVbo) glDeleteBuffers(1, &buf.instVbo);
    buf = {};
}

void DeepDiveRender::setupPropLodBuffers(PropLodBuffers& buf,
                                         const std::vector<EnvVertex>& verts,
                                         const std::vector<uint32_t>& inds,
                                         size_t initialMaxInst) {
    if (buf.vao == 0) {
        glGenVertexArrays(1, &buf.vao);
        glGenBuffers(1, &buf.vbo);
        glGenBuffers(1, &buf.ebo);
        glGenBuffers(1, &buf.instVbo);
    }

    buf.indexCount   = static_cast<GLsizei>(inds.size());
    buf.maxInstances = static_cast<GLsizei>(std::max<size_t>(64, initialMaxInst));

    glBindVertexArray(buf.vao);

    glBindBuffer(GL_ARRAY_BUFFER, buf.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(EnvVertex),
                 verts.empty() ? nullptr : verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t),
                 inds.empty() ? nullptr : inds.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                          reinterpret_cast<void*>(offsetof(EnvVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                          reinterpret_cast<void*>(offsetof(EnvVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                          reinterpret_cast<void*>(offsetof(EnvVertex, color)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                          reinterpret_cast<void*>(offsetof(EnvVertex, param)));

    glBindBuffer(GL_ARRAY_BUFFER, buf.instVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<size_t>(buf.maxInstances) * sizeof(PropGpuInstance),
                 nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(PropGpuInstance),
                          reinterpret_cast<void*>(offsetof(PropGpuInstance, posScaleXZ)));
    glVertexAttribDivisor(4, 1);

    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(PropGpuInstance),
                          reinterpret_cast<void*>(offsetof(PropGpuInstance, rotScaleY)));
    glVertexAttribDivisor(5, 1);

    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(PropGpuInstance),
                          reinterpret_cast<void*>(offsetof(PropGpuInstance, tintFlags)));
    glVertexAttribDivisor(6, 1);

    glBindVertexArray(0);
}

void DeepDiveRender::ensurePropInstCapacity(PropLodBuffers& buf, size_t needed) {
    if (needed <= static_cast<size_t>(buf.maxInstances)) return;
    const size_t newCap = std::max(needed + needed / 2, static_cast<size_t>(512));
    buf.maxInstances = static_cast<GLsizei>(newCap);
    glBindBuffer(GL_ARRAY_BUFFER, buf.instVbo);
    glBufferData(GL_ARRAY_BUFFER, newCap * sizeof(PropGpuInstance), nullptr, GL_DYNAMIC_DRAW);
}

void DeepDiveRender::deletePropLodBuffers(PropLodBuffers& buf) {
    if (buf.vao) glDeleteVertexArrays(1, &buf.vao);
    if (buf.vbo) glDeleteBuffers(1, &buf.vbo);
    if (buf.ebo) glDeleteBuffers(1, &buf.ebo);
    if (buf.instVbo) glDeleteBuffers(1, &buf.instVbo);
    buf = {};
}

void DeepDiveRender::uploadWorld(const WorldData& world) {
    m_chunkLodStates.assign(world.chunks.size(), ChunkLodState{});
    m_fishLodState.assign(world.fishAgents.size(), 0);
    m_mantaLodState.assign(world.mantaAgents.size(), 0);
    m_anglerLodState.assign(world.anglerAgents.size(), 0);
    m_seagullLodState.assign(world.seagullAgents.size(), 0);
    m_whaleLodState = 0;

    auto setupEnvVAO = [&](GLuint vao, GLuint vbo, GLuint ebo,
                           const std::vector<EnvVertex>& verts,
                           const std::vector<uint32_t>& inds) {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(EnvVertex),
                     verts.empty() ? nullptr : verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t),
                     inds.empty() ? nullptr : inds.data(), GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                              reinterpret_cast<void*>(offsetof(EnvVertex, pos)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                              reinterpret_cast<void*>(offsetof(EnvVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                              reinterpret_cast<void*>(offsetof(EnvVertex, color)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(EnvVertex),
                              reinterpret_cast<void*>(offsetof(EnvVertex, param)));
    };

    setupEnvVAO(m_terrainVAO, m_terrainVBO, m_terrainEBO,
                world.terrainVertices, world.terrainIndices);
    setupEnvVAO(m_propVAO, m_propVBO, m_propEBO,
                world.propVertices, world.propIndices);

    for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
        const auto& proto = world.propPrototypes[a];
        setupPropLodBuffers(m_propLod0[a], proto.lod0Verts, proto.lod0Inds, 2048);
        setupPropLodBuffers(m_propLod1[a], proto.lod1Verts, proto.lod1Inds, 4096);
        m_visPropsLod0[a].reserve(2048);
        m_visPropsLod1[a].reserve(4096);
    }

    // Reserve +1 slot in fish & manta LOD0 buffers for the third-person player avatar
    setupFaunaLodBuffers(m_fishLod0,    world.fishLod0Verts,    world.fishLod0Inds,    world.fishAgents.size() + 1);
    setupFaunaLodBuffers(m_fishLod1,    world.fishLod1Verts,    world.fishLod1Inds,    world.fishAgents.size());
    setupFaunaLodBuffers(m_mantaLod0,   world.mantaLod0Verts,   world.mantaLod0Inds,   world.mantaAgents.size() + 1);
    setupFaunaLodBuffers(m_mantaLod1,   world.mantaLod1Verts,   world.mantaLod1Inds,   world.mantaAgents.size());
    setupFaunaLodBuffers(m_whaleLod0,   world.whaleLod0Verts,   world.whaleLod0Inds,   1);
    setupFaunaLodBuffers(m_whaleLod1,   world.whaleLod1Verts,   world.whaleLod1Inds,   1);
    setupFaunaLodBuffers(m_anglerLod0,  world.anglerLod0Verts,  world.anglerLod0Inds,  world.anglerAgents.size());
    setupFaunaLodBuffers(m_anglerLod1,  world.anglerLod1Verts,  world.anglerLod1Inds,  world.anglerAgents.size());
    setupFaunaLodBuffers(m_seagullLod0, world.seagullLod0Verts, world.seagullLod0Inds, world.seagullAgents.size());
    setupFaunaLodBuffers(m_seagullLod1, world.seagullLod1Verts, world.seagullLod1Inds, world.seagullAgents.size());

    m_visFishLod0.reserve(world.fishAgents.size() + 1);
    m_visFishLod1.reserve(world.fishAgents.size());
    m_visMantaLod0.reserve(world.mantaAgents.size() + 1);
    m_visMantaLod1.reserve(world.mantaAgents.size());
    m_visWhaleLod0.reserve(1);
    m_visWhaleLod1.reserve(1);
    m_visAnglerLod0.reserve(world.anglerAgents.size());
    m_visAnglerLod1.reserve(world.anglerAgents.size());
    m_visSeagullLod0.reserve(world.seagullAgents.size());
    m_visSeagullLod1.reserve(world.seagullAgents.size());

    m_rayIndexCount    = static_cast<GLsizei>(world.godRayMeshInds.size());
    m_rayInstanceCount = static_cast<GLsizei>(world.godRayInstances.size());

    glBindVertexArray(m_rayVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_rayVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 world.godRayMeshVerts.size() * sizeof(QVector3D),
                 world.godRayMeshVerts.empty() ? nullptr : world.godRayMeshVerts.data(),
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_rayEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 world.godRayMeshInds.size() * sizeof(uint32_t),
                 world.godRayMeshInds.empty() ? nullptr : world.godRayMeshInds.data(),
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(QVector3D), nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m_rayInstVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 world.godRayInstances.size() * sizeof(GodRayInstance),
                 world.godRayInstances.empty() ? nullptr : world.godRayInstances.data(),
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GodRayInstance),
                          reinterpret_cast<void*>(offsetof(GodRayInstance, topCenterRTop)));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GodRayInstance),
                          reinterpret_cast<void*>(offsetof(GodRayInstance, botCenterRBot)));
    glVertexAttribDivisor(2, 1);

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GodRayInstance),
                          reinterpret_cast<void*>(offsetof(GodRayInstance, rayParams)));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(m_particleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_particleVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 world.particles.size() * sizeof(Particle),
                 world.particles.empty() ? nullptr : world.particles.data(),
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Particle),
                          reinterpret_cast<void*>(offsetof(Particle, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Particle),
                          reinterpret_cast<void*>(offsetof(Particle, colorSize)));
    glBindVertexArray(0);
}

// ============================================================================
// RUNTIME TOGGLES
// ============================================================================

void DeepDiveRender::togglePostProcess() {
    m_postProcessEnabled = !m_postProcessEnabled;
}

void DeepDiveRender::toggleShadows() {
    m_shadowsEnabled = !m_shadowsEnabled;
}

void DeepDiveRender::toggleWireframe() {
    m_wireframeMode = !m_wireframeMode;
}

void DeepDiveRender::toggleTessellation() {
    if (m_tessSupported) {
        m_tessEnabled = !m_tessEnabled;
    }
}

// ============================================================================
// CULLING & INSTANCED BATCHING
// ============================================================================

void DeepDiveRender::gatherAndUploadInstProps(const WorldData& world,
                                              const std::vector<std::pair<int, float>>& chunks,
                                              bool forShadow) {
    for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
        m_visPropsLod0[a].clear();
        m_visPropsLod1[a].clear();
    }

    for (const auto& [idx, d] : chunks) {
        const auto& chunk = world.chunks[idx];
        ChunkLodState& st = m_chunkLodStates[idx];

        for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
            const auto& src = chunk.propInstances[a];
            if (src.empty()) continue;

            const auto& cfg = PROP_LOD_CFG[a];
            int lod = 2;

            if (forShadow) {
                if (!cfg.castShadow || d > cfg.shadowMaxDist) continue;
                lod = (d < cfg.shadowLod0Dist) ? 0 : 1;
            } else {
                const float thresh[2] = {cfg.lod0Dist, cfg.maxDist};
                st.propLod[a] = updateLodWithHysteresis(st.propLod[a], d, thresh, 2, cfg.hysteresis);
                lod = st.propLod[a];
            }

            if (lod == 0) {
                m_visPropsLod0[a].insert(m_visPropsLod0[a].end(), src.begin(), src.end());
            } else if (lod == 1) {
                m_visPropsLod1[a].insert(m_visPropsLod1[a].end(), src.begin(), src.end());
            }
        }
    }

    auto uploadBatch = [&](PropLodBuffers& buf, const std::vector<PropGpuInstance>& list) {
        if (list.empty() || buf.instVbo == 0) return;
        ensurePropInstCapacity(buf, list.size());
        glBindBuffer(GL_ARRAY_BUFFER, buf.instVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, list.size() * sizeof(PropGpuInstance), list.data());
    };

    for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
        uploadBatch(m_propLod0[a], m_visPropsLod0[a]);
        uploadBatch(m_propLod1[a], m_visPropsLod1[a]);
    }
}

void DeepDiveRender::drawInstPropOne(const PropLodBuffers& buf,
                                     size_t count,
                                     GLenum primitiveMode,
                                     bool countTris) {
    if (count == 0 || buf.indexCount == 0) return;
    const GLsizei clamped = static_cast<GLsizei>(std::min(count, static_cast<size_t>(buf.maxInstances)));
    glBindVertexArray(buf.vao);
    glDrawElementsInstanced(primitiveMode, buf.indexCount, GL_UNSIGNED_INT, nullptr, clamped);
    if (countTris) {
        m_stats.drawnTriangles += (buf.indexCount / 3) * clamped;
        m_stats.visibleInstProps += clamped;
    }
}

void DeepDiveRender::drawInstPropBatches(bool forShadow, bool countTris, bool skipTessProps) {
    for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
        if (forShadow && !PROP_LOD_CFG[a].castShadow) continue;
        if (!(skipTessProps && isTessellatedPropArchetype(a))) {
            drawInstPropOne(m_propLod0[a], m_visPropsLod0[a].size(), GL_TRIANGLES, countTris);
        }
        drawInstPropOne(m_propLod1[a], m_visPropsLod1[a].size(), GL_TRIANGLES, countTris);
    }
}

void DeepDiveRender::cullAndUploadFauna(const DeepDiveEngine& engine, const FrustumPlanes& frustum) {
    const WorldData& world = engine.world();
    const QVector3D camPos = engine.cameraPosition();
    const FaunaAgent& whale = engine.whale();

    m_visFishLod0.clear();
    m_visFishLod1.clear();
    m_visMantaLod0.clear();
    m_visMantaLod1.clear();
    m_visWhaleLod0.clear();
    m_visWhaleLod1.clear();
    m_visAnglerLod0.clear();
    m_visAnglerLod1.clear();
    m_visSeagullLod0.clear();
    m_visSeagullLod1.clear();

    const bool camAboveWater = engine.isCameraAboveWater();

    constexpr float FISH_LOD0_DIST    = 115.0f;
    constexpr float FISH_MAX_DIST     = 240.0f;
    constexpr float MANTA_LOD0_DIST   = 200.0f;
    constexpr float MANTA_MAX_DIST    = 350.0f;
    constexpr float WHALE_LOD0_DIST   = 330.0f;
    const float     whaleMaxDist      = camAboveWater ? 900.0f : 460.0f;
    constexpr float ANGLER_LOD0_DIST  = 165.0f;
    constexpr float ANGLER_MAX_DIST   = 310.0f;
    constexpr float SEAGULL_LOD0_DIST = 165.0f;
    const float     seagullMaxDist    = camAboveWater ? 950.0f : 450.0f;

    for (size_t i = 0; i < world.fishAgents.size(); ++i) {
        const auto& f = world.fishAgents[i];
        const float dist = (f.pos - camPos).length();
        if (dist > FISH_MAX_DIST) continue;
        if (!frustum.intersectsSphere(f.pos, f.boundingRadius * 1.4f)) continue;

        uint8_t& st = m_fishLodState[i];
        st = updateBinaryLod(st, dist, FISH_LOD0_DIST, 6.0f);
        if (st == 0) m_visFishLod0.push_back(packFaunaInstance(f));
        else         m_visFishLod1.push_back(packFaunaInstance(f));
    }

    for (size_t i = 0; i < world.mantaAgents.size(); ++i) {
        const auto& m = world.mantaAgents[i];
        const float dist = (m.pos - camPos).length();
        if (dist > MANTA_MAX_DIST) continue;
        if (!frustum.intersectsSphere(m.pos, m.boundingRadius * 1.6f)) continue;

        uint8_t& st = m_mantaLodState[i];
        st = updateBinaryLod(st, dist, MANTA_LOD0_DIST, 8.0f);
        if (st == 0) m_visMantaLod0.push_back(packFaunaInstance(m));
        else         m_visMantaLod1.push_back(packFaunaInstance(m));
    }

    {
        const float whaleDist = (whale.pos - camPos).length();
        if (whaleDist <= whaleMaxDist && frustum.intersectsSphere(whale.pos, whale.boundingRadius * 1.25f)) {
            m_whaleLodState = updateBinaryLod(m_whaleLodState, whaleDist, WHALE_LOD0_DIST, 12.0f);
            if (m_whaleLodState == 0) m_visWhaleLod0.push_back(packFaunaInstance(whale));
            else                      m_visWhaleLod1.push_back(packFaunaInstance(whale));
        }
    }

    for (size_t i = 0; i < world.anglerAgents.size(); ++i) {
        const auto& a = world.anglerAgents[i];
        const float dist = (a.pos - camPos).length();
        if (dist > ANGLER_MAX_DIST) continue;
        if (!frustum.intersectsSphere(a.pos, a.boundingRadius * 2.0f)) continue;

        uint8_t& st = m_anglerLodState[i];
        st = updateBinaryLod(st, dist, ANGLER_LOD0_DIST, 8.0f);
        if (st == 0) m_visAnglerLod0.push_back(packFaunaInstance(a));
        else         m_visAnglerLod1.push_back(packFaunaInstance(a));
    }

    for (size_t i = 0; i < world.seagullAgents.size(); ++i) {
        const auto& g = world.seagullAgents[i];
        const float dist = (g.pos - camPos).length();
        if (dist > seagullMaxDist) continue;
        if (!frustum.intersectsSphere(g.pos, g.boundingRadius * 1.8f)) continue;

        uint8_t& st = m_seagullLodState[i];
        st = updateBinaryLod(st, dist, SEAGULL_LOD0_DIST, 10.0f);
        if (st == 0) m_visSeagullLod0.push_back(packFaunaInstance(g));
        else         m_visSeagullLod1.push_back(packFaunaInstance(g));
    }

    // Append third-person player avatar at full LOD0 (rendered in both shadow and main passes)
    if (engine.hasPlayerAvatar()) {
        const FaunaAgent& av = engine.playerAvatar();
        if (frustum.intersectsSphere(av.pos, av.boundingRadius * 1.6f)) {
            if (engine.cameraMode() == CameraMode::ThirdPersonFish) {
                m_visFishLod0.push_back(packFaunaInstance(av));
            } else if (engine.cameraMode() == CameraMode::ThirdPersonManta) {
                m_visMantaLod0.push_back(packFaunaInstance(av));
            }
        }
    }

    auto upload = [&](FaunaLodBuffers& buf, const std::vector<FaunaGpuInstance>& list) {
        if (list.empty() || buf.instVbo == 0) return;
        const size_t count = std::min(list.size(), static_cast<size_t>(buf.maxInstances));
        glBindBuffer(GL_ARRAY_BUFFER, buf.instVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(FaunaGpuInstance), list.data());
    };

    upload(m_fishLod0,    m_visFishLod0);
    upload(m_fishLod1,    m_visFishLod1);
    upload(m_mantaLod0,   m_visMantaLod0);
    upload(m_mantaLod1,   m_visMantaLod1);
    upload(m_whaleLod0,   m_visWhaleLod0);
    upload(m_whaleLod1,   m_visWhaleLod1);
    upload(m_anglerLod0,  m_visAnglerLod0);
    upload(m_anglerLod1,  m_visAnglerLod1);
    upload(m_seagullLod0, m_visSeagullLod0);
    upload(m_seagullLod1, m_visSeagullLod1);

    m_stats.visibleFishLod0  = static_cast<int>(m_visFishLod0.size());
    m_stats.visibleFishLod1  = static_cast<int>(m_visFishLod1.size());
    m_stats.visibleMantaLod0 = static_cast<int>(m_visMantaLod0.size());
    m_stats.visibleMantaLod1 = static_cast<int>(m_visMantaLod1.size());
    m_stats.visibleAnglers   = static_cast<int>(m_visAnglerLod0.size() + m_visAnglerLod1.size());
    m_stats.visibleSeagulls  = static_cast<int>(m_visSeagullLod0.size() + m_visSeagullLod1.size());
}

// ============================================================================
// MAIN RENDER PIPELINE
// ============================================================================

void DeepDiveRender::render(const DeepDiveEngine& engine, int physW, int physH, GLuint targetFramebuffer) {
    physW = std::max(64, physW);
    physH = std::max(64, physH);
    resizePostProcessBuffers(physW, physH);

    const WorldData& world     = engine.world();
    const BiomeVisuals visuals = engine.biomeVisuals();
    const QVector3D camPos     = engine.cameraPosition();
    const QVector3D camForward = engine.cameraForward();
    const FaunaAgent& whale    = engine.whale();
    const float simTime        = engine.simulationTime();
    const int lureCount        = engine.lureCount();
    const auto& lurePositions  = engine.lurePositions();
    const bool camAboveWater   = engine.isCameraAboveWater();
    const int camAboveInt      = camAboveWater ? 1 : 0;

    const bool useTess = (m_tessSupported && m_tessEnabled && m_gl4 != nullptr);
    const float maxTessF = static_cast<float>(std::max(1, m_maxTessLevel));

    constexpr float UNDERWATER_FAR_PLANE  = 460.0f;
    constexpr float ABOVE_WATER_FAR_PLANE = 1700.0f;
    const float zFar = camAboveWater ? ABOVE_WATER_FAR_PLANE : UNDERWATER_FAR_PLANE;

    QMatrix4x4 proj;
    const float aspect = static_cast<float>(physW) / static_cast<float>(physH);
    const float fovY = std::clamp(65.0f + engine.dynamicFovOffset(), 52.0f, 78.0f);
    proj.perspective(fovY, aspect, 0.25f, zFar);

    QMatrix4x4 view;
    view.lookAt(camPos, camPos + camForward, QVector3D(0.0f, 1.0f, 0.0f));
    const QMatrix4x4 viewProj = proj * view;
    const QMatrix4x4 invViewProj = viewProj.inverted();

    FrustumPlanes camFrustum;
    camFrustum.extractFromMatrix(viewProj);

    auto drawPropRange = [&](const MeshRange& range, bool countTris) {
        if (range.indexCount == 0) return;
        const void* offsetPtr = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(range.indexOffset) * sizeof(uint32_t));
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(range.indexCount), GL_UNSIGNED_INT, offsetPtr);
        if (countTris) {
            m_stats.drawnTriangles += static_cast<int>(range.indexCount / 3);
        }
    };

    auto drawFaunaBatch = [&](const FaunaLodBuffers& buf, size_t instanceCount,
                              GLenum primitiveMode, bool countTris) {
        if (instanceCount == 0 || buf.indexCount == 0) return;
        const GLsizei clamped = static_cast<GLsizei>(std::min(instanceCount, static_cast<size_t>(buf.maxInstances)));
        glBindVertexArray(buf.vao);
        glDrawElementsInstanced(primitiveMode, buf.indexCount, GL_UNSIGNED_INT, nullptr, clamped);
        if (countTris) {
            m_stats.drawnTriangles += (buf.indexCount / 3) * clamped;
        }
    };

    // ========================================================================
    // PASS 0: Directional Shadow Map (kept non-tessellated for speed)
    // ========================================================================
    const float orthoHalfSize = 110.0f;
    const float texelWorldStep = (orthoHalfSize * 2.0f) / SHADOW_MAP_RES;
    const QVector3D shadowFocus(
        std::floor(camPos.x() / texelWorldStep) * texelWorldStep,
        0.0f,
        std::floor(camPos.z() / texelWorldStep) * texelWorldStep
        );

    QMatrix4x4 lightProj;
    lightProj.ortho(-orthoHalfSize, orthoHalfSize, -orthoHalfSize, orthoHalfSize, 1.0f, 270.0f);
    QMatrix4x4 lightView;
    lightView.lookAt(shadowFocus + visuals.sunDir * 125.0f, shadowFocus, QVector3D(0.0f, 0.0f, 1.0f));
    const QMatrix4x4 lightSpaceMatrix = lightProj * lightView;

    if (m_shadowsEnabled) {
        FrustumPlanes lightFrustum;
        lightFrustum.extractFromMatrix(lightSpaceMatrix);

        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
        glViewport(0, 0, SHADOW_MAP_RES, SHADOW_MAP_RES);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glClear(GL_DEPTH_BUFFER_BIT);

        m_shadowEnvShader.bind();
        m_shadowEnvShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        m_shadowEnvShader.setUniformValue("uTime", simTime);

        std::vector<std::pair<int, float>> shadowChunks;
        shadowChunks.reserve(64);

        glBindVertexArray(m_terrainVAO);
        for (int i = 0; i < static_cast<int>(world.chunks.size()); ++i) {
            const auto& chunk = world.chunks[i];
            if (!lightFrustum.intersectsSphere(chunk.center, chunk.boundingRadius)) continue;

            const float d = distanceToAABB(camPos, chunk.minBounds, chunk.maxBounds);
            shadowChunks.emplace_back(i, d);

            const int sLod = (d < 85.0f) ? 0 : (d < 165.0f ? 1 : (d < 260.0f ? 2 : 3));
            const auto& range = chunk.terrainLodRanges[sLod];
            const void* idxPtr = reinterpret_cast<const void*>(
                static_cast<uintptr_t>(range.indexOffset) * sizeof(uint32_t));
            glDrawElementsBaseVertex(GL_TRIANGLES,
                                     static_cast<GLsizei>(range.indexCount),
                                     GL_UNSIGNED_INT, idxPtr,
                                     static_cast<GLint>(chunk.terrainBaseVertex));
        }

        glBindVertexArray(m_propVAO);
        for (const auto& [idx, d] : shadowChunks) {
            drawPropRange(world.chunks[idx].shipwreckRange, false);
        }

        gatherAndUploadInstProps(world, shadowChunks, true);
        m_shadowInstPropShader.bind();
        m_shadowInstPropShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        m_shadowInstPropShader.setUniformValue("uTime", simTime);
        drawInstPropBatches(true, false);

        cullAndUploadFauna(engine, lightFrustum);
        m_shadowFaunaShader.bind();
        m_shadowFaunaShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        m_shadowFaunaShader.setUniformValue("uTime", simTime);

        drawFaunaBatch(m_fishLod0,    m_visFishLod0.size(),    GL_TRIANGLES, false);
        drawFaunaBatch(m_fishLod1,    m_visFishLod1.size(),    GL_TRIANGLES, false);
        drawFaunaBatch(m_mantaLod0,   m_visMantaLod0.size(),   GL_TRIANGLES, false);
        drawFaunaBatch(m_mantaLod1,   m_visMantaLod1.size(),   GL_TRIANGLES, false);
        drawFaunaBatch(m_whaleLod0,   m_visWhaleLod0.size(),   GL_TRIANGLES, false);
        drawFaunaBatch(m_whaleLod1,   m_visWhaleLod1.size(),   GL_TRIANGLES, false);
        drawFaunaBatch(m_anglerLod0,  m_visAnglerLod0.size(),  GL_TRIANGLES, false);
        drawFaunaBatch(m_anglerLod1,  m_visAnglerLod1.size(),  GL_TRIANGLES, false);
        drawFaunaBatch(m_seagullLod0, m_visSeagullLod0.size(), GL_TRIANGLES, false);
        drawFaunaBatch(m_seagullLod1, m_visSeagullLod1.size(), GL_TRIANGLES, false);
    }

    // ========================================================================
    // PASS 1: Main Scene Render into MSAA HDR FBO
    // ========================================================================
    m_stats.visibleChunks = 0;
    m_stats.totalChunks = static_cast<int>(world.chunks.size());
    m_stats.lodCounts[0] = m_stats.lodCounts[1] = m_stats.lodCounts[2] = m_stats.lodCounts[3] = 0;
    m_stats.drawnTriangles = 0;
    m_stats.visibleInstProps = 0;

    cullAndUploadFauna(engine, camFrustum);

    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
    glViewport(0, 0, physW, physH);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (useTess) {
        m_gl4->glPatchParameteri(GL_PATCH_VERTICES, 3);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rockTex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_3D, m_cloudShapeTex);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_3D, m_cloudDetailTex);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_cloudWeatherTex);

    glDepthFunc(GL_LEQUAL);
    m_skyShader.bind();
    m_skyShader.setUniformValue("uCloudShapeTex", 4);
    m_skyShader.setUniformValue("uCloudDetailTex", 5);
    m_skyShader.setUniformValue("uCloudWeatherTex", 6);
    m_skyShader.setUniformValue("uInvViewProj", invViewProj);
    m_skyShader.setUniformValue("uCamPos", camPos);
    m_skyShader.setUniformValue("uSunDir", visuals.sunDir);
    m_skyShader.setUniformValue("uSunColor", visuals.sunColor);
    m_skyShader.setUniformValue("uWaterColorDeep", visuals.waterDeep);
    m_skyShader.setUniformValue("uWaterColorShallow", visuals.waterShallow);
    m_skyShader.setUniformValue("uWaterY", ProcMath::WATER_SURFACE_Y);
    m_skyShader.setUniformValue("uTime", simTime);
    m_skyShader.setUniformValue("uCamAboveWater", camAboveInt);
    glBindVertexArray(m_skyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (m_wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    auto bindCommonEnvUniforms = [&](QOpenGLShaderProgram& prog) {
        prog.bind();
        prog.setUniformValue("uRockTex", 0);
        prog.setUniformValue("uShadowMap", 3);
        prog.setUniformValue("uViewProj", viewProj);
        prog.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        prog.setUniformValue("uCamPos", camPos);
        prog.setUniformValue("uSunDir", visuals.sunDir);
        prog.setUniformValue("uSunColor", visuals.sunColor);
        prog.setUniformValue("uWaterColorDeep", visuals.waterDeep);
        prog.setUniformValue("uWaterColorShallow", visuals.waterShallow);
        prog.setUniformValue("uVibrance", visuals.vibrance);
        prog.setUniformValue("uFogScale", visuals.fogScale);
        prog.setUniformValue("uTime", simTime);
        prog.setUniformValue("uWaterY", ProcMath::WATER_SURFACE_Y);
        prog.setUniformValue("uCamAboveWater", camAboveInt);
        prog.setUniformValue("uFarPlane", zFar);
        prog.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);
        prog.setUniformValue("uLureCount", lureCount);
        if (lureCount > 0) {
            prog.setUniformValueArray("uLurePos", lurePositions.data(), lureCount);
        }
    };

    glDepthFunc(GL_LESS);

    static constexpr float TERRAIN_LOD_THRESH[3] = {210.0f, 310.0f, 395.0f};

    std::vector<std::pair<int, float>> visibleChunks;
    visibleChunks.reserve(camAboveWater ? 256 : 128);

    const float chunkMaxDist = camAboveWater ? 1660.0f : UNDERWATER_FAR_PLANE;

    // 1.1. Seafloor & cliff walls (4-tier LOD; LOD0 uses hardware tessellation when enabled)
    for (int i = 0; i < static_cast<int>(world.chunks.size()); ++i) {
        const auto& chunk = world.chunks[i];
        if (!camFrustum.intersectsSphere(chunk.center, chunk.boundingRadius)) continue;

        const float d = distanceToAABB(camPos, chunk.minBounds, chunk.maxBounds);
        if (d > chunkMaxDist) continue;

        // Beyond 460m above water, only keep chunks whose cliffs rise to/above water surface
        // (in WorldGeneratorEnvironment.cpp maxBounds.y = maxY + 32.0f)
        if (d > UNDERWATER_FAR_PLANE && chunk.maxBounds.y() < ProcMath::WATER_SURFACE_Y + 29.0f) {
            continue;
        }

        ChunkLodState& st = m_chunkLodStates[i];
        st.terrainLod = updateLodWithHysteresis(st.terrainLod, d, TERRAIN_LOD_THRESH, 3, 8.0f);

        m_stats.visibleChunks++;
        m_stats.lodCounts[st.terrainLod]++;
        visibleChunks.emplace_back(i, d);
    }

    auto drawTerrainChunk = [&](int idx, GLenum primitiveMode) {
        const auto& chunk = world.chunks[idx];
        const int lod = m_chunkLodStates[idx].terrainLod;
        const auto& range = chunk.terrainLodRanges[lod];
        const void* idxPtr = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(range.indexOffset) * sizeof(uint32_t));
        glDrawElementsBaseVertex(primitiveMode,
                                 static_cast<GLsizei>(range.indexCount),
                                 GL_UNSIGNED_INT, idxPtr,
                                 static_cast<GLint>(chunk.terrainBaseVertex));
        m_stats.drawnTriangles += static_cast<int>(range.indexCount / 3);
    };

    glBindVertexArray(m_terrainVAO);
    if (useTess) {
        bindCommonEnvUniforms(m_envTessShader);
        m_envTessShader.setUniformValue("uMaxTessLevel", maxTessF);
        for (const auto& [idx, d] : visibleChunks) {
            if (m_chunkLodStates[idx].terrainLod == 0) {
                drawTerrainChunk(idx, GL_PATCHES);
            }
        }
    }

    bindCommonEnvUniforms(m_envShader);
    for (const auto& [idx, d] : visibleChunks) {
        if (!useTess || m_chunkLodStates[idx].terrainLod > 0) {
            drawTerrainChunk(idx, GL_TRIANGLES);
        }
    }

    // 1.2. Static unique props (shipwrecks & abyssal sea pens) — m_envShader is already bound
    glBindVertexArray(m_propVAO);
    for (const auto& [idx, d] : visibleChunks) {
        const auto& chunk = world.chunks[idx];
        ChunkLodState& st = m_chunkLodStates[idx];

        if (st.detailVisible && d > 192.0f)       st.detailVisible = false;
        else if (!st.detailVisible && d < 176.0f) st.detailVisible = true;
        if (st.detailVisible) drawPropRange(chunk.detailRange, true);

        if (d < 430.0f) drawPropRange(chunk.shipwreckRange, true);
    }

    // 1.3. Instanced environment props (LOD0 rocks & flora tessellated when enabled)
    gatherAndUploadInstProps(world, visibleChunks, false);
    if (useTess) {
        bindCommonEnvUniforms(m_instPropTessShader);
        m_instPropTessShader.setUniformValue("uCarpetTint", visuals.carpetTint);
        m_instPropTessShader.setUniformValue("uKelpTint", visuals.kelpTint);
        m_instPropTessShader.setUniformValue("uMaxTessLevel", maxTessF);

        for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
            if (isTessellatedPropArchetype(a)) {
                drawInstPropOne(m_propLod0[a], m_visPropsLod0[a].size(), GL_PATCHES, true);
            }
        }
    }

    bindCommonEnvUniforms(m_instPropShader);
    m_instPropShader.setUniformValue("uCarpetTint", visuals.carpetTint);
    m_instPropShader.setUniformValue("uKelpTint", visuals.kelpTint);
    drawInstPropBatches(false, true, useTess);

    // 1.4. Marine & aerial fauna (LOD0 smoothly tessellated when enabled; LOD1 stays GL_TRIANGLES)
    if (useTess) {
        bindCommonEnvUniforms(m_faunaTessShader);
        m_faunaTessShader.setUniformValue("uMaxTessLevel", maxTessF);
        drawFaunaBatch(m_fishLod0,    m_visFishLod0.size(),    GL_PATCHES, true);
        drawFaunaBatch(m_mantaLod0,   m_visMantaLod0.size(),   GL_PATCHES, true);
        drawFaunaBatch(m_whaleLod0,   m_visWhaleLod0.size(),   GL_PATCHES, true);
        drawFaunaBatch(m_anglerLod0,  m_visAnglerLod0.size(),  GL_PATCHES, true);
        drawFaunaBatch(m_seagullLod0, m_visSeagullLod0.size(), GL_PATCHES, true);

        bindCommonEnvUniforms(m_faunaShader);
        drawFaunaBatch(m_fishLod1,    m_visFishLod1.size(),    GL_TRIANGLES, true);
        drawFaunaBatch(m_mantaLod1,   m_visMantaLod1.size(),   GL_TRIANGLES, true);
        drawFaunaBatch(m_whaleLod1,   m_visWhaleLod1.size(),   GL_TRIANGLES, true);
        drawFaunaBatch(m_anglerLod1,  m_visAnglerLod1.size(),  GL_TRIANGLES, true);
        drawFaunaBatch(m_seagullLod1, m_visSeagullLod1.size(), GL_TRIANGLES, true);
    } else {
        bindCommonEnvUniforms(m_faunaShader);
        drawFaunaBatch(m_fishLod0,    m_visFishLod0.size(),    GL_TRIANGLES, true);
        drawFaunaBatch(m_fishLod1,    m_visFishLod1.size(),    GL_TRIANGLES, true);
        drawFaunaBatch(m_mantaLod0,   m_visMantaLod0.size(),   GL_TRIANGLES, true);
        drawFaunaBatch(m_mantaLod1,   m_visMantaLod1.size(),   GL_TRIANGLES, true);
        drawFaunaBatch(m_whaleLod0,   m_visWhaleLod0.size(),   GL_TRIANGLES, true);
        drawFaunaBatch(m_whaleLod1,   m_visWhaleLod1.size(),   GL_TRIANGLES, true);
        drawFaunaBatch(m_anglerLod0,  m_visAnglerLod0.size(),  GL_TRIANGLES, true);
        drawFaunaBatch(m_anglerLod1,  m_visAnglerLod1.size(),  GL_TRIANGLES, true);
        drawFaunaBatch(m_seagullLod0, m_visSeagullLod0.size(), GL_TRIANGLES, true);
        drawFaunaBatch(m_seagullLod1, m_visSeagullLod1.size(), GL_TRIANGLES, true);
    }

    // Resolve scene depth for shoreline foam, shoal transparency, and water contact ripples
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFBO);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rockTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);

    const QVector2D waterCenter(std::floor(camPos.x() * 2.0f) * 0.5f,
                                std::floor(camPos.z() * 2.0f) * 0.5f);
    QOpenGLShaderProgram& activeWaterShader = useTess ? m_waterTessShader : m_waterShader;
    activeWaterShader.bind();
    activeWaterShader.setUniformValue("uRockTex", 0);
    activeWaterShader.setUniformValue("uSceneDepthTex", 1);
    activeWaterShader.setUniformValue("uCloudWeatherTex", 6);
    activeWaterShader.setUniformValue("uInvResolution", QVector2D(1.0f / physW, 1.0f / physH));
    activeWaterShader.setUniformValue("uViewProj", viewProj);
    activeWaterShader.setUniformValue("uWaterCenter", waterCenter);
    activeWaterShader.setUniformValue("uWaterY", ProcMath::WATER_SURFACE_Y);
    activeWaterShader.setUniformValue("uCamPos", camPos);
    activeWaterShader.setUniformValue("uSunDir", visuals.sunDir);
    activeWaterShader.setUniformValue("uSunColor", visuals.sunColor);
    activeWaterShader.setUniformValue("uWaterColorDeep", visuals.waterDeep);
    activeWaterShader.setUniformValue("uWaterColorShallow", visuals.waterShallow);
    activeWaterShader.setUniformValue("uFogScale", visuals.fogScale);
    activeWaterShader.setUniformValue("uTime", simTime);
    activeWaterShader.setUniformValue("uCamAboveWater", camAboveInt);
    activeWaterShader.setUniformValue("uFarPlane", zFar);
    activeWaterShader.setUniformValue("uWhaleSurfA",
                                      QVector4D(whale.pos.x(), whale.pos.y(), whale.pos.z(), whale.scale));
    activeWaterShader.setUniformValue("uWhaleSurfB",
                                      QVector4D(std::sin(whale.yaw), std::cos(whale.yaw),
                                                std::sin(whale.pitch), std::cos(whale.pitch)));
    if (useTess) {
        activeWaterShader.setUniformValue("uMaxTessLevel", maxTessF);
    }
    glBindVertexArray(m_waterVAO);
    glDrawElements(useTess ? GL_PATCHES : GL_TRIANGLES, m_waterIndexCount, GL_UNSIGNED_INT, nullptr);
    m_stats.drawnTriangles += static_cast<int>(m_waterIndexCount / 3);

    if (m_wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // Underwater volumetric rays and bubble/plankton suspension (skipped while camera is in air)
    if (!camAboveWater) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);

        if (m_rayIndexCount > 0 && m_rayInstanceCount > 0) {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT); // Render back faces so shafts work both outside and inside

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_rockTex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);

            m_rayShader.bind();
            m_rayShader.setUniformValue("uRockTex", 0);
            m_rayShader.setUniformValue("uSceneDepthTex", 1);
            m_rayShader.setUniformValue("uShadowMap", 3);
            m_rayShader.setUniformValue("uViewProj", viewProj);
            m_rayShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
            m_rayShader.setUniformValue("uInvResolution", QVector2D(1.0f / physW, 1.0f / physH));
            m_rayShader.setUniformValue("uCamPos", camPos);
            m_rayShader.setUniformValue("uSunDir", visuals.sunDir);
            m_rayShader.setUniformValue("uSunColor", visuals.sunColor);
            m_rayShader.setUniformValue("uWaterColorShallow", visuals.waterShallow);
            m_rayShader.setUniformValue("uFogScale", visuals.fogScale);
            m_rayShader.setUniformValue("uTime", simTime);
            m_rayShader.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);
            m_rayShader.setUniformValue("uFarPlane", zFar);

            glBindVertexArray(m_rayVAO);
            glDrawElementsInstanced(GL_TRIANGLES, m_rayIndexCount, GL_UNSIGNED_INT, nullptr, m_rayInstanceCount);

            glDisable(GL_CULL_FACE);
            glEnable(GL_DEPTH_TEST);
        }

        if (!world.particles.empty() && m_particleVBO != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, m_particleVBO);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            world.particles.size() * sizeof(Particle),
                            world.particles.data());

            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            m_particleShader.bind();
            m_particleShader.setUniformValue("uViewProj", viewProj);
            m_particleShader.setUniformValue("uCamPos", camPos);
            m_particleShader.setUniformValue("uViewportHeight", static_cast<float>(physH));
            m_particleShader.setUniformValue("uWaterY", ProcMath::WATER_SURFACE_Y);
            glBindVertexArray(m_particleVAO);
            glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(world.particles.size()));
        }

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // 3D airborne spray droplets, breach fountains & surfacing whale blow spout
    const auto& sprayVerts = engine.sprayVertices();
    if (!sprayVerts.empty() && m_sprayVBO != 0 && m_sprayMaxVertices > 0) {
        const GLsizei sprayCount = static_cast<GLsizei>(
            std::min(sprayVerts.size(), static_cast<size_t>(m_sprayMaxVertices)));

        glBindBuffer(GL_ARRAY_BUFFER, m_sprayVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<size_t>(sprayCount) * sizeof(SprayGpuVertex),
                        sprayVerts.data());

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_rockTex);

        m_sprayShader.bind();
        m_sprayShader.setUniformValue("uRockTex", 0);
        m_sprayShader.setUniformValue("uViewProj", viewProj);
        m_sprayShader.setUniformValue("uCamPos", camPos);
        m_sprayShader.setUniformValue("uSunDir", visuals.sunDir);
        m_sprayShader.setUniformValue("uViewportHeight", static_cast<float>(physH));
        m_sprayShader.setUniformValue("uTime", simTime);
        m_sprayShader.setUniformValue("uWaterY", ProcMath::WATER_SURFACE_Y);
        m_sprayShader.setUniformValue("uFarPlane", zFar);

        glBindVertexArray(m_sprayVAO);
        glDrawArrays(GL_POINTS, 0, sprayCount);

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ========================================================================
    // PASS 2: Resolve MSAA + Auto-Exposure + HDR Bloom Pyramid + Composite
    // ========================================================================
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFBO);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    // Real frame delta time for smooth eye-adaptation (unaffected by bullet-time)
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    float realAdaptDt = 0.016f;
    if (m_lastAdaptTimeNs > 0) {
        realAdaptDt = std::clamp(static_cast<float>(nowNs - m_lastAdaptTimeNs) * 1e-9f, 0.001f, 0.1f);
    }
    m_lastAdaptTimeNs = nowNs;

    // Generate mip chain for fast GPU log-average luminance sampling
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
    glGenerateMipmap(GL_TEXTURE_2D);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(m_skyVAO);

    // 2.1. 1x1 GPU Auto-Exposure (kept warm in both media so breaching has natural iris adaptation)
    const int nextExpIdx = 1 - m_exposureIdx;
    const float maxMip = std::floor(std::log2(static_cast<float>(std::max(physW, physH))));
    const float lumaMip = std::max(0.0f, maxMip - 4.0f);

    glBindFramebuffer(GL_FRAMEBUFFER, m_exposureFBO[nextExpIdx]);
    glViewport(0, 0, 1, 1);
    m_autoExposureShader.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_exposureTex[m_exposureIdx]);
    m_autoExposureShader.setUniformValue("uSceneColorTex", 0);
    m_autoExposureShader.setUniformValue("uPrevExposureTex", 1);
    m_autoExposureShader.setUniformValue("uLumaMip", lumaMip);
    m_autoExposureShader.setUniformValue("uRealDt", realAdaptDt);
    m_autoExposureShader.setUniformValue("uCamAboveWater", camAboveInt);
    m_autoExposureShader.setUniformValue("uResetExposure", m_exposureInitialized ? 0 : 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    m_exposureIdx = nextExpIdx;
    m_exposureInitialized = true;

    // 2.2. Bloom: above water -> 5-cascade Karis HDR pyramid; underwater -> original scatter-blur
    if (m_postProcessEnabled) {
        if (camAboveWater) {
            m_hdrBloomDownShader.bind();
            m_hdrBloomDownShader.setUniformValue("uSourceTex", 0);
            m_hdrBloomDownShader.setUniformValue("uThreshold", 1.20f);
            m_hdrBloomDownShader.setUniformValue("uKnee", 0.45f);

            GLuint srcTex = m_sceneColorTex;
            int srcW = physW;
            int srcH = physH;
            for (int i = 0; i < HDR_BLOOM_MIPS; ++i) {
                glBindFramebuffer(GL_FRAMEBUFFER, m_hdrBloomFBO[i]);
                glViewport(0, 0, m_hdrBloomW[i], m_hdrBloomH[i]);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, srcTex);
                m_hdrBloomDownShader.setUniformValue("uTexelSize", QVector2D(1.0f / srcW, 1.0f / srcH));
                m_hdrBloomDownShader.setUniformValue("uFirstPass", (i == 0) ? 1 : 0);
                glDrawArrays(GL_TRIANGLES, 0, 6);

                srcTex = m_hdrBloomTex[i];
                srcW = m_hdrBloomW[i];
                srcH = m_hdrBloomH[i];
            }

            // Additive tent upsample back up the pyramid
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            m_hdrBloomUpShader.bind();
            m_hdrBloomUpShader.setUniformValue("uSourceTex", 0);
            m_hdrBloomUpShader.setUniformValue("uFilterRadius", 1.35f);

            for (int i = HDR_BLOOM_MIPS - 1; i > 0; --i) {
                glBindFramebuffer(GL_FRAMEBUFFER, m_hdrBloomFBO[i - 1]);
                glViewport(0, 0, m_hdrBloomW[i - 1], m_hdrBloomH[i - 1]);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_hdrBloomTex[i]);
                m_hdrBloomUpShader.setUniformValue("uTexelSize",
                                                   QVector2D(1.0f / m_hdrBloomW[i], 1.0f / m_hdrBloomH[i]));
                m_hdrBloomUpShader.setUniformValue("uLevelWeight", 0.78f);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            glDisable(GL_BLEND);
        } else {
            const int halfW = std::max(32, physW / 2);
            const int halfH = std::max(32, physH / 2);
            glViewport(0, 0, halfW, halfH);

            m_scatterBlurShader.bind();
            m_scatterBlurShader.setUniformValue("uSourceTex", 0);

            for (int iter = 0; iter < 2; ++iter) {
                glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[0]);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, (iter == 0) ? m_sceneColorTex : m_bloomTex[1]);
                m_scatterBlurShader.setUniformValue("uTexelStep", QVector2D(1.0f / halfW, 0.0f));
                m_scatterBlurShader.setUniformValue("uExtractBright", (iter == 0) ? 1 : 0);
                glDrawArrays(GL_TRIANGLES, 0, 6);

                glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[1]);
                glBindTexture(GL_TEXTURE_2D, m_bloomTex[0]);
                m_scatterBlurShader.setUniformValue("uTexelStep", QVector2D(0.0f, 1.0f / halfH));
                m_scatterBlurShader.setUniformValue("uExtractBright", 0);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, physW, physH);

    const QVector4D sunClip = viewProj * QVector4D(camPos + visuals.sunDir * 100.0f, 1.0f);
    QVector3D sunScreen(0.5f, 0.85f, 0.0f);
    if (sunClip.w() > 0.001f) {
        const QVector2D ndc(sunClip.x() / sunClip.w(), sunClip.y() / sunClip.w());
        sunScreen.setX(ndc.x() * 0.5f + 0.5f);
        sunScreen.setY(ndc.y() * 0.5f + 0.5f);
        const float facing = QVector3D::dotProduct(camForward, visuals.sunDir);
        sunScreen.setZ(ProcMath::smoothstep(-0.15f, 0.65f, facing));
    }

    const float distToSurface = std::max(0.4f, ProcMath::WATER_SURFACE_Y - camPos.y());
    const float surfaceProximity = 1.0f - ProcMath::smoothstep(1.8f, 16.0f, distToSurface);

    m_postProcessShader.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, camAboveWater ? m_hdrBloomTex[0] : m_bloomTex[1]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_rockTex);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_exposureTex[m_exposureIdx]);

    m_postProcessShader.setUniformValue("uSceneColorTex", 0);
    m_postProcessShader.setUniformValue("uSceneDepthTex", 1);
    m_postProcessShader.setUniformValue("uBloomTex", 2);
    m_postProcessShader.setUniformValue("uRockTex", 3);
    m_postProcessShader.setUniformValue("uExposureTex", 4);
    m_postProcessShader.setUniformValue("uInvResolution", QVector2D(1.0f / physW, 1.0f / physH));
    m_postProcessShader.setUniformValue("uSunScreenPos", sunScreen);
    m_postProcessShader.setUniformValue("uWaterTint", visuals.waterShallow);
    m_postProcessShader.setUniformValue("uSurfaceProximity", surfaceProximity);
    m_postProcessShader.setUniformValue("uTime", simTime);
    m_postProcessShader.setUniformValue("uEnablePost", m_postProcessEnabled ? 1 : 0);
    m_postProcessShader.setUniformValue("uCamAboveWater", camAboveInt);
    m_postProcessShader.setUniformValue("uSplashAmount", engine.splashScreenAmount());
    m_postProcessShader.setUniformValue("uLensWetness", engine.lensWetness());
    m_postProcessShader.setUniformValue("uBulletTime", engine.bulletTimeFactor());
    m_postProcessShader.setUniformValue("uFarPlane", zFar);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    m_postProcessShader.release();
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
}
