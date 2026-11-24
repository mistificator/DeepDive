#pragma once
#include <QVector3D>
#include <array>
#include <cstdint>
#include <random>
#include <vector>
#include "Noise.h"
#include "WorldGenerator.h"

enum class CameraMode {
    FirstPerson = 0,
    ThirdPersonFish = 1,
    ThirdPersonManta = 2
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

    // Water/air transition events (for audio splashes, rumble, and 3D spray bursts)
    bool breachedSurface = false;
    bool splashedDown = false;
    bool cameraCrossedSurface = false;
    bool cameraEnteredAir = false;
    float splashIntensity = 0.0f;
};

struct BiomeVisuals {
    QVector3D waterShallow;
    QVector3D waterDeep;
    QVector3D sunColor;
    QVector3D carpetTint;
    QVector3D kelpTint;
    QVector3D sunDir;
    float vibrance = 1.0f;
    float fogScale = 1.0f;
};

class DeepDiveEngine {
public:
    static constexpr int MAX_LURE_LIGHTS = 8;
    static constexpr int MAX_SPRAY_PARTICLES = 384;

    DeepDiveEngine();
    ~DeepDiveEngine() = default;

    // World lifecycle & simulation step (dt is real frame delta time in seconds)
    void setWorld(WorldData&& world, uint32_t seed);
    EngineStepEvents update(float dt, const EngineInput& input);

    // World & gameplay actions
    void cycleBiome();
    void cycleCameraMode();
    void summonWhaleNearPlayer();

    // Read-only state for renderer, HUD, and audio
    const WorldData& world() const { return m_world; }
    const FaunaAgent& whale() const { return m_whale; }
    const std::array<QVector3D, MAX_LURE_LIGHTS>& lurePositions() const { return m_lurePositions; }
    int lureCount() const { return m_lureCount; }
    const std::vector<SprayGpuVertex>& sprayVertices() const { return m_sprayGpuVerts; }

    uint32_t seed() const { return m_seed; }
    float simulationTime() const { return m_time; }
    bool isPlayerColliding() const { return m_playerColliding; }

    // Surface breaching, bullet-time & dual-medium state
    bool isPlayerAirborne() const { return m_isAirborne; }
    bool isCameraAboveWater() const { return m_camAboveWater; }
    float timeScale() const { return m_timeScale; }
    float bulletTimeFactor() const { return m_bulletTimeFactor; }
    float splashScreenAmount() const { return m_splashScreenAmount; }
    float lensWetness() const { return m_lensWetness; }
    float dynamicFovOffset() const { return m_dynamicFovOffset; }
    float waterHeightAtCamera() const { return m_camWaterHeight; }
    float surfaceAltitude() const;

    CameraMode cameraMode() const { return m_cameraMode; }
    const char* cameraModeName() const;
    bool hasPlayerAvatar() const { return m_cameraMode != CameraMode::FirstPerson; }
    const FaunaAgent& playerAvatar() const { return m_playerAvatar; }

    QVector3D playerPosition() const { return m_playerPos; }
    QVector3D cameraPosition() const { return m_camPos; }
    QVector3D cameraForward() const;
    QVector3D cameraRight() const;
    float cameraSpeed() const { return m_playerVel.length(); }
    float diveDepth() const;
    float radialDistance() const;

    int paletteIndex() const { return m_paletteIndex; }
    int audioBiomeIndex() const { return (m_paletteIndex == 0) ? 0 : (m_paletteIndex - 1); }
    const char* biomeName() const;
    BiomeVisuals biomeVisuals() const;

private:
    enum class WhaleState {
        Cruising,
        AvoidingTurn
    };

    struct WhaleColliderSphere {
        QVector3D center;
        float radius = 1.0f;
    };

    struct AirSprayParticle {
        QVector3D pos;
        QVector3D vel;
        float age = 0.0f;
        float lifetime = 1.0f;
        float baseSize = 0.4f;
        float drag = 0.45f;
        float gravityScale = 1.0f;
        bool isMist = false;
        bool active = false;
    };

    static constexpr int WHALE_COLLIDER_COUNT = 7;

    void configurePlayerAvatar(bool resetOrientation);
    float playerCollisionRadius() const;
    float playerFloorMargin() const;
    float playerSurfaceMargin() const;
    void updatePlayerAvatarAndCamera(float realDt, float simDt, const QVector3D& wishDir, bool moving);
    bool isCameraPointClear(const QVector3D& pt, float radius, bool allowAboveWater) const;

    void spawnWhalePass(bool nearPlayer);
    float sampleWhaleClearance(float x, float z) const;
    float measureWhaleFreeDist(float x, float z, float yaw, float maxDist) const;
    QVector3D pickWhaleOpenWaypoint(const QVector3D& nearAnchor, float minDist, float maxDist) const;
    void updateWhaleNavigation(float dt, bool& whaleCalledOut);
    void refreshWhaleColliders();
    QVector3D computeWhaleAvoidanceForce(const QVector3D& pos, float agentRadius, float margin) const;

    void updateFaunaAgents(float dt);
    void updateAnglerAgents(float dt);
    void updateSeagullAgents(float dt);
    void resolveFaunaAndPlayerCollisions(float& maxPlayerImpactSpeed);
    void refreshLureLights();
    QVector3D computeAnglerLureWorldPos(const FaunaAgent& a) const;
    void updateParticles(float dt);

    void spawnSprayBurst(const QVector3D& origin, const QVector3D& inheritedVel,
                         int count, float radialSpeed, float upSpeed,
                         float sizeScale, float mistRatio, float gravityScale = 1.0f);
    void updateAirSpray(float simDt);

    // World & simulation state
    WorldData m_world;
    uint32_t m_seed = 42u;
    int m_paletteIndex = 0; // 0 = Tropical Reef (Vibrant) by default
    float m_time = 0.0f;
    bool m_playerColliding = false;
    bool m_wasPlayerColliding = false;

    // Breaching, airborne ballistics, bullet-time & screen wetness
    bool m_isAirborne = false;
    bool m_camAboveWater = false;
    float m_camWaterHeight = ProcMath::WATER_SURFACE_Y;
    float m_airborneTime = 0.0f;
    float m_airTrailTimer = 0.0f;
    float m_breachCooldown = 0.0f;
    float m_bulletTimeRealElapsed = 0.0f;
    float m_timeScale = 1.0f;
    float m_bulletTimeFactor = 0.0f;
    float m_splashScreenAmount = 0.0f;
    float m_lensWetness = 0.0f;
    float m_dynamicFovOffset = 0.0f;

    std::array<QVector3D, MAX_LURE_LIGHTS> m_lurePositions{};
    int m_lureCount = 0;

    // 3D airborne spray / mist ring buffer (breach fountains, splashdown crowns, whale blow spout)
    std::array<AirSprayParticle, MAX_SPRAY_PARTICLES> m_sprayPool{};
    int m_sprayCursor = 0;
    std::vector<SprayGpuVertex> m_sprayGpuVerts;

    // Autonomous humpback whale state & multi-sphere collider hull
    std::mt19937 m_rng;
    FaunaAgent m_whale;
    std::array<WhaleColliderSphere, WHALE_COLLIDER_COUNT> m_whaleColliders{};
    float m_whaleBroadRadius = 20.0f;

    QVector3D m_whaleWaypoint{0.0f, 20.5f, 0.0f};
    float m_whaleWaypointTimer = 0.0f;
    float m_whaleSongTimer = 7.0f;
    float m_whaleBlowTimer = 1.2f;

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

    // Player body, third-person avatar, and camera rig
    CameraMode m_cameraMode = CameraMode::FirstPerson;
    FaunaAgent m_playerAvatar{};
    QVector3D m_playerPos{0.0f, -6.0f, 20.0f};
    QVector3D m_playerVel{0.0f, 0.0f, 0.0f};
    QVector3D m_camPos{0.0f, -6.0f, 20.0f};
    float m_camArmDist = 0.0f;
    float m_yaw = 0.0f;
    float m_pitch = 0.06f;

    // Reusable scratch buffer for fish spatial hash (zero per-frame allocations)
    std::vector<int> m_fishHashNext;
};
