#include "WorldGenerator.h"
#include "Noise.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

inline int worldToChunkCoord(float v, float halfSize, float chunkSize, int chunksPerSide) {
    int c = static_cast<int>(std::floor((v + halfSize) / chunkSize));
    return std::clamp(c, 0, chunksPerSide - 1);
}

// Компактный дедупликатор ID коллайдеров на стеке (исключает 2x-4x всплески сил на границах чанков)
struct SmallIdSet {
    static constexpr size_t INLINE_CAP = 32;
    uint32_t inlineBuf[INLINE_CAP];
    size_t inlineCount = 0;
    std::vector<uint32_t> overflow;

    bool insert(uint32_t id) {
        for (size_t i = 0; i < inlineCount; ++i) {
            if (inlineBuf[i] == id) return false;
        }
        for (uint32_t v : overflow) {
            if (v == id) return false;
        }
        if (inlineCount < INLINE_CAP) {
            inlineBuf[inlineCount++] = id;
        } else {
            overflow.push_back(id);
        }
        return true;
    }
};

// Согласованная с InstPropVert ориентация: направляет локальную ось +Z вдоль единичного вектора dir
inline void alignForwardAxisToDir(const QVector3D& dir, float& outYaw, float& outPitch) {
    QVector3D d = dir.normalized();
    outPitch = std::asin(ProcMath::clamp(d.y(), -1.0f, 1.0f));
    float hLen = std::sqrt(d.x() * d.x() + d.z() * d.z());
    outYaw = (hLen > 1e-4f) ? std::atan2(d.x(), d.z()) : 0.0f;
}

void computeClosestPointOnOBB(const QVector3D& p, const OBBCollider& obb,
                              QVector3D& outClosest, QVector3D& outPushNormal,
                              float& outPenetration, float radius) {
    QVector3D d = p - obb.center;
    float dx = QVector3D::dotProduct(d, obb.axisX);
    float dy = QVector3D::dotProduct(d, obb.axisY);
    float dz = QVector3D::dotProduct(d, obb.axisZ);

    float cx = ProcMath::clamp(dx, -obb.halfExtents.x(), obb.halfExtents.x());
    float cy = ProcMath::clamp(dy, -obb.halfExtents.y(), obb.halfExtents.y());
    float cz = ProcMath::clamp(dz, -obb.halfExtents.z(), obb.halfExtents.z());

    outClosest = obb.center + obb.axisX * cx + obb.axisY * cy + obb.axisZ * cz;
    QVector3D diff = p - outClosest;
    float distSq = diff.lengthSquared();

    if (distSq >= radius * radius) {
        outPenetration = 0.0f;
        return;
    }

    if (distSq > 1e-6f) {
        float dist = std::sqrt(distSq);
        outPushNormal = diff / dist;
        outPenetration = radius - dist;
    } else {
        float px = obb.halfExtents.x() - std::abs(dx);
        float py = obb.halfExtents.y() - std::abs(dy);
        float pz = obb.halfExtents.z() - std::abs(dz);
        if (py <= px && py <= pz) {
            outPushNormal = obb.axisY * ((dy >= 0.0f) ? 1.0f : -1.0f);
            outPenetration = py + radius;
        } else if (px <= pz) {
            outPushNormal = obb.axisX * ((dx >= 0.0f) ? 1.0f : -1.0f);
            outPenetration = px + radius;
        } else {
            outPushNormal = obb.axisZ * ((dz >= 0.0f) ? 1.0f : -1.0f);
            outPenetration = pz + radius;
        }
    }
}

// Деревянная балка между двумя точками p0 -> p1 (для затонувших кораблей)
void appendTimberSegment(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                         const QVector3D& p0, const QVector3D& p1,
                         float widthA, float widthB,
                         const QVector3D& refAxis,
                         const QVector3D& color, float matType = 1.0f) {
    QVector3D axis = p1 - p0;
    float len = axis.length();
    if (len < 1e-4f) return;
    QVector3D dir = axis / len;

    QVector3D side1 = QVector3D::crossProduct(dir, refAxis);
    if (side1.lengthSquared() < 1e-4f) {
        side1 = QVector3D::crossProduct(dir, QVector3D(0.0f, 1.0f, 0.0f));
        if (side1.lengthSquared() < 1e-4f) {
            side1 = QVector3D::crossProduct(dir, QVector3D(1.0f, 0.0f, 0.0f));
        }
    }
    side1.normalize();
    QVector3D side2 = QVector3D::crossProduct(dir, side1).normalized();

    QVector3D h1 = side1 * (widthA * 0.5f);
    QVector3D h2 = side2 * (widthB * 0.5f);

    QVector3D c0 = p0 - h1 - h2;
    QVector3D c1 = p0 + h1 - h2;
    QVector3D c2 = p0 + h1 + h2;
    QVector3D c3 = p0 - h1 + h2;
    QVector3D c4 = p1 - h1 - h2;
    QVector3D c5 = p1 + h1 - h2;
    QVector3D c6 = p1 + h1 + h2;
    QVector3D c7 = p1 - h1 + h2;

    auto addQuad = [&](const QVector3D& a, const QVector3D& b, const QVector3D& c, const QVector3D& d, const QVector3D& n) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        verts.push_back({a, n, color, QVector2D(matType, 0.0f)});
        verts.push_back({b, n, color, QVector2D(matType, 0.0f)});
        verts.push_back({c, n, color, QVector2D(matType, 0.0f)});
        verts.push_back({d, n, color, QVector2D(matType, 0.0f)});
        inds.insert(inds.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };

    addQuad(c0, c1, c5, c4, -side2);
    addQuad(c2, c3, c7, c6,  side2);
    addQuad(c1, c2, c6, c5,  side1);
    addQuad(c3, c0, c4, c7, -side1);
    addQuad(c3, c2, c1, c0, -dir);
    addQuad(c4, c5, c6, c7,  dir);
}

// Обшивочная доска корпуса (4 вершины с толщиной и гарантированным внешним CCW-обходом)
void appendPlankStrip(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                      const QVector3D& a0, const QVector3D& a1,
                      const QVector3D& b0, const QVector3D& b1,
                      const QVector3D& outNormal, float thickness,
                      const QVector3D& color, float matType = 1.0f) {
    QVector3D n = (outNormal.lengthSquared() > 1e-6f) ? outNormal.normalized() : QVector3D(0.0f, 1.0f, 0.0f);
    QVector3D off = n * thickness;

    auto addQuad = [&](const QVector3D& p0, const QVector3D& p1, const QVector3D& p2, const QVector3D& p3,
                       const QVector3D& qn, const QVector3D& col) {
        QVector3D faceN = QVector3D::crossProduct(p1 - p0, p2 - p0)
        + QVector3D::crossProduct(p2 - p0, p3 - p0);
        const bool flip = QVector3D::dotProduct(faceN, qn) < 0.0f;

        uint32_t base = static_cast<uint32_t>(verts.size());
        verts.push_back({p0, qn, col, QVector2D(matType, 0.0f)});
        verts.push_back({p1, qn, col, QVector2D(matType, 0.0f)});
        verts.push_back({p2, qn, col, QVector2D(matType, 0.0f)});
        verts.push_back({p3, qn, col, QVector2D(matType, 0.0f)});
        if (!flip) {
            inds.insert(inds.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        } else {
            inds.insert(inds.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        }
    };

    addQuad(a0 + off, b0 + off, b1 + off, a1 + off,  n, color);
    addQuad(b0, a0, a1, b1, -n, color * 0.78f);

    QVector3D upEdge = QVector3D::crossProduct(b1 - a1, n);
    QVector3D edgeDir = (a1 - a0) + (b1 - b0);
    if (upEdge.lengthSquared() < 1e-6f) {
        upEdge = (edgeDir.lengthSquared() > 1e-6f) ? edgeDir.normalized() : QVector3D(0.0f, 1.0f, 0.0f);
    } else {
        upEdge.normalize();
        if (QVector3D::dotProduct(upEdge, edgeDir) < 0.0f) {
            upEdge = -upEdge;
        }
    }
    addQuad(a1, b1, b1 + off, a1 + off,  upEdge, color * 0.85f);
    addQuad(b0, a0, a0 + off, b0 + off, -upEdge, color * 0.85f);
}

// Сшивка шва и сглаживание полюсов для сферических каменных прототипов
void computeSphereRockNormals(const std::vector<QVector3D>& pts, int stacks, int slices,
                              std::vector<QVector3D>& accumN) {
    accumN.assign(pts.size(), QVector3D(0.0f, 0.0f, 0.0f));
    const int rowStride = slices + 1;

    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            int i00 = st * rowStride + sl;
            int i10 = i00 + 1;
            int i01 = i00 + rowStride;
            int i11 = i01 + 1;

            if (st > 0) {
                QVector3D fn1 = QVector3D::crossProduct(pts[i10] - pts[i00], pts[i01] - pts[i00]);
                accumN[i00] += fn1;
                accumN[i10] += fn1;
                accumN[i01] += fn1;
            }
            if (st + 1 < stacks) {
                QVector3D fn2 = QVector3D::crossProduct(pts[i11] - pts[i10], pts[i01] - pts[i10]);
                accumN[i10] += fn2;
                accumN[i11] += fn2;
                accumN[i01] += fn2;
            }
        }
    }

    // Сшивка меридиана sl == 0 и sl == slices
    for (int st = 1; st < stacks; ++st) {
        int a = st * rowStride;
        int b = a + slices;
        QVector3D merged = accumN[a] + accumN[b];
        accumN[a] = accumN[b] = merged;
    }

    // Усреднение нормалей на северном и южном полюсах
    QVector3D northSum(0.0f, 0.0f, 0.0f);
    QVector3D southSum(0.0f, 0.0f, 0.0f);
    const int southBase = stacks * rowStride;
    for (int sl = 0; sl <= slices; ++sl) {
        northSum += accumN[sl];
        southSum += accumN[southBase + sl];
    }
    for (int sl = 0; sl <= slices; ++sl) {
        accumN[sl]             = northSum;
        accumN[southBase + sl] = southSum;
    }
}

// ============================================================================
// ПРОТОТИП 0: ГРАНЁНЫЙ СЛОИСТЫЙ ВАЛУН (PROP_BOULDER_CHUNK)
// ============================================================================

void buildBoulderPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                           int stacks, int slices, uint32_t rockSeed) {
    constexpr int PLANE_COUNT = 10;
    QVector3D cutNormals[PLANE_COUNT];
    float cutOffsets[PLANE_COUNT];
    for (int k = 0; k < PLANE_COUNT; ++k) {
        float a = ProcMath::hash3(k, 1, 7, rockSeed + 911u) * TWO_PI;
        float z = ProcMath::hash3(k, 2, 11, rockSeed + 911u) * 2.0f - 1.0f;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        cutNormals[k] = QVector3D(r * std::cos(a), z, r * std::sin(a)).normalized();
        cutOffsets[k] = 0.78f + ProcMath::hash3(k, 3, 13, rockSeed + 911u) * 0.24f;
    }

    const int rowStride = slices + 1;
    std::vector<QVector3D> pts((stacks + 1) * rowStride);
    std::vector<float> cavity((stacks + 1) * rowStride, 1.0f);

    for (int st = 0; st <= stacks; ++st) {
        float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int sl = 0; sl <= slices; ++sl) {
            int slWrap = sl % slices;
            float theta = TWO_PI * static_cast<float>(slWrap) / static_cast<float>(slices);
            QVector3D dir(sp * std::cos(theta), cp, sp * std::sin(theta));

            float planeR = 1.28f;
            for (int k = 0; k < PLANE_COUNT; ++k) {
                float proj = QVector3D::dotProduct(dir, cutNormals[k]);
                if (proj > 0.15f) {
                    float d = cutOffsets[k] / proj;
                    float h = ProcMath::clamp(0.5f + 0.5f * (d - planeR) / 0.12f, 0.0f, 1.0f);
                    planeR = ProcMath::mix(d, planeR, h) - 0.12f * h * (1.0f - h);
                }
            }

            float macro     = ProcMath::fbm(dir.x() * 1.85f, dir.y() * 1.85f, dir.z() * 1.85f, 3, rockSeed);
            float ridged    = ProcMath::ridgedFbm(dir.x() * 2.8f + 1.3f, dir.y() * 2.8f, dir.z() * 2.8f - 0.9f, 3, rockSeed + 41u);
            float cracks    = ProcMath::voronoiCracks2D(dir.x() * 2.9f + dir.y() * 1.2f, dir.z() * 2.9f - dir.y() * 0.9f, rockSeed + 83u);
            float crackMask = ProcMath::smoothstep(0.03f, 0.24f, cracks);
            float strata    = std::sin(dir.y() * 11.0f + macro * 2.4f) * 0.045f;

            float r = planeR * (0.86f + macro * 0.14f + ridged * 0.16f + strata) - (1.0f - crackMask) * 0.12f;
            float yMod = (dir.y() < -0.35f) ? (0.80f + 0.20f * (dir.y() + 1.0f)) : 1.0f;
            pts[st * rowStride + sl] = QVector3D(dir.x() * r, dir.y() * r * yMod, dir.z() * r);
            cavity[st * rowStride + sl] = 0.58f + 0.42f * crackMask;
        }
    }

    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    std::vector<QVector3D> accumN;
    computeSphereRockNormals(pts, stacks, slices, accumN);

    for (int st = 0; st <= stacks; ++st) {
        for (int sl = 0; sl <= slices; ++sl) {
            int id = st * rowStride + sl;
            QVector3D p = pts[id];
            QVector3D n = (accumN[id].lengthSquared() > 1e-6f) ? accumN[id].normalized() : p.normalized();

            float moss = ProcMath::smoothstep(0.42f, 0.84f, n.y());
            float shade = ProcMath::clamp((0.72f + (p.length() - 0.72f) * 0.65f) * cavity[id], 0.42f, 1.08f);
            QVector3D stoneCol = QVector3D(0.66f, 0.71f, 0.75f) * shade;
            QVector3D mossCol  = QVector3D(0.42f, 0.72f, 0.54f) * shade;
            QVector3D col = ProcMath::mix(stoneCol, mossCol, moss * 0.48f);
            verts.push_back({p, n, col, QVector2D(0.0f, 0.0f)});
        }
    }

    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            uint32_t i00 = baseIdx + st * rowStride + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + rowStride;
            uint32_t i11 = i01 + 1;
            if (st > 0)          inds.insert(inds.end(), {i00, i10, i01});
            if (st + 1 < stacks) inds.insert(inds.end(), {i10, i11, i01});
        }
    }
}

// ============================================================================
// ПРОТОТИП 1: РАСКОЛОТАЯ СКАЛЬНАЯ ПЛИТА / УСТУП ДЛЯ ОБРЫВОВ (PROP_CLIFF_CRAG)
// ============================================================================

void buildCliffCragPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                             int stacks, int slices, uint32_t cragSeed) {
    constexpr int PLANE_COUNT = 8;
    QVector3D cutNormals[PLANE_COUNT];
    float cutOffsets[PLANE_COUNT];
    for (int k = 0; k < PLANE_COUNT; ++k) {
        float a = ProcMath::hash3(k, 5, 19, cragSeed + 701u) * TWO_PI;
        float z = ProcMath::hash3(k, 6, 23, cragSeed + 701u) * 1.6f - 0.8f;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        cutNormals[k] = QVector3D(r * std::cos(a), z, r * std::sin(a)).normalized();
        cutOffsets[k] = 0.82f + ProcMath::hash3(k, 7, 29, cragSeed + 701u) * 0.22f;
    }

    const int rowStride = slices + 1;
    std::vector<QVector3D> pts((stacks + 1) * rowStride);
    std::vector<float> cavity((stacks + 1) * rowStride, 1.0f);

    for (int st = 0; st <= stacks; ++st) {
        float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int sl = 0; sl <= slices; ++sl) {
            int slWrap = sl % slices;
            float theta = TWO_PI * static_cast<float>(slWrap) / static_cast<float>(slices);
            QVector3D dir(sp * std::cos(theta), cp, sp * std::sin(theta));

            float planeR = 1.25f;
            for (int k = 0; k < PLANE_COUNT; ++k) {
                float proj = QVector3D::dotProduct(dir, cutNormals[k]);
                if (proj > 0.18f) {
                    float d = cutOffsets[k] / proj;
                    float h = ProcMath::clamp(0.5f + 0.5f * (d - planeR) / 0.10f, 0.0f, 1.0f);
                    planeR = ProcMath::mix(d, planeR, h) - 0.10f * h * (1.0f - h);
                }
            }

            float crag = ProcMath::ridgedFbm(dir.x() * 2.4f + 3.1f, dir.y() * 3.1f, dir.z() * 2.3f, 3, cragSeed);
            float strata = std::sin(dir.y() * 12.5f + crag * 2.6f) * 0.085f
                           + std::sin(dir.y() * 25.0f - dir.x() * 2.0f) * 0.030f;
            float crack = ProcMath::voronoiCracks2D(dir.x() * 3.2f + dir.y(), dir.z() * 3.2f - dir.y(), cragSeed + 127u);
            float crackMask = ProcMath::smoothstep(0.03f, 0.22f, crack);

            float sx = 1.32f;
            float sy = 0.72f;
            float sz = (dir.z() >= 0.0f) ? 1.28f : 0.90f;
            float r = planeR * (0.84f + crag * 0.22f + strata) - (1.0f - crackMask) * 0.11f;

            float x = dir.x() * sx * r;
            float y = dir.y() * sy * r;
            float z = dir.z() * sz * r + 0.16f;

            if (z > 0.15f && y > 0.05f) {
                y *= 0.84f;
                z += 0.18f * ProcMath::smoothstep(0.05f, 0.75f, dir.y());
            }
            pts[st * rowStride + sl] = QVector3D(x, y, z);
            cavity[st * rowStride + sl] = 0.55f + 0.45f * crackMask;
        }
    }

    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    std::vector<QVector3D> accumN;
    computeSphereRockNormals(pts, stacks, slices, accumN);

    for (int st = 0; st <= stacks; ++st) {
        for (int sl = 0; sl <= slices; ++sl) {
            int id = st * rowStride + sl;
            QVector3D p = pts[id];
            QVector3D n = (accumN[id].lengthSquared() > 1e-6f) ? accumN[id].normalized() : p.normalized();

            float topShelf  = ProcMath::smoothstep(0.35f, 0.80f, n.y());
            float underDark = ProcMath::smoothstep(-0.12f, -0.72f, n.y());
            QVector3D baseRock(0.56f, 0.64f, 0.69f);
            QVector3D mossTop(0.44f, 0.72f, 0.54f);
            QVector3D col = ProcMath::mix(baseRock, mossTop, topShelf * 0.58f);
            col *= (1.0f - underDark * 0.36f) * cavity[id];
            verts.push_back({p, n, col, QVector2D(0.0f, 0.0f)});
        }
    }

    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            uint32_t i00 = baseIdx + st * rowStride + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + rowStride;
            uint32_t i11 = i01 + 1;
            if (st > 0)          inds.insert(inds.end(), {i00, i10, i01});
            if (st + 1 < stacks) inds.insert(inds.end(), {i10, i11, i01});
        }
    }
}
} // namespace

// ============================================================================
// COLLISION WORLD (O(1) пространственная сетка + скольжение по нормалям)
// ============================================================================

float CollisionWorld::sampleHeight(float x, float z) const {
    if (heightField.empty() || heightGridSize < 2) {
        return ProcMath::sampleTerrainHeight(x, z, seed);
    }
    const float fx = (x + worldHalfSize) / heightCellSize;
    const float fz = (z + worldHalfSize) / heightCellSize;
    const int maxIdx = heightGridSize - 2;

    const int ix = std::clamp(static_cast<int>(std::floor(fx)), 0, maxIdx);
    const int iz = std::clamp(static_cast<int>(std::floor(fz)), 0, maxIdx);
    const float tx = ProcMath::clamp(fx - static_cast<float>(ix), 0.0f, 1.0f);
    const float tz = ProcMath::clamp(fz - static_cast<float>(iz), 0.0f, 1.0f);

    const size_t row0 = static_cast<size_t>(iz) * heightGridSize + ix;
    const size_t row1 = row0 + heightGridSize;

    const float h00 = heightField[row0];
    const float h10 = heightField[row0 + 1];
    const float h01 = heightField[row1];
    const float h11 = heightField[row1 + 1];

    return ProcMath::mix(ProcMath::mix(h00, h10, tx), ProcMath::mix(h01, h11, tx), tz);
}

QVector3D CollisionWorld::sampleNormal(float x, float z) const {
    if (heightField.empty() || heightGridSize < 2) {
        return ProcMath::sampleTerrainNormal(x, z, seed);
    }
    const float eps = heightCellSize;
    const float hL = sampleHeight(x - eps, z);
    const float hR = sampleHeight(x + eps, z);
    const float hD = sampleHeight(x, z - eps);
    const float hU = sampleHeight(x, z + eps);
    return QVector3D(hL - hR, 2.0f * eps, hD - hU).normalized();
}

void CollisionWorld::buildGrid() {
    const int totalCells = chunksPerSide * chunksPerSide;
    cellSpheres.assign(totalCells, {});
    cellObbs.assign(totalCells, {});

    for (uint32_t i = 0; i < spheres.size(); ++i) {
        const auto& s = spheres[i];
        int minX = worldToChunkCoord(s.center.x() - s.radius, worldHalfSize, chunkSize, chunksPerSide);
        int maxX = worldToChunkCoord(s.center.x() + s.radius, worldHalfSize, chunkSize, chunksPerSide);
        int minZ = worldToChunkCoord(s.center.z() - s.radius, worldHalfSize, chunkSize, chunksPerSide);
        int maxZ = worldToChunkCoord(s.center.z() + s.radius, worldHalfSize, chunkSize, chunksPerSide);
        for (int cz = minZ; cz <= maxZ; ++cz) {
            for (int cx = minX; cx <= maxX; ++cx) {
                cellSpheres[cz * chunksPerSide + cx].push_back(i);
            }
        }
    }

    for (uint32_t i = 0; i < obbs.size(); ++i) {
        const auto& o = obbs[i];
        int minX = worldToChunkCoord(o.center.x() - o.boundingRadius, worldHalfSize, chunkSize, chunksPerSide);
        int maxX = worldToChunkCoord(o.center.x() + o.boundingRadius, worldHalfSize, chunkSize, chunksPerSide);
        int minZ = worldToChunkCoord(o.center.z() - o.boundingRadius, worldHalfSize, chunkSize, chunksPerSide);
        int maxZ = worldToChunkCoord(o.center.z() + o.boundingRadius, worldHalfSize, chunkSize, chunksPerSide);
        for (int cz = minZ; cz <= maxZ; ++cz) {
            for (int cx = minX; cx <= maxX; ++cx) {
                cellObbs[cz * chunksPerSide + cx].push_back(i);
            }
        }
    }
}

QVector3D CollisionWorld::computeAvoidanceForce(const QVector3D& pos, float probeRadius, float margin) const {
    QVector3D force(0.0f, 0.0f, 0.0f);
    const float queryR = probeRadius + margin;
    const float invMargin = 1.0f / std::max(margin, 0.5f);

    if (!cellSpheres.empty() || !cellObbs.empty()) {
        int minX = worldToChunkCoord(pos.x() - queryR, worldHalfSize, chunkSize, chunksPerSide);
        int maxX = worldToChunkCoord(pos.x() + queryR, worldHalfSize, chunkSize, chunksPerSide);
        int minZ = worldToChunkCoord(pos.z() - queryR, worldHalfSize, chunkSize, chunksPerSide);
        int maxZ = worldToChunkCoord(pos.z() + queryR, worldHalfSize, chunkSize, chunksPerSide);
        const bool multiCell = (minX != maxX || minZ != maxZ);

        SmallIdSet seenSpheres;
        SmallIdSet seenObbs;

        for (int cz = minZ; cz <= maxZ; ++cz) {
            for (int cx = minX; cx <= maxX; ++cx) {
                const int cellIdx = cz * chunksPerSide + cx;

                if (!cellSpheres.empty()) {
                    for (uint32_t id : cellSpheres[cellIdx]) {
                        if (multiCell && !seenSpheres.insert(id)) continue;
                        const auto& s = spheres[id];
                        QVector3D diff = pos - s.center;
                        float safeDist = s.radius + queryR;
                        float d2 = diff.lengthSquared();
                        if (d2 < safeDist * safeDist && d2 > 1e-5f) {
                            float d = std::sqrt(d2);
                            float strength = (safeDist - d) * invMargin;
                            force += (diff / d) * (strength * strength * 12.0f);
                        }
                    }
                }

                if (!cellObbs.empty()) {
                    for (uint32_t id : cellObbs[cellIdx]) {
                        if (multiCell && !seenObbs.insert(id)) continue;
                        const auto& o = obbs[id];
                        float broadR = o.boundingRadius + queryR;
                        if ((pos - o.center).lengthSquared() > broadR * broadR) continue;

                        QVector3D closest, pushN;
                        float pen = 0.0f;
                        computeClosestPointOnOBB(pos, o, closest, pushN, pen, queryR);
                        if (pen > 0.0f) {
                            float strength = pen * invMargin;
                            force += pushN * (strength * strength * 16.0f);
                        }
                    }
                }
            }
        }
    }

    float floorY = sampleHeight(pos.x(), pos.z());
    QVector3D terrainN = sampleNormal(pos.x(), pos.z());
    float signedDist = (pos.y() - floorY) * terrainN.y();
    if (signedDist < queryR) {
        float strength = (queryR - signedDist) * invMargin;
        force += terrainN * (strength * strength * 18.0f);
    }

    float wallF = ProcMath::sampleBasinWallFactor(pos.x(), pos.z(), seed);
    if (wallF > 0.08f) {
        QVector3D inward(-pos.x(), 0.0f, -pos.z());
        if (inward.lengthSquared() > 0.01f) {
            inward.normalize();
            force += inward * (wallF * wallF * 45.0f);
        }
    }

    float distToSurf = ProcMath::WATER_SURFACE_Y - pos.y();
    if (distToSurf < queryR) {
        force.setY(force.y() - ((queryR - distToSurf) * invMargin) * 12.0f);
    }

    return force;
}

bool CollisionWorld::resolveSphereCollision(QVector3D& pos, QVector3D* vel, float radius,
                                            float minFloorMargin, float surfaceMargin) const {
    bool collided = false;
    auto slideVelocity = [&](const QVector3D& normal) {
        if (!vel) return;
        float vn = QVector3D::dotProduct(*vel, normal);
        if (vn < 0.0f) {
            *vel -= normal * vn;
        }
    };

    const bool hasGrid = !cellSpheres.empty() || !cellObbs.empty();

    for (int iter = 0; iter < 2; ++iter) {
        float r2 = pos.x() * pos.x() + pos.z() * pos.z();
        float maxR = ProcMath::PLAYABLE_HARD_RADIUS - radius;
        if (r2 > maxR * maxR && r2 > 1e-8f) {
            float r = std::sqrt(r2);
            QVector3D inward(-pos.x() / r, 0.0f, -pos.z() / r);
            pos.setX(-inward.x() * maxR);
            pos.setZ(-inward.z() * maxR);
            slideVelocity(inward);
            collided = true;
        }

        float maxY = ProcMath::WATER_SURFACE_Y - surfaceMargin;
        if (pos.y() > maxY) {
            pos.setY(maxY);
            slideVelocity(QVector3D(0.0f, -1.0f, 0.0f));
            collided = true;
        }

        float effFloorR = radius + minFloorMargin;
        float hCenter = sampleHeight(pos.x(), pos.z());
        QVector3D nCenter = sampleNormal(pos.x(), pos.z());
        float signedDist = (pos.y() - hCenter) * nCenter.y();
        if (signedDist < effFloorR) {
            float push = effFloorR - signedDist;
            pos += nCenter * push;
            slideVelocity(nCenter);
            collided = true;
        }

        const float probeOff = radius * 0.78f;
        const float dirs[4][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
        for (const auto& d : dirs) {
            float px = pos.x() + d[0] * probeOff;
            float pz = pos.z() + d[1] * probeOff;
            float hp = sampleHeight(px, pz);
            if (pos.y() < hp + radius * 0.45f) {
                QVector3D np = sampleNormal(px, pz);
                QVector3D pushDir = (np + QVector3D(-d[0] * 0.6f, 0.15f, -d[1] * 0.6f)).normalized();
                float depthPen = std::min((hp + radius * 0.45f) - pos.y(), radius * 0.85f);
                pos += pushDir * (depthPen * 0.55f);
                slideVelocity(pushDir);
                collided = true;
            }
        }

        float hFinal = sampleHeight(pos.x(), pos.z());
        if (pos.y() < hFinal + effFloorR * 0.72f) {
            pos.setY(hFinal + effFloorR * 0.72f);
            slideVelocity(QVector3D(0.0f, 1.0f, 0.0f));
            collided = true;
        }

        if (!hasGrid) continue;

        int minX = worldToChunkCoord(pos.x() - radius, worldHalfSize, chunkSize, chunksPerSide);
        int maxX = worldToChunkCoord(pos.x() + radius, worldHalfSize, chunkSize, chunksPerSide);
        int minZ = worldToChunkCoord(pos.z() - radius, worldHalfSize, chunkSize, chunksPerSide);
        int maxZ = worldToChunkCoord(pos.z() + radius, worldHalfSize, chunkSize, chunksPerSide);
        const bool multiCell = (minX != maxX || minZ != maxZ);

        SmallIdSet seenSpheres;
        SmallIdSet seenObbs;

        for (int cz = minZ; cz <= maxZ; ++cz) {
            for (int cx = minX; cx <= maxX; ++cx) {
                const int cellIdx = cz * chunksPerSide + cx;

                if (!cellSpheres.empty()) {
                    for (uint32_t id : cellSpheres[cellIdx]) {
                        if (multiCell && !seenSpheres.insert(id)) continue;
                        const auto& s = spheres[id];
                        QVector3D diff = pos - s.center;
                        float minDist = s.radius + radius;
                        float d2 = diff.lengthSquared();
                        if (d2 < minDist * minDist) {
                            float d = std::sqrt(std::max(d2, 1e-8f));
                            QVector3D n = (d > 1e-4f) ? (diff / d) : QVector3D(0.0f, 1.0f, 0.0f);
                            pos = s.center + n * minDist;
                            slideVelocity(n);
                            collided = true;
                        }
                    }
                }

                if (!cellObbs.empty()) {
                    for (uint32_t id : cellObbs[cellIdx]) {
                        if (multiCell && !seenObbs.insert(id)) continue;
                        const auto& o = obbs[id];
                        float broadR = o.boundingRadius + radius;
                        if ((pos - o.center).lengthSquared() > broadR * broadR) continue;

                        QVector3D closest, pushN;
                        float pen = 0.0f;
                        computeClosestPointOnOBB(pos, o, closest, pushN, pen, radius);
                        if (pen > 0.0f) {
                            pos += pushN * pen;
                            slideVelocity(pushN);
                            collided = true;
                        }
                    }
                }
            }
        }
    }
    return collided;
}

bool CollisionWorld::isPointClear(const QVector3D& pos, float radius) const {
    float maxR = ProcMath::PLAYABLE_HARD_RADIUS - radius;
    if (pos.x() * pos.x() + pos.z() * pos.z() > maxR * maxR) return false;
    if (pos.y() > ProcMath::WATER_SURFACE_Y - radius) return false;
    if (pos.y() < sampleHeight(pos.x(), pos.z()) + radius) return false;

    if (cellSpheres.empty() && cellObbs.empty()) return true;

    int minX = worldToChunkCoord(pos.x() - radius, worldHalfSize, chunkSize, chunksPerSide);
    int maxX = worldToChunkCoord(pos.x() + radius, worldHalfSize, chunkSize, chunksPerSide);
    int minZ = worldToChunkCoord(pos.z() - radius, worldHalfSize, chunkSize, chunksPerSide);
    int maxZ = worldToChunkCoord(pos.z() + radius, worldHalfSize, chunkSize, chunksPerSide);
    const bool multiCell = (minX != maxX || minZ != maxZ);

    SmallIdSet seenSpheres;
    SmallIdSet seenObbs;

    for (int cz = minZ; cz <= maxZ; ++cz) {
        for (int cx = minX; cx <= maxX; ++cx) {
            const int cellIdx = cz * chunksPerSide + cx;

            if (!cellSpheres.empty()) {
                for (uint32_t id : cellSpheres[cellIdx]) {
                    if (multiCell && !seenSpheres.insert(id)) continue;
                    const auto& s = spheres[id];
                    float rSum = s.radius + radius;
                    if ((pos - s.center).lengthSquared() < rSum * rSum) return false;
                }
            }
            if (!cellObbs.empty()) {
                for (uint32_t id : cellObbs[cellIdx]) {
                    if (multiCell && !seenObbs.insert(id)) continue;
                    const auto& o = obbs[id];
                    float broadR = o.boundingRadius + radius;
                    if ((pos - o.center).lengthSquared() > broadR * broadR) continue;

                    QVector3D closest, pushN;
                    float pen = 0.0f;
                    computeClosestPointOnOBB(pos, o, closest, pushN, pen, radius);
                    if (pen > 0.0f) return false;
                }
            }
        }
    }
    return true;
}

// ============================================================================
// ТОЧКА ВХОДА ГЕНЕРАТОРА МИРА И ОБЩИЕ МАСКИ ОКРУЖЕНИЯ
// ============================================================================

float WorldGenerator::samplePlateauIslandMask(float wx, float wz, uint32_t seed) {
    float n1 = ProcMath::fbm(wx * 0.030f + 3.1f, 0.4f, wz * 0.030f - 1.7f, 3, seed + 1607u);
    float n2 = ProcMath::fbm(wx * 0.072f - 2.3f, 1.1f, wz * 0.072f + 4.9f, 2, seed + 1621u);
    return ProcMath::smoothstep(0.02f, 0.30f, n1 * 0.78f + n2 * 0.22f);
}

WorldData WorldGenerator::generate(uint32_t seed) {
    WorldData data;
    data.collision.seed = seed;
    data.collision.chunksPerSide = WorldData::CHUNKS_PER_SIDE;
    data.collision.chunkSize = WorldData::CHUNK_SIZE;
    data.collision.worldHalfSize = ProcMath::WORLD_HALF_SIZE;

    generateTerrainChunks(data, seed);
    generateEnvironmentPrototypes(data);
    generateFloraPrototypes(data);
    generateChunkRocks(data, seed);
    generateChunkFlora(data, seed);
    generateShipwrecks(data, seed);

    data.collision.buildGrid();

    generateFaunaMeshes(data);
    generateWhaleMeshes(data);
    generateAnglerMeshes(data);
    generateSeagullMeshes(data);
    generateFaunaAgents(data, seed);
    generateGodRaysAndParticles(data, seed);
    return data;
}

// ============================================================================
// РЕЛЬЕФ ЧАНКОВ (48x48 + ПОСТПРОЦЕССИНГ + АДАПТИВНАЯ ПЕРЕГЕНЕРАЦИЯ + HEIGHTFIELD)
// ============================================================================

void WorldGenerator::generateTerrainChunks(WorldData& data, uint32_t seed) {
    constexpr int C = WorldData::CHUNKS_PER_SIDE;
    constexpr int N = WorldData::CHUNK_GRID_N;     // 48
    constexpr int G = N + 1;                       // 49
    constexpr int HALO = 4;                        // гало-рамка: 100% детерминированные границы без швов
    constexpr int P = G + 2 * HALO;                // 57
    constexpr int TOTAL_CHUNKS = C * C;
    constexpr float CHUNK_SZ = WorldData::CHUNK_SIZE;
    constexpr float CELL_SZ  = CHUNK_SZ / static_cast<float>(N);
    constexpr float HALF_W   = ProcMath::WORLD_HALF_SIZE;
    constexpr float SKIRT_DROP = 14.0f;
    constexpr uint32_t NO_VERT = 0xFFFFFFFFu;

    struct ChunkTerrainTemp {
        std::vector<EnvVertex> verts;
        std::array<std::vector<uint32_t>, WorldData::TERRAIN_LOD_COUNT> lodInds;
    };

    data.chunks.resize(TOTAL_CHUNKS);
    std::vector<ChunkTerrainTemp> chunkTemps(TOTAL_CHUNKS);

    // Точная глобальная координата по целому индексу узла (битовая идентичность на стыках чанков)
    auto gridWorldCoord = [&](int gIdx) -> float {
        return static_cast<float>(-static_cast<double>(HALF_W) +
                                  (static_cast<double>(gIdx) * static_cast<double>(CHUNK_SZ)) / static_cast<double>(N));
    };

    // Детерминированный 3D-скульптинг скальной стены (ломает регулярную сетку, формирует слои и сколы)
    auto evalSculptedPoint = [&](float wx, float wy, float wz, float gx, float gz) -> QVector3D {
        float slopeMag = std::sqrt(gx * gx + gz * gz);
        float steep = ProcMath::smoothstep(0.28f, 1.35f, slopeMag);
        if (steep <= 0.002f || slopeMag < 1e-4f) {
            return QVector3D(wx, wy, wz);
        }

        float outX = -gx / slopeMag;
        float outZ = -gz / slopeMag;
        float tanX = -outZ;
        float tanZ =  outX;

        float macroBlock = ProcMath::fbm(wx * 0.052f, wy * 0.048f, wz * 0.052f, 3, seed + 5113u);
        float crag3D     = ProcMath::fbm(wx * 0.115f, wy * 0.115f, wz * 0.115f, 3, seed + 5231u);
        float crack3D    = ProcMath::voronoiCracks2D(wx * 0.068f + wy * 0.055f, wz * 0.068f - wy * 0.048f, seed + 5309u);
        float fractureIn = (1.0f - ProcMath::smoothstep(0.04f, 0.24f, crack3D)) * -0.48f;
        float tanJitter  = ProcMath::fbm(wx * 0.14f + 9.3f, wy * 0.12f, wz * 0.14f - 4.7f, 2, seed + 5417u);

        // Запас от схлопывания полуячеек LOD0 (шаг 0.5 * CELL_SZ)
        float maxPush    = 0.24f * CELL_SZ;
        float normalPush = ProcMath::clamp((macroBlock * 0.58f + crag3D * 0.68f + fractureIn) * 0.72f, -1.0f, 1.0f);
        float tanPush    = ProcMath::clamp(tanJitter, -0.75f, 0.75f) * 0.36f;

        return QVector3D(
            wx + (outX * normalPush + tanX * tanPush) * maxPush * steep,
            wy + (macroBlock * 0.18f + fractureIn * 0.12f) * steep,
            wz + (outZ * normalPush + tanZ * tanPush) * maxPush * steep
            );
    };

    auto evalTerrainColor = [&](float wx, float wy, float wz, const QVector3D& norm) -> QVector3D {
        float slope     = 1.0f - ProcMath::clamp(norm.y(), 0.0f, 1.0f);
        float wallF     = ProcMath::sampleBasinWallFactor(wx, wz, seed);
        float heightMix = ProcMath::smoothstep(-22.0f, 10.0f, wy);
        float abyssMix  = ProcMath::smoothstep(-26.0f, -50.0f, wy);
        float spireMix  = ProcMath::smoothstep(6.0f, 28.0f, wy) * (1.0f - wallF);

        QVector3D sandCol(0.58f, 0.56f, 0.45f);
        QVector3D mossRock(0.20f, 0.38f, 0.30f);
        QVector3D sharpCrag(0.20f, 0.25f, 0.29f);
        QVector3D cliffBasalt(0.15f, 0.19f, 0.24f);
        QVector3D abyssSlate(0.08f, 0.11f, 0.16f);
        QVector3D grottoTint(0.36f, 0.22f, 0.44f);

        float grottoMask = ProcMath::smoothstep(0.18f, 0.68f,
                                                ProcMath::fbm(wx * 0.006f, 0.0f, wz * 0.006f, 2, seed + 444u) * 0.5f + 0.5f);

        QVector3D col = ProcMath::mix(sandCol, mossRock, ProcMath::clamp(slope * 2.4f + heightMix * 0.52f, 0.0f, 1.0f));
        col = ProcMath::mix(col, sharpCrag, ProcMath::clamp(ProcMath::smoothstep(0.20f, 0.56f, slope) * 0.88f + spireMix * 0.65f, 0.0f, 1.0f));
        col = ProcMath::mix(col, grottoTint, grottoMask * 0.32f * (1.0f - wallF) * (1.0f - abyssMix));
        col = ProcMath::mix(col, cliffBasalt, ProcMath::clamp(wallF * 0.85f + ProcMath::smoothstep(22.0f, 36.0f, wy) * 0.55f, 0.0f, 1.0f));

        if (wy > 1.5f && wy < 26.5f && norm.y() > 0.56f && wallF < 0.78f) {
            float shallowMask = ProcMath::smoothstep(1.5f, 7.5f, wy)
            * (1.0f - ProcMath::smoothstep(24.5f, 27.0f, wy));
            float flatMask    = ProcMath::smoothstep(0.56f, 0.78f, norm.y());
            float islandMask  = samplePlateauIslandMask(wx, wz, seed);
            float mossBlend   = shallowMask * flatMask * islandMask;
            if (mossBlend > 0.01f) {
                QVector3D lushMoss = ProcMath::mix(QVector3D(0.14f, 0.50f, 0.24f),
                                                   QVector3D(0.32f, 0.72f, 0.26f),
                                                   ProcMath::smoothstep(0.35f, 0.85f, islandMask));
                col = ProcMath::mix(col, lushMoss, mossBlend * 0.82f);
            }
        }

        return ProcMath::mix(col, abyssSlate, abyssMix * 0.92f);
    };

    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (unsigned int t = 0; t < numThreads; ++t) {
        int startChunk = (t * TOTAL_CHUNKS) / numThreads;
        int endChunk   = ((t + 1) * TOTAL_CHUNKS) / numThreads;

        workers.emplace_back([&, startChunk, endChunk]() {
            std::vector<float> h0(P * P);
            std::vector<float> h1(P * P);
            std::vector<float> h2(P * P);
            constexpr int POS_DIM = N + 3; // [-1 .. N+1]
            std::vector<QVector3D> posGrid(POS_DIM * POS_DIM);
            std::vector<uint8_t> subQ((N + 2) * (N + 2)); // [-1 .. N]
            std::vector<uint32_t> hEdgeMid(G * N);
            std::vector<uint32_t> vEdgeMid(N * G);

            for (int ci = startChunk; ci < endChunk; ++ci) {
                int cx = ci % C;
                int cz = ci / C;
                WorldChunk& chunk = data.chunks[ci];
                ChunkTerrainTemp& tmp = chunkTemps[ci];
                chunk.cx = cx;
                chunk.cz = cz;

                const int baseGx = cx * N;
                const int baseGz = cz * N;
                const float originX = gridWorldCoord(baseGx);
                const float originZ = gridWorldCoord(baseGz);

                // ПРОХОД 0: Сэмплирование поля высот с гало-рамкой [-4 .. N+4]
                for (int pz = 0; pz < P; ++pz) {
                    float wz = gridWorldCoord(baseGz + pz - HALO);
                    for (int px = 0; px < P; ++px) {
                        float wx = gridWorldCoord(baseGx + px - HALO);
                        h0[pz * P + px] = ProcMath::sampleTerrainHeight(wx, wz, seed);
                    }
                }

                // ПРОХОД 1 (ПОСТПРОЦЕССИНГ): Despike / Anti-Needle фильтр [-3 .. N+3]
                for (int pz = 1; pz < P - 1; ++pz) {
                    for (int px = 1; px < P - 1; ++px) {
                        float nb[8] = {
                            h0[(pz - 1) * P + (px - 1)], h0[(pz - 1) * P + px], h0[(pz - 1) * P + (px + 1)],
                            h0[ pz      * P + (px - 1)],                        h0[ pz      * P + (px + 1)],
                            h0[(pz + 1) * P + (px - 1)], h0[(pz + 1) * P + px], h0[(pz + 1) * P + (px + 1)]
                        };
                        std::sort(nb, nb + 8);
                        float h = h0[pz * P + px];

                        float allowedHi = nb[6] + 0.50f * CELL_SZ;
                        float allowedLo = nb[1] - 0.50f * CELL_SZ;
                        if (h > allowedHi) {
                            float excess = h - allowedHi;
                            h = allowedHi + 0.34f * CELL_SZ * std::tanh(excess / (0.85f * CELL_SZ));
                        } else if (h < allowedLo) {
                            float excess = allowedLo - h;
                            h = allowedLo - 0.34f * CELL_SZ * std::tanh(excess / (0.85f * CELL_SZ));
                        }

                        if (h > nb[7]) {
                            h = nb[7] + (h - nb[7]) * 0.32f;
                        } else if (h < nb[0]) {
                            h = nb[0] - (nb[0] - h) * 0.32f;
                        }
                        h1[pz * P + px] = h;
                    }
                }

                // ПРОХОД 2 (ПОСТПРОЦЕССИНГ): Краесохраняющая фаска кромок и гашение сеточной «пилы» [-2 .. N+2]
                for (int pz = 2; pz < P - 2; ++pz) {
                    for (int px = 2; px < P - 2; ++px) {
                        float h   = h1[pz * P + px];
                        float hL  = h1[pz * P + (px - 1)];
                        float hR  = h1[pz * P + (px + 1)];
                        float hD  = h1[(pz - 1) * P + px];
                        float hU  = h1[(pz + 1) * P + px];
                        float hDL = h1[(pz - 1) * P + (px - 1)];
                        float hDR = h1[(pz - 1) * P + (px + 1)];
                        float hUL = h1[(pz + 1) * P + (px - 1)];
                        float hUR = h1[(pz + 1) * P + (px + 1)];

                        float avg = (2.0f * (hL + hR + hD + hU) + (hDL + hDR + hUL + hUR)) / 12.0f;
                        float lap = std::abs(h - avg);
                        float k = ProcMath::clamp(0.14f + 0.36f * ProcMath::smoothstep(0.18f, 1.25f, lap), 0.14f, 0.50f);
                        h2[pz * P + px] = ProcMath::mix(h, avg, k);
                    }
                }

                auto getH2 = [&](int ix, int iz) -> float {
                    return h2[(iz + HALO) * P + (ix + HALO)];
                };
                auto getGrad = [&](int ix, int iz, float& outGx, float& outGz) {
                    outGx = (getH2(ix + 1, iz) - getH2(ix - 1, iz)) / (2.0f * CELL_SZ);
                    outGz = (getH2(ix, iz + 1) - getH2(ix, iz - 1)) / (2.0f * CELL_SZ);
                };

                // ПРОХОД 3: 3D-скульптинг всех базовых вершин с 1-ячеечным гало [-1 .. N+1]
                for (int iz = -1; iz <= N + 1; ++iz) {
                    float wz = gridWorldCoord(baseGz + iz);
                    for (int ix = -1; ix <= N + 1; ++ix) {
                        float wx = gridWorldCoord(baseGx + ix);
                        float wy = getH2(ix, iz);
                        float gx = 0.0f, gz = 0.0f;
                        getGrad(ix, iz, gx, gz);
                        posGrid[(iz + 1) * POS_DIM + (ix + 1)] = evalSculptedPoint(wx, wy, wz, gx, gz);
                    }
                }
                auto getPos = [&](int ix, int iz) -> const QVector3D& {
                    return posGrid[(iz + 1) * POS_DIM + (ix + 1)];
                };

                const uint32_t baseGridCount = G * G;
                const uint32_t skirtTop      = baseGridCount;
                const uint32_t skirtBot      = skirtTop + G;
                const uint32_t skirtLeft     = skirtBot + G;
                const uint32_t skirtRight    = skirtLeft + G;

                tmp.verts.clear();
                tmp.verts.reserve(baseGridCount + 4 * G + 2600);
                tmp.verts.resize(baseGridCount + 4 * G);

                auto surfIdx = [&](int ix, int iz) -> uint32_t {
                    return static_cast<uint32_t>(iz * G + ix);
                };

                float minY =  1e6f;
                float maxY = -1e6f;

                for (int iz = 0; iz <= N; ++iz) {
                    for (int ix = 0; ix <= N; ++ix) {
                        const QVector3D& p = getPos(ix, iz);
                        minY = std::min(minY, p.y());
                        maxY = std::max(maxY, p.y());

                        float wy = getH2(ix, iz);
                        float maxEdgeSlope = std::max({
                                                 std::abs(wy - getH2(ix - 1, iz)), std::abs(wy - getH2(ix + 1, iz)),
                                                 std::abs(wy - getH2(ix, iz - 1)), std::abs(wy - getH2(ix, iz + 1))
                                             }) / CELL_SZ;
                        float localFlatness = 1.0f / std::sqrt(1.0f + maxEdgeSlope * maxEdgeSlope);
                        float steepness = ProcMath::clamp(1.0f - localFlatness, 0.0f, 1.0f);

                        EnvVertex& v = tmp.verts[surfIdx(ix, iz)];
                        v.pos    = p;
                        v.normal = QVector3D(0.0f, 1.0f, 0.0f);
                        v.color  = QVector3D(0.5f, 0.5f, 0.5f);
                        v.param  = QVector2D(0.0f, steepness);
                    }
                }

                // ПРОХОД 4: Детерминированная адаптивная перегенерация крутых участков (2x2 Red-Green)
                auto subAt = [&](int qx, int qz) -> uint8_t& {
                    return subQ[(qz + 1) * (N + 2) + (qx + 1)];
                };
                for (int qz = -1; qz <= N; ++qz) {
                    for (int qx = -1; qx <= N; ++qx) {
                        float h00 = getH2(qx,     qz);
                        float h10 = getH2(qx + 1, qz);
                        float h01 = getH2(qx,     qz + 1);
                        float h11 = getH2(qx + 1, qz + 1);
                        float span  = std::max({h00, h10, h01, h11}) - std::min({h00, h10, h01, h11});
                        float twist = std::abs((h00 + h11) - (h10 + h01));
                        subAt(qx, qz) = (span > 0.90f || twist > 0.30f) ? 1 : 0;
                    }
                }

                auto catmullMid = [](float hm1, float h0v, float h1v, float h2v) -> float {
                    float m = 0.5625f * (h0v + h1v) - 0.0625f * (hm1 + h2v);
                    float lo = std::min(h0v, h1v) - 0.14f;
                    float hi = std::max(h0v, h1v) + 0.14f;
                    return ProcMath::clamp(m, lo, hi);
                };

                auto pushMidVertex = [&](float wx, float wy, float wz, float gx, float gz, float steepVal) -> uint32_t {
                    QVector3D p = evalSculptedPoint(wx, wy, wz, gx, gz);
                    minY = std::min(minY, p.y());
                    maxY = std::max(maxY, p.y());
                    uint32_t idx = static_cast<uint32_t>(tmp.verts.size());
                    tmp.verts.push_back({p, QVector3D(0.0f, 1.0f, 0.0f), QVector3D(0.5f, 0.5f, 0.5f), QVector2D(0.0f, steepVal)});
                    return idx;
                };

                std::fill(hEdgeMid.begin(), hEdgeMid.end(), NO_VERT);
                for (int iz = 0; iz <= N; ++iz) {
                    float wz = gridWorldCoord(baseGz + iz);
                    for (int ix = 0; ix < N; ++ix) {
                        if (!subAt(ix, iz - 1) && !subAt(ix, iz)) continue;
                        float wx = 0.5f * (gridWorldCoord(baseGx + ix) + gridWorldCoord(baseGx + ix + 1));
                        float wy = catmullMid(getH2(ix - 1, iz), getH2(ix, iz), getH2(ix + 1, iz), getH2(ix + 2, iz));
                        float gx0, gz0, gx1, gz1;
                        getGrad(ix, iz, gx0, gz0);
                        getGrad(ix + 1, iz, gx1, gz1);
                        float steepAvg = 0.5f * (tmp.verts[surfIdx(ix, iz)].param.y() + tmp.verts[surfIdx(ix + 1, iz)].param.y());
                        hEdgeMid[iz * N + ix] = pushMidVertex(wx, wy, wz, 0.5f * (gx0 + gx1), 0.5f * (gz0 + gz1), steepAvg);
                    }
                }

                std::fill(vEdgeMid.begin(), vEdgeMid.end(), NO_VERT);
                for (int iz = 0; iz < N; ++iz) {
                    float wz = 0.5f * (gridWorldCoord(baseGz + iz) + gridWorldCoord(baseGz + iz + 1));
                    for (int ix = 0; ix <= N; ++ix) {
                        if (!subAt(ix - 1, iz) && !subAt(ix, iz)) continue;
                        float wx = gridWorldCoord(baseGx + ix);
                        float wy = catmullMid(getH2(ix, iz - 1), getH2(ix, iz), getH2(ix, iz + 1), getH2(ix, iz + 2));
                        float gx0, gz0, gx1, gz1;
                        getGrad(ix, iz, gx0, gz0);
                        getGrad(ix, iz + 1, gx1, gz1);
                        float steepAvg = 0.5f * (tmp.verts[surfIdx(ix, iz)].param.y() + tmp.verts[surfIdx(ix, iz + 1)].param.y());
                        vEdgeMid[iz * G + ix] = pushMidVertex(wx, wy, wz, 0.5f * (gx0 + gx1), 0.5f * (gz0 + gz1), steepAvg);
                    }
                }

                // Контурно-ориентированная триангуляция квада (диагональ ложится вдоль линии обрыва)
                auto emitAdaptiveQuad = [&](std::vector<uint32_t>& outInds,
                                            uint32_t v00, uint32_t v10, uint32_t v01, uint32_t v11) {
                    float dMain = std::abs(tmp.verts[v00].pos.y() - tmp.verts[v11].pos.y());
                    float dAlt  = std::abs(tmp.verts[v10].pos.y() - tmp.verts[v01].pos.y());
                    if (dMain < dAlt) {
                        outInds.insert(outInds.end(), {v00, v01, v11, v00, v11, v10});
                    } else {
                        outInds.insert(outInds.end(), {v00, v01, v10, v10, v01, v11});
                    }
                };

                auto& lod0 = tmp.lodInds[0];
                lod0.reserve(N * N * 10);

                for (int iz = 0; iz < N; ++iz) {
                    for (int ix = 0; ix < N; ++ix) {
                        uint32_t v00 = surfIdx(ix,     iz);
                        uint32_t v10 = surfIdx(ix + 1, iz);
                        uint32_t v01 = surfIdx(ix,     iz + 1);
                        uint32_t v11 = surfIdx(ix + 1, iz + 1);

                        uint32_t mTop   = hEdgeMid[ iz      * N + ix];
                        uint32_t mBot   = hEdgeMid[(iz + 1) * N + ix];
                        uint32_t mLeft  = vEdgeMid[ iz      * G + ix];
                        uint32_t mRight = vEdgeMid[ iz      * G + (ix + 1)];

                        if (subAt(ix, iz)) {
                            float wx = 0.5f * (gridWorldCoord(baseGx + ix) + gridWorldCoord(baseGx + ix + 1));
                            float wz = 0.5f * (gridWorldCoord(baseGz + iz) + gridWorldCoord(baseGz + iz + 1));
                            float wy = 0.25f * (getH2(ix, iz) + getH2(ix + 1, iz) + getH2(ix, iz + 1) + getH2(ix + 1, iz + 1));
                            float gx0, gz0, gx1, gz1, gx2, gz2, gx3, gz3;
                            getGrad(ix, iz, gx0, gz0);         getGrad(ix + 1, iz, gx1, gz1);
                            getGrad(ix, iz + 1, gx2, gz2);     getGrad(ix + 1, iz + 1, gx3, gz3);
                            float stAvg = 0.25f * (tmp.verts[v00].param.y() + tmp.verts[v10].param.y()
                                                   + tmp.verts[v01].param.y() + tmp.verts[v11].param.y());
                            uint32_t cMid = pushMidVertex(wx, wy, wz,
                                                          0.25f * (gx0 + gx1 + gx2 + gx3),
                                                          0.25f * (gz0 + gz1 + gz2 + gz3), stAvg);

                            emitAdaptiveQuad(lod0, v00,   mTop,   mLeft, cMid);
                            emitAdaptiveQuad(lod0, mTop,  v10,    cMid,  mRight);
                            emitAdaptiveQuad(lod0, mLeft, cMid,   v01,   mBot);
                            emitAdaptiveQuad(lod0, cMid,  mRight, mBot,  v11);
                        } else {
                            int splits = (mTop != NO_VERT) + (mBot != NO_VERT) + (mLeft != NO_VERT) + (mRight != NO_VERT);
                            if (splits == 0) {
                                emitAdaptiveQuad(lod0, v00, v10, v01, v11);
                            } else {
                                uint32_t ring[8];
                                int rCount = 0;
                                ring[rCount++] = v00;
                                if (mLeft  != NO_VERT) ring[rCount++] = mLeft;
                                ring[rCount++] = v01;
                                if (mBot   != NO_VERT) ring[rCount++] = mBot;
                                ring[rCount++] = v11;
                                if (mRight != NO_VERT) ring[rCount++] = mRight;
                                ring[rCount++] = v10;
                                if (mTop   != NO_VERT) ring[rCount++] = mTop;

                                if (splits == 1) {
                                    int mPos = 0;
                                    for (int k = 0; k < rCount; ++k) {
                                        if (ring[k] >= baseGridCount) { mPos = k; break; }
                                    }
                                    uint32_t mV = ring[mPos];
                                    for (int k = 1; k <= 3; ++k) {
                                        uint32_t a = ring[(mPos + k) % rCount];
                                        uint32_t b = ring[(mPos + k + 1) % rCount];
                                        lod0.insert(lod0.end(), {mV, a, b});
                                    }
                                } else {
                                    float wx = 0.5f * (gridWorldCoord(baseGx + ix) + gridWorldCoord(baseGx + ix + 1));
                                    float wz = 0.5f * (gridWorldCoord(baseGz + iz) + gridWorldCoord(baseGz + iz + 1));
                                    float wy = 0.25f * (getH2(ix, iz) + getH2(ix + 1, iz) + getH2(ix, iz + 1) + getH2(ix + 1, iz + 1));
                                    float gx0, gz0, gx1, gz1, gx2, gz2, gx3, gz3;
                                    getGrad(ix, iz, gx0, gz0);         getGrad(ix + 1, iz, gx1, gz1);
                                    getGrad(ix, iz + 1, gx2, gz2);     getGrad(ix + 1, iz + 1, gx3, gz3);
                                    float stAvg = 0.25f * (tmp.verts[v00].param.y() + tmp.verts[v10].param.y()
                                                           + tmp.verts[v01].param.y() + tmp.verts[v11].param.y());
                                    uint32_t cMid = pushMidVertex(wx, wy, wz,
                                                                  0.25f * (gx0 + gx1 + gx2 + gx3),
                                                                  0.25f * (gz0 + gz1 + gz2 + gz3), stAvg);
                                    for (int k = 0; k < rCount; ++k) {
                                        lod0.insert(lod0.end(), {ring[k], ring[(k + 1) % rCount], cMid});
                                    }
                                }
                            }
                        }
                    }
                }

                // ПРОХОД 5: Истинные взвешенные по площади нормали 3D-меша + бесшовные границы
                std::vector<QVector3D> accumN(tmp.verts.size(), QVector3D(0.0f, 0.0f, 0.0f));
                for (size_t k = 0; k + 2 < lod0.size(); k += 3) {
                    uint32_t i0 = lod0[k], i1 = lod0[k + 1], i2 = lod0[k + 2];
                    QVector3D fn = QVector3D::crossProduct(tmp.verts[i1].pos - tmp.verts[i0].pos,
                                                           tmp.verts[i2].pos - tmp.verts[i0].pos);
                    accumN[i0] += fn;
                    accumN[i1] += fn;
                    accumN[i2] += fn;
                }

                auto haloNormal = [&](int ix, int iz) -> QVector3D {
                    const QVector3D& pL = getPos(ix - 1, iz);
                    const QVector3D& pR = getPos(ix + 1, iz);
                    const QVector3D& pD = getPos(ix, iz - 1);
                    const QVector3D& pU = getPos(ix, iz + 1);
                    QVector3D n = QVector3D::crossProduct(pU - pD, pR - pL);
                    return (n.lengthSquared() > 1e-6f) ? n.normalized() : QVector3D(0.0f, 1.0f, 0.0f);
                };

                for (size_t vIdx = 0; vIdx < tmp.verts.size(); ++vIdx) {
                    if (vIdx >= skirtTop && vIdx < skirtRight + G) continue;
                    QVector3D n = (accumN[vIdx].lengthSquared() > 1e-6f) ? accumN[vIdx].normalized() : QVector3D(0.0f, 1.0f, 0.0f);
                    tmp.verts[vIdx].normal = n;
                }

                for (int i = 0; i <= N; ++i) {
                    tmp.verts[surfIdx(i, 0)].normal = haloNormal(i, 0);
                    tmp.verts[surfIdx(i, N)].normal = haloNormal(i, N);
                    tmp.verts[surfIdx(0, i)].normal = haloNormal(0, i);
                    tmp.verts[surfIdx(N, i)].normal = haloNormal(N, i);
                }
                for (int i = 0; i < N; ++i) {
                    if (hEdgeMid[0 * N + i] != NO_VERT) {
                        tmp.verts[hEdgeMid[0 * N + i]].normal = (tmp.verts[surfIdx(i, 0)].normal + tmp.verts[surfIdx(i + 1, 0)].normal).normalized();
                    }
                    if (hEdgeMid[N * N + i] != NO_VERT) {
                        tmp.verts[hEdgeMid[N * N + i]].normal = (tmp.verts[surfIdx(i, N)].normal + tmp.verts[surfIdx(i + 1, N)].normal).normalized();
                    }
                    if (vEdgeMid[i * G + 0] != NO_VERT) {
                        tmp.verts[vEdgeMid[i * G + 0]].normal = (tmp.verts[surfIdx(0, i)].normal + tmp.verts[surfIdx(0, i + 1)].normal).normalized();
                    }
                    if (vEdgeMid[i * G + N] != NO_VERT) {
                        tmp.verts[vEdgeMid[i * G + N]].normal = (tmp.verts[surfIdx(N, i)].normal + tmp.verts[surfIdx(N, i + 1)].normal).normalized();
                    }
                }

                for (size_t vIdx = 0; vIdx < tmp.verts.size(); ++vIdx) {
                    if (vIdx >= skirtTop && vIdx < skirtRight + G) continue;
                    auto& v = tmp.verts[vIdx];
                    v.color = evalTerrainColor(v.pos.x(), v.pos.y(), v.pos.z(), v.normal);
                }

                auto makeSkirtVert = [&](const EnvVertex& src) {
                    EnvVertex s = src;
                    s.pos.setY(s.pos.y() - SKIRT_DROP);
                    s.color *= 0.80f;
                    return s;
                };
                for (int i = 0; i <= N; ++i) {
                    tmp.verts[skirtTop   + i] = makeSkirtVert(tmp.verts[surfIdx(i, 0)]);
                    tmp.verts[skirtBot   + i] = makeSkirtVert(tmp.verts[surfIdx(i, N)]);
                    tmp.verts[skirtLeft  + i] = makeSkirtVert(tmp.verts[surfIdx(0, i)]);
                    tmp.verts[skirtRight + i] = makeSkirtVert(tmp.verts[surfIdx(N, i)]);
                }

                auto emitSkirtEdge = [&](uint32_t e0, uint32_t e1, uint32_t sk0, uint32_t sk1, uint32_t mid, bool flipOrder) {
                    auto addQuad = [&](uint32_t a, uint32_t b, uint32_t sa, uint32_t sb) {
                        if (!flipOrder) lod0.insert(lod0.end(), {a, b, sa, b, sb, sa});
                        else            lod0.insert(lod0.end(), {a, sa, b, b, sa, sb});
                    };
                    if (mid == NO_VERT) {
                        addQuad(e0, e1, sk0, sk1);
                    } else {
                        uint32_t skMid = static_cast<uint32_t>(tmp.verts.size());
                        tmp.verts.push_back(makeSkirtVert(tmp.verts[mid]));
                        addQuad(e0,  mid, sk0,   skMid);
                        addQuad(mid, e1,  skMid, sk1);
                    }
                };
                for (int i = 0; i < N; ++i) {
                    emitSkirtEdge(surfIdx(i, 0), surfIdx(i + 1, 0), skirtTop + i,   skirtTop + i + 1,   hEdgeMid[0 * N + i], false);
                    emitSkirtEdge(surfIdx(i, N), surfIdx(i + 1, N), skirtBot + i,   skirtBot + i + 1,   hEdgeMid[N * N + i], true);
                    emitSkirtEdge(surfIdx(0, i), surfIdx(0, i + 1), skirtLeft + i,  skirtLeft + i + 1,  vEdgeMid[i * G + 0], true);
                    emitSkirtEdge(surfIdx(N, i), surfIdx(N, i + 1), skirtRight + i, skirtRight + i + 1, vEdgeMid[i * G + N], false);
                }

                // LOD1, LOD2, LOD3 с контурно-ориентированными диагоналями
                const int steps[WorldData::TERRAIN_LOD_COUNT] = {1, 2, 4, 8};
                for (int lod = 1; lod < WorldData::TERRAIN_LOD_COUNT; ++lod) {
                    int step = steps[lod];
                    auto& outInds = tmp.lodInds[lod];
                    outInds.reserve(((N / step) * (N / step) + 4 * (N / step)) * 6);

                    for (int iz = 0; iz < N; iz += step) {
                        for (int ix = 0; ix < N; ix += step) {
                            emitAdaptiveQuad(outInds,
                                             surfIdx(ix, iz), surfIdx(ix + step, iz),
                                             surfIdx(ix, iz + step), surfIdx(ix + step, iz + step));
                        }
                    }
                    for (int i = 0; i < N; i += step) {
                        uint32_t t0 = surfIdx(i, 0), t1 = surfIdx(i + step, 0);
                        uint32_t tk0 = skirtTop + i, tk1 = skirtTop + i + step;
                        outInds.insert(outInds.end(), {t0, t1, tk0, t1, tk1, tk0});

                        uint32_t b0 = surfIdx(i, N), b1 = surfIdx(i + step, N);
                        uint32_t bk0 = skirtBot + i, bk1 = skirtBot + i + step;
                        outInds.insert(outInds.end(), {b0, bk0, b1, b1, bk0, bk1});

                        uint32_t l0 = surfIdx(0, i), l1 = surfIdx(0, i + step);
                        uint32_t lk0 = skirtLeft + i, lk1 = skirtLeft + i + step;
                        outInds.insert(outInds.end(), {l0, lk0, l1, l1, lk0, lk1});

                        uint32_t r0 = surfIdx(N, i), r1 = surfIdx(N, i + step);
                        uint32_t rk0 = skirtRight + i, rk1 = skirtRight + i + step;
                        outInds.insert(outInds.end(), {r0, r1, rk0, r1, rk1, rk0});
                    }
                }

                // Запас +32м по Y учитывает высоту колонн ламинарии и используется как быстрый тест наличия надводных скал
                chunk.minBounds = QVector3D(originX - CELL_SZ, minY - SKIRT_DROP, originZ - CELL_SZ);
                chunk.maxBounds = QVector3D(originX + CHUNK_SZ + CELL_SZ, maxY + 32.0f, originZ + CHUNK_SZ + CELL_SZ);
                chunk.center    = (chunk.minBounds + chunk.maxBounds) * 0.5f;
                chunk.boundingRadius = (chunk.maxBounds - chunk.center).length();
            }
        });
    }
    for (auto& th : workers) th.join();

    size_t totalVerts = 0;
    size_t totalInds  = 0;
    for (const auto& t : chunkTemps) {
        totalVerts += t.verts.size();
        for (int lod = 0; lod < WorldData::TERRAIN_LOD_COUNT; ++lod) {
            totalInds += t.lodInds[lod].size();
        }
    }
    data.terrainVertices.clear();
    data.terrainIndices.clear();
    data.terrainVertices.reserve(totalVerts);
    data.terrainIndices.reserve(totalInds);

    for (int ci = 0; ci < TOTAL_CHUNKS; ++ci) {
        WorldChunk& chunk = data.chunks[ci];
        const auto& tmp = chunkTemps[ci];

        chunk.terrainBaseVertex = static_cast<uint32_t>(data.terrainVertices.size());
        data.terrainVertices.insert(data.terrainVertices.end(), tmp.verts.begin(), tmp.verts.end());

        for (int lod = 0; lod < WorldData::TERRAIN_LOD_COUNT; ++lod) {
            uint32_t offset = static_cast<uint32_t>(data.terrainIndices.size());
            data.terrainIndices.insert(data.terrainIndices.end(),
                                       tmp.lodInds[lod].begin(), tmp.lodInds[lod].end());
            chunk.terrainLodRanges[lod] = {offset, static_cast<uint32_t>(tmp.lodInds[lod].size())};
        }
    }

    // Кеш постпроцессированных высот для коллизий и точной посадки всех пропов и кораблей
    constexpr int HF_SIZE = C * N + 1; // 961
    data.collision.heightGridSize = HF_SIZE;
    data.collision.heightCellSize = CELL_SZ;
    data.collision.heightField.assign(static_cast<size_t>(HF_SIZE) * HF_SIZE, 0.0f);

    for (int ci = 0; ci < TOTAL_CHUNKS; ++ci) {
        const auto& chunk = data.chunks[ci];
        const EnvVertex* src = &data.terrainVertices[chunk.terrainBaseVertex];
        const int baseGx = chunk.cx * N;
        const int baseGz = chunk.cz * N;
        for (int iz = 0; iz <= N; ++iz) {
            float* dstRow = &data.collision.heightField[static_cast<size_t>(baseGz + iz) * HF_SIZE + baseGx];
            const EnvVertex* srcRow = &src[iz * G];
            for (int ix = 0; ix <= N; ++ix) {
                dstRow[ix] = srcRow[ix].pos.y();
            }
        }
    }
}

// ============================================================================
// ПРОТОТИПЫ И РАССЕЛЕНИЕ КАМНЕЙ / СКАЛЬНЫХ УСТУПОВ (+ СФЕР-КОЛЛАЙДЕРЫ)
// ============================================================================

void WorldGenerator::generateEnvironmentPrototypes(WorldData& data) {
    // 0: Гранёный слоистый валун
    buildBoulderPrototype(data.propPrototypes[PROP_BOULDER_CHUNK].lod0Verts,
                          data.propPrototypes[PROP_BOULDER_CHUNK].lod0Inds, 18, 22, 3011u);
    buildBoulderPrototype(data.propPrototypes[PROP_BOULDER_CHUNK].lod1Verts,
                          data.propPrototypes[PROP_BOULDER_CHUNK].lod1Inds, 8, 10, 3011u);
    data.propPrototypes[PROP_BOULDER_CHUNK].baseRadius = 1.35f;

    // 1: Выступающая скальная плита для обрывов и кромок
    buildCliffCragPrototype(data.propPrototypes[PROP_CLIFF_CRAG].lod0Verts,
                            data.propPrototypes[PROP_CLIFF_CRAG].lod0Inds, 16, 20, 3191u);
    buildCliffCragPrototype(data.propPrototypes[PROP_CLIFF_CRAG].lod1Verts,
                            data.propPrototypes[PROP_CLIFF_CRAG].lod1Inds, 7, 9, 3191u);
    data.propPrototypes[PROP_CLIFF_CRAG].baseRadius = 1.85f;
}

void WorldGenerator::generateChunkRocks(WorldData& data, uint32_t seed) {
    constexpr int C = WorldData::CHUNKS_PER_SIDE;
    constexpr int TOTAL_CHUNKS = C * C;
    constexpr float CHUNK_SZ = WorldData::CHUNK_SIZE;
    constexpr float HALF_W = ProcMath::WORLD_HALF_SIZE;
    const auto& pits = ProcMath::getAbyssalPits(seed);

    std::vector<std::vector<SphereCollider>> chunkSpheres(TOTAL_CHUNKS);
    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (unsigned int th = 0; th < numThreads; ++th) {
        int startChunk = (th * TOTAL_CHUNKS) / numThreads;
        int endChunk   = ((th + 1) * TOTAL_CHUNKS) / numThreads;

        workers.emplace_back([&, startChunk, endChunk]() {
            for (int ci = startChunk; ci < endChunk; ++ci) {
                WorldChunk& chunk = data.chunks[ci];
                auto& spheresOut = chunkSpheres[ci];

                float originX = -HALF_W + chunk.cx * CHUNK_SZ;
                float originZ = -HALF_W + chunk.cz * CHUNK_SZ;
                float chunkCenterX = originX + CHUNK_SZ * 0.5f;
                float chunkCenterZ = originZ + CHUNK_SZ * 0.5f;
                float centerWallF  = ProcMath::sampleBasinWallFactor(chunkCenterX, chunkCenterZ, seed);

                auto nearPitCore = [&](float wx, float wz, float frac) {
                    for (const auto& pit : pits) {
                        float dx = wx - pit.x, dz = wz - pit.z;
                        float r = pit.radius * frac;
                        if (dx * dx + dz * dz < r * r) return true;
                    }
                    return false;
                };

                // 1. Валуны и россыпи камней на дне и рифах
                if (centerWallF < 0.90f) {
                    for (int b = 0; b < 24; ++b) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, b * 4 + 1, seed + 211u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, b * 4 + 2, seed + 211u);
                        float hs = ProcMath::hash3(chunk.cx, chunk.cz, b * 4 + 3, seed + 211u);
                        float hr = ProcMath::hash3(chunk.cx, chunk.cz, b * 4 + 4, seed + 211u);

                        float wx = originX + 3.0f + hx * (CHUNK_SZ - 6.0f);
                        float wz = originZ + 3.0f + hz * (CHUNK_SZ - 6.0f);
                        float wallF = ProcMath::sampleBasinWallFactor(wx, wz, seed);
                        if (wallF > 0.86f) continue;

                        bool isHeroBoulder = (b < 10);
                        if (isHeroBoulder) {
                            if ((wx * wx + (wz - 20.0f) * (wz - 20.0f)) < 250.0f) continue;
                            if (((wx - 32.0f) * (wx - 32.0f) + (wz + 34.0f) * (wz + 34.0f)) < 680.0f) continue;
                            if (nearPitCore(wx, wz, 0.55f)) continue;
                        }

                        float wy = data.collision.sampleHeight(wx, wz);
                        if (wy > 22.0f) continue;

                        float scaleXZ = isHeroBoulder
                                            ? (2.5f + hs * 4.8f + wallF * 3.2f)
                                            : (0.75f + hs * 1.75f);
                        float scaleY = scaleXZ * (0.64f + hr * 0.52f);

                        QVector3D center(wx, wy + scaleY * 0.22f, wz);
                        float abyssDark = ProcMath::smoothstep(-26.0f, -48.0f, wy);
                        QVector3D tint = ProcMath::mix(
                            QVector3D(0.40f + hs * 0.14f, 0.48f + hr * 0.14f, 0.48f + hs * 0.10f),
                            QVector3D(0.16f, 0.20f, 0.26f), abyssDark);

                        PropGpuInstance inst;
                        inst.posScaleXZ = QVector4D(center.x(), center.y(), center.z(), scaleXZ);
                        inst.rotScaleY  = QVector4D(hr * TWO_PI, (hs - 0.5f) * 0.35f, (hx - 0.5f) * 0.35f, scaleY);
                        inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                        chunk.propInstances[PROP_BOULDER_CHUNK].push_back(inst);

                        if (scaleXZ >= 2.2f) {
                            spheresOut.push_back({center, std::max(scaleXZ, scaleY) * 0.96f});
                        }
                    }
                }

                // 2. Целенаправленное 3D-наращивание плит по кромкам обрывов, стенам и осыпям
                {
                    constexpr int SCAN_N = 12;
                    int placedCrags = 0;
                    for (int sz = 0; sz < SCAN_N && placedCrags < 74; ++sz) {
                        for (int sx = 0; sx < SCAN_N && placedCrags < 74; ++sx) {
                            int idx = sz * SCAN_N + sx;
                            float jx = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 1, seed + 257u);
                            float jz = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 2, seed + 257u);
                            float hs = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 3, seed + 257u);
                            float hr = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 4, seed + 257u);

                            float wx = originX + ((static_cast<float>(sx) + 0.12f + jx * 0.76f) / SCAN_N) * CHUNK_SZ;
                            float wz = originZ + ((static_cast<float>(sz) + 0.12f + jz * 0.76f) / SCAN_N) * CHUNK_SZ;
                            float wy = data.collision.sampleHeight(wx, wz);
                            if (wy < -58.0f || wy > 36.0f) continue;

                            QVector3D norm = data.collision.sampleNormal(wx, wz);
                            float hAvg = 0.25f * (
                                             data.collision.sampleHeight(wx - 2.6f, wz) +
                                             data.collision.sampleHeight(wx + 2.6f, wz) +
                                             data.collision.sampleHeight(wx, wz - 2.6f) +
                                             data.collision.sampleHeight(wx, wz + 2.6f)
                                             );
                            float lipCurv = wy - hAvg;

                            bool isSteepWall = (norm.y() < 0.68f);
                            bool isCliffLip  = (lipCurv > 0.42f && norm.y() < 0.86f);
                            bool isCliffFoot = (lipCurv < -0.55f && norm.y() < 0.88f);

                            if (!isSteepWall && !isCliffLip && !isCliffFoot) continue;
                            if (!isCliffLip && hr > 0.62f) continue;

                            float abyssDark = ProcMath::smoothstep(-24.0f, -50.0f, wy);
                            QVector3D tint = ProcMath::mix(
                                QVector3D(0.34f + hs * 0.13f, 0.43f + hr * 0.13f, 0.45f + hs * 0.10f),
                                QVector3D(0.14f, 0.18f, 0.24f), abyssDark);

                            if (isCliffFoot && !isSteepWall) {
                                float bXZ = 1.15f + hs * 1.95f;
                                float bY  = bXZ * (0.52f + hr * 0.36f);
                                PropGpuInstance inst;
                                inst.posScaleXZ = QVector4D(wx, wy + bY * 0.18f, wz, bXZ);
                                inst.rotScaleY  = QVector4D(hr * TWO_PI, (hs - 0.5f) * 0.40f, (jx - 0.5f) * 0.40f, bY);
                                inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                                chunk.propInstances[PROP_BOULDER_CHUNK].push_back(inst);
                                continue;
                            }

                            float scaleXZ = isCliffLip ? (1.55f + hs * 2.40f) : (1.75f + hs * 3.15f);
                            float scaleY  = isCliffLip ? (0.95f + hr * 1.45f) : (1.30f + hr * 2.30f);

                            float yaw = 0.0f, pitch = 0.0f;
                            alignForwardAxisToDir(norm, yaw, pitch);
                            float roll = (hr - 0.5f) * 0.65f;

                            QVector3D pos = QVector3D(wx, wy - (isCliffLip ? 0.32f * scaleY : 0.0f), wz)
                                            - norm * (scaleXZ * (isCliffLip ? 0.46f : 0.38f));

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(pos.x(), pos.y(), pos.z(), scaleXZ);
                            inst.rotScaleY  = QVector4D(yaw, pitch, roll, scaleY);
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                            chunk.propInstances[PROP_CLIFF_CRAG].push_back(inst);
                            ++placedCrags;

                            if (scaleXZ >= 2.8f) {
                                spheresOut.push_back({pos + norm * (scaleXZ * 0.42f), scaleXZ * 0.78f});
                            }
                        }
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    // Последовательная регистрация сфер-коллайдеров в CollisionWorld и чанках
    for (int ci = 0; ci < TOTAL_CHUNKS; ++ci) {
        WorldChunk& chunk = data.chunks[ci];
        for (const auto& sph : chunkSpheres[ci]) {
            uint32_t colId = static_cast<uint32_t>(data.collision.spheres.size());
            data.collision.spheres.push_back(sph);
            chunk.sphereColliderIds.push_back(colId);
        }
    }
}

// ============================================================================
// ЗАТОНУВШИЕ КОРАБЛИ (5 типов, 14 судов с OBB-коллайдерами)
// ============================================================================

void WorldGenerator::appendBox(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                               const QVector3D& center, const QVector3D& size,
                               const QVector3D& right, const QVector3D& up, const QVector3D& forward,
                               const QVector3D& color, float matType) {
    QVector3D hx = right * (size.x() * 0.5f);
    QVector3D hy = up * (size.y() * 0.5f);
    QVector3D hz = forward * (size.z() * 0.5f);

    auto addQuad = [&](const QVector3D& p0, const QVector3D& p1, const QVector3D& p2, const QVector3D& p3, const QVector3D& n) {
        uint32_t b = static_cast<uint32_t>(verts.size());
        verts.push_back({p0, n, color, QVector2D(matType, 0.0f)});
        verts.push_back({p1, n, color, QVector2D(matType, 0.0f)});
        verts.push_back({p2, n, color, QVector2D(matType, 0.0f)});
        verts.push_back({p3, n, color, QVector2D(matType, 0.0f)});
        inds.insert(inds.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    };

    QVector3D p000 = center - hx - hy - hz;
    QVector3D p100 = center + hx - hy - hz;
    QVector3D p010 = center - hx + hy - hz;
    QVector3D p110 = center + hx + hy - hz;
    QVector3D p001 = center - hx - hy + hz;
    QVector3D p101 = center + hx - hy + hz;
    QVector3D p011 = center - hx + hy + hz;
    QVector3D p111 = center + hx + hy + hz;

    addQuad(p001, p101, p111, p011,  forward);
    addQuad(p100, p000, p010, p110, -forward);
    addQuad(p101, p100, p110, p111,  right);
    addQuad(p000, p001, p011, p010, -right);
    addQuad(p011, p111, p110, p010,  up);
    addQuad(p000, p100, p101, p001, -up);
}

void WorldGenerator::generateShipwrecks(WorldData& data, uint32_t seed) {
    constexpr int NUM_SHIPS = 14;
    const auto& pits = ProcMath::getAbyssalPits(seed);

    struct HullParams {
        float length;
        float halfBeam;
        float depth;
        float bowSharpness;
        float sternWidthFactor;
        float poopRise;
        float forecastleRise;
        float tumblehome;
        int   ribCount;
        int   mastCount;
        bool  hasBowsprit;
        bool  hasSternGallery;
        QVector3D woodDark;
        QVector3D woodLight;
        QVector3D woodTrim;
    };

    auto buildShipSection = [&](const QVector3D& origin,
                                const QVector3D& rAxis, const QVector3D& uAxis, const QVector3D& fAxis,
                                const HullParams& hp, float uMin, float uMax,
                                bool buildBow, bool buildStern, bool brokenFront, bool brokenBack,
                                uint32_t shipSeed) {
        auto toWorld = [&](float lx, float ly, float lz) -> QVector3D {
            return origin + rAxis * lx + uAxis * ly + fAxis * lz;
        };

        auto beamAt = [&](float u) -> float {
            if (u >= 0.0f) {
                float bowShape = 1.0f - std::pow(u, hp.bowSharpness);
                return hp.halfBeam * std::max(0.03f, bowShape);
            } else {
                float k = -u;
                float sternShape = 1.0f - (1.0f - hp.sternWidthFactor) * std::pow(k, 1.85f);
                return hp.halfBeam * sternShape;
            }
        };

        auto sheerHeightAt = [&](float u) -> float {
            float sternLift = ProcMath::smoothstep(-0.25f, -1.0f, u) * hp.poopRise;
            float bowLift   = ProcMath::smoothstep(0.30f,   1.0f, u) * hp.forecastleRise;
            float curveDip  = 0.14f * hp.depth * (u * u);
            return hp.depth + curveDip + sternLift + bowLift;
        };

        auto ribPointLocal = [&](float u, float s, float side) -> QVector3D {
            float z = u * (hp.length * 0.5f);
            float hb = beamAt(u);
            float hTop = sheerHeightAt(u);

            float keelRise = (u < -0.55f) ? (std::pow((-u - 0.55f) / 0.45f, 1.8f) * hp.depth * 0.28f)
                             : (u >  0.65f) ? (std::pow((u - 0.65f) / 0.35f, 1.9f) * hp.depth * 0.22f)
                                           : 0.0f;

            float flare = std::sin(s * PI * 0.68f);
            float tumble = 1.0f - hp.tumblehome * std::pow(std::max(0.0f, s - 0.62f) / 0.38f, 1.7f);
            float x = side * hb * flare * tumble;
            float y = keelRise + (hTop - keelRise) * std::pow(s, 1.28f);
            return QVector3D(x, y, z);
        };

        float zStart = uMin * (hp.length * 0.5f);
        float zEnd   = uMax * (hp.length * 0.5f);
        appendTimberSegment(data.propVertices, data.propIndices,
                            toWorld(0.0f, 0.0f, zStart), toWorld(0.0f, 0.0f, zEnd),
                            0.58f, 0.72f, uAxis, hp.woodDark);
        appendTimberSegment(data.propVertices, data.propIndices,
                            toWorld(0.0f, 0.52f, zStart + 0.4f), toWorld(0.0f, 0.52f, zEnd - 0.4f),
                            0.42f, 0.36f, uAxis, hp.woodLight * 0.85f);

        constexpr int RIB_SEGS = 5;
        int secRibs = std::max(5, static_cast<int>(std::round(hp.ribCount * (uMax - uMin) * 0.5f)));

        std::vector<float> ribU(secRibs);
        for (int r = 0; r < secRibs; ++r) {
            float fr = static_cast<float>(r) / static_cast<float>(secRibs - 1);
            ribU[r] = ProcMath::mix(uMin + 0.03f, uMax - 0.03f, fr);
        }

        float breachU1 = -0.35f + ProcMath::hash3(static_cast<int>(shipSeed), 1, 9, seed) * 0.75f;
        float breachSide1 = (ProcMath::hash3(static_cast<int>(shipSeed), 2, 9, seed) > 0.5f) ? 1.0f : -1.0f;
        float breachU2 = -0.60f + ProcMath::hash3(static_cast<int>(shipSeed), 3, 9, seed) * 1.10f;

        for (int r = 0; r < secRibs; ++r) {
            float u = ribU[r];
            for (float side : {-1.0f, 1.0f}) {
                float hRot = ProcMath::hash3(static_cast<int>(shipSeed), r, (side > 0 ? 1 : 2), seed + 91u);
                int maxSeg = RIB_SEGS;
                if (std::abs(u - breachU1) < 0.22f && side == breachSide1 && hRot < 0.62f) {
                    maxSeg = 2 + (r % 2);
                } else if (brokenFront && r == secRibs - 1 && hRot < 0.55f) {
                    maxSeg = 3;
                } else if (brokenBack && r == 0 && hRot < 0.55f) {
                    maxSeg = 3;
                }

                for (int sg = 0; sg < maxSeg; ++sg) {
                    float s0 = static_cast<float>(sg) / RIB_SEGS;
                    float s1 = static_cast<float>(sg + 1) / RIB_SEGS;
                    QVector3D l0 = ribPointLocal(u, s0, side);
                    QVector3D l1 = ribPointLocal(u, s1, side);
                    appendTimberSegment(data.propVertices, data.propIndices,
                                        toWorld(l0.x(), l0.y(), l0.z()),
                                        toWorld(l1.x(), l1.y(), l1.z()),
                                        0.30f, 0.34f, fAxis, hp.woodLight);
                }

                if (r + 1 < secRibs) {
                    float uNext = ribU[r + 1];
                    float uMid = 0.5f * (u + uNext);
                    for (int sg = 0; sg < RIB_SEGS; ++sg) {
                        float ph = ProcMath::hash3(static_cast<int>(shipSeed), r * 7 + sg, (side > 0 ? 5 : 6), seed + 173u);
                        bool inBreach1 = (std::abs(uMid - breachU1) < 0.28f && side == breachSide1 && sg >= 1);
                        bool inBreach2 = (std::abs(uMid - breachU2) < 0.20f && side == -breachSide1 && sg >= 2);
                        bool weatheredGap = (sg >= 3 && ph < 0.34f) || (sg == 2 && ph < 0.24f) || (sg <= 1 && ph < 0.14f);
                        if (inBreach1 || inBreach2 || weatheredGap) continue;

                        float s0 = static_cast<float>(sg) / RIB_SEGS;
                        float s1 = static_cast<float>(sg + 1) / RIB_SEGS;

                        QVector3D la0 = ribPointLocal(u,     s0, side);
                        QVector3D la1 = ribPointLocal(u,     s1, side);
                        QVector3D lb0 = ribPointLocal(uNext, s0, side);
                        QVector3D lb1 = ribPointLocal(uNext, s1, side);

                        QVector3D wa0 = toWorld(la0.x(), la0.y(), la0.z());
                        QVector3D wa1 = toWorld(la1.x(), la1.y(), la1.z());
                        QVector3D wb0 = toWorld(lb0.x(), lb0.y(), lb0.z());
                        QVector3D wb1 = toWorld(lb1.x(), lb1.y(), lb1.z());

                        QVector3D edgeZ = wb0 - wa0;
                        QVector3D edgeS = wa1 - wa0;
                        QVector3D outN = QVector3D::crossProduct(edgeS, edgeZ) * side;
                        if (outN.lengthSquared() < 1e-4f) outN = rAxis * side;
                        QVector3D plankCol = (sg % 2 == 0) ? hp.woodDark : (hp.woodLight * 0.88f);
                        appendPlankStrip(data.propVertices, data.propIndices, wa0, wa1, wb0, wb1, outN, 0.16f, plankCol);
                    }
                }
            }

            float deckS = 0.78f;
            QVector3D pL = ribPointLocal(u, deckS, -1.0f);
            QVector3D pR = ribPointLocal(u, deckS,  1.0f);
            float beamHash = ProcMath::hash3(static_cast<int>(shipSeed), r, 33, seed + 251u);
            if (beamHash > 0.22f) {
                appendTimberSegment(data.propVertices, data.propIndices,
                                    toWorld(pL.x(), pL.y(), pL.z()),
                                    toWorld(pR.x(), pR.y(), pR.z()),
                                    0.28f, 0.30f, uAxis, hp.woodDark);
            }

            if (r + 1 < secRibs) {
                float uNext = ribU[r + 1];
                bool isCargoHatch = (u > -0.22f && u < 0.28f && beamHash < 0.72f);
                if (!isCargoHatch && beamHash > 0.35f) {
                    QVector3D qL0 = ribPointLocal(u,     deckS, -0.88f);
                    QVector3D qR0 = ribPointLocal(u,     deckS,  0.88f);
                    QVector3D qL1 = ribPointLocal(uNext, deckS, -0.88f);
                    QVector3D qR1 = ribPointLocal(uNext, deckS,  0.88f);
                    appendPlankStrip(data.propVertices, data.propIndices,
                                     toWorld(qL0.x(), qL0.y(), qL0.z()),
                                     toWorld(qR0.x(), qR0.y(), qR0.z()),
                                     toWorld(qL1.x(), qL1.y(), qL1.z()),
                                     toWorld(qR1.x(), qR1.y(), qR1.z()),
                                     uAxis, 0.14f, hp.woodLight * 0.82f);
                }
            }
        }

        for (int r = 0; r + 1 < secRibs; ++r) {
            for (float side : {-1.0f, 1.0f}) {
                float gh = ProcMath::hash3(static_cast<int>(shipSeed), r, (side > 0 ? 71 : 72), seed);
                if (std::abs(ribU[r] - breachU1) < 0.24f && side == breachSide1) continue;
                if (gh < 0.18f) continue;
                QVector3D g0 = ribPointLocal(ribU[r],     1.0f, side);
                QVector3D g1 = ribPointLocal(ribU[r + 1], 1.0f, side);
                appendTimberSegment(data.propVertices, data.propIndices,
                                    toWorld(g0.x(), g0.y(), g0.z()),
                                    toWorld(g1.x(), g1.y(), g1.z()),
                                    0.32f, 0.26f, uAxis, hp.woodTrim);
            }
        }

        if (buildBow) {
            float zBowBase = hp.length * 0.46f;
            float bowH = sheerHeightAt(1.0f);
            QVector3D stemPts[5] = {
                toWorld(0.0f, 0.0f,          zBowBase),
                toWorld(0.0f, bowH * 0.28f,  zBowBase + hp.length * 0.055f),
                toWorld(0.0f, bowH * 0.62f,  zBowBase + hp.length * 0.105f),
                toWorld(0.0f, bowH * 0.94f,  zBowBase + hp.length * 0.135f),
                toWorld(0.0f, bowH * 1.12f,  zBowBase + hp.length * 0.165f)
            };
            for (int k = 0; k < 4; ++k) {
                appendTimberSegment(data.propVertices, data.propIndices,
                                    stemPts[k], stemPts[k + 1], 0.48f, 0.56f, rAxis, hp.woodDark);
            }

            float uLast = ribU.back();
            for (float side : {-1.0f, 1.0f}) {
                for (int sg = 1; sg <= 4; ++sg) {
                    float s = static_cast<float>(sg) / 4.5f;
                    QVector3D rPt = ribPointLocal(uLast, s, side);
                    appendTimberSegment(data.propVertices, data.propIndices,
                                        toWorld(rPt.x(), rPt.y(), rPt.z()),
                                        stemPts[std::min(4, sg)],
                                        0.22f, 0.24f, uAxis, hp.woodLight * 0.92f);
                }
            }

            if (hp.hasBowsprit) {
                float bowspritLen = hp.length * (0.28f + 0.08f * ProcMath::hash3(static_cast<int>(shipSeed), 41, 1, seed));
                QVector3D bsRoot = toWorld(0.0f, bowH * 0.86f, zBowBase - 1.2f);
                QVector3D bsTip  = toWorld(0.0f, bowH * 0.86f + bowspritLen * 0.36f, zBowBase + bowspritLen);
                appendTimberSegment(data.propVertices, data.propIndices, bsRoot, bsTip, 0.38f, 0.38f, rAxis, hp.woodTrim);
                QVector3D yardCenter = ProcMath::mix(bsRoot, bsTip, 0.58f);
                appendTimberSegment(data.propVertices, data.propIndices,
                                    yardCenter - rAxis * (hp.halfBeam * 0.95f),
                                    yardCenter + rAxis * (hp.halfBeam * 0.95f),
                                    0.20f, 0.20f, uAxis, hp.woodLight);
            }
        }

        if (buildStern) {
            float zStern = -hp.length * 0.5f;
            float sternH = sheerHeightAt(-1.0f);
            float sternW = beamAt(-1.0f);

            QVector3D spBot = toWorld(0.0f, 0.0f, zStern + 0.4f);
            QVector3D spTop = toWorld(0.0f, sternH * 0.98f, zStern - 1.1f);
            appendTimberSegment(data.propVertices, data.propIndices, spBot, spTop, 0.48f, 0.56f, rAxis, hp.woodDark);

            float rudderAng = (ProcMath::hash3(static_cast<int>(shipSeed), 51, 2, seed) - 0.5f) * 0.9f;
            QVector3D rudDir = (-fAxis * std::cos(rudderAng) + rAxis * std::sin(rudderAng)).normalized();
            QVector3D rMid = ProcMath::mix(spBot, spTop, 0.36f) + rudDir * 0.95f;
            appendBox(data.propVertices, data.propIndices, rMid,
                      QVector3D(0.24f, sternH * 0.58f, 1.85f),
                      QVector3D::crossProduct(uAxis, rudDir).normalized(), uAxis, rudDir, hp.woodTrim, 1.0f);

            QVector3D tBotL = toWorld(-sternW * 0.72f, hp.depth * 0.28f, zStern + 0.2f);
            QVector3D tBotR = toWorld( sternW * 0.72f, hp.depth * 0.28f, zStern + 0.2f);
            QVector3D tMidL = toWorld(-sternW * 0.96f, sternH * 0.64f,   zStern - 0.65f);
            QVector3D tMidR = toWorld( sternW * 0.96f, sternH * 0.64f,   zStern - 0.65f);
            QVector3D tTopL = toWorld(-sternW * 0.80f, sternH * 1.04f,   zStern - 1.35f);
            QVector3D tTopR = toWorld( sternW * 0.80f, sternH * 1.04f,   zStern - 1.35f);

            appendPlankStrip(data.propVertices, data.propIndices, tBotL, tMidL, tBotR, tMidR, -fAxis, 0.20f, hp.woodDark);

            appendTimberSegment(data.propVertices, data.propIndices, tMidL, tTopL, 0.34f, 0.34f, fAxis, hp.woodTrim);
            appendTimberSegment(data.propVertices, data.propIndices, tMidR, tTopR, 0.34f, 0.34f, fAxis, hp.woodTrim);
            appendTimberSegment(data.propVertices, data.propIndices, tTopL, tTopR, 0.38f, 0.34f, uAxis, hp.woodTrim);
            appendTimberSegment(data.propVertices, data.propIndices, tMidL, tMidR, 0.34f, 0.32f, uAxis, hp.woodTrim);

            if (hp.hasSternGallery) {
                int mullions = 5;
                for (int m = 1; m < mullions; ++m) {
                    float fm = static_cast<float>(m) / static_cast<float>(mullions);
                    QVector3D wmBot = ProcMath::mix(tMidL, tMidR, fm);
                    QVector3D wmTop = ProcMath::mix(tTopL, tTopR, fm);
                    appendTimberSegment(data.propVertices, data.propIndices, wmBot, wmTop, 0.18f, 0.22f, fAxis, hp.woodTrim);
                }
                appendTimberSegment(data.propVertices, data.propIndices,
                                    ProcMath::mix(tMidL, tTopL, 0.55f),
                                    ProcMath::mix(tMidR, tTopR, 0.55f),
                                    0.16f, 0.20f, uAxis, hp.woodLight);

                float poopFrontU = std::min(uMax - 0.1f, -0.45f);
                if (poopFrontU > uMin + 0.08f) {
                    QVector3D pfL = ribPointLocal(poopFrontU, 0.96f, -0.92f);
                    QVector3D pfR = ribPointLocal(poopFrontU, 0.96f,  0.92f);
                    appendPlankStrip(data.propVertices, data.propIndices,
                                     tTopL - uAxis * 0.55f, tTopR - uAxis * 0.55f,
                                     toWorld(pfL.x(), pfL.y(), pfL.z()),
                                     toWorld(pfR.x(), pfR.y(), pfR.z()),
                                     uAxis, 0.16f, hp.woodLight * 0.85f);
                }
            } else {
                appendPlankStrip(data.propVertices, data.propIndices, tMidL, tTopL, tMidR, tTopR, -fAxis, 0.18f, hp.woodLight * 0.82f);
            }
        }

        auto addSplinters = [&](float uEdge, float dirSign) {
            for (int k = 0; k < 6; ++k) {
                float side = (k % 2 == 0) ? -1.0f : 1.0f;
                float s = 0.20f + 0.14f * k;
                QVector3D p0 = ribPointLocal(uEdge, s, side);
                float spLen = 1.2f + ProcMath::hash3(static_cast<int>(shipSeed), k, 88, seed) * 2.4f;
                QVector3D w0 = toWorld(p0.x(), p0.y(), p0.z());
                QVector3D w1 = w0 + fAxis * (dirSign * spLen) + uAxis * ((k % 3 - 1) * 0.35f);
                appendTimberSegment(data.propVertices, data.propIndices, w0, w1, 0.24f, 0.14f, rAxis, hp.woodLight * 0.85f);
            }
        };
        if (brokenFront) addSplinters(uMax,  1.0f);
        if (brokenBack)  addSplinters(uMin, -1.0f);

        const float mastUs[3] = {0.34f, -0.02f, -0.44f};
        for (int m = 0; m < hp.mastCount; ++m) {
            float mu = mastUs[m];
            if (mu < uMin + 0.08f || mu > uMax - 0.08f) continue;

            float mhHash = ProcMath::hash3(static_cast<int>(shipSeed), m, 61, seed);
            bool snapped = (mhHash > 0.48f);
            float fullH = (m == 1) ? (hp.length * 0.62f) : (hp.length * 0.48f);
            float mastH = snapped ? (fullH * (0.34f + mhHash * 0.28f)) : fullH;

            QVector3D mBase = toWorld(0.0f, 0.55f, mu * hp.length * 0.5f);
            float lean = (mhHash - 0.5f) * 0.14f;
            QVector3D mTop = mBase + (uAxis + fAxis * lean + rAxis * (lean * 0.6f)).normalized() * mastH;
            appendTimberSegment(data.propVertices, data.propIndices, mBase, mTop, 0.52f, 0.52f, rAxis, hp.woodDark);

            if (!snapped) {
                QVector3D yardPos = ProcMath::mix(mBase, mTop, 0.64f);
                float yardW = hp.halfBeam * 2.5f;
                float yTilt = (ProcMath::hash3(static_cast<int>(shipSeed), m, 62, seed) - 0.5f) * 0.42f;
                QVector3D yDir = (rAxis * std::cos(yTilt) + uAxis * std::sin(yTilt)).normalized();
                appendTimberSegment(data.propVertices, data.propIndices,
                                    yardPos - yDir * (yardW * 0.5f),
                                    yardPos + yDir * (yardW * 0.5f),
                                    0.26f, 0.26f, fAxis, hp.woodLight);

                QVector3D topYard = ProcMath::mix(mBase, mTop, 0.85f);
                appendTimberSegment(data.propVertices, data.propIndices,
                                    topYard - yDir * (yardW * 0.34f),
                                    topYard + yDir * (yardW * 0.34f),
                                    0.20f, 0.20f, fAxis, hp.woodLight);

                appendBox(data.propVertices, data.propIndices,
                          ProcMath::mix(mBase, mTop, 0.72f),
                          QVector3D(1.65f, 0.32f, 1.45f), rAxis, uAxis, fAxis, hp.woodTrim, 1.0f);
            }
        }
    };

    for (int s = 0; s < NUM_SHIPS; ++s) {
        int shipType = s % 5;

        float wx = 0.0f, wz = 0.0f, wy = 0.0f;
        bool placed = false;
        int chosenCx = 0, chosenCz = 0;

        for (int attempt = 0; attempt < 12; ++attempt) {
            if (s == 0 && attempt == 0) {
                wx = 32.0f;
                wz = -34.0f;
            } else if (s == 1 && attempt == 0) {
                shipType = 2;
                wx = -58.0f;
                wz = -24.0f;
            } else {
                float ang = (TWO_PI * s) / static_cast<float>(NUM_SHIPS)
                + ProcMath::hash3(s, attempt, 2, seed + 109u) * 0.65f + attempt * 0.25f;
                float dist = 115.0f + ProcMath::hash3(s, attempt, 4, seed + 109u) * 320.0f;
                wx = std::cos(ang) * dist;
                wz = std::sin(ang) * dist;
            }

            if (ProcMath::sampleBasinWallFactor(wx, wz, seed) > 0.28f) continue;

            bool inPit = false;
            for (const auto& pit : pits) {
                float dx = wx - pit.x, dz = wz - pit.z;
                if (dx * dx + dz * dz < (pit.radius * 0.92f) * (pit.radius * 0.92f)) {
                    inPit = true;
                    break;
                }
            }
            if (inPit) continue;

            wy = data.collision.sampleHeight(wx, wz);
            if (wy < -34.0f || wy > 4.0f) continue;

            chosenCx = worldToChunkCoord(wx, ProcMath::WORLD_HALF_SIZE, WorldData::CHUNK_SIZE, WorldData::CHUNKS_PER_SIDE);
            chosenCz = worldToChunkCoord(wz, ProcMath::WORLD_HALF_SIZE, WorldData::CHUNK_SIZE, WorldData::CHUNKS_PER_SIDE);
            if (data.chunks[chosenCz * WorldData::CHUNKS_PER_SIDE + chosenCx].shipwreckRange.indexCount > 0) continue;

            placed = true;
            break;
        }
        if (!placed) continue;

        WorldChunk& chunk = data.chunks[chosenCz * WorldData::CHUNKS_PER_SIDE + chosenCx];

        float hVar1 = ProcMath::hash3(s, 11, 12, seed + 401u);
        float hVar2 = ProcMath::hash3(s, 13, 14, seed + 401u);

        HullParams hp{};
        if (shipType == 0) {
            hp = {31.0f + hVar1 * 5.0f, 4.4f + hVar2 * 0.7f, 4.8f, 2.15f, 0.56f, 2.9f, 1.35f, 0.22f,
                  17, 3, true, true,
                  QVector3D(0.22f, 0.16f, 0.12f), QVector3D(0.32f, 0.24f, 0.18f), QVector3D(0.36f, 0.29f, 0.20f)};
        } else if (shipType == 1) {
            hp = {24.0f + hVar1 * 4.0f, 3.4f + hVar2 * 0.5f, 3.8f, 1.75f, 0.52f, 1.15f, 0.95f, 0.14f,
                  15, 2, true, false,
                  QVector3D(0.18f, 0.20f, 0.18f), QVector3D(0.27f, 0.28f, 0.24f), QVector3D(0.31f, 0.26f, 0.19f)};
        } else if (shipType == 2) {
            hp = {30.0f + hVar1 * 4.0f, 4.2f + hVar2 * 0.6f, 4.5f, 2.05f, 0.58f, 2.3f, 1.10f, 0.20f,
                  16, 2, true, true,
                  QVector3D(0.20f, 0.15f, 0.13f), QVector3D(0.29f, 0.23f, 0.19f), QVector3D(0.34f, 0.27f, 0.21f)};
        } else if (shipType == 3) {
            hp = {21.5f + hVar1 * 3.5f, 3.9f + hVar2 * 0.5f, 4.2f, 2.35f, 0.64f, 2.5f, 1.25f, 0.24f,
                  13, 2, true, true,
                  QVector3D(0.26f, 0.18f, 0.13f), QVector3D(0.35f, 0.26f, 0.19f), QVector3D(0.40f, 0.30f, 0.21f)};
        } else {
            hp = {25.5f + hVar1 * 4.0f, 4.9f + hVar2 * 0.7f, 5.0f, 2.65f, 0.68f, 1.9f, 1.60f, 0.18f,
                  15, 2, false, false,
                  QVector3D(0.23f, 0.19f, 0.15f), QVector3D(0.31f, 0.27f, 0.21f), QVector3D(0.34f, 0.28f, 0.22f)};
        }

        float yaw   = (s == 0) ? 0.65f : (ProcMath::hash3(s, 5, 6, seed) * TWO_PI);
        float pitch = (ProcMath::hash3(s, 9, 10, seed) - 0.5f) * 0.20f;
        float roll  = (shipType == 3)
                         ? ((hVar1 > 0.5f ? 1.0f : -1.0f) * (0.85f + hVar2 * 0.28f))
                         : ((ProcMath::hash3(s, 7, 8, seed) - 0.5f) * 0.42f);

        auto makeBasis = [](float yw, float pt, float rl, QVector3D& rOut, QVector3D& uOut, QVector3D& fOut) {
            fOut = QVector3D(std::sin(yw) * std::cos(pt), std::sin(pt), std::cos(yw) * std::cos(pt)).normalized();
            QVector3D rBase = QVector3D::crossProduct(QVector3D(0.0f, 1.0f, 0.0f), fOut).normalized();
            QVector3D uBase = QVector3D::crossProduct(fOut, rBase).normalized();
            rOut = (rBase * std::cos(rl) + uBase * std::sin(rl)).normalized();
            uOut = QVector3D::crossProduct(fOut, rOut).normalized();
        };

        uint32_t startIdx = static_cast<uint32_t>(data.propIndices.size());

        if (shipType == 2) {
            QVector3D r1, u1, f1, r2, u2, f2;
            makeBasis(yaw + 0.18f, pitch + 0.12f, roll - 0.28f, r1, u1, f1);
            makeBasis(yaw - 0.24f, pitch - 0.09f, roll + 0.34f, r2, u2, f2);

            QVector3D bowAnchor(wx + f1.x() * 3.8f, 0.0f, wz + f1.z() * 3.8f);
            bowAnchor.setY(data.collision.sampleHeight(bowAnchor.x(), bowAnchor.z()) - 0.2f);

            QVector3D sternAnchor(wx - f2.x() * 3.8f, 0.0f, wz - f2.z() * 3.8f);
            sternAnchor.setY(data.collision.sampleHeight(sternAnchor.x(), sternAnchor.z()) - 0.2f);

            buildShipSection(bowAnchor,   r1, u1, f1, hp,  0.04f,  1.0f, true,  false, false, true,  seed + s * 97u);
            buildShipSection(sternAnchor, r2, u2, f2, hp, -1.0f,  -0.04f, false, true,  true,  false, seed + s * 97u + 19u);

            QVector3D mP0 = sternAnchor + u2 * 1.4f + r2 * 2.5f;
            QVector3D mP1 = bowAnchor   + u1 * 0.9f - r1 * 4.2f;
            appendTimberSegment(data.propVertices, data.propIndices, mP0, mP1, 0.44f, 0.44f, QVector3D(0, 1, 0), hp.woodDark);

            auto regObb = [&](const QVector3D& anchor, const QVector3D& rx, const QVector3D& ux, const QVector3D& fx, float zShift) {
                OBBCollider obb;
                obb.center = anchor + fx * zShift + ux * (hp.depth * 0.55f);
                obb.axisX = rx;
                obb.axisY = ux;
                obb.axisZ = fx;
                obb.halfExtents = QVector3D(hp.halfBeam * 1.04f, hp.depth * 0.65f, hp.length * 0.27f);
                obb.boundingRadius = obb.halfExtents.length();
                uint32_t id = static_cast<uint32_t>(data.collision.obbs.size());
                data.collision.obbs.push_back(obb);
                chunk.obbColliderIds.push_back(id);
            };
            regObb(bowAnchor,   r1, u1, f1,  hp.length * 0.25f);
            regObb(sternAnchor, r2, u2, f2, -hp.length * 0.25f);
        } else {
            QVector3D rTilted, uTilted, fwd;
            makeBasis(yaw, pitch, roll, rTilted, uTilted, fwd);

            float sinkOffset = (shipType == 3) ? (hp.halfBeam * 0.35f) : -0.35f;
            QVector3D shipOrigin(wx, wy + sinkOffset, wz);

            buildShipSection(shipOrigin, rTilted, uTilted, fwd, hp, -1.0f, 1.0f, true, true, false, false, seed + s * 97u);

            OBBCollider hullObb;
            hullObb.center = shipOrigin + uTilted * (hp.depth * 0.55f);
            hullObb.axisX = rTilted;
            hullObb.axisY = uTilted;
            hullObb.axisZ = fwd;
            hullObb.halfExtents = QVector3D(hp.halfBeam * 1.04f, hp.depth * 0.68f, hp.length * 0.52f);
            hullObb.boundingRadius = hullObb.halfExtents.length();

            uint32_t obbId = static_cast<uint32_t>(data.collision.obbs.size());
            data.collision.obbs.push_back(hullObb);
            chunk.obbColliderIds.push_back(obbId);
        }

        uint32_t addedCount = static_cast<uint32_t>(data.propIndices.size()) - startIdx;
        chunk.shipwreckRange = {startIdx, addedCount};

        chunk.maxBounds.setY(std::max(chunk.maxBounds.y(), wy + 26.0f));
        chunk.center = (chunk.minBounds + chunk.maxBounds) * 0.5f;
        chunk.boundingRadius = std::max(chunk.boundingRadius, (chunk.maxBounds - chunk.center).length() + 18.0f);
    }
}

// ============================================================================
// ОБЪЁМНЫЕ СОЛНЕЧНЫЕ ЛУЧИ, ПУЗЫРЬКИ И ПЛАНКТОН
// ============================================================================

void WorldGenerator::generateGodRaysAndParticles(WorldData& data, uint32_t seed) {
    // 1. Единая замкнутая прокси-геометрия единичного цилиндра (CCW наружу)
    // Локальные координаты: x = cos(a), y = 0 (верх) .. 1 (низ), z = sin(a)
    constexpr int SIDES = 16;
    data.godRayMeshVerts.clear();
    data.godRayMeshInds.clear();
    data.godRayMeshVerts.reserve(2 + SIDES * 2);

    const uint32_t topCenterIdx = 0;
    const uint32_t botCenterIdx = 1;
    data.godRayMeshVerts.emplace_back(0.0f, 0.0f, 0.0f);
    data.godRayMeshVerts.emplace_back(0.0f, 1.0f, 0.0f);

    const uint32_t topRingBase = 2;
    const uint32_t botRingBase = 2 + SIDES;
    for (int s = 0; s < SIDES; ++s) {
        float a = (TWO_PI * static_cast<float>(s)) / static_cast<float>(SIDES);
        float c = std::cos(a), sn = std::sin(a);
        data.godRayMeshVerts.emplace_back(c, 0.0f, sn);
    }
    for (int s = 0; s < SIDES; ++s) {
        float a = (TWO_PI * static_cast<float>(s)) / static_cast<float>(SIDES);
        float c = std::cos(a), sn = std::sin(a);
        data.godRayMeshVerts.emplace_back(c, 1.0f, sn);
    }

    for (int s = 0; s < SIDES; ++s) {
        uint32_t sNext = static_cast<uint32_t>((s + 1) % SIDES);
        uint32_t t0 = topRingBase + s;
        uint32_t t1 = topRingBase + sNext;
        uint32_t b0 = botRingBase + s;
        uint32_t b1 = botRingBase + sNext;

        // Боковая стенка (нормаль наружу в правостороннем базисе U, A, W)
        data.godRayMeshInds.insert(data.godRayMeshInds.end(), {t0, b0, t1, t1, b0, b1});
        // Верхний торец (нормаль -A)
        data.godRayMeshInds.insert(data.godRayMeshInds.end(), {topCenterIdx, t0, t1});
        // Нижний торец (нормаль +A)
        data.godRayMeshInds.insert(data.godRayMeshInds.end(), {botCenterIdx, b1, b0});
    }

    // 2. Инстансы объёмных солнечных столбов по всей акватории
    const QVector3D sunDir = QVector3D(-0.28f, 0.92f, -0.22f).normalized();
    constexpr int SHAFT_COUNT = 210;
    data.godRayInstances.clear();
    data.godRayInstances.reserve(SHAFT_COUNT);

    for (int i = 0; i < SHAFT_COUNT; ++i) {
        float h1 = ProcMath::hash3(i, 1, 2, seed + 601u);
        float h2 = ProcMath::hash3(i, 3, 4, seed + 601u);
        float h3 = ProcMath::hash3(i, 5, 6, seed + 601u);
        float h4 = ProcMath::hash3(i, 7, 8, seed + 601u);

        float ang  = h1 * TWO_PI;
        float dist = (i < 14) ? (24.0f + std::sqrt(h2) * 95.0f) : (std::sqrt(h2) * 460.0f);
        float x = std::cos(ang) * dist;
        float z = std::sin(ang) * dist;

        float floorTop = data.collision.sampleHeight(x, z);
        if (floorTop > 13.0f) continue;

        float topY = ProcMath::WATER_SURFACE_Y - 0.10f;
        float estDrop = std::max(18.0f, topY - floorTop);
        QVector3D estBot = QVector3D(x, topY, z) - sunDir * (estDrop / sunDir.y());
        float floorBot = data.collision.sampleHeight(estBot.x(), estBot.z());
        float botY = std::max(std::min(floorTop, floorBot) - 6.0f, -40.0f);

        float shaftLen = (topY - botY) / sunDir.y();
        if (shaftLen < 10.0f) continue;

        QVector3D topCenter(x, topY, z);
        QVector3D botCenter = topCenter - sunDir * shaftLen;

        float rTop = 2.2f + h3 * 3.0f;
        float rBot = rTop * (1.35f + h4 * 0.35f);
        float intensity = 0.85f + h4 * 0.50f;
        float freqScale = 0.85f + h3 * 0.45f;

        GodRayInstance inst;
        inst.topCenterRTop = QVector4D(topCenter.x(), topCenter.y(), topCenter.z(), rTop);
        inst.botCenterRBot = QVector4D(botCenter.x(), botCenter.y(), botCenter.z(), rBot);
        inst.rayParams     = QVector4D(intensity, h1, freqScale, 0.0f);
        data.godRayInstances.push_back(inst);
    }

    // 3. Всплывающие вертикальные завесы пузырьков
    constexpr int BUBBLE_STREAMS     = 36;
    constexpr int BUBBLES_PER_STREAM = 34;
    constexpr int AMBIENT_PARTICLES  = 2376; // Всего ровно 3600 частиц
    data.particles.clear();
    data.particles.reserve(BUBBLE_STREAMS * BUBBLES_PER_STREAM + AMBIENT_PARTICLES);

    for (int st = 0; st < BUBBLE_STREAMS; ++st) {
        float sx = (ProcMath::hash3(st, 1, 7, seed + 751u) - 0.5f) * 156.0f;
        float sz = (ProcMath::hash3(st, 2, 9, seed + 751u) - 0.5f) * 156.0f;
        float streamSpd = 1.9f + ProcMath::hash3(st, 3, 11, seed + 751u) * 1.8f;

        for (int b = 0; b < BUBBLES_PER_STREAM; ++b) {
            float h1 = ProcMath::hash3(st, b, 1, seed + 761u);
            float h2 = ProcMath::hash3(st, b, 2, seed + 761u);
            float h3 = ProcMath::hash3(st, b, 3, seed + 761u);

            float ang = h1 * TWO_PI;
            float rad = 0.12f + h2 * 1.15f;
            float y = -28.0f + (static_cast<float>(b) / static_cast<float>(BUBBLES_PER_STREAM)) * 56.0f + (h3 - 0.5f) * 1.2f;

            Particle p;
            p.pos = QVector3D(sx + std::cos(ang) * rad, y, sz + std::sin(ang) * rad);
            float szVal = -(0.12f + h3 * 0.34f); // отрицательный размер -> шейдер рисует пузырёк с бликом
            p.colorSize = QVector4D(0.80f, 0.97f, 1.00f, szVal);
            p.speedY = streamSpd + (h2 - 0.5f) * 0.45f;
            p.phase = h1 * TWO_PI;
            data.particles.push_back(p);
        }
    }

    // 4. Рассеянные пузырьки, морской снег и биолюминесцентный планктон в толще воды
    for (int i = 0; i < AMBIENT_PARTICLES; ++i) {
        float hx = ProcMath::hash3(i, 1, 3, seed + 733u);
        float hy = ProcMath::hash3(i, 2, 4, seed + 733u);
        float hz = ProcMath::hash3(i, 3, 5, seed + 733u);
        float ht = ProcMath::hash3(i, 4, 6, seed + 733u);

        bool isBubble = (ht > 0.54f);
        Particle p;
        p.pos = QVector3D((hx - 0.5f) * 160.0f, -26.0f + hy * 54.0f, (hz - 0.5f) * 160.0f);
        if (isBubble) {
            float sz = -(0.13f + (ht - 0.54f) * 0.42f);
            p.colorSize = QVector4D(0.76f, 0.96f, 1.0f, sz);
            p.speedY = 1.55f + hy * 2.15f;
        } else {
            float sz = 0.15f + ht * 0.34f;
            p.colorSize = (ht < 0.28f) ? QVector4D(0.36f, 1.0f, 0.78f, sz)
                                       : QVector4D(0.50f, 0.80f, 1.0f, sz);
            p.speedY = 0.18f + hy * 0.42f;
        }
        p.phase = hx * TWO_PI;
        data.particles.push_back(p);
    }
}
