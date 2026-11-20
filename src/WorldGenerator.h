#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

struct EnvVertex {
    QVector3D pos;
    QVector3D normal;
    QVector3D color;
    QVector2D param; // x: 0=rock/sand, 1=shipwood, 2=coral/grass/sponge, 3=kelp, 4=abyssalGlow; y: sway/heightFactor
};

struct FaunaVertex {
    QVector3D pos;
    QVector3D normal;
    QVector3D color; // > 1.3 по компоненте используется как биолюминесценция (фонарик удильщика)
};

// Компактная структура инстанса для GPU (передаются только видимые объекты)
struct FaunaGpuInstance {
    QVector4D posScale;  // xyz: worldPos, w: scale
    QVector4D rotPhase;  // x: yaw, y: phase, z: type (0=fish, 1=manta, 2=whale, 3=angler), w: bank
    QVector4D pitchTint; // x: pitch, yzw: color tint
};

// Физический агент фауны на CPU (руление и обход препятствий)
struct FaunaAgent {
    QVector3D pos;
    QVector3D vel;
    QVector3D homeCenter;
    QVector3D tint{1.0f, 1.0f, 1.0f};
    float homeRadius = 25.0f;
    float preferredDepth = -8.0f;
    float cruiseSpeed = 4.0f;
    float scale = 1.0f;
    float boundingRadius = 1.0f;
    float phase = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float bank = 0.0f;
    float type = 0.0f; // 0 = fish, 1 = manta, 2 = whale, 3 = anglerfish
};

struct RayVertex {
    QVector3D pos;
    QVector4D color;
    QVector2D uv;
};

struct Particle {
    QVector3D pos;
    QVector4D colorSize;
    float speedY;
    float phase;
};

struct MeshRange {
    uint32_t indexOffset = 0;
    uint32_t indexCount  = 0;
};

struct SphereCollider {
    QVector3D center;
    float radius = 1.0f;
};

struct OBBCollider {
    QVector3D center;
    QVector3D axisX;
    QVector3D axisY;
    QVector3D axisZ;
    QVector3D halfExtents;
    float boundingRadius = 1.0f;
};

struct WorldChunk {
    int cx = 0;
    int cz = 0;
    QVector3D minBounds;
    QVector3D maxBounds;
    QVector3D center;
    float boundingRadius = 60.0f;

    uint32_t terrainBaseVertex = 0;

    MeshRange boulderLod0;
    MeshRange boulderLod1;
    MeshRange kelpLod0;
    MeshRange kelpLod1;
    MeshRange floraLod0;      // морская трава, ветвистые/пластинчатые кораллы, веерные горгонарии, кустистые водоросли
    MeshRange floraLod1;      // облегченные силуэты флоры на средней/дальней дистанции
    MeshRange detailRange;    // губки, актинии/анемоны и глубоководные светящиеся перья
    MeshRange shipwreckRange;

    std::vector<uint32_t> sphereColliderIds;
    std::vector<uint32_t> obbColliderIds;
};

struct CollisionWorld {
    uint32_t seed = 42u;
    int chunksPerSide = 20;
    float chunkSize = 80.0f;
    float worldHalfSize = 800.0f;

    std::vector<SphereCollider> spheres;
    std::vector<OBBCollider> obbs;
    std::vector<std::vector<uint32_t>> cellSpheres;
    std::vector<std::vector<uint32_t>> cellObbs;

    void buildGrid();
    QVector3D computeAvoidanceForce(const QVector3D& pos, float probeRadius, float margin) const;
    bool resolveSphereCollision(QVector3D& pos, QVector3D* vel, float radius,
                                float minFloorMargin = 0.0f, float surfaceMargin = 1.5f) const;
    bool isPointClear(const QVector3D& pos, float radius) const;
};

struct WorldData {
    static constexpr int CHUNKS_PER_SIDE = 20;
    static constexpr float CHUNK_SIZE = 80.0f;
    static constexpr int CHUNK_GRID_N = 48; // 48x48 ячеек (шаг 1.67 м на LOD0; делится на 1, 2, 4, 8)
    static constexpr int CHUNK_VERTS_PER_CHUNK = (CHUNK_GRID_N + 1) * (CHUNK_GRID_N + 1) + 4 * (CHUNK_GRID_N + 1);
    static constexpr int TERRAIN_LOD_COUNT = 4;

    std::vector<EnvVertex> terrainVertices;
    std::vector<uint32_t> terrainIndices;
    std::array<MeshRange, TERRAIN_LOD_COUNT> terrainLodRanges{};

    std::vector<EnvVertex> propVertices;
    std::vector<uint32_t> propIndices;

    std::vector<WorldChunk> chunks;
    CollisionWorld collision;

    std::vector<FaunaVertex> fishLod0Verts;
    std::vector<uint32_t>    fishLod0Inds;
    std::vector<FaunaVertex> fishLod1Verts;
    std::vector<uint32_t>    fishLod1Inds;
    std::vector<FaunaAgent>  fishAgents;

    std::vector<FaunaVertex> mantaLod0Verts;
    std::vector<uint32_t>    mantaLod0Inds;
    std::vector<FaunaVertex> mantaLod1Verts;
    std::vector<uint32_t>    mantaLod1Inds;
    std::vector<FaunaAgent>  mantaAgents;

    std::vector<FaunaVertex> whaleLod0Verts;
    std::vector<uint32_t>    whaleLod0Inds;
    std::vector<FaunaVertex> whaleLod1Verts;
    std::vector<uint32_t>    whaleLod1Inds;

    std::vector<FaunaVertex> anglerLod0Verts;
    std::vector<uint32_t>    anglerLod0Inds;
    std::vector<FaunaVertex> anglerLod1Verts;
    std::vector<uint32_t>    anglerLod1Inds;
    std::vector<FaunaAgent>  anglerAgents;

    std::vector<RayVertex> godRayVertices;
    std::vector<Particle>  particles;
};

class WorldGenerator {
public:
    static WorldData generate(uint32_t seed = 42u);

private:
    static void buildTerrainLodIndices(WorldData& data);
    static void generateTerrainChunks(WorldData& data, uint32_t seed);
    static void generateChunkProps(WorldData& data, uint32_t seed);
    static void generateShipwrecks(WorldData& data, uint32_t seed);
    static void generateFaunaMeshes(WorldData& data);
    static void generateWhaleMeshes(WorldData& data);
    static void generateAnglerMeshes(WorldData& data);
    static void generateFaunaAgents(WorldData& data, uint32_t seed);
    static void generateGodRaysAndParticles(WorldData& data, uint32_t seed);

    static void appendBox(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                          const QVector3D& center, const QVector3D& size,
                          const QVector3D& right, const QVector3D& up, const QVector3D& forward,
                          const QVector3D& color, float matType);

    static void appendRockBoulder(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                                  const QVector3D& center, float rx, float ry, float rz,
                                  int stacks, int slices, uint32_t rockSeed, const QVector3D& baseColor);

    static void appendKelpStalk(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                                const QVector3D& rootPos, float height, uint32_t stalkSeed, bool highLod);
};
