#include "DeepDiveEngine.h"
#include "Noise.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

inline float wrapAngle(float a) {
    while (a > PI)  a -= TWO_PI;
    while (a < -PI) a += TWO_PI;
    return a;
}

inline QVector3D faunaLocalToWorld(const FaunaAgent& a, const QVector3D& localPos) {
    const QVector3D s = localPos * a.scale;
    const float cy = std::cos(a.yaw),   sy = std::sin(a.yaw);
    const float cp = std::cos(a.pitch), sp = std::sin(a.pitch);
    const float cb = std::cos(a.bank),  sb = std::sin(a.bank);

    const QVector3D v1(cb * s.x() - sb * s.y(),
                       sb * s.x() + cb * s.y(),
                       s.z());
    const QVector3D v2(v1.x(),
                       cp * v1.y() + sp * v1.z(),
                       -sp * v1.y() + cp * v1.z());
    const QVector3D v3(cy * v2.x() + sy * v2.z(),
                       v2.y(),
                       -sy * v2.x() + cy * v2.z());
    return v3 + a.pos;
}

inline float effectiveFishRadius(const FaunaAgent& a) {
    return std::max(a.boundingRadius * 0.70f, 0.42f * a.scale);
}

inline float effectiveMantaRadius(const FaunaAgent& a) {
    return std::max(a.boundingRadius, 1.55f * a.scale);
}

inline float effectiveAnglerRadius(const FaunaAgent& a) {
    return std::max(a.boundingRadius, 1.15f * a.scale);
}

// Push two spherical bodies apart and cancel inward relative velocity
inline bool resolveSpherePair(QVector3D& posA, QVector3D& velA, float rA, float invMassA,
                              QVector3D& posB, QVector3D& velB, float rB, float invMassB,
                              float yAnisotropy = 1.0f, float restitution = 1.08f,
                              float* outImpactSpeed = nullptr) {
    const float invMassSum = invMassA + invMassB;
    if (invMassSum <= 1e-6f) return false;

    const QVector3D diff = posA - posB;
    const QVector3D scaled(diff.x(), diff.y() * yAnisotropy, diff.z());
    const float rSum = rA + rB;
    const float distSq = scaled.lengthSquared();
    if (distSq >= rSum * rSum) return false;

    QVector3D normal;
    float pen = 0.0f;
    if (distSq > 1e-6f) {
        const float dist = std::sqrt(distSq);
        pen = rSum - dist;
        normal = QVector3D(scaled.x(), scaled.y() / yAnisotropy, scaled.z());
        if (normal.lengthSquared() > 1e-6f) {
            normal.normalize();
        } else {
            normal = QVector3D(1.0f, 0.0f, 0.0f);
        }
    } else {
        pen = rSum;
        normal = QVector3D(1.0f, 0.0f, 0.0f);
    }

    const float wA = invMassA / invMassSum;
    const float wB = invMassB / invMassSum;
    posA += normal * (pen * wA);
    posB -= normal * (pen * wB);

    const float relVn = QVector3D::dotProduct(velA - velB, normal);
    if (relVn < 0.0f) {
        if (outImpactSpeed) {
            *outImpactSpeed = std::max(*outImpactSpeed, -relVn);
        }
        const float impulse = -relVn * restitution;
        velA += normal * (impulse * wA);
        velB -= normal * (impulse * wB);
    }
    return true;
}

constexpr const char* BIOME_NAMES[4] = {
    "Tropical Reef (Vibrant)",
    "Kelp Reef (Emerald)",
    "Purple Grotto",
    "Abyssal Trench"
};

constexpr const char* CAMERA_MODE_NAMES[3] = {
    "1st Person",
    "3rd Person (Fish)",
    "3rd Person (Manta)"
};
} // namespace

// ============================================================================
// LIFECYCLE & WORLD SETUP
// ============================================================================

DeepDiveEngine::DeepDiveEngine() {
    std::random_device rd;
    const auto timeSeed = static_cast<uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    m_rng.seed(rd() ^ timeSeed);

    configurePlayerAvatar(true);
}

void DeepDiveEngine::setWorld(WorldData&& world, uint32_t seed) {
    m_seed = seed;
    m_world = std::move(world);
    m_playerColliding = false;
    m_wasPlayerColliding = false;
    m_fishHashNext.resize(m_world.fishAgents.size(), -1);

    m_world.collision.resolveSphereCollision(
        m_playerPos, &m_playerVel,
        playerCollisionRadius(), playerFloorMargin(), playerSurfaceMargin());

    spawnWhalePass(true);
    refreshWhaleColliders();
    configurePlayerAvatar(true);

    const float targetArm = (m_cameraMode == CameraMode::ThirdPersonFish) ? 4.2f
                                                                          : (m_cameraMode == CameraMode::ThirdPersonManta ? 9.2f : 0.0f);
    m_camArmDist = targetArm;
    updatePlayerAvatarAndCamera(0.016f, QVector3D(0.0f, 0.0f, 0.0f), false);
    refreshLureLights();
}

// ============================================================================
// CONTROLS, CAMERA MODE & STATE QUERIES
// ============================================================================

void DeepDiveEngine::cycleBiome() {
    m_paletteIndex = (m_paletteIndex + 1) % 4;
}

void DeepDiveEngine::cycleCameraMode() {
    const int next = (static_cast<int>(m_cameraMode) + 1) % 3;
    m_cameraMode = static_cast<CameraMode>(next);

    configurePlayerAvatar(true);

    // Ensure the resized player hull (e.g. switching from small fish to manta) is clear of rocks
    m_world.collision.resolveSphereCollision(
        m_playerPos, &m_playerVel,
        playerCollisionRadius(), playerFloorMargin(), playerSurfaceMargin());

    float unusedImpact = 0.0f;
    resolveFaunaAndPlayerCollisions(unusedImpact);

    m_camArmDist = (m_cameraMode == CameraMode::ThirdPersonFish) ? 4.2f
                                                                 : (m_cameraMode == CameraMode::ThirdPersonManta ? 9.2f : 0.0f);
    updatePlayerAvatarAndCamera(0.016f, QVector3D(0.0f, 0.0f, 0.0f), false);
}

void DeepDiveEngine::summonWhaleNearPlayer() {
    spawnWhalePass(true);
    refreshWhaleColliders();
}

const char* DeepDiveEngine::cameraModeName() const {
    return CAMERA_MODE_NAMES[static_cast<int>(m_cameraMode)];
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

float DeepDiveEngine::diveDepth() const {
    return std::max(0.0f, ProcMath::WATER_SURFACE_Y - m_playerPos.y());
}

float DeepDiveEngine::radialDistance() const {
    return std::sqrt(m_playerPos.x() * m_playerPos.x() + m_playerPos.z() * m_playerPos.z());
}

const char* DeepDiveEngine::biomeName() const {
    return BIOME_NAMES[m_paletteIndex];
}

BiomeVisuals DeepDiveEngine::biomeVisuals() const {
    BiomeVisuals v;
    v.sunDir = QVector3D(-0.28f, 0.92f, -0.22f).normalized();

    if (m_paletteIndex == 0) {
        // Tropical Reef (Vibrant)
        v.waterShallow = QVector3D(0.11f, 0.68f, 0.78f);
        v.waterDeep    = QVector3D(0.02f, 0.24f, 0.38f);
        v.sunColor     = QVector3D(0.94f, 1.06f, 0.96f);
        v.carpetTint   = QVector3D(0.28f, 0.90f, 0.44f);
        v.kelpTint     = QVector3D(0.88f, 1.06f, 0.60f);
        v.vibrance     = 1.38f;
        v.fogScale     = 1.05f;
    } else if (m_paletteIndex == 1) {
        // Kelp Reef (Emerald)
        v.waterShallow = QVector3D(0.12f, 0.62f, 0.66f);
        v.waterDeep    = QVector3D(0.025f, 0.22f, 0.30f);
        v.sunColor     = QVector3D(0.68f, 0.98f, 0.96f);
        v.carpetTint   = QVector3D(0.20f, 0.78f, 0.46f);
        v.kelpTint     = QVector3D(0.74f, 0.98f, 0.54f);
        v.vibrance     = 1.15f;
        v.fogScale     = 1.12f;
    } else if (m_paletteIndex == 2) {
        // Purple Grotto
        v.waterShallow = QVector3D(0.38f, 0.42f, 0.72f);
        v.waterDeep    = QVector3D(0.11f, 0.08f, 0.26f);
        v.sunColor     = QVector3D(0.80f, 0.86f, 1.12f);
        v.carpetTint   = QVector3D(0.98f, 0.28f, 0.72f);
        v.kelpTint     = QVector3D(0.90f, 0.46f, 0.98f);
        v.vibrance     = 1.24f;
        v.fogScale     = 1.12f;
    } else {
        // Abyssal Trench
        v.waterShallow = QVector3D(0.07f, 0.38f, 0.54f);
        v.waterDeep    = QVector3D(0.01f, 0.08f, 0.16f);
        v.sunColor     = QVector3D(0.52f, 0.88f, 1.02f);
        v.carpetTint   = QVector3D(0.18f, 0.60f, 0.66f);
        v.kelpTint     = QVector3D(0.46f, 0.80f, 0.72f);
        v.vibrance     = 1.08f;
        v.fogScale     = 1.25f;
    }

    return v;
}

// ============================================================================
// THIRD-PERSON AVATAR & SPRING-ARM CAMERA
// ============================================================================

void DeepDiveEngine::configurePlayerAvatar(bool resetOrientation) {
    if (m_cameraMode == CameraMode::ThirdPersonFish) {
        m_playerAvatar.type           = 0.0f;
        m_playerAvatar.scale          = 0.85f;
        m_playerAvatar.boundingRadius = 0.72f;
        m_playerAvatar.cruiseSpeed    = 6.5f;
        m_playerAvatar.tint           = QVector3D(1.18f, 0.94f, 0.54f);
    } else if (m_cameraMode == CameraMode::ThirdPersonManta) {
        m_playerAvatar.type           = 1.0f;
        m_playerAvatar.scale          = 1.35f;
        m_playerAvatar.boundingRadius = 2.05f;
        m_playerAvatar.cruiseSpeed    = 8.0f;
        m_playerAvatar.tint           = QVector3D(0.96f, 1.06f, 1.16f);
    }

    m_playerAvatar.pos = m_playerPos;
    m_playerAvatar.vel = m_playerVel;

    if (resetOrientation) {
        const QVector3D fwd = cameraForward();
        const float horiz = std::sqrt(fwd.x() * fwd.x() + fwd.z() * fwd.z());
        m_playerAvatar.yaw   = std::atan2(fwd.x(), fwd.z());
        m_playerAvatar.pitch = ProcMath::clamp(std::atan2(fwd.y(), std::max(horiz, 0.1f)), -0.55f, 0.55f);
        m_playerAvatar.bank  = 0.0f;
    }
}

float DeepDiveEngine::playerCollisionRadius() const {
    if (m_cameraMode == CameraMode::ThirdPersonFish)  return 0.72f;
    if (m_cameraMode == CameraMode::ThirdPersonManta) return 1.95f;
    return 1.45f;
}

float DeepDiveEngine::playerFloorMargin() const {
    if (m_cameraMode == CameraMode::ThirdPersonFish)  return 0.40f;
    if (m_cameraMode == CameraMode::ThirdPersonManta) return 0.90f;
    return 0.45f;
}

float DeepDiveEngine::playerSurfaceMargin() const {
    if (m_cameraMode == CameraMode::ThirdPersonFish)  return 0.85f;
    if (m_cameraMode == CameraMode::ThirdPersonManta) return 1.40f;
    return 1.60f;
}

bool DeepDiveEngine::isCameraPointClear(const QVector3D& pt, float radius) const {
    if (pt.y() > ProcMath::WATER_SURFACE_Y - radius) {
        return false;
    }

    const float rHoriz = std::sqrt(pt.x() * pt.x() + pt.z() * pt.z());
    if (rHoriz > ProcMath::BASIN_INNER_RADIUS - radius) {
        return false;
    }

    const auto& col = m_world.collision;
    if (pt.y() < col.sampleHeight(pt.x(), pt.z()) + radius) {
        return false;
    }

    if (!col.cellSpheres.empty() && col.chunksPerSide > 0) {
        const int minCx = std::clamp(static_cast<int>(std::floor((pt.x() - radius + col.worldHalfSize) / col.chunkSize)),
                                     0, col.chunksPerSide - 1);
        const int maxCx = std::clamp(static_cast<int>(std::floor((pt.x() + radius + col.worldHalfSize) / col.chunkSize)),
                                     0, col.chunksPerSide - 1);
        const int minCz = std::clamp(static_cast<int>(std::floor((pt.z() - radius + col.worldHalfSize) / col.chunkSize)),
                                     0, col.chunksPerSide - 1);
        const int maxCz = std::clamp(static_cast<int>(std::floor((pt.z() + radius + col.worldHalfSize) / col.chunkSize)),
                                     0, col.chunksPerSide - 1);
        for (int cz = minCz; cz <= maxCz; ++cz) {
            for (int cx = minCx; cx <= maxCx; ++cx) {
                for (uint32_t id : col.cellSpheres[cz * col.chunksPerSide + cx]) {
                    const auto& sph = col.spheres[id];
                    const float rSum = sph.radius + radius;
                    if ((pt - sph.center).lengthSquared() < rSum * rSum) {
                        return false;
                    }
                }
            }
        }
    }

    // Prevent the third-person camera from clipping inside the whale
    if ((pt - m_whale.pos).lengthSquared() < (m_whaleBroadRadius + radius) * (m_whaleBroadRadius + radius)) {
        for (const auto& ws : m_whaleColliders) {
            const float rSum = ws.radius + radius;
            if ((pt - ws.center).lengthSquared() < rSum * rSum) {
                return false;
            }
        }
    }

    return true;
}

void DeepDiveEngine::updatePlayerAvatarAndCamera(float dt, const QVector3D& wishDir, bool moving) {
    if (m_cameraMode == CameraMode::FirstPerson) {
        m_camPos = m_playerPos;
        m_camArmDist = 0.0f;
        return;
    }

    const bool isManta = (m_cameraMode == CameraMode::ThirdPersonManta);
    const float speed  = m_playerVel.length();

    // Update avatar transform and swim cycle phase
    m_playerAvatar.pos = m_playerPos;
    m_playerAvatar.vel = m_playerVel;

    const float phaseRate = isManta
                                ? (0.10f + speed * 0.085f)
                                : (0.30f + speed * 0.24f);
    m_playerAvatar.phase = std::fmod(m_playerAvatar.phase + dt * phaseRate, TWO_PI * 16.0f);

    const float turnSmooth = isManta ? 5.2f : 9.5f;
    const float maxPitch   = isManta ? 0.55f : 0.72f;
    const float maxBank    = isManta ? 0.42f : 0.48f;

    QVector3D steerDir(0.0f, 0.0f, 0.0f);
    if (moving && wishDir.lengthSquared() > 1e-4f) {
        steerDir = wishDir.normalized();
    } else if (speed > 0.65f) {
        steerDir = m_playerVel / speed;
    }

    if (steerDir.lengthSquared() > 1e-4f) {
        const float horiz = std::sqrt(steerDir.x() * steerDir.x() + steerDir.z() * steerDir.z());
        float targetYaw = m_playerAvatar.yaw;
        if (horiz > 0.08f) {
            targetYaw = std::atan2(steerDir.x(), steerDir.z());
        }
        const float dYaw = wrapAngle(targetYaw - m_playerAvatar.yaw);
        const float k = ProcMath::clamp(dt * turnSmooth, 0.0f, 1.0f);
        m_playerAvatar.yaw = wrapAngle(m_playerAvatar.yaw + dYaw * k);

        const float targetPitch = ProcMath::clamp(
            std::atan2(steerDir.y(), std::max(horiz, 0.2f)), -maxPitch, maxPitch);
        m_playerAvatar.pitch = ProcMath::mix(m_playerAvatar.pitch, targetPitch, k);

        const float targetBank = ProcMath::clamp(-dYaw * (isManta ? 0.85f : 1.05f), -maxBank, maxBank);
        m_playerAvatar.bank = ProcMath::mix(m_playerAvatar.bank, targetBank, ProcMath::clamp(dt * 5.0f, 0.0f, 1.0f));
    } else {
        // Gently level out when hovering so the user can orbit and inspect the fish/manta
        const float relax = ProcMath::clamp(dt * 3.0f, 0.0f, 1.0f);
        m_playerAvatar.pitch = ProcMath::mix(m_playerAvatar.pitch, 0.0f, relax);
        m_playerAvatar.bank  = ProcMath::mix(m_playerAvatar.bank,  0.0f, relax);
    }

    // Spring-arm camera behind and slightly above the avatar
    const QVector3D lookFwd = cameraForward();
    const float desiredDist = isManta ? 9.2f : 4.2f;
    const float minDist     = isManta ? 2.6f : 1.35f;
    const float liftY       = isManta ? 1.15f : 0.45f;
    const float camProbeR   = 0.55f;

    QVector3D anchor = m_playerPos + QVector3D(0.0f, liftY, 0.0f);
    anchor.setY(std::min(anchor.y(), ProcMath::WATER_SURFACE_Y - 0.65f));

    constexpr int TRACE_STEPS = 14;
    float clearDist = desiredDist;
    for (int i = 1; i <= TRACE_STEPS; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(TRACE_STEPS);
        const float d = desiredDist * t;
        const QVector3D probe = anchor - lookFwd * d;
        if (!isCameraPointClear(probe, camProbeR)) {
            clearDist = std::max(minDist, desiredDist * (static_cast<float>(i - 1) / static_cast<float>(TRACE_STEPS)));
            break;
        }
    }

    // Pull in rapidly on obstacles, extend smoothly when water opens up
    const float armRate = (clearDist < m_camArmDist) ? 18.0f : 5.5f;
    m_camArmDist = ProcMath::mix(m_camArmDist, clearDist, ProcMath::clamp(dt * armRate, 0.0f, 1.0f));
    m_camPos = anchor - lookFwd * m_camArmDist;

    // Final safety clamp for camera against seafloor/surface
    m_world.collision.resolveSphereCollision(m_camPos, nullptr, 0.45f, 0.35f, 0.55f);
}

// ============================================================================
// HUMPBACK WHALE NAVIGATION & MULTI-SPHERE HULL
// ============================================================================

void DeepDiveEngine::refreshWhaleColliders() {
    const float sc = std::max(m_whale.scale, 2.65f);
    m_whaleBroadRadius = 5.8f * sc;

    // 5 spine spheres + 2 pectoral fin spheres in local whale coordinates
    constexpr std::array<QVector3D, WHALE_COLLIDER_COUNT> LOCAL_OFFSETS = {{
        { 0.0f,  0.00f,  0.00f}, // mid torso
        { 0.0f,  0.05f,  2.75f}, // head / jaw
        { 0.0f,  0.02f,  4.45f}, // snout tip
        { 0.0f, -0.04f, -2.65f}, // rear torso
        { 0.0f, -0.06f, -4.55f}, // tail peduncle / flukes
        { 2.15f, -0.35f, 1.05f}, // left pectoral fin
        {-2.15f, -0.35f, 1.05f}  // right pectoral fin
    }};

    constexpr std::array<float, WHALE_COLLIDER_COUNT> LOCAL_RADII = {{
        1.32f, 1.12f, 0.78f, 0.96f, 0.72f, 0.78f, 0.78f
    }};

    for (int i = 0; i < WHALE_COLLIDER_COUNT; ++i) {
        m_whaleColliders[i].center = faunaLocalToWorld(m_whale, LOCAL_OFFSETS[i]);
        m_whaleColliders[i].radius = LOCAL_RADII[i] * sc;
    }
}

QVector3D DeepDiveEngine::computeWhaleAvoidanceForce(const QVector3D& pos, float agentRadius, float margin) const {
    const float broad = m_whaleBroadRadius + agentRadius + margin;
    if ((pos - m_whale.pos).lengthSquared() >= broad * broad) {
        return QVector3D(0.0f, 0.0f, 0.0f);
    }

    QVector3D force(0.0f, 0.0f, 0.0f);
    for (const auto& ws : m_whaleColliders) {
        const QVector3D diff = pos - ws.center;
        const float triggerR = ws.radius + agentRadius + margin;
        const float distSq = diff.lengthSquared();
        if (distSq < triggerR * triggerR && distSq > 1e-5f) {
            const float dist = std::sqrt(distSq);
            const float penRatio = (triggerR - dist) / std::max(margin, 0.5f);
            force += (diff / dist) * (penRatio * penRatio * 14.0f + penRatio * 4.5f);
        }
    }
    return force;
}

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
                const float cx = m_playerPos.x() + camFwd.x() * fd + camRgt.x() * so;
                const float cz = m_playerPos.z() + camFwd.z() * fd + camRgt.z() * so;
                for (float yr : yawRel) {
                    evalCandidate(cx, cz, wrapAngle(camHeading + yr), fd * 0.25f);
                }
            }
        }

        if (bestScore < 55.0f) {
            for (int i = 0; i < 24; ++i) {
                const float ang = (TWO_PI * static_cast<float>(i)) / 24.0f;
                const float rad = 45.0f + 4.0f * sc;
                const float cx = m_playerPos.x() + std::cos(ang) * rad;
                const float cz = m_playerPos.z() + std::sin(ang) * rad;
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
            if ((m_whale.pos - m_playerPos).length() < 360.0f) {
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
        if ((m_whale.pos - m_playerPos).length() < 340.0f) {
            whaleCalledOut = true;
        }
    }
}

// ============================================================================
// FAUNA, ANGLERFISH & INTER-ENTITY COLLISIONS
// ============================================================================

void DeepDiveEngine::updateFaunaAgents(float dt) {
    const bool playerIsSmallFish = (m_cameraMode == CameraMode::ThirdPersonFish);

    auto stepGroup = [&](std::vector<FaunaAgent>& agents,
                         bool isMantaGroup,
                         float lookAhead, float avoidMargin,
                         float floorMargin, float surfaceMargin,
                         float verticalAmp, float turnSmooth, float maxBank, bool fleePlayer) {
        const size_t count = agents.size();
        for (size_t i = 0; i < count; ++i) {
            auto& a = agents[i];
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

            // Reef fish gently scatter from diver or manta, but swim comfortably alongside another small fish
            if (fleePlayer && !playerIsSmallFish) {
                const QVector3D fromPlayer = a.pos - m_playerPos;
                const float dSq = fromPlayer.lengthSquared();
                if (dSq < 49.0f && dSq > 1e-4f) {
                    const float d = std::sqrt(dSq);
                    steer += (fromPlayer / d) * ((7.0f - d) * 3.8f);
                }
            }

            // Predictive manta-to-manta avoidance so rays gracefully bank around each other
            if (isMantaGroup) {
                const float rA = effectiveMantaRadius(a);
                for (size_t j = 0; j < count; ++j) {
                    if (i == j) continue;
                    const auto& b = agents[j];
                    const float rB = effectiveMantaRadius(b);
                    const QVector3D diff = a.pos - b.pos;
                    const QVector3D scaled(diff.x(), diff.y() * 1.65f, diff.z());
                    const float avoidR = rA + rB + 7.5f;
                    const float dSq = scaled.lengthSquared();
                    if (dSq < avoidR * avoidR && dSq > 1e-5f) {
                        const float d = std::sqrt(dSq);
                        const float t = (avoidR - d) / avoidR;
                        QVector3D push(diff.x(), diff.y() * 0.75f, diff.z());
                        if (push.lengthSquared() > 1e-5f) push.normalize();
                        steer += push * (t * t * 18.0f + t * 5.0f);
                    }
                }

                // Mantas also smoothly steer around the player
                const QVector3D fromPlayer = a.pos - m_playerPos;
                const float pAvoidR = rA + playerCollisionRadius() + 4.5f;
                const float pDistSq = fromPlayer.lengthSquared();
                if (pDistSq < pAvoidR * pAvoidR && pDistSq > 1e-4f) {
                    const float d = std::sqrt(pDistSq);
                    const float t = (pAvoidR - d) / pAvoidR;
                    steer += (fromPlayer / d) * (t * 9.5f);
                }
            } else {
                // Reef fish steer around nearby mantas
                const float rF = effectiveFishRadius(a);
                for (const auto& m : m_world.mantaAgents) {
                    const QVector3D diff = a.pos - m.pos;
                    const float avoidR = rF + effectiveMantaRadius(m) + 2.8f;
                    const float dSq = diff.lengthSquared();
                    if (dSq < avoidR * avoidR && dSq > 1e-4f) {
                        const float d = std::sqrt(dSq);
                        steer += (diff / d) * (((avoidR - d) / avoidR) * 11.0f);
                    }
                }
            }

            // Predictive avoidance of the whale hull
            const float whaleMargin = isMantaGroup ? 7.5f : 4.2f;
            steer += computeWhaleAvoidanceForce(a.pos, a.boundingRadius, whaleMargin);
            steer += computeWhaleAvoidanceForce(a.pos + fwd * lookAhead, a.boundingRadius, whaleMargin) * 0.65f;

            // Static environment avoidance
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

    stepGroup(m_world.fishAgents,  false, 4.8f,  3.2f, 1.1f, 2.5f, 0.75f, 7.5f, 0.40f, true);
    stepGroup(m_world.mantaAgents, true,  11.5f, 6.5f, 2.4f, 4.0f, 1.35f, 4.2f, 0.34f, false);
}

QVector3D DeepDiveEngine::computeAnglerLureWorldPos(const FaunaAgent& a) const {
    QVector3D localPos(0.0f, 0.84f, 1.38f);

    const float lureW = ProcMath::smoothstep(0.45f, 1.35f, localPos.z())
                        * ProcMath::smoothstep(0.15f, 0.85f, localPos.y());
    localPos.setX(localPos.x() + std::sin(m_time * 2.1f + a.phase) * 0.09f * lureW);
    localPos.setY(localPos.y() + std::cos(m_time * 1.7f + a.phase) * 0.07f * lureW);
    return faunaLocalToWorld(a, localPos);
}

void DeepDiveEngine::refreshLureLights() {
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

        const QVector3D toPlayer = m_playerPos - a.pos;
        const float distToPlayer = toPlayer.length();
        const QVector3D toHome = a.homeCenter - a.pos;
        const QVector3D horizToHome(toHome.x(), 0.0f, toHome.z());
        const float homeDist = horizToHome.length();

        const float floorY = m_world.collision.sampleHeight(a.pos.x(), a.pos.z());
        const float maxPitY = std::min(a.homeCenter.y() + 14.0f, -24.0f);
        const bool stalking = (distToPlayer < DETECT_RADIUS);

        QVector3D steer(0.0f, 0.0f, 0.0f);
        float targetSpeed = a.cruiseSpeed;

        if (stalking) {
            const QVector3D dirToPlayer = toPlayer / std::max(distToPlayer, 0.001f);
            QVector3D side(-dirToPlayer.z(), 0.0f, dirToPlayer.x());
            if (side.lengthSquared() > 1e-4f) side.normalize();

            if (distToPlayer > AMBUSH_MAX_DIST) {
                const float pull = ProcMath::clamp((distToPlayer - AMBUSH_MAX_DIST) / 14.0f, 0.25f, 1.0f);
                steer += dirToPlayer * (2.8f * pull);
                targetSpeed = a.cruiseSpeed * (0.75f + 0.55f * pull);
            } else if (distToPlayer < AMBUSH_MIN_DIST) {
                const float push = ProcMath::clamp((AMBUSH_MIN_DIST - distToPlayer) / 5.0f, 0.2f, 1.2f);
                steer -= dirToPlayer * (3.6f * push);
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

            const float hoverY = ProcMath::clamp(m_playerPos.y() - 0.8f, floorY + 4.4f, maxPitY);
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

        steer += computeWhaleAvoidanceForce(a.pos, a.boundingRadius, 6.0f);
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
        if (stalking && distToPlayer > 0.1f) {
            const float hDist = std::sqrt(toPlayer.x() * toPlayer.x() + toPlayer.z() * toPlayer.z());
            targetYaw   = std::atan2(toPlayer.x(), toPlayer.z());
            targetPitch = ProcMath::clamp(std::atan2(toPlayer.y(), std::max(hDist, 0.3f)), -0.45f, 0.45f);
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
}

void DeepDiveEngine::resolveFaunaAndPlayerCollisions(float& maxPlayerImpactSpeed) {
    auto& fishes  = m_world.fishAgents;
    auto& mantas  = m_world.mantaAgents;
    auto& anglers = m_world.anglerAgents;

    const float pRadius = playerCollisionRadius();
    const float pInvMass = (m_cameraMode == CameraMode::ThirdPersonFish) ? 1.0f
                                                                         : (m_cameraMode == CameraMode::ThirdPersonManta ? 0.22f : 0.35f);

    // Helper: resolve any dynamic sphere against the 7-sphere whale hull (infinite whale mass)
    auto resolveAgainstWhale = [&](QVector3D& pos, QVector3D& vel, float r, float* impactOut) -> bool {
        const float broad = m_whaleBroadRadius + r;
        if ((pos - m_whale.pos).lengthSquared() >= broad * broad) return false;

        bool hit = false;
        QVector3D whaleVelCopy = m_whale.vel;
        for (const auto& ws : m_whaleColliders) {
            QVector3D wsCenter = ws.center;
            if (resolveSpherePair(pos, vel, r, 1.0f,
                                  wsCenter, whaleVelCopy, ws.radius, 0.0f,
                                  1.0f, 1.08f, impactOut)) {
                hit = true;
            }
        }
        return hit;
    };

    // 1. Manta <-> Manta (2 relaxation passes, anisotropic to protect wide wingspan)
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < mantas.size(); ++i) {
            const float rA = effectiveMantaRadius(mantas[i]);
            for (size_t j = i + 1; j < mantas.size(); ++j) {
                const float rB = effectiveMantaRadius(mantas[j]);
                resolveSpherePair(mantas[i].pos, mantas[i].vel, rA, 1.0f,
                                  mantas[j].pos, mantas[j].vel, rB, 1.0f,
                                  1.65f, 1.12f);
            }
        }
    }

    // 2. Angler <-> Angler & Manta <-> Angler
    for (size_t i = 0; i < anglers.size(); ++i) {
        const float rA = effectiveAnglerRadius(anglers[i]);
        for (size_t j = i + 1; j < anglers.size(); ++j) {
            resolveSpherePair(anglers[i].pos, anglers[i].vel, rA, 1.0f,
                              anglers[j].pos, anglers[j].vel, effectiveAnglerRadius(anglers[j]), 1.0f,
                              1.0f, 1.10f);
        }
        for (auto& m : mantas) {
            resolveSpherePair(m.pos, m.vel, effectiveMantaRadius(m), 0.6f,
                              anglers[i].pos, anglers[i].vel, rA, 0.4f,
                              1.25f, 1.10f);
        }
    }

    // 3. Fish <-> Fish via flat 3D spatial hash (zero heap allocations)
    if (fishes.size() > 1) {
        constexpr uint32_t BUCKET_COUNT = 2048u;
        constexpr uint32_t BUCKET_MASK  = BUCKET_COUNT - 1u;
        constexpr float CELL_SIZE = 3.2f;

        auto cellHash = [](int cx, int cy, int cz) -> uint32_t {
            return ((static_cast<uint32_t>(cx) * 73856093u)
                    ^ (static_cast<uint32_t>(cy) * 19349663u)
                    ^ (static_cast<uint32_t>(cz) * 83492791u)) & BUCKET_MASK;
        };

        std::array<int, BUCKET_COUNT> head;
        head.fill(-1);
        if (m_fishHashNext.size() != fishes.size()) {
            m_fishHashNext.resize(fishes.size(), -1);
        }

        for (int i = 0; i < static_cast<int>(fishes.size()); ++i) {
            const auto& p = fishes[i].pos;
            const uint32_t h = cellHash(static_cast<int>(std::floor(p.x() / CELL_SIZE)),
                                        static_cast<int>(std::floor(p.y() / CELL_SIZE)),
                                        static_cast<int>(std::floor(p.z() / CELL_SIZE)));
            m_fishHashNext[i] = head[h];
            head[h] = i;
        }

        for (int i = 0; i < static_cast<int>(fishes.size()); ++i) {
            const auto& p = fishes[i].pos;
            const int bx = static_cast<int>(std::floor(p.x() / CELL_SIZE));
            const int by = static_cast<int>(std::floor(p.y() / CELL_SIZE));
            const int bz = static_cast<int>(std::floor(p.z() / CELL_SIZE));
            const float rA = effectiveFishRadius(fishes[i]);

            uint32_t visited[27];
            int visitedCount = 0;

            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const uint32_t h = cellHash(bx + dx, by + dy, bz + dz);
                        bool seen = false;
                        for (int k = 0; k < visitedCount; ++k) {
                            if (visited[k] == h) { seen = true; break; }
                        }
                        if (seen) continue;
                        visited[visitedCount++] = h;

                        for (int j = head[h]; j != -1; j = m_fishHashNext[j]) {
                            if (j <= i) continue;
                            resolveSpherePair(fishes[i].pos, fishes[i].vel, rA, 1.0f,
                                              fishes[j].pos, fishes[j].vel, effectiveFishRadius(fishes[j]), 1.0f,
                                              1.0f, 1.05f);
                        }
                    }
                }
            }
        }
    }

    // 4. Fish <-> Mantas & Anglers, and All Fauna <-> Whale
    for (auto& f : fishes) {
        const float rF = effectiveFishRadius(f);
        bool moved = resolveAgainstWhale(f.pos, f.vel, rF, nullptr);

        for (auto& m : mantas) {
            if (resolveSpherePair(f.pos, f.vel, rF, 0.95f,
                                  m.pos, m.vel, effectiveMantaRadius(m), 0.05f,
                                  1.35f, 1.08f)) {
                moved = true;
            }
        }
        for (auto& ang : anglers) {
            if (resolveSpherePair(f.pos, f.vel, rF, 0.95f,
                                  ang.pos, ang.vel, effectiveAnglerRadius(ang), 0.05f,
                                  1.0f, 1.08f)) {
                moved = true;
            }
        }
        if (moved) {
            m_world.collision.resolveSphereCollision(f.pos, &f.vel, f.boundingRadius, 1.1f, 2.5f);
        }
    }

    for (auto& m : mantas) {
        if (resolveAgainstWhale(m.pos, m.vel, effectiveMantaRadius(m), nullptr)) {
            m_world.collision.resolveSphereCollision(m.pos, &m.vel, m.boundingRadius, 2.4f, 4.0f);
        }
    }

    for (auto& ang : anglers) {
        if (resolveAgainstWhale(ang.pos, ang.vel, effectiveAnglerRadius(ang), nullptr)) {
            m_world.collision.resolveSphereCollision(ang.pos, &ang.vel, ang.boundingRadius, 1.8f, 14.0f);
        }
    }

    // 5. Player <-> Whale (2 passes so player never clips inside the moving whale)
    bool playerHitFauna = false;
    for (int pass = 0; pass < 2; ++pass) {
        if (resolveAgainstWhale(m_playerPos, m_playerVel, pRadius, &maxPlayerImpactSpeed)) {
            playerHitFauna = true;
        }
    }

    // 6. Player <-> Mantas
    for (auto& m : mantas) {
        const float yAniso = (m_cameraMode == CameraMode::ThirdPersonManta) ? 1.55f : 1.25f;
        if (resolveSpherePair(m_playerPos, m_playerVel, pRadius, pInvMass,
                              m.pos, m.vel, effectiveMantaRadius(m), 0.25f,
                              yAniso, 1.08f, &maxPlayerImpactSpeed)) {
            playerHitFauna = true;
            m_world.collision.resolveSphereCollision(m.pos, &m.vel, m.boundingRadius, 2.4f, 4.0f);
        }
    }

    // 7. Player <-> Anglers
    for (auto& ang : anglers) {
        if (resolveSpherePair(m_playerPos, m_playerVel, pRadius, pInvMass,
                              ang.pos, ang.vel, effectiveAnglerRadius(ang), 0.30f,
                              1.0f, 1.08f, &maxPlayerImpactSpeed)) {
            playerHitFauna = true;
            m_world.collision.resolveSphereCollision(ang.pos, &ang.vel, ang.boundingRadius, 1.8f, 14.0f);
        }
    }

    // 8. Player <-> Fish
    const float fishInvMass = 1.0f;
    const float playerVsFishInvMass = (m_cameraMode == CameraMode::ThirdPersonFish) ? 0.65f : 0.08f;
    for (auto& f : fishes) {
        if (resolveSpherePair(m_playerPos, m_playerVel, pRadius, playerVsFishInvMass,
                              f.pos, f.vel, effectiveFishRadius(f), fishInvMass,
                              1.0f, 1.06f)) {
            if (m_cameraMode == CameraMode::ThirdPersonFish) {
                playerHitFauna = true;
            }
            m_world.collision.resolveSphereCollision(f.pos, &f.vel, f.boundingRadius, 1.1f, 2.5f);
        }
    }

    // Re-clamp player against terrain/rocks if fauna pushed the player near geometry
    if (playerHitFauna) {
        m_playerColliding = true;
        m_world.collision.resolveSphereCollision(
            m_playerPos, &m_playerVel,
            pRadius, playerFloorMargin(), playerSurfaceMargin());
    }
}

void DeepDiveEngine::updateParticles(float dt) {
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
}

// ============================================================================
// MAIN SIMULATION STEP
// ============================================================================

EngineStepEvents DeepDiveEngine::update(float dt, const EngineInput& input) {
    EngineStepEvents events;
    m_time += dt;

    m_yaw += input.yawDelta;
    m_pitch = ProcMath::clamp(m_pitch + input.pitchDelta, -1.48f, 1.48f);

    // 1. Update whale first so its multi-sphere hull is fresh for player & fauna avoidance
    updateWhaleNavigation(dt, events.whaleCalled);
    refreshWhaleColliders();

    // 2. Integrate player body movement
    const QVector3D fwd = cameraForward();
    const QVector3D rgt = cameraRight();
    QVector3D wishDir = fwd * input.moveForward
                        + rgt * input.moveRight
                        + QVector3D(0.0f, input.moveUp, 0.0f);

    const float wishLenSq = wishDir.lengthSquared();
    const bool isMoving = (wishLenSq > 1e-4f);
    if (wishLenSq > 1.0f) {
        wishDir /= std::sqrt(wishLenSq);
    }

    float accel = 18.0f;
    float drag  = 3.0f;
    if (m_cameraMode == CameraMode::ThirdPersonFish) {
        accel = input.sprint ? 48.0f : 19.0f;
        drag  = 3.4f;
    } else if (m_cameraMode == CameraMode::ThirdPersonManta) {
        accel = input.sprint ? 54.0f : 21.0f;
        drag  = 2.35f;
    } else {
        accel = input.sprint ? 58.0f : 18.0f;
        drag  = 3.0f;
    }

    const float preCollisionSpeed = m_playerVel.length();

    m_playerVel += wishDir * (accel * dt);
    m_playerVel *= std::exp(-drag * dt);
    m_playerPos += m_playerVel * dt;
    m_playerPos.setY(m_playerPos.y() + std::sin(m_time * 1.4f) * 0.006f);

    const bool hitEnv = m_world.collision.resolveSphereCollision(
        m_playerPos, &m_playerVel,
        playerCollisionRadius(), playerFloorMargin(), playerSurfaceMargin());
    m_playerColliding = hitEnv;

    // 3. Step fish, mantas & anglers, then resolve all mutual fauna & player collisions
    updateFaunaAgents(dt);
    updateAnglerAgents(dt);

    float maxImpactSpeed = hitEnv ? preCollisionSpeed : 0.0f;
    resolveFaunaAndPlayerCollisions(maxImpactSpeed);

    if (m_playerColliding && !m_wasPlayerColliding) {
        events.playerCollisionImpact = true;
        events.collisionImpactSpeed = maxImpactSpeed;
    }
    m_wasPlayerColliding = m_playerColliding;

    // 4. Sync third-person avatar orientation and compute collision-aware camera position
    updatePlayerAvatarAndCamera(dt, wishDir, isMoving);
    refreshLureLights();
    updateParticles(dt);

    return events;
}
