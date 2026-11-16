#include "WorldGenerator.h"
#include "Noise.h"
#include <cmath>
#include <random>

namespace {
constexpr float PI = 3.14159265358979323846f;

QVector3D calcTerrainNormal(float x, float z, uint32_t seed) {
    const float eps = 0.55f;
    float hL = ProcMath::sampleTerrainHeight(x - eps, z, seed);
    float hR = ProcMath::sampleTerrainHeight(x + eps, z, seed);
    float hD = ProcMath::sampleTerrainHeight(x, z - eps, seed);
    float hU = ProcMath::sampleTerrainHeight(x, z + eps, seed);
    return QVector3D(hL - hR, 2.0f * eps, hD - hU).normalized();
}
} // namespace

WorldData WorldGenerator::generate(uint32_t seed) {
    WorldData data;
    data.envVertices.reserve(320000);
    data.envIndices.reserve(780000);

    generateTerrain(data, seed);
    generateRockBoulders(data, seed);
    generateKelpForest(data, seed);
    generateSpongesAndCorals(data, seed);
    generateShipwrecks(data, seed);
    generateFaunaMeshes(data);
    generateWhaleMesh(data);
    generateFaunaInstances(data, seed);
    generateGodRaysAndParticles(data, seed);

    return data;
}

void WorldGenerator::generateTerrain(WorldData& data, uint32_t seed) {
    const int gridN = 220;
    const float worldSize = 240.0f;
    const float step = worldSize / static_cast<float>(gridN);
    const float half = worldSize * 0.5f;

    uint32_t baseIdx = static_cast<uint32_t>(data.envVertices.size());

    for (int iz = 0; iz <= gridN; ++iz) {
        for (int ix = 0; ix <= gridN; ++ix) {
            float x = -half + ix * step;
            float z = -half + iz * step;
            float y = ProcMath::sampleTerrainHeight(x, z, seed);
            QVector3D n = calcTerrainNormal(x, z, seed);

            float biomeNoise = ProcMath::fbm(x * 0.02f, 2.5f, z * 0.02f, 3, seed + 99u);
            QVector3D sandCol(0.16f, 0.52f, 0.55f);
            QVector3D rockCol(0.11f, 0.34f, 0.38f);
            QVector3D purpleMoss(0.42f, 0.18f, 0.50f);

            float slope = 1.0f - ProcMath::clamp(n.y(), 0.0f, 1.0f);
            QVector3D col = ProcMath::mix(sandCol, rockCol, ProcMath::smoothstep(0.12f, 0.50f, slope));

            if (biomeNoise > 0.12f && slope < 0.45f) {
                float mossFactor = ProcMath::smoothstep(0.12f, 0.45f, biomeNoise);
                col = ProcMath::mix(col, purpleMoss, mossFactor * 0.72f);
            }

            data.envVertices.push_back({QVector3D(x, y, z), n, col, QVector2D(0.0f, 0.0f)});
        }
    }

    for (int iz = 0; iz < gridN; ++iz) {
        for (int ix = 0; ix < gridN; ++ix) {
            uint32_t i0 = baseIdx + iz * (gridN + 1) + ix;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + (gridN + 1);
            uint32_t i3 = i2 + 1;
            data.envIndices.insert(data.envIndices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
}

void WorldGenerator::generateRockBoulders(WorldData& data, uint32_t seed) {
    std::mt19937 rng(seed + 555u);
    std::uniform_real_distribution<float> distPos(-95.0f, 95.0f);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    const int boulderCount = 220;
    const int stacks = 14;
    const int slices = 20;

    for (int b = 0; b < boulderCount; ++b) {
        float cx = distPos(rng);
        float cz = distPos(rng);
        float cy = ProcMath::sampleTerrainHeight(cx, cz, seed);

        float rx = 2.4f + dist01(rng) * 5.6f;
        float ry = 1.7f + dist01(rng) * 4.0f;
        float rz = 2.4f + dist01(rng) * 5.6f;

        QVector3D rockCol(0.12f + dist01(rng) * 0.05f, 0.35f + dist01(rng) * 0.08f, 0.39f + dist01(rng) * 0.08f);
        uint32_t baseIdx = static_cast<uint32_t>(data.envVertices.size());

        auto displacedPos = [&](float theta, float phi) -> QVector3D {
            QVector3D dir(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            float n = ProcMath::fbm(dir.x() * 1.45f + b * 3.1f, dir.y() * 1.45f, dir.z() * 1.45f + b * 1.7f, 3, seed);
            float disp = 1.0f + n * 0.30f;
            return QVector3D(cx + dir.x() * rx * disp,
                             cy + dir.y() * ry * disp - ry * 0.28f,
                             cz + dir.z() * rz * disp);
        };

        for (int st = 0; st <= stacks; ++st) {
            float v = static_cast<float>(st) / stacks;
            float theta = v * PI;
            for (int sl = 0; sl <= slices; ++sl) {
                float u = static_cast<float>(sl % slices) / slices;
                float phi = u * 2.0f * PI;

                QVector3D p = displacedPos(theta, phi);
                QVector3D sphereN(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
                QVector3D pTheta = displacedPos(std::min(PI, theta + 0.06f), phi);
                QVector3D pPhi   = displacedPos(theta, phi + 0.06f);
                QVector3D dispN  = QVector3D::crossProduct(pTheta - p, pPhi - p).normalized();

                QVector3D n = (dispN.lengthSquared() > 0.01f)
                                  ? (dispN * 0.55f + sphereN * 0.45f).normalized()
                                  : sphereN;

                data.envVertices.push_back({p, n, rockCol, QVector2D(0.0f, 0.0f)});
            }
        }

        for (int st = 0; st < stacks; ++st) {
            for (int sl = 0; sl < slices; ++sl) {
                uint32_t i0 = baseIdx + st * (slices + 1) + sl;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = i0 + (slices + 1);
                uint32_t i3 = i2 + 1;
                data.envIndices.insert(data.envIndices.end(), {i0, i2, i1, i1, i2, i3});
            }
        }
    }
}

void WorldGenerator::generateKelpForest(WorldData& data, uint32_t seed) {
    std::mt19937 rng(seed + 101u);
    std::uniform_real_distribution<float> distPos(-95.0f, 95.0f);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    const int kelpCount = 420;
    for (int k = 0; k < kelpCount; ++k) {
        float x = distPos(rng);
        float z = distPos(rng);
        float yBase = ProcMath::sampleTerrainHeight(x, z, seed);
        if (yBase > -1.5f) continue;

        float totalHeight = 14.0f + dist01(rng) * 22.0f;
        int segments = 20;
        float segH = totalHeight / static_cast<float>(segments);

        bool isPurpleBiome = ProcMath::fbm(x * 0.02f, 2.5f, z * 0.02f, 3, seed + 99u) > 0.22f;
        QVector3D baseKelpCol = isPurpleBiome
                                    ? QVector3D(0.32f, 0.22f, 0.52f)
                                    : QVector3D(0.12f, 0.46f, 0.24f);
        QVector3D tipKelpCol = isPurpleBiome
                                   ? QVector3D(0.55f, 0.42f, 0.68f)
                                   : QVector3D(0.38f, 0.68f, 0.28f);

        for (int s = 0; s < segments; ++s) {
            float h0 = static_cast<float>(s) / segments;
            float h1 = static_cast<float>(s + 1) / segments;
            float y0 = yBase + s * segH;
            float y1 = y0 + segH;
            float w0 = 0.11f * (1.0f - h0 * 0.55f);
            float w1 = 0.11f * (1.0f - h1 * 0.55f);

            QVector3D c0 = ProcMath::mix(baseKelpCol, tipKelpCol, h0);
            QVector3D c1 = ProcMath::mix(baseKelpCol, tipKelpCol, h1);

            for (int plane = 0; plane < 2; ++plane) {
                QVector3D off0 = (plane == 0) ? QVector3D(w0, 0, 0) : QVector3D(0, 0, w0);
                QVector3D off1 = (plane == 0) ? QVector3D(w1, 0, 0) : QVector3D(0, 0, w1);
                QVector3D nrm  = (plane == 0) ? QVector3D(0, 0.35f, 1).normalized() : QVector3D(1, 0.35f, 0).normalized();

                uint32_t idx = static_cast<uint32_t>(data.envVertices.size());
                data.envVertices.push_back({QVector3D(x, y0, z) - off0, nrm, c0, QVector2D(3.0f, h0)});
                data.envVertices.push_back({QVector3D(x, y0, z) + off0, nrm, c0, QVector2D(3.0f, h0)});
                data.envVertices.push_back({QVector3D(x, y1, z) + off1, nrm, c1, QVector2D(3.0f, h1)});
                data.envVertices.push_back({QVector3D(x, y1, z) - off1, nrm, c1, QVector2D(3.0f, h1)});
                data.envIndices.insert(data.envIndices.end(), {idx, idx + 1, idx + 2, idx, idx + 2, idx + 3});
            }

            for (int l = 0; l < 2; ++l) {
                float angle = (s * 2.39996f + l * PI);
                QVector3D dir(std::cos(angle), 0.0f, std::sin(angle));
                QVector3D side(-dir.z(), 0.0f, dir.x());

                float leafLen = (1.35f + 0.75f * std::sin(h0 * PI)) * (0.80f + 0.40f * dist01(rng));
                float maxWid  = 0.24f * (0.75f + 0.35f * std::sin(h0 * PI));
                QVector3D root(x, (y0 + y1) * 0.5f, z);

                constexpr int bladeSegs = 4;
                uint32_t bladeBase = static_cast<uint32_t>(data.envVertices.size());

                for (int bs = 0; bs <= bladeSegs; ++bs) {
                    float t = static_cast<float>(bs) / bladeSegs;
                    float w = std::sin(std::pow(t, 0.7f) * PI) * maxWid;
                    float archY = std::sin(t * PI * 0.85f) * 0.26f - t * t * 0.42f;
                    float waveTwist = std::sin(t * 4.5f + s) * 0.06f;

                    QVector3D center = root + dir * (t * leafLen) + QVector3D(0.0f, archY, 0.0f);
                    QVector3D bladeN = (QVector3D(0.0f, 1.0f, 0.0f) + dir * (0.35f * t) + side * waveTwist).normalized();
                    QVector3D bladeCol = ProcMath::mix(c0, c1 * 1.18f, t);
                    float hBlade = ProcMath::clamp(h0 + t * 0.08f, 0.0f, 1.0f);

                    data.envVertices.push_back({center - side * w, bladeN, bladeCol, QVector2D(3.0f, hBlade)});
                    data.envVertices.push_back({center + side * w, bladeN, bladeCol, QVector2D(3.0f, hBlade)});
                }

                for (int bs = 0; bs < bladeSegs; ++bs) {
                    uint32_t i0 = bladeBase + bs * 2;
                    uint32_t i1 = i0 + 1;
                    uint32_t i2 = i0 + 2;
                    uint32_t i3 = i0 + 3;
                    data.envIndices.insert(data.envIndices.end(), {i0, i1, i3, i0, i3, i2});
                }
            }
        }
    }
}

void WorldGenerator::generateSpongesAndCorals(WorldData& data, uint32_t seed) {
    std::mt19937 rng(seed + 202u);
    std::uniform_real_distribution<float> distPos(-85.0f, 85.0f);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    const int spongeCount = 150;
    for (int i = 0; i < spongeCount; ++i) {
        float cx = distPos(rng);
        float cz = distPos(rng);
        float cy = ProcMath::sampleTerrainHeight(cx, cz, seed) - 0.15f;

        float radius = 0.55f + dist01(rng) * 0.85f;
        float height = 0.9f + dist01(rng) * 1.6f;
        int slices = 18;
        int stacks = 9;

        bool isPurple = dist01(rng) > 0.45f;
        QVector3D spongeCol = isPurple
                                  ? QVector3D(0.46f, 0.28f, 0.65f)
                                  : QVector3D(0.55f, 0.42f, 0.24f);
        QVector3D rimCol = spongeCol * 1.35f;

        uint32_t baseIdx = static_cast<uint32_t>(data.envVertices.size());
        for (int st = 0; st <= stacks; ++st) {
            float v = static_cast<float>(st) / stacks;
            float profile = std::sin(v * PI * 0.78f + 0.35f);
            float rOuter = radius * (0.75f + 0.35f * profile);
            float y = cy + v * height;

            for (int sl = 0; sl <= slices; ++sl) {
                float u = static_cast<float>(sl % slices) / slices;
                float a = u * 2.0f * PI;
                float bumpy = 1.0f + 0.12f * ProcMath::noise3D(cx + std::cos(a) * 2.0f, v * 4.0f, cz + std::sin(a) * 2.0f, seed);
                float px = cx + std::cos(a) * rOuter * bumpy;
                float pz = cz + std::sin(a) * rOuter * bumpy;

                QVector3D n(std::cos(a), 0.25f * (v - 0.5f), std::sin(a));
                QVector3D col = ProcMath::mix(spongeCol * 0.7f, rimCol, v);
                data.envVertices.push_back({QVector3D(px, y, pz), n.normalized(), col, QVector2D(2.0f, 0.0f)});
            }
        }

        for (int st = 0; st < stacks; ++st) {
            for (int sl = 0; sl < slices; ++sl) {
                uint32_t i0 = baseIdx + st * (slices + 1) + sl;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = i0 + (slices + 1);
                uint32_t i3 = i2 + 1;
                data.envIndices.insert(data.envIndices.end(), {i0, i1, i2, i1, i3, i2});
            }
        }
    }

    const int coralClusters = 220;
    for (int i = 0; i < coralClusters; ++i) {
        float cx = distPos(rng);
        float cz = distPos(rng);
        float cy = ProcMath::sampleTerrainHeight(cx, cz, seed);

        QVector3D coralCol = (dist01(rng) > 0.5f)
                                 ? QVector3D(0.78f, 0.28f, 0.52f)
                                 : QVector3D(0.62f, 0.22f, 0.75f);

        int branches = 5 + static_cast<int>(dist01(rng) * 6.0f);
        for (int b = 0; b < branches; ++b) {
            float ang = dist01(rng) * 2.0f * PI;
            float tilt = 0.25f + dist01(rng) * 0.55f;
            float len = 0.8f + dist01(rng) * 1.5f;

            QVector3D p0(cx, cy, cz);
            QVector3D p1 = p0 + QVector3D(std::cos(ang) * tilt * len, len, std::sin(ang) * tilt * len);
            QVector3D side(-std::sin(ang) * 0.08f, 0.0f, std::cos(ang) * 0.08f);
            QVector3D n(std::cos(ang), 0.6f, std::sin(ang));

            uint32_t idx = static_cast<uint32_t>(data.envVertices.size());
            data.envVertices.push_back({p0 - side, n.normalized(), coralCol * 0.7f, QVector2D(2.0f, 0.0f)});
            data.envVertices.push_back({p0 + side, n.normalized(), coralCol * 0.7f, QVector2D(2.0f, 0.0f)});
            data.envVertices.push_back({p1 + side * 0.5f, n.normalized(), coralCol * 1.2f, QVector2D(2.0f, 1.0f)});
            data.envVertices.push_back({p1 - side * 0.5f, n.normalized(), coralCol * 1.2f, QVector2D(2.0f, 1.0f)});
            data.envIndices.insert(data.envIndices.end(), {idx, idx + 1, idx + 2, idx, idx + 2, idx + 3});
        }
    }
}

void WorldGenerator::addBox(WorldData& data, const QVector3D& center, const QVector3D& size,
                            const QVector3D& right, const QVector3D& up, const QVector3D& forward,
                            const QVector3D& color, float matType) {
    QVector3D r = right * (size.x() * 0.5f);
    QVector3D u = up * (size.y() * 0.5f);
    QVector3D f = forward * (size.z() * 0.5f);

    QVector3D corners[8] = {
        center - r - u - f, center + r - u - f, center + r + u - f, center - r + u - f,
        center - r - u + f, center + r - u + f, center + r + u + f, center - r + u + f
    };

    const int faces[6][4] = {
        {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
        {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}
    };
    const QVector3D normals[6] = {-forward, forward, -right, right, up, -up};

    for (int i = 0; i < 6; ++i) {
        uint32_t idx = static_cast<uint32_t>(data.envVertices.size());
        for (int k = 0; k < 4; ++k) {
            QVector3D cPos = corners[faces[i][k]];
            QVector3D smoothN = (normals[i] * 0.78f + (cPos - center).normalized() * 0.22f).normalized();
            data.envVertices.push_back({cPos, smoothN, color, QVector2D(matType, 0.0f)});
        }
        data.envIndices.insert(data.envIndices.end(), {idx, idx + 1, idx + 2, idx, idx + 2, idx + 3});
    }
}

void WorldGenerator::generateShipwrecks(WorldData& data, uint32_t /*seed*/) {
    struct WreckSpec {
        QVector3D pos;
        float yaw;
        float pitch;
        float roll;
        float scale;
    };

    std::vector<WreckSpec> wrecks = {
        {QVector3D(8.0f, -6.5f, -32.0f), 0.35f, 0.28f, -0.38f, 1.25f},
        {QVector3D(-22.0f, -16.5f, 28.0f), -1.1f, -0.12f, 0.22f, 1.0f}
    };

    QVector3D woodDark(0.16f, 0.26f, 0.28f);
    QVector3D woodLight(0.24f, 0.36f, 0.37f);

    for (const auto& w : wrecks) {
        float cy = std::cos(w.yaw), sy = std::sin(w.yaw);
        float cp = std::cos(w.pitch), sp = std::sin(w.pitch);
        float cr = std::cos(w.roll), sr = std::sin(w.roll);

        QVector3D forward(sy * cp, sp, cy * cp);
        QVector3D right(cy * cr + sy * sp * sr, -cp * sr, -sy * cr + cy * sp * sr);
        QVector3D up = QVector3D::crossProduct(forward, right).normalized();
        right = QVector3D::crossProduct(up, forward).normalized();

        float s = w.scale;
        addBox(data, w.pos, QVector3D(0.7f * s, 0.9f * s, 26.0f * s), right, up, forward, woodDark, 1.0f);

        int ribCount = 16;
        for (int i = 0; i < ribCount; ++i) {
            float t = static_cast<float>(i) / (ribCount - 1) * 2.0f - 1.0f;
            float hullShape = 1.0f - t * t * 0.65f;
            float zOff = t * 12.0f * s;
            float ribWidth = 4.6f * hullShape * s;
            float ribHeight = 4.2f * (0.85f + 0.25f * t * t) * s;

            QVector3D ribBase = w.pos + forward * zOff;

            if (i % 2 == 0) {
                addBox(data, ribBase + up * (ribHeight * 0.85f),
                       QVector3D(ribWidth * 1.85f, 0.28f * s, 0.35f * s),
                       right, up, forward, woodLight, 1.0f);
            }

            for (int side = -1; side <= 1; side += 2) {
                for (int seg = 0; seg < 4; ++seg) {
                    float a0 = (seg / 4.0f) * (PI * 0.48f);
                    float a1 = ((seg + 1) / 4.0f) * (PI * 0.48f);
                    float am = (a0 + a1) * 0.5f;

                    float rx = std::sin(am) * ribWidth * side;
                    float ry = (1.0f - std::cos(am)) * ribHeight;
                    QVector3D segPos = ribBase + right * rx + up * ry;

                    QVector3D segUp = (right * (std::cos(am) * side) + up * std::sin(am)).normalized();
                    QVector3D segRight = QVector3D::crossProduct(segUp, forward).normalized();

                    addBox(data, segPos, QVector3D(0.32f * s, 1.45f * s, 0.35f * s),
                           segRight, segUp, forward, woodLight, 1.0f);

                    if ((i + seg) % 3 != 0 && i < ribCount - 1) {
                        addBox(data, segPos + forward * (0.75f * s),
                               QVector3D(0.16f * s, 0.42f * s, 1.65f * s),
                               segRight, segUp, forward, woodDark, 1.0f);
                    }
                }
            }
        }

        float mastOffsets[2] = {-3.5f * s, 4.5f * s};
        float mastHeights[2] = {18.0f * s, 15.0f * s};
        for (int m = 0; m < 2; ++m) {
            QVector3D mastCenter = w.pos + forward * mastOffsets[m] + up * (mastHeights[m] * 0.5f);
            addBox(data, mastCenter, QVector3D(0.55f * s, mastHeights[m], 0.55f * s),
                   right, up, forward, woodDark, 1.0f);

            addBox(data, w.pos + forward * mastOffsets[m] + up * (mastHeights[m] * 0.65f),
                   QVector3D(9.5f * s, 0.32f * s, 0.32f * s),
                   right, up, forward, woodLight, 1.0f);
            addBox(data, w.pos + forward * mastOffsets[m] + up * (mastHeights[m] * 0.88f),
                   QVector3D(6.5f * s, 0.26f * s, 0.26f * s),
                   right, up, forward, woodLight, 1.0f);
        }
    }
}

void WorldGenerator::generateFaunaMeshes(WorldData& data) {
    const int rings = 10;
    const int radial = 8;
    QVector3D dorsalCol(0.12f, 0.48f, 0.66f);
    QVector3D bellyCol(0.68f, 0.92f, 0.98f);
    QVector3D stripeCol(0.25f, 0.88f, 0.95f);

    for (int ir = 0; ir <= rings; ++ir) {
        float t = static_cast<float>(ir) / rings;
        float z = 0.55f - t * 1.0f;
        float profile = std::sin(std::pow(t, 0.68f) * PI);
        float rx = 0.11f * profile + 0.01f;
        float ry = 0.21f * profile + 0.015f;

        for (int ia = 0; ia < radial; ++ia) {
            float ang = (static_cast<float>(ia) / radial) * 2.0f * PI;
            float cx = std::cos(ang);
            float sy = std::sin(ang);

            QVector3D pos(cx * rx, sy * ry, z);
            QVector3D norm(cx / rx, sy / ry, 0.15f * (0.5f - t));
            norm.normalize();

            QVector3D col = ProcMath::mix(bellyCol, dorsalCol, sy * 0.5f + 0.5f);
            if (std::abs(sy) < 0.28f) col = stripeCol;
            data.fishMeshVertices.push_back({pos, norm, col});
        }
    }

    for (int ir = 0; ir < rings; ++ir) {
        for (int ia = 0; ia < radial; ++ia) {
            uint32_t i0 = ir * radial + ia;
            uint32_t i1 = ir * radial + ((ia + 1) % radial);
            uint32_t i2 = (ir + 1) * radial + ia;
            uint32_t i3 = (ir + 1) * radial + ((ia + 1) % radial);
            data.fishMeshIndices.insert(data.fishMeshIndices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }

    uint32_t fBase = static_cast<uint32_t>(data.fishMeshVertices.size());
    data.fishMeshVertices.push_back({QVector3D(0.0f,  0.0f, -0.42f), QVector3D(1, 0, 0), stripeCol});
    data.fishMeshVertices.push_back({QVector3D(0.0f,  0.26f,-0.75f), QVector3D(1, 0, 0), dorsalCol});
    data.fishMeshVertices.push_back({QVector3D(0.0f,  0.0f, -0.58f), QVector3D(1, 0, 0), bellyCol});
    data.fishMeshVertices.push_back({QVector3D(0.0f, -0.26f,-0.75f), QVector3D(1, 0, 0), dorsalCol});
    data.fishMeshVertices.push_back({QVector3D(0.0f,  0.18f, 0.12f), QVector3D(1, 0, 0), dorsalCol});
    data.fishMeshVertices.push_back({QVector3D(0.0f,  0.34f,-0.12f), QVector3D(1, 0, 0), stripeCol});
    data.fishMeshVertices.push_back({QVector3D(0.0f,  0.14f,-0.20f), QVector3D(1, 0, 0), dorsalCol});

    data.fishMeshIndices.insert(data.fishMeshIndices.end(), {
                                                                fBase, fBase + 1, fBase + 2,  fBase, fBase + 2, fBase + 1,
                                                                fBase, fBase + 2, fBase + 3,  fBase, fBase + 3, fBase + 2,
                                                                fBase + 4, fBase + 5, fBase + 6,  fBase + 4, fBase + 6, fBase + 5
                                                            });

    const int gridX = 16;
    const int gridZ = 12;
    QVector3D mantaBack(0.10f, 0.28f, 0.38f);
    QVector3D mantaWingTip(0.22f, 0.56f, 0.68f);
    QVector3D mantaBelly(0.65f, 0.88f, 0.92f);

    for (int side = 0; side < 2; ++side) {
        uint32_t layerBase = static_cast<uint32_t>(data.mantaMeshVertices.size());
        float signY = (side == 0) ? 1.0f : -1.0f;

        for (int iz = 0; iz <= gridZ; ++iz) {
            float vz = static_cast<float>(iz) / gridZ;
            float z = (0.45f - vz) * 2.4f;
            float span = std::pow(std::sin(vz * PI), 0.75f) * 2.15f;

            for (int ix = 0; ix <= gridX; ++ix) {
                float vx = static_cast<float>(ix) / gridX * 2.0f - 1.0f;
                float x = vx * span;
                float sweep = -std::abs(vx) * 0.45f;
                float bodyThick = std::exp(-vx * vx * 4.5f) * std::sin(vz * PI) * 0.20f + 0.015f;

                QVector3D pos(x, signY * bodyThick, z + sweep);
                QVector3D norm(-vx * 0.25f, signY, 0.0f);
                norm.normalize();

                QVector3D col = (side == 0)
                                    ? ProcMath::mix(mantaBack, mantaWingTip, std::abs(vx) * 0.85f)
                                    : ProcMath::mix(mantaBelly, mantaWingTip, std::abs(vx) * 0.55f);

                data.mantaMeshVertices.push_back({pos, norm, col});
            }
        }

        for (int iz = 0; iz < gridZ; ++iz) {
            for (int ix = 0; ix < gridX; ++ix) {
                uint32_t i0 = layerBase + iz * (gridX + 1) + ix;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = i0 + (gridX + 1);
                uint32_t i3 = i2 + 1;
                if (side == 0) {
                    data.mantaMeshIndices.insert(data.mantaMeshIndices.end(), {i0, i2, i1, i1, i2, i3});
                } else {
                    data.mantaMeshIndices.insert(data.mantaMeshIndices.end(), {i0, i1, i2, i1, i3, i2});
                }
            }
        }
    }
}

// Процедурная 3D-модель горбатого кита (туловище, горловые складки, грудные плавники и широкие хвостовые лопасти)
void WorldGenerator::generateWhaleMesh(WorldData& data) {
    const int rings = 26;
    const int radial = 16;

    QVector3D whaleDark(0.08f, 0.20f, 0.30f);
    QVector3D whaleSlate(0.15f, 0.34f, 0.46f);
    QVector3D whaleBelly(0.58f, 0.78f, 0.84f);

    // 1. Веретенообразное тело кита (от рострума z = +4.2 до хвостового стебля z = -4.2)
    for (int ir = 0; ir <= rings; ++ir) {
        float t = static_cast<float>(ir) / rings;
        float z = 4.2f - t * 8.4f;

        // Анатомический профиль горбатого кита
        float env = std::sin(std::pow(t, 0.62f) * PI);
        float rx = 0.92f * env + 0.05f;
        float ryUp = 0.78f * env + 0.05f;
        // Выпуклое горло и брюшко в передней трети тела
        float throatBulge = std::exp(-std::pow((t - 0.26f) / 0.16f, 2.0f)) * 0.38f;
        float ryDown = (0.75f * env + throatBulge + 0.05f);

        // Спинной горб и плавник в районе t = 0.66
        float dorsalHump = std::exp(-std::pow((t - 0.66f) / 0.045f, 2.0f)) * 0.36f;

        for (int ia = 0; ia < radial; ++ia) {
            float ang = (static_cast<float>(ia) / radial) * 2.0f * PI;
            float cx = std::cos(ang);
            float sy = std::sin(ang);

            float ry = (sy >= 0.0f) ? ryUp : ryDown;
            float y = sy * ry;
            if (sy > 0.7f && std::abs(cx) < 0.35f) {
                y += dorsalHump * (sy - 0.7f) / 0.3f;
            }

            QVector3D pos(cx * rx, y, z);
            QVector3D norm(cx / rx, sy / ry, 0.12f * (0.4f - t));
            norm.normalize();

            // Окраска: темная спина и светлое полосатое брюшко (вентральные складки)
            QVector3D col = whaleDark;
            if (sy < -0.15f && t < 0.62f) {
                float grooves = 0.5f + 0.5f * std::sin(cx * 28.0f);
                col = ProcMath::mix(whaleSlate, whaleBelly, (-sy - 0.15f) * 1.15f * (0.75f + 0.25f * grooves));
            } else {
                col = ProcMath::mix(whaleSlate, whaleDark, ProcMath::clamp(sy * 0.6f + 0.4f, 0.0f, 1.0f));
            }

            data.whaleMeshVertices.push_back({pos, norm, col});
        }
    }

    for (int ir = 0; ir < rings; ++ir) {
        for (int ia = 0; ia < radial; ++ia) {
            uint32_t i0 = ir * radial + ia;
            uint32_t i1 = ir * radial + ((ia + 1) % radial);
            uint32_t i2 = (ir + 1) * radial + ia;
            uint32_t i3 = (ir + 1) * radial + ((ia + 1) % radial);
            data.whaleMeshIndices.insert(data.whaleMeshIndices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }

    // 2. Широкие горизонтальные хвостовые лопасти (Whale Fluke) от z = -3.8 до z = -5.4
    const int flukeX = 16;
    const int flukeZ = 8;
    for (int layer = 0; layer < 2; ++layer) {
        uint32_t baseIdx = static_cast<uint32_t>(data.whaleMeshVertices.size());
        float signY = (layer == 0) ? 1.0f : -1.0f;

        for (int iz = 0; iz <= flukeZ; ++iz) {
            float vz = static_cast<float>(iz) / flukeZ; // 0 = передняя кромка, 1 = задняя кромка с выемкой
            for (int ix = 0; ix <= flukeX; ++ix) {
                float vx = static_cast<float>(ix) / flukeX * 2.0f - 1.0f; // [-1..1]
                float absX = std::abs(vx);

                float span = 2.35f;
                float x = vx * span * std::pow(std::sin(vz * PI * 0.65f + 0.25f), 0.4f);

                // Стреловидная передняя кромка и характерная V-образная выемка в центре задней кромки хвоста
                float zLead  = -3.75f - absX * 0.75f;
                float notch  = std::exp(-absX * 9.0f) * 0.28f;
                float zTrail = -5.15f - absX * 0.35f + notch;
                float z = ProcMath::mix(zLead, zTrail, vz);

                float thick = std::sin(vz * PI) * (1.0f - absX * 0.72f) * 0.09f + 0.012f;
                QVector3D pos(x, signY * thick, z);
                QVector3D norm(-vx * 0.15f, signY, 0.0f);
                norm.normalize();

                // Нижняя сторона хвостового плавника горбатого кита имеет светлый узор
                QVector3D col = (layer == 0)
                                    ? ProcMath::mix(whaleDark, whaleSlate, absX * 0.5f)
                                    : ProcMath::mix(whaleBelly, whaleSlate, vz * 0.45f);

                data.whaleMeshVertices.push_back({pos, norm, col});
            }
        }

        for (int iz = 0; iz < flukeZ; ++iz) {
            for (int ix = 0; ix < flukeX; ++ix) {
                uint32_t i0 = baseIdx + iz * (flukeX + 1) + ix;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = i0 + (flukeX + 1);
                uint32_t i3 = i2 + 1;
                if (layer == 0) {
                    data.whaleMeshIndices.insert(data.whaleMeshIndices.end(), {i0, i2, i1, i1, i2, i3});
                } else {
                    data.whaleMeshIndices.insert(data.whaleMeshIndices.end(), {i0, i1, i2, i1, i3, i2});
                }
            }
        }
    }

    // 3. Длинные крыловидные грудные плавники (левый и правый)
    const int finU = 10;
    const int finV = 6;
    for (int side = -1; side <= 1; side += 2) {
        for (int layer = 0; layer < 2; ++layer) {
            uint32_t baseIdx = static_cast<uint32_t>(data.whaleMeshVertices.size());
            float signY = (layer == 0) ? 1.0f : -1.0f;

            for (int iu = 0; iu <= finU; ++iu) {
                float u = static_cast<float>(iu) / finU; // вдоль длины плавника (0 = корень, 1 = кончик)
                float chord = std::sin(std::pow(u, 0.6f) * PI) * 0.65f + 0.08f;

                for (int iv = 0; iv <= finV; ++iv) {
                    float v = static_cast<float>(iv) / finV; // от передней к задней кромке
                    float x = side * (0.72f + u * 2.45f);
                    float y = -0.32f - u * 0.72f + signY * std::sin(v * PI) * 0.06f * (1.0f - u * 0.6f);
                    float z = 1.45f - u * 1.15f - v * chord;

                    QVector3D norm(0.0f, signY, 0.15f * (0.5f - v));
                    norm.normalize();
                    QVector3D col = (layer == 0 && v > 0.25f) ? whaleSlate : whaleBelly;
                    data.whaleMeshVertices.push_back({QVector3D(x, y, z), norm, col});
                }
            }

            for (int iu = 0; iu < finU; ++iu) {
                for (int iv = 0; iv < finV; ++iv) {
                    uint32_t i0 = baseIdx + iu * (finV + 1) + iv;
                    uint32_t i1 = i0 + 1;
                    uint32_t i2 = i0 + (finV + 1);
                    uint32_t i3 = i2 + 1;
                    bool flip = (side < 0) ^ (layer == 1);
                    if (!flip) {
                        data.whaleMeshIndices.insert(data.whaleMeshIndices.end(), {i0, i2, i1, i1, i2, i3});
                    } else {
                        data.whaleMeshIndices.insert(data.whaleMeshIndices.end(), {i0, i1, i2, i1, i3, i2});
                    }
                }
            }
        }
    }
}

void WorldGenerator::generateFaunaInstances(WorldData& data, uint32_t seed) {
    std::mt19937 rng(seed + 303u);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    for (int s = 0; s < 6; ++s) {
        QVector3D schoolCenter(
            (dist01(rng) - 0.5f) * 75.0f,
            -11.0f + dist01(rng) * 15.0f,
            (dist01(rng) - 0.5f) * 75.0f
            );
        float baseR = 7.0f + dist01(rng) * 8.0f;
        float dirSign = (s % 2 == 0) ? 1.0f : -1.0f;

        for (int f = 0; f < 65; ++f) {
            float radius = baseR + (dist01(rng) - 0.5f) * 4.5f;
            float angle = dist01(rng) * 2.0f * PI;
            float angSpeed = dirSign * (0.22f + dist01(rng) * 0.08f);
            float height = schoolCenter.y() + (dist01(rng) - 0.5f) * 3.8f;
            float scale = 0.42f + dist01(rng) * 0.25f;

            FaunaInstance inst;
            inst.orbitCenter = schoolCenter;
            inst.orbitRadius = radius;
            inst.orbitAngle = angle;
            inst.angularSpeed = angSpeed;
            inst.baseHeight = height;

            QVector3D pos(
                schoolCenter.x() + std::cos(angle) * radius,
                height,
                schoolCenter.z() + std::sin(angle) * radius
                );
            float yaw = (angSpeed > 0.0f) ? (-angle) : (PI - angle);
            inst.posScale = QVector4D(pos, scale);
            inst.rotPhase = QVector4D(yaw, dist01(rng) * 6.28f, 0.0f, -dirSign * 0.12f);
            data.fishInstances.push_back(inst);
        }
    }

    for (int m = 0; m < 6; ++m) {
        QVector3D center(
            (dist01(rng) - 0.5f) * 60.0f,
            1.0f + dist01(rng) * 11.0f,
            (dist01(rng) - 0.5f) * 60.0f
            );
        float radius = 16.0f + dist01(rng) * 12.0f;
        float angle = dist01(rng) * 2.0f * PI;
        float dirSign = (m % 2 == 0) ? 1.0f : -1.0f;
        float angSpeed = dirSign * (0.09f + dist01(rng) * 0.04f);

        FaunaInstance inst;
        inst.orbitCenter = center;
        inst.orbitRadius = radius;
        inst.orbitAngle = angle;
        inst.angularSpeed = angSpeed;
        inst.baseHeight = center.y();

        QVector3D pos(
            center.x() + std::cos(angle) * radius,
            center.y(),
            center.z() + std::sin(angle) * radius
            );
        float yaw = (angSpeed > 0.0f) ? (-angle) : (PI - angle);
        inst.posScale = QVector4D(pos, 1.85f + dist01(rng) * 0.65f);
        inst.rotPhase = QVector4D(yaw, dist01(rng) * 6.28f, 1.0f, -dirSign * 0.18f);
        data.mantaInstances.push_back(inst);
    }

    // Инициализируем гигантского кита (масштаб 2.6x -> ~25 метров в длину)
    FaunaInstance whale;
    whale.orbitCenter = QVector3D(0.0f, 11.5f, 0.0f);
    whale.orbitRadius = 90.0f;
    whale.orbitAngle  = 0.0f;
    whale.angularSpeed = 0.035f;
    whale.baseHeight  = 11.5f;
    whale.posScale    = QVector4D(-65.0f, 11.5f, -25.0f, 2.65f);
    whale.rotPhase    = QVector4D(1.25f, 0.0f, 2.0f, 0.0f); // z = 2.0 (Whale)
    data.whaleInstances.push_back(whale);
}

void WorldGenerator::generateGodRaysAndParticles(WorldData& data, uint32_t seed) {
    std::mt19937 rng(seed + 404u);
    std::uniform_real_distribution<float> distPos(-75.0f, 75.0f);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    QVector3D sunDir = QVector3D(-0.28f, 0.92f, -0.22f).normalized();
    const int rayCount = 36;
    for (int i = 0; i < rayCount; ++i) {
        QVector3D topPos(distPos(rng), 24.0f, distPos(rng));
        QVector3D botPos = topPos - sunDir * 48.0f;
        float width = 2.8f + dist01(rng) * 4.5f;
        float alpha = 0.038f + dist01(rng) * 0.038f;
        QVector4D rayCol(0.42f, 0.95f, 0.98f, alpha);

        QVector3D dirs[2] = {QVector3D(1, 0, 0), QVector3D(0, 0, 1)};
        for (int d = 0; d < 2; ++d) {
            QVector3D side = dirs[d] * width;
            RayVertex v0{botPos - side * 1.4f, rayCol, QVector2D(0.0f, 0.0f)};
            RayVertex v1{botPos + side * 1.4f, rayCol, QVector2D(1.0f, 0.0f)};
            RayVertex v2{topPos + side * 0.6f, rayCol, QVector2D(1.0f, 1.0f)};
            RayVertex v3{topPos - side * 0.6f, rayCol, QVector2D(0.0f, 1.0f)};

            data.godRayVertices.insert(data.godRayVertices.end(), {v0, v1, v2, v0, v2, v3});
        }
    }

    const int particleCount = 1800;
    data.particles.reserve(particleCount);
    for (int i = 0; i < particleCount; ++i) {
        bool isBubble = (i < 450);
        Particle p;
        p.pos = QVector3D(distPos(rng), -22.0f + dist01(rng) * 42.0f, distPos(rng));
        p.phase = dist01(rng) * 6.28f;

        if (isBubble) {
            float sz = -(0.35f + dist01(rng) * 0.65f);
            p.colorSize = QVector4D(0.65f, 0.96f, 1.0f, sz);
            p.speedY = 1.6f + dist01(rng) * 1.8f;
        } else {
            float sz = 0.22f + dist01(rng) * 0.35f;
            QVector3D col = (dist01(rng) > 0.3f)
                                ? QVector3D(0.45f, 1.0f, 0.68f)
                                : QVector3D(0.85f, 0.55f, 1.0f);
            p.colorSize = QVector4D(col.x(), col.y(), col.z(), sz);
            p.speedY = 0.15f + dist01(rng) * 0.25f;
        }
        data.particles.push_back(p);
    }
}
