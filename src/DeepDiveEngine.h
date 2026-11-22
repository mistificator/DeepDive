#pragma once
#include <QVector3D>
#include <array>
#include <cstdint>
#include <random>
#include "WorldGenerator.h"

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

    DeepDiveEngine();
    ~DeepDiveEngine() = default;

    // World lifecycle & simulation step
    void setWorld(WorldData&& world, uint32_t seed);
    EngineStepEvents update(float dt, const EngineInput& input);

    // World & gameplay actions
    void cycleBiome();
    void summonWhaleNearPlayer();

    // Read-only state for renderer, HUD, and audio
    const WorldData& world() const { return m_world; }
    const FaunaAgent& whale() const { return m_whale; }
    const std::array<QVector3D, MAX_LURE_LIGHTS>& lurePositions() const { return m_lurePositions; }
    int lureCount() const { return m_lureCount; }

    uint32_t seed() const { return m_seed; }
    float simulationTime() const { return m_time; }
    bool isPlayerColliding() const { return m_playerColliding; }

    QVector3D cameraPosition() const { return m_camPos; }
    QVector3D cameraForward() const;
    QVector3D cameraRight() const;
    float cameraSpeed() const { return m_camVel.length(); }
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

    void spawnWhalePass(bool nearPlayer);
    float sampleWhaleClearance(float x, float z) const;
    float measureWhaleFreeDist(float x, float z, float yaw, float maxDist) const;
    QVector3D pickWhaleOpenWaypoint(const QVector3D& nearAnchor, float minDist, float maxDist) const;
    void updateWhaleNavigation(float dt, bool& whaleCalledOut);

    void updateFaunaAgents(float dt);
    void updateAnglerAgents(float dt);
    void refreshLureLights();
    QVector3D computeAnglerLureWorldPos(const FaunaAgent& a) const;
    void updateParticles(float dt);

    // World & simulation state
    WorldData m_world;
    uint32_t m_seed = 42u;
    int m_paletteIndex = 0; // 0 = Tropical Reef (Vibrant) by default
    float m_time = 0.0f;
    bool m_playerColliding = false;
    bool m_wasPlayerColliding = false;

    std::array<QVector3D, MAX_LURE_LIGHTS> m_lurePositions{};
    int m_lureCount = 0;

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
