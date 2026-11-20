#include "DeepDiveWidget.h"
#include "Shaders.h"
#include "Noise.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QOpenGLContext>
#include <QSurface>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace {
constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

inline float wrapAngle(float a) {
    while (a > PI)  a -= TWO_PI;
    while (a < -PI) a += TWO_PI;
    return a;
}

inline float distanceToAABB(const QVector3D& p, const QVector3D& bmin, const QVector3D& bmax) {
    float cx = ProcMath::clamp(p.x(), bmin.x(), bmax.x());
    float cy = ProcMath::clamp(p.y(), bmin.y(), bmax.y());
    float cz = ProcMath::clamp(p.z(), bmin.z(), bmax.z());
    return (p - QVector3D(cx, cy, cz)).length();
}

inline FaunaGpuInstance packFaunaInstance(const FaunaAgent& a) {
    FaunaGpuInstance gpu;
    gpu.posScale  = QVector4D(a.pos.x(), a.pos.y(), a.pos.z(), a.scale);
    gpu.rotPhase  = QVector4D(a.yaw, a.phase, a.type, a.bank);
    gpu.pitchTint = QVector4D(a.pitch, a.tint.x(), a.tint.y(), a.tint.z());
    return gpu;
}

// Переключение уровня LOD с гистерезисом (±band) — убирает мерцание/перещёлкивание на границе
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
        float len = pl.toVector3D().length();
        if (len > 1e-6f) {
            pl /= len;
        }
    }
}

bool FrustumPlanes::intersectsSphere(const QVector3D& center, float radius) const {
    for (const auto& pl : planes) {
        float dist = pl.x() * center.x() + pl.y() * center.y() + pl.z() * center.z() + pl.w();
        if (dist < -radius) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// DEEP DIVE WIDGET
// ============================================================================

DeepDiveWidget::DeepDiveWidget(std::vector<AdapterCapability> adapters, QWidget* parent)
    : QOpenGLWidget(parent), m_adapters(std::move(adapters)) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(1280, 720);

    m_audioEngine = new UnderwaterAudioEngine(this);

    connect(&m_timer, &QTimer::timeout, this, QOverload<>::of(&DeepDiveWidget::update));
    m_timer.start(0);
    m_clock.start();
}

DeepDiveWidget::~DeepDiveWidget() {
    makeCurrent();
    destroyPostProcessBuffers();
    if (m_shadowFBO) glDeleteFramebuffers(1, &m_shadowFBO);
    if (m_shadowDepthTex) glDeleteTextures(1, &m_shadowDepthTex);
    if (m_rockTex) glDeleteTextures(1, &m_rockTex);

    if (m_terrainVAO) glDeleteVertexArrays(1, &m_terrainVAO);
    if (m_terrainVBO) glDeleteBuffers(1, &m_terrainVBO);
    if (m_terrainEBO) glDeleteBuffers(1, &m_terrainEBO);

    if (m_propVAO) glDeleteVertexArrays(1, &m_propVAO);
    if (m_propVBO) glDeleteBuffers(1, &m_propVBO);
    if (m_propEBO) glDeleteBuffers(1, &m_propEBO);

    deleteFaunaLodBuffers(m_fishLod0);
    deleteFaunaLodBuffers(m_fishLod1);
    deleteFaunaLodBuffers(m_mantaLod0);
    deleteFaunaLodBuffers(m_mantaLod1);
    deleteFaunaLodBuffers(m_whaleLod0);
    deleteFaunaLodBuffers(m_whaleLod1);
    deleteFaunaLodBuffers(m_anglerLod0);
    deleteFaunaLodBuffers(m_anglerLod1);

    if (m_skyVAO) glDeleteVertexArrays(1, &m_skyVAO);
    if (m_skyVBO) glDeleteBuffers(1, &m_skyVBO);
    if (m_waterVAO) glDeleteVertexArrays(1, &m_waterVAO);
    if (m_waterVBO) glDeleteBuffers(1, &m_waterVBO);
    if (m_waterEBO) glDeleteBuffers(1, &m_waterEBO);
    if (m_rayVAO) glDeleteVertexArrays(1, &m_rayVAO);
    if (m_rayVBO) glDeleteBuffers(1, &m_rayVBO);
    if (m_particleVAO) glDeleteVertexArrays(1, &m_particleVAO);
    if (m_particleVBO) glDeleteBuffers(1, &m_particleVBO);
    doneCurrent();
}

void DeepDiveWidget::queryOpenGLCapabilities() {
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
    } else if (!m_adapters.empty()) {
        vramStr = QString(" (%1 MB VRAM)").arg(m_adapters.front().dedicatedVramMB);
    }

    m_glRendererInfo = QString("%1 [%2]%3")
                           .arg(renderer ? renderer : "Unknown GPU")
                           .arg(vendor ? vendor : "Unknown Vendor")
                           .arg(vramStr);
}

void DeepDiveWidget::createShadowMapBuffer() {
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
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
}

void DeepDiveWidget::createWaterSurfaceMesh() {
    constexpr int RINGS = 72;
    constexpr int SECTORS = 96;
    constexpr float RADIUS = 455.0f;

    std::vector<QVector2D> verts;
    verts.reserve(1 + RINGS * SECTORS);
    verts.emplace_back(0.0f, 0.0f);

    for (int r = 1; r <= RINGS; ++r) {
        float t = static_cast<float>(r) / static_cast<float>(RINGS);
        // Квадратичное сгущение колец прямо над камерой для детальных волн вблизи
        float rad = RADIUS * std::pow(t, 2.2f);
        for (int s = 0; s < SECTORS; ++s) {
            float ang = (TWO_PI * static_cast<float>(s)) / static_cast<float>(SECTORS);
            verts.emplace_back(std::cos(ang) * rad, std::sin(ang) * rad);
        }
    }

    std::vector<uint32_t> inds;
    inds.reserve(SECTORS * 3 + (RINGS - 1) * SECTORS * 6);

    // Внутренний веер вокруг центра
    for (int s = 0; s < SECTORS; ++s) {
        uint32_t i1 = 1 + static_cast<uint32_t>(s);
        uint32_t i2 = 1 + static_cast<uint32_t>((s + 1) % SECTORS);
        inds.insert(inds.end(), {0u, i1, i2});
    }

    // Концентрические кольца до горизонта
    for (int r = 1; r < RINGS; ++r) {
        uint32_t ringA = 1 + static_cast<uint32_t>((r - 1) * SECTORS);
        uint32_t ringB = 1 + static_cast<uint32_t>(r * SECTORS);
        for (int s = 0; s < SECTORS; ++s) {
            uint32_t sNext = static_cast<uint32_t>((s + 1) % SECTORS);
            uint32_t a0 = ringA + s;
            uint32_t a1 = ringA + sNext;
            uint32_t b0 = ringB + s;
            uint32_t b1 = ringB + sNext;
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

void DeepDiveWidget::createProceduralRockTexture() {
    constexpr int N = 1024;
    std::vector<float> heightMap(N * N);

    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (unsigned int t = 0; t < numThreads; ++t) {
        int yStart = (t * N) / numThreads;
        int yEnd   = ((t + 1) * N) / numThreads;
        workers.emplace_back([&, yStart, yEnd]() {
            for (int y = yStart; y < yEnd; ++y) {
                float v = static_cast<float>(y) / N;
                for (int x = 0; x < N; ++x) {
                    float u = static_cast<float>(x) / N;

                    float fbmVal = 0.0f;
                    fbmVal += 0.36f * ProcMath::tileableNoise2D(u, v, 8,   101u);
                    fbmVal += 0.24f * ProcMath::tileableNoise2D(u, v, 16,  202u);
                    fbmVal += 0.16f * ProcMath::tileableNoise2D(u, v, 32,  303u);
                    fbmVal += 0.11f * ProcMath::tileableNoise2D(u, v, 64,  404u);
                    fbmVal += 0.07f * ProcMath::tileableNoise2D(u, v, 128, 505u);
                    fbmVal += 0.06f * ProcMath::tileableNoise2D(u, v, 256, 606u);

                    float vor1 = ProcMath::tileableVoronoi2D(u, v, 8,  707u);
                    float vor2 = ProcMath::tileableVoronoi2D(u, v, 24, 808u);
                    float cracks = ProcMath::smoothstep(0.02f, 0.22f, vor1) * 0.7f
                                   + ProcMath::smoothstep(0.02f, 0.18f, vor2) * 0.3f;

                    heightMap[y * N + x] = ProcMath::clamp(fbmVal * 0.65f + cracks * 0.35f, 0.0f, 1.0f);
                }
            }
        });
    }
    for (auto& th : workers) th.join();

    std::vector<uint8_t> rgba(N * N * 4);
    const int mask = N - 1;
    const float normalStrength = 7.0f;

    for (int y = 0; y < N; ++y) {
        int yU = (y - 1) & mask;
        int yD = (y + 1) & mask;
        for (int x = 0; x < N; ++x) {
            int xL = (x - 1) & mask;
            int xR = (x + 1) & mask;

            float hL = heightMap[y * N + xL];
            float hR = heightMap[y * N + xR];
            float hU = heightMap[yU * N + x];
            float hD = heightMap[yD * N + x];

            QVector3D norm((hL - hR) * normalStrength, (hU - hD) * normalStrength, 1.0f);
            norm.normalize();

            size_t idx = static_cast<size_t>(y * N + x) * 4;
            rgba[idx + 0] = static_cast<uint8_t>(ProcMath::clamp(norm.x() * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[idx + 1] = static_cast<uint8_t>(ProcMath::clamp(norm.y() * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[idx + 2] = static_cast<uint8_t>(ProcMath::clamp(norm.z() * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[idx + 3] = static_cast<uint8_t>(heightMap[y * N + x] * 255.0f);
        }
    }

    glGenTextures(1, &m_rockTex);
    glBindTexture(GL_TEXTURE_2D, m_rockTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
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

void DeepDiveWidget::destroyPostProcessBuffers() {
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
}

void DeepDiveWidget::resizePostProcessBuffers(int physW, int physH) {
    physW = std::max(64, physW);
    physH = std::max(64, physH);
    if (physW == m_fbWidth && physH == m_fbHeight && m_msaaFBO != 0) return;

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

    int halfW = std::max(32, physW / 2);
    int halfH = std::max(32, physH / 2);
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

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
}

void DeepDiveWidget::setupFaunaLodBuffers(FaunaLodBuffers& buf,
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

void DeepDiveWidget::deleteFaunaLodBuffers(FaunaLodBuffers& buf) {
    if (buf.vao) glDeleteVertexArrays(1, &buf.vao);
    if (buf.vbo) glDeleteBuffers(1, &buf.vbo);
    if (buf.ebo) glDeleteBuffers(1, &buf.ebo);
    if (buf.instVbo) glDeleteBuffers(1, &buf.instVbo);
    buf = {};
}

void DeepDiveWidget::initializeGL() {
    initializeOpenGLFunctions();
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_PROGRAM_POINT_SIZE);

    queryOpenGLCapabilities();
    createProceduralRockTexture();
    createShadowMapBuffer();

    m_shadowEnvShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ShadowEnvVert);
    m_shadowEnvShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ShadowDepthFrag);
    m_shadowEnvShader.link();

    m_shadowFaunaShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ShadowFaunaVert);
    m_shadowFaunaShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ShadowDepthFrag);
    m_shadowFaunaShader.link();

    m_envShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::EnvironmentVert);
    m_envShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::EnvironmentFrag);
    m_envShader.link();

    m_faunaShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::FaunaVert);
    m_faunaShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::FaunaFrag);
    m_faunaShader.link();

    m_skyShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::SkyboxVert);
    m_skyShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::SkyboxFrag);
    m_skyShader.link();

    m_waterShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::WaterSurfaceVert);
    m_waterShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::WaterSurfaceFrag);
    m_waterShader.link();

    m_rayShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::TranslucentVert);
    m_rayShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::TranslucentFrag);
    m_rayShader.link();

    m_particleShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::ParticleVert);
    m_particleShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ParticleFrag);
    m_particleShader.link();

    m_scatterBlurShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_scatterBlurShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::ScatterBlurFrag);
    m_scatterBlurShader.link();

    m_postProcessShader.addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::PostQuadVert);
    m_postProcessShader.addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::PostProcessFrag);
    m_postProcessShader.link();

    float skyQuad[] = {-1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
                       -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f};
    glGenVertexArrays(1, &m_skyVAO);
    glGenBuffers(1, &m_skyVBO);
    glBindVertexArray(m_skyVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_skyVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyQuad), skyQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    createWaterSurfaceMesh();

    glGenVertexArrays(1, &m_terrainVAO);
    glGenBuffers(1, &m_terrainVBO);
    glGenBuffers(1, &m_terrainEBO);

    glGenVertexArrays(1, &m_propVAO);
    glGenBuffers(1, &m_propVBO);
    glGenBuffers(1, &m_propEBO);

    glGenVertexArrays(1, &m_rayVAO);
    glGenBuffers(1, &m_rayVBO);

    glGenVertexArrays(1, &m_particleVAO);
    glGenBuffers(1, &m_particleVBO);

    rebuildWorld(m_seed);

    qreal dpr = devicePixelRatioF();
    resizePostProcessBuffers(int(width() * dpr), int(height() * dpr));
}

void DeepDiveWidget::rebuildWorld(uint32_t seed) {
    m_world = WorldGenerator::generate(seed);
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
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(EnvVertex), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t), inds.data(), GL_STATIC_DRAW);

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

    glBindVertexArray(m_rayVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_rayVBO);
    glBufferData(GL_ARRAY_BUFFER, m_world.godRayVertices.size() * sizeof(RayVertex),
                 m_world.godRayVertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RayVertex),
                          reinterpret_cast<void*>(offsetof(RayVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(RayVertex),
                          reinterpret_cast<void*>(offsetof(RayVertex, color)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RayVertex),
                          reinterpret_cast<void*>(offsetof(RayVertex, uv)));

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
// НАВИГАЦИЯ КИТА В ВЫСОКОЙ ОТКРЫТОЙ ВОДЕ (без застреваний и дёрганий)
// ============================================================================

float DeepDiveWidget::sampleWhaleClearance(float x, float z) const {
    constexpr float CRUISE_CEILING = ProcMath::WATER_SURFACE_Y - 5.2f; // ~22.8 м

    // 1. Внешнее скальное кольцо кальдеры — не приближаемся к отвесным стенам
    float r = std::sqrt(x * x + z * z);
    float wallF = ProcMath::sampleBasinWallFactor(x, z, m_seed);
    if (wallF > 0.04f || r > ProcMath::BASIN_INNER_RADIUS - 75.0f) {
        return -60.0f - (r - (ProcMath::BASIN_INNER_RADIUS - 75.0f)) * 0.8f - wallF * 80.0f;
    }

    // 2. Максимальная высота рельефа в пятне 18 м вокруг позиции кита
    float maxTerrain = ProcMath::sampleTerrainHeight(x, z, m_seed);
    constexpr float offsets[4][2] = {{18.0f, 0.0f}, {-18.0f, 0.0f}, {0.0f, 18.0f}, {0.0f, -18.0f}};
    for (const auto& o : offsets) {
        maxTerrain = std::max(maxTerrain, ProcMath::sampleTerrainHeight(x + o[0], z + o[1], m_seed));
    }

    float clearance = CRUISE_CEILING - maxTerrain;

    // 3. Явный обход 22 центральных скальных пиков и кекуров с запасом под 28-метровый корпус
    const auto& spires = ProcMath::getCentralSpires(m_seed);
    for (const auto& sp : spires) {
        float dx = x - sp.x;
        float dz = z - sp.z;
        float keepOut = sp.radius + 34.0f;
        float d2 = dx * dx + dz * dz;
        if (d2 < keepOut * keepOut) {
            float d = std::sqrt(d2);
            // Чем выше пик, тем шире и строже коридор обхода
            float peakPenalty = (keepOut - d) * (0.65f + ProcMath::smoothstep(24.0f, 52.0f, sp.height) * 0.95f);
            clearance -= peakPenalty;
        }
    }

    return clearance;
}

QVector3D DeepDiveWidget::pickWhaleOpenWaypoint(const QVector3D& nearAnchor, float minDist, float maxDist) const {
    constexpr float CRUISE_Y = ProcMath::WATER_SURFACE_Y - 7.5f; // ~20.5 м
    QVector3D bestPos(0.0f, CRUISE_Y, 0.0f);
    float bestScore = -1e9f;

    float baseAng = std::fmod(m_time * 0.31f + m_whale.yaw + 0.7f, TWO_PI);
    for (int i = 0; i < 28; ++i) {
        float h1 = ProcMath::hash3(i, static_cast<int>(m_time * 3.0f), 11, m_seed + 4101u);
        float h2 = ProcMath::hash3(i, static_cast<int>(m_time * 3.0f), 19, m_seed + 4101u);
        float ang = baseAng + (TWO_PI * static_cast<float>(i)) / 28.0f + (h1 - 0.5f) * 0.35f;
        float dist = ProcMath::mix(minDist, maxDist, h2);

        float cx = nearAnchor.x() + std::cos(ang) * dist;
        float cz = nearAnchor.z() + std::sin(ang) * dist;

        // Держим цели в просторной центральной зоне кальдеры
        float rad = std::sqrt(cx * cx + cz * cz);
        float maxPlayR = ProcMath::BASIN_INNER_RADIUS - 105.0f;
        if (rad > maxPlayR) {
            float k = maxPlayR / std::max(rad, 1.0f);
            cx *= k;
            cz *= k;
        }

        float clear = sampleWhaleClearance(cx, cz);
        // Проверяем также середину отрезка от текущего положения кита, чтобы не гнать его сквозь кекур
        float midClear = sampleWhaleClearance(0.5f * (m_whale.pos.x() + cx), 0.5f * (m_whale.pos.z() + cz));
        float score = std::min(clear, midClear * 1.1f) + clear * 0.35f;

        if (score > bestScore) {
            bestScore = score;
            float floorY = ProcMath::sampleTerrainHeight(cx, cz, m_seed);
            float y = ProcMath::clamp(std::max(floorY + 14.0f, 18.0f + h1 * 3.5f),
                                      15.0f, ProcMath::WATER_SURFACE_Y - 6.0f);
            bestPos = QVector3D(cx, y, cz);
        }
    }
    return bestPos;
}

void DeepDiveWidget::spawnWhalePass(bool nearPlayer) {
    m_whale.type = 2.0f;
    m_whale.scale = 2.65f;
    m_whale.boundingRadius = 5.0f;
    m_whale.cruiseSpeed = 5.2f;
    m_whale.preferredDepth = ProcMath::WATER_SURFACE_Y - 7.5f; // ~20.5 м
    m_whale.homeCenter = QVector3D(0.0f, m_whale.preferredDepth, 0.0f);
    m_whale.homeRadius = ProcMath::BASIN_INNER_RADIUS - 115.0f;
    m_whale.tint = QVector3D(1.0f, 1.0f, 1.0f);
    m_whale.phase = 0.0f;

    QVector3D anchor = nearPlayer ? m_camPos : QVector3D(0.0f, 19.0f, 0.0f);
    float anchorR = std::sqrt(anchor.x() * anchor.x() + anchor.z() * anchor.z());
    if (anchorR > ProcMath::BASIN_INNER_RADIUS - 140.0f) {
        float k = (ProcMath::BASIN_INNER_RADIUS - 140.0f) / std::max(anchorR, 1.0f);
        anchor.setX(anchor.x() * k);
        anchor.setZ(anchor.z() * k);
    }

    QVector3D bestPos(0.0f, 20.5f, -55.0f);
    QVector3D bestDir(1.0f, 0.0f, 0.2f);
    bestDir.normalize();
    float bestScore = -1e9f;

    float baseAng = std::fmod(m_time * 0.41f + 0.9f, TWO_PI);
    for (int attempt = 0; attempt < 28; ++attempt) {
        float ang = baseAng + (TWO_PI * static_cast<float>(attempt)) / 28.0f;
        QVector3D dir(std::sin(ang), 0.0f, std::cos(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());

        float distBack = nearPlayer ? 72.0f : 120.0f;
        QVector3D cand = anchor - dir * distBack + side * (nearPlayer ? 14.0f : 26.0f);

        float c0 = sampleWhaleClearance(cand.x(), cand.z());
        float c1 = sampleWhaleClearance(cand.x() + dir.x() * 35.0f, cand.z() + dir.z() * 35.0f);
        float c2 = sampleWhaleClearance(cand.x() + dir.x() * 75.0f, cand.z() + dir.z() * 75.0f);
        float minCourseClear = std::min({c0, c1, c2});

        if (minCourseClear > bestScore) {
            bestScore = minCourseClear;
            float floorY = ProcMath::sampleTerrainHeight(cand.x(), cand.z(), m_seed);
            float y = ProcMath::clamp(std::max(floorY + 13.5f, 19.5f),
                                      16.0f, ProcMath::WATER_SURFACE_Y - 5.8f);
            bestPos = QVector3D(cand.x(), y, cand.z());
            bestDir = dir;
        }
    }

    m_whale.pos = bestPos;
    m_whale.yaw = std::atan2(bestDir.x(), bestDir.z());
    m_whale.pitch = 0.0f;
    m_whale.bank = 0.0f;
    m_whale.vel = bestDir * m_whale.cruiseSpeed;
    m_whaleWanderAngle = m_whale.yaw;

    // Первая цель — далеко впереди по свободному коридору
    QVector3D forwardHint = bestPos + bestDir * 150.0f;
    if (sampleWhaleClearance(forwardHint.x(), forwardHint.z()) > 12.0f) {
        m_whaleWaypoint = QVector3D(forwardHint.x(), 20.0f, forwardHint.z());
    } else {
        m_whaleWaypoint = pickWhaleOpenWaypoint(QVector3D(0.0f, 20.0f, 0.0f), 90.0f, 260.0f);
    }
    m_whaleWaypointTimer = 26.0f;
    m_whaleSongTimer = nearPlayer ? 2.5f : 8.0f;
}

void DeepDiveWidget::updateWhaleNavigation(float dt) {
    // 1. Обновляем путевую точку в открытой воде при приближении или по таймеру
    QVector3D toWp(m_whaleWaypoint.x() - m_whale.pos.x(), 0.0f, m_whaleWaypoint.z() - m_whale.pos.z());
    float distToWp = toWp.length();
    m_whaleWaypointTimer -= dt;
    if (distToWp < 42.0f || m_whaleWaypointTimer <= 0.0f) {
        m_whaleWaypoint = pickWhaleOpenWaypoint(QVector3D(0.0f, 20.0f, 0.0f), 110.0f, 320.0f);
        m_whaleWaypointTimer = 28.0f;
        toWp = QVector3D(m_whaleWaypoint.x() - m_whale.pos.x(), 0.0f, m_whaleWaypoint.z() - m_whale.pos.z());
        distToWp = toWp.length();
    }

    // Желаемый курс к открытой точке с лёгким естественным рысканием
    QVector3D desiredHoriz = (distToWp > 1.0f)
                                 ? (toWp / distToWp)
                                 : QVector3D(std::sin(m_whale.yaw), 0.0f, std::cos(m_whale.yaw));
    float baseTargetYaw = std::atan2(desiredHoriz.x(), desiredHoriz.z())
                          + std::sin(m_time * 0.16f) * 0.14f;

    // 2. Веерный упреждающий выбор самого свободного курса (5 лучей на 24, 48 и 76 м)
    constexpr float PROBE_ANGLES[5] = {0.0f, 0.42f, -0.42f, 0.90f, -0.90f};
    constexpr float PROBE_DISTS[3]  = {24.0f, 48.0f, 76.0f};

    float straightMinClear = 1e9f;
    float bestBeamYaw = m_whale.yaw;
    float bestBeamScore = -1e9f;

    for (int b = 0; b < 5; ++b) {
        float candYaw = wrapAngle(m_whale.yaw + PROBE_ANGLES[b]);
        float sx = std::sin(candYaw);
        float sz = std::cos(candYaw);

        float minClear = 1e9f;
        float avgClear = 0.0f;
        for (float pd : PROBE_DISTS) {
            float cl = sampleWhaleClearance(m_whale.pos.x() + sx * pd, m_whale.pos.z() + sz * pd);
            minClear = std::min(minClear, cl);
            avgClear += cl;
        }
        avgClear /= 3.0f;
        if (b == 0) straightMinClear = minClear;

        // Оцениваем: сначала чистота воды, затем совпадение с целевым направлением
        float align = std::cos(wrapAngle(candYaw - baseTargetYaw));
        float turnPenalty = std::abs(PROBE_ANGLES[b]) * 4.5f;
        float score = std::min(minClear, 24.0f) * 2.4f + std::min(avgClear, 28.0f) * 0.6f
                      + align * 12.0f - turnPenalty;

        if (score > bestBeamScore) {
            bestBeamScore = score;
            bestBeamYaw = candYaw;
        }
    }

    // Если впереди чисто — идём на путевую точку, иначе заранее плавно обходим скалу по лучшему лучу
    float chosenYaw = (straightMinClear > 14.0f) ? baseTargetYaw : bestBeamYaw;

    // Мягкий приоритет возврата к центру, если кит подошёл к внешнему краю бассейна
    float distFromCenter = std::sqrt(m_whale.pos.x() * m_whale.pos.x() + m_whale.pos.z() * m_whale.pos.z());
    if (distFromCenter > ProcMath::BASIN_INNER_RADIUS - 105.0f) {
        float inwardYaw = std::atan2(-m_whale.pos.x(), -m_whale.pos.z());
        float edgeBlend = ProcMath::smoothstep(ProcMath::BASIN_INNER_RADIUS - 105.0f,
                                               ProcMath::BASIN_INNER_RADIUS - 65.0f,
                                               distFromCenter);
        chosenYaw = wrapAngle(chosenYaw + wrapAngle(inwardYaw - chosenYaw) * edgeBlend);
    }

    // 3. Строго ограниченная угловая скорость поворота — величавое движение без малейших рывков
    constexpr float MAX_YAW_RATE = 0.42f; // рад/с (~24°/сек)
    float yawErr = wrapAngle(chosenYaw - m_whale.yaw);
    float yawStep = ProcMath::clamp(yawErr * 1.4f, -MAX_YAW_RATE, MAX_YAW_RATE) * dt;
    m_whale.yaw = wrapAngle(m_whale.yaw + yawStep);

    // 4. Высокий безопасный эшелон плавания (над дном и под гладью воды)
    float fx = std::sin(m_whale.yaw);
    float fz = std::cos(m_whale.yaw);
    float floorHere  = ProcMath::sampleTerrainHeight(m_whale.pos.x(), m_whale.pos.z(), m_seed);
    float floorAhead = ProcMath::sampleTerrainHeight(m_whale.pos.x() + fx * 26.0f, m_whale.pos.z() + fz * 26.0f, m_seed);
    float maxLocalFloor = std::max(floorHere, floorAhead);

    constexpr float SURFACE_LIMIT_Y = ProcMath::WATER_SURFACE_Y - 5.4f; // ~22.6 м
    float desiredAlt = 19.5f + std::sin(m_time * 0.24f) * 2.2f;
    float targetY = ProcMath::clamp(std::max(desiredAlt, maxLocalFloor + 10.5f),
                                    13.5f, SURFACE_LIMIT_Y);

    float targetPitch = ProcMath::clamp((targetY - m_whale.pos.y()) * 0.065f, -0.22f, 0.22f);
    m_whale.pitch = ProcMath::mix(m_whale.pitch, targetPitch, ProcMath::clamp(dt * 1.8f, 0.0f, 1.0f));

    // Плавный крен в сторону поворота
    float yawRate = (dt > 1e-4f) ? (yawStep / dt) : 0.0f;
    float targetBank = ProcMath::clamp(-yawRate * 0.42f, -0.18f, 0.18f);
    m_whale.bank = ProcMath::mix(m_whale.bank, targetBank, ProcMath::clamp(dt * 2.0f, 0.0f, 1.0f));

    // 5. Интегрируем позицию строго вдоль ориентации корпуса (кит всегда плывёт носом вперёд)
    QVector3D forward(std::sin(m_whale.yaw) * std::cos(m_whale.pitch),
                      std::sin(m_whale.pitch),
                      std::cos(m_whale.yaw) * std::cos(m_whale.pitch));
    forward.normalize();
    m_whale.vel = forward * m_whale.cruiseSpeed;
    m_whale.pos += m_whale.vel * dt;

    // 6. Мягкое выталкивание без резких разворотов, если кекур всё же оказался слишком близко
    const auto& spires = ProcMath::getCentralSpires(m_seed);
    for (const auto& sp : spires) {
        if (sp.height < 22.0f) continue; // низкие подводные скалы далеко внизу
        float dx = m_whale.pos.x() - sp.x;
        float dz = m_whale.pos.z() - sp.z;
        float safeR = sp.radius + 18.0f;
        float d2 = dx * dx + dz * dz;
        if (d2 < safeR * safeR && d2 > 1e-4f) {
            float d = std::sqrt(d2);
            float push = std::min((safeR - d), 10.0f * dt);
            m_whale.pos.setX(m_whale.pos.x() + (dx / d) * push);
            m_whale.pos.setZ(m_whale.pos.z() + (dz / d) * push);
        }
    }

    // Страховка вертикального коридора (выше рельефа, ниже поверхности воды)
    float curFloor = ProcMath::sampleTerrainHeight(m_whale.pos.x(), m_whale.pos.z(), m_seed);
    if (m_whale.pos.y() < curFloor + 6.5f) {
        m_whale.pos.setY(std::min(curFloor + 6.5f, SURFACE_LIMIT_Y));
    }
    if (m_whale.pos.y() > SURFACE_LIMIT_Y) {
        m_whale.pos.setY(SURFACE_LIMIT_Y);
    }

    // Страховка радиуса кальдеры
    float curR = std::sqrt(m_whale.pos.x() * m_whale.pos.x() + m_whale.pos.z() * m_whale.pos.z());
    float hardWhaleR = ProcMath::BASIN_INNER_RADIUS - 55.0f;
    if (curR > hardWhaleR) {
        float k = hardWhaleR / curR;
        m_whale.pos.setX(m_whale.pos.x() * k);
        m_whale.pos.setZ(m_whale.pos.z() * k);
    }

    // Периодическая песня кита
    m_whaleSongTimer -= dt;
    if (m_whaleSongTimer <= 0.0f) {
        m_whaleSongTimer = 24.0f + std::fmod(m_time * 3.7f, 14.0f);
        if (m_audioEngine && (m_whale.pos - m_camPos).length() < 340.0f) {
            m_audioEngine->triggerWhaleCall();
        }
    }
}

void DeepDiveWidget::updateFaunaAgents(float dt) {
    auto stepGroup = [&](std::vector<FaunaAgent>& agents,
                         float lookAhead, float avoidMargin,
                         float floorMargin, float surfaceMargin,
                         float verticalAmp, float turnSmooth, float maxBank, bool fleePlayer) {
        for (auto& a : agents) {
            QVector3D fwd = a.vel.lengthSquared() > 1e-4f
                                ? a.vel.normalized()
                                : QVector3D(std::sin(a.yaw), 0.0f, std::cos(a.yaw));

            QVector3D toHome = a.homeCenter - a.pos;
            QVector3D horizToHome(toHome.x(), 0.0f, toHome.z());
            float homeDist = horizToHome.length();

            QVector3D steer(0.0f, 0.0f, 0.0f);
            if (homeDist > 0.5f) {
                QVector3D radial = horizToHome / homeDist;
                QVector3D tangent(-radial.z(), 0.0f, radial.x());
                steer += tangent * 3.2f;
                if (homeDist > a.homeRadius) {
                    float pull = (homeDist - a.homeRadius) / std::max(a.homeRadius, 1.0f);
                    steer += radial * (pull * 14.0f + 2.5f);
                }
            }

            float swirl = std::sin(m_time * 0.85f + a.phase) * 1.4f;
            QVector3D side(-fwd.z(), 0.0f, fwd.x());
            steer += side * swirl;

            float targetY = a.preferredDepth + std::sin(m_time * 0.9f + a.phase) * verticalAmp;
            steer.setY(steer.y() + (targetY - a.pos.y()) * 1.3f);

            if (fleePlayer) {
                QVector3D fromCam = a.pos - m_camPos;
                float camDistSq = fromCam.lengthSquared();
                if (camDistSq < 49.0f && camDistSq > 1e-4f) {
                    float d = std::sqrt(camDistSq);
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
            float targetSpd = ProcMath::mix(spd, a.cruiseSpeed, ProcMath::clamp(dt * 3.5f, 0.0f, 1.0f));
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

            float horizSpd = std::sqrt(a.vel.x() * a.vel.x() + a.vel.z() * a.vel.z());
            float targetYaw = std::atan2(a.vel.x(), a.vel.z());
            float dYaw = wrapAngle(targetYaw - a.yaw);
            float k = ProcMath::clamp(dt * turnSmooth, 0.0f, 1.0f);
            a.yaw = wrapAngle(a.yaw + dYaw * k);

            float targetPitch = ProcMath::clamp(std::atan2(a.vel.y(), std::max(horizSpd, 0.2f)), -0.55f, 0.55f);
            a.pitch = ProcMath::mix(a.pitch, targetPitch, k);

            float targetBank = ProcMath::clamp(-dYaw * 0.9f, -maxBank, maxBank);
            a.bank = ProcMath::mix(a.bank, targetBank, ProcMath::clamp(dt * 4.0f, 0.0f, 1.0f));
        }
    };

    stepGroup(m_world.fishAgents,  4.8f, 3.2f, 1.1f, 2.5f, 0.75f, 7.5f, 0.40f, true);
    stepGroup(m_world.mantaAgents, 11.5f, 6.5f, 2.4f, 4.0f, 1.35f, 4.2f, 0.34f, false);
}

QVector3D DeepDiveWidget::computeAnglerLureWorldPos(const FaunaAgent& a) const {
    QVector3D localPos(0.0f, 0.84f, 1.38f);

    float lureW = ProcMath::smoothstep(0.45f, 1.35f, localPos.z())
                  * ProcMath::smoothstep(0.15f, 0.85f, localPos.y());
    localPos.setX(localPos.x() + std::sin(m_time * 2.1f + a.phase) * 0.09f * lureW);
    localPos.setY(localPos.y() + std::cos(m_time * 1.7f + a.phase) * 0.07f * lureW);
    localPos *= a.scale;

    const float cy = std::cos(a.yaw),   sy = std::sin(a.yaw);
    const float cp = std::cos(a.pitch), sp = std::sin(a.pitch);
    const float cb = std::cos(a.bank),  sb = std::sin(a.bank);

    QVector3D v1(cb * localPos.x() - sb * localPos.y(),
                 sb * localPos.x() + cb * localPos.y(),
                 localPos.z());
    QVector3D v2(v1.x(),
                 cp * v1.y() + sp * v1.z(),
                 -sp * v1.y() + cp * v1.z());
    QVector3D v3(cy * v2.x() + sy * v2.z(),
                 v2.y(),
                 -sy * v2.x() + cy * v2.z());

    return v3 + a.pos;
}

void DeepDiveWidget::updateAnglerAgents(float dt) {
    constexpr float DETECT_RADIUS   = 60.0f;
    constexpr float AMBUSH_MIN_DIST = 10.0f;
    constexpr float AMBUSH_MAX_DIST = 14.0f;

    for (auto& a : m_world.anglerAgents) {
        QVector3D fwd(std::sin(a.yaw) * std::cos(a.pitch),
                      std::sin(a.pitch),
                      std::cos(a.yaw) * std::cos(a.pitch));
        if (fwd.lengthSquared() > 1e-4f) fwd.normalize();
        else fwd = QVector3D(0.0f, 0.0f, 1.0f);

        QVector3D toCam = m_camPos - a.pos;
        float distToCam = toCam.length();
        QVector3D toHome = a.homeCenter - a.pos;
        QVector3D horizToHome(toHome.x(), 0.0f, toHome.z());
        float homeDist = horizToHome.length();

        float floorY = ProcMath::sampleTerrainHeight(a.pos.x(), a.pos.z(), m_seed);
        float maxPitY = std::min(a.homeCenter.y() + 14.0f, -24.0f);
        bool stalking = (distToCam < DETECT_RADIUS);

        QVector3D steer(0.0f, 0.0f, 0.0f);
        float targetSpeed = a.cruiseSpeed;

        if (stalking) {
            QVector3D dirToCam = toCam / std::max(distToCam, 0.001f);
            QVector3D side(-dirToCam.z(), 0.0f, dirToCam.x());
            if (side.lengthSquared() > 1e-4f) side.normalize();

            if (distToCam > AMBUSH_MAX_DIST) {
                float pull = ProcMath::clamp((distToCam - AMBUSH_MAX_DIST) / 14.0f, 0.25f, 1.0f);
                steer += dirToCam * (2.8f * pull);
                targetSpeed = a.cruiseSpeed * (0.75f + 0.55f * pull);
            } else if (distToCam < AMBUSH_MIN_DIST) {
                float push = ProcMath::clamp((AMBUSH_MIN_DIST - distToCam) / 5.0f, 0.2f, 1.2f);
                steer -= dirToCam * (3.6f * push);
                steer += side * (std::sin(m_time * 0.6f + a.phase) * 1.1f);
                targetSpeed = a.cruiseSpeed * 0.65f;
            } else {
                steer += side * (std::sin(m_time * 0.55f + a.phase) * 0.85f);
                targetSpeed = 0.45f;
            }

            float stalkMaxR = a.homeRadius * 1.45f;
            if (homeDist > stalkMaxR) {
                QVector3D radial = horizToHome / std::max(homeDist, 0.01f);
                float excess = (homeDist - stalkMaxR) / std::max(a.homeRadius, 1.0f);
                steer += radial * (excess * 14.0f + 3.0f);
            }

            float hoverY = ProcMath::clamp(m_camPos.y() - 0.8f, floorY + 4.4f, maxPitY);
            steer.setY(steer.y() + (hoverY - a.pos.y()) * 0.85f);
        } else {
            if (homeDist > 0.5f) {
                QVector3D radial = horizToHome / homeDist;
                QVector3D tangent(-radial.z(), 0.0f, radial.x());
                steer += tangent * 1.85f;
                if (homeDist > a.homeRadius) {
                    float pull = (homeDist - a.homeRadius) / std::max(a.homeRadius, 1.0f);
                    steer += radial * (pull * 10.0f + 2.2f);
                }
            } else {
                steer += QVector3D(std::cos(a.phase), 0.0f, std::sin(a.phase)) * 1.5f;
            }

            QVector3D side(-fwd.z(), 0.0f, fwd.x());
            steer += side * (std::sin(m_time * 0.45f + a.phase) * 0.75f);

            float patrolY = ProcMath::clamp(
                std::max(a.preferredDepth, floorY + 4.8f) + std::sin(m_time * 0.55f + a.phase) * 1.4f,
                floorY + 4.2f, maxPitY);
            steer.setY(steer.y() + (patrolY - a.pos.y()) * 0.95f);
        }

        steer += m_world.collision.computeAvoidanceForce(a.pos, a.boundingRadius, 4.8f) * 1.15f;
        steer += m_world.collision.computeAvoidanceForce(a.pos + fwd * 6.5f, a.boundingRadius, 5.2f) * 0.75f;

        a.vel += steer * dt;
        a.vel *= std::exp(-1.4f * dt);

        float spd = a.vel.length();
        if (spd > targetSpeed && spd > 1e-4f) {
            float newSpd = ProcMath::mix(spd, targetSpeed, ProcMath::clamp(dt * 2.8f, 0.0f, 1.0f));
            a.vel = (a.vel / spd) * newSpd;
        } else if (!stalking && spd < a.cruiseSpeed * 0.55f) {
            QVector3D dir = (spd > 1e-3f) ? (a.vel / spd) : fwd;
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
            float hDist = std::sqrt(toCam.x() * toCam.x() + toCam.z() * toCam.z());
            targetYaw   = std::atan2(toCam.x(), toCam.z());
            targetPitch = ProcMath::clamp(std::atan2(toCam.y(), std::max(hDist, 0.3f)), -0.45f, 0.45f);
        } else if (a.vel.lengthSquared() > 0.04f) {
            float hSpd  = std::sqrt(a.vel.x() * a.vel.x() + a.vel.z() * a.vel.z());
            targetYaw   = std::atan2(a.vel.x(), a.vel.z());
            targetPitch = ProcMath::clamp(std::atan2(a.vel.y(), std::max(hSpd, 0.25f)), -0.32f, 0.32f);
        }

        float dYaw = wrapAngle(targetYaw - a.yaw);
        float turnK = ProcMath::clamp(dt * (stalking ? 2.4f : 1.8f), 0.0f, 1.0f);
        a.yaw   = wrapAngle(a.yaw + dYaw * turnK);
        a.pitch = ProcMath::mix(a.pitch, targetPitch, turnK);
        float targetBank = ProcMath::clamp(-dYaw * 0.45f, -0.18f, 0.18f);
        a.bank  = ProcMath::mix(a.bank, targetBank, ProcMath::clamp(dt * 2.2f, 0.0f, 1.0f));
    }

    std::vector<std::pair<float, QVector3D>> lures;
    lures.reserve(m_world.anglerAgents.size());
    for (const auto& a : m_world.anglerAgents) {
        QVector3D lurePos = computeAnglerLureWorldPos(a);
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

void DeepDiveWidget::cullAndUploadFauna(const FrustumPlanes& frustum) {
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
        float dist = (f.pos - m_camPos).length();
        if (dist > FISH_MAX_DIST) continue;
        if (!frustum.intersectsSphere(f.pos, f.boundingRadius * 1.4f)) continue;

        uint8_t& st = m_fishLodState[i];
        st = updateBinaryLod(st, dist, FISH_LOD0_DIST, 6.0f);
        if (st == 0) m_visFishLod0.push_back(packFaunaInstance(f));
        else         m_visFishLod1.push_back(packFaunaInstance(f));
    }

    for (size_t i = 0; i < m_world.mantaAgents.size(); ++i) {
        const auto& m = m_world.mantaAgents[i];
        float dist = (m.pos - m_camPos).length();
        if (dist > MANTA_MAX_DIST) continue;
        if (!frustum.intersectsSphere(m.pos, m.boundingRadius * 1.6f)) continue;

        uint8_t& st = m_mantaLodState[i];
        st = updateBinaryLod(st, dist, MANTA_LOD0_DIST, 8.0f);
        if (st == 0) m_visMantaLod0.push_back(packFaunaInstance(m));
        else         m_visMantaLod1.push_back(packFaunaInstance(m));
    }

    {
        float whaleDist = (m_whale.pos - m_camPos).length();
        if (whaleDist <= WHALE_MAX_DIST && frustum.intersectsSphere(m_whale.pos, 18.0f)) {
            m_whaleLodState = updateBinaryLod(m_whaleLodState, whaleDist, WHALE_LOD0_DIST, 12.0f);
            if (m_whaleLodState == 0) m_visWhaleLod0.push_back(packFaunaInstance(m_whale));
            else                      m_visWhaleLod1.push_back(packFaunaInstance(m_whale));
        }
    }

    for (size_t i = 0; i < m_world.anglerAgents.size(); ++i) {
        const auto& a = m_world.anglerAgents[i];
        float dist = (a.pos - m_camPos).length();
        if (dist > ANGLER_MAX_DIST) continue;
        if (!frustum.intersectsSphere(a.pos, a.boundingRadius * 2.0f)) continue;

        uint8_t& st = m_anglerLodState[i];
        st = updateBinaryLod(st, dist, ANGLER_LOD0_DIST, 8.0f);
        if (st == 0) m_visAnglerLod0.push_back(packFaunaInstance(a));
        else         m_visAnglerLod1.push_back(packFaunaInstance(a));
    }

    auto upload = [&](FaunaLodBuffers& buf, const std::vector<FaunaGpuInstance>& list) {
        if (list.empty() || buf.instVbo == 0) return;
        size_t count = std::min(list.size(), static_cast<size_t>(buf.maxInstances));
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

void DeepDiveWidget::resizeGL(int w, int h) {
    qreal dpr = devicePixelRatioF();
    resizePostProcessBuffers(int(w * dpr), int(h * dpr));
}

QVector3D DeepDiveWidget::cameraForward() const {
    return QVector3D(
               std::sin(m_yaw) * std::cos(m_pitch),
               std::sin(m_pitch),
               -std::cos(m_yaw) * std::cos(m_pitch)
               ).normalized();
}

QVector3D DeepDiveWidget::cameraRight() const {
    return QVector3D(std::cos(m_yaw), 0.0f, std::sin(m_yaw)).normalized();
}

void DeepDiveWidget::updateSimulation(float dt) {
    QVector3D wishDir(0.0f, 0.0f, 0.0f);
    QVector3D fwd = cameraForward();
    QVector3D rgt = cameraRight();

    if (m_pressedKeys.contains(Qt::Key_W)) wishDir += fwd;
    if (m_pressedKeys.contains(Qt::Key_S)) wishDir -= fwd;
    if (m_pressedKeys.contains(Qt::Key_D)) wishDir += rgt;
    if (m_pressedKeys.contains(Qt::Key_A)) wishDir -= rgt;
    if (m_pressedKeys.contains(Qt::Key_Space)) wishDir += QVector3D(0.0f, 1.0f, 0.0f);
    if (m_pressedKeys.contains(Qt::Key_Control)) wishDir -= QVector3D(0.0f, 1.0f, 0.0f);

    if (wishDir.lengthSquared() > 0.001f) wishDir.normalize();
    float accel = m_pressedKeys.contains(Qt::Key_Shift) ? 58.0f : 18.0f;

    m_camVel += wishDir * (accel * dt);
    m_camVel *= std::exp(-3.0f * dt);
    m_camPos += m_camVel * dt;
    m_camPos.setY(m_camPos.y() + std::sin(m_time * 1.4f) * 0.006f);

    m_stats.playerColliding = m_world.collision.resolveSphereCollision(
        m_camPos, &m_camVel, 1.45f, 0.45f, 1.6f);

    if (m_audioEngine) {
        int audioBiome = (m_paletteIndex == 0) ? 0 : (m_paletteIndex - 1);
        m_audioEngine->updateState(m_camPos.y(), m_camVel.length(), audioBiome);
    }

    updateFaunaAgents(dt);
    updateWhaleNavigation(dt);
    updateAnglerAgents(dt);

    constexpr float BOX_SPAN_XZ = 160.0f;
    constexpr float BOX_HALF_XZ = BOX_SPAN_XZ * 0.5f;
    constexpr float BOX_SPAN_Y  = 56.0f;
    constexpr float BOX_HALF_Y  = BOX_SPAN_Y * 0.5f;

    float yCenter = std::min(m_camPos.y(), ProcMath::WATER_SURFACE_Y - BOX_HALF_Y - 1.5f);

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
        float nextX = p.pos.x() + std::sin(m_time * 1.8f + p.phase) * 0.25f * dt;
        float nextY = p.pos.y() + p.speedY * dt;
        p.pos.setX(wrapAround(nextX, m_camPos.x(), BOX_SPAN_XZ, BOX_HALF_XZ));
        p.pos.setY(wrapAround(nextY, yCenter,      BOX_SPAN_Y,  BOX_HALF_Y));
        p.pos.setZ(wrapAround(p.pos.z(), m_camPos.z(), BOX_SPAN_XZ, BOX_HALF_XZ));
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_particleVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    m_world.particles.size() * sizeof(Particle),
                    m_world.particles.data());
}

void DeepDiveWidget::paintGL() {
    qint64 nowNs = m_clock.nsecsElapsed();
    float dt = (m_lastFrameNs == 0) ? 0.016f : (nowNs - m_lastFrameNs) * 1e-9f;
    m_lastFrameNs = nowNs;
    dt = std::min(dt, 0.05f);
    m_time += dt;
    m_fps = m_fps * 0.92f + (1.0f / std::max(dt, 0.0001f)) * 0.08f;

    updateSimulation(dt);

    qreal dpr = devicePixelRatioF();
    int physW = std::max(64, int(width() * dpr));
    int physH = std::max(64, int(height() * dpr));
    resizePostProcessBuffers(physW, physH);

    // 4 биома: 0 = яркий сочный тропический риф (по умолчанию)
    QVector3D waterShallow, waterDeep, sunColor;
    float vibrance = 1.0f;
    float fogScale = 1.0f;

    if (m_paletteIndex == 0) {
        // Tropical Reef (Vibrant): сочный риф в плотной прозрачной бирюзе без воздушного пересвета
        waterShallow = QVector3D(0.06f, 0.54f, 0.64f);
        waterDeep    = QVector3D(0.01f, 0.16f, 0.30f);
        sunColor     = QVector3D(0.68f, 0.94f, 0.90f);
        vibrance     = 1.32f;
        fogScale     = 1.00f;
    } else if (m_paletteIndex == 1) {
        // Kelp Reef (Emerald)
        waterShallow = QVector3D(0.08f, 0.48f, 0.52f);
        waterDeep    = QVector3D(0.015f, 0.14f, 0.21f);
        sunColor     = QVector3D(0.50f, 0.88f, 0.90f);
        vibrance     = 1.10f;
        fogScale     = 1.10f;
    } else if (m_paletteIndex == 2) {
        // Purple Grotto
        waterShallow = QVector3D(0.28f, 0.30f, 0.56f);
        waterDeep    = QVector3D(0.08f, 0.05f, 0.19f);
        sunColor     = QVector3D(0.62f, 0.72f, 0.96f);
        vibrance     = 1.18f;
        fogScale     = 1.12f;
    } else {
        // Abyssal Trench
        waterShallow = QVector3D(0.05f, 0.28f, 0.40f);
        waterDeep    = QVector3D(0.008f, 0.05f, 0.11f);
        sunColor     = QVector3D(0.38f, 0.74f, 0.88f);
        vibrance     = 1.04f;
        fogScale     = 1.25f;
    }

    QVector3D sunDir = QVector3D(-0.28f, 0.92f, -0.22f).normalized();

    QMatrix4x4 proj;
    float aspect = float(physW) / float(physH);
    proj.perspective(65.0f, aspect, 0.25f, 460.0f);

    QMatrix4x4 view;
    view.lookAt(m_camPos, m_camPos + cameraForward(), QVector3D(0.0f, 1.0f, 0.0f));
    QMatrix4x4 viewProj = proj * view;
    QMatrix4x4 invViewProj = viewProj.inverted();

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
        GLsizei clamped = static_cast<GLsizei>(std::min(instanceCount, static_cast<size_t>(buf.maxInstances)));
        glBindVertexArray(buf.vao);
        glDrawElementsInstanced(GL_TRIANGLES, buf.indexCount, GL_UNSIGNED_INT, nullptr, clamped);
        if (countTris) {
            m_stats.drawnTriangles += (buf.indexCount / 3) * clamped;
        }
    };

    // ========================================================================
    // ЭТАП 0: Направленная карта теней (Shadow Map Pass)
    // ========================================================================
    const float orthoHalfSize = 110.0f;
    const float texelWorldStep = (orthoHalfSize * 2.0f) / SHADOW_MAP_RES;
    QVector3D shadowFocus(
        std::floor(m_camPos.x() / texelWorldStep) * texelWorldStep,
        0.0f,
        std::floor(m_camPos.z() / texelWorldStep) * texelWorldStep
        );

    QMatrix4x4 lightProj;
    lightProj.ortho(-orthoHalfSize, orthoHalfSize, -orthoHalfSize, orthoHalfSize, 1.0f, 270.0f);
    QMatrix4x4 lightView;
    lightView.lookAt(shadowFocus + sunDir * 125.0f, shadowFocus, QVector3D(0.0f, 0.0f, 1.0f));
    QMatrix4x4 lightSpaceMatrix = lightProj * lightView;

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

            float d = distanceToAABB(m_camPos, chunk.minBounds, chunk.maxBounds);
            shadowChunks.emplace_back(i, d);

            int sLod = (d < 135.0f) ? 1 : (d < 240.0f ? 2 : 3);
            const auto& range = m_world.terrainLodRanges[sLod];
            const void* idxPtr = reinterpret_cast<const void*>(
                static_cast<uintptr_t>(range.indexOffset) * sizeof(uint32_t));
            glDrawElementsBaseVertex(GL_TRIANGLES,
                                     static_cast<GLsizei>(range.indexCount),
                                     GL_UNSIGNED_INT, idxPtr,
                                     static_cast<GLint>(chunk.terrainBaseVertex));
        }

        glBindVertexArray(m_propVAO);
        for (const auto& [idx, d] : shadowChunks) {
            const auto& chunk = m_world.chunks[idx];
            drawPropRange((d < 140.0f) ? chunk.boulderLod0 : chunk.boulderLod1, false);
            drawPropRange((d < 120.0f) ? chunk.kelpLod0    : chunk.kelpLod1,    false);
            if (d < 95.0f)       drawPropRange(chunk.floraLod0, false);
            else if (d < 155.0f) drawPropRange(chunk.floraLod1, false);
            drawPropRange(chunk.shipwreckRange, false);
        }

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
    // ЭТАП 1: Основной рендер сцены в MSAA HDR FBO
    // ========================================================================
    m_stats.visibleChunks = 0;
    m_stats.totalChunks = static_cast<int>(m_world.chunks.size());
    m_stats.lodCounts[0] = m_stats.lodCounts[1] = m_stats.lodCounts[2] = m_stats.lodCounts[3] = 0;
    m_stats.drawnTriangles = 0;

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
    m_skyShader.setUniformValue("uRockTex", 0);
    m_skyShader.setUniformValue("uInvViewProj", invViewProj);
    m_skyShader.setUniformValue("uCamPos", m_camPos);
    m_skyShader.setUniformValue("uSunDir", sunDir);
    m_skyShader.setUniformValue("uSunColor", sunColor);
    m_skyShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_skyShader.setUniformValue("uWaterColorShallow", waterShallow);
    m_skyShader.setUniformValue("uTime", m_time);
    glBindVertexArray(m_skyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (m_wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    glDepthFunc(GL_LESS);
    m_envShader.bind();
    m_envShader.setUniformValue("uRockTex", 0);
    m_envShader.setUniformValue("uShadowMap", 3);
    m_envShader.setUniformValue("uViewProj", viewProj);
    m_envShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
    m_envShader.setUniformValue("uCamPos", m_camPos);
    m_envShader.setUniformValue("uSunDir", sunDir);
    m_envShader.setUniformValue("uSunColor", sunColor);
    m_envShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_envShader.setUniformValue("uWaterColorShallow", waterShallow);
    m_envShader.setUniformValue("uVibrance", vibrance);
    m_envShader.setUniformValue("uFogScale", fogScale);
    m_envShader.setUniformValue("uTime", m_time);
    m_envShader.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);
    m_envShader.setUniformValue("uLureCount", m_lureCount);
    if (m_lureCount > 0) {
        m_envShader.setUniformValueArray("uLurePos", m_lurePositions.data(), m_lureCount);
    }

    static constexpr float TERRAIN_LOD_THRESH[3] = {210.0f, 310.0f, 395.0f};
    static constexpr float BOULDER_LOD_THRESH[2] = {265.0f, 420.0f};
    static constexpr float KELP_LOD_THRESH[2]    = {190.0f, 320.0f};
    static constexpr float FLORA_LOD_THRESH[2]   = {185.0f, 315.0f};

    std::vector<std::pair<int, float>> visibleChunks;
    visibleChunks.reserve(128);

    glBindVertexArray(m_terrainVAO);
    for (int i = 0; i < static_cast<int>(m_world.chunks.size()); ++i) {
        const auto& chunk = m_world.chunks[i];
        if (!camFrustum.intersectsSphere(chunk.center, chunk.boundingRadius)) continue;

        float d = distanceToAABB(m_camPos, chunk.minBounds, chunk.maxBounds);
        if (d > 460.0f) continue;

        ChunkLodState& st = m_chunkLodStates[i];
        st.terrainLod = updateLodWithHysteresis(st.terrainLod, d, TERRAIN_LOD_THRESH, 3, 8.0f);

        m_stats.visibleChunks++;
        m_stats.lodCounts[st.terrainLod]++;
        visibleChunks.emplace_back(i, d);

        const auto& range = m_world.terrainLodRanges[st.terrainLod];
        const void* idxPtr = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(range.indexOffset) * sizeof(uint32_t));
        glDrawElementsBaseVertex(GL_TRIANGLES,
                                 static_cast<GLsizei>(range.indexCount),
                                 GL_UNSIGNED_INT, idxPtr,
                                 static_cast<GLint>(chunk.terrainBaseVertex));
        m_stats.drawnTriangles += static_cast<int>(range.indexCount / 3);
    }

    glBindVertexArray(m_propVAO);
    for (const auto& [idx, d] : visibleChunks) {
        const auto& chunk = m_world.chunks[idx];
        ChunkLodState& st = m_chunkLodStates[idx];

        st.boulderLod = updateLodWithHysteresis(st.boulderLod, d, BOULDER_LOD_THRESH, 2, 10.0f);
        if (st.boulderLod == 0)      drawPropRange(chunk.boulderLod0, true);
        else if (st.boulderLod == 1) drawPropRange(chunk.boulderLod1, true);

        st.kelpLod = updateLodWithHysteresis(st.kelpLod, d, KELP_LOD_THRESH, 2, 8.0f);
        if (st.kelpLod == 0)         drawPropRange(chunk.kelpLod0, true);
        else if (st.kelpLod == 1)    drawPropRange(chunk.kelpLod1, true);

        st.floraLod = updateLodWithHysteresis(st.floraLod, d, FLORA_LOD_THRESH, 2, 8.0f);
        if (st.floraLod == 0)        drawPropRange(chunk.floraLod0, true);
        else if (st.floraLod == 1)   drawPropRange(chunk.floraLod1, true);

        if (st.detailVisible && d > 192.0f)       st.detailVisible = false;
        else if (!st.detailVisible && d < 176.0f) st.detailVisible = true;
        if (st.detailVisible) drawPropRange(chunk.detailRange, true);

        if (d < 430.0f) drawPropRange(chunk.shipwreckRange, true);
    }

    m_faunaShader.bind();
    m_faunaShader.setUniformValue("uRockTex", 0);
    m_faunaShader.setUniformValue("uShadowMap", 3);
    m_faunaShader.setUniformValue("uViewProj", viewProj);
    m_faunaShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
    m_faunaShader.setUniformValue("uCamPos", m_camPos);
    m_faunaShader.setUniformValue("uSunDir", sunDir);
    m_faunaShader.setUniformValue("uSunColor", sunColor);
    m_faunaShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_faunaShader.setUniformValue("uWaterColorShallow", waterShallow);
    m_faunaShader.setUniformValue("uVibrance", vibrance);
    m_faunaShader.setUniformValue("uFogScale", fogScale);
    m_faunaShader.setUniformValue("uTime", m_time);
    m_faunaShader.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);
    m_faunaShader.setUniformValue("uLureCount", m_lureCount);
    if (m_lureCount > 0) {
        m_faunaShader.setUniformValueArray("uLurePos", m_lurePositions.data(), m_lureCount);
    }

    drawFaunaBatch(m_fishLod0,   m_visFishLod0.size(),   true);
    drawFaunaBatch(m_fishLod1,   m_visFishLod1.size(),   true);
    drawFaunaBatch(m_mantaLod0,  m_visMantaLod0.size(),  true);
    drawFaunaBatch(m_mantaLod1,  m_visMantaLod1.size(),  true);
    drawFaunaBatch(m_whaleLod0,  m_visWhaleLod0.size(),  true);
    drawFaunaBatch(m_whaleLod1,  m_visWhaleLod1.size(),  true);
    drawFaunaBatch(m_anglerLod0, m_visAnglerLod0.size(), true);
    drawFaunaBatch(m_anglerLod1, m_visAnglerLod1.size(), true);

    // Реальная 3D-поверхность воды на уровне y = 28 м (отсекает скалы выше глади и рисует границу сред)
    QVector2D waterCenter(std::floor(m_camPos.x() * 2.0f) * 0.5f,
                          std::floor(m_camPos.z() * 2.0f) * 0.5f);
    m_waterShader.bind();
    m_waterShader.setUniformValue("uRockTex", 0);
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
    glBindVertexArray(m_waterVAO);
    glDrawElements(GL_TRIANGLES, m_waterIndexCount, GL_UNSIGNED_INT, nullptr);
    m_stats.drawnTriangles += static_cast<int>(m_waterIndexCount / 3);

    if (m_wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);

    m_rayShader.bind();
    m_rayShader.setUniformValue("uViewProj", viewProj);
    m_rayShader.setUniformValue("uCamPos", m_camPos);
    m_rayShader.setUniformValue("uTime", m_time);
    glBindVertexArray(m_rayVAO);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_world.godRayVertices.size()));

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
    // ЭТАП 2: Resolve MSAA + Двухпроходный Bloom + Композитинг PostFX
    // ========================================================================
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFBO);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBlitFramebuffer(0, 0, physW, physH, 0, 0, physW, physH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glDisable(GL_DEPTH_TEST);
    int halfW = std::max(32, physW / 2);
    int halfH = std::max(32, physH / 2);
    glViewport(0, 0, halfW, halfH);

    m_scatterBlurShader.bind();
    m_scatterBlurShader.setUniformValue("uSourceTex", 0);
    glBindVertexArray(m_skyVAO);

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

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glViewport(0, 0, physW, physH);

    QVector4D sunClip = viewProj * QVector4D(m_camPos + sunDir * 100.0f, 1.0f);
    QVector3D sunScreen(0.5f, 0.85f, 0.0f);
    if (sunClip.w() > 0.001f) {
        QVector2D ndc(sunClip.x() / sunClip.w(), sunClip.y() / sunClip.w());
        sunScreen.setX(ndc.x() * 0.5f + 0.5f);
        sunScreen.setY(ndc.y() * 0.5f + 0.5f);
        float facing = QVector3D::dotProduct(cameraForward(), sunDir);
        sunScreen.setZ(ProcMath::smoothstep(-0.15f, 0.65f, facing));
    }

    m_postProcessShader.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_bloomTex[1]);

    float distToSurface = std::max(0.4f, ProcMath::WATER_SURFACE_Y - m_camPos.y());
    float surfaceProximity = 1.0f - ProcMath::smoothstep(1.8f, 16.0f, distToSurface);

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

    // ========================================================================
    // HUD (скрывается / показывается по клавише ` или ё)
    // ========================================================================
    if (m_hudVisible) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setPen(QColor(210, 245, 255, 230));
        painter.setFont(QFont("Consolas", 10, QFont::Bold));

        const char* biomeNames[] = {
            "Tropical Reef (Vibrant)",
            "Kelp Reef (Emerald)",
            "Purple Grotto",
            "Abyssal Trench"
        };
        QString audioStatus = (m_audioEngine && !m_audioEngine->isMuted())
                                  ? QString("ON (%1%)").arg(qRound(m_audioEngine->volume() * 100.0f))
                                  : QString("MUTED");

        float radialDist = std::sqrt(m_camPos.x() * m_camPos.x() + m_camPos.z() * m_camPos.z());
        float diveDepth  = std::max(0.0f, ProcMath::WATER_SURFACE_Y - m_camPos.y());

        QString hud = QString(
                          "GPU: %1 | %2x MSAA | Shadows: %3 | PostFX: %4 | Audio: %5 | FPS: %6\n"
                          "Chunks: %7/%8 (LOD0:%9 LOD1:%10 LOD2:%11 LOD3:%12) | Tris: %13k | Fish: %14+%15 | Mantas: %16+%17 | Anglers: %18\n"
                          "Depth: %19m | R: %20m %21 | Seed: %22 | Biome: %23 | [`] HUD [B] Biome [R] Regen [L] Shad [P] Post [F] Wire [K] Whale [M] Mute"
                          ).arg(m_glRendererInfo)
                          .arg(m_activeSamples)
                          .arg(m_shadowsEnabled ? "ON" : "OFF")
                          .arg(m_postProcessEnabled ? "ON" : "OFF")
                          .arg(audioStatus)
                          .arg(qRound(m_fps))
                          .arg(m_stats.visibleChunks)
                          .arg(m_stats.totalChunks)
                          .arg(m_stats.lodCounts[0])
                          .arg(m_stats.lodCounts[1])
                          .arg(m_stats.lodCounts[2])
                          .arg(m_stats.lodCounts[3])
                          .arg(m_stats.drawnTriangles / 1000.0, 0, 'f', 1)
                          .arg(m_stats.visibleFishLod0)
                          .arg(m_stats.visibleFishLod1)
                          .arg(m_stats.visibleMantaLod0)
                          .arg(m_stats.visibleMantaLod1)
                          .arg(m_stats.visibleAnglers)
                          .arg(qRound(diveDepth))
                          .arg(qRound(radialDist))
                          .arg(m_stats.playerColliding ? "[COLLISION]" : "")
                          .arg(m_seed)
                          .arg(biomeNames[m_paletteIndex]);

        painter.fillRect(QRect(14, 14, 1220, 68), QColor(5, 22, 32, 165));
        painter.drawText(QRect(24, 18, 1200, 60), Qt::AlignLeft | Qt::AlignVCenter, hud);
    }
}

void DeepDiveWidget::keyPressEvent(QKeyEvent* event) {
    // Переключение HUD по клавише ` / ~ (и русской ё / Ё)
    const QString txt = event->text();
    const bool isHudToggle = (event->key() == Qt::Key_QuoteLeft ||
                              event->key() == Qt::Key_AsciiTilde ||
                              txt == QLatin1String("`") ||
                              txt == QLatin1String("~") ||
                              (!txt.isEmpty() && (txt[0] == QChar(0x0451) || txt[0] == QChar(0x0401))));
    if (isHudToggle) {
        m_hudVisible = !m_hudVisible;
        return;
    }

    m_pressedKeys.insert(event->key());
    if (event->key() == Qt::Key_B) {
        m_paletteIndex = (m_paletteIndex + 1) % 4;
    } else if (event->key() == Qt::Key_R) {
        m_seed += 17u;
        makeCurrent();
        rebuildWorld(m_seed);
        doneCurrent();
    } else if (event->key() == Qt::Key_P) {
        m_postProcessEnabled = !m_postProcessEnabled;
    } else if (event->key() == Qt::Key_L) {
        m_shadowsEnabled = !m_shadowsEnabled;
    } else if (event->key() == Qt::Key_F) {
        m_wireframeMode = !m_wireframeMode;
    } else if (event->key() == Qt::Key_V) {
        m_vsyncEnabled = !m_vsyncEnabled;
        m_timer.setInterval(m_vsyncEnabled ? 0 : 1);
    } else if (event->key() == Qt::Key_M) {
        if (m_audioEngine) m_audioEngine->toggleMute();
    } else if (event->key() == Qt::Key_K) {
        spawnWhalePass(true);
        if (m_audioEngine) m_audioEngine->triggerWhaleCall();
    } else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
        if (m_audioEngine) m_audioEngine->adjustVolume(0.05f);
    } else if (event->key() == Qt::Key_Minus) {
        if (m_audioEngine) m_audioEngine->adjustVolume(-0.05f);
    } else if (event->key() == Qt::Key_Escape) {
        close();
    }
}

void DeepDiveWidget::keyReleaseEvent(QKeyEvent* event) {
    m_pressedKeys.remove(event->key());
}

void DeepDiveWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_mouseLooking = true;
        m_lastMousePos = event->pos();
        setCursor(Qt::BlankCursor);
    }
}

void DeepDiveWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_mouseLooking) return;
    QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    m_yaw   += delta.x() * 0.0035f;
    m_pitch -= delta.y() * 0.0035f;
    m_pitch  = ProcMath::clamp(m_pitch, -1.48f, 1.48f);
}

void DeepDiveWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_mouseLooking = false;
        unsetCursor();
    }
}
