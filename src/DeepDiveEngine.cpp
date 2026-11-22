#include "DeepDiveEngine.h"
#include "Noise.h"
#include "Shaders.h"
#include <QVector2D>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

inline float wrapAngle(float a) {
    while (a > PI)  a -= TWO_PI;
    while (a < -PI) a += TWO_PI;
    return a;
}

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

// LOD transition thresholds for each of the 8 instanced prop archetypes (LOD0 -> LOD1 -> Cull)
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
    {235.0f, 430.0f, 10.0f, true,  105.0f, 155.0f},
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
    {120.0f, 230.0f,  8.0f, false,   0.0f,   0.0f}
}};

constexpr const char* BIOME_NAMES[4] = {
    "Tropical Reef (Vibrant)",
    "Kelp Reef (Emerald)",
    "Purple Grotto",
    "Abyssal Trench"
};
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
// DEEP DIVE ENGINE — LIFECYCLE & GPU RESOURCES
// ============================================================================

DeepDiveEngine::DeepDiveEngine() {
    std::random_device rd;
    const auto timeSeed = static_cast<uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    m_rng.seed(rd() ^ timeSeed);
}

void DeepDiveEngine::initializeGLContext() {
    initializeOpenGLFunctions();
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_PROGRAM_POINT_SIZE);
}

void DeepDiveEngine::initBaseResources(const std::vector<AdapterCapability>& adapters) {
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
}

void DeepDiveEngine::compileShadowShaders() {
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

void DeepDiveEngine::compileEnvironmentShaders() {
    m_envShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::EnvironmentVert);
    m_envShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::EnvironmentFrag);
    m_envShader.link();

    m_instPropShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::InstPropVert);
    m_instPropShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::EnvironmentFrag);
    m_instPropShader.link();

    m_faunaShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::FaunaVert);
    m_faunaShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::FaunaFrag);
    m_faunaShader.link();
}

void DeepDiveEngine::compileWaterAndLightShaders() {
    m_waterShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::WaterSurfaceVert);
    m_waterShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::WaterSurfaceFrag);
    m_waterShader.link();

    m_skyShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::SkyboxVert);
    m_skyShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::SkyboxFrag);
    m_skyShader.link();

    m_rayShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::TranslucentVert);
    m_rayShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::TranslucentFrag);
    m_rayShader.link();
}

void DeepDiveEngine::compilePostProcessShaders() {
    m_particleShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ParticleVert);
    m_particleShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ParticleFrag);
    m_particleShader.link();

    m_scatterBlurShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_scatterBlurShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ScatterBlurFrag);
    m_scatterBlurShader.link();

    m_postProcessShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_postProcessShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::PostProcessFrag);
    m_postProcessShader.link();
}

void DeepDiveEngine::initRenderBuffers(int physW, int physH) {
    createWaterSurfaceMesh();
    resizePostProcessBuffers(physW, physH);
}

void DeepDiveEngine::refreshGpuCapabilities(const std::vector<AdapterCapability>& adapters) {
    queryOpenGLCapabilities(adapters);
}

void DeepDiveEngine::cleanup() {
    destroyPostProcessBuffers();

    if (m_shadowFBO) { glDeleteFramebuffers(1, &m_shadowFBO); m_shadowFBO = 0; }
    if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex); m_shadowDepthTex = 0; }
    if (m_rockTex) { glDeleteTextures(1, &m_rockTex); m_rockTex = 0; }

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
}

void DeepDiveEngine::queryOpenGLCapabilities(const std::vector<AdapterCapability>& adapters) {
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

void DeepDiveEngine::createShadowMapBuffer() {
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

void DeepDiveEngine::createWaterSurfaceMesh() {
    constexpr int RINGS = 72;
    constexpr int SECTORS = 96;
    constexpr float RADIUS = 455.0f;

    std::vector<QVector2D> verts;
    verts.reserve(1 + RINGS * SECTORS);
    verts.emplace_back(0.0f, 0.0f);

    for (int r = 1; r <= RINGS; ++r) {
        const float t = static_cast<float>(r) / static_cast<float>(RINGS);
        const float rad = RADIUS * std::pow(t, 2.2f);
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

std::vector<uint8_t> DeepDiveEngine::generateRockTextureData(int size) {
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

                    // Smooth seamless domain-warp breaking Voronoi cell regularity
                    const float wU = (ProcMath::tileableNoise2D(u, v, 4, 113u) - 0.5f) * 0.14f
                                     + (ProcMath::tileableNoise2D(u, v, 8, 127u) - 0.5f) * 0.06f;
                    const float wV = (ProcMath::tileableNoise2D(u, v, 4, 131u) - 0.5f) * 0.14f
                                     + (ProcMath::tileableNoise2D(u, v, 8, 137u) - 0.5f) * 0.06f;
                    const float wu = ProcMath::fract(u + wU + 1.0f);
                    const float wv = ProcMath::fract(v + wV + 1.0f);

                    // Multi-octave rock roughness
                    float fbmVal = 0.0f;
                    fbmVal += 0.30f * ProcMath::tileableNoise2D(wu, wv, 8,   101u);
                    fbmVal += 0.24f * ProcMath::tileableNoise2D(wu, wv, 16,  202u);
                    fbmVal += 0.18f * ProcMath::tileableNoise2D(u,  v,  32,  303u);
                    fbmVal += 0.13f * ProcMath::tileableNoise2D(u,  v,  64,  404u);
                    fbmVal += 0.09f * ProcMath::tileableNoise2D(u,  v,  128, 505u);
                    fbmVal += 0.06f * ProcMath::tileableNoise2D(u,  v,  256, 606u);

                    // Sedimentary strata bands
                    float strata = 0.5f + 0.5f * std::sin((wv * 16.0f + fbmVal * 2.4f) * TWO_PI * 0.25f);
                    strata = ProcMath::smoothstep(0.15f, 0.85f, strata);

                    // Warped geological cracks
                    const float vor1 = ProcMath::tileableVoronoi2D(wu, wv, 4,  707u);
                    const float vor2 = ProcMath::tileableVoronoi2D(wu, wv, 12, 808u);
                    const float vor3 = ProcMath::tileableVoronoi2D(u,  v,  32, 909u);
                    const float crackMain = ProcMath::smoothstep(0.015f, 0.14f, vor1);
                    const float crackMed  = ProcMath::smoothstep(0.015f, 0.16f, vor2);
                    const float crackFine = ProcMath::smoothstep(0.02f,  0.18f, vor3);

                    const float cavity = ProcMath::clamp(crackMain * 0.52f + crackMed * 0.30f + crackFine * 0.18f, 0.0f, 1.0f);
                    const float h = ProcMath::clamp(fbmVal * 0.54f + strata * 0.18f + cavity * 0.28f, 0.0f, 1.0f);

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

    // 3x3 Sobel operator for crisp artifact-free normals
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

void DeepDiveEngine::uploadRockTexture(const std::vector<uint8_t>& rgba, int size) {
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

void DeepDiveEngine::destroyPostProcessBuffers() {
    if (m_msaaFBO) glDeleteFramebuffers(1, &m_msaaFBO);
    if (m_msaaColorRBO) glDeleteRenderbuffers(1, &m_msaaColorRBO);
    if (m_msaaDepthRBO) glDeleteRenderbuffers(1, &m_msaaDepthRBO);

    if (m_resolveFBO) glDeleteFramebuffers(1, &m_resolveFBO);
    if (m_sceneColorTex) glDeleteTextures(1, &m_sceneColorTex);
    if (m_sceneDepthTex) glDeleteTextures(1, &m_sceneDepthTex);

    if (m_bloomFBO[0]) glDeleteFramebuffers(2, m_bloomFBO);
    if (m_bloomTex[0]) glDeleteTextures(2, m_bloomTex);

    m_msaaFBO = m_msaaColorRBO = m_msaaDepthRBO = 0;
    m_resolveFBO = m_sceneColorTex = m_sceneDepthTex = 0;
    m_bloomFBO[0] = m_bloomFBO[1] = m_bloomTex[0] = m_bloomTex[1] = 0;
    m_fbWidth = m_fbHeight = 0;
}

void DeepDiveEngine::resize(int physW, int physH) {
    resizePostProcessBuffers(physW, physH);
}

void DeepDiveEngine::resizePostProcessBuffers(int physW, int physH) {
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneColorTex, 0);

    glGenTextures(1, &m_sceneDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, physW, physH, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_sceneDepthTex, 0);

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

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
}

void DeepDiveEngine::clearScreen(float r, float g, float b) {
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void DeepDiveEngine::setupFaunaLodBuffers(FaunaLodBuffers& buf,
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
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(FaunaVertex), verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t), inds.data(), GL_STATIC_DRAW);

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

void DeepDiveEngine::deleteFaunaLodBuffers(FaunaLodBuffers& buf) {
    if (buf.vao) glDeleteVertexArrays(1, &buf.vao);
    if (buf.vbo) glDeleteBuffers(1, &buf.vbo);
    if (buf.ebo) glDeleteBuffers(1, &buf.ebo);
    if (buf.instVbo) glDeleteBuffers(1, &buf.instVbo);
    buf = {};
}

void DeepDiveEngine::setupPropLodBuffers(PropLodBuffers& buf,
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

void DeepDiveEngine::ensurePropInstCapacity(PropLodBuffers& buf, size_t needed) {
    if (needed <= static_cast<size_t>(buf.maxInstances)) return;
    const size_t newCap = std::max(needed + needed / 2, static_cast<size_t>(512));
    buf.maxInstances = static_cast<GLsizei>(newCap);
    glBindBuffer(GL_ARRAY_BUFFER, buf.instVbo);
    glBufferData(GL_ARRAY_BUFFER, newCap * sizeof(PropGpuInstance), nullptr, GL_DYNAMIC_DRAW);
}

void DeepDiveEngine::deletePropLodBuffers(PropLodBuffers& buf) {
    if (buf.vao) glDeleteVertexArrays(1, &buf.vao);
    if (buf.vbo) glDeleteBuffers(1, &buf.vbo);
    if (buf.ebo) glDeleteBuffers(1, &buf.ebo);
    if (buf.instVbo) glDeleteBuffers(1, &buf.instVbo);
    buf = {};
}

void DeepDiveEngine::uploadWorld(WorldData&& world, uint32_t seed) {
    m_seed = seed;
    m_world = std::move(world);
    m_chunkLodStates.assign(m_world.chunks.size(), ChunkLodState{});
    m_fishLodState.assign(m_world.fishAgents.size(), 0);
    m_mantaLodState.assign(m_world.mantaAgents.size(), 0);
    m_anglerLodState.assign(m_world.anglerAgents.size(), 0);
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
                m_world.terrainVertices, m_world.terrainIndices);
    setupEnvVAO(m_propVAO, m_propVBO, m_propEBO,
                m_world.propVertices, m_world.propIndices);

    for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
        const auto& proto = m_world.propPrototypes[a];
        setupPropLodBuffers(m_propLod0[a], proto.lod0Verts, proto.lod0Inds, 2048);
        setupPropLodBuffers(m_propLod1[a], proto.lod1Verts, proto.lod1Inds, 4096);
        m_visPropsLod0[a].reserve(2048);
        m_visPropsLod1[a].reserve(4096);
    }

    setupFaunaLodBuffers(m_fishLod0,   m_world.fishLod0Verts,   m_world.fishLod0Inds,   m_world.fishAgents.size());
    setupFaunaLodBuffers(m_fishLod1,   m_world.fishLod1Verts,   m_world.fishLod1Inds,   m_world.fishAgents.size());
    setupFaunaLodBuffers(m_mantaLod0,  m_world.mantaLod0Verts,  m_world.mantaLod0Inds,  m_world.mantaAgents.size());
    setupFaunaLodBuffers(m_mantaLod1,  m_world.mantaLod1Verts,  m_world.mantaLod1Inds,  m_world.mantaAgents.size());
    setupFaunaLodBuffers(m_whaleLod0,  m_world.whaleLod0Verts,  m_world.whaleLod0Inds,  1);
    setupFaunaLodBuffers(m_whaleLod1,  m_world.whaleLod1Verts,  m_world.whaleLod1Inds,  1);
    setupFaunaLodBuffers(m_anglerLod0, m_world.anglerLod0Verts, m_world.anglerLod0Inds, m_world.anglerAgents.size());
    setupFaunaLodBuffers(m_anglerLod1, m_world.anglerLod1Verts, m_world.anglerLod1Inds, m_world.anglerAgents.size());

    m_visFishLod0.reserve(m_world.fishAgents.size());
    m_visFishLod1.reserve(m_world.fishAgents.size());
    m_visMantaLod0.reserve(m_world.mantaAgents.size());
    m_visMantaLod1.reserve(m_world.mantaAgents.size());
    m_visWhaleLod0.reserve(1);
    m_visWhaleLod1.reserve(1);
    m_visAnglerLod0.reserve(m_world.anglerAgents.size());
    m_visAnglerLod1.reserve(m_world.anglerAgents.size());

    m_rayIndexCount    = static_cast<GLsizei>(m_world.godRayMeshInds.size());
    m_rayInstanceCount = static_cast<GLsizei>(m_world.godRayInstances.size());

    glBindVertexArray(m_rayVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_rayVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 m_world.godRayMeshVerts.size() * sizeof(QVector3D),
                 m_world.godRayMeshVerts.empty() ? nullptr : m_world.godRayMeshVerts.data(),
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_rayEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 m_world.godRayMeshInds.size() * sizeof(uint32_t),
                 m_world.godRayMeshInds.empty() ? nullptr : m_world.godRayMeshInds.data(),
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(QVector3D), nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m_rayInstVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 m_world.godRayInstances.size() * sizeof(GodRayInstance),
                 m_world.godRayInstances.empty() ? nullptr : m_world.godRayInstances.data(),
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
    glBufferData(GL_ARRAY_BUFFER, m_world.particles.size() * sizeof(Particle),
                 m_world.particles.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Particle),
                          reinterpret_cast<void*>(offsetof(Particle, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Particle),
                          reinterpret_cast<void*>(offsetof(Particle, colorSize)));
    glBindVertexArray(0);

    m_world.collision.resolveSphereCollision(m_camPos, &m_camVel, 1.45f, 0.5f, 1.8f);
    spawnWhalePass(true);
}

// ============================================================================
// RUNTIME CONTROLS & QUERIES
// ============================================================================

void DeepDiveEngine::cycleBiome() {
    m_paletteIndex = (m_paletteIndex + 1) % 4;
}

void DeepDiveEngine::togglePostProcess() {
    m_postProcessEnabled = !m_postProcessEnabled;
}

void DeepDiveEngine::toggleShadows() {
    m_shadowsEnabled = !m_shadowsEnabled;
}

void DeepDiveEngine::toggleWireframe() {
    m_wireframeMode = !m_wireframeMode;
}

void DeepDiveEngine::summonWhaleNearPlayer() {
    spawnWhalePass(true);
}

float DeepDiveEngine::diveDepth() const {
    return std::max(0.0f, ProcMath::WATER_SURFACE_Y - m_camPos.y());
}

float DeepDiveEngine::radialDistance() const {
    return std::sqrt(m_camPos.x() * m_camPos.x() + m_camPos.z() * m_camPos.z());
}

const char* DeepDiveEngine::biomeName() const {
    return BIOME_NAMES[m_paletteIndex];
}

QVector3D DeepDiveEngine::cameraForward() const {
    return QVector3D(
               std::sin(m_yaw) * std::cos(m_pitch),
               std::sin(m_pitch),
               -std::cos(m_yaw) * std::cos(m_pitch)
               ).normalized();
}

QVector3D DeepDiveEngine::cameraRight() const {
    return QVector3D(std::cos(m_yaw), 0.0f, std::sin(m_yaw)).normalized();
}

// ============================================================================
// BATCHING & CULLING
// ============================================================================

void DeepDiveEngine::gatherAndUploadInstProps(const std::vector<std::pair<int, float>>& chunks, bool forShadow) {
    for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
        m_visPropsLod0[a].clear();
        m_visPropsLod1[a].clear();
    }

    for (const auto& [idx, d] : chunks) {
        const auto& chunk = m_world.chunks[idx];
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

void DeepDiveEngine::drawInstPropBatches(bool forShadow, bool countTris) {
    auto drawOne = [&](const PropLodBuffers& buf, size_t count) {
        if (count == 0 || buf.indexCount == 0) return;
        const GLsizei clamped = static_cast<GLsizei>(std::min(count, static_cast<size_t>(buf.maxInstances)));
        glBindVertexArray(buf.vao);
        glDrawElementsInstanced(GL_TRIANGLES, buf.indexCount, GL_UNSIGNED_INT, nullptr, clamped);
        if (countTris) {
            m_stats.drawnTriangles += (buf.indexCount / 3) * clamped;
            m_stats.visibleInstProps += clamped;
        }
    };

    for (int a = 0; a < PROP_ARCHETYPE_COUNT; ++a) {
        if (forShadow && !PROP_LOD_CFG[a].castShadow) continue;
        drawOne(m_propLod0[a], m_visPropsLod0[a].size());
        drawOne(m_propLod1[a], m_visPropsLod1[a].size());
    }
}

void DeepDiveEngine::cullAndUploadFauna(const FrustumPlanes& frustum) {
    m_visFishLod0.clear();
    m_visFishLod1.clear();
    m_visMantaLod0.clear();
    m_visMantaLod1.clear();
    m_visWhaleLod0.clear();
    m_visWhaleLod1.clear();
    m_visAnglerLod0.clear();
    m_visAnglerLod1.clear();

    constexpr float FISH_LOD0_DIST   = 115.0f;
    constexpr float FISH_MAX_DIST    = 240.0f;
    constexpr float MANTA_LOD0_DIST  = 200.0f;
    constexpr float MANTA_MAX_DIST   = 350.0f;
    constexpr float WHALE_LOD0_DIST  = 330.0f;
    constexpr float WHALE_MAX_DIST   = 460.0f;
    constexpr float ANGLER_LOD0_DIST = 165.0f;
    constexpr float ANGLER_MAX_DIST  = 310.0f;

    for (size_t i = 0; i < m_world.fishAgents.size(); ++i) {
        const auto& f = m_world.fishAgents[i];
        const float dist = (f.pos - m_camPos).length();
        if (dist > FISH_MAX_DIST) continue;
        if (!frustum.intersectsSphere(f.pos, f.boundingRadius * 1.4f)) continue;

        uint8_t& st = m_fishLodState[i];
        st = updateBinaryLod(st, dist, FISH_LOD0_DIST, 6.0f);
        if (st == 0) m_visFishLod0.push_back(packFaunaInstance(f));
        else         m_visFishLod1.push_back(packFaunaInstance(f));
    }

    for (size_t i = 0; i < m_world.mantaAgents.size(); ++i) {
        const auto& m = m_world.mantaAgents[i];
        const float dist = (m.pos - m_camPos).length();
        if (dist > MANTA_MAX_DIST) continue;
        if (!frustum.intersectsSphere(m.pos, m.boundingRadius * 1.6f)) continue;

        uint8_t& st = m_mantaLodState[i];
        st = updateBinaryLod(st, dist, MANTA_LOD0_DIST, 8.0f);
        if (st == 0) m_visMantaLod0.push_back(packFaunaInstance(m));
        else         m_visMantaLod1.push_back(packFaunaInstance(m));
    }

    {
        const float whaleDist = (m_whale.pos - m_camPos).length();
        if (whaleDist <= WHALE_MAX_DIST && frustum.intersectsSphere(m_whale.pos, m_whale.boundingRadius * 1.25f)) {
            m_whaleLodState = updateBinaryLod(m_whaleLodState, whaleDist, WHALE_LOD0_DIST, 12.0f);
            if (m_whaleLodState == 0) m_visWhaleLod0.push_back(packFaunaInstance(m_whale));
            else                      m_visWhaleLod1.push_back(packFaunaInstance(m_whale));
        }
    }

    for (size_t i = 0; i < m_world.anglerAgents.size(); ++i) {
        const auto& a = m_world.anglerAgents[i];
        const float dist = (a.pos - m_camPos).length();
        if (dist > ANGLER_MAX_DIST) continue;
        if (!frustum.intersectsSphere(a.pos, a.boundingRadius * 2.0f)) continue;

        uint8_t& st = m_anglerLodState[i];
        st = updateBinaryLod(st, dist, ANGLER_LOD0_DIST, 8.0f);
        if (st == 0) m_visAnglerLod0.push_back(packFaunaInstance(a));
        else         m_visAnglerLod1.push_back(packFaunaInstance(a));
    }

    auto upload = [&](FaunaLodBuffers& buf, const std::vector<FaunaGpuInstance>& list) {
        if (list.empty() || buf.instVbo == 0) return;
        const size_t count = std::min(list.size(), static_cast<size_t>(buf.maxInstances));
        glBindBuffer(GL_ARRAY_BUFFER, buf.instVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(FaunaGpuInstance), list.data());
    };

    upload(m_fishLod0,   m_visFishLod0);
    upload(m_fishLod1,   m_visFishLod1);
    upload(m_mantaLod0,  m_visMantaLod0);
    upload(m_mantaLod1,  m_visMantaLod1);
    upload(m_whaleLod0,  m_visWhaleLod0);
    upload(m_whaleLod1,  m_visWhaleLod1);
    upload(m_anglerLod0, m_visAnglerLod0);
    upload(m_anglerLod1, m_visAnglerLod1);

    m_stats.visibleFishLod0  = static_cast<int>(m_visFishLod0.size());
    m_stats.visibleFishLod1  = static_cast<int>(m_visFishLod1.size());
    m_stats.visibleMantaLod0 = static_cast<int>(m_visMantaLod0.size());
    m_stats.visibleMantaLod1 = static_cast<int>(m_visMantaLod1.size());
    m_stats.visibleAnglers   = static_cast<int>(m_visAnglerLod0.size() + m_visAnglerLod1.size());
}

// ============================================================================
// HUMPBACK WHALE NAVIGATION
// ============================================================================

float DeepDiveEngine::sampleWhaleClearance(float x, float z) const {
    const float sc           = std::max(m_whale.scale, 2.65f);
    const float bodyBot      = 1.12f * sc;
    const float skimCeilingY = ProcMath::WATER_SURFACE_Y - 0.25f * sc;
    const float maxPassableY = skimCeilingY - bodyBot - 0.6f;
    const float trunkR       = 1.25f * sc;

    const float r = std::sqrt(x * x + z * z);
    const float wallF = ProcMath::sampleBasinWallFactor(x, z, m_seed);
    const float safeOuterR = ProcMath::BASIN_INNER_RADIUS - (28.0f + trunkR);
    if (wallF > 0.08f || r > safeOuterR) {
        return -50.0f - std::max(0.0f, r - safeOuterR) - wallF * 100.0f;
    }

    const auto& col = m_world.collision;
    float maxObsY = std::max({
        col.sampleHeight(x, z),
        col.sampleHeight(x + trunkR, z),
        col.sampleHeight(x - trunkR, z),
        col.sampleHeight(x, z + trunkR),
        col.sampleHeight(x, z - trunkR)
    });

    if (!col.cellSpheres.empty()) {
        const int cx = std::clamp(static_cast<int>(std::floor((x + col.worldHalfSize) / col.chunkSize)),
                                  0, col.chunksPerSide - 1);
        const int cz = std::clamp(static_cast<int>(std::floor((z + col.worldHalfSize) / col.chunkSize)),
                                  0, col.chunksPerSide - 1);
        for (uint32_t id : col.cellSpheres[cz * col.chunksPerSide + cx]) {
            const auto& sph = col.spheres[id];
            const float topY = sph.center.y() + sph.radius;
            if (topY <= maxObsY) continue;
            const float dx = x - sph.center.x();
            const float dz = z - sph.center.z();
            const float rSum = sph.radius + trunkR * 0.85f;
            if (dx * dx + dz * dz < rSum * rSum) {
                maxObsY = topY;
            }
        }
    }

    return maxPassableY - maxObsY;
}

float DeepDiveEngine::measureWhaleFreeDist(float x, float z, float yaw, float maxDist) const {
    const float sx = std::sin(yaw);
    const float sz = std::cos(yaw);
    constexpr float STEP = 6.0f;
    for (float d = STEP; d <= maxDist; d += STEP) {
        if (sampleWhaleClearance(x + sx * d, z + sz * d) <= 0.0f) {
            return d - STEP;
        }
    }
    return maxDist;
}

QVector3D DeepDiveEngine::pickWhaleOpenWaypoint(const QVector3D& nearAnchor, float minDist, float maxDist) const {
    const float sc                = std::max(m_whale.scale, 2.65f);
    const float submergedCeilingY = ProcMath::WATER_SURFACE_Y - (0.85f * sc + 0.9f);
    const float cruiseY           = submergedCeilingY - 0.8f;
    const float maxPlayR          = ProcMath::BASIN_INNER_RADIUS - (65.0f + 2.5f * sc);

    QVector3D bestPos(0.0f, cruiseY, 0.0f);
    float bestScore = -1e9f;

    const float baseAng = std::fmod(m_time * 0.37f + m_whale.yaw + 0.5f, TWO_PI);
    for (int i = 0; i < 28; ++i) {
        const float h1 = ProcMath::hash3(i, static_cast<int>(m_time * 3.0f), 11, m_seed + 4101u);
        const float h2 = ProcMath::hash3(i, static_cast<int>(m_time * 3.0f), 19, m_seed + 4101u);
        const float ang = baseAng + (TWO_PI * static_cast<float>(i)) / 28.0f + (h1 - 0.5f) * 0.25f;
        const float dist = ProcMath::mix(minDist, maxDist, h2);

        float cx = nearAnchor.x() + std::cos(ang) * dist;
        float cz = nearAnchor.z() + std::sin(ang) * dist;

        const float rad = std::sqrt(cx * cx + cz * cz);
        if (rad > maxPlayR) {
            const float k = maxPlayR / std::max(rad, 1.0f);
            cx *= k;
            cz *= k;
        }

        const float cl = sampleWhaleClearance(cx, cz);
        const float dirYaw = std::atan2(cx - m_whale.pos.x(), cz - m_whale.pos.z());
        const float freeD = measureWhaleFreeDist(m_whale.pos.x(), m_whale.pos.z(), dirYaw, std::min(dist, 120.0f));
        const float score = freeD * 1.4f + std::clamp(cl, -20.0f, 18.0f) * 2.5f;

        if (score > bestScore) {
            bestScore = score;
            bestPos = QVector3D(cx, cruiseY, cz);
        }
    }
    return bestPos;
}

void DeepDiveEngine::spawnWhalePass(bool nearPlayer) {
    constexpr float BASE_WHALE_SCALE = 2.65f;
    std::uniform_real_distribution<float> sizeDist(1.0f, 2.0f);
    std::uniform_real_distribution<float> phaseDist(0.0f, TWO_PI);
    std::uniform_real_distribution<float> firstSurfCdDist(5.0f, 9.0f);

    const float sizeMult = sizeDist(m_rng);

    m_whale.type           = 2.0f;
    m_whale.scale          = BASE_WHALE_SCALE * sizeMult;
    m_whale.boundingRadius = 6.8f * m_whale.scale;
    m_whale.cruiseSpeed    = 5.0f + 0.85f * (sizeMult - 1.0f);

    const float sc                = m_whale.scale;
    const float bodyBot           = 1.12f * sc;
    const float submergedCeilingY = ProcMath::WATER_SURFACE_Y - (0.85f * sc + 0.9f);
    const float skimCeilingY      = ProcMath::WATER_SURFACE_Y - 0.25f * sc;
    const float cruiseY           = submergedCeilingY - 0.8f;

    m_whale.preferredDepth = cruiseY;
    m_whale.homeCenter     = QVector3D(0.0f, cruiseY, 0.0f);
    m_whale.homeRadius     = ProcMath::BASIN_INNER_RADIUS - 80.0f;
    m_whale.tint           = QVector3D(1.0f, 1.0f, 1.0f);
    m_whale.phase          = phaseDist(m_rng);

    auto pickSafeY = [&](float x, float z) {
        const float floorY = m_world.collision.sampleHeight(x, z);
        return ProcMath::clamp(std::max(cruiseY, floorY + bodyBot + 1.2f), 10.0f, skimCeilingY);
    };

    QVector3D bestPos(0.0f, cruiseY, -55.0f);
    float bestYaw = 0.2f;
    float bestScore = -1e9f;

    auto evalCandidate = [&](float cx, float cz, float candYaw, float distPenalty) {
        const float c0 = sampleWhaleClearance(cx, cz);
        const float cBack = sampleWhaleClearance(cx - std::sin(candYaw) * (4.5f * sc),
                                                 cz - std::cos(candYaw) * (4.5f * sc));
        const float freeAhead = measureWhaleFreeDist(cx, cz, candYaw, 110.0f);
        const float minCl = std::min(c0, cBack);
        const float score = (minCl > 0.0f ? (25.0f + std::min(minCl, 16.0f) * 2.0f) : (minCl * 4.0f))
                            + freeAhead * 1.15f - distPenalty;
        if (score > bestScore) {
            bestScore = score;
            bestPos = QVector3D(cx, pickSafeY(cx, cz), cz);
            bestYaw = candYaw;
        }
    };

    if (nearPlayer) {
        const QVector3D camFwd(std::sin(m_yaw), 0.0f, -std::cos(m_yaw));
        const QVector3D camRgt(std::cos(m_yaw), 0.0f,  std::sin(m_yaw));
        const float sideSign = (sizeDist(m_rng) > 1.5f) ? 1.0f : -1.0f;

        const float fwdDists[4] = {36.0f + 3.0f * sc, 52.0f + 3.5f * sc, 72.0f + 4.0f * sc, 28.0f + 2.5f * sc};
        const float sideOffs[3] = {-24.0f * sideSign, 0.0f, 24.0f * sideSign};
        const float yawRel[4]   = {1.45f * sideSign, 1.10f * sideSign, -1.45f * sideSign, -1.10f * sideSign};
        const float camHeading  = std::atan2(camFwd.x(), camFwd.z());

        for (float fd : fwdDists) {
            for (float so : sideOffs) {
                const float cx = m_camPos.x() + camFwd.x() * fd + camRgt.x() * so;
                const float cz = m_camPos.z() + camFwd.z() * fd + camRgt.z() * so;
                for (float yr : yawRel) {
                    evalCandidate(cx, cz, wrapAngle(camHeading + yr), fd * 0.25f);
                }
            }
        }

        if (bestScore < 55.0f) {
            for (int i = 0; i < 24; ++i) {
                const float ang = (TWO_PI * static_cast<float>(i)) / 24.0f;
                const float rad = 45.0f + 4.0f * sc;
                const float cx = m_camPos.x() + std::cos(ang) * rad;
                const float cz = m_camPos.z() + std::sin(ang) * rad;
                const float tangYaw = std::atan2(-std::sin(ang), std::cos(ang));
                evalCandidate(cx, cz, tangYaw, 15.0f);
                evalCandidate(cx, cz, wrapAngle(tangYaw + PI), 15.0f);
            }
        }
    }

    if (!nearPlayer || bestScore < 30.0f) {
        bestScore = -1e9f;
        const float baseAng = phaseDist(m_rng);
        for (int i = 0; i < 32; ++i) {
            const float ang = baseAng + (TWO_PI * static_cast<float>(i)) / 32.0f;
            const float rad = 70.0f + 35.0f * (i % 3);
            const float cx = std::cos(ang) * rad;
            const float cz = std::sin(ang) * rad;
            const float tangYaw = std::atan2(-std::sin(ang), std::cos(ang));
            evalCandidate(cx, cz, tangYaw, 0.0f);
        }
    }

    const QVector3D dir(std::sin(bestYaw), 0.0f, std::cos(bestYaw));
    m_whale.pos   = bestPos;
    m_whale.yaw   = bestYaw;
    m_whale.pitch = 0.0f;
    m_whale.bank  = 0.0f;
    m_whale.vel   = dir * m_whale.cruiseSpeed;

    m_whaleState          = WhaleState::Cruising;
    m_whaleTurnDir        = (sizeMult > 1.5f) ? 1.0f : -1.0f;
    m_whaleAvoidTimer     = 0.0f;
    m_whaleAvoidAccumYaw  = 0.0f;
    m_whaleCurrentYawRate = 0.0f;
    m_whaleProgressAnchor = m_whale.pos;
    m_whaleProgressTimer  = 0.0f;
    m_whaleStuckAccum     = 0.0f;

    m_whaleSurfacing       = false;
    m_whaleSurfaceBlend    = 0.0f;
    m_whaleSurfaceTimer    = 0.0f;
    m_whaleSurfaceCooldown = firstSurfCdDist(m_rng);

    const float freeD = measureWhaleFreeDist(bestPos.x(), bestPos.z(), bestYaw, 140.0f);
    if (freeD >= 45.0f) {
        m_whaleWaypoint = QVector3D(bestPos.x() + dir.x() * (freeD - 10.0f),
                                    cruiseY,
                                    bestPos.z() + dir.z() * (freeD - 10.0f));
    } else {
        m_whaleWaypoint = pickWhaleOpenWaypoint(QVector3D(0.0f, cruiseY, 0.0f), 90.0f, 240.0f);
    }
    m_whaleWaypointTimer = 24.0f;
    m_whaleSongTimer = nearPlayer ? 1.8f : 7.0f;
}

void DeepDiveEngine::updateWhaleNavigation(float dt, bool& whaleCalledOut) {
    const float sc                = std::max(m_whale.scale, 2.65f);
    const float halfLen           = 5.2f * sc;
    const float bodyBot           = 1.12f * sc;
    const float submergedCeilingY = ProcMath::WATER_SURFACE_Y - (0.85f * sc + 0.9f);
    const float skimCeilingY      = ProcMath::WATER_SURFACE_Y - 0.25f * sc;
    const float surfacingCeilingY = ProcMath::WATER_SURFACE_Y;
    const float cruiseY           = submergedCeilingY - 0.8f;
    const float safeTurnR         = ProcMath::BASIN_INNER_RADIUS - (65.0f + halfLen);

    const float triggerDist = halfLen * 1.35f + 16.0f;
    const float openDist    = halfLen * 1.80f + 22.0f;

    const float surfBlendRate = m_whaleSurfacing ? (dt * 0.65f) : (dt * 0.20f);
    m_whaleSurfaceBlend = ProcMath::clamp(
        m_whaleSurfaceBlend + (m_whaleSurfacing ? surfBlendRate : -surfBlendRate), 0.0f, 1.0f);
    const float smoothSurf     = m_whaleSurfaceBlend * m_whaleSurfaceBlend * (3.0f - 2.0f * m_whaleSurfaceBlend);
    const float activeCeilingY = ProcMath::mix(skimCeilingY, surfacingCeilingY, smoothSurf);

    const float fxPre = std::sin(m_whale.yaw);
    const float fzPre = std::cos(m_whale.yaw);
    const float distFromCenter = std::sqrt(m_whale.pos.x() * m_whale.pos.x() + m_whale.pos.z() * m_whale.pos.z());
    const float freeForward    = measureWhaleFreeDist(m_whale.pos.x(), m_whale.pos.z(), m_whale.yaw, 120.0f);
    const bool  atRimHeadingOut = (distFromCenter > safeTurnR)
                                 && ((fxPre * m_whale.pos.x() + fzPre * m_whale.pos.z()) > -0.10f * distFromCenter);

    auto pickTurnDirection = [&](const QVector3D* wallPushDir) -> float {
        const float sideX =  std::cos(m_whale.yaw);
        const float sideZ = -std::sin(m_whale.yaw);

        float posWindowAng = 99.0f;
        float negWindowAng = 99.0f;
        float posSum = 0.0f;
        float negSum = 0.0f;

        for (int step = 1; step <= 11; ++step) {
            const float a = static_cast<float>(step) * (PI / 12.0f);
            const float dPos = measureWhaleFreeDist(m_whale.pos.x(), m_whale.pos.z(), wrapAngle(m_whale.yaw + a), 110.0f);
            const float dNeg = measureWhaleFreeDist(m_whale.pos.x(), m_whale.pos.z(), wrapAngle(m_whale.yaw - a), 110.0f);
            posSum += dPos;
            negSum += dNeg;
            if (posWindowAng > 10.0f && dPos >= openDist) posWindowAng = a;
            if (negWindowAng > 10.0f && dNeg >= openDist) negWindowAng = a;
        }

        if (distFromCenter > ProcMath::BASIN_INNER_RADIUS * 0.62f) {
            const float inwardDot = (-m_whale.pos.x() * sideX - m_whale.pos.z() * sideZ) / std::max(distFromCenter, 1.0f);
            if (inwardDot > 0.25f)  return  1.0f;
            if (inwardDot < -0.25f) return -1.0f;
        }

        if (std::abs(posWindowAng - negWindowAng) > 0.20f) {
            return (posWindowAng < negWindowAng) ? 1.0f : -1.0f;
        }
        if (wallPushDir && wallPushDir->lengthSquared() > 1e-4f) {
            const float sideDot = wallPushDir->x() * sideX + wallPushDir->z() * sideZ;
            if (std::abs(sideDot) > 0.22f) return (sideDot >= 0.0f) ? 1.0f : -1.0f;
        }
        return (posSum >= negSum) ? 1.0f : -1.0f;
    };

    auto startAvoidingTurn = [&](const QVector3D* wallPushDir) {
        if (m_whaleState == WhaleState::AvoidingTurn) return;
        m_whaleState         = WhaleState::AvoidingTurn;
        m_whaleTurnDir       = pickTurnDirection(wallPushDir);
        m_whaleAvoidTimer    = 0.0f;
        m_whaleAvoidAccumYaw = 0.0f;
    };

    if (m_whaleSurfacing) {
        m_whaleSurfaceTimer -= dt;
        if (m_whaleSurfaceTimer <= 0.0f || m_whaleState == WhaleState::AvoidingTurn) {
            m_whaleSurfacing = false;
            std::uniform_real_distribution<float> cdDist(14.0f, 24.0f);
            m_whaleSurfaceCooldown = cdDist(m_rng);
        }
    } else if (m_whaleState == WhaleState::Cruising) {
        m_whaleSurfaceCooldown -= dt;
        if (m_whaleSurfaceCooldown <= 0.0f && freeForward >= 42.0f && distFromCenter < safeTurnR) {
            m_whaleSurfacing = true;
            std::uniform_real_distribution<float> durDist(12.0f, 18.0f);
            m_whaleSurfaceTimer = durDist(m_rng);
            if ((m_whale.pos - m_camPos).length() < 360.0f) {
                whaleCalledOut = true;
            }
        }
    }

    float targetYawRate = 0.0f;
    float speedScale    = 1.0f;

    if (m_whaleState == WhaleState::Cruising) {
        if (freeForward < triggerDist || atRimHeadingOut) {
            startAvoidingTurn(nullptr);
        }
    }

    if (m_whaleState == WhaleState::Cruising) {
        QVector3D toWp(m_whaleWaypoint.x() - m_whale.pos.x(), 0.0f, m_whaleWaypoint.z() - m_whale.pos.z());
        float distToWp = toWp.length();
        m_whaleWaypointTimer -= dt;
        if (distToWp < (32.0f + halfLen * 0.5f) || m_whaleWaypointTimer <= 0.0f) {
            m_whaleWaypoint = pickWhaleOpenWaypoint(QVector3D(0.0f, cruiseY, 0.0f), 100.0f, 270.0f);
            m_whaleWaypointTimer = 24.0f;
            toWp = QVector3D(m_whaleWaypoint.x() - m_whale.pos.x(), 0.0f, m_whaleWaypoint.z() - m_whale.pos.z());
            distToWp = toWp.length();
        }

        const float desiredYaw = (distToWp > 1.0f)
                                     ? (std::atan2(toWp.x(), toWp.z()) + std::sin(m_time * 0.14f) * 0.07f)
                                     : m_whale.yaw;
        const float yawErr = wrapAngle(desiredYaw - m_whale.yaw);
        targetYawRate = ProcMath::clamp(yawErr * 0.85f, -0.40f, 0.40f);
        speedScale    = 1.0f;
    } else {
        m_whaleAvoidTimer += dt;

        const float prox = ProcMath::clamp(freeForward / openDist, 0.0f, 1.0f);
        speedScale    = ProcMath::mix(0.32f, 0.90f, prox);
        targetYawRate = m_whaleTurnDir * ProcMath::mix(0.95f, 0.62f, prox);

        const float freeLeft  = measureWhaleFreeDist(m_whale.pos.x(), m_whale.pos.z(), wrapAngle(m_whale.yaw + 0.20f), openDist);
        const float freeRight = measureWhaleFreeDist(m_whale.pos.x(), m_whale.pos.z(), wrapAngle(m_whale.yaw - 0.20f), openDist);
        const bool inwardOk   = (distFromCenter < safeTurnR)
                              || ((fxPre * (-m_whale.pos.x()) + fzPre * (-m_whale.pos.z())) > 0.20f * distFromCenter);

        bool windowOpened = (m_whaleAvoidTimer >= 0.45f)
                            && (freeForward >= openDist)
                            && (freeLeft >= openDist * 0.60f)
                            && (freeRight >= openDist * 0.60f)
                            && inwardOk;

        if (!windowOpened && m_whaleAvoidAccumYaw > 3.75f) {
            float bestYaw = m_whale.yaw;
            float bestD   = -1.0f;
            for (int i = 0; i < 24; ++i) {
                const float a = (TWO_PI * static_cast<float>(i)) / 24.0f;
                const float d = measureWhaleFreeDist(m_whale.pos.x(), m_whale.pos.z(), a, 120.0f);
                if (d > bestD) { bestD = d; bestYaw = a; }
            }
            if (std::abs(wrapAngle(bestYaw - m_whale.yaw)) < 0.35f || m_whaleAvoidAccumYaw > 5.8f) {
                if (bestD < 14.0f) {
                    QVector3D toCenter(-m_whale.pos.x(), 0.0f, -m_whale.pos.z());
                    if (toCenter.lengthSquared() > 1.0f) {
                        toCenter.normalize();
                        m_whale.pos += toCenter * 10.0f;
                        m_whale.yaw = std::atan2(toCenter.x(), toCenter.z());
                    }
                }
                windowOpened = true;
            }
        }

        if (windowOpened) {
            m_whaleState         = WhaleState::Cruising;
            m_whaleAvoidAccumYaw = 0.0f;
            m_whaleStuckAccum    = 0.0f;

            const float wpDist = std::clamp(freeForward - 8.0f, 45.0f, 130.0f);
            float wx = m_whale.pos.x() + fxPre * wpDist;
            float wz = m_whale.pos.z() + fzPre * wpDist;
            const float wr = std::sqrt(wx * wx + wz * wz);
            const float maxWpR = ProcMath::BASIN_INNER_RADIUS - 85.0f;
            if (wr > maxWpR) {
                const float k = maxWpR / wr;
                wx *= k;
                wz *= k;
            }
            m_whaleWaypoint      = QVector3D(wx, cruiseY, wz);
            m_whaleWaypointTimer = 18.0f;
        }
    }

    m_whaleCurrentYawRate = ProcMath::mix(m_whaleCurrentYawRate, targetYawRate,
                                          ProcMath::clamp(dt * 3.5f, 0.0f, 1.0f));
    const float yawStep = m_whaleCurrentYawRate * dt;
    m_whale.yaw = wrapAngle(m_whale.yaw + yawStep);
    if (m_whaleState == WhaleState::AvoidingTurn) {
        m_whaleAvoidAccumYaw += std::abs(yawStep);
    }

    const float fx = std::sin(m_whale.yaw);
    const float fz = std::cos(m_whale.yaw);
    const float rx =  fz;
    const float rz = -fx;

    const auto& col = m_world.collision;
    const float maxLocalFloor = std::max({
        col.sampleHeight(m_whale.pos.x(), m_whale.pos.z()),
        col.sampleHeight(m_whale.pos.x() + fx * (halfLen * 0.85f), m_whale.pos.z() + fz * (halfLen * 0.85f)),
        col.sampleHeight(m_whale.pos.x() + fx * (halfLen * 1.50f), m_whale.pos.z() + fz * (halfLen * 1.50f)),
        col.sampleHeight(m_whale.pos.x() + rx * (1.25f * sc),      m_whale.pos.z() + rz * (1.25f * sc)),
        col.sampleHeight(m_whale.pos.x() - rx * (1.25f * sc),      m_whale.pos.z() - rz * (1.25f * sc))
    });

    const float desiredAlt = m_whaleSurfacing
                                 ? (surfacingCeilingY + std::sin(m_time * 0.65f) * 0.12f)
                                 : (cruiseY + std::sin(m_time * 0.22f) * 1.0f);
    const float targetY = ProcMath::clamp(std::max(desiredAlt, maxLocalFloor + bodyBot + 1.0f),
                                          10.0f, activeCeilingY);

    const float maxUpPitch = m_whaleSurfacing ? 0.34f : 0.26f;
    const float targetPitch = ProcMath::clamp((targetY - m_whale.pos.y()) * 0.13f, -0.26f, maxUpPitch);
    m_whale.pitch = ProcMath::mix(m_whale.pitch, targetPitch, ProcMath::clamp(dt * 2.4f, 0.0f, 1.0f));

    const float targetBank = ProcMath::clamp(-m_whaleCurrentYawRate * 0.30f, -0.20f, 0.20f);
    m_whale.bank = ProcMath::mix(m_whale.bank, targetBank, ProcMath::clamp(dt * 2.4f, 0.0f, 1.0f));

    QVector3D forward(fx * std::cos(m_whale.pitch),
                      std::sin(m_whale.pitch),
                      fz * std::cos(m_whale.pitch));
    forward.normalize();
    m_whale.vel = forward * (m_whale.cruiseSpeed * speedScale);
    m_whale.pos += m_whale.vel * dt;

    const QVector3D checkPts[3] = {
        m_whale.pos,
        m_whale.pos + QVector3D(fx, 0.0f, fz) * (halfLen * 0.72f),
        m_whale.pos - QVector3D(fx, 0.0f, fz) * (halfLen * 0.68f)
    };

    QVector3D horizPush(0.0f, 0.0f, 0.0f);
    float neededMinY = -1e9f;
    const float gradStep = 3.5f * sc;

    for (int i = 0; i < 3; ++i) {
        const auto& pt = checkPts[i];
        const float reqClear = (i == 0) ? (bodyBot + 0.5f) : (0.65f * sc + 0.5f);
        const float h = col.sampleHeight(pt.x(), pt.z());
        const float reqY = h + reqClear;
        if (reqY > m_whale.pos.y()) {
            neededMinY = std::max(neededMinY, std::min(reqY, activeCeilingY));
        }
        if (reqY > activeCeilingY) {
            const float hPx = col.sampleHeight(pt.x() + gradStep, pt.z());
            const float hNx = col.sampleHeight(pt.x() - gradStep, pt.z());
            const float hPz = col.sampleHeight(pt.x(), pt.z() + gradStep);
            const float hNz = col.sampleHeight(pt.x(), pt.z() - gradStep);
            QVector3D outDir(hNx - hPx, 0.0f, hNz - hPz);
            if (outDir.lengthSquared() < 0.25f) outDir = QVector3D(-pt.x(), 0.0f, -pt.z());
            if (outDir.lengthSquared() > 1e-4f) {
                outDir.normalize();
                horizPush += outDir * std::clamp((reqY - activeCeilingY) * 0.6f + 2.0f, 2.0f, 12.0f);
            }
        }
    }

    if (horizPush.lengthSquared() > 1e-4f) {
        const float pushLen = horizPush.length();
        const QVector3D pushDir = horizPush / pushLen;
        const float step = std::min(pushLen, 12.0f * dt);
        m_whale.pos.setX(m_whale.pos.x() + pushDir.x() * step);
        m_whale.pos.setZ(m_whale.pos.z() + pushDir.z() * step);
        if (m_whaleState == WhaleState::Cruising) {
            startAvoidingTurn(&pushDir);
        }
    }

    if (neededMinY > m_whale.pos.y()) {
        m_whale.pos.setY(std::min(neededMinY, activeCeilingY));
    }
    if (m_whale.pos.y() > activeCeilingY) {
        m_whale.pos.setY(activeCeilingY);
    }

    const float curR = std::sqrt(m_whale.pos.x() * m_whale.pos.x() + m_whale.pos.z() * m_whale.pos.z());
    const float hardWhaleR = ProcMath::BASIN_INNER_RADIUS - 36.0f;
    if (curR > hardWhaleR) {
        const float k = hardWhaleR / curR;
        m_whale.pos.setX(m_whale.pos.x() * k);
        m_whale.pos.setZ(m_whale.pos.z() * k);
        const QVector3D inward(-m_whale.pos.x() / curR, 0.0f, -m_whale.pos.z() / curR);
        startAvoidingTurn(&inward);
    }

    m_whaleSongTimer -= dt;
    if (m_whaleSongTimer <= 0.0f) {
        m_whaleSongTimer = 24.0f + std::fmod(m_time * 3.7f, 14.0f);
        if ((m_whale.pos - m_camPos).length() < 340.0f) {
            whaleCalledOut = true;
        }
    }
}

// ============================================================================
// FAUNA & ANGLERFISH AI
// ============================================================================

void DeepDiveEngine::updateFaunaAgents(float dt) {
    auto stepGroup = [&](std::vector<FaunaAgent>& agents,
                         float lookAhead, float avoidMargin,
                         float floorMargin, float surfaceMargin,
                         float verticalAmp, float turnSmooth, float maxBank, bool fleePlayer) {
        for (auto& a : agents) {
            const QVector3D fwd = a.vel.lengthSquared() > 1e-4f
                                      ? a.vel.normalized()
                                      : QVector3D(std::sin(a.yaw), 0.0f, std::cos(a.yaw));

            const QVector3D toHome = a.homeCenter - a.pos;
            const QVector3D horizToHome(toHome.x(), 0.0f, toHome.z());
            const float homeDist = horizToHome.length();

            QVector3D steer(0.0f, 0.0f, 0.0f);
            if (homeDist > 0.5f) {
                const QVector3D radial = horizToHome / homeDist;
                const QVector3D tangent(-radial.z(), 0.0f, radial.x());
                steer += tangent * 3.2f;
                if (homeDist > a.homeRadius) {
                    const float pull = (homeDist - a.homeRadius) / std::max(a.homeRadius, 1.0f);
                    steer += radial * (pull * 14.0f + 2.5f);
                }
            }

            const float swirl = std::sin(m_time * 0.85f + a.phase) * 1.4f;
            const QVector3D side(-fwd.z(), 0.0f, fwd.x());
            steer += side * swirl;

            const float targetY = a.preferredDepth + std::sin(m_time * 0.9f + a.phase) * verticalAmp;
            steer.setY(steer.y() + (targetY - a.pos.y()) * 1.3f);

            if (fleePlayer) {
                const QVector3D fromCam = a.pos - m_camPos;
                const float camDistSq = fromCam.lengthSquared();
                if (camDistSq < 49.0f && camDistSq > 1e-4f) {
                    const float d = std::sqrt(camDistSq);
                    steer += (fromCam / d) * ((7.0f - d) * 3.8f);
                }
            }

            steer += m_world.collision.computeAvoidanceForce(a.pos, a.boundingRadius, avoidMargin);
            steer += m_world.collision.computeAvoidanceForce(a.pos + fwd * lookAhead, a.boundingRadius, avoidMargin) * 0.85f;

            a.vel += steer * dt;
            float spd = a.vel.length();
            if (spd < 0.05f) {
                a.vel = fwd * a.cruiseSpeed;
                spd = a.cruiseSpeed;
            }

            QVector3D dir = a.vel / spd;
            dir.setY(ProcMath::clamp(dir.y(), -0.52f, 0.52f));
            dir.normalize();
            const float targetSpd = ProcMath::mix(spd, a.cruiseSpeed, ProcMath::clamp(dt * 3.5f, 0.0f, 1.0f));
            a.vel = dir * targetSpd;

            a.pos += a.vel * dt;

            if (m_world.collision.resolveSphereCollision(a.pos, &a.vel, a.boundingRadius, floorMargin, surfaceMargin)) {
                if (a.vel.lengthSquared() < 0.25f) {
                    QVector3D escape(-a.pos.x(), 0.2f, -a.pos.z());
                    if (escape.lengthSquared() > 1e-4f) escape.normalize();
                    else escape = QVector3D(1.0f, 0.0f, 0.0f);
                    a.vel = escape * a.cruiseSpeed;
                }
            }

            const float horizSpd = std::sqrt(a.vel.x() * a.vel.x() + a.vel.z() * a.vel.z());
            const float targetYaw = std::atan2(a.vel.x(), a.vel.z());
            const float dYaw = wrapAngle(targetYaw - a.yaw);
            const float k = ProcMath::clamp(dt * turnSmooth, 0.0f, 1.0f);
            a.yaw = wrapAngle(a.yaw + dYaw * k);

            const float targetPitch = ProcMath::clamp(std::atan2(a.vel.y(), std::max(horizSpd, 0.2f)), -0.55f, 0.55f);
            a.pitch = ProcMath::mix(a.pitch, targetPitch, k);

            const float targetBank = ProcMath::clamp(-dYaw * 0.9f, -maxBank, maxBank);
            a.bank = ProcMath::mix(a.bank, targetBank, ProcMath::clamp(dt * 4.0f, 0.0f, 1.0f));
        }
    };

    stepGroup(m_world.fishAgents,  4.8f, 3.2f, 1.1f, 2.5f, 0.75f, 7.5f, 0.40f, true);
    stepGroup(m_world.mantaAgents, 11.5f, 6.5f, 2.4f, 4.0f, 1.35f, 4.2f, 0.34f, false);
}

QVector3D DeepDiveEngine::computeAnglerLureWorldPos(const FaunaAgent& a) const {
    QVector3D localPos(0.0f, 0.84f, 1.38f);

    const float lureW = ProcMath::smoothstep(0.45f, 1.35f, localPos.z())
                        * ProcMath::smoothstep(0.15f, 0.85f, localPos.y());
    localPos.setX(localPos.x() + std::sin(m_time * 2.1f + a.phase) * 0.09f * lureW);
    localPos.setY(localPos.y() + std::cos(m_time * 1.7f + a.phase) * 0.07f * lureW);
    localPos *= a.scale;

    const float cy = std::cos(a.yaw),   sy = std::sin(a.yaw);
    const float cp = std::cos(a.pitch), sp = std::sin(a.pitch);
    const float cb = std::cos(a.bank),  sb = std::sin(a.bank);

    const QVector3D v1(cb * localPos.x() - sb * localPos.y(),
                       sb * localPos.x() + cb * localPos.y(),
                       localPos.z());
    const QVector3D v2(v1.x(),
                       cp * v1.y() + sp * v1.z(),
                       -sp * v1.y() + cp * v1.z());
    const QVector3D v3(cy * v2.x() + sy * v2.z(),
                       v2.y(),
                       -sy * v2.x() + cy * v2.z());

    return v3 + a.pos;
}

void DeepDiveEngine::updateAnglerAgents(float dt) {
    constexpr float DETECT_RADIUS   = 60.0f;
    constexpr float AMBUSH_MIN_DIST = 10.0f;
    constexpr float AMBUSH_MAX_DIST = 14.0f;

    for (auto& a : m_world.anglerAgents) {
        QVector3D fwd(std::sin(a.yaw) * std::cos(a.pitch),
                      std::sin(a.pitch),
                      std::cos(a.yaw) * std::cos(a.pitch));
        if (fwd.lengthSquared() > 1e-4f) fwd.normalize();
        else fwd = QVector3D(0.0f, 0.0f, 1.0f);

        const QVector3D toCam = m_camPos - a.pos;
        const float distToCam = toCam.length();
        const QVector3D toHome = a.homeCenter - a.pos;
        const QVector3D horizToHome(toHome.x(), 0.0f, toHome.z());
        const float homeDist = horizToHome.length();

        const float floorY = m_world.collision.sampleHeight(a.pos.x(), a.pos.z());
        const float maxPitY = std::min(a.homeCenter.y() + 14.0f, -24.0f);
        const bool stalking = (distToCam < DETECT_RADIUS);

        QVector3D steer(0.0f, 0.0f, 0.0f);
        float targetSpeed = a.cruiseSpeed;

        if (stalking) {
            const QVector3D dirToCam = toCam / std::max(distToCam, 0.001f);
            QVector3D side(-dirToCam.z(), 0.0f, dirToCam.x());
            if (side.lengthSquared() > 1e-4f) side.normalize();

            if (distToCam > AMBUSH_MAX_DIST) {
                const float pull = ProcMath::clamp((distToCam - AMBUSH_MAX_DIST) / 14.0f, 0.25f, 1.0f);
                steer += dirToCam * (2.8f * pull);
                targetSpeed = a.cruiseSpeed * (0.75f + 0.55f * pull);
            } else if (distToCam < AMBUSH_MIN_DIST) {
                const float push = ProcMath::clamp((AMBUSH_MIN_DIST - distToCam) / 5.0f, 0.2f, 1.2f);
                steer -= dirToCam * (3.6f * push);
                steer += side * (std::sin(m_time * 0.6f + a.phase) * 1.1f);
                targetSpeed = a.cruiseSpeed * 0.65f;
            } else {
                steer += side * (std::sin(m_time * 0.55f + a.phase) * 0.85f);
                targetSpeed = 0.45f;
            }

            const float stalkMaxR = a.homeRadius * 1.45f;
            if (homeDist > stalkMaxR) {
                const QVector3D radial = horizToHome / std::max(homeDist, 0.01f);
                const float excess = (homeDist - stalkMaxR) / std::max(a.homeRadius, 1.0f);
                steer += radial * (excess * 14.0f + 3.0f);
            }

            const float hoverY = ProcMath::clamp(m_camPos.y() - 0.8f, floorY + 4.4f, maxPitY);
            steer.setY(steer.y() + (hoverY - a.pos.y()) * 0.85f);
        } else {
            if (homeDist > 0.5f) {
                const QVector3D radial = horizToHome / homeDist;
                const QVector3D tangent(-radial.z(), 0.0f, radial.x());
                steer += tangent * 1.85f;
                if (homeDist > a.homeRadius) {
                    const float pull = (homeDist - a.homeRadius) / std::max(a.homeRadius, 1.0f);
                    steer += radial * (pull * 10.0f + 2.2f);
                }
            } else {
                steer += QVector3D(std::cos(a.phase), 0.0f, std::sin(a.phase)) * 1.5f;
            }

            const QVector3D side(-fwd.z(), 0.0f, fwd.x());
            steer += side * (std::sin(m_time * 0.45f + a.phase) * 0.75f);

            const float patrolY = ProcMath::clamp(
                std::max(a.preferredDepth, floorY + 4.8f) + std::sin(m_time * 0.55f + a.phase) * 1.4f,
                floorY + 4.2f, maxPitY);
            steer.setY(steer.y() + (patrolY - a.pos.y()) * 0.95f);
        }

        steer += m_world.collision.computeAvoidanceForce(a.pos, a.boundingRadius, 4.8f) * 1.15f;
        steer += m_world.collision.computeAvoidanceForce(a.pos + fwd * 6.5f, a.boundingRadius, 5.2f) * 0.75f;

        a.vel += steer * dt;
        a.vel *= std::exp(-1.4f * dt);

        const float spd = a.vel.length();
        if (spd > targetSpeed && spd > 1e-4f) {
            const float newSpd = ProcMath::mix(spd, targetSpeed, ProcMath::clamp(dt * 2.8f, 0.0f, 1.0f));
            a.vel = (a.vel / spd) * newSpd;
        } else if (!stalking && spd < a.cruiseSpeed * 0.55f) {
            const QVector3D dir = (spd > 1e-3f) ? (a.vel / spd) : fwd;
            a.vel = dir * ProcMath::mix(spd, a.cruiseSpeed, ProcMath::clamp(dt * 2.0f, 0.0f, 1.0f));
        }

        a.pos += a.vel * dt;

        if (m_world.collision.resolveSphereCollision(a.pos, &a.vel, a.boundingRadius, 1.8f, 14.0f)) {
            if (a.vel.lengthSquared() < 0.08f && homeDist > 1.0f) {
                a.vel = (horizToHome / homeDist) * (a.cruiseSpeed * 0.7f);
            }
        }
        if (a.pos.y() > maxPitY) {
            a.pos.setY(maxPitY);
            if (a.vel.y() > 0.0f) a.vel.setY(0.0f);
        }

        float targetYaw = a.yaw;
        float targetPitch = 0.0f;
        if (stalking && distToCam > 0.1f) {
            const float hDist = std::sqrt(toCam.x() * toCam.x() + toCam.z() * toCam.z());
            targetYaw   = std::atan2(toCam.x(), toCam.z());
            targetPitch = ProcMath::clamp(std::atan2(toCam.y(), std::max(hDist, 0.3f)), -0.45f, 0.45f);
        } else if (a.vel.lengthSquared() > 0.04f) {
            const float hSpd  = std::sqrt(a.vel.x() * a.vel.x() + a.vel.z() * a.vel.z());
            targetYaw   = std::atan2(a.vel.x(), a.vel.z());
            targetPitch = ProcMath::clamp(std::atan2(a.vel.y(), std::max(hSpd, 0.25f)), -0.32f, 0.32f);
        }

        const float dYaw = wrapAngle(targetYaw - a.yaw);
        const float turnK = ProcMath::clamp(dt * (stalking ? 2.4f : 1.8f), 0.0f, 1.0f);
        a.yaw   = wrapAngle(a.yaw + dYaw * turnK);
        a.pitch = ProcMath::mix(a.pitch, targetPitch, turnK);
        const float targetBank = ProcMath::clamp(-dYaw * 0.45f, -0.18f, 0.18f);
        a.bank  = ProcMath::mix(a.bank, targetBank, ProcMath::clamp(dt * 2.2f, 0.0f, 1.0f));
    }

    std::vector<std::pair<float, QVector3D>> lures;
    lures.reserve(m_world.anglerAgents.size());
    for (const auto& a : m_world.anglerAgents) {
        const QVector3D lurePos = computeAnglerLureWorldPos(a);
        lures.emplace_back((lurePos - m_camPos).lengthSquared(), lurePos);
    }
    if (lures.size() > static_cast<size_t>(MAX_LURE_LIGHTS)) {
        std::partial_sort(lures.begin(), lures.begin() + MAX_LURE_LIGHTS, lures.end(),
                          [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    }
    m_lureCount = std::min(static_cast<int>(lures.size()), MAX_LURE_LIGHTS);
    for (int i = 0; i < m_lureCount; ++i) {
        m_lurePositions[i] = lures[i].second;
    }
}

// ============================================================================
// SIMULATION STEP
// ============================================================================

EngineStepEvents DeepDiveEngine::update(float dt, const EngineInput& input) {
    EngineStepEvents events;
    m_time += dt;

    m_yaw += input.yawDelta;
    m_pitch = ProcMath::clamp(m_pitch + input.pitchDelta, -1.48f, 1.48f);

    const QVector3D fwd = cameraForward();
    const QVector3D rgt = cameraRight();
    QVector3D wishDir = fwd * input.moveForward
                        + rgt * input.moveRight
                        + QVector3D(0.0f, input.moveUp, 0.0f);

    const float wishLenSq = wishDir.lengthSquared();
    if (wishLenSq > 1.0f) {
        wishDir /= std::sqrt(wishLenSq);
    }

    const float accel = input.sprint ? 58.0f : 18.0f;
    const float preCollisionSpeed = m_camVel.length();

    m_camVel += wishDir * (accel * dt);
    m_camVel *= std::exp(-3.0f * dt);
    m_camPos += m_camVel * dt;
    m_camPos.setY(m_camPos.y() + std::sin(m_time * 1.4f) * 0.006f);

    m_stats.playerColliding = m_world.collision.resolveSphereCollision(
        m_camPos, &m_camVel, 1.45f, 0.45f, 1.6f);

    if (m_stats.playerColliding && !m_wasPlayerColliding) {
        events.playerCollisionImpact = true;
        events.collisionImpactSpeed = preCollisionSpeed;
    }
    m_wasPlayerColliding = m_stats.playerColliding;

    updateFaunaAgents(dt);
    updateWhaleNavigation(dt, events.whaleCalled);
    updateAnglerAgents(dt);

    constexpr float BOX_SPAN_XZ = 160.0f;
    constexpr float BOX_HALF_XZ = BOX_SPAN_XZ * 0.5f;
    constexpr float BOX_SPAN_Y  = 56.0f;
    constexpr float BOX_HALF_Y  = BOX_SPAN_Y * 0.5f;

    const float yCenter = std::min(m_camPos.y(), ProcMath::WATER_SURFACE_Y - BOX_HALF_Y - 1.5f);

    auto wrapAround = [](float val, float center, float span, float halfSpan) {
        float d = val - center;
        if (d > halfSpan || d < -halfSpan) {
            d = std::fmod(d + halfSpan, span);
            if (d < 0.0f) d += span;
            return center + d - halfSpan;
        }
        return val;
    };

    for (auto& p : m_world.particles) {
        const float nextX = p.pos.x() + std::sin(m_time * 1.8f + p.phase) * 0.25f * dt;
        const float nextY = p.pos.y() + p.speedY * dt;
        p.pos.setX(wrapAround(nextX, m_camPos.x(), BOX_SPAN_XZ, BOX_HALF_XZ));
        p.pos.setY(wrapAround(nextY, yCenter,      BOX_SPAN_Y,  BOX_HALF_Y));
        p.pos.setZ(wrapAround(p.pos.z(), m_camPos.z(), BOX_SPAN_XZ, BOX_HALF_XZ));
    }

    if (!m_world.particles.empty() && m_particleVBO != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, m_particleVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        m_world.particles.size() * sizeof(Particle),
                        m_world.particles.data());
    }

    return events;
}

// ============================================================================
// RENDER PIPELINE
// ============================================================================

void DeepDiveEngine::render(int physW, int physH, GLuint targetFramebuffer) {
    physW = std::max(64, physW);
    physH = std::max(64, physH);
    resizePostProcessBuffers(physW, physH);

    QVector3D waterShallow, waterDeep, sunColor, carpetTint, kelpTint;
    float vibrance = 1.0f;
    float fogScale = 1.0f;

    if (m_paletteIndex == 0) {
        // Tropical Reef (Vibrant)
        waterShallow = QVector3D(0.11f, 0.68f, 0.78f);
        waterDeep    = QVector3D(0.02f, 0.24f, 0.38f);
        sunColor     = QVector3D(0.94f, 1.06f, 0.96f);
        carpetTint   = QVector3D(0.28f, 0.90f, 0.44f);
        kelpTint     = QVector3D(0.88f, 1.06f, 0.60f);
        vibrance     = 1.38f;
        fogScale     = 1.05f;
    } else if (m_paletteIndex == 1) {
        // Kelp Reef (Emerald)
        waterShallow = QVector3D(0.12f, 0.62f, 0.66f);
        waterDeep    = QVector3D(0.025f, 0.22f, 0.30f);
        sunColor     = QVector3D(0.68f, 0.98f, 0.96f);
        carpetTint   = QVector3D(0.20f, 0.78f, 0.46f);
        kelpTint     = QVector3D(0.74f, 0.98f, 0.54f);
        vibrance     = 1.15f;
        fogScale     = 1.12f;
    } else if (m_paletteIndex == 2) {
        // Purple Grotto
        waterShallow = QVector3D(0.38f, 0.42f, 0.72f);
        waterDeep    = QVector3D(0.11f, 0.08f, 0.26f);
        sunColor     = QVector3D(0.80f, 0.86f, 1.12f);
        carpetTint   = QVector3D(0.98f, 0.28f, 0.72f);
        kelpTint     = QVector3D(0.90f, 0.46f, 0.98f);
        vibrance     = 1.24f;
        fogScale     = 1.12f;
    } else {
        // Abyssal Trench
        waterShallow = QVector3D(0.07f, 0.38f, 0.54f);
        waterDeep    = QVector3D(0.01f, 0.08f, 0.16f);
        sunColor     = QVector3D(0.52f, 0.88f, 1.02f);
        carpetTint   = QVector3D(0.18f, 0.60f, 0.66f);
        kelpTint     = QVector3D(0.46f, 0.80f, 0.72f);
        vibrance     = 1.08f;
        fogScale     = 1.25f;
    }

    const QVector3D sunDir = QVector3D(-0.28f, 0.92f, -0.22f).normalized();

    QMatrix4x4 proj;
    const float aspect = static_cast<float>(physW) / static_cast<float>(physH);
    proj.perspective(65.0f, aspect, 0.25f, 460.0f);

    QMatrix4x4 view;
    view.lookAt(m_camPos, m_camPos + cameraForward(), QVector3D(0.0f, 1.0f, 0.0f));
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

    auto drawFaunaBatch = [&](const FaunaLodBuffers& buf, size_t instanceCount, bool countTris) {
        if (instanceCount == 0 || buf.indexCount == 0) return;
        const GLsizei clamped = static_cast<GLsizei>(std::min(instanceCount, static_cast<size_t>(buf.maxInstances)));
        glBindVertexArray(buf.vao);
        glDrawElementsInstanced(GL_TRIANGLES, buf.indexCount, GL_UNSIGNED_INT, nullptr, clamped);
        if (countTris) {
            m_stats.drawnTriangles += (buf.indexCount / 3) * clamped;
        }
    };

    // ========================================================================
    // PASS 0: Directional Shadow Map
    // ========================================================================
    const float orthoHalfSize = 110.0f;
    const float texelWorldStep = (orthoHalfSize * 2.0f) / SHADOW_MAP_RES;
    const QVector3D shadowFocus(
        std::floor(m_camPos.x() / texelWorldStep) * texelWorldStep,
        0.0f,
        std::floor(m_camPos.z() / texelWorldStep) * texelWorldStep
        );

    QMatrix4x4 lightProj;
    lightProj.ortho(-orthoHalfSize, orthoHalfSize, -orthoHalfSize, orthoHalfSize, 1.0f, 270.0f);
    QMatrix4x4 lightView;
    lightView.lookAt(shadowFocus + sunDir * 125.0f, shadowFocus, QVector3D(0.0f, 0.0f, 1.0f));
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
        m_shadowEnvShader.setUniformValue("uTime", m_time);

        std::vector<std::pair<int, float>> shadowChunks;
        shadowChunks.reserve(64);

        glBindVertexArray(m_terrainVAO);
        for (int i = 0; i < static_cast<int>(m_world.chunks.size()); ++i) {
            const auto& chunk = m_world.chunks[i];
            if (!lightFrustum.intersectsSphere(chunk.center, chunk.boundingRadius)) continue;

            const float d = distanceToAABB(m_camPos, chunk.minBounds, chunk.maxBounds);
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
            drawPropRange(m_world.chunks[idx].shipwreckRange, false);
        }

        gatherAndUploadInstProps(shadowChunks, true);
        m_shadowInstPropShader.bind();
        m_shadowInstPropShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        m_shadowInstPropShader.setUniformValue("uTime", m_time);
        drawInstPropBatches(true, false);

        cullAndUploadFauna(lightFrustum);
        m_shadowFaunaShader.bind();
        m_shadowFaunaShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        m_shadowFaunaShader.setUniformValue("uTime", m_time);

        drawFaunaBatch(m_fishLod0,   m_visFishLod0.size(),   false);
        drawFaunaBatch(m_fishLod1,   m_visFishLod1.size(),   false);
        drawFaunaBatch(m_mantaLod0,  m_visMantaLod0.size(),  false);
        drawFaunaBatch(m_mantaLod1,  m_visMantaLod1.size(),  false);
        drawFaunaBatch(m_whaleLod0,  m_visWhaleLod0.size(),  false);
        drawFaunaBatch(m_whaleLod1,  m_visWhaleLod1.size(),  false);
        drawFaunaBatch(m_anglerLod0, m_visAnglerLod0.size(), false);
        drawFaunaBatch(m_anglerLod1, m_visAnglerLod1.size(), false);
    }

    // ========================================================================
    // PASS 1: Main Scene Render into MSAA HDR FBO
    // ========================================================================
    m_stats.visibleChunks = 0;
    m_stats.totalChunks = static_cast<int>(m_world.chunks.size());
    m_stats.lodCounts[0] = m_stats.lodCounts[1] = m_stats.lodCounts[2] = m_stats.lodCounts[3] = 0;
    m_stats.drawnTriangles = 0;
    m_stats.visibleInstProps = 0;

    cullAndUploadFauna(camFrustum);

    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
    glViewport(0, 0, physW, physH);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rockTex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);

    glDepthFunc(GL_LEQUAL);
    m_skyShader.bind();
    m_skyShader.setUniformValue("uInvViewProj", invViewProj);
    m_skyShader.setUniformValue("uCamPos", m_camPos);
    m_skyShader.setUniformValue("uSunDir", sunDir);
    m_skyShader.setUniformValue("uSunColor", sunColor);
    m_skyShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_skyShader.setUniformValue("uWaterColorShallow", waterShallow);
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
        prog.setUniformValue("uCamPos", m_camPos);
        prog.setUniformValue("uSunDir", sunDir);
        prog.setUniformValue("uSunColor", sunColor);
        prog.setUniformValue("uWaterColorDeep", waterDeep);
        prog.setUniformValue("uWaterColorShallow", waterShallow);
        prog.setUniformValue("uVibrance", vibrance);
        prog.setUniformValue("uFogScale", fogScale);
        prog.setUniformValue("uTime", m_time);
        prog.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);
        prog.setUniformValue("uLureCount", m_lureCount);
        if (m_lureCount > 0) {
            prog.setUniformValueArray("uLurePos", m_lurePositions.data(), m_lureCount);
        }
    };

    glDepthFunc(GL_LESS);
    bindCommonEnvUniforms(m_envShader);

    static constexpr float TERRAIN_LOD_THRESH[3] = {210.0f, 310.0f, 395.0f};

    std::vector<std::pair<int, float>> visibleChunks;
    visibleChunks.reserve(128);

    // 1.1. Seafloor & cliff walls (4-tier LOD)
    glBindVertexArray(m_terrainVAO);
    for (int i = 0; i < static_cast<int>(m_world.chunks.size()); ++i) {
        const auto& chunk = m_world.chunks[i];
        if (!camFrustum.intersectsSphere(chunk.center, chunk.boundingRadius)) continue;

        const float d = distanceToAABB(m_camPos, chunk.minBounds, chunk.maxBounds);
        if (d > 460.0f) continue;

        ChunkLodState& st = m_chunkLodStates[i];
        st.terrainLod = updateLodWithHysteresis(st.terrainLod, d, TERRAIN_LOD_THRESH, 3, 8.0f);

        m_stats.visibleChunks++;
        m_stats.lodCounts[st.terrainLod]++;
        visibleChunks.emplace_back(i, d);

        const auto& range = chunk.terrainLodRanges[st.terrainLod];
        const void* idxPtr = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(range.indexOffset) * sizeof(uint32_t));
        glDrawElementsBaseVertex(GL_TRIANGLES,
                                 static_cast<GLsizei>(range.indexCount),
                                 GL_UNSIGNED_INT, idxPtr,
                                 static_cast<GLint>(chunk.terrainBaseVertex));
        m_stats.drawnTriangles += static_cast<int>(range.indexCount / 3);
    }

    // 1.2. Static unique props (shipwrecks & abyssal sea pens)
    glBindVertexArray(m_propVAO);
    for (const auto& [idx, d] : visibleChunks) {
        const auto& chunk = m_world.chunks[idx];
        ChunkLodState& st = m_chunkLodStates[idx];

        if (st.detailVisible && d > 192.0f)       st.detailVisible = false;
        else if (!st.detailVisible && d < 176.0f) st.detailVisible = true;
        if (st.detailVisible) drawPropRange(chunk.detailRange, true);

        if (d < 430.0f) drawPropRange(chunk.shipwreckRange, true);
    }

    // 1.3. Instanced environment props (boulders, crags, kelp, sponges, carpets, corals)
    gatherAndUploadInstProps(visibleChunks, false);
    bindCommonEnvUniforms(m_instPropShader);
    m_instPropShader.setUniformValue("uCarpetTint", carpetTint);
    m_instPropShader.setUniformValue("uKelpTint", kelpTint);
    drawInstPropBatches(false, true);

    // 1.4. Marine fauna (reef fish, manta rays, humpback whale, anglerfish)
    bindCommonEnvUniforms(m_faunaShader);
    drawFaunaBatch(m_fishLod0,   m_visFishLod0.size(),   true);
    drawFaunaBatch(m_fishLod1,   m_visFishLod1.size(),   true);
    drawFaunaBatch(m_mantaLod0,  m_visMantaLod0.size(),  true);
    drawFaunaBatch(m_mantaLod1,  m_visMantaLod1.size(),  true);
    drawFaunaBatch(m_whaleLod0,  m_visWhaleLod0.size(),  true);
    drawFaunaBatch(m_whaleLod1,  m_visWhaleLod1.size(),  true);
    drawFaunaBatch(m_anglerLod0, m_visAnglerLod0.size(), true);
    drawFaunaBatch(m_anglerLod1, m_visAnglerLod1.size(), true);

    // Resolve scene depth for shoreline foam and water contact ripples
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFBO);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rockTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);

    const QVector2D waterCenter(std::floor(m_camPos.x() * 2.0f) * 0.5f,
                                std::floor(m_camPos.z() * 2.0f) * 0.5f);
    m_waterShader.bind();
    m_waterShader.setUniformValue("uRockTex", 0);
    m_waterShader.setUniformValue("uSceneDepthTex", 1);
    m_waterShader.setUniformValue("uInvResolution", QVector2D(1.0f / physW, 1.0f / physH));
    m_waterShader.setUniformValue("uViewProj", viewProj);
    m_waterShader.setUniformValue("uWaterCenter", waterCenter);
    m_waterShader.setUniformValue("uWaterY", ProcMath::WATER_SURFACE_Y);
    m_waterShader.setUniformValue("uCamPos", m_camPos);
    m_waterShader.setUniformValue("uSunDir", sunDir);
    m_waterShader.setUniformValue("uSunColor", sunColor);
    m_waterShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_waterShader.setUniformValue("uWaterColorShallow", waterShallow);
    m_waterShader.setUniformValue("uFogScale", fogScale);
    m_waterShader.setUniformValue("uTime", m_time);
    m_waterShader.setUniformValue("uWhaleSurfA",
                                  QVector4D(m_whale.pos.x(), m_whale.pos.y(), m_whale.pos.z(), m_whale.scale));
    m_waterShader.setUniformValue("uWhaleSurfB",
                                  QVector4D(std::sin(m_whale.yaw), std::cos(m_whale.yaw),
                                            std::sin(m_whale.pitch), std::cos(m_whale.pitch)));
    glBindVertexArray(m_waterVAO);
    glDrawElements(GL_TRIANGLES, m_waterIndexCount, GL_UNSIGNED_INT, nullptr);
    m_stats.drawnTriangles += static_cast<int>(m_waterIndexCount / 3);

    if (m_wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

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
        m_rayShader.setUniformValue("uCamPos", m_camPos);
        m_rayShader.setUniformValue("uSunDir", sunDir);
        m_rayShader.setUniformValue("uSunColor", sunColor);
        m_rayShader.setUniformValue("uWaterColorShallow", waterShallow);
        m_rayShader.setUniformValue("uFogScale", fogScale);
        m_rayShader.setUniformValue("uTime", m_time);
        m_rayShader.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);

        glBindVertexArray(m_rayVAO);
        glDrawElementsInstanced(GL_TRIANGLES, m_rayIndexCount, GL_UNSIGNED_INT, nullptr, m_rayInstanceCount);

        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_particleShader.bind();
    m_particleShader.setUniformValue("uViewProj", viewProj);
    m_particleShader.setUniformValue("uCamPos", m_camPos);
    m_particleShader.setUniformValue("uViewportHeight", static_cast<float>(physH));
    glBindVertexArray(m_particleVAO);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_world.particles.size()));

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    // ========================================================================
    // PASS 2: Resolve MSAA + Optional Bloom + Final Composite
    // ========================================================================
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFBO);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(m_skyVAO);

    if (m_postProcessEnabled) {
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

    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, physW, physH);

    const QVector4D sunClip = viewProj * QVector4D(m_camPos + sunDir * 100.0f, 1.0f);
    QVector3D sunScreen(0.5f, 0.85f, 0.0f);
    if (sunClip.w() > 0.001f) {
        const QVector2D ndc(sunClip.x() / sunClip.w(), sunClip.y() / sunClip.w());
        sunScreen.setX(ndc.x() * 0.5f + 0.5f);
        sunScreen.setY(ndc.y() * 0.5f + 0.5f);
        const float facing = QVector3D::dotProduct(cameraForward(), sunDir);
        sunScreen.setZ(ProcMath::smoothstep(-0.15f, 0.65f, facing));
    }

    const float distToSurface = std::max(0.4f, ProcMath::WATER_SURFACE_Y - m_camPos.y());
    const float surfaceProximity = 1.0f - ProcMath::smoothstep(1.8f, 16.0f, distToSurface);

    m_postProcessShader.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_bloomTex[1]);

    m_postProcessShader.setUniformValue("uSceneColorTex", 0);
    m_postProcessShader.setUniformValue("uSceneDepthTex", 1);
    m_postProcessShader.setUniformValue("uBloomTex", 2);
    m_postProcessShader.setUniformValue("uInvResolution", QVector2D(1.0f / physW, 1.0f / physH));
    m_postProcessShader.setUniformValue("uSunScreenPos", sunScreen);
    m_postProcessShader.setUniformValue("uWaterTint", waterShallow);
    m_postProcessShader.setUniformValue("uSurfaceProximity", surfaceProximity);
    m_postProcessShader.setUniformValue("uTime", m_time);
    m_postProcessShader.setUniformValue("uEnablePost", m_postProcessEnabled ? 1 : 0);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
}
