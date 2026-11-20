#include "WorldGenerator.h"
#include "Noise.h"
#include <cmath>
#include <algorithm>
#include <thread>

namespace {
constexpr float PI = 3.14159265358979323846f;

inline int worldToChunkCoord(float v, float halfSize, float chunkSize, int chunksPerSide) {
    int c = static_cast<int>(std::floor((v + halfSize) / chunkSize));
    return std::clamp(c, 0, chunksPerSide - 1);
}

// Маска органических «островков» сочной зелени на неглубоких плато у поверхности
inline float samplePlateauIslandMask(float wx, float wz, uint32_t seed) {
    float n1 = ProcMath::fbm(wx * 0.030f + 3.1f, 0.4f, wz * 0.030f - 1.7f, 3, seed + 1607u);
    float n2 = ProcMath::fbm(wx * 0.072f - 2.3f, 1.1f, wz * 0.072f + 4.9f, 2, seed + 1621u);
    return ProcMath::smoothstep(0.02f, 0.30f, n1 * 0.78f + n2 * 0.22f);
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

// Деревянная балка между двумя точками p0 -> p1 (для кораблей)
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

// Обшивочная доска (4 вершины с толщиной)
void appendPlankStrip(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                      const QVector3D& a0, const QVector3D& a1,
                      const QVector3D& b0, const QVector3D& b1,
                      const QVector3D& outNormal, float thickness,
                      const QVector3D& color, float matType = 1.0f) {
    QVector3D n = outNormal.normalized();
    QVector3D off = n * thickness;

    auto addQuad = [&](const QVector3D& p0, const QVector3D& p1, const QVector3D& p2, const QVector3D& p3, const QVector3D& qn, const QVector3D& col) {
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
    addQuad(a1, b1, b1 + off, a1 + off, upEdge, color * 0.85f);
    addQuad(b0, a0, a0 + off, b0 + off, -upEdge, color * 0.85f);
}

// ============================================================================
// ПРОЦЕДУРНЫЕ ГЕНЕРАТОРЫ РАЗНООБРАЗНОЙ ПОДВОДНОЙ ФЛОРЫ
// ============================================================================

// 0. Островковая солнечная зелень неглубоких плато (широкие веерные листья и сочные ленты у поверхности)
void appendPlateauGreeneryTuft(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                               const QVector3D& center, float scale, float maxHeight,
                               uint32_t tuftSeed, bool highLod) {
    float cHash = ProcMath::hash3(static_cast<int>(tuftSeed), 3, 5, 1709u);
    QVector3D baseCol, tipCol;
    if (cHash < 0.45f) {
        baseCol = QVector3D(0.11f, 0.46f, 0.20f);
        tipCol  = QVector3D(0.46f, 0.88f, 0.28f); // сочный салатово-изумрудный
    } else if (cHash < 0.78f) {
        baseCol = QVector3D(0.08f, 0.44f, 0.30f);
        tipCol  = QVector3D(0.28f, 0.86f, 0.52f); // тропический изумруд
    } else {
        baseCol = QVector3D(0.22f, 0.48f, 0.14f);
        tipCol  = QVector3D(0.68f, 0.88f, 0.22f); // солнечная золотисто-зелёная розетка
    }

    int leaves = highLod ? 8 : 4;
    int segs   = highLod ? 3 : 2;
    QVector3D rootBase = center - QVector3D(0.0f, 0.12f, 0.0f);

    for (int i = 0; i < leaves; ++i) {
        float h1 = ProcMath::hash3(static_cast<int>(tuftSeed), i, 1, 1721u);
        float h2 = ProcMath::hash3(static_cast<int>(tuftSeed), i, 2, 1721u);
        float h3 = ProcMath::hash3(static_cast<int>(tuftSeed), i, 3, 1721u);

        float ang    = (2.0f * PI * i) / leaves + h1 * 0.55f;
        bool broad   = (i % 2 == 0);
        float rawH   = (broad ? (0.95f + h2 * 0.95f) : (1.25f + h2 * 1.15f)) * scale;
        float height = std::min(rawH, maxHeight);
        if (height < 0.35f) continue;

        float spread = (broad ? (0.85f + h3 * 0.65f) : (0.45f + h3 * 0.45f)) * scale;
        float maxW   = (broad ? (highLod ? 0.36f : 0.48f) : (highLod ? 0.18f : 0.26f)) * scale;

        QVector3D dir(std::cos(ang), 0.0f, std::sin(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D norm = (QVector3D(0.0f, 0.82f, 0.0f) + dir * 0.55f).normalized();
        QVector3D root = rootBase + dir * (0.14f + h1 * 0.36f * scale);

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float archY = height * (broad ? (t * (1.22f - 0.35f * t)) : t);
            float archR = spread * std::pow(t, broad ? 1.25f : 1.55f);
            float w = maxW * (broad ? std::sin(std::max(0.14f, t * 0.92f) * PI) : (1.0f - t * 0.78f));
            QVector3D p = root + dir * archR + QVector3D(0.0f, archY, 0.0f);
            QVector3D col = ProcMath::mix(baseCol, tipCol, t);
            verts.push_back({p - side * w, norm, col, QVector2D(2.0f, t * 0.88f)});
            verts.push_back({p + side * w, norm, col, QVector2D(2.0f, t * 0.88f)});
        }
        for (int s = 0; s < segs; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
}

// 1. Пучок морской травы (Posidonia / Eelgrass)
void appendSeagrassPatch(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                         const QVector3D& center, float scale, uint32_t patchSeed, bool highLod) {
    int blades = highLod ? 7 : 3;
    int segs   = highLod ? 3 : 2;
    QVector3D baseCol(0.10f, 0.38f, 0.20f);
    QVector3D tipCol(0.36f, 0.82f, 0.36f);

    for (int b = 0; b < blades; ++b) {
        float h1 = ProcMath::hash3(static_cast<int>(patchSeed), b, 1, 401u);
        float h2 = ProcMath::hash3(static_cast<int>(patchSeed), b, 2, 401u);
        float h3 = ProcMath::hash3(static_cast<int>(patchSeed), b, 3, 401u);

        float ang = (2.0f * PI * b) / blades + h1 * 0.85f;
        float offR = (highLod ? 0.15f : 0.20f) + h2 * 0.75f * scale;
        float height = (1.35f + h3 * 1.45f) * scale;
        float width  = (highLod ? 0.13f : 0.22f) * scale;
        float lean   = (0.28f + h1 * 0.36f) * scale;

        QVector3D dir(std::cos(ang), 0.0f, std::sin(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D norm = (dir + QVector3D(0.0f, 0.35f, 0.0f)).normalized();
        QVector3D root = center + dir * offR - QVector3D(0.0f, 0.08f, 0.0f);

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float w = width * (1.0f - t * 0.82f);
            QVector3D p = root + dir * (lean * t * t) + QVector3D(0.0f, height * t, 0.0f);
            QVector3D col = ProcMath::mix(baseCol, tipCol, t);
            verts.push_back({p - side * w, norm, col, QVector2D(2.0f, t * 0.92f)});
            verts.push_back({p + side * w, norm, col, QVector2D(2.0f, t * 0.92f)});
        }
        for (int s = 0; s < segs; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
}

// 2. Кустистые листовые водоросли (Sargassum / Bushy Algae — средний ярус 1.6..3.8 м)
void appendBushyAlgae(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                      const QVector3D& center, float scale, uint32_t bushSeed, bool highLod) {
    float cHash = ProcMath::hash3(static_cast<int>(bushSeed), 9, 3, 521u);
    QVector3D baseCol, tipCol;
    if (cHash < 0.42f) {
        baseCol = QVector3D(0.13f, 0.40f, 0.22f);
        tipCol  = QVector3D(0.40f, 0.82f, 0.36f);
    } else if (cHash < 0.75f) {
        baseCol = QVector3D(0.34f, 0.30f, 0.10f);
        tipCol  = QVector3D(0.78f, 0.64f, 0.18f);
    } else {
        baseCol = QVector3D(0.44f, 0.12f, 0.24f);
        tipCol  = QVector3D(0.88f, 0.32f, 0.44f);
    }

    int fronds = highLod ? 6 : 3;
    int segs   = highLod ? 3 : 2;
    for (int f = 0; f < fronds; ++f) {
        float h1 = ProcMath::hash3(static_cast<int>(bushSeed), f, 1, 523u);
        float h2 = ProcMath::hash3(static_cast<int>(bushSeed), f, 2, 523u);

        float ang = (2.0f * PI * f) / fronds + h1 * 0.55f;
        float len = (1.85f + h2 * 1.45f) * scale;
        float spread = (0.75f + h1 * 0.65f) * scale;
        float maxW = (highLod ? 0.44f : 0.62f) * scale;

        QVector3D dir(std::cos(ang), 0.0f, std::sin(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D norm = (QVector3D(0.0f, 0.75f, 0.0f) + dir * 0.65f).normalized();
        QVector3D root = center - QVector3D(0.0f, 0.12f, 0.0f);

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float archY = len * (t * (1.28f - 0.38f * t));
            float archR = spread * std::pow(t, 1.35f);
            float w = maxW * std::sin(std::max(0.12f, t * 0.92f) * PI);
            QVector3D p = root + dir * archR + QVector3D(0.0f, archY, 0.0f);
            QVector3D col = ProcMath::mix(baseCol, tipCol, t);
            verts.push_back({p - side * w, norm, col, QVector2D(2.0f, t * 0.85f)});
            verts.push_back({p + side * w, norm, col, QVector2D(2.0f, t * 0.85f)});
        }
        for (int s = 0; s < segs; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
}

void appendCoralBranch(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                       const QVector3D& p0, const QVector3D& p1,
                       float r0, float r1, int sides,
                       const QVector3D& col0, const QVector3D& col1, float sway0 = 0.0f, float sway1 = 0.0f) {
    QVector3D axis = p1 - p0;
    float len = axis.length();
    if (len < 1e-4f) return;
    QVector3D dir = axis / len;

    QVector3D u = QVector3D::crossProduct(dir, QVector3D(0.0f, 1.0f, 0.0f));
    if (u.lengthSquared() < 1e-4f) u = QVector3D::crossProduct(dir, QVector3D(1.0f, 0.0f, 0.0f));
    u.normalize();
    QVector3D v = QVector3D::crossProduct(dir, u).normalized();

    uint32_t b0 = static_cast<uint32_t>(verts.size());
    for (int s = 0; s <= sides; ++s) {
        float a = (2.0f * PI * s) / sides;
        QVector3D rad = u * std::cos(a) + v * std::sin(a);
        verts.push_back({p0 + rad * r0, rad, col0, QVector2D(2.0f, sway0)});
    }
    uint32_t b1 = static_cast<uint32_t>(verts.size());
    for (int s = 0; s <= sides; ++s) {
        float a = (2.0f * PI * s) / sides;
        QVector3D rad = u * std::cos(a) + v * std::sin(a);
        verts.push_back({p1 + rad * r1, (rad + dir * 0.35f).normalized(), col1, QVector2D(2.0f, sway1)});
    }
    for (int s = 0; s < sides; ++s) {
        inds.insert(inds.end(), {b0 + s, b0 + s + 1, b1 + s, b0 + s + 1, b1 + s + 1, b1 + s});
    }
}

// 3. Ветвистый «олений» коралл (Staghorn Coral)
void appendStaghornCoral(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                         const QVector3D& center, float scale, uint32_t coralSeed, bool highLod) {
    float cHash = ProcMath::hash3(static_cast<int>(coralSeed), 1, 7, 613u);
    QVector3D baseCol, tipCol;
    if (cHash < 0.34f) {
        baseCol = QVector3D(0.84f, 0.22f, 0.38f);
        tipCol  = QVector3D(1.00f, 0.64f, 0.72f);
    } else if (cHash < 0.68f) {
        baseCol = QVector3D(0.86f, 0.46f, 0.12f);
        tipCol  = QVector3D(1.00f, 0.84f, 0.36f);
    } else {
        baseCol = QVector3D(0.50f, 0.24f, 0.78f);
        tipCol  = QVector3D(0.46f, 0.92f, 0.98f);
    }

    int trunks = highLod ? 4 : 3;
    int sides  = highLod ? 5 : 3;
    QVector3D root = center - QVector3D(0.0f, 0.15f, 0.0f);

    for (int t = 0; t < trunks; ++t) {
        float h1 = ProcMath::hash3(static_cast<int>(coralSeed), t, 1, 617u);
        float h2 = ProcMath::hash3(static_cast<int>(coralSeed), t, 2, 617u);
        float ang = (2.0f * PI * t) / trunks + h1 * 0.7f;
        QVector3D d1(std::cos(ang) * (0.48f + h2 * 0.30f), 0.85f, std::sin(ang) * (0.48f + h2 * 0.30f));
        d1.normalize();
        float len1 = (1.15f + h1 * 0.55f) * scale;
        QVector3D mid = root + d1 * len1;
        appendCoralBranch(verts, inds, root, mid, 0.18f * scale, 0.11f * scale, sides, baseCol, ProcMath::mix(baseCol, tipCol, 0.45f));

        if (highLod) {
            for (int f = 0; f < 2; ++f) {
                float fAng = ang + (f == 0 ? 0.65f : -0.65f) + (h2 - 0.5f) * 0.3f;
                QVector3D d2(std::cos(fAng) * 0.58f, 0.80f, std::sin(fAng) * 0.58f);
                d2.normalize();
                QVector3D tip = mid + d2 * ((0.75f + 0.35f * f) * scale);
                appendCoralBranch(verts, inds, mid, tip, 0.10f * scale, 0.035f * scale, 4,
                                  ProcMath::mix(baseCol, tipCol, 0.45f), tipCol);
            }
        }
    }
}

// 4. Пластинчатый / столовый коралл (Table Coral)
void appendTableCoral(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                      const QVector3D& center, const QVector3D& surfNormal, float scale, uint32_t tableSeed, bool highLod) {
    float cHash = ProcMath::hash3(static_cast<int>(tableSeed), 2, 5, 709u);
    QVector3D innerCol, rimCol;
    if (cHash < 0.35f) {
        innerCol = QVector3D(0.76f, 0.32f, 0.24f);
        rimCol   = QVector3D(0.98f, 0.72f, 0.42f);
    } else if (cHash < 0.70f) {
        innerCol = QVector3D(0.48f, 0.26f, 0.68f);
        rimCol   = QVector3D(0.90f, 0.56f, 0.94f);
    } else {
        innerCol = QVector3D(0.20f, 0.58f, 0.44f);
        rimCol   = QVector3D(0.58f, 0.94f, 0.62f);
    }

    int tiers = highLod ? 2 : 1;
    int segs  = highLod ? 10 : 6;
    QVector3D outDir(surfNormal.x(), 0.0f, surfNormal.z());
    if (outDir.lengthSquared() < 0.01f) outDir = QVector3D(1.0f, 0.0f, 0.0f);
    outDir.normalize();

    for (int tier = 0; tier < tiers; ++tier) {
        float radius = (1.35f - tier * 0.42f) * scale;
        float yOff   = (0.55f + tier * 0.52f) * scale;
        QVector3D discCenter = center + outDir * (0.35f * scale * (1.0f - tier * 0.4f)) + QVector3D(0.0f, yOff, 0.0f);

        if (tier == 0) {
            appendCoralBranch(verts, inds, center - QVector3D(0.0f, 0.15f, 0.0f), discCenter,
                              0.26f * scale, 0.22f * scale, highLod ? 5 : 4, innerCol * 0.75f, innerCol);
        }

        uint32_t cIdx = static_cast<uint32_t>(verts.size());
        verts.push_back({discCenter + QVector3D(0.0f, 0.12f * scale, 0.0f), QVector3D(0.0f, 1.0f, 0.0f), innerCol, QVector2D(2.0f, 0.0f)});
        uint32_t rimStart = static_cast<uint32_t>(verts.size());

        for (int s = 0; s <= segs; ++s) {
            float a = (2.0f * PI * s) / segs;
            float wave = 1.0f + 0.18f * std::sin(a * 5.0f + static_cast<float>(tableSeed));
            float curl = 0.08f * scale * std::cos(a * 4.0f + tier);
            QVector3D p = discCenter + QVector3D(std::cos(a) * radius * wave, curl, std::sin(a) * radius * wave);
            QVector3D n(std::cos(a) * 0.25f, 0.96f, std::sin(a) * 0.25f);
            n.normalize();
            verts.push_back({p, n, rimCol, QVector2D(2.0f, 0.0f)});
        }

        for (int s = 0; s < segs; ++s) {
            inds.insert(inds.end(), {cIdx, rimStart + s, rimStart + s + 1});
        }
    }
}

// 5. Веерная горгонария (Gorgonian Sea Fan)
void appendSeaFan(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                  const QVector3D& center, float scale, uint32_t fanSeed, bool highLod) {
    float cHash = ProcMath::hash3(static_cast<int>(fanSeed), 3, 8, 761u);
    QVector3D baseCol, tipCol;
    if (cHash < 0.36f) {
        baseCol = QVector3D(0.80f, 0.14f, 0.28f);
        tipCol  = QVector3D(1.00f, 0.40f, 0.48f);
    } else if (cHash < 0.70f) {
        baseCol = QVector3D(0.88f, 0.38f, 0.10f);
        tipCol  = QVector3D(1.00f, 0.74f, 0.22f);
    } else {
        baseCol = QVector3D(0.62f, 0.16f, 0.74f);
        tipCol  = QVector3D(0.95f, 0.48f, 0.98f);
    }

    float yaw = ProcMath::hash3(static_cast<int>(fanSeed), 4, 2, 769u) * 2.0f * PI;
    QVector3D planeX(std::cos(yaw), 0.0f, std::sin(yaw));
    QVector3D planeN(-planeX.z(), 0.0f, planeX.x());
    QVector3D root = center - QVector3D(0.0f, 0.12f, 0.0f);

    int ribs = highLod ? 7 : 3;
    int segs = highLod ? 3 : 2;
    float fanH = 2.4f * scale;
    float fanW = 1.95f * scale;

    for (int r = 0; r < ribs; ++r) {
        float u = (ribs > 1) ? ((static_cast<float>(r) / (ribs - 1)) * 2.0f - 1.0f) : 0.0f;
        float spreadAng = u * 0.92f;
        float ribLen = fanH * (0.72f + 0.28f * std::cos(u * PI * 0.5f));
        float ribW   = (highLod ? 0.22f : 0.42f) * scale;

        QVector3D dir = (planeX * std::sin(spreadAng) + QVector3D(0.0f, std::cos(spreadAng), 0.0f)).normalized();
        QVector3D perp = QVector3D::crossProduct(dir, planeN).normalized();

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float w = ribW * (0.35f + 0.95f * std::sin(t * PI * 0.85f));
            float curl = std::sin(t * PI) * 0.18f * scale * (r % 2 == 0 ? 1.0f : -1.0f);
            QVector3D p = root + dir * (ribLen * t) + planeX * (u * fanW * 0.12f * t * t) + planeN * curl;
            QVector3D col = ProcMath::mix(baseCol, tipCol, t);
            verts.push_back({p - perp * w, planeN, col, QVector2D(2.0f, t * 0.48f)});
            verts.push_back({p + perp * w, planeN, col, QVector2D(2.0f, t * 0.48f)});
        }
        for (int s = 0; s < segs; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
}

// 6. Морская актиния / анемон
void appendSeaAnemone(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                      const QVector3D& center, float scale, uint32_t anemSeed) {
    float cHash = ProcMath::hash3(static_cast<int>(anemSeed), 5, 1, 829u);
    QVector3D stalkCol, tipCol;
    if (cHash < 0.35f) {
        stalkCol = QVector3D(0.64f, 0.18f, 0.52f);
        tipCol   = QVector3D(1.00f, 0.48f, 0.92f);
    } else if (cHash < 0.70f) {
        stalkCol = QVector3D(0.16f, 0.54f, 0.56f);
        tipCol   = QVector3D(0.42f, 0.98f, 0.90f);
    } else {
        stalkCol = QVector3D(0.84f, 0.34f, 0.16f);
        tipCol   = QVector3D(1.00f, 0.80f, 0.30f);
    }

    QVector3D baseP = center - QVector3D(0.0f, 0.08f, 0.0f);
    QVector3D crown = center + QVector3D(0.0f, 0.42f * scale, 0.0f);
    appendCoralBranch(verts, inds, baseP, crown, 0.42f * scale, 0.36f * scale, 6, stalkCol * 0.8f, stalkCol, 0.0f, 0.1f);

    constexpr int TENTACLES = 10;
    for (int t = 0; t < TENTACLES; ++t) {
        float h1 = ProcMath::hash3(static_cast<int>(anemSeed), t, 2, 839u);
        float ang = (2.0f * PI * t) / TENTACLES + h1 * 0.25f;
        float ringR = ((t % 2 == 0) ? 0.28f : 0.15f) * scale;
        QVector3D dir(std::cos(ang), 0.0f, std::sin(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D tRoot = crown + dir * ringR;
        float tLen = (0.78f + h1 * 0.42f) * scale;
        float spread = ((t % 2 == 0) ? 0.62f : 0.28f) * scale;

        QVector3D pMid = tRoot + dir * (spread * 0.45f) + QVector3D(0.0f, tLen * 0.58f, 0.0f);
        QVector3D pTip = tRoot + dir * spread + QVector3D(0.0f, tLen, 0.0f);
        QVector3D norm = (dir + QVector3D(0.0f, 0.5f, 0.0f)).normalized();
        float w0 = 0.095f * scale;
        float w1 = 0.055f * scale;

        uint32_t vb = static_cast<uint32_t>(verts.size());
        verts.push_back({tRoot - side * w0, norm, stalkCol, QVector2D(2.0f, 0.15f)});
        verts.push_back({tRoot + side * w0, norm, stalkCol, QVector2D(2.0f, 0.15f)});
        verts.push_back({pMid  - side * w1, norm, ProcMath::mix(stalkCol, tipCol, 0.55f), QVector2D(2.0f, 0.55f)});
        verts.push_back({pMid  + side * w1, norm, ProcMath::mix(stalkCol, tipCol, 0.55f), QVector2D(2.0f, 0.55f)});
        verts.push_back({pTip,              norm, tipCol,   QVector2D(2.0f, 0.95f)});
        inds.insert(inds.end(), {vb, vb + 1, vb + 2, vb + 1, vb + 3, vb + 2, vb + 2, vb + 3, vb + 4});
    }
}

// 7. Глубоководные биолюминесцентные морские перья (растут в абиссальных ямах, param.x = 4.0)
void appendAbyssalSeaPen(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                         const QVector3D& center, float scale, uint32_t penSeed) {
    float cHash = ProcMath::hash3(static_cast<int>(penSeed), 7, 3, 941u);
    QVector3D baseCol(0.08f, 0.18f, 0.28f);
    QVector3D glowCol = (cHash < 0.45f) ? QVector3D(0.28f, 0.95f, 0.98f)
                        : (cHash < 0.78f) ? QVector3D(0.45f, 0.58f, 1.00f)
                                          : QVector3D(0.78f, 0.35f, 0.98f);

    int stalks = 2 + (penSeed % 2);
    for (int k = 0; k < stalks; ++k) {
        float h1 = ProcMath::hash3(static_cast<int>(penSeed), k, 1, 947u);
        float ang = h1 * 2.0f * PI;
        QVector3D dir(std::cos(ang), 0.0f, std::sin(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D norm = (dir + QVector3D(0.0f, 0.3f, 0.0f)).normalized();

        float height = (1.8f + h1 * 1.6f) * scale;
        float maxW   = 0.32f * scale;
        QVector3D root = center + dir * (0.25f * k) - QVector3D(0.0f, 0.1f, 0.0f);

        constexpr int SEGS = 4;
        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= SEGS; ++s) {
            float t = static_cast<float>(s) / SEGS;
            float plume = std::sin(std::pow(t, 0.75f) * PI);
            float w = maxW * (0.14f + 0.86f * plume);
            QVector3D p = root + dir * (0.35f * scale * t * t) + QVector3D(0.0f, height * t, 0.0f);
            QVector3D col = ProcMath::mix(baseCol, glowCol, t);
            verts.push_back({p - side * w, norm, col, QVector2D(4.0f, t)});
            verts.push_back({p + side * w, norm, col, QVector2D(4.0f, t)});
        }
        for (int s = 0; s < SEGS; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
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
        float ang = (2.0f * PI * s) / sides;
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
            float theta = 2.0f * PI * static_cast<float>(sl) / static_cast<float>(slices);
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
            float a = (2.0f * PI * j) / radialSegs;
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
    if (pos.x() * pos.x() + pos.z() * pos.z() > (ProcMath::PLAYABLE_HARD_RADIUS - radius) * (ProcMath::PLAYABLE_HARD_RADIUS - radius)) {
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
// ГЕНЕРАЦИЯ МИРА (1600x1600 м, Рельеф 48x48, Островки зелени, Удильщики, Корабли)
// ============================================================================

WorldData WorldGenerator::generate(uint32_t seed) {
    WorldData data;
    data.collision.seed = seed;
    data.collision.chunksPerSide = WorldData::CHUNKS_PER_SIDE;
    data.collision.chunkSize = WorldData::CHUNK_SIZE;
    data.collision.worldHalfSize = ProcMath::WORLD_HALF_SIZE;

    buildTerrainLodIndices(data);
    generateTerrainChunks(data, seed);
    generateChunkProps(data, seed);
    generateShipwrecks(data, seed);

    data.collision.buildGrid();

    generateFaunaMeshes(data);
    generateWhaleMeshes(data);
    generateAnglerMeshes(data);
    generateFaunaAgents(data, seed);
    generateGodRaysAndParticles(data, seed);
    return data;
}

void WorldGenerator::buildTerrainLodIndices(WorldData& data) {
    constexpr int N = WorldData::CHUNK_GRID_N;     // 48
    constexpr int G = N + 1;                       // 49
    constexpr uint32_t SURF_COUNT   = G * G;
    constexpr uint32_t SKIRT_TOP    = SURF_COUNT;
    constexpr uint32_t SKIRT_BOT    = SKIRT_TOP + G;
    constexpr uint32_t SKIRT_LEFT   = SKIRT_BOT + G;
    constexpr uint32_t SKIRT_RIGHT  = SKIRT_LEFT + G;

    auto surfIdx = [&](int ix, int iz) -> uint32_t {
        return static_cast<uint32_t>(iz * G + ix);
    };

    const int steps[WorldData::TERRAIN_LOD_COUNT] = {1, 2, 4, 8};
    data.terrainIndices.clear();

    for (int lod = 0; lod < WorldData::TERRAIN_LOD_COUNT; ++lod) {
        int step = steps[lod];
        uint32_t startOffset = static_cast<uint32_t>(data.terrainIndices.size());

        for (int iz = 0; iz < N; iz += step) {
            for (int ix = 0; ix < N; ix += step) {
                uint32_t i00 = surfIdx(ix, iz);
                uint32_t i10 = surfIdx(ix + step, iz);
                uint32_t i01 = surfIdx(ix, iz + step);
                uint32_t i11 = surfIdx(ix + step, iz + step);

                data.terrainIndices.push_back(i00);
                data.terrainIndices.push_back(i01);
                data.terrainIndices.push_back(i10);

                data.terrainIndices.push_back(i10);
                data.terrainIndices.push_back(i01);
                data.terrainIndices.push_back(i11);
            }
        }

        for (int i = 0; i < N; i += step) {
            uint32_t t0 = surfIdx(i, 0), t1 = surfIdx(i + step, 0);
            uint32_t tk0 = SKIRT_TOP + i, tk1 = SKIRT_TOP + i + step;
            data.terrainIndices.insert(data.terrainIndices.end(), {t0, t1, tk0, t1, tk1, tk0});

            uint32_t b0 = surfIdx(i, N), b1 = surfIdx(i + step, N);
            uint32_t bk0 = SKIRT_BOT + i, bk1 = SKIRT_BOT + i + step;
            data.terrainIndices.insert(data.terrainIndices.end(), {b0, bk0, b1, b1, bk0, bk1});

            uint32_t l0 = surfIdx(0, i), l1 = surfIdx(0, i + step);
            uint32_t lk0 = SKIRT_LEFT + i, lk1 = SKIRT_LEFT + i + step;
            data.terrainIndices.insert(data.terrainIndices.end(), {l0, lk0, l1, l1, lk0, lk1});

            uint32_t r0 = surfIdx(N, i), r1 = surfIdx(N, i + step);
            uint32_t rk0 = SKIRT_RIGHT + i, rk1 = SKIRT_RIGHT + i + step;
            data.terrainIndices.insert(data.terrainIndices.end(), {r0, r1, rk0, r1, rk1, rk0});
        }

        uint32_t count = static_cast<uint32_t>(data.terrainIndices.size()) - startOffset;
        data.terrainLodRanges[lod] = {startOffset, count};
    }
}

void WorldGenerator::generateTerrainChunks(WorldData& data, uint32_t seed) {
    constexpr int C = WorldData::CHUNKS_PER_SIDE;
    constexpr int N = WorldData::CHUNK_GRID_N;     // 48
    constexpr int G = N + 1;                       // 49
    constexpr int P = G + 2;                       // 51
    constexpr int TOTAL_CHUNKS = C * C;
    constexpr float CHUNK_SZ = WorldData::CHUNK_SIZE;
    constexpr float CELL_SZ  = CHUNK_SZ / static_cast<float>(N);
    constexpr float HALF_W   = ProcMath::WORLD_HALF_SIZE;
    constexpr float SKIRT_DROP = 14.0f;

    data.chunks.resize(TOTAL_CHUNKS);
    data.terrainVertices.resize(static_cast<size_t>(TOTAL_CHUNKS) * WorldData::CHUNK_VERTS_PER_CHUNK);

    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (unsigned int t = 0; t < numThreads; ++t) {
        int startChunk = (t * TOTAL_CHUNKS) / numThreads;
        int endChunk   = ((t + 1) * TOTAL_CHUNKS) / numThreads;

        workers.emplace_back([&, startChunk, endChunk]() {
            std::vector<float> hCache(P * P);
            for (int ci = startChunk; ci < endChunk; ++ci) {
                int cx = ci % C;
                int cz = ci / C;

                WorldChunk& chunk = data.chunks[ci];
                chunk.cx = cx;
                chunk.cz = cz;
                chunk.terrainBaseVertex = static_cast<uint32_t>(ci * WorldData::CHUNK_VERTS_PER_CHUNK);

                float originX = -HALF_W + cx * CHUNK_SZ;
                float originZ = -HALF_W + cz * CHUNK_SZ;

                for (int gz = 0; gz < P; ++gz) {
                    float wz = originZ + (gz - 1) * CELL_SZ;
                    for (int gx = 0; gx < P; ++gx) {
                        float wx = originX + (gx - 1) * CELL_SZ;
                        hCache[gz * P + gx] = ProcMath::sampleTerrainHeight(wx, wz, seed);
                    }
                }

                float minY =  1e6f;
                float maxY = -1e6f;
                EnvVertex* dst = &data.terrainVertices[chunk.terrainBaseVertex];

                for (int iz = 0; iz <= N; ++iz) {
                    float wz = originZ + iz * CELL_SZ;
                    int pz = iz + 1;
                    for (int ix = 0; ix <= N; ++ix) {
                        float wx = originX + ix * CELL_SZ;
                        int px = ix + 1;

                        float wy = hCache[pz * P + px];
                        float hL = hCache[pz * P + (px - 1)];
                        float hR = hCache[pz * P + (px + 1)];
                        float hD = hCache[(pz - 1) * P + px];
                        float hU = hCache[(pz + 1) * P + px];
                        QVector3D norm(hL - hR, 2.0f * CELL_SZ, hD - hU);
                        norm.normalize();

                        minY = std::min(minY, wy);
                        maxY = std::max(maxY, wy);

                        float slope     = 1.0f - ProcMath::clamp(norm.y(), 0.0f, 1.0f);
                        float wallF     = ProcMath::sampleBasinWallFactor(wx, wz, seed);
                        float heightMix = ProcMath::smoothstep(-22.0f, 10.0f, wy);
                        float abyssMix  = ProcMath::smoothstep(-26.0f, -50.0f, wy);
                        float spireMix  = ProcMath::smoothstep(6.0f, 28.0f, wy) * (1.0f - wallF);

                        QVector3D sandCol(0.62f, 0.58f, 0.44f);
                        QVector3D mossRock(0.20f, 0.36f, 0.28f);
                        QVector3D sharpCrag(0.22f, 0.25f, 0.29f);
                        QVector3D cliffBasalt(0.16f, 0.19f, 0.24f);
                        QVector3D abyssSlate(0.08f, 0.11f, 0.16f);
                        QVector3D grottoTint(0.32f, 0.21f, 0.42f);

                        float grottoMask = ProcMath::smoothstep(0.18f, 0.68f,
                                                                ProcMath::fbm(wx * 0.006f, 0.0f, wz * 0.006f, 2, seed + 444u) * 0.5f + 0.5f);

                        QVector3D col = ProcMath::mix(sandCol, mossRock, ProcMath::clamp(slope * 2.5f + heightMix * 0.55f, 0.0f, 1.0f));
                        col = ProcMath::mix(col, sharpCrag, ProcMath::clamp(ProcMath::smoothstep(0.24f, 0.62f, slope) * 0.85f + spireMix * 0.65f, 0.0f, 1.0f));
                        col = ProcMath::mix(col, grottoTint, grottoMask * 0.30f * (1.0f - wallF) * (1.0f - abyssMix));
                        col = ProcMath::mix(col, cliffBasalt, ProcMath::clamp(wallF * 0.85f + ProcMath::smoothstep(22.0f, 36.0f, wy) * 0.55f, 0.0f, 1.0f));

                        // Островки сочного подводного мха на неглубоких солнечных плато (y: +1.5 .. +26 м)
                        if (wy > 1.5f && wy < 26.5f && norm.y() > 0.54f && wallF < 0.78f) {
                            float shallowMask = ProcMath::smoothstep(1.5f, 7.5f, wy)
                                                * (1.0f - ProcMath::smoothstep(24.5f, 27.0f, wy));
                            float flatMask    = ProcMath::smoothstep(0.54f, 0.78f, norm.y());
                            float islandMask  = samplePlateauIslandMask(wx, wz, seed);
                            float mossBlend   = shallowMask * flatMask * islandMask;
                            if (mossBlend > 0.01f) {
                                QVector3D lushMoss = ProcMath::mix(QVector3D(0.14f, 0.50f, 0.24f),
                                                                   QVector3D(0.32f, 0.72f, 0.26f),
                                                                   ProcMath::smoothstep(0.35f, 0.85f, islandMask));
                                col = ProcMath::mix(col, lushMoss, mossBlend * 0.82f);
                            }
                        }

                        col = ProcMath::mix(col, abyssSlate, abyssMix * 0.92f);

                        EnvVertex& v = dst[iz * G + ix];
                        v.pos    = QVector3D(wx, wy, wz);
                        v.normal = norm;
                        v.color  = col;
                        v.param  = QVector2D(0.0f, 0.0f);
                    }
                }

                uint32_t skirtTop   = G * G;
                uint32_t skirtBot   = skirtTop + G;
                uint32_t skirtLeft  = skirtBot + G;
                uint32_t skirtRight = skirtLeft + G;

                for (int i = 0; i <= N; ++i) {
                    auto makeSkirt = [&](const EnvVertex& src) {
                        EnvVertex s = src;
                        s.pos.setY(s.pos.y() - SKIRT_DROP);
                        s.color *= 0.80f;
                        return s;
                    };
                    dst[skirtTop + i]   = makeSkirt(dst[0 * G + i]);
                    dst[skirtBot + i]   = makeSkirt(dst[N * G + i]);
                    dst[skirtLeft + i]  = makeSkirt(dst[i * G + 0]);
                    dst[skirtRight + i] = makeSkirt(dst[i * G + N]);
                }

                chunk.minBounds = QVector3D(originX, minY - SKIRT_DROP, originZ);
                chunk.maxBounds = QVector3D(originX + CHUNK_SZ, maxY + 30.0f, originZ + CHUNK_SZ);
                chunk.center    = (chunk.minBounds + chunk.maxBounds) * 0.5f;
                chunk.boundingRadius = (chunk.maxBounds - chunk.center).length();
            }
        });
    }
    for (auto& th : workers) th.join();
}

void WorldGenerator::appendRockBoulder(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                                       const QVector3D& center, float rx, float ry, float rz,
                                       int stacks, int slices, uint32_t rockSeed, const QVector3D& baseColor) {
    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    std::vector<QVector3D> pts((stacks + 1) * (slices + 1));

    for (int st = 0; st <= stacks; ++st) {
        float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int sl = 0; sl <= slices; ++sl) {
            float theta = 2.0f * PI * static_cast<float>(sl) / static_cast<float>(slices);
            QVector3D dir(sp * std::cos(theta), cp, sp * std::sin(theta));
            float disp = 1.0f + ProcMath::fbm(dir.x() * 1.9f, dir.y() * 1.9f, dir.z() * 1.9f, 3, rockSeed) * 0.36f;
            pts[st * (slices + 1) + sl] = center + QVector3D(dir.x() * rx * disp, dir.y() * ry * disp, dir.z() * rz * disp);
        }
    }

    for (int st = 0; st <= stacks; ++st) {
        for (int sl = 0; sl <= slices; ++sl) {
            QVector3D p = pts[st * (slices + 1) + sl];
            int stU = std::max(0, st - 1), stD = std::min(stacks, st + 1);
            int slL = (sl - 1 + slices) % slices, slR = (sl + 1) % slices;
            QVector3D t1 = pts[stD * (slices + 1) + sl] - pts[stU * (slices + 1) + sl];
            QVector3D t2 = pts[st * (slices + 1) + slR] - pts[st * (slices + 1) + slL];
            QVector3D n = QVector3D::crossProduct(t2, t1);
            if (n.lengthSquared() < 1e-6f) n = (p - center);
            n.normalize();

            float moss = ProcMath::smoothstep(0.35f, 0.85f, n.y());
            QVector3D col = ProcMath::mix(baseColor, QVector3D(0.18f, 0.40f, 0.26f), moss * 0.58f);
            verts.push_back({p, n, col, QVector2D(0.0f, 0.0f)});
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

void WorldGenerator::appendKelpStalk(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                                     const QVector3D& rootPos, float height, uint32_t stalkSeed, bool highLod) {
    int segs = highLod ? 11 : 5;
    float baseWidth = highLod ? 0.36f : 0.68f;
    float twist = ProcMath::hash3(static_cast<int>(stalkSeed), 3, 7, 99u) * 2.0f * PI;
    float hueVar = ProcMath::hash3(static_cast<int>(stalkSeed), 5, 11, 99u);
    QVector3D stalkCol = ProcMath::mix(QVector3D(0.12f, 0.45f, 0.24f), QVector3D(0.24f, 0.48f, 0.16f), hueVar);
    QVector3D tipCol   = ProcMath::mix(QVector3D(0.26f, 0.78f, 0.34f), QVector3D(0.46f, 0.82f, 0.26f), hueVar);

    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    for (int s = 0; s <= segs; ++s) {
        float t = static_cast<float>(s) / static_cast<float>(segs);
        float y = rootPos.y() + t * height;
        float angle = twist + t * 2.4f;
        QVector3D side(std::cos(angle), 0.0f, std::sin(angle));
        QVector3D norm(-side.z(), 0.25f, side.x());
        norm.normalize();

        float w = baseWidth * (1.0f - t * 0.42f);
        QVector3D col = ProcMath::mix(stalkCol, tipCol, t);

        verts.push_back({QVector3D(rootPos.x(), y, rootPos.z()) - side * w, norm, col, QVector2D(3.0f, t)});
        verts.push_back({QVector3D(rootPos.x(), y, rootPos.z()) + side * w, norm, col, QVector2D(3.0f, t)});
    }

    for (int s = 0; s < segs; ++s) {
        uint32_t i0 = baseIdx + s * 2;
        inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
    }

    if (highLod) {
        for (int s = 2; s < segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float y = rootPos.y() + t * height;
            float sideSign = (s % 2 == 0) ? 1.0f : -1.0f;
            float angle = twist + t * 2.6f + sideSign * 0.7f;

            QVector3D dir(std::cos(angle), 0.22f, std::sin(angle));
            dir.normalize();
            QVector3D upLeaf(0.0f, 1.0f, 0.0f);
            QVector3D norm = QVector3D::crossProduct(dir, upLeaf).normalized();

            float leafLen = (1.65f + 0.90f * (1.0f - t));
            float leafW = 0.46f;
            QVector3D p0(rootPos.x(), y, rootPos.z());
            QVector3D pMid = p0 + dir * (leafLen * 0.55f) + QVector3D(0.0f, 0.25f, 0.0f);
            QVector3D pTip = p0 + dir * leafLen - QVector3D(0.0f, 0.15f, 0.0f);
            QVector3D lCol = ProcMath::mix(stalkCol * 1.15f, tipCol * 1.10f, t);

            uint32_t lb = static_cast<uint32_t>(verts.size());
            verts.push_back({p0, norm, lCol, QVector2D(3.0f, t)});
            verts.push_back({pMid + upLeaf * leafW, norm, lCol, QVector2D(3.0f, std::min(1.0f, t + 0.06f))});
            verts.push_back({pMid - upLeaf * leafW, norm, lCol, QVector2D(3.0f, std::min(1.0f, t + 0.06f))});
            verts.push_back({pTip, norm, lCol * 1.12f, QVector2D(3.0f, std::min(1.0f, t + 0.10f))});

            inds.insert(inds.end(), {lb, lb + 1, lb + 2, lb + 1, lb + 3, lb + 2});
        }
    }
}

void WorldGenerator::generateChunkProps(WorldData& data, uint32_t seed) {
    constexpr int C = WorldData::CHUNKS_PER_SIDE;
    constexpr int TOTAL_CHUNKS = C * C;
    constexpr float CHUNK_SZ = WorldData::CHUNK_SIZE;
    constexpr float HALF_W = ProcMath::WORLD_HALF_SIZE;
    const auto& pits = ProcMath::getAbyssalPits(seed);

    struct ChunkPropTemp {
        std::vector<EnvVertex> verts;
        std::vector<uint32_t> boulder0Inds;
        std::vector<uint32_t> boulder1Inds;
        std::vector<uint32_t> kelp0Inds;
        std::vector<uint32_t> kelp1Inds;
        std::vector<uint32_t> flora0Inds;
        std::vector<uint32_t> flora1Inds;
        std::vector<uint32_t> detailInds;
        std::vector<SphereCollider> spheres;
    };

    std::vector<ChunkPropTemp> chunkTemps(TOTAL_CHUNKS);
    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (unsigned int th = 0; th < numThreads; ++th) {
        int startChunk = (th * TOTAL_CHUNKS) / numThreads;
        int endChunk   = ((th + 1) * TOTAL_CHUNKS) / numThreads;

        workers.emplace_back([&, startChunk, endChunk]() {
            for (int ci = startChunk; ci < endChunk; ++ci) {
                const WorldChunk& chunk = data.chunks[ci];
                ChunkPropTemp& tmp = chunkTemps[ci];
                tmp.verts.reserve(4200);

                float originX = -HALF_W + chunk.cx * CHUNK_SZ;
                float originZ = -HALF_W + chunk.cz * CHUNK_SZ;
                float chunkCenterX = originX + CHUNK_SZ * 0.5f;
                float chunkCenterZ = originZ + CHUNK_SZ * 0.5f;
                float centerWallF  = ProcMath::sampleBasinWallFactor(chunkCenterX, chunkCenterZ, seed);

                // ------------------------------------------------------------
                // 1. Валуны и скальные обломки (LOD0/LOD1 + коллайдеры)
                // ------------------------------------------------------------
                struct BoulderSpec {
                    QVector3D center;
                    float rx, ry, rz;
                    uint32_t rSeed;
                    QVector3D color;
                };
                std::vector<BoulderSpec> chunkBoulders;

                for (int b = 0; b < 9; ++b) {
                    float hx = ProcMath::hash3(chunk.cx, chunk.cz, b * 3 + 1, seed + 211u);
                    float hz = ProcMath::hash3(chunk.cx, chunk.cz, b * 3 + 2, seed + 211u);
                    float hs = ProcMath::hash3(chunk.cx, chunk.cz, b * 3 + 3, seed + 211u);

                    float wx = originX + 5.0f + hx * (CHUNK_SZ - 10.0f);
                    float wz = originZ + 5.0f + hz * (CHUNK_SZ - 10.0f);
                    float wallF = ProcMath::sampleBasinWallFactor(wx, wz, seed);
                    if (wallF > 0.84f) continue;

                    if ((wx * wx + (wz - 20.0f) * (wz - 20.0f)) < 260.0f) continue;
                    if (((wx - 32.0f) * (wx - 32.0f) + (wz + 34.0f) * (wz + 34.0f)) < 680.0f) continue;

                    bool inPitCore = false;
                    for (const auto& pit : pits) {
                        float dx = wx - pit.x, dz = wz - pit.z;
                        if (dx * dx + dz * dz < (pit.radius * 0.55f) * (pit.radius * 0.55f)) {
                            inPitCore = true;
                            break;
                        }
                    }
                    if (inPitCore) continue;

                    float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                    if (wy > 18.0f) continue;

                    float baseR = 2.4f + hs * 5.6f + wallF * 4.5f;
                    float rx = baseR * (0.85f + 0.35f * ProcMath::hash3(ci, b, 11, seed));
                    float ry = baseR * (0.65f + 0.55f * ProcMath::hash3(ci, b, 13, seed));
                    float rz = baseR * (0.85f + 0.35f * ProcMath::hash3(ci, b, 17, seed));

                    QVector3D center(wx, wy + ry * 0.32f, wz);
                    float abyssDark = ProcMath::smoothstep(-26.0f, -48.0f, wy);
                    QVector3D col = ProcMath::mix(QVector3D(0.24f + hs * 0.08f, 0.29f + hs * 0.07f, 0.31f + hs * 0.06f),
                                                  QVector3D(0.10f, 0.13f, 0.17f), abyssDark);
                    chunkBoulders.push_back({center, rx, ry, rz, seed + static_cast<uint32_t>(ci * 37 + b * 13), col});

                    float colR = std::max({rx, ry, rz}) * 1.06f;
                    tmp.spheres.push_back({center, colR});
                }

                for (const auto& bs : chunkBoulders) {
                    appendRockBoulder(tmp.verts, tmp.boulder0Inds, bs.center, bs.rx, bs.ry, bs.rz, 8, 10, bs.rSeed, bs.color);
                }
                for (const auto& bs : chunkBoulders) {
                    appendRockBoulder(tmp.verts, tmp.boulder1Inds, bs.center, bs.rx, bs.ry, bs.rz, 4, 6, bs.rSeed, bs.color);
                }

                // ------------------------------------------------------------
                // 2. Густые леса гигантской ламинарии (LOD0/LOD1)
                // ------------------------------------------------------------
                struct KelpSpec {
                    QVector3D root;
                    float height;
                    uint32_t kSeed;
                };
                std::vector<KelpSpec> chunkKelp;

                float forestNoise = ProcMath::fbm(chunkCenterX * 0.0085f, 0.0f, chunkCenterZ * 0.0085f, 2, seed + 701u);
                if (centerWallF < 0.52f && forestNoise > -0.35f) {
                    int kelpCount = (forestNoise > 0.12f) ? 28 : 14;
                    for (int k = 0; k < kelpCount; ++k) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, k * 5 + 1, seed + 307u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, k * 5 + 2, seed + 307u);
                        float hh = ProcMath::hash3(chunk.cx, chunk.cz, k * 5 + 3, seed + 307u);

                        float wx = originX + 2.5f + hx * (CHUNK_SZ - 5.0f);
                        float wz = originZ + 2.5f + hz * (CHUNK_SZ - 5.0f);
                        if (ProcMath::sampleBasinWallFactor(wx, wz, seed) > 0.42f) continue;

                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                        if (wy > -2.5f || wy < -34.0f) continue;

                        QVector3D n = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.4f);
                        if (n.y() < 0.62f) continue;

                        float maxH = std::min(21.0f, (ProcMath::WATER_SURFACE_Y - 4.5f) - wy);
                        if (maxH < 5.5f) continue;
                        float height = 6.5f + hh * (maxH - 6.5f);
                        chunkKelp.push_back({QVector3D(wx, wy - 0.35f, wz), height, seed + static_cast<uint32_t>(ci * 97 + k * 19)});
                    }
                }

                for (const auto& ks : chunkKelp) {
                    appendKelpStalk(tmp.verts, tmp.kelp0Inds, ks.root, ks.height, ks.kSeed, true);
                }
                for (const auto& ks : chunkKelp) {
                    appendKelpStalk(tmp.verts, tmp.kelp1Inds, ks.root, ks.height, ks.kSeed, false);
                }

                // ------------------------------------------------------------
                // 3. Разнообразная флора с LOD0/LOD1 + Островки зелени на плато у поверхности
                // ------------------------------------------------------------
                struct FloraSpec {
                    int kind; // 0=seagrass, 1=bushyAlgae, 2=staghorn, 3=tableCoral, 4=seaFan, 5=plateauGreenery
                    QVector3D pos;
                    QVector3D normal;
                    float scale;
                    float maxH;
                    uint32_t fSeed;
                };
                std::vector<FloraSpec> chunkFlora;

                if (centerWallF < 0.62f) {
                    float meadowNoise = ProcMath::fbm(chunkCenterX * 0.011f, 1.5f, chunkCenterZ * 0.011f, 2, seed + 881u);
                    int floraAttempts = (meadowNoise > -0.15f) ? 46 : 32;

                    for (int f = 0; f < floraAttempts; ++f) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 1, seed + 641u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 2, seed + 641u);
                        float hs = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 3, seed + 641u);
                        float hk = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 4, seed + 641u);

                        float wx = originX + 2.5f + hx * (CHUNK_SZ - 5.0f);
                        float wz = originZ + 2.5f + hz * (CHUNK_SZ - 5.0f);
                        if (ProcMath::sampleBasinWallFactor(wx, wz, seed) > 0.56f) continue;

                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                        if (wy < -32.0f || wy > 23.5f) continue;

                        QVector3D norm = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.4f);
                        float ny = norm.y();
                        uint32_t fSeed = seed + static_cast<uint32_t>(ci * 131 + f * 29);

                        if (ny > 0.78f && wy < 8.0f) {
                            if (hk < 0.54f) {
                                chunkFlora.push_back({0, QVector3D(wx, wy, wz), norm, 0.85f + hs * 0.75f, 4.0f, fSeed});
                            } else if (hk < 0.82f) {
                                chunkFlora.push_back({1, QVector3D(wx, wy, wz), norm, 0.80f + hs * 0.70f, 4.0f, fSeed});
                            } else {
                                chunkFlora.push_back({2, QVector3D(wx, wy, wz), norm, 0.85f + hs * 0.65f, 4.0f, fSeed});
                            }
                        } else if (ny > 0.46f) {
                            if (hk < 0.32f) {
                                chunkFlora.push_back({2, QVector3D(wx, wy, wz), norm, 0.85f + hs * 0.75f, 4.0f, fSeed});
                            } else if (hk < 0.58f) {
                                chunkFlora.push_back({3, QVector3D(wx, wy, wz), norm, 0.85f + hs * 0.70f, 4.0f, fSeed});
                            } else if (hk < 0.82f) {
                                chunkFlora.push_back({4, QVector3D(wx, wy, wz), norm, 0.80f + hs * 0.65f, 4.0f, fSeed});
                            } else {
                                chunkFlora.push_back({1, QVector3D(wx, wy, wz), norm, 0.75f + hs * 0.55f, 4.0f, fSeed});
                            }
                        } else if (ny > 0.24f && wy < 16.0f) {
                            if (hk < 0.58f) {
                                chunkFlora.push_back({4, QVector3D(wx, wy, wz), norm, 0.85f + hs * 0.75f, 4.0f, fSeed});
                            } else {
                                chunkFlora.push_back({3, QVector3D(wx, wy, wz), norm, 0.80f + hs * 0.65f, 4.0f, fSeed});
                            }
                        }
                    }
                }

                // 3б. Кластерные островки сочной подводной зелени на неглубоких плато у поверхности (y: +2 .. +26.2 м)
                if (centerWallF < 0.76f && chunk.maxBounds.y() > 31.0f) {
                    for (int isl = 0; isl < 12; ++isl) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, isl * 5 + 1, seed + 1811u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, isl * 5 + 2, seed + 1811u);
                        float hr = ProcMath::hash3(chunk.cx, chunk.cz, isl * 5 + 3, seed + 1811u);

                        float cxIsl = originX + 5.0f + hx * (CHUNK_SZ - 10.0f);
                        float czIsl = originZ + 5.0f + hz * (CHUNK_SZ - 10.0f);
                        if (ProcMath::sampleBasinWallFactor(cxIsl, czIsl, seed) > 0.72f) continue;

                        float cyIsl = ProcMath::sampleTerrainHeight(cxIsl, czIsl, seed);
                        if (cyIsl < 2.0f || cyIsl > 26.2f) continue;

                        QVector3D cNorm = ProcMath::sampleTerrainNormal(cxIsl, czIsl, seed, 1.3f);
                        if (cNorm.y() < 0.56f) continue;

                        float islMask = samplePlateauIslandMask(cxIsl, czIsl, seed);
                        if (islMask < 0.20f) continue;

                        // Собираем плотный живой островок из 6–9 кустов в радиусе 3.5..6.8 м
                        int tufts = 6 + static_cast<int>(hr * 3.9f);
                        float islandR = 3.6f + hr * 3.4f;
                        for (int t = 0; t < tufts; ++t) {
                            float th1 = ProcMath::hash3(ci, isl * 13 + t, 1, seed + 1823u);
                            float th2 = ProcMath::hash3(ci, isl * 13 + t, 2, seed + 1823u);
                            float th3 = ProcMath::hash3(ci, isl * 13 + t, 3, seed + 1823u);

                            float ang = th1 * 2.0f * PI;
                            float rad = std::sqrt(th2) * islandR;
                            float wx = cxIsl + std::cos(ang) * rad;
                            float wz = czIsl + std::sin(ang) * rad;
                            float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                            if (wy < 1.0f || wy > ProcMath::WATER_SURFACE_Y - 1.15f) continue;

                            QVector3D n = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.2f);
                            if (n.y() < 0.50f) continue;

                            float clearance = (ProcMath::WATER_SURFACE_Y - 0.85f) - wy;
                            uint32_t tSeed = seed + static_cast<uint32_t>(ci * 211 + isl * 37 + t * 7);

                            // В центре островка — пышные веерные кусты зелени, по краям — яркие кораллы/трава
                            if (t < tufts - 1 || th3 < 0.65f) {
                                float sc = (0.78f + th3 * 0.72f) * (1.12f - 0.30f * (rad / islandR));
                                chunkFlora.push_back({5, QVector3D(wx, wy, wz), n, sc, clearance, tSeed});
                            } else if (clearance > 1.8f) {
                                chunkFlora.push_back({2, QVector3D(wx, wy, wz), n, 0.70f + th3 * 0.45f, clearance, tSeed});
                            }
                        }
                    }
                }

                auto emitFlora = [&](std::vector<uint32_t>& outInds, bool highLod) {
                    for (const auto& fs : chunkFlora) {
                        switch (fs.kind) {
                        case 0: appendSeagrassPatch(tmp.verts, outInds, fs.pos, fs.scale, fs.fSeed, highLod); break;
                        case 1: appendBushyAlgae(tmp.verts, outInds, fs.pos, fs.scale, fs.fSeed, highLod); break;
                        case 2: appendStaghornCoral(tmp.verts, outInds, fs.pos, fs.scale, fs.fSeed, highLod); break;
                        case 3: appendTableCoral(tmp.verts, outInds, fs.pos, fs.normal, fs.scale, fs.fSeed, highLod); break;
                        case 4: appendSeaFan(tmp.verts, outInds, fs.pos, fs.scale, fs.fSeed, highLod); break;
                        case 5: appendPlateauGreeneryTuft(tmp.verts, outInds, fs.pos, fs.scale, fs.maxH, fs.fSeed, highLod); break;
                        }
                    }
                };
                emitFlora(tmp.flora0Inds, true);
                emitFlora(tmp.flora1Inds, false);

// ------------------------------------------------------------
                // 4. Детальная ближняя флора: трубчатые губки, морские анемоны и абиссальные перья
                // ------------------------------------------------------------
                if (centerWallF < 0.64f) {
                    for (int c = 0; c < 18; ++c) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, c * 4 + 1, seed + 509u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, c * 4 + 2, seed + 509u);
                        float ht = ProcMath::hash3(chunk.cx, chunk.cz, c * 4 + 3, seed + 509u);

                        float wx = originX + 3.0f + hx * (CHUNK_SZ - 6.0f);
                        float wz = originZ + 3.0f + hz * (CHUNK_SZ - 6.0f);
                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                        uint32_t dSeed = seed + static_cast<uint32_t>(ci * 173 + c * 31);

                        if (wy <= -30.0f && wy >= -72.0f) {
                            QVector3D n = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.4f);
                            if (n.y() > 0.45f) {
                                appendAbyssalSeaPen(tmp.verts, tmp.detailInds, QVector3D(wx, wy, wz), 0.85f + ht * 0.75f, dSeed);
                            }
                            continue;
                        }

                        if (wy > 24.5f) continue;

                        // Защита от острых пиков, краёв обрывов и слишком крутых склонов
                        QVector3D cNorm = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.2f);
                        if (cNorm.y() < 0.75f) continue;

                        constexpr float checkR = 1.15f;
                        float hL = ProcMath::sampleTerrainHeight(wx - checkR, wz, seed);
                        float hR = ProcMath::sampleTerrainHeight(wx + checkR, wz, seed);
                        float hD = ProcMath::sampleTerrainHeight(wx, wz - checkR, seed);
                        float hU = ProcMath::sampleTerrainHeight(wx, wz + checkR, seed);
                        float avgH = 0.25f * (hL + hR + hD + hU);
                        float hMin = std::min({wy, hL, hR, hD, hU});
                        float hMax = std::max({wy, hL, hR, hD, hU});

                        // Отсекаем острые вершины (центр заметно выше окружения) и резкие обрывы
                        if ((wy - avgH) > 0.26f || std::max(wy - hMin, hMax - wy) > 1.25f) continue;

                        if (c % 2 == 0) {
                            QVector3D col = (ht < 0.35f) ? QVector3D(0.92f, 0.34f, 0.46f)
                                            : (ht < 0.70f) ? QVector3D(0.78f, 0.30f, 0.86f)
                                                           : QVector3D(0.96f, 0.66f, 0.22f);
                            int tubes = 3 + (c % 3);
                            for (int t = 0; t < tubes; ++t) {
                                float ang = (2.0f * PI * t) / tubes + ht * 6.0f;
                                float offR = 0.30f + 0.32f * (t % 2);
                                float hTube = 1.05f + ht * 1.65f + (t % 2) * 0.45f;
                                float rTube = 0.21f + ht * 0.13f;

                                float tx = wx + std::cos(ang) * offR;
                                float tz = wz + std::sin(ang) * offR;
                                float ty = ProcMath::sampleTerrainHeight(tx, tz, seed);

                                // Замеряем минимальную высоту грунта по периметру конкретной трубки
                                float sampleR = rTube * 1.25f;
                                float tMin = std::min({
                                    ty,
                                    ProcMath::sampleTerrainHeight(tx - sampleR, tz, seed),
                                    ProcMath::sampleTerrainHeight(tx + sampleR, tz, seed),
                                    ProcMath::sampleTerrainHeight(tx, tz - sampleR, seed),
                                    ProcMath::sampleTerrainHeight(tx, tz + sampleR, seed)
                                });

                                // Погружаем основание глубоко в скалу с учётом уклона
                                float sinkDepth = 0.45f + (1.0f - cNorm.y()) * 1.35f;
                                float baseY  = tMin - sinkDepth;
                                float topY   = std::min(ty + hTube, ProcMath::WATER_SURFACE_Y - 0.8f);
                                float totalH = topY - baseY;
                                if (totalH < 0.6f) continue;

                                uint32_t vb = static_cast<uint32_t>(tmp.verts.size());
                                constexpr int SIDES = 6;
                                for (int ring = 0; ring <= 2; ++ring) {
                                    float rf = ring * 0.5f;
                                    float yPos = baseY + rf * totalH;
                                    float rad = rTube * (0.92f + rf * 0.30f);
                                    // Подземная часть трубки не колышется — остаётся намертво вросшей в породу
                                    float sway = ProcMath::clamp((yPos - ty) / std::max(hTube, 0.2f), 0.0f, 1.0f) * 0.35f;
                                    for (int s = 0; s <= SIDES; ++s) {
                                        float sa = (2.0f * PI * s) / SIDES;
                                        QVector3D n(std::cos(sa), 0.2f, std::sin(sa));
                                        n.normalize();
                                        QVector3D p(tx + n.x() * rad, yPos, tz + n.z() * rad);
                                        tmp.verts.push_back({p, n, col * (0.85f + rf * 0.25f), QVector2D(2.0f, sway)});
                                    }
                                }
                                for (int ring = 0; ring < 2; ++ring) {
                                    for (int s = 0; s < SIDES; ++s) {
                                        uint32_t i00 = vb + ring * (SIDES + 1) + s;
                                        uint32_t i10 = i00 + 1;
                                        uint32_t i01 = i00 + (SIDES + 1);
                                        uint32_t i11 = i01 + 1;
                                        tmp.detailInds.insert(tmp.detailInds.end(), {i00, i10, i01, i10, i11, i01});
                                    }
                                }
                            }
                        } else {
                            // Погружаем ножку анемона чуть глубже на склонах
                            float anemSink = 0.22f + (1.0f - cNorm.y()) * 0.75f;
                            appendSeaAnemone(tmp.verts, tmp.detailInds, QVector3D(wx, wy - anemSink, wz), 0.75f + ht * 0.65f, dSeed);
                        }
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    size_t totalVerts = 0;
    size_t totalInds  = 0;
    for (const auto& t : chunkTemps) {
        totalVerts += t.verts.size();
        totalInds  += t.boulder0Inds.size() + t.boulder1Inds.size()
                    + t.kelp0Inds.size()    + t.kelp1Inds.size()
                    + t.flora0Inds.size()   + t.flora1Inds.size()
                    + t.detailInds.size();
    }
    data.propVertices.reserve(totalVerts + 80000);
    data.propIndices.reserve(totalInds + 140000);

    auto appendRange = [&](const std::vector<uint32_t>& src, uint32_t vBase, MeshRange& dstRange) {
        dstRange.indexOffset = static_cast<uint32_t>(data.propIndices.size());
        dstRange.indexCount  = static_cast<uint32_t>(src.size());
        for (uint32_t idx : src) {
            data.propIndices.push_back(vBase + idx);
        }
    };

    for (int ci = 0; ci < TOTAL_CHUNKS; ++ci) {
        WorldChunk& chunk = data.chunks[ci];
        const ChunkPropTemp& tmp = chunkTemps[ci];

        uint32_t vBase = static_cast<uint32_t>(data.propVertices.size());
        data.propVertices.insert(data.propVertices.end(), tmp.verts.begin(), tmp.verts.end());

        appendRange(tmp.boulder0Inds, vBase, chunk.boulderLod0);
        appendRange(tmp.boulder1Inds, vBase, chunk.boulderLod1);
        appendRange(tmp.kelp0Inds,    vBase, chunk.kelpLod0);
        appendRange(tmp.kelp1Inds,    vBase, chunk.kelpLod1);
        appendRange(tmp.flora0Inds,   vBase, chunk.floraLod0);
        appendRange(tmp.flora1Inds,   vBase, chunk.floraLod1);
        appendRange(tmp.detailInds,   vBase, chunk.detailRange);

        for (const auto& sph : tmp.spheres) {
            uint32_t colId = static_cast<uint32_t>(data.collision.spheres.size());
            data.collision.spheres.push_back(sph);
            chunk.sphereColliderIds.push_back(colId);
        }
    }
}

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

// ============================================================================
// 5 ТИПОВ ЗАТОНУВШИХ КОРАБЛЕЙ: ИЗОГНУТЫЕ ШПАНГОУТЫ, НОС, КОРМА, ЮТ И ПРОБОИНЫ
// ============================================================================

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
                float ang = (2.0f * PI * s) / static_cast<float>(NUM_SHIPS)
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

        float yaw   = (s == 0) ? 0.65f : (ProcMath::hash3(s, 5, 6, seed) * 2.0f * PI);
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

            buildShipSection(bowAnchor,   r1, u1, f1, hp,  0.04f, 1.0f, true,  false, false, true,  seed + s * 97u);
            buildShipSection(sternAnchor, r2, u2, f2, hp, -1.0f, -0.04f, false, true,  true,  false, seed + s * 97u + 19u);

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

void WorldGenerator::generateFaunaMeshes(WorldData& data) {
    auto buildFish = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds, int rings, int segs, bool withFins) {
        for (int i = 0; i <= rings; ++i) {
            float t = static_cast<float>(i) / rings;
            float z = 0.65f - t * 1.40f;
            float profile = std::sin(t * PI);
            float rx = 0.16f * std::pow(profile, 0.75f);
            float ry = 0.28f * std::pow(profile, 0.65f);

            QVector3D bodyCol = ProcMath::mix(QVector3D(0.98f, 0.72f, 0.18f), QVector3D(0.18f, 0.84f, 0.96f), t);

            for (int j = 0; j <= segs; ++j) {
                float a = (2.0f * PI * j) / segs;
                float cx = std::cos(a), sy = std::sin(a);
                QVector3D p(cx * rx, sy * ry, z);
                QVector3D n(cx / std::max(rx, 0.02f), sy / std::max(ry, 0.02f), (0.5f - t) * 0.6f);
                n.normalize();
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

        uint32_t tb = static_cast<uint32_t>(verts.size());
        QVector3D tailCol(0.98f, 0.44f, 0.16f);
        verts.push_back({QVector3D(0.0f,  0.0f, -0.68f), QVector3D(1, 0, 0), tailCol});
        verts.push_back({QVector3D(0.0f,  0.34f, -1.08f), QVector3D(1, 0, 0), tailCol});
        verts.push_back({QVector3D(0.0f, -0.34f, -1.08f), QVector3D(1, 0, 0), tailCol});
        verts.push_back({QVector3D(0.0f,  0.0f, -0.92f), QVector3D(1, 0, 0), tailCol});
        inds.insert(inds.end(), {tb, tb + 1, tb + 3, tb, tb + 3, tb + 2});

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

    auto buildManta = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds, int spanSegs, int chordSegs) {
        QVector3D dorsalCol(0.10f, 0.18f, 0.28f);
        QVector3D bellyCol(0.86f, 0.92f, 0.96f);

        auto addSurface = [&](float ySign) {
            uint32_t base = static_cast<uint32_t>(verts.size());
            for (int ix = 0; ix <= spanSegs; ++ix) {
                float u = (static_cast<float>(ix) / spanSegs) * 2.0f - 1.0f;
                float absU = std::abs(u);
                float x = u * 2.15f;
                float leadZ = 0.85f - absU * 0.72f;
                float trailZ = -1.05f + absU * 0.68f;
                for (int iz = 0; iz <= chordSegs; ++iz) {
                    float v = static_cast<float>(iz) / chordSegs;
                    float z = ProcMath::mix(leadZ, trailZ, v);
                    float thickness = (1.0f - absU * 0.88f) * std::sin(v * PI) * 0.22f;
                    float y = ySign * thickness;
                    QVector3D n(u * 0.18f, ySign, (v - 0.5f) * 0.15f);
                    n.normalize();
                    verts.push_back({QVector3D(x, y, z), n, (ySign > 0.0f) ? dorsalCol : bellyCol});
                }
            }
            for (int ix = 0; ix < spanSegs; ++ix) {
                for (int iz = 0; iz < chordSegs; ++iz) {
                    uint32_t i00 = base + ix * (chordSegs + 1) + iz;
                    uint32_t i10 = i00 + (chordSegs + 1);
                    uint32_t i01 = i00 + 1;
                    uint32_t i11 = i10 + 1;
                    if (ySign > 0.0f) inds.insert(inds.end(), {i00, i01, i10, i10, i01, i11});
                    else              inds.insert(inds.end(), {i00, i10, i01, i10, i11, i01});
                }
            }
        };

        addSurface( 1.0f);
        addSurface(-1.0f);

        uint32_t tb = static_cast<uint32_t>(verts.size());
        verts.push_back({QVector3D(-0.05f, 0.0f, -0.95f), QVector3D(0, 1, 0), dorsalCol});
        verts.push_back({QVector3D( 0.05f, 0.0f, -0.95f), QVector3D(0, 1, 0), dorsalCol});
        verts.push_back({QVector3D( 0.0f,  0.0f, -2.95f), QVector3D(0, 1, 0), dorsalCol});
        inds.insert(inds.end(), {tb, tb + 1, tb + 2});
    };

    buildManta(data.mantaLod0Verts, data.mantaLod0Inds, 12, 6);
    buildManta(data.mantaLod1Verts, data.mantaLod1Inds, 6, 3);
}

void WorldGenerator::generateWhaleMeshes(WorldData& data) {
    auto buildWhale = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds, int bodySegs, int radialSegs) {
        QVector3D backCol(0.11f, 0.16f, 0.24f);
        QVector3D bellyCol(0.72f, 0.78f, 0.84f);

        for (int i = 0; i <= bodySegs; ++i) {
            float t = static_cast<float>(i) / bodySegs;
            float z = 4.8f - t * 10.0f;

            float width, height;
            if (t < 0.28f) {
                float u = t / 0.28f;
                width  = 0.12f + 1.22f * std::pow(std::sin(u * PI * 0.5f), 0.75f);
                height = 0.14f + 1.08f * std::pow(std::sin(u * PI * 0.5f), 0.70f);
            } else {
                float u = (t - 0.28f) / 0.72f;
                width  = 1.34f * std::pow(1.0f - u, 1.35f) + 0.14f;
                height = 1.22f * std::pow(1.0f - u, 1.15f) + 0.18f;
            }

            for (int j = 0; j <= radialSegs; ++j) {
                float ang = (2.0f * PI * j) / radialSegs;
                float ca = std::cos(ang), sa = std::sin(ang);
                float yOff = (sa < 0.0f && t < 0.55f) ? (sa * height * 1.12f) : (sa * height);
                QVector3D p(ca * width, yOff, z);
                QVector3D n(ca / std::max(width, 0.08f), sa / std::max(height, 0.08f), (0.35f - t) * 0.2f);
                n.normalize();

                float bellyMask = ProcMath::smoothstep(-0.15f, -0.65f, sa) * (1.0f - ProcMath::smoothstep(0.55f, 0.85f, t));
                float pleats = (bellyMask > 0.2f) ? (0.94f + 0.06f * std::sin(ang * 14.0f)) : 1.0f;
                QVector3D col = ProcMath::mix(backCol, bellyCol * pleats, bellyMask);
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

        auto addWingQuad = [&](const QVector3D& p0, const QVector3D& p1, const QVector3D& p2, const QVector3D& p3,
                               const QVector3D& n, const QVector3D& col) {
            uint32_t b = static_cast<uint32_t>(verts.size());
            verts.push_back({p0, n, col});
            verts.push_back({p1, n, col});
            verts.push_back({p2, n, col});
            verts.push_back({p3, n, col});
            inds.insert(inds.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
            inds.insert(inds.end(), {b, b + 2, b + 1, b, b + 3, b + 2});
        };

        for (float s : {-1.0f, 1.0f}) {
            QVector3D r0(s * 1.05f, -0.38f,  2.15f);
            QVector3D r1(s * 1.02f, -0.42f,  0.85f);
            QVector3D m0(s * 2.65f, -0.55f,  1.25f);
            QVector3D m1(s * 2.45f, -0.58f,  0.25f);
            QVector3D tip(s * 4.25f, -0.68f, -0.45f);
            addWingQuad(r0, m0, m1, r1, QVector3D(0.0f, 1.0f, 0.0f), backCol * 1.15f);
            uint32_t tb = static_cast<uint32_t>(verts.size());
            verts.push_back({m0, QVector3D(0, 1, 0), bellyCol * 0.85f});
            verts.push_back({tip, QVector3D(0, 1, 0), bellyCol * 0.95f});
            verts.push_back({m1, QVector3D(0, 1, 0), bellyCol * 0.85f});
            inds.insert(inds.end(), {tb, tb + 1, tb + 2, tb, tb + 2, tb + 1});
        }

        for (float s : {-1.0f, 1.0f}) {
            QVector3D f0(0.0f,      0.0f, -4.95f);
            QVector3D f1(s * 1.35f, 0.04f, -5.45f);
            QVector3D f2(s * 2.95f, 0.02f, -6.25f);
            QVector3D f3(s * 0.25f, 0.0f, -5.65f);
            addWingQuad(f0, f1, f2, f3, QVector3D(0.0f, 1.0f, 0.0f), backCol * 1.1f);
        }

        addWingQuad(QVector3D(0.0f, 0.65f, -1.35f),
                    QVector3D(0.0f, 1.18f, -2.05f),
                    QVector3D(0.0f, 0.82f, -2.35f),
                    QVector3D(0.0f, 0.52f, -2.25f),
                    QVector3D(1.0f, 0.0f, 0.0f), backCol);
    };

    buildWhale(data.whaleLod0Verts, data.whaleLod0Inds, 22, 14);
    buildWhale(data.whaleLod1Verts, data.whaleLod1Inds, 9, 7);
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

            float env = std::sin(std::pow(t, 0.52f) * PI);
            float rx = 0.08f + 0.70f * std::pow(env, 0.85f) * (1.0f - 0.35f * t);
            float ryUp = 0.08f + 0.56f * std::pow(env, 0.80f);
            float ryDown = 0.08f + 0.74f * std::pow(env, 0.75f) * (1.0f - 0.28f * t);

            for (int j = 0; j <= radialSegs; ++j) {
                float a = (2.0f * PI * j) / radialSegs;
                float ca = std::cos(a), sa = std::sin(a);
                float ry = (sa >= 0.0f) ? ryUp : ryDown;

                float zMod = z;
                QVector3D col = ProcMath::mix(skinMottled, skinDark, sa * 0.5f + 0.5f);
                if (t < 0.22f && sa < 0.15f && sa > -0.78f) {
                    zMod -= (0.22f - t) * 1.15f;
                    col = mouthDark;
                }

                float bump = 1.0f + 0.05f * std::sin(a * 5.0f + t * 14.0f);
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

        for (float side : {-1.0f, 1.0f}) {
            appendFaunaSphere(verts, inds, QVector3D(side * 0.42f, 0.28f, 0.46f),
                              QVector3D(0.085f, 0.085f, 0.085f), highLod ? 6 : 4, highLod ? 8 : 5, eyeGlow);
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
                float a = (2.0f * PI * f) / 4.0f + 0.4f;
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

void WorldGenerator::generateFaunaAgents(WorldData& data, uint32_t seed) {
    const QVector3D schoolTints[] = {
        {1.10f, 0.96f, 0.82f},
        {0.56f, 1.12f, 1.22f},
        {1.22f, 0.58f, 0.72f},
        {0.78f, 1.18f, 0.68f},
        {1.18f, 0.94f, 0.38f}
    };

    for (int sc = 0; sc < 28; ++sc) {
        float sx, sz;
        if (sc < 3) {
            float a = sc * 2.1f;
            sx = std::cos(a) * 28.0f;
            sz = 10.0f + std::sin(a) * 28.0f;
        } else {
            float a = ProcMath::hash3(sc, 1, 2, seed + 811u) * 2.0f * PI;
            float r = 65.0f + ProcMath::hash3(sc, 3, 4, seed + 811u) * 380.0f;
            sx = std::cos(a) * r;
            sz = std::sin(a) * r;
        }
        float floorY = ProcMath::sampleTerrainHeight(sx, sz, seed);
        float safeFloor = std::max(floorY, -24.0f);
        float baseY = ProcMath::clamp(safeFloor + 7.5f + ProcMath::hash3(sc, 5, 6, seed) * 10.0f, -18.0f, 16.0f);
        QVector3D schoolCenter(sx, baseY, sz);
        QVector3D tint = schoolTints[sc % 5];

        for (int f = 0; f < 24; ++f) {
            float h1 = ProcMath::hash3(sc, f, 1, seed + 823u);
            float h2 = ProcMath::hash3(sc, f, 2, seed + 823u);
            float h3 = ProcMath::hash3(sc, f, 3, seed + 823u);

            float ang = h1 * 2.0f * PI;
            float rad = 3.0f + h2 * 14.0f;
            QVector3D pos(sx + std::cos(ang) * rad, baseY + (h3 - 0.5f) * 6.0f, sz + std::sin(ang) * rad);

            data.collision.resolveSphereCollision(pos, nullptr, 1.2f, 1.5f, 3.0f);

            FaunaAgent agent;
            agent.pos = pos;
            agent.homeCenter = schoolCenter;
            agent.homeRadius = 22.0f + h2 * 16.0f;
            agent.preferredDepth = baseY;
            agent.cruiseSpeed = 4.2f + h3 * 2.6f;
            agent.scale = 0.55f + h1 * 0.55f;
            agent.boundingRadius = 0.85f * agent.scale;
            agent.phase = h1 * 12.0f;
            agent.type = 0.0f;
            agent.tint = tint;
            agent.vel = QVector3D(-std::sin(ang), 0.0f, std::cos(ang)) * agent.cruiseSpeed;
            agent.yaw = std::atan2(agent.vel.x(), agent.vel.z());
            data.fishAgents.push_back(agent);
        }
    }

    for (int m = 0; m < 26; ++m) {
        float h1 = ProcMath::hash3(m, 1, 7, seed + 907u);
        float h2 = ProcMath::hash3(m, 2, 8, seed + 907u);
        float h3 = ProcMath::hash3(m, 3, 9, seed + 907u);

        float a = (2.0f * PI * m) / 26.0f + h1 * 0.35f;
        float dist = (m < 3) ? (35.0f + m * 14.0f) : (75.0f + h2 * 365.0f);
        float mx = std::cos(a) * dist;
        float mz = std::sin(a) * dist;
        float floorY = ProcMath::sampleTerrainHeight(mx, mz, seed);
        float my = ProcMath::clamp(std::max(floorY + 10.0f, -2.0f + h3 * 16.0f), -10.0f, 20.0f);

        QVector3D pos(mx, my, mz);
        data.collision.resolveSphereCollision(pos, nullptr, 3.6f, 2.5f, 4.0f);

        FaunaAgent manta;
        manta.pos = pos;
        manta.homeCenter = QVector3D(mx, my, mz);
        manta.homeRadius = 55.0f + h1 * 50.0f;
        manta.preferredDepth = my;
        manta.cruiseSpeed = 4.6f + h2 * 2.2f;
        manta.scale = 1.15f + h3 * 0.75f;
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

        float ang = h1 * 2.0f * PI;
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
    for (int i = 0; i < 140; ++i) {
        float h1 = ProcMath::hash3(i, 1, 2, seed + 601u);
        float h2 = ProcMath::hash3(i, 3, 4, seed + 601u);
        float h3 = ProcMath::hash3(i, 5, 6, seed + 601u);

        float ang = h1 * 2.0f * PI;
        float dist = std::sqrt(h2) * 470.0f;
        float x = std::cos(ang) * dist;
        float z = std::sin(ang) * dist;
        float floorY = ProcMath::sampleTerrainHeight(x, z, seed);
        if (floorY > 14.0f) continue;

        float topY = ProcMath::WATER_SURFACE_Y;
        float botY = std::max(floorY - 2.0f, -34.0f);
        float width = 3.8f + h3 * 5.2f;
        QVector3D slant(-0.28f, 0.92f, -0.22f);
        slant.normalize();

        QVector3D topCenter(x, topY, z);
        QVector3D botCenter = topCenter - slant * ((topY - botY) / slant.y());
        QVector3D side(std::cos(h3 * PI), 0.0f, std::sin(h3 * PI));
        side *= width;

        QVector4D col(0.45f, 0.95f, 1.0f, 0.095f);
        RayVertex v0{botCenter - side, col, {0.0f, 0.0f}};
        RayVertex v1{botCenter + side, col, {1.0f, 0.0f}};
        RayVertex v2{topCenter + side, col, {1.0f, 1.0f}};
        RayVertex v3{topCenter - side, col, {0.0f, 1.0f}};
        data.godRayVertices.insert(data.godRayVertices.end(), {v0, v1, v2, v0, v2, v3});
    }

    data.particles.reserve(2200);
    for (int i = 0; i < 2200; ++i) {
        float hx = ProcMath::hash3(i, 1, 3, seed + 733u);
        float hy = ProcMath::hash3(i, 2, 4, seed + 733u);
        float hz = ProcMath::hash3(i, 3, 5, seed + 733u);
        float ht = ProcMath::hash3(i, 4, 6, seed + 733u);

        bool isBubble = (ht > 0.58f);
        Particle p;
        p.pos = QVector3D((hx - 0.5f) * 160.0f, -24.0f + hy * 50.0f, (hz - 0.5f) * 160.0f);
        if (isBubble) {
            float sz = -(0.14f + (ht - 0.58f) * 0.45f);
            p.colorSize = QVector4D(0.75f, 0.95f, 1.0f, sz);
            p.speedY = 1.6f + hy * 2.2f;
        } else {
            float sz = 0.16f + ht * 0.34f;
            p.colorSize = (ht < 0.28f) ? QVector4D(0.32f, 1.0f, 0.78f, sz)
                                       : QVector4D(0.45f, 0.75f, 1.0f, sz);
            p.speedY = 0.22f + hy * 0.45f;
        }
        p.phase = hx * 6.28318f;
        data.particles.push_back(p);
    }
}
