#include "WorldGenerator.h"
#include "Noise.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

inline int worldToChunkCoord(float v, float halfSize, float chunkSize, int chunksPerSide) {
    int c = static_cast<int>(std::floor((v + halfSize) / chunkSize));
    return std::clamp(c, 0, chunksPerSide - 1);
}

void computeClosestPointOnOBB(const QVector3D& p, const OBBCollider& obb,
                              QVector3D& outClosest, QVector3D& outPushNormal, float& outPenetration, float radius) {
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

// Обшивочная доска корпуса (4 вершины с толщиной)
void appendPlankStrip(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                      const QVector3D& a0, const QVector3D& a1,
                      const QVector3D& b0, const QVector3D& b1,
                      const QVector3D& outNormal, float thickness,
                      const QVector3D& color, float matType = 1.0f) {
    QVector3D n = outNormal.normalized();
    QVector3D off = n * thickness;

    auto addQuad = [&](const QVector3D& p0, const QVector3D& p1, const QVector3D& p2, const QVector3D& p3,
                       const QVector3D& qn, const QVector3D& col) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        verts.push_back({p0, qn, col, QVector2D(matType, 0.0f)});
        verts.push_back({p1, qn, col, QVector2D(matType, 0.0f)});
        verts.push_back({p2, qn, col, QVector2D(matType, 0.0f)});
        verts.push_back({p3, qn, col, QVector2D(matType, 0.0f)});
        inds.insert(inds.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };

    addQuad(a0 + off, b0 + off, b1 + off, a1 + off,  n, color);
    addQuad(b0, a0, a1, b1, -n, color * 0.78f);

    QVector3D upEdge = QVector3D::crossProduct(b1 - a1, n).normalized();
    addQuad(a1, b1, b1 + off, a1 + off,  upEdge, color * 0.85f);
    addQuad(b0, a0, a0 + off, b0 + off, -upEdge, color * 0.85f);
}

void appendFaunaCone(std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                     const QVector3D& baseCenter, const QVector3D& tipPos,
                     float baseRadius, int sides,
                     const QVector3D& baseCol, const QVector3D& tipCol) {
    QVector3D axis = tipPos - baseCenter;
    float len = axis.length();
    if (len < 1e-4f) return;
    QVector3D dir = axis / len;

    QVector3D u = QVector3D::crossProduct(dir, QVector3D(0.0f, 1.0f, 0.0f));
    if (u.lengthSquared() < 1e-4f) u = QVector3D::crossProduct(dir, QVector3D(1.0f, 0.0f, 0.0f));
    u.normalize();
    QVector3D v = QVector3D::crossProduct(dir, u).normalized();

    uint32_t ringStart = static_cast<uint32_t>(verts.size());
    for (int s = 0; s <= sides; ++s) {
        float ang = (TWO_PI * s) / sides;
        QVector3D rad = u * std::cos(ang) + v * std::sin(ang);
        QVector3D n = (rad + dir * 0.25f).normalized();
        verts.push_back({baseCenter + rad * baseRadius, n, baseCol});
    }
    uint32_t tipIdx = static_cast<uint32_t>(verts.size());
    verts.push_back({tipPos, dir, tipCol});

    for (int s = 0; s < sides; ++s) {
        inds.insert(inds.end(), {ringStart + s, ringStart + s + 1, tipIdx});
    }
}

void appendFaunaSphere(std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                       const QVector3D& center, const QVector3D& radii,
                       int stacks, int slices, const QVector3D& color) {
    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    for (int st = 0; st <= stacks; ++st) {
        float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int sl = 0; sl <= slices; ++sl) {
            float theta = TWO_PI * static_cast<float>(sl) / static_cast<float>(slices);
            QVector3D n(sp * std::cos(theta), cp, sp * std::sin(theta));
            QVector3D p = center + QVector3D(n.x() * radii.x(), n.y() * radii.y(), n.z() * radii.z());
            verts.push_back({p, n, color});
        }
    }
    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            uint32_t i00 = baseIdx + st * (slices + 1) + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + (slices + 1);
            uint32_t i11 = i01 + 1;
            inds.insert(inds.end(), {i00, i01, i10, i10, i01, i11});
        }
    }
}

void appendFaunaTube(std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                     const std::vector<QVector3D>& spine, const std::vector<float>& radii,
                     int radialSegs, const QVector3D& colStart, const QVector3D& colEnd) {
    if (spine.size() < 2) return;
    uint32_t baseIdx = static_cast<uint32_t>(verts.size());

    for (size_t i = 0; i < spine.size(); ++i) {
        float t = static_cast<float>(i) / static_cast<float>(spine.size() - 1);
        QVector3D tang;
        if (i == 0) tang = spine[1] - spine[0];
        else if (i + 1 == spine.size()) tang = spine[i] - spine[i - 1];
        else tang = spine[i + 1] - spine[i - 1];
        tang.normalize();

        QVector3D u = QVector3D::crossProduct(tang, QVector3D(1.0f, 0.0f, 0.0f));
        if (u.lengthSquared() < 1e-4f) u = QVector3D::crossProduct(tang, QVector3D(0.0f, 1.0f, 0.0f));
        u.normalize();
        QVector3D v = QVector3D::crossProduct(tang, u).normalized();

        QVector3D col = ProcMath::mix(colStart, colEnd, t);
        float r = radii[i];
        for (int j = 0; j <= radialSegs; ++j) {
            float a = (TWO_PI * j) / radialSegs;
            QVector3D n = (u * std::cos(a) + v * std::sin(a)).normalized();
            verts.push_back({spine[i] + n * r, n, col});
        }
    }

    for (size_t i = 0; i + 1 < spine.size(); ++i) {
        for (int j = 0; j < radialSegs; ++j) {
            uint32_t i00 = baseIdx + static_cast<uint32_t>(i) * (radialSegs + 1) + j;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + (radialSegs + 1);
            uint32_t i11 = i01 + 1;
            inds.insert(inds.end(), {i00, i01, i10, i10, i01, i11});
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
    float queryR = probeRadius + margin;

    int minX = worldToChunkCoord(pos.x() - queryR, worldHalfSize, chunkSize, chunksPerSide);
    int maxX = worldToChunkCoord(pos.x() + queryR, worldHalfSize, chunkSize, chunksPerSide);
    int minZ = worldToChunkCoord(pos.z() - queryR, worldHalfSize, chunkSize, chunksPerSide);
    int maxZ = worldToChunkCoord(pos.z() + queryR, worldHalfSize, chunkSize, chunksPerSide);

    for (int cz = minZ; cz <= maxZ; ++cz) {
        for (int cx = minX; cx <= maxX; ++cx) {
            const auto& sList = cellSpheres[cz * chunksPerSide + cx];
            for (uint32_t id : sList) {
                const auto& s = spheres[id];
                QVector3D diff = pos - s.center;
                float safeDist = s.radius + queryR;
                float d2 = diff.lengthSquared();
                if (d2 < safeDist * safeDist && d2 > 1e-5f) {
                    float d = std::sqrt(d2);
                    float strength = (safeDist - d) / std::max(margin, 0.5f);
                    force += (diff / d) * (strength * strength * 12.0f);
                }
            }

            const auto& oList = cellObbs[cz * chunksPerSide + cx];
            for (uint32_t id : oList) {
                const auto& o = obbs[id];
                if ((pos - o.center).lengthSquared() > (o.boundingRadius + queryR) * (o.boundingRadius + queryR)) {
                    continue;
                }
                QVector3D closest, pushN;
                float pen = 0.0f;
                computeClosestPointOnOBB(pos, o, closest, pushN, pen, queryR);
                if (pen > 0.0f) {
                    float strength = pen / std::max(margin, 0.5f);
                    force += pushN * (strength * strength * 16.0f);
                }
            }
        }
    }

    float floorY = ProcMath::sampleTerrainHeight(pos.x(), pos.z(), seed);
    QVector3D terrainN = ProcMath::sampleTerrainNormal(pos.x(), pos.z(), seed);
    float signedDist = (pos.y() - floorY) * terrainN.y();
    if (signedDist < queryR) {
        float strength = (queryR - signedDist) / std::max(margin, 0.5f);
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
        force.setY(force.y() - ((queryR - distToSurf) / std::max(margin, 0.5f)) * 12.0f);
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

    for (int iter = 0; iter < 2; ++iter) {
        float r2 = pos.x() * pos.x() + pos.z() * pos.z();
        float maxR = ProcMath::PLAYABLE_HARD_RADIUS - radius;
        if (r2 > maxR * maxR) {
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
        float hCenter = ProcMath::sampleTerrainHeight(pos.x(), pos.z(), seed);
        QVector3D nCenter = ProcMath::sampleTerrainNormal(pos.x(), pos.z(), seed);
        float signedDist = (pos.y() - hCenter) * nCenter.y();
        if (signedDist < effFloorR) {
            float push = effFloorR - signedDist;
            pos += nCenter * push;
            slideVelocity(nCenter);
            collided = true;
        }

        const float probeOff = radius * 0.78f;
        const float dirs[4][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
        for (auto& d : dirs) {
            float px = pos.x() + d[0] * probeOff;
            float pz = pos.z() + d[1] * probeOff;
            float hp = ProcMath::sampleTerrainHeight(px, pz, seed);
            if (pos.y() < hp + radius * 0.45f) {
                QVector3D np = ProcMath::sampleTerrainNormal(px, pz, seed);
                QVector3D pushDir = (np + QVector3D(-d[0] * 0.6f, 0.15f, -d[1] * 0.6f)).normalized();
                float depthPen = std::min((hp + radius * 0.45f) - pos.y(), radius * 0.85f);
                pos += pushDir * (depthPen * 0.55f);
                slideVelocity(pushDir);
                collided = true;
            }
        }

        float hFinal = ProcMath::sampleTerrainHeight(pos.x(), pos.z(), seed);
        if (pos.y() < hFinal + effFloorR * 0.72f) {
            pos.setY(hFinal + effFloorR * 0.72f);
            slideVelocity(QVector3D(0.0f, 1.0f, 0.0f));
            collided = true;
        }

        int minX = worldToChunkCoord(pos.x() - radius, worldHalfSize, chunkSize, chunksPerSide);
        int maxX = worldToChunkCoord(pos.x() + radius, worldHalfSize, chunkSize, chunksPerSide);
        int minZ = worldToChunkCoord(pos.z() - radius, worldHalfSize, chunkSize, chunksPerSide);
        int maxZ = worldToChunkCoord(pos.z() + radius, worldHalfSize, chunkSize, chunksPerSide);

        for (int cz = minZ; cz <= maxZ; ++cz) {
            for (int cx = minX; cx <= maxX; ++cx) {
                const auto& sList = cellSpheres[cz * chunksPerSide + cx];
                for (uint32_t id : sList) {
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

                const auto& oList = cellObbs[cz * chunksPerSide + cx];
                for (uint32_t id : oList) {
                    const auto& o = obbs[id];
                    if ((pos - o.center).lengthSquared() > (o.boundingRadius + radius) * (o.boundingRadius + radius)) {
                        continue;
                    }
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
    return collided;
}

bool CollisionWorld::isPointClear(const QVector3D& pos, float radius) const {
    if (pos.x() * pos.x() + pos.z() * pos.z() >
        (ProcMath::PLAYABLE_HARD_RADIUS - radius) * (ProcMath::PLAYABLE_HARD_RADIUS - radius)) {
        return false;
    }
    if (pos.y() > ProcMath::WATER_SURFACE_Y - radius) return false;
    if (pos.y() < ProcMath::sampleTerrainHeight(pos.x(), pos.z(), seed) + radius) return false;

    int minX = worldToChunkCoord(pos.x() - radius, worldHalfSize, chunkSize, chunksPerSide);
    int maxX = worldToChunkCoord(pos.x() + radius, worldHalfSize, chunkSize, chunksPerSide);
    int minZ = worldToChunkCoord(pos.z() - radius, worldHalfSize, chunkSize, chunksPerSide);
    int maxZ = worldToChunkCoord(pos.z() + radius, worldHalfSize, chunkSize, chunksPerSide);

    for (int cz = minZ; cz <= maxZ; ++cz) {
        for (int cx = minX; cx <= maxX; ++cx) {
            for (uint32_t id : cellSpheres[cz * chunksPerSide + cx]) {
                const auto& s = spheres[id];
                float rSum = s.radius + radius;
                if ((pos - s.center).lengthSquared() < rSum * rSum) return false;
            }
            for (uint32_t id : cellObbs[cz * chunksPerSide + cx]) {
                const auto& o = obbs[id];
                if ((pos - o.center).lengthSquared() > (o.boundingRadius + radius) * (o.boundingRadius + radius)) continue;
                QVector3D closest, pushN;
                float pen = 0.0f;
                computeClosestPointOnOBB(pos, o, closest, pushN, pen, radius);
                if (pen > 0.0f) return false;
            }
        }
    }
    return true;
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

            wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
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
            bowAnchor.setY(ProcMath::sampleTerrainHeight(bowAnchor.x(), bowAnchor.z(), seed) - 0.2f);

            QVector3D sternAnchor(wx - f2.x() * 3.8f, 0.0f, wz - f2.z() * 3.8f);
            sternAnchor.setY(ProcMath::sampleTerrainHeight(sternAnchor.x(), sternAnchor.z(), seed) - 0.2f);

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
// ПРОЦЕДУРНЫЕ 3D-МОДЕЛИ ФАУНЫ (РЫБЫ, СКАТЫ-МАНТЫ, ГОРБАТЫЙ КИТ, УДИЛЬЩИК)
// ============================================================================

void WorldGenerator::generateFaunaMeshes(WorldData& data) {
    auto buildFish = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds, int rings, int segs, bool withFins) {
        for (int i = 0; i <= rings; ++i) {
            float t = static_cast<float>(i) / rings;
            float z = 0.65f - t * 1.40f;
            float profile = std::max(0.0f, std::sin(t * PI));
            float rx = 0.16f * std::pow(profile, 0.75f);
            float ry = 0.28f * std::pow(profile, 0.65f);

            QVector3D bodyCol = ProcMath::mix(QVector3D(0.98f, 0.72f, 0.18f), QVector3D(0.18f, 0.84f, 0.96f), t);

            for (int j = 0; j <= segs; ++j) {
                float a = (TWO_PI * j) / segs;
                float cx = std::cos(a), sy = std::sin(a);
                QVector3D p(cx * rx, sy * ry, z);
                QVector3D n;
                if (i == 0) {
                    n = QVector3D(0.0f, 0.0f, 1.0f);
                } else if (i == rings) {
                    n = QVector3D(0.0f, 0.0f, -1.0f);
                } else {
                    n = QVector3D(cx / std::max(rx, 0.02f), sy / std::max(ry, 0.02f), (0.5f - t) * 0.6f).normalized();
                }
                QVector3D col = (sy < -0.2f) ? QVector3D(0.94f, 0.96f, 0.98f) : bodyCol;
                verts.push_back({p, n, col});
            }
        }
        for (int i = 0; i < rings; ++i) {
            for (int j = 0; j < segs; ++j) {
                uint32_t i00 = i * (segs + 1) + j;
                uint32_t i10 = i00 + 1;
                uint32_t i01 = i00 + (segs + 1);
                uint32_t i11 = i01 + 1;
                inds.insert(inds.end(), {i00, i01, i10, i10, i01, i11});
            }
        }

        // Основание хвоста надёжно входит внутрь хвостового стебля (тело доходит до z = -0.75)
        uint32_t tb = static_cast<uint32_t>(verts.size());
        QVector3D tailCol(0.98f, 0.44f, 0.16f);
        verts.push_back({QVector3D(0.0f,  0.05f, -0.64f), QVector3D(1, 0, 0), tailCol});
        verts.push_back({QVector3D(0.0f, -0.05f, -0.64f), QVector3D(1, 0, 0), tailCol});
        verts.push_back({QVector3D(0.0f,  0.34f, -1.08f), QVector3D(1, 0, 0), tailCol});
        verts.push_back({QVector3D(0.0f, -0.34f, -1.08f), QVector3D(1, 0, 0), tailCol});
        verts.push_back({QVector3D(0.0f,  0.0f,  -0.90f), QVector3D(1, 0, 0), tailCol});
        inds.insert(inds.end(), {
                                    tb + 0, tb + 2, tb + 4,
                                    tb + 0, tb + 4, tb + 1,
                                    tb + 1, tb + 4, tb + 3
                                });

        if (withFins) {
            uint32_t db = static_cast<uint32_t>(verts.size());
            verts.push_back({QVector3D(0.0f, 0.24f,  0.15f), QVector3D(1, 0, 0), tailCol});
            verts.push_back({QVector3D(0.0f, 0.48f, -0.18f), QVector3D(1, 0, 0), tailCol});
            verts.push_back({QVector3D(0.0f, 0.16f, -0.38f), QVector3D(1, 0, 0), tailCol});
            inds.insert(inds.end(), {db, db + 1, db + 2});
        }
    };

    buildFish(data.fishLod0Verts, data.fishLod0Inds, 8, 8, true);
    buildFish(data.fishLod1Verts, data.fishLod1Inds, 4, 4, false);

    auto buildManta = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                         int spanSegs, int chordSegs, bool highLod) {
        const QVector3D dorsalDark(0.06f, 0.11f, 0.19f);
        const QVector3D dorsalMid(0.11f, 0.18f, 0.28f);
        const QVector3D chevronCol(0.38f, 0.50f, 0.60f);
        const QVector3D bellyLight(0.70f, 0.76f, 0.80f);
        const QVector3D bellyBorder(0.20f, 0.28f, 0.37f);
        const QVector3D eyeCol(0.02f, 0.03f, 0.05f);

        // Гарантированный обход против часовой стрелки (CCW) по направлению вершинных нормалей
        auto emitTriCCW = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
            const QVector3D& p0 = verts[i0].pos;
            const QVector3D& p1 = verts[i1].pos;
            const QVector3D& p2 = verts[i2].pos;
            QVector3D faceN = QVector3D::crossProduct(p1 - p0, p2 - p0);
            QVector3D avgN  = verts[i0].normal + verts[i1].normal + verts[i2].normal;
            if (QVector3D::dotProduct(faceN, avgN) >= 0.0f) {
                inds.insert(inds.end(), {i0, i1, i2});
            } else {
                inds.insert(inds.end(), {i0, i2, i1});
            }
        };

        auto appendTubeCCW = [&](const std::vector<QVector3D>& spine,
                                 const std::vector<float>& radii,
                                 int radialSegs,
                                 const QVector3D& topCol,
                                 const QVector3D& botCol,
                                 bool closeTip) {
            if (spine.size() < 2) return;
            const uint32_t baseIdx = static_cast<uint32_t>(verts.size());
            const int nPts = static_cast<int>(spine.size());

            for (int i = 0; i < nPts; ++i) {
                QVector3D tang;
                if (i == 0) tang = spine[1] - spine[0];
                else if (i + 1 == nPts) tang = spine[i] - spine[i - 1];
                else tang = spine[i + 1] - spine[i - 1];
                tang.normalize();

                QVector3D u = QVector3D::crossProduct(tang, QVector3D(0.0f, 1.0f, 0.0f));
                if (u.lengthSquared() < 1e-4f) {
                    u = QVector3D::crossProduct(tang, QVector3D(1.0f, 0.0f, 0.0f));
                }
                u.normalize();
                QVector3D v = QVector3D::crossProduct(tang, u).normalized();

                float r = radii[i];
                for (int j = 0; j < radialSegs; ++j) {
                    float a = (TWO_PI * j) / radialSegs;
                    QVector3D n = (u * std::cos(a) + v * std::sin(a)).normalized();
                    float topMix = ProcMath::smoothstep(-0.35f, 0.35f, n.y());
                    QVector3D col = ProcMath::mix(botCol, topCol, topMix);
                    verts.push_back({spine[i] + n * r, n, col});
                }
            }

            for (int i = 0; i + 1 < nPts; ++i) {
                uint32_t r0 = baseIdx + i * radialSegs;
                uint32_t r1 = baseIdx + (i + 1) * radialSegs;
                for (int j = 0; j < radialSegs; ++j) {
                    int jNext = (j + 1) % radialSegs;
                    emitTriCCW(r0 + j, r1 + j, r0 + jNext);
                    emitTriCCW(r0 + jNext, r1 + j, r1 + jNext);
                }
            }

            if (closeTip) {
                QVector3D tipDir = (spine.back() - spine[nPts - 2]).normalized();
                uint32_t tipIdx = static_cast<uint32_t>(verts.size());
                verts.push_back({spine.back() + tipDir * (radii.back() * 2.2f), tipDir, topCol});
                uint32_t lastR = baseIdx + (nPts - 1) * radialSegs;
                for (int j = 0; j < radialSegs; ++j) {
                    int jNext = (j + 1) % radialSegs;
                    emitTriCCW(lastR + j, tipIdx, lastR + jNext);
                }
            }
        };

        // 1. Крылья и тело ската (верхняя и нижняя поверхности с гладкими нормалями)
        auto addSurface = [&](float ySign) {
            const int cols = chordSegs + 1;
            const int rows = spanSegs + 1;
            std::vector<QVector3D> gridPos(rows * cols);
            std::vector<QVector3D> gridCol(rows * cols);

            for (int ix = 0; ix <= spanSegs; ++ix) {
                float u = (static_cast<float>(ix) / spanSegs) * 2.0f - 1.0f;
                float absU = std::abs(u);
                float x = u * 2.35f;

                // Плавный стреловидный передний край и изогнутый задний край
                float leadZ  = 0.78f - 1.18f * std::pow(absU, 1.35f);
                float trailZ = -1.15f + 0.68f * std::pow(absU, 0.85f) + 0.16f * std::sin(absU * PI);
                float spanThick = 0.13f * std::pow(1.0f - absU * 0.94f, 1.15f)
                                  + 0.12f * std::exp(-(absU * absU) / 0.075f);

                for (int iz = 0; iz <= chordSegs; ++iz) {
                    float v = static_cast<float>(iz) / chordSegs;
                    float z = ProcMath::mix(leadZ, trailZ, v);
                    float chordProf = std::sin(std::pow(ProcMath::clamp(v, 0.0f, 1.0f), 0.72f) * PI);
                    float yScale = (ySign > 0.0f) ? 1.0f : 0.65f;
                    float y = ySign * spanThick * chordProf * yScale;

                    QVector3D col;
                    if (ySign > 0.0f) {
                        // Спина: тёмно-синий градиент + характерные светлые плечевые пятна манты
                        float wingFade = ProcMath::smoothstep(0.15f, 0.95f, absU);
                        col = ProcMath::mix(dorsalMid, dorsalDark, wingFade * 0.75f + v * 0.25f);

                        float chevU = (absU - 0.25f) / 0.16f;
                        float chevV = (v - (0.20f + absU * 0.35f)) / 0.13f;
                        float chevron = std::exp(-(chevU * chevU + chevV * chevV));
                        chevron *= ProcMath::smoothstep(0.05f, 0.14f, absU)
                                   * (1.0f - ProcMath::smoothstep(0.42f, 0.58f, absU));
                        col = ProcMath::mix(col, chevronCol, chevron * 0.72f);
                    } else {
                        // Брюхо: мягкий светлый оттенок, тёмная окантовка крыльев и жаберные складки
                        float edgeBorder = ProcMath::smoothstep(0.68f, 0.98f, v) * 0.65f
                                           + ProcMath::smoothstep(0.62f, 0.98f, absU) * 0.55f;
                        col = ProcMath::mix(bellyLight, bellyBorder, ProcMath::clamp(edgeBorder, 0.0f, 0.85f));

                        float gillMask = ProcMath::smoothstep(0.05f, 0.11f, absU)
                                         * (1.0f - ProcMath::smoothstep(0.20f, 0.28f, absU))
                                         * ProcMath::smoothstep(0.15f, 0.23f, v)
                                         * (1.0f - ProcMath::smoothstep(0.42f, 0.50f, v));
                        float gillStripes = 0.5f + 0.5f * std::sin(v * 70.0f);
                        col *= (1.0f - 0.16f * gillMask * gillStripes);
                    }

                    gridPos[ix * cols + iz] = QVector3D(x, y, z);
                    gridCol[ix * cols + iz] = col;
                }
            }

            uint32_t base = static_cast<uint32_t>(verts.size());
            for (int ix = 0; ix <= spanSegs; ++ix) {
                int ixL = std::max(0, ix - 1);
                int ixR = std::min(spanSegs, ix + 1);
                for (int iz = 0; iz <= chordSegs; ++iz) {
                    int izF = std::max(0, iz - 1);
                    int izB = std::min(chordSegs, iz + 1);

                    QVector3D tX = gridPos[ixR * cols + iz] - gridPos[ixL * cols + iz];
                    QVector3D tZ = gridPos[ix * cols + izB] - gridPos[ix * cols + izF];
                    // tX вдоль +X, tZ вдоль -Z -> cross(tX, tZ) смотрит строго вверх (+Y)
                    QVector3D n = QVector3D::crossProduct(tX, tZ) * ySign;
                    if (n.lengthSquared() < 1e-6f) n = QVector3D(0.0f, ySign, 0.0f);
                    n.normalize();

                    verts.push_back({gridPos[ix * cols + iz], n, gridCol[ix * cols + iz]});
                }
            }

            for (int ix = 0; ix < spanSegs; ++ix) {
                for (int iz = 0; iz < chordSegs; ++iz) {
                    uint32_t i00 = base + ix * cols + iz;
                    uint32_t i10 = i00 + cols;
                    uint32_t i01 = i00 + 1;
                    uint32_t i11 = i10 + 1;
                    emitTriCCW(i00, i10, i01);
                    emitTriCCW(i10, i11, i01);
                }
            }
        };

        addSurface( 1.0f);
        addSurface(-1.0f);

        // 2. Головные плавники («рожки» манты по бокам рта)
        for (float side : {-1.0f, 1.0f}) {
            std::vector<QVector3D> lobeSpine = {
                QVector3D(side * 0.36f, -0.01f, 0.58f),
                QVector3D(side * 0.35f, -0.02f, 0.80f),
                QVector3D(side * 0.30f, -0.03f, 1.00f),
                QVector3D(side * 0.24f, -0.04f, 1.16f)
            };
            std::vector<float> lobeRadii = {0.068f, 0.056f, 0.038f, 0.018f};
            appendTubeCCW(lobeSpine, lobeRadii, highLod ? 6 : 4, dorsalDark, bellyBorder, true);
        }

        // 3. Небольшой спинной плавник у основания хвоста
        {
            uint32_t base = static_cast<uint32_t>(verts.size());
            QVector3D pFront(0.0f, 0.06f, -0.88f);
            QVector3D pPeak (0.0f, 0.22f, -1.16f);
            QVector3D pBack (0.0f, 0.02f, -1.24f);
            QVector3D pLeft (-0.035f, 0.05f, -1.06f);
            QVector3D pRight( 0.035f, 0.05f, -1.06f);

            verts.push_back({pFront, QVector3D(0.0f, 0.5f, 0.85f).normalized(), dorsalMid});
            verts.push_back({pPeak,  QVector3D(0.0f, 0.9f, -0.3f).normalized(), dorsalDark});
            verts.push_back({pBack,  QVector3D(0.0f, 0.3f, -0.95f).normalized(), dorsalDark});
            verts.push_back({pLeft,  QVector3D(-1.0f, 0.2f, 0.0f).normalized(), dorsalDark});
            verts.push_back({pRight, QVector3D( 1.0f, 0.2f, 0.0f).normalized(), dorsalDark});

            emitTriCCW(base + 0, base + 3, base + 1);
            emitTriCCW(base + 3, base + 2, base + 1);
            emitTriCCW(base + 0, base + 1, base + 4);
            emitTriCCW(base + 4, base + 1, base + 2);
        }

        // 4. Объёмный 3D-хвост (плавно сужающийся кнут с правильными нормалями)
        {
            const int tailSegs = highLod ? 8 : 4;
            std::vector<QVector3D> tailSpine;
            std::vector<float> tailRadii;
            tailSpine.reserve(tailSegs + 1);
            tailRadii.reserve(tailSegs + 1);

            for (int k = 0; k <= tailSegs; ++k) {
                float t = static_cast<float>(k) / static_cast<float>(tailSegs);
                float z = ProcMath::mix(-0.98f, -3.12f, t);
                float y = -0.02f * std::sin(t * PI * 0.85f);
                float r = 0.052f * std::pow(1.0f - t, 1.35f) + 0.006f;
                tailSpine.emplace_back(0.0f, y, z);
                tailRadii.push_back(r);
            }
            appendTubeCCW(tailSpine, tailRadii, highLod ? 6 : 4, dorsalDark, bellyBorder, true);
        }

        // 5. Глаза у основания головных плавников (на LOD0)
        if (highLod) {
            for (float side : {-1.0f, 1.0f}) {
                QVector3D c(side * 0.41f, 0.025f, 0.66f);
                uint32_t eBase = static_cast<uint32_t>(verts.size());
                constexpr int stN = 4, slN = 6;
                for (int st = 0; st <= stN; ++st) {
                    float phi = PI * static_cast<float>(st) / stN;
                    float sp = std::sin(phi), cp = std::cos(phi);
                    for (int sl = 0; sl <= slN; ++sl) {
                        float th = TWO_PI * static_cast<float>(sl) / slN;
                        QVector3D n(sp * std::cos(th), cp, sp * std::sin(th));
                        verts.push_back({c + n * 0.032f, n, eyeCol});
                    }
                }
                for (int st = 0; st < stN; ++st) {
                    for (int sl = 0; sl < slN; ++sl) {
                        uint32_t i00 = eBase + st * (slN + 1) + sl;
                        uint32_t i10 = i00 + 1;
                        uint32_t i01 = i00 + (slN + 1);
                        uint32_t i11 = i01 + 1;
                        if (st > 0) emitTriCCW(i00, i01, i10);
                        if (st + 1 < stN) emitTriCCW(i10, i01, i11);
                    }
                }
            }
        }
    };

    buildManta(data.mantaLod0Verts, data.mantaLod0Inds, 24, 12, true);
    buildManta(data.mantaLod1Verts, data.mantaLod1Inds, 10, 5, false);
}

void WorldGenerator::generateWhaleMeshes(WorldData& data) {
    auto buildWhale = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                         int bodySegs, int radialSegs, bool highLod) {
        const QVector3D backCol(0.10f, 0.16f, 0.25f);
        const QVector3D flankCol(0.16f, 0.23f, 0.33f);
        const QVector3D bellyCol(0.54f, 0.62f, 0.68f);
        const QVector3D finVentralCol(0.24f, 0.31f, 0.39f);
        const QVector3D mouthDark(0.05f, 0.04f, 0.07f);
        const QVector3D baleenCol(0.36f, 0.38f, 0.41f);
        const QVector3D eyeSclera(0.48f, 0.54f, 0.59f);
        const QVector3D eyePupil(0.02f, 0.025f, 0.04f);

        auto emitTriCCW = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
            const QVector3D& p0 = verts[i0].pos;
            const QVector3D& p1 = verts[i1].pos;
            const QVector3D& p2 = verts[i2].pos;
            QVector3D faceN = QVector3D::crossProduct(p1 - p0, p2 - p0);
            QVector3D avgN  = verts[i0].normal + verts[i1].normal + verts[i2].normal;
            if (QVector3D::dotProduct(faceN, avgN) >= 0.0f) {
                inds.insert(inds.end(), {i0, i1, i2});
            } else {
                inds.insert(inds.end(), {i0, i2, i1});
            }
        };

        auto appendSphereCCW = [&](const QVector3D& center, const QVector3D& radii,
                                   int stacks, int slices, const QVector3D& col) {
            uint32_t base = static_cast<uint32_t>(verts.size());
            for (int st = 0; st <= stacks; ++st) {
                float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
                float sp = std::sin(phi), cp = std::cos(phi);
                for (int sl = 0; sl <= slices; ++sl) {
                    float th = TWO_PI * static_cast<float>(sl) / static_cast<float>(slices);
                    QVector3D n(sp * std::cos(th), cp, sp * std::sin(th));
                    QVector3D p = center + QVector3D(n.x() * radii.x(), n.y() * radii.y(), n.z() * radii.z());
                    QVector3D rn(n.x() / std::max(radii.x(), 0.01f),
                                 n.y() / std::max(radii.y(), 0.01f),
                                 n.z() / std::max(radii.z(), 0.01f));
                    rn.normalize();
                    verts.push_back({p, rn, col});
                }
            }
            for (int st = 0; st < stacks; ++st) {
                for (int sl = 0; sl < slices; ++sl) {
                    uint32_t i00 = base + st * (slices + 1) + sl;
                    uint32_t i10 = i00 + 1;
                    uint32_t i01 = i00 + (slices + 1);
                    uint32_t i11 = i01 + 1;
                    if (st > 0) emitTriCCW(i00, i01, i10);
                    if (st + 1 < stacks) emitTriCCW(i10, i01, i11);
                }
            }
        };

        auto evalBodySurface = [&](float t, float ang, QVector3D& outPos, QVector3D& outCol) {
            float z = 4.95f - t * 10.05f;
            float rx = 0.0f, ryTop = 0.0f, ryBot = 0.0f;

            if (t < 0.34f) {
                float u = t / 0.34f;
                float s = std::sin(u * PI * 0.5f);
                rx    = 1.24f * std::pow(s, 0.80f);
                ryTop = 0.84f * std::pow(s, 0.96f);
                ryBot = 1.12f * std::pow(s, 0.78f);
            } else {
                float u = (t - 0.34f) / 0.66f;
                rx    = 1.24f * std::pow(1.0f - u, 1.55f) + 0.09f * (1.0f - u);
                ryTop = 0.84f * std::pow(1.0f - u, 1.18f) + 0.14f * (1.0f - u);
                ryBot = 1.12f * std::pow(1.0f - u, 1.38f) + 0.12f * (1.0f - u);
            }

            float ca = std::cos(ang);
            float sa = std::sin(ang);
            float ry = (sa >= 0.0f) ? ryTop : ryBot;

            float centerY = 0.08f * (1.0f - ProcMath::smoothstep(0.0f, 0.42f, t));
            if (sa > 0.15f && t < 0.30f) {
                ry *= (1.0f - 0.10f * (1.0f - t / 0.30f) * sa);
            }

            float mt = ProcMath::clamp(t / 0.31f, 0.0f, 1.0f);
            float mouthSa = -0.05f + 0.11f * std::sin(mt * PI) - 0.25f * mt * mt;
            float mouthFade = (1.0f - ProcMath::smoothstep(0.24f, 0.315f, t))
                              * ProcMath::smoothstep(0.004f, 0.026f, t);
            float dMouth = std::abs(sa - mouthSa);
            float mouthGroove = (1.0f - ProcMath::smoothstep(0.015f, 0.145f, dMouth)) * mouthFade;

            float lipUpper = (1.0f - ProcMath::smoothstep(0.08f, 0.22f, std::abs(sa - (mouthSa + 0.14f)))) * mouthFade;
            float lipLower = (1.0f - ProcMath::smoothstep(0.08f, 0.22f, std::abs(sa - (mouthSa - 0.14f)))) * mouthFade;
            float radialMod = 1.0f - 0.26f * mouthGroove + 0.035f * lipUpper + 0.030f * lipLower;

            float bellyMask = ProcMath::smoothstep(-0.18f, -0.58f, sa)
                              * (1.0f - ProcMath::smoothstep(0.54f, 0.84f, t));
            float pleatWave = (bellyMask > 0.15f) ? std::sin(ang * 18.0f) : 0.0f;
            float pleatRelief = 1.0f + 0.016f * pleatWave * bellyMask;

            outPos = QVector3D(ca * rx * radialMod * pleatRelief,
                               centerY + sa * ry * radialMod * pleatRelief,
                               z);

            float flankBlend = ProcMath::smoothstep(0.35f, -0.25f, sa);
            QVector3D col = ProcMath::mix(backCol, flankCol, flankBlend * 0.75f);
            float pleatShade = 0.88f + 0.12f * pleatWave;
            col = ProcMath::mix(col, bellyCol * pleatShade, bellyMask);

            if (mouthGroove > 0.01f) {
                float baleenMask = ProcMath::smoothstep(0.0f, 0.09f, sa - mouthSa) * mouthGroove;
                QVector3D cleftCol = ProcMath::mix(mouthDark, baleenCol, baleenMask * 0.72f);
                col = ProcMath::mix(col, cleftCol, ProcMath::smoothstep(0.18f, 0.78f, mouthGroove));
            }
            outCol = col;
        };

        std::vector<float> tRings;
        tRings.reserve(bodySegs + 3);
        tRings.push_back(0.010f);
        tRings.push_back(0.028f);
        for (int i = 1; i < bodySegs; ++i) {
            float u = static_cast<float>(i) / static_cast<float>(bodySegs);
            tRings.push_back(0.045f + u * 0.935f);
        }
        tRings.push_back(0.990f);

        const int numRings = static_cast<int>(tRings.size());
        std::vector<QVector3D> gridPos(numRings * radialSegs);
        std::vector<QVector3D> gridCol(numRings * radialSegs);

        for (int r = 0; r < numRings; ++r) {
            for (int j = 0; j < radialSegs; ++j) {
                float ang = (TWO_PI * j) / radialSegs;
                evalBodySurface(tRings[r], ang, gridPos[r * radialSegs + j], gridCol[r * radialSegs + j]);
            }
        }

        const QVector3D noseTipPos(0.0f, 0.075f, 5.02f);
        const QVector3D tailTipPos(0.0f, 0.0f, -5.22f);

        uint32_t bodyBase = static_cast<uint32_t>(verts.size());
        for (int r = 0; r < numRings; ++r) {
            for (int j = 0; j < radialSegs; ++j) {
                const QVector3D& pPrev = (r > 0) ? gridPos[(r - 1) * radialSegs + j] : noseTipPos;
                const QVector3D& pNext = (r + 1 < numRings) ? gridPos[(r + 1) * radialSegs + j] : tailTipPos;
                int jPrev = (j - 1 + radialSegs) % radialSegs;
                int jNext = (j + 1) % radialSegs;
                QVector3D tZ = pNext - pPrev;
                QVector3D tA = gridPos[r * radialSegs + jNext] - gridPos[r * radialSegs + jPrev];
                QVector3D n = QVector3D::crossProduct(tZ, tA);
                if (n.lengthSquared() < 1e-6f) n = QVector3D(0.0f, 1.0f, 0.0f);
                n.normalize();
                verts.push_back({gridPos[r * radialSegs + j], n, gridCol[r * radialSegs + j]});
            }
        }

        uint32_t noseIdx = static_cast<uint32_t>(verts.size());
        verts.push_back({noseTipPos, QVector3D(0.0f, 0.08f, 1.0f).normalized(), backCol * 1.05f});
        uint32_t tailIdx = static_cast<uint32_t>(verts.size());
        verts.push_back({tailTipPos, QVector3D(0.0f, 0.0f, -1.0f), backCol});

        for (int j = 0; j < radialSegs; ++j) {
            int jNext = (j + 1) % radialSegs;
            emitTriCCW(noseIdx, bodyBase + j, bodyBase + jNext);
            uint32_t lastRing = bodyBase + (numRings - 1) * radialSegs;
            emitTriCCW(tailIdx, lastRing + jNext, lastRing + j);
        }
        for (int r = 0; r + 1 < numRings; ++r) {
            uint32_t r0 = bodyBase + r * radialSegs;
            uint32_t r1 = bodyBase + (r + 1) * radialSegs;
            for (int j = 0; j < radialSegs; ++j) {
                int jNext = (j + 1) % radialSegs;
                emitTriCCW(r0 + j, r1 + j, r0 + jNext);
                emitTriCCW(r0 + jNext, r1 + j, r1 + jNext);
            }
        }

        if (highLod) {
            for (float side : {-1.0f, 1.0f}) {
                for (int k = 0; k < 10; ++k) {
                    float tB = 0.035f + static_cast<float>(k) * 0.022f;
                    float mt = ProcMath::clamp(tB / 0.31f, 0.0f, 1.0f);
                    float mouthSa = -0.05f + 0.11f * std::sin(mt * PI) - 0.25f * mt * mt;
                    float aTop = (side > 0.0f) ? std::asin(ProcMath::clamp(mouthSa + 0.06f, -0.95f, 0.95f))
                                               : (PI - std::asin(ProcMath::clamp(mouthSa + 0.06f, -0.95f, 0.95f)));
                    float aBot = (side > 0.0f) ? std::asin(ProcMath::clamp(mouthSa - 0.06f, -0.95f, 0.95f))
                                               : (PI - std::asin(ProcMath::clamp(mouthSa - 0.06f, -0.95f, 0.95f)));
                    QVector3D pTop, pBot, dummyCol;
                    evalBodySurface(tB, aTop, pTop, dummyCol);
                    evalBodySurface(tB, aBot, pBot, dummyCol);
                    pTop.setX(pTop.x() * 0.96f);
                    pBot.setX(pBot.x() * 0.92f);
                    appendFaunaCone(verts, inds, pTop, pBot, 0.032f, 4, baleenCol, mouthDark * 1.4f);
                }
            }
        }

        for (float side : {-1.0f, 1.0f}) {
            float angEye = (side > 0.0f) ? -0.10f : (PI + 0.10f);
            QVector3D skinP, dummyCol;
            evalBodySurface(0.305f, angEye, skinP, dummyCol);
            QVector3D outDir(side * 0.94f, -0.03f, 0.34f);
            outDir.normalize();

            appendSphereCCW(skinP - outDir * 0.025f, QVector3D(0.115f, 0.100f, 0.165f),
                            highLod ? 6 : 4, highLod ? 8 : 5, backCol * 0.72f);
            appendSphereCCW(skinP + outDir * 0.028f, QVector3D(0.082f, 0.074f, 0.094f),
                            highLod ? 7 : 4, highLod ? 9 : 6, eyeSclera);
            appendSphereCCW(skinP + outDir * 0.076f, QVector3D(0.052f, 0.050f, 0.056f),
                            highLod ? 5 : 3, highLod ? 7 : 5, eyePupil);
        }

        struct FinSection {
            QVector3D le;
            QVector3D te;
            float thick;
            QVector3D topCol;
            QVector3D botCol;
        };

        auto appendAirfoilFin = [&](const std::vector<FinSection>& secs,
                                    const QVector3D& tipPos,
                                    const QVector3D& upAxis,
                                    int chordSegs) {
            const int ringPts = chordSegs * 2;
            const int nSec = static_cast<int>(secs.size());
            std::vector<QVector3D> pts(nSec * ringPts);
            std::vector<QVector3D> cols(nSec * ringPts);
            std::vector<QVector3D> centers(nSec);

            for (int k = 0; k < nSec; ++k) {
                const auto& s = secs[k];
                centers[k] = 0.5f * (s.le + s.te);
                for (int j = 0; j < ringPts; ++j) {
                    bool isTop = (j <= chordSegs);
                    float c = isTop
                                  ? (static_cast<float>(j) / static_cast<float>(chordSegs))
                                  : (static_cast<float>(ringPts - j) / static_cast<float>(chordSegs));
                    float profile = std::sin(std::pow(ProcMath::clamp(c, 0.0f, 1.0f), 0.65f) * PI);
                    float yOff = (isTop ? 1.0f : -1.0f) * s.thick * profile;
                    pts[k * ringPts + j] = ProcMath::mix(s.le, s.te, c) + upAxis * yOff;
                    cols[k * ringPts + j] = (j == 0 || j == chordSegs)
                                                ? ProcMath::mix(s.topCol, s.botCol, 0.35f)
                                                : (isTop ? s.topCol : s.botCol);
                }
            }

            uint32_t fBase = static_cast<uint32_t>(verts.size());
            for (int k = 0; k < nSec; ++k) {
                for (int j = 0; j < ringPts; ++j) {
                    int jPrev = (j - 1 + ringPts) % ringPts;
                    int jNext = (j + 1) % ringPts;
                    const QVector3D& pPrev = (k > 0) ? pts[(k - 1) * ringPts + j] : pts[k * ringPts + j];
                    const QVector3D& pNext = (k + 1 < nSec) ? pts[(k + 1) * ringPts + j] : tipPos;

                    QVector3D tSpan = pNext - pPrev;
                    QVector3D tChord = pts[k * ringPts + jNext] - pts[k * ringPts + jPrev];
                    QVector3D n = QVector3D::crossProduct(tSpan, tChord);
                    QVector3D radialOut = pts[k * ringPts + j] - centers[k];
                    if (n.lengthSquared() < 1e-6f) n = radialOut;
                    if (QVector3D::dotProduct(n, radialOut) < 0.0f) n = -n;
                    n.normalize();

                    verts.push_back({pts[k * ringPts + j], n, cols[k * ringPts + j]});
                }
            }

            QVector3D tipDir = (tipPos - centers.back()).normalized();
            uint32_t tipIdx = static_cast<uint32_t>(verts.size());
            verts.push_back({tipPos, tipDir, ProcMath::mix(secs.back().topCol, secs.back().botCol, 0.45f)});

            for (int k = 0; k + 1 < nSec; ++k) {
                uint32_t r0 = fBase + k * ringPts;
                uint32_t r1 = fBase + (k + 1) * ringPts;
                for (int j = 0; j < ringPts; ++j) {
                    int jNext = (j + 1) % ringPts;
                    emitTriCCW(r0 + j, r1 + j, r0 + jNext);
                    emitTriCCW(r0 + jNext, r1 + j, r1 + jNext);
                }
            }
            uint32_t lastR = fBase + (nSec - 1) * ringPts;
            for (int j = 0; j < ringPts; ++j) {
                int jNext = (j + 1) % ringPts;
                emitTriCCW(lastR + j, tipIdx, lastR + jNext);
            }
        };

        const int finChordSegs = highLod ? 5 : 3;

        for (float s : {-1.0f, 1.0f}) {
            std::vector<FinSection> pec = {
                {QVector3D(s * 0.88f, -0.34f,  1.45f), QVector3D(s * 0.86f, -0.40f,  0.32f), 0.15f, backCol, finVentralCol},
                {QVector3D(s * 2.02f, -0.48f,  1.05f), QVector3D(s * 1.96f, -0.52f,  0.00f), 0.11f, backCol * 1.05f, finVentralCol * 1.06f},
                {QVector3D(s * 3.12f, -0.58f,  0.36f), QVector3D(s * 3.00f, -0.60f, -0.42f), 0.075f, backCol * 1.12f, finVentralCol * 1.12f},
                {QVector3D(s * 3.92f, -0.64f, -0.22f), QVector3D(s * 3.82f, -0.65f, -0.68f), 0.040f, backCol * 1.15f, finVentralCol * 1.15f}
            };
            appendAirfoilFin(pec, QVector3D(s * 4.32f, -0.67f, -0.78f), QVector3D(0.0f, 1.0f, 0.0f), finChordSegs);
        }

        for (float s : {-1.0f, 1.0f}) {
            std::vector<FinSection> fluke = {
                {QVector3D(0.0f,      0.00f, -4.62f), QVector3D(0.0f,      0.00f, -5.20f), 0.13f, backCol, finVentralCol * 0.90f},
                {QVector3D(s * 0.95f, 0.02f, -4.92f), QVector3D(s * 0.88f, 0.02f, -5.78f), 0.088f, backCol, finVentralCol * 0.95f},
                {QVector3D(s * 2.10f, 0.03f, -5.48f), QVector3D(s * 1.98f, 0.02f, -6.24f), 0.052f, backCol * 1.05f, finVentralCol},
                {QVector3D(s * 2.68f, 0.02f, -5.98f), QVector3D(s * 2.58f, 0.02f, -6.46f), 0.026f, backCol * 1.08f, finVentralCol}
            };
            appendAirfoilFin(fluke, QVector3D(s * 2.92f, 0.01f, -6.62f), QVector3D(0.0f, 1.0f, 0.0f), finChordSegs);
        }

        std::vector<FinSection> dorsal = {
            {QVector3D(0.0f, 0.42f, -1.05f), QVector3D(0.0f, 0.32f, -2.35f), 0.12f, backCol, backCol},
            {QVector3D(0.0f, 0.78f, -1.65f), QVector3D(0.0f, 0.68f, -2.38f), 0.072f, backCol, backCol},
            {QVector3D(0.0f, 1.04f, -2.08f), QVector3D(0.0f, 0.96f, -2.48f), 0.038f, backCol * 1.06f, backCol * 1.06f}
        };
        appendAirfoilFin(dorsal, QVector3D(0.0f, 1.18f, -2.58f), QVector3D(1.0f, 0.0f, 0.0f), highLod ? 4 : 3);
    };

    buildWhale(data.whaleLod0Verts, data.whaleLod0Inds, 28, 20, true);
    buildWhale(data.whaleLod1Verts, data.whaleLod1Inds, 12, 10, false);
}

void WorldGenerator::generateAnglerMeshes(WorldData& data) {
    auto buildAngler = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                          int bodySegs, int radialSegs, bool highLod) {
        QVector3D skinDark(0.11f, 0.09f, 0.12f);
        QVector3D skinMottled(0.20f, 0.15f, 0.16f);
        QVector3D mouthDark(0.06f, 0.02f, 0.04f);
        QVector3D toothBase(0.76f, 0.72f, 0.66f);
        QVector3D toothTip(0.98f, 0.96f, 0.90f);
        QVector3D finCol(0.16f, 0.13f, 0.18f);
        QVector3D lureGlow(0.85f, 2.45f, 2.25f);
        QVector3D eyeGlow(1.38f, 1.56f, 0.72f);

        for (int i = 0; i <= bodySegs; ++i) {
            float t = static_cast<float>(i) / bodySegs;
            float z = 0.72f - t * 2.08f;

            float env = std::max(0.0f, std::sin(std::pow(t, 0.52f) * PI));
            float rx = 0.08f + 0.70f * std::pow(env, 0.85f) * (1.0f - 0.35f * t);
            float ryUp = 0.08f + 0.56f * std::pow(env, 0.80f);
            float ryDown = 0.08f + 0.74f * std::pow(env, 0.75f) * (1.0f - 0.28f * t);

            for (int j = 0; j <= radialSegs; ++j) {
                float a = (TWO_PI * j) / radialSegs;
                float ca = std::cos(a), sa = std::sin(a);
                float ry = (sa >= 0.0f) ? ryUp : ryDown;

                float zMod = z;
                QVector3D col = ProcMath::mix(skinMottled, skinDark, sa * 0.5f + 0.5f);
                if (t < 0.22f && sa < 0.15f && sa > -0.78f) {
                    zMod -= (0.22f - t) * 1.15f;
                    col = mouthDark;
                }

                float bump = 1.0f + 0.05f * std::sin(std::abs(ca) * 5.0f + sa * 3.0f + t * 14.0f);
                QVector3D p(ca * rx * bump, sa * ry * bump, zMod);
                QVector3D n(ca / std::max(rx, 0.05f), sa / std::max(ry, 0.05f), (0.35f - t) * 0.45f);
                n.normalize();
                verts.push_back({p, n, col});
            }
        }

        for (int i = 0; i < bodySegs; ++i) {
            for (int j = 0; j < radialSegs; ++j) {
                uint32_t i00 = i * (radialSegs + 1) + j;
                uint32_t i10 = i00 + 1;
                uint32_t i01 = i00 + (radialSegs + 1);
                uint32_t i11 = i01 + 1;
                inds.insert(inds.end(), {i00, i01, i10, i10, i01, i11});
            }
        }

        int jawPts = highLod ? 9 : 5;
        std::vector<QVector3D> jawSpine(jawPts);
        std::vector<float> jawRadii(jawPts);
        for (int k = 0; k < jawPts; ++k) {
            float u = (static_cast<float>(k) / (jawPts - 1)) * 2.0f - 1.0f;
            float x = std::sin(u * PI * 0.5f) * 0.58f;
            float z = 0.24f + std::cos(u * PI * 0.5f) * 0.78f;
            float y = -0.50f + std::cos(u * PI * 0.5f) * 0.24f;
            jawSpine[k] = QVector3D(x, y, z);
            jawRadii[k] = 0.14f - std::abs(u) * 0.03f;
        }
        appendFaunaTube(verts, inds, jawSpine, jawRadii, highLod ? 7 : 5, skinMottled, skinMottled);

        int lowerTeeth = highLod ? 11 : 5;
        for (int k = 0; k < lowerTeeth; ++k) {
            float u = (static_cast<float>(k) / (lowerTeeth - 1)) * 2.0f - 1.0f;
            float x = std::sin(u * PI * 0.46f) * 0.54f;
            float z = 0.28f + std::cos(u * PI * 0.46f) * 0.73f;
            float y = -0.44f + std::cos(u * PI * 0.46f) * 0.22f;
            float toothLen = (0.30f + 0.16f * (1.0f - std::abs(u))) * ((k % 2 == 0) ? 1.0f : 0.72f);

            QVector3D baseP(x, y + 0.05f, z);
            QVector3D tipP = baseP + QVector3D(-x * 0.22f, toothLen, -0.14f);
            appendFaunaCone(verts, inds, baseP, tipP, 0.042f, highLod ? 5 : 4, toothBase, toothTip);
        }

        int upperTeeth = highLod ? 9 : 5;
        for (int k = 0; k < upperTeeth; ++k) {
            float u = (static_cast<float>(k) / (upperTeeth - 1)) * 2.0f - 1.0f;
            float x = std::sin(u * PI * 0.44f) * 0.48f;
            float z = 0.34f + std::cos(u * PI * 0.44f) * 0.46f;
            float y = 0.18f - std::abs(u) * 0.14f;
            float toothLen = 0.22f + 0.12f * (1.0f - std::abs(u));

            QVector3D baseP(x, y, z);
            QVector3D tipP = baseP + QVector3D(-x * 0.18f, -toothLen, -0.10f);
            appendFaunaCone(verts, inds, baseP, tipP, 0.036f, highLod ? 5 : 4, toothBase, toothTip);
        }

        QVector3D pupilCol(0.02f, 0.02f, 0.03f);
        for (float side : {-1.0f, 1.0f}) {
            QVector3D socketCenter(side * 0.49f, 0.33f, 0.49f);
            appendFaunaSphere(verts, inds, socketCenter,
                              QVector3D(0.112f, 0.106f, 0.106f),
                              highLod ? 6 : 4, highLod ? 8 : 5, skinDark);

            QVector3D eyeCenter(side * 0.52f, 0.35f, 0.53f);
            appendFaunaSphere(verts, inds, eyeCenter,
                              QVector3D(0.095f, 0.095f, 0.095f),
                              highLod ? 7 : 4, highLod ? 9 : 6, eyeGlow);

            if (highLod) {
                QVector3D lookDir = QVector3D(side * 0.56f, 0.37f, 0.74f).normalized();
                appendFaunaSphere(verts, inds, eyeCenter + lookDir * 0.070f,
                                  QVector3D(0.036f, 0.036f, 0.036f),
                                  5, 7, pupilCol);
            }
        }

        std::vector<QVector3D> stalk = {
            QVector3D(0.0f, 0.48f, 0.22f),
            QVector3D(0.0f, 0.88f, 0.42f),
            QVector3D(0.0f, 1.12f, 0.78f),
            QVector3D(0.0f, 1.06f, 1.14f),
            QVector3D(0.0f, 0.86f, 1.38f)
        };
        std::vector<float> stalkR = {0.055f, 0.040f, 0.030f, 0.024f, 0.020f};
        appendFaunaTube(verts, inds, stalk, stalkR, highLod ? 6 : 4, skinDark, QVector3D(0.35f, 0.85f, 0.82f));

        appendFaunaSphere(verts, inds, QVector3D(0.0f, 0.84f, 1.38f),
                          QVector3D(0.14f, 0.17f, 0.14f), highLod ? 8 : 5, highLod ? 10 : 6, lureGlow);

        if (highLod) {
            for (int f = 0; f < 4; ++f) {
                float a = (TWO_PI * f) / 4.0f + 0.4f;
                QVector3D bP(std::cos(a) * 0.08f, 0.72f, 1.38f + std::sin(a) * 0.08f);
                QVector3D tP(std::cos(a) * 0.16f, 0.56f, 1.38f + std::sin(a) * 0.16f);
                appendFaunaCone(verts, inds, bP, tP, 0.022f, 4, lureGlow, lureGlow);
            }
        }

        auto addFinQuad = [&](const QVector3D& p0, const QVector3D& p1, const QVector3D& p2, const QVector3D& p3,
                              const QVector3D& n, const QVector3D& col) {
            uint32_t b = static_cast<uint32_t>(verts.size());
            verts.push_back({p0, n, col});
            verts.push_back({p1, n, col});
            verts.push_back({p2, n, col});
            verts.push_back({p3, n, col});
            inds.insert(inds.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
        };

        addFinQuad(QVector3D(0.0f,  0.14f, -1.28f),
                   QVector3D(0.0f,  0.56f, -1.88f),
                   QVector3D(0.0f, -0.52f, -1.88f),
                   QVector3D(0.0f, -0.14f, -1.28f),
                   QVector3D(1.0f, 0.0f, 0.0f), finCol);

        addFinQuad(QVector3D(0.0f, 0.36f, -0.35f),
                   QVector3D(0.0f, 0.84f, -0.85f),
                   QVector3D(0.0f, 0.45f, -1.18f),
                   QVector3D(0.0f, 0.18f, -1.05f),
                   QVector3D(1.0f, 0.0f, 0.0f), finCol);

        for (float s : {-1.0f, 1.0f}) {
            addFinQuad(QVector3D(s * 0.56f, -0.10f,  0.02f),
                       QVector3D(s * 1.22f,  0.06f, -0.18f),
                       QVector3D(s * 1.18f, -0.28f, -0.52f),
                       QVector3D(s * 0.52f, -0.24f, -0.32f),
                       QVector3D(0.0f, 1.0f, 0.0f), finCol * 1.15f);
        }
    };

    buildAngler(data.anglerLod0Verts, data.anglerLod0Inds, 14, 12, true);
    buildAngler(data.anglerLod1Verts, data.anglerLod1Inds, 7, 7, false);
}

// ============================================================================
// ЗАСЕЛЕНИЕ ФАУНОЙ, ОБЪЁМНЫЕ СОЛНЕЧНЫЕ ЛУЧИ И ЗАВЕСЫ ПУЗЫРЬКОВ
// ============================================================================

void WorldGenerator::generateFaunaAgents(WorldData& data, uint32_t seed) {
    const QVector3D schoolTints[] = {
        {1.12f, 0.96f, 0.80f},
        {0.56f, 1.14f, 1.24f},
        {1.24f, 0.56f, 0.74f},
        {0.76f, 1.18f, 0.66f},
        {1.20f, 0.92f, 0.36f}
    };

    constexpr int SCHOOL_COUNT    = 34;
    constexpr int FISH_PER_SCHOOL = 26;
    data.fishAgents.reserve(SCHOOL_COUNT * FISH_PER_SCHOOL);

    for (int sc = 0; sc < SCHOOL_COUNT; ++sc) {
        float sx, sz;
        if (sc < 4) {
            float a = sc * 1.57f + 0.35f;
            sx = std::cos(a) * 28.0f;
            sz = 10.0f + std::sin(a) * 28.0f;
        } else {
            float a = ProcMath::hash3(sc, 1, 2, seed + 811u) * TWO_PI;
            float r = 55.0f + ProcMath::hash3(sc, 3, 4, seed + 811u) * 390.0f;
            sx = std::cos(a) * r;
            sz = std::sin(a) * r;
        }
        float floorY = ProcMath::sampleTerrainHeight(sx, sz, seed);
        float safeFloor = std::max(floorY, -24.0f);
        float baseY = ProcMath::clamp(safeFloor + 7.0f + ProcMath::hash3(sc, 5, 6, seed) * 10.5f, -18.0f, 16.5f);
        QVector3D schoolCenter(sx, baseY, sz);
        QVector3D tint = schoolTints[sc % 5];

        for (int f = 0; f < FISH_PER_SCHOOL; ++f) {
            float h1 = ProcMath::hash3(sc, f, 1, seed + 823u);
            float h2 = ProcMath::hash3(sc, f, 2, seed + 823u);
            float h3 = ProcMath::hash3(sc, f, 3, seed + 823u);

            float ang = h1 * TWO_PI;
            float rad = 2.8f + h2 * 13.5f;
            QVector3D pos(sx + std::cos(ang) * rad, baseY + (h3 - 0.5f) * 6.0f, sz + std::sin(ang) * rad);

            data.collision.resolveSphereCollision(pos, nullptr, 1.2f, 1.5f, 3.0f);

            FaunaAgent agent;
            agent.pos = pos;
            agent.homeCenter = schoolCenter;
            agent.homeRadius = 22.0f + h2 * 16.0f;
            agent.preferredDepth = baseY;
            agent.cruiseSpeed = 4.2f + h3 * 2.6f;
            agent.scale = 0.52f + h1 * 0.56f;
            agent.boundingRadius = 0.85f * agent.scale;
            agent.phase = h1 * 12.0f;
            agent.type = 0.0f;
            agent.tint = tint;
            agent.vel = QVector3D(-std::sin(ang), 0.0f, std::cos(ang)) * agent.cruiseSpeed;
            agent.yaw = std::atan2(agent.vel.x(), agent.vel.z());
            data.fishAgents.push_back(agent);
        }
    }

    constexpr int MANTA_COUNT = 32;
    data.mantaAgents.reserve(MANTA_COUNT);

    for (int m = 0; m < MANTA_COUNT; ++m) {
        float h1 = ProcMath::hash3(m, 1, 7, seed + 907u);
        float h2 = ProcMath::hash3(m, 2, 8, seed + 907u);
        float h3 = ProcMath::hash3(m, 3, 9, seed + 907u);

        float a = (TWO_PI * m) / static_cast<float>(MANTA_COUNT) + h1 * 0.35f;
        float dist = (m < 4) ? (30.0f + m * 14.0f) : (70.0f + h2 * 375.0f);
        float mx = std::cos(a) * dist;
        float mz = std::sin(a) * dist;
        float floorY = ProcMath::sampleTerrainHeight(mx, mz, seed);
        float my = ProcMath::clamp(std::max(floorY + 10.0f, -2.0f + h3 * 16.0f), -10.0f, 20.5f);

        QVector3D pos(mx, my, mz);
        data.collision.resolveSphereCollision(pos, nullptr, 3.6f, 2.5f, 4.0f);

        FaunaAgent manta;
        manta.pos = pos;
        manta.homeCenter = QVector3D(mx, my, mz);
        manta.homeRadius = 55.0f + h1 * 50.0f;
        manta.preferredDepth = my;
        manta.cruiseSpeed = 4.6f + h2 * 2.2f;
        manta.scale = 1.15f + h3 * 0.78f;
        manta.boundingRadius = 2.6f * manta.scale;
        manta.phase = h1 * 10.0f;
        manta.type = 1.0f;
        manta.vel = QVector3D(-std::sin(a), 0.0f, std::cos(a)) * manta.cruiseSpeed;
        manta.yaw = std::atan2(manta.vel.x(), manta.vel.z());
        data.mantaAgents.push_back(manta);
    }

    const auto& pits = ProcMath::getAbyssalPits(seed);
    for (int i = 0; i < ProcMath::ABYSSAL_PIT_COUNT; ++i) {
        const auto& pit = pits[i];
        float h1 = ProcMath::hash3(i, 11, 21, seed + 3313u);
        float h2 = ProcMath::hash3(i, 12, 22, seed + 3313u);

        float ang = h1 * TWO_PI;
        float ax = pit.x + std::cos(ang) * 6.0f;
        float az = pit.z + std::sin(ang) * 6.0f;
        float bottomY = ProcMath::sampleTerrainHeight(ax, az, seed);
        float ay = bottomY + 5.2f;

        QVector3D pos(ax, ay, az);
        data.collision.resolveSphereCollision(pos, nullptr, 3.0f, 2.6f, 6.0f);

        FaunaAgent angler;
        angler.pos = pos;
        angler.homeCenter = QVector3D(pit.x, bottomY + 5.5f, pit.z);
        angler.homeRadius = pit.radius * 0.34f;
        angler.preferredDepth = bottomY + 5.2f;
        angler.cruiseSpeed = 1.65f + h2 * 0.55f;
        angler.scale = 2.05f + h1 * 0.45f;
        angler.boundingRadius = 1.35f * angler.scale;
        angler.phase = h1 * 10.0f;
        angler.type = 3.0f;
        angler.tint = QVector3D(1.0f, 1.0f, 1.0f);
        angler.vel = QVector3D(-std::sin(ang), 0.0f, std::cos(ang)) * angler.cruiseSpeed;
        angler.yaw = std::atan2(angler.vel.x(), angler.vel.z());
        data.anglerAgents.push_back(angler);
    }
}

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
        // Первые 14 лучей — ближе к центральному каньону, остальные по всей чаше
        float dist = (i < 14) ? (24.0f + std::sqrt(h2) * 95.0f) : (std::sqrt(h2) * 460.0f);
        float x = std::cos(ang) * dist;
        float z = std::sin(ang) * dist;

        float floorTop = ProcMath::sampleTerrainHeight(x, z, seed);
        if (floorTop > 13.0f) continue;

        float topY = ProcMath::WATER_SURFACE_Y - 0.10f;
        // Проверяем глубину и в точке падения наклонного луча, чтобы столб не обрывался над ямой
        float estDrop = std::max(18.0f, topY - floorTop);
        QVector3D estBot = QVector3D(x, topY, z) - sunDir * (estDrop / sunDir.y());
        float floorBot = ProcMath::sampleTerrainHeight(estBot.x(), estBot.z(), seed);
        float botY = std::max(std::min(floorTop, floorBot) - 6.0f, -40.0f);

        float shaftLen = (topY - botY) / sunDir.y();
        if (shaftLen < 10.0f) continue;

        QVector3D topCenter(x, topY, z);
        QVector3D botCenter = topCenter - sunDir * shaftLen;

        float rTop = 2.2f + h3 * 3.0f;
        float rBot = rTop * (1.35f + h4 * 0.35f); // лёгкое рассеивание пучка с глубиной
        float intensity = 0.85f + h4 * 0.50f;
        float freqScale = 0.85f + h3 * 0.45f;

        GodRayInstance inst;
        inst.topCenterRTop = QVector4D(topCenter.x(), topCenter.y(), topCenter.z(), rTop);
        inst.botCenterRBot = QVector4D(botCenter.x(), botCenter.y(), botCenter.z(), rBot);
        inst.rayParams     = QVector4D(intensity, h1, freqScale, 0.0f);
        data.godRayInstances.push_back(inst);
    }

    // 1. Всплывающие вертикальные завесы пузырьков (как вдоль колонн ламинарии на референсе)
    constexpr int BUBBLE_STREAMS      = 36;
    constexpr int BUBBLES_PER_STREAM  = 34;
    constexpr int AMBIENT_PARTICLES   = 2376; // Всего ровно 3600 частиц
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

    // 2. Рассеянные пузырьки, морской снег и биолюминесцентный планктон в толще воды
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
