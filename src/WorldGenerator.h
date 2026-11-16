#pragma once
#include <vector>
#include <cstdint>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

struct EnvVertex {
    QVector3D pos;
    QVector3D normal;
    QVector3D color;
    QVector2D param;
};

struct FaunaVertex {
    QVector3D pos;
    QVector3D normal;
    QVector3D color;
};

struct FaunaInstance {
    QVector4D posScale; // xyz: position, w: scale
    QVector4D rotPhase; // x: yaw, y: phase, z: type (0=fish, 1=manta, 2=whale), w: bankAngle
    QVector3D orbitCenter;
    float orbitRadius;
    float orbitAngle;
    float angularSpeed;
    float baseHeight;
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

struct WorldData {
    std::vector<EnvVertex> envVertices;
    std::vector<uint32_t> envIndices;

    std::vector<FaunaVertex> fishMeshVertices;
    std::vector<uint32_t> fishMeshIndices;
    std::vector<FaunaInstance> fishInstances;

    std::vector<FaunaVertex> mantaMeshVertices;
    std::vector<uint32_t> mantaMeshIndices;
    std::vector<FaunaInstance> mantaInstances;

    std::vector<FaunaVertex> whaleMeshVertices;
    std::vector<uint32_t> whaleMeshIndices;
    std::vector<FaunaInstance> whaleInstances;

    std::vector<RayVertex> godRayVertices;
    std::vector<Particle> particles;
};

class WorldGenerator {
public:
    static WorldData generate(uint32_t seed = 42u);

private:
    static void generateTerrain(WorldData& data, uint32_t seed);
    static void generateRockBoulders(WorldData& data, uint32_t seed);
    static void generateKelpForest(WorldData& data, uint32_t seed);
    static void generateSpongesAndCorals(WorldData& data, uint32_t seed);
    static void generateShipwrecks(WorldData& data, uint32_t seed);
    static void generateFaunaMeshes(WorldData& data);
    static void generateWhaleMesh(WorldData& data);
    static void generateFaunaInstances(WorldData& data, uint32_t seed);
    static void generateGodRaysAndParticles(WorldData& data, uint32_t seed);

    static void addBox(WorldData& data, const QVector3D& center, const QVector3D& size,
                       const QVector3D& right, const QVector3D& up, const QVector3D& forward,
                       const QVector3D& color, float matType);
};
