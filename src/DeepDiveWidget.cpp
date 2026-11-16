#include "DeepDiveWidget.h"
#include "Shaders.h"
#include "Noise.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QOpenGLContext>
#include <QSurface>
#include <thread>
#include <cmath>

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
    glDeleteVertexArrays(1, &m_envVAO);
    glDeleteBuffers(1, &m_envVBO);
    glDeleteBuffers(1, &m_envEBO);
    glDeleteVertexArrays(1, &m_fishVAO);
    glDeleteBuffers(1, &m_fishVBO);
    glDeleteBuffers(1, &m_fishEBO);
    glDeleteBuffers(1, &m_fishInstVBO);
    glDeleteVertexArrays(1, &m_mantaVAO);
    glDeleteBuffers(1, &m_mantaVBO);
    glDeleteBuffers(1, &m_mantaEBO);
    glDeleteBuffers(1, &m_mantaInstVBO);
    glDeleteVertexArrays(1, &m_whaleVAO);
    glDeleteBuffers(1, &m_whaleVBO);
    glDeleteBuffers(1, &m_whaleEBO);
    glDeleteBuffers(1, &m_whaleInstVBO);
    glDeleteVertexArrays(1, &m_skyVAO);
    glDeleteBuffers(1, &m_skyVBO);
    glDeleteVertexArrays(1, &m_rayVAO);
    glDeleteBuffers(1, &m_rayVBO);
    glDeleteVertexArrays(1, &m_particleVAO);
    glDeleteBuffers(1, &m_particleVBO);
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

    glGenVertexArrays(1, &m_envVAO);
    glGenBuffers(1, &m_envVBO);
    glGenBuffers(1, &m_envEBO);

    glGenVertexArrays(1, &m_fishVAO);
    glGenBuffers(1, &m_fishVBO);
    glGenBuffers(1, &m_fishEBO);
    glGenBuffers(1, &m_fishInstVBO);

    glGenVertexArrays(1, &m_mantaVAO);
    glGenBuffers(1, &m_mantaVBO);
    glGenBuffers(1, &m_mantaEBO);
    glGenBuffers(1, &m_mantaInstVBO);

    glGenVertexArrays(1, &m_whaleVAO);
    glGenBuffers(1, &m_whaleVBO);
    glGenBuffers(1, &m_whaleEBO);
    glGenBuffers(1, &m_whaleInstVBO);

    glGenVertexArrays(1, &m_rayVAO);
    glGenBuffers(1, &m_rayVBO);

    glGenVertexArrays(1, &m_particleVAO);
    glGenBuffers(1, &m_particleVBO);

    rebuildWorld(m_seed);
    spawnWhalePass(true);

    qreal dpr = devicePixelRatioF();
    resizePostProcessBuffers(int(width() * dpr), int(height() * dpr));
}

void DeepDiveWidget::rebuildWorld(uint32_t seed) {
    m_world = WorldGenerator::generate(seed);

    glBindVertexArray(m_envVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_envVBO);
    glBufferData(GL_ARRAY_BUFFER, m_world.envVertices.size() * sizeof(EnvVertex),
                 m_world.envVertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_envEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_world.envIndices.size() * sizeof(uint32_t),
                 m_world.envIndices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex), reinterpret_cast<void*>(offsetof(EnvVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex), reinterpret_cast<void*>(offsetof(EnvVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(EnvVertex), reinterpret_cast<void*>(offsetof(EnvVertex, color)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(EnvVertex), reinterpret_cast<void*>(offsetof(EnvVertex, param)));

    auto setupFaunaVAO = [&](GLuint vao, GLuint vbo, GLuint ebo, GLuint instVbo,
                             const std::vector<FaunaVertex>& verts,
                             const std::vector<uint32_t>& inds,
                             const std::vector<FaunaInstance>& insts) {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(FaunaVertex), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size() * sizeof(uint32_t), inds.data(), GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FaunaVertex), reinterpret_cast<void*>(offsetof(FaunaVertex, pos)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(FaunaVertex), reinterpret_cast<void*>(offsetof(FaunaVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(FaunaVertex), reinterpret_cast<void*>(offsetof(FaunaVertex, color)));

        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glBufferData(GL_ARRAY_BUFFER, insts.size() * sizeof(FaunaInstance), insts.data(), GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(FaunaInstance), reinterpret_cast<void*>(offsetof(FaunaInstance, posScale)));
        glVertexAttribDivisor(3, 1);

        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(FaunaInstance), reinterpret_cast<void*>(offsetof(FaunaInstance, rotPhase)));
        glVertexAttribDivisor(4, 1);
    };

    setupFaunaVAO(m_fishVAO, m_fishVBO, m_fishEBO, m_fishInstVBO,
                  m_world.fishMeshVertices, m_world.fishMeshIndices, m_world.fishInstances);
    setupFaunaVAO(m_mantaVAO, m_mantaVBO, m_mantaEBO, m_mantaInstVBO,
                  m_world.mantaMeshVertices, m_world.mantaMeshIndices, m_world.mantaInstances);
    setupFaunaVAO(m_whaleVAO, m_whaleVBO, m_whaleEBO, m_whaleInstVBO,
                  m_world.whaleMeshVertices, m_world.whaleMeshIndices, m_world.whaleInstances);

    glBindVertexArray(m_rayVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_rayVBO);
    glBufferData(GL_ARRAY_BUFFER, m_world.godRayVertices.size() * sizeof(RayVertex),
                 m_world.godRayVertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RayVertex), reinterpret_cast<void*>(offsetof(RayVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(RayVertex), reinterpret_cast<void*>(offsetof(RayVertex, color)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RayVertex), reinterpret_cast<void*>(offsetof(RayVertex, uv)));

    glBindVertexArray(m_particleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_particleVBO);
    glBufferData(GL_ARRAY_BUFFER, m_world.particles.size() * sizeof(Particle),
                 m_world.particles.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Particle), reinterpret_cast<void*>(offsetof(Particle, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Particle), reinterpret_cast<void*>(offsetof(Particle, colorSize)));
}

void DeepDiveWidget::spawnWhalePass(bool nearPlayer) {
    float angle = std::fmod(m_time * 0.37f + 0.8f, 6.2831853f);
    m_whaleDir = QVector3D(std::sin(angle), 0.0f, std::cos(angle)).normalized();
    QVector3D side(-m_whaleDir.z(), 0.0f, m_whaleDir.x());

    float height = std::max(m_camPos.y() + 14.0f, 8.5f);
    if (nearPlayer) {
        m_whaleStartPos = QVector3D(m_camPos.x(), height, m_camPos.z()) - m_whaleDir * 52.0f + side * 8.0f;
        m_whaleMaxDist  = 140.0f;
    } else {
        m_whaleStartPos = QVector3D(m_camPos.x(), height, m_camPos.z()) - m_whaleDir * 85.0f + side * 14.0f;
        m_whaleMaxDist  = 190.0f;
    }

    m_whaleTravelDist = 0.0f;
    m_whaleActive = true;
    m_whaleSongTriggered = false;
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
    float accel = m_pressedKeys.contains(Qt::Key_Shift) ? 32.0f : 14.0f;

    m_camVel += wishDir * (accel * dt);
    m_camVel *= std::exp(-3.2f * dt);
    m_camPos += m_camVel * dt;
    m_camPos.setY(m_camPos.y() + std::sin(m_time * 1.4f) * 0.006f);

    float floorY = ProcMath::sampleTerrainHeight(m_camPos.x(), m_camPos.z(), m_seed) + 1.2f;
    if (m_camPos.y() < floorY) m_camPos.setY(floorY);

    if (m_audioEngine) {
        m_audioEngine->updateState(m_camPos.y(), m_camVel.length(), m_paletteIndex);
    }

    auto updateFaunaGroup = [&](std::vector<FaunaInstance>& group, GLuint vbo, float verticalAmp) {
        for (auto& inst : group) {
            inst.orbitAngle += inst.angularSpeed * dt;
            float r = inst.orbitRadius + std::sin(m_time * 0.6f + inst.rotPhase.y()) * 1.2f;

            float x = inst.orbitCenter.x() + std::cos(inst.orbitAngle) * r;
            float z = inst.orbitCenter.z() + std::sin(inst.orbitAngle) * r;
            float y = inst.baseHeight + std::sin(m_time * 0.9f + inst.rotPhase.y()) * verticalAmp;

            float vx = -std::sin(inst.orbitAngle) * inst.angularSpeed * r;
            float vz =  std::cos(inst.orbitAngle) * inst.angularSpeed * r;
            float yaw = std::atan2(vx, vz);

            inst.posScale = QVector4D(x, y, z, inst.posScale.w());
            inst.rotPhase.setX(yaw);
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, group.size() * sizeof(FaunaInstance), group.data());
    };

    updateFaunaGroup(m_world.fishInstances, m_fishInstVBO, 0.55f);
    updateFaunaGroup(m_world.mantaInstances, m_mantaInstVBO, 1.15f);

    // Обновление медленно проплывающего гигантского кита
    if (!m_world.whaleInstances.empty()) {
        auto& whale = m_world.whaleInstances[0];
        if (m_whaleActive) {
            m_whaleTravelDist += m_whaleSpeed * dt;
            float progress = m_whaleTravelDist / std::max(1.0f, m_whaleMaxDist);

            // Плавная дуга курса и глубины
            QVector3D side(-m_whaleDir.z(), 0.0f, m_whaleDir.x());
            float curveOffset = std::sin(progress * 3.14159265f) * 12.0f;
            float verticalGlide = std::sin(m_time * 0.45f) * 1.4f;

            QVector3D pos = m_whaleStartPos + m_whaleDir * m_whaleTravelDist + side * curveOffset;
            pos.setY(m_whaleStartPos.y() + verticalGlide);

            QVector3D tangent = (m_whaleDir * m_whaleSpeed + side * (std::cos(progress * 3.14159265f) * 0.7f)).normalized();
            float yaw = std::atan2(tangent.x(), tangent.z());
            float bank = std::sin(m_time * 0.4f) * 0.08f;

            whale.posScale = QVector4D(pos, 2.65f);
            whale.rotPhase = QVector4D(yaw, 0.0f, 2.0f, bank);

            // Когда кит приближается к середине своего маршрута над каньоном — звучит его песня
            if (!m_whaleSongTriggered && progress > 0.28f) {
                m_whaleSongTriggered = true;
                if (m_audioEngine) m_audioEngine->triggerWhaleCall();
            }

            if (m_whaleTravelDist >= m_whaleMaxDist) {
                m_whaleActive = false;
                m_whaleCooldown = 22.0f; // пауза 22 секунды до следующего появления кита
                whale.posScale = QVector4D(0.0f, -500.0f, 0.0f, 0.01f);
            }
        } else {
            m_whaleCooldown -= dt;
            if (m_whaleCooldown <= 0.0f) {
                spawnWhalePass(false);
            }
        }

        glBindBuffer(GL_ARRAY_BUFFER, m_whaleInstVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        m_world.whaleInstances.size() * sizeof(FaunaInstance),
                        m_world.whaleInstances.data());
    }

    for (auto& p : m_world.particles) {
        p.pos.setY(p.pos.y() + p.speedY * dt);
        p.pos.setX(p.pos.x() + std::sin(m_time * 1.8f + p.phase) * 0.25f * dt);
        if (p.pos.y() > 20.0f) p.pos.setY(-22.0f);
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

    QVector3D waterShallow, waterDeep;
    if (m_paletteIndex == 0) {
        waterShallow = QVector3D(0.16f, 0.72f, 0.80f);
        waterDeep    = QVector3D(0.03f, 0.24f, 0.34f);
    } else if (m_paletteIndex == 1) {
        waterShallow = QVector3D(0.45f, 0.48f, 0.74f);
        waterDeep    = QVector3D(0.14f, 0.09f, 0.28f);
    } else {
        waterShallow = QVector3D(0.08f, 0.42f, 0.58f);
        waterDeep    = QVector3D(0.01f, 0.08f, 0.16f);
    }

    QVector3D sunDir = QVector3D(-0.28f, 0.92f, -0.22f).normalized();

    // ========================================================================
    // ЭТАП 0: Рендеринг направленной карты теней (Shadow Map Pass)
    // ========================================================================
    const float orthoHalfSize = 85.0f;
    const float texelWorldStep = (orthoHalfSize * 2.0f) / SHADOW_MAP_RES;
    // Привязываем центр каскада теней к шагу текселя, чтобы тени не мерцали при плавании камеры
    QVector3D shadowFocus(
        std::floor(m_camPos.x() / texelWorldStep) * texelWorldStep,
        0.0f,
        std::floor(m_camPos.z() / texelWorldStep) * texelWorldStep
        );

    QMatrix4x4 lightProj;
    lightProj.ortho(-orthoHalfSize, orthoHalfSize, -orthoHalfSize, orthoHalfSize, 1.0f, 165.0f);
    QMatrix4x4 lightView;
    lightView.lookAt(shadowFocus + sunDir * 78.0f, shadowFocus, QVector3D(0.0f, 0.0f, 1.0f));
    QMatrix4x4 lightSpaceMatrix = lightProj * lightView;

    if (m_shadowsEnabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
        glViewport(0, 0, SHADOW_MAP_RES, SHADOW_MAP_RES);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glClear(GL_DEPTH_BUFFER_BIT);

        // Окружение + анимированная ламинария + корабли + скалы
        m_shadowEnvShader.bind();
        m_shadowEnvShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        m_shadowEnvShader.setUniformValue("uTime", m_time);
        glBindVertexArray(m_envVAO);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_world.envIndices.size()), GL_UNSIGNED_INT, nullptr);

        // Рыбы, скаты-манты и гигантский кит также отбрасывают движущиеся тени на дно
        m_shadowFaunaShader.bind();
        m_shadowFaunaShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
        m_shadowFaunaShader.setUniformValue("uTime", m_time);

        glBindVertexArray(m_fishVAO);
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(m_world.fishMeshIndices.size()),
                                GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(m_world.fishInstances.size()));

        glBindVertexArray(m_mantaVAO);
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(m_world.mantaMeshIndices.size()),
                                GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(m_world.mantaInstances.size()));

        if (m_whaleActive) {
            glBindVertexArray(m_whaleVAO);
            glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(m_world.whaleMeshIndices.size()),
                                    GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(m_world.whaleInstances.size()));
        }
    }

    // ========================================================================
    // ЭТАП 1: Основной рендер сцены в 8x MSAA HDR FBO с рассеянными тенями
    // ========================================================================
    QMatrix4x4 proj;
    float aspect = float(physW) / float(physH);
    proj.perspective(65.0f, aspect, 0.2f, 220.0f);

    QMatrix4x4 view;
    view.lookAt(m_camPos, m_camPos + cameraForward(), QVector3D(0.0f, 1.0f, 0.0f));
    QMatrix4x4 viewProj = proj * view;
    QMatrix4x4 invViewProj = viewProj.inverted();

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
    m_skyShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_skyShader.setUniformValue("uWaterColorShallow", waterShallow);
    m_skyShader.setUniformValue("uTime", m_time);
    glBindVertexArray(m_skyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthFunc(GL_LESS);
    m_envShader.bind();
    m_envShader.setUniformValue("uRockTex", 0);
    m_envShader.setUniformValue("uShadowMap", 3);
    m_envShader.setUniformValue("uViewProj", viewProj);
    m_envShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
    m_envShader.setUniformValue("uCamPos", m_camPos);
    m_envShader.setUniformValue("uSunDir", sunDir);
    m_envShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_envShader.setUniformValue("uWaterColorShallow", waterShallow);
    m_envShader.setUniformValue("uTime", m_time);
    m_envShader.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);

    glBindVertexArray(m_envVAO);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_world.envIndices.size()), GL_UNSIGNED_INT, nullptr);

    m_faunaShader.bind();
    m_faunaShader.setUniformValue("uRockTex", 0);
    m_faunaShader.setUniformValue("uShadowMap", 3);
    m_faunaShader.setUniformValue("uViewProj", viewProj);
    m_faunaShader.setUniformValue("uLightSpaceMatrix", lightSpaceMatrix);
    m_faunaShader.setUniformValue("uCamPos", m_camPos);
    m_faunaShader.setUniformValue("uSunDir", sunDir);
    m_faunaShader.setUniformValue("uWaterColorDeep", waterDeep);
    m_faunaShader.setUniformValue("uWaterColorShallow", waterShallow);
    m_faunaShader.setUniformValue("uTime", m_time);
    m_faunaShader.setUniformValue("uEnableShadows", m_shadowsEnabled ? 1 : 0);

    glBindVertexArray(m_fishVAO);
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(m_world.fishMeshIndices.size()),
                            GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(m_world.fishInstances.size()));

    glBindVertexArray(m_mantaVAO);
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(m_world.mantaMeshIndices.size()),
                            GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(m_world.mantaInstances.size()));

    if (m_whaleActive) {
        glBindVertexArray(m_whaleVAO);
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(m_world.whaleMeshIndices.size()),
                                GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(m_world.whaleInstances.size()));
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
    // ЭТАП 2: Resolve 8x MSAA + Двухпроходный Bloom + Композитинг PostFX
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

    m_postProcessShader.setUniformValue("uSceneColorTex", 0);
    m_postProcessShader.setUniformValue("uSceneDepthTex", 1);
    m_postProcessShader.setUniformValue("uBloomTex", 2);
    m_postProcessShader.setUniformValue("uInvResolution", QVector2D(1.0f / physW, 1.0f / physH));
    m_postProcessShader.setUniformValue("uSunScreenPos", sunScreen);
    m_postProcessShader.setUniformValue("uWaterTint", waterShallow);
    m_postProcessShader.setUniformValue("uTime", m_time);
    m_postProcessShader.setUniformValue("uEnablePost", m_postProcessEnabled ? 1 : 0);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);

    // ========================================================================
    // HUD
    // ========================================================================
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QColor(210, 245, 255, 230));
    painter.setFont(QFont("Consolas", 10, QFont::Bold));

    const char* biomeNames[] = {"Kelp Reef (Emerald)", "Purple Grotto", "Abyssal Trench"};
    QString audioStatus = (m_audioEngine && !m_audioEngine->isMuted())
                              ? QString("ON (%1%)").arg(qRound(m_audioEngine->volume() * 100.0f))
                              : QString("MUTED");

    QString hud = QString(
                      "GPU: %1 | %2x MSAA | Shadows: %3 | PostFX: %4 | Audio: %5 | FPS: %6\n"
                      "Seed: %7 | Biome: %8 | [B] Biome [R] Regen [L] Shadows [P] PostFX [K] Summon Whale [M] Mute"
                      ).arg(m_glRendererInfo)
                      .arg(m_activeSamples)
                      .arg(m_shadowsEnabled ? "ON (Soft 16-tap)" : "OFF")
                      .arg(m_postProcessEnabled ? "ON" : "OFF")
                      .arg(audioStatus)
                      .arg(qRound(m_fps))
                      .arg(m_seed)
                      .arg(biomeNames[m_paletteIndex]);

    painter.fillRect(QRect(14, 14, 1020, 52), QColor(5, 22, 32, 165));
    painter.drawText(QRect(24, 18, 1000, 44), Qt::AlignLeft | Qt::AlignVCenter, hud);
}

void DeepDiveWidget::keyPressEvent(QKeyEvent* event) {
    m_pressedKeys.insert(event->key());
    if (event->key() == Qt::Key_B) {
        m_paletteIndex = (m_paletteIndex + 1) % 3;
    } else if (event->key() == Qt::Key_R) {
        m_seed += 17u;
        makeCurrent();
        rebuildWorld(m_seed);
        doneCurrent();
    } else if (event->key() == Qt::Key_P) {
        m_postProcessEnabled = !m_postProcessEnabled;
    } else if (event->key() == Qt::Key_L) {
        m_shadowsEnabled = !m_shadowsEnabled;
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
