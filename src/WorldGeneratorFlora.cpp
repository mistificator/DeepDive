#include "WorldGenerator.h"
#include "Noise.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

// Маска органических «островков» сочной зелени на неглубоких плато у поверхности
inline float samplePlateauIslandMask(float wx, float wz, uint32_t seed) {
    float n1 = ProcMath::fbm(wx * 0.030f + 3.1f, 0.4f, wz * 0.030f - 1.7f, 3, seed + 1607u);
    float n2 = ProcMath::fbm(wx * 0.072f - 2.3f, 1.1f, wz * 0.072f + 4.9f, 2, seed + 1621u);
    return ProcMath::smoothstep(0.02f, 0.30f, n1 * 0.78f + n2 * 0.22f);
}

// Маска естественных лужаек и карманов травы на надводных скалах и верхушках кекуров
inline float sampleCliffGrassPatchMask(float wx, float wz, uint32_t seed) {
    float n1 = ProcMath::fbm(wx * 0.024f - 4.7f, 0.6f, wz * 0.024f + 2.3f, 3, seed + 3701u);
    float n2 = ProcMath::fbm(wx * 0.068f + 1.9f, 1.3f, wz * 0.068f - 5.1f, 2, seed + 3719u);
    return ProcMath::smoothstep(-0.04f, 0.32f, n1 * 0.72f + n2 * 0.28f);
}

// Согласованная с InstPropVert ориентация: направляет локальную ось +Y вдоль единичного вектора dir
inline void alignUpAxisToDir(const QVector3D& dir, float& outYaw, float& outPitch) {
    QVector3D d = dir.normalized();
    outPitch = std::acos(ProcMath::clamp(d.y(), -1.0f, 1.0f));
    float hLen = std::sqrt(d.x() * d.x() + d.z() * d.z());
    outYaw = (hLen > 1e-4f) ? std::atan2(-d.x(), -d.z()) : 0.0f;
}

// Согласованная с InstPropVert ориентация: направляет локальную ось +Z вдоль единичного вектора dir
inline void alignForwardAxisToDir(const QVector3D& dir, float& outYaw, float& outPitch) {
    QVector3D d = dir.normalized();
    outPitch = std::asin(ProcMath::clamp(d.y(), -1.0f, 1.0f));
    float hLen = std::sqrt(d.x() * d.x() + d.z() * d.z());
    outYaw = (hLen > 1e-4f) ? std::atan2(d.x(), d.z()) : 0.0f;
}

// Вспомогательная трубка / ветвь с замкнутой шапочкой на кончике (для кораллов и стеблей)
void appendTaperedTube(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                       const QVector3D& p0, const QVector3D& p1,
                       float r0, float r1, int sides,
                       const QVector3D& col0, const QVector3D& col1,
                       float matType, float sway0, float sway1, bool capTip = true) {
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
        float a = (TWO_PI * s) / sides;
        QVector3D rad = u * std::cos(a) + v * std::sin(a);
        verts.push_back({p0 + rad * r0, rad, col0, QVector2D(matType, sway0)});
    }
    uint32_t b1 = static_cast<uint32_t>(verts.size());
    for (int s = 0; s <= sides; ++s) {
        float a = (TWO_PI * s) / sides;
        QVector3D rad = u * std::cos(a) + v * std::sin(a);
        QVector3D n = (rad + dir * 0.30f).normalized();
        verts.push_back({p1 + rad * r1, n, col1, QVector2D(matType, sway1)});
    }
    for (int s = 0; s < sides; ++s) {
        inds.insert(inds.end(), {b0 + s, b0 + s + 1, b1 + s, b0 + s + 1, b1 + s + 1, b1 + s});
    }

    if (capTip && r1 > 0.004f) {
        uint32_t tipIdx = static_cast<uint32_t>(verts.size());
        verts.push_back({p1 + dir * (r1 * 0.85f), dir, col1 * 1.06f, QVector2D(matType, sway1)});
        for (int s = 0; s < sides; ++s) {
            inds.insert(inds.end(), {b1 + s, b1 + s + 1, tipIdx});
        }
    }
}

// Компактный светящийся пузырёк (октаэдр — 6 вершин, 8 треугольников) для колонн ламинарии
void appendGlowOctahedron(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                          const QVector3D& center, float radius,
                          const QVector3D& col, float matType, float swayY) {
    uint32_t b = static_cast<uint32_t>(verts.size());
    const QVector3D dirs[6] = {
        { 0.0f,  1.0f,  0.0f}, { 1.0f,  0.0f,  0.0f}, { 0.0f,  0.0f,  1.0f},
        {-1.0f,  0.0f,  0.0f}, { 0.0f,  0.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}
    };
    for (const auto& d : dirs) {
        verts.push_back({center + d * radius, d, col, QVector2D(matType, swayY)});
    }
    const uint32_t tris[24] = {
        0, 2, 1,  0, 3, 2,  0, 4, 3,  0, 1, 4,
        5, 1, 2,  5, 2, 3,  5, 3, 4,  5, 4, 1
    };
    for (uint32_t idx : tris) inds.push_back(b + idx);
}

// ============================================================================
// ПРОТОТИП 0: ГРАНЁНЫЙ СЛОИСТЫЙ ВАЛУН (PROP_BOULDER_CHUNK)
// ============================================================================

void buildBoulderPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                           int stacks, int slices, uint32_t rockSeed) {
    // 10 случайных плоскостей скола формируют естественные каменные грани
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

    std::vector<QVector3D> pts((stacks + 1) * (slices + 1));
    std::vector<float> cavity((stacks + 1) * (slices + 1), 1.0f);

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
            pts[st * (slices + 1) + sl] = QVector3D(dir.x() * r, dir.y() * r * yMod, dir.z() * r);
            cavity[st * (slices + 1) + sl] = 0.58f + 0.42f * crackMask;
        }
    }

    // Взвешенные по площади внешние нормали (строго согласованы с CCW обходом наружу)
    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    std::vector<QVector3D> accumN((stacks + 1) * (slices + 1), QVector3D(0.0f, 0.0f, 0.0f));
    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            int i00 = st * (slices + 1) + sl;
            int i10 = i00 + 1;
            int i01 = i00 + (slices + 1);
            int i11 = i01 + 1;
            QVector3D fn1 = QVector3D::crossProduct(pts[i10] - pts[i00], pts[i01] - pts[i00]);
            QVector3D fn2 = QVector3D::crossProduct(pts[i11] - pts[i10], pts[i01] - pts[i10]);
            accumN[i00] += fn1; accumN[i10] += fn1 + fn2; accumN[i01] += fn1 + fn2; accumN[i11] += fn2;
        }
    }
    for (int st = 0; st <= stacks; ++st) {
        int a = st * (slices + 1);
        int b = a + slices;
        QVector3D merged = accumN[a] + accumN[b];
        accumN[a] = accumN[b] = merged;
    }

    for (int st = 0; st <= stacks; ++st) {
        for (int sl = 0; sl <= slices; ++sl) {
            int id = st * (slices + 1) + sl;
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
            uint32_t i00 = baseIdx + st * (slices + 1) + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + (slices + 1);
            uint32_t i11 = i01 + 1;
            inds.insert(inds.end(), {i00, i10, i01, i10, i11, i01});
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

    std::vector<QVector3D> pts((stacks + 1) * (slices + 1));
    std::vector<float> cavity((stacks + 1) * (slices + 1), 1.0f);

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
            pts[st * (slices + 1) + sl] = QVector3D(x, y, z);
            cavity[st * (slices + 1) + sl] = 0.55f + 0.45f * crackMask;
        }
    }

    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    std::vector<QVector3D> accumN((stacks + 1) * (slices + 1), QVector3D(0.0f, 0.0f, 0.0f));
    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            int i00 = st * (slices + 1) + sl;
            int i10 = i00 + 1;
            int i01 = i00 + (slices + 1);
            int i11 = i01 + 1;
            QVector3D fn1 = QVector3D::crossProduct(pts[i10] - pts[i00], pts[i01] - pts[i00]);
            QVector3D fn2 = QVector3D::crossProduct(pts[i11] - pts[i10], pts[i01] - pts[i10]);
            accumN[i00] += fn1; accumN[i10] += fn1 + fn2; accumN[i01] += fn1 + fn2; accumN[i11] += fn2;
        }
    }
    for (int st = 0; st <= stacks; ++st) {
        int a = st * (slices + 1);
        int b = a + slices;
        QVector3D merged = accumN[a] + accumN[b];
        accumN[a] = accumN[b] = merged;
    }

    for (int st = 0; st <= stacks; ++st) {
        for (int sl = 0; sl <= slices; ++sl) {
            int id = st * (slices + 1) + sl;
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
            uint32_t i00 = baseIdx + st * (slices + 1) + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + (slices + 1);
            uint32_t i11 = i01 + 1;
            inds.insert(inds.end(), {i00, i10, i01, i10, i11, i01});
        }
    }
}

// ============================================================================
// ПРОТОТИП 2: ПЫШНАЯ КОЛОННА ЛАМИНАРИИ + СВЕТЯЩИЕСЯ ПУЗЫРЬКИ (PROP_KELP_COLUMN)
// ============================================================================

void buildKelpColumnPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    constexpr float CANONICAL_H = 14.0f;
    const QVector3D stipeBaseCol(0.14f, 0.34f, 0.16f);
    const QVector3D stipeTopCol(0.32f, 0.52f, 0.22f);
    const QVector3D leafBaseCol(0.16f, 0.40f, 0.20f);
    const QVector3D leafMidCol(0.32f, 0.56f, 0.22f);
    const QVector3D leafTipCol(0.62f, 0.70f, 0.26f);
    const QVector3D vesicleCol(0.92f, 1.05f, 0.68f);

    // 1. Центральный переплетённый стебель (2 жилы на LOD0, 1 на LOD1)
    int stipeCount = highLod ? 2 : 1;
    int stipeSegs  = highLod ? 14 : 6;
    int stipeSides = highLod ? 5 : 3;

    for (int st = 0; st < stipeCount; ++st) {
        float phaseOff = st * PI;
        float orbitR   = highLod ? 0.09f : 0.0f;
        QVector3D prevP(std::cos(phaseOff) * orbitR, -0.45f, std::sin(phaseOff) * orbitR);

        for (int s = 1; s <= stipeSegs; ++s) {
            float t0 = static_cast<float>(s - 1) / stipeSegs;
            float t1 = static_cast<float>(s) / stipeSegs;
            float y1 = -0.45f + t1 * (CANONICAL_H + 0.45f);
            float ang = phaseOff + t1 * 7.5f;
            float rad = orbitR * (1.0f - t1 * 0.45f);
            QVector3D curP(std::cos(ang) * rad, y1, std::sin(ang) * rad);

            float r0 = (highLod ? 0.115f : 0.16f) * (1.0f - t0 * 0.42f);
            float r1 = (highLod ? 0.115f : 0.16f) * (1.0f - t1 * 0.42f);
            appendTaperedTube(verts, inds, prevP, curP, r0, r1, stipeSides,
                              ProcMath::mix(stipeBaseCol, stipeTopCol, t0),
                              ProcMath::mix(stipeBaseCol, stipeTopCol, t1),
                              3.0f, t0, t1, (s == stipeSegs));
            prevP = curP;
        }
    }

    // 2. Пышная спираль широких изогнутых листьев (54 на LOD0, 18 на LOD1)
    const int leafCount = highLod ? 54 : 18;
    const int segs      = highLod ? 4  : 2;
    constexpr float GOLDEN_ANG = 2.39996323f;

    for (int i = 0; i < leafCount; ++i) {
        float t = 0.07f + (static_cast<float>(i) / static_cast<float>(leafCount - 1)) * 0.92f;
        float yRoot = t * CANONICAL_H;
        float h1 = ProcMath::hash3(i, 7, 13, 4409u);
        float h2 = ProcMath::hash3(i, 11, 19, 4409u);
        float h3 = ProcMath::hash3(i, 17, 23, 4409u);

        float yaw = static_cast<float>(i) * GOLDEN_ANG + (h1 - 0.5f) * 0.35f;
        QVector3D dir(std::cos(yaw), 0.0f, std::sin(yaw));
        QVector3D side(-dir.z(), 0.0f, dir.x());

        float canopyProfile = 0.68f + 0.48f * std::sin(std::pow(t, 0.72f) * PI * 0.92f);
        float leafLen = (1.65f + h2 * 1.15f) * canopyProfile;
        float leafMaxW = (highLod ? 0.36f : 0.58f) * (0.78f + h3 * 0.42f);
        float riseAmp = (0.42f + h1 * 0.52f) * (0.55f + 0.65f * t);
        float droopAmp = 0.38f + (1.0f - t) * 0.55f;

        QVector3D root = dir * 0.08f + QVector3D(0.0f, yRoot, 0.0f);
        QVector3D baseCol = ProcMath::mix(leafBaseCol, leafMidCol, t * 0.75f + h3 * 0.25f);
        QVector3D tipCol  = ProcMath::mix(leafMidCol, leafTipCol, 0.35f + 0.65f * t);

        if (highLod) {
            uint32_t vb = static_cast<uint32_t>(verts.size());
            for (int s = 0; s <= segs; ++s) {
                float u = static_cast<float>(s) / static_cast<float>(segs);
                float archY = std::sin(u * PI * 0.78f) * riseAmp - (u * u) * droopAmp;
                float reach = u * leafLen;
                float ripple = std::sin(u * 9.0f + i) * 0.06f * u;
                float w = leafMaxW * std::sin(std::pow(ProcMath::clamp(u, 0.04f, 0.98f), 0.65f) * PI);
                if (s == segs) w = 0.015f;

                QVector3D center = root + dir * reach + QVector3D(0.0f, archY, 0.0f) + side * ripple;
                float swayT = ProcMath::clamp((yRoot + archY * 0.5f) / CANONICAL_H, 0.02f, 1.0f);
                QVector3D col = ProcMath::mix(baseCol, tipCol, u);

                QVector3D nMid = (QVector3D(0.0f, 0.88f, 0.0f) + dir * 0.44f).normalized();
                QVector3D nL   = (nMid - side * 0.35f).normalized();
                QVector3D nR   = (nMid + side * 0.35f).normalized();

                verts.push_back({center - side * w + QVector3D(0.0f, 0.05f * w, 0.0f), nL, col * 0.94f, QVector2D(3.0f, swayT)});
                verts.push_back({center,                                                nMid, col * 1.06f, QVector2D(3.0f, swayT)});
                verts.push_back({center + side * w + QVector3D(0.0f, 0.05f * w, 0.0f), nR, col * 0.94f, QVector2D(3.0f, swayT)});
            }
            for (int s = 0; s < segs; ++s) {
                uint32_t r0 = vb + s * 3;
                uint32_t r1 = r0 + 3;
                inds.insert(inds.end(), {
                                            r0 + 0, r0 + 1, r1 + 0,  r0 + 1, r1 + 1, r1 + 0,
                                            r0 + 1, r0 + 2, r1 + 1,  r0 + 2, r1 + 2, r1 + 1
                                        });
            }
        } else {
            uint32_t vb = static_cast<uint32_t>(verts.size());
            QVector3D norm = (QVector3D(0.0f, 0.85f, 0.0f) + dir * 0.50f).normalized();
            for (int s = 0; s <= segs; ++s) {
                float u = static_cast<float>(s) / static_cast<float>(segs);
                float archY = std::sin(u * PI * 0.78f) * riseAmp - (u * u) * droopAmp;
                float w = leafMaxW * (s == segs ? 0.05f : std::sin(std::max(0.15f, u * 0.92f) * PI));
                QVector3D center = root + dir * (u * leafLen) + QVector3D(0.0f, archY, 0.0f);
                float swayT = ProcMath::clamp(yRoot / CANONICAL_H, 0.02f, 1.0f);
                QVector3D col = ProcMath::mix(baseCol, tipCol, u);
                verts.push_back({center - side * w, norm, col, QVector2D(3.0f, swayT)});
                verts.push_back({center + side * w, norm, col, QVector2D(3.0f, swayT)});
            }
            for (int s = 0; s < segs; ++s) {
                uint32_t i0 = vb + s * 2;
                inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
            }
        }
    }

    // 3. Светящиеся биолюминесцентные пузырьки (пневматоцисты) вдоль стебля и листьев (param.x = 5.0)
    const int vesicleCount = highLod ? 30 : 10;
    for (int v = 0; v < vesicleCount; ++v) {
        float t = 0.14f + (static_cast<float>(v) / static_cast<float>(vesicleCount - 1)) * 0.82f;
        float h1 = ProcMath::hash3(v, 31, 43, 4517u);
        float h2 = ProcMath::hash3(v, 37, 47, 4517u);
        float ang = v * GOLDEN_ANG * 1.37f + h1 * 1.2f;
        float rad = 0.24f + h2 * 0.88f;
        QVector3D pos(std::cos(ang) * rad, t * CANONICAL_H, std::sin(ang) * rad);
        float vRad = highLod ? (0.065f + h1 * 0.045f) : (0.105f + h1 * 0.04f);
        appendGlowOctahedron(verts, inds, pos, vRad, vesicleCol, 5.0f, t);
    }
}

// ============================================================================
// ПРОТОТИП 3: ТОЛСТОСТЕННАЯ ПОЛАЯ ГУБКА С ТЁМНЫМ НУТРОМ (PROP_BARREL_SPONGE)
// ============================================================================

void buildBarrelSpongePrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    const int slices = highLod ? 16 : 8;

    struct RingDef {
        float r;
        float y;
        float aoFactor;
        float bumpAmp;
        bool  isInner;
    };

    std::vector<RingDef> profile;
    if (highLod) {
        profile = {
            {0.46f, -0.32f, 0.56f, 0.05f, false},
            {0.58f,  0.05f, 0.72f, 0.10f, false},
            {0.70f,  0.48f, 0.88f, 0.14f, false},
            {0.72f,  0.94f, 0.96f, 0.14f, false},
            {0.66f,  1.32f, 1.02f, 0.12f, false},
            {0.58f,  1.46f, 1.08f, 0.10f, false},
            {0.47f,  1.45f, 0.96f, 0.09f, true},
            {0.41f,  1.22f, 0.62f, 0.08f, true},
            {0.34f,  0.76f, 0.36f, 0.06f, true},
            {0.24f,  0.26f, 0.22f, 0.04f, true}
        };
    } else {
        profile = {
            {0.48f, -0.28f, 0.60f, 0.05f, false},
            {0.72f,  0.65f, 0.92f, 0.12f, false},
            {0.62f,  1.42f, 1.05f, 0.10f, false},
            {0.44f,  1.40f, 0.85f, 0.08f, true},
            {0.28f,  0.45f, 0.25f, 0.04f, true}
        };
    }

    const int rings = static_cast<int>(profile.size());
    std::vector<QVector3D> pts(rings * (slices + 1));

    for (int r = 0; r < rings; ++r) {
        const auto& rd = profile[r];
        for (int sl = 0; sl <= slices; ++sl) {
            int slWrap = sl % slices;
            float a = TWO_PI * static_cast<float>(slWrap) / static_cast<float>(slices);
            float ca = std::cos(a), sa = std::sin(a);

            float nBump   = ProcMath::fbm(ca * 2.1f + 1.5f, rd.y * 2.4f, sa * 2.1f - 0.8f, 3, 5209u);
            float flute   = std::sin(a * 6.0f + rd.y * 2.0f) * 0.055f;
            float rimWave = (rd.y > 1.15f) ? (std::sin(a * 3.0f + 0.7f) * 0.06f + nBump * 0.09f) : 0.0f;

            float rad = rd.r * (1.0f + nBump * rd.bumpAmp + flute);
            pts[r * (slices + 1) + sl] = QVector3D(ca * rad, rd.y + rimWave, sa * rad);
        }
    }

    const QVector3D neutralWarm(0.84f, 0.78f, 0.74f);
    uint32_t baseIdx = static_cast<uint32_t>(verts.size());

    for (int r = 0; r < rings; ++r) {
        for (int sl = 0; sl <= slices; ++sl) {
            QVector3D p = pts[r * (slices + 1) + sl];
            int rPrev = std::max(0, r - 1), rNext = std::min(rings - 1, r + 1);
            int slL = (sl - 1 + slices) % slices, slR = (sl + 1) % slices;

            QVector3D tRing = pts[rNext * (slices + 1) + sl] - pts[rPrev * (slices + 1) + sl];
            QVector3D tAng  = pts[r * (slices + 1) + slR]    - pts[r * (slices + 1) + slL];
            QVector3D n = QVector3D::crossProduct(tRing, tAng);
            if (n.lengthSquared() < 1e-6f) n = QVector3D(0.0f, 1.0f, 0.0f);
            n.normalize();

            QVector3D col = neutralWarm * profile[r].aoFactor;
            verts.push_back({p, n, col, QVector2D(2.0f, 0.0f)});
        }
    }

    // Строго CCW обход, согласованный с внешними нормалями (и внутрь полости для внутренних стенок)
    for (int r = 0; r + 1 < rings; ++r) {
        for (int sl = 0; sl < slices; ++sl) {
            uint32_t i00 = baseIdx + r * (slices + 1) + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + (slices + 1);
            uint32_t i11 = i01 + 1;
            inds.insert(inds.end(), {i00, i01, i10, i10, i01, i11});
        }
    }

    // Тёмное дно внутри полости губки (CCW вверх)
    uint32_t centerBottom = static_cast<uint32_t>(verts.size());
    float innerFloorY = profile.back().y - 0.08f;
    verts.push_back({QVector3D(0.0f, innerFloorY, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
                     neutralWarm * 0.16f, QVector2D(2.0f, 0.0f)});
    uint32_t lastRing = baseIdx + (rings - 1) * (slices + 1);
    for (int sl = 0; sl < slices; ++sl) {
        inds.insert(inds.end(), {lastRing + sl, centerBottom, lastRing + sl + 1});
    }
}

// ============================================================================
// ПРОТОТИП 4: ПЛОТНЫЙ КОВЁР ПРИДОННОЙ ТРАВЫ (PROP_GROUND_CARPET)
// ============================================================================

void buildGroundCarpetPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    const int blades = highLod ? 26 : 9;
    const int segs   = highLod ? 3  : 2;

    const QVector3D rootCol(0.46f, 0.54f, 0.52f);
    const QVector3D tipCol(0.96f, 1.00f, 0.98f);

    for (int b = 0; b < blades; ++b) {
        float h1 = ProcMath::hash3(b, 1, 13, 6101u);
        float h2 = ProcMath::hash3(b, 2, 17, 6101u);
        float h3 = ProcMath::hash3(b, 3, 19, 6101u);
        float h4 = ProcMath::hash3(b, 4, 23, 6101u);

        float posAng = (TWO_PI * b) / blades + h1 * 0.7f;
        float posRad = std::sqrt((static_cast<float>(b) + 0.5f) / blades) * 2.15f;
        QVector3D root(std::cos(posAng) * posRad, -0.14f, std::sin(posAng) * posRad);

        float bladeAng = h2 * TWO_PI;
        bool tallRibbon = (b % 5 == 0);
        float height = tallRibbon ? (1.05f + h3 * 0.72f) : (0.48f + h3 * 0.58f);
        float width  = (highLod ? 0.11f : 0.24f) * (tallRibbon ? 1.15f : 0.95f);
        float lean   = (tallRibbon ? 0.45f : 0.26f) + h4 * 0.34f;

        QVector3D dir(std::cos(bladeAng), 0.0f, std::sin(bladeAng));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D norm = (QVector3D(0.0f, 0.70f, 0.0f) + dir * 0.70f).normalized();

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float w = width * (1.0f - t * 0.86f);
            QVector3D p = root + dir * (lean * t * t) + QVector3D(0.0f, height * t, 0.0f);
            QVector3D col = ProcMath::mix(rootCol, tipCol, t) * (0.86f + h4 * 0.24f);
            verts.push_back({p - side * w, norm, col, QVector2D(2.0f, t * 0.85f)});
            verts.push_back({p + side * w, norm, col, QVector2D(2.0f, t * 0.85f)});
        }
        for (int s = 0; s < segs; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
}

// ============================================================================
// ПРОТОТИП 5: МНОГОЯРУСНЫЙ ВЕТВИСТЫЙ КОРАЛЛ (PROP_CORAL_BUSH)
// ============================================================================

void buildCoralBushPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    const QVector3D baseCol(0.60f, 0.56f, 0.58f);
    const QVector3D midCol(0.84f, 0.80f, 0.82f);
    const QVector3D tipCol(1.00f, 0.98f, 0.96f);

    QVector3D root(0.0f, -0.32f, 0.0f);
    int trunks = highLod ? 5 : 4;
    int sides  = highLod ? 5 : 3;

    appendTaperedTube(verts, inds, root, QVector3D(0.0f, 0.16f, 0.0f),
                      0.34f, 0.22f, sides, baseCol * 0.78f, baseCol, 2.0f, 0.0f, 0.0f, false);

    for (int t = 0; t < trunks; ++t) {
        float h1 = ProcMath::hash3(t, 1, 5, 7103u);
        float h2 = ProcMath::hash3(t, 2, 7, 7103u);
        float ang = (TWO_PI * t) / trunks + (h1 - 0.5f) * 0.45f;
        QVector3D d1(std::cos(ang) * (0.42f + h2 * 0.28f), 0.86f, std::sin(ang) * (0.42f + h2 * 0.28f));
        d1.normalize();

        QVector3D mid = QVector3D(0.0f, 0.08f, 0.0f) + d1 * (0.85f + h1 * 0.35f);
        appendTaperedTube(verts, inds, QVector3D(0.0f, 0.04f, 0.0f), mid,
                          0.14f, 0.095f, sides, baseCol, midCol, 2.0f, 0.0f, 0.0f, !highLod);

        int forks = highLod ? 2 : 1;
        for (int f = 0; f < forks; ++f) {
            float fAng = ang + ((f == 0) ? 0.58f : -0.58f) + (h2 - 0.5f) * 0.25f;
            QVector3D d2(std::cos(fAng) * 0.56f, 0.80f, std::sin(fAng) * 0.56f);
            d2.normalize();
            QVector3D forkEnd = mid + d2 * (0.62f + 0.22f * f);
            appendTaperedTube(verts, inds, mid, forkEnd, 0.088f, 0.052f, highLod ? 4 : 3,
                              midCol, ProcMath::mix(midCol, tipCol, 0.55f), 2.0f, 0.0f, 0.0f, !highLod);

            if (highLod) {
                for (int k = 0; k < 2; ++k) {
                    float kAng = fAng + ((k == 0) ? 0.48f : -0.48f);
                    QVector3D d3(std::cos(kAng) * 0.48f, 0.88f, std::sin(kAng) * 0.48f);
                    d3.normalize();
                    QVector3D tip = forkEnd + d3 * (0.42f + 0.16f * k);
                    appendTaperedTube(verts, inds, forkEnd, tip, 0.048f, 0.022f, 4,
                                      ProcMath::mix(midCol, tipCol, 0.55f), tipCol, 2.0f, 0.0f, 0.0f, true);
                }
            }
        }
    }
}

// ============================================================================
// ПРОТОТИП 6: МНОГОЯРУСНЫЙ ПЛАСТИНЧАТЫЙ / ВЕЕРНЫЙ КОРАЛЛ (PROP_TABLE_FAN)
// ============================================================================

void buildTableFanPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    const QVector3D innerCol(0.72f, 0.68f, 0.70f);
    const QVector3D rimCol(1.00f, 0.96f, 0.92f);
    const QVector3D underCol(0.40f, 0.38f, 0.43f);

    int tiers = highLod ? 3 : 2;
    int segs  = highLod ? 16 : 8;

    // Удлинённый корень с расширением книзу — гарантированно врезается в склон без просвета
    appendTaperedTube(verts, inds, QVector3D(0.0f, -0.82f, 0.0f), QVector3D(0.0f, 0.48f, 0.0f),
                      0.40f, 0.20f, highLod ? 7 : 5, underCol * 0.85f, innerCol, 2.0f, 0.0f, 0.0f, false);

    for (int tier = 0; tier < tiers; ++tier) {
        float rad   = 1.38f - tier * 0.34f;
        float yBase = 0.42f + tier * 0.40f;
        float offX  = (tier == 1) ? 0.24f : (tier == 2 ? -0.18f : 0.0f);
        float offZ  = (tier == 1) ? -0.14f : (tier == 2 ? 0.20f : 0.0f);
        QVector3D discC(offX, yBase, offZ);

        uint32_t topC   = static_cast<uint32_t>(verts.size());
        verts.push_back({discC + QVector3D(0.0f, 0.14f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f), innerCol, QVector2D(2.0f, 0.0f)});
        uint32_t topRim = static_cast<uint32_t>(verts.size());

        for (int s = 0; s <= segs; ++s) {
            float a = (TWO_PI * s) / segs;
            float wave = 1.0f + 0.18f * std::sin(a * 5.0f + tier * 1.4f) + 0.08f * std::cos(a * 3.0f);
            float curl = 0.10f * std::sin(a * 4.0f + tier);
            QVector3D p = discC + QVector3D(std::cos(a) * rad * wave, curl + 0.06f, std::sin(a) * rad * wave);
            QVector3D n(std::cos(a) * 0.22f, 0.97f, std::sin(a) * 0.22f);
            n.normalize();
            verts.push_back({p, n, rimCol, QVector2D(2.0f, 0.0f)});
        }

        uint32_t botC   = static_cast<uint32_t>(verts.size());
        verts.push_back({discC - QVector3D(0.0f, 0.12f, 0.0f), QVector3D(0.0f, -1.0f, 0.0f), underCol * 0.85f, QVector2D(2.0f, 0.0f)});
        uint32_t botRim = static_cast<uint32_t>(verts.size());

        for (int s = 0; s <= segs; ++s) {
            float a = (TWO_PI * s) / segs;
            float wave = 1.0f + 0.18f * std::sin(a * 5.0f + tier * 1.4f) + 0.08f * std::cos(a * 3.0f);
            float curl = 0.10f * std::sin(a * 4.0f + tier);
            QVector3D p = discC + QVector3D(std::cos(a) * rad * wave, curl - 0.05f, std::sin(a) * rad * wave);
            QVector3D n(std::cos(a) * 0.25f, -0.96f, std::sin(a) * 0.25f);
            n.normalize();
            verts.push_back({p, n, underCol, QVector2D(2.0f, 0.0f)});
        }

        // Правильный обход против часовой стрелки (CCW) снаружи + замкнутая боковая кромка
        for (int s = 0; s < segs; ++s) {
            inds.insert(inds.end(), {topC, topRim + s + 1, topRim + s});
            inds.insert(inds.end(), {botC, botRim + s,     botRim + s + 1});
            inds.insert(inds.end(), {
                                        topRim + s,     topRim + s + 1, botRim + s,
                                        topRim + s + 1, botRim + s + 1, botRim + s
                                    });
        }
    }
}

// ============================================================================
// ПРОТОТИП 7: КУСТИСТЫЕ ВОДОРОСЛИ / РОЗЕТКА ПЛАТО / АНЕМОН (PROP_ANEMONE_PEN)
// ============================================================================

void buildAnemoneBushPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    const int fronds = highLod ? 11 : 5;
    const int segs   = highLod ? 3  : 2;
    const QVector3D baseCol(0.54f, 0.62f, 0.56f);
    const QVector3D tipCol(0.96f, 1.00f, 0.94f);

    QVector3D rootBase(0.0f, -0.22f, 0.0f);

    for (int f = 0; f < fronds; ++f) {
        float h1 = ProcMath::hash3(f, 3, 11, 8117u);
        float h2 = ProcMath::hash3(f, 5, 13, 8117u);
        bool outerTier = (f < (fronds * 2) / 3);

        float ang    = (TWO_PI * f) / fronds + h1 * 0.42f;
        float height = (outerTier ? 1.15f : 1.55f) + h2 * 0.45f;
        float spread = (outerTier ? 1.10f : 0.55f) + h1 * 0.35f;
        float maxW   = (highLod ? 0.30f : 0.46f) * (outerTier ? 1.08f : 0.82f);

        QVector3D dir(std::cos(ang), 0.0f, std::sin(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D norm = (QVector3D(0.0f, 0.78f, 0.0f) + dir * 0.62f).normalized();
        QVector3D root = rootBase + dir * (outerTier ? 0.16f : 0.06f);

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float archY = height * (t * (1.26f - 0.36f * t));
            float archR = spread * std::pow(t, 1.32f);
            float w = maxW * std::sin(std::max(0.14f, t * 0.92f) * PI);
            QVector3D p = root + dir * archR + QVector3D(0.0f, archY, 0.0f);
            QVector3D col = ProcMath::mix(baseCol, tipCol, t);
            verts.push_back({p - side * w, norm, col, QVector2D(2.0f, t * 0.80f)});
            verts.push_back({p + side * w, norm, col, QVector2D(2.0f, t * 0.80f)});
        }
        for (int s = 0; s < segs; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
}

// ============================================================================
// ПРОТОТИП 8: ПРИБРЕЖНАЯ НАДВОДНАЯ ТРАВА НА СКАЛАХ (PROP_CLIFF_GRASS, param.x = 6.0)
// ============================================================================

void buildCliffGrassPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    const int blades = highLod ? 28 : 10;
    const int segs   = highLod ? 4  : 2;

    const QVector3D rootCol(0.14f, 0.30f, 0.10f);
    const QVector3D midCol(0.28f, 0.56f, 0.16f);
    const QVector3D tipCol(0.62f, 0.70f, 0.24f);
    const QVector3D dryTipCol(0.74f, 0.68f, 0.30f);

    for (int b = 0; b < blades; ++b) {
        float h1 = ProcMath::hash3(b, 1, 17, 9103u);
        float h2 = ProcMath::hash3(b, 2, 23, 9103u);
        float h3 = ProcMath::hash3(b, 3, 29, 9103u);
        float h4 = ProcMath::hash3(b, 4, 31, 9103u);

        float posAng = (TWO_PI * static_cast<float>(b)) / static_cast<float>(blades) + (h1 - 0.5f) * 0.65f;
        float posRad = std::sqrt((static_cast<float>(b) + 0.45f) / static_cast<float>(blades)) * 2.25f;
        QVector3D root(std::cos(posAng) * posRad, -0.32f, std::sin(posAng) * posRad);

        // Внешние травинки слегка расходятся веером, внутренние тянутся выше
        float bladeAng = posAng + (h2 - 0.5f) * 1.15f;
        bool tallPlume = (b % 4 == 0);
        float height = tallPlume ? (1.45f + h3 * 0.85f) : (0.88f + h3 * 0.72f);
        float width  = (highLod ? 0.125f : 0.25f) * (tallPlume ? 1.12f : 0.96f);
        float lean   = (tallPlume ? 0.78f : 0.48f) + h4 * 0.42f;

        QVector3D dir(std::cos(bladeAng), 0.0f, std::sin(bladeAng));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D endTipCol = (h4 > 0.68f) ? dryTipCol : tipCol;

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float w = width * (1.0f - t * 0.92f);
            float archY = height * (t * (1.22f - 0.28f * t));
            float archR = lean * (t * t);

            QVector3D p = root + dir * archR + QVector3D(0.0f, archY, 0.0f);
            QVector3D col = (t < 0.52f)
                                ? ProcMath::mix(rootCol, midCol, t / 0.52f)
                                : ProcMath::mix(midCol, endTipCol, (t - 0.52f) / 0.48f);
            col *= (0.86f + h1 * 0.26f);

            QVector3D norm = (QVector3D(0.0f, 0.82f - t * 0.25f, 0.0f) + dir * (0.35f + t * 0.45f)).normalized();
            verts.push_back({p - side * w, norm, col, QVector2D(6.0f, t)});
            verts.push_back({p + side * w, norm, col, QVector2D(6.0f, t)});
        }
        for (int s = 0; s < segs; ++s) {
            uint32_t i0 = vb + s * 2;
            inds.insert(inds.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
        }
    }
}

// Глубоководные биолюминесцентные морские перья (растут в абиссальных ямах, param.x = 4.0)
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
        float ang = h1 * TWO_PI;
        QVector3D dir(std::cos(ang), 0.0f, std::sin(ang));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D norm = (dir + QVector3D(0.0f, 0.3f, 0.0f)).normalized();

        float height = (1.8f + h1 * 1.6f) * scale;
        float maxW   = 0.32f * scale;
        QVector3D root = center + dir * (0.25f * k) - QVector3D(0.0f, 0.15f, 0.0f);

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
} // namespace

// ============================================================================
// ГЕНЕРАЦИЯ МИРА (точка входа + постпроцессинг рельефа + прототипы + флора)
// ============================================================================

WorldData WorldGenerator::generate(uint32_t seed) {
    WorldData data;
    data.collision.seed = seed;
    data.collision.chunksPerSide = WorldData::CHUNKS_PER_SIDE;
    data.collision.chunkSize = WorldData::CHUNK_SIZE;
    data.collision.worldHalfSize = ProcMath::WORLD_HALF_SIZE;

    generateTerrainChunks(data, seed);
    generatePropPrototypes(data);
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

        float strataWarp  = ProcMath::fbm(wx * 0.032f, wy * 0.018f, wz * 0.032f, 2, seed + 5113u) * 3.0f;
        float strataPhase = wy * 0.88f + strataWarp;
        float strataLedge = std::sin(strataPhase) * 0.56f + std::sin(strataPhase * 2.25f + 0.8f) * 0.24f;

        float crag3D     = ProcMath::fbm(wx * 0.115f, wy * 0.135f, wz * 0.115f, 3, seed + 5231u);
        float crack3D    = ProcMath::voronoiCracks2D(wx * 0.072f + wy * 0.044f, wz * 0.072f - wy * 0.038f, seed + 5309u);
        float fractureIn = (1.0f - ProcMath::smoothstep(0.04f, 0.23f, crack3D)) * -0.45f;
        float tanJitter  = ProcMath::fbm(wx * 0.15f + 9.3f, wy * 0.11f, wz * 0.15f - 4.7f, 2, seed + 5417u);

        // Запас от схлопывания полуячеек LOD0 (шаг 0.5 * CELL_SZ)
        float maxPush    = 0.24f * CELL_SZ;
        float normalPush = ProcMath::clamp((strataLedge * 0.52f + crag3D * 0.72f + fractureIn) * 0.68f, -1.0f, 1.0f);
        float tanPush    = ProcMath::clamp(tanJitter, -0.75f, 0.75f) * 0.36f;

        return QVector3D(
            wx + (outX * normalPush + tanX * tanPush) * maxPush * steep,
            wy + strataLedge * 0.20f * steep,
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

    // Кеш постпроцессированных высот для коллизий и точной посадки всех пропов
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

void WorldGenerator::generatePropPrototypes(WorldData& data) {
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

    // 2: Пышная колонна ламинарии (каноническая высота 14 м + светящиеся пузырьки)
    buildKelpColumnPrototype(data.propPrototypes[PROP_KELP_COLUMN].lod0Verts,
                             data.propPrototypes[PROP_KELP_COLUMN].lod0Inds, true);
    buildKelpColumnPrototype(data.propPrototypes[PROP_KELP_COLUMN].lod1Verts,
                             data.propPrototypes[PROP_KELP_COLUMN].lod1Inds, false);
    data.propPrototypes[PROP_KELP_COLUMN].baseRadius = 15.0f;

    // 3: Толстостенная полая бочкообразная / трубчатая губка
    buildBarrelSpongePrototype(data.propPrototypes[PROP_BARREL_SPONGE].lod0Verts,
                               data.propPrototypes[PROP_BARREL_SPONGE].lod0Inds, true);
    buildBarrelSpongePrototype(data.propPrototypes[PROP_BARREL_SPONGE].lod1Verts,
                               data.propPrototypes[PROP_BARREL_SPONGE].lod1Inds, false);
    data.propPrototypes[PROP_BARREL_SPONGE].baseRadius = 1.65f;

    // 4: Плотный ковёр-куст придонной травы (26 лезвий в одном инстансе)
    buildGroundCarpetPrototype(data.propPrototypes[PROP_GROUND_CARPET].lod0Verts,
                               data.propPrototypes[PROP_GROUND_CARPET].lod0Inds, true);
    buildGroundCarpetPrototype(data.propPrototypes[PROP_GROUND_CARPET].lod1Verts,
                               data.propPrototypes[PROP_GROUND_CARPET].lod1Inds, false);
    data.propPrototypes[PROP_GROUND_CARPET].baseRadius = 2.55f;

    // 5: Пышный многоярусный ветвистый коралл
    buildCoralBushPrototype(data.propPrototypes[PROP_CORAL_BUSH].lod0Verts,
                            data.propPrototypes[PROP_CORAL_BUSH].lod0Inds, true);
    buildCoralBushPrototype(data.propPrototypes[PROP_CORAL_BUSH].lod1Verts,
                            data.propPrototypes[PROP_CORAL_BUSH].lod1Inds, false);
    data.propPrototypes[PROP_CORAL_BUSH].baseRadius = 1.95f;

    // 6: Многоярусный пластинчатый коралл
    buildTableFanPrototype(data.propPrototypes[PROP_TABLE_FAN].lod0Verts,
                           data.propPrototypes[PROP_TABLE_FAN].lod0Inds, true);
    buildTableFanPrototype(data.propPrototypes[PROP_TABLE_FAN].lod1Verts,
                           data.propPrototypes[PROP_TABLE_FAN].lod1Inds, false);
    data.propPrototypes[PROP_TABLE_FAN].baseRadius = 1.85f;

    // 7: Кустистые водоросли / солнечная розетка плато / актиния
    buildAnemoneBushPrototype(data.propPrototypes[PROP_ANEMONE_PEN].lod0Verts,
                              data.propPrototypes[PROP_ANEMONE_PEN].lod0Inds, true);
    buildAnemoneBushPrototype(data.propPrototypes[PROP_ANEMONE_PEN].lod1Verts,
                              data.propPrototypes[PROP_ANEMONE_PEN].lod1Inds, false);
    data.propPrototypes[PROP_ANEMONE_PEN].baseRadius = 1.90f;

    // 8: Колышущаяся на ветру прибрежная трава на надводных скалах и кекурах
    buildCliffGrassPrototype(data.propPrototypes[PROP_CLIFF_GRASS].lod0Verts,
                             data.propPrototypes[PROP_CLIFF_GRASS].lod0Inds, true);
    buildCliffGrassPrototype(data.propPrototypes[PROP_CLIFF_GRASS].lod1Verts,
                             data.propPrototypes[PROP_CLIFF_GRASS].lod1Inds, false);
    data.propPrototypes[PROP_CLIFF_GRASS].baseRadius = 2.65f;
}

// ============================================================================
// ПЛОТНОЕ ЭКОЛОГИЧЕСКОЕ ЗАСЕЛЕНИЕ ЧАНКОВ (СКАЛЫ, ЛАМИНАРИЯ, КОВЁР, ГУБКИ, КОРАЛЛЫ, НАДВОДНАЯ ТРАВА)
// ============================================================================

void WorldGenerator::generateChunkProps(WorldData& data, uint32_t seed) {
    constexpr int C = WorldData::CHUNKS_PER_SIDE;
    constexpr int TOTAL_CHUNKS = C * C;
    constexpr float CHUNK_SZ = WorldData::CHUNK_SIZE;
    constexpr float HALF_W = ProcMath::WORLD_HALF_SIZE;
    constexpr float MAX_UNDERWATER_FLORA_TOP_Y = ProcMath::WATER_SURFACE_Y - 1.65f;
    const auto& pits = ProcMath::getAbyssalPits(seed);

    struct ChunkPropTemp {
        std::vector<EnvVertex> detailVerts;
        std::vector<uint32_t>  detailInds;
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
                WorldChunk& chunk = data.chunks[ci];
                ChunkPropTemp& tmp = chunkTemps[ci];

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

                // ------------------------------------------------------------
                // 1. Валуны и россыпи камней на дне и рифах
                // ------------------------------------------------------------
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
                            tmp.spheres.push_back({center, std::max(scaleXZ, scaleY) * 0.96f});
                        }
                    }
                }

                // ------------------------------------------------------------
                // 2. Целенаправленное 3D-наращивание плит по кромкам обрывов, стенам и осыпям
                // ------------------------------------------------------------
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
                                tmp.spheres.push_back({pos + norm * (scaleXZ * 0.42f), scaleXZ * 0.78f});
                            }
                        }
                    }
                }

                // ------------------------------------------------------------
                // 3. Рощи гигантских пышных колонн ламинарии (кластерные аллеи)
                // ------------------------------------------------------------
                float forestNoise = ProcMath::fbm(chunkCenterX * 0.0085f, 0.0f, chunkCenterZ * 0.0085f, 2, seed + 701u);
                if (centerWallF < 0.56f && forestNoise > -0.34f) {
                    int groveCount = (forestNoise > 0.10f) ? 6 : 3;
                    for (int g = 0; g < groveCount; ++g) {
                        float gxH = ProcMath::hash3(chunk.cx, chunk.cz, g * 4 + 1, seed + 307u);
                        float gzH = ProcMath::hash3(chunk.cx, chunk.cz, g * 4 + 2, seed + 307u);
                        float grH = ProcMath::hash3(chunk.cx, chunk.cz, g * 4 + 3, seed + 307u);

                        float gx = originX + 7.0f + gxH * (CHUNK_SZ - 14.0f);
                        float gz = originZ + 7.0f + gzH * (CHUNK_SZ - 14.0f);
                        if (ProcMath::sampleBasinWallFactor(gx, gz, seed) > 0.45f) continue;

                        int stalksInGrove = 4 + static_cast<int>(grH * 4.9f);
                        float groveR = 4.2f + grH * 4.5f;

                        for (int k = 0; k < stalksInGrove; ++k) {
                            float kh1 = ProcMath::hash3(ci, g * 19 + k, 1, seed + 311u);
                            float kh2 = ProcMath::hash3(ci, g * 19 + k, 2, seed + 311u);
                            float kh3 = ProcMath::hash3(ci, g * 19 + k, 3, seed + 311u);

                            float ang = kh1 * TWO_PI;
                            float rad = std::sqrt(kh2) * groveR;
                            float wx = gx + std::cos(ang) * rad;
                            float wz = gz + std::sin(ang) * rad;
                            if ((wx * wx + (wz - 20.0f) * (wz - 20.0f)) < 110.0f) continue;

                            float wy = data.collision.sampleHeight(wx, wz);
                            if (wy > 4.0f || wy < -36.0f) continue;

                            QVector3D n = data.collision.sampleNormal(wx, wz);
                            if (n.y() < 0.56f) continue;

                            float maxH = std::min(25.0f, (ProcMath::WATER_SURFACE_Y - 2.4f) - wy);
                            if (maxH < 7.5f) continue;
                            float height = 8.5f + kh3 * (maxH - 8.5f);

                            float scaleY  = height / 14.0f;
                            float scaleXZ = 0.85f + kh2 * 0.52f;

                            QVector3D tint(0.86f + kh1 * 0.28f, 0.90f + kh3 * 0.22f, 0.82f + kh2 * 0.24f);
                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(wx, wy - 0.30f, wz, scaleXZ);
                            inst.rotScaleY  = QVector4D(kh1 * TWO_PI, (kh2 - 0.5f) * 0.08f, (kh3 - 0.5f) * 0.08f, scaleY);
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 2.0f);
                            chunk.propInstances[PROP_KELP_COLUMN].push_back(inst);
                        }
                    }
                }

                // ------------------------------------------------------------
                // 4. Сплошной придонный ковёр травы и водорослей (PROP_GROUND_CARPET)
                // ------------------------------------------------------------
                if (centerWallF < 0.66f) {
                    float meadowNoise = ProcMath::fbm(chunkCenterX * 0.011f, 1.5f, chunkCenterZ * 0.011f, 2, seed + 881u);
                    int carpetAttempts = (meadowNoise > -0.20f) ? 84 : 52;

                    for (int g = 0; g < carpetAttempts; ++g) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, g * 4 + 1, seed + 641u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, g * 4 + 2, seed + 641u);
                        float hs = ProcMath::hash3(chunk.cx, chunk.cz, g * 4 + 3, seed + 641u);
                        float hr = ProcMath::hash3(chunk.cx, chunk.cz, g * 4 + 4, seed + 641u);

                        float wx = originX + 2.0f + hx * (CHUNK_SZ - 4.0f);
                        float wz = originZ + 2.0f + hz * (CHUNK_SZ - 4.0f);
                        if (ProcMath::sampleBasinWallFactor(wx, wz, seed) > 0.62f) continue;

                        float wy = data.collision.sampleHeight(wx, wz);
                        if (wy < -33.0f || wy > 23.2f) continue;

                        QVector3D norm = data.collision.sampleNormal(wx, wz);
                        if (norm.y() < 0.64f) continue;

                        float yaw = 0.0f, pitch = 0.0f;
                        QVector3D tiltN = (norm * 0.65f + QVector3D(0.0f, 0.35f, 0.0f)).normalized();
                        alignUpAxisToDir(tiltN, yaw, pitch);

                        float scaleXZ = 1.05f + hs * 0.85f;
                        float scaleY  = 0.72f + hr * 0.68f;
                        if (wy + scaleY * 1.80f > MAX_UNDERWATER_FLORA_TOP_Y) continue;

                        QVector3D tint(0.84f + hs * 0.30f, 0.88f + hr * 0.24f, 0.84f + hx * 0.28f);

                        PropGpuInstance inst;
                        inst.posScaleXZ = QVector4D(wx, wy - 0.14f, wz, scaleXZ);
                        inst.rotScaleY  = QVector4D(yaw + hr * TWO_PI * 0.15f, pitch, 0.0f, scaleY);
                        inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 1.0f);
                        chunk.propInstances[PROP_GROUND_CARPET].push_back(inst);
                    }
                }

                // ------------------------------------------------------------
                // 5. Рифовые кораллы: ветвистые кусты (PROP_CORAL_BUSH) и пластинчатые (PROP_TABLE_FAN)
                // ------------------------------------------------------------
                if (centerWallF < 0.68f) {
                    for (int f = 0; f < 34; ++f) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 1, seed + 691u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 2, seed + 691u);
                        float hs = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 3, seed + 691u);
                        float hk = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 4, seed + 691u);
                        float hc = ProcMath::hash3(chunk.cx, chunk.cz, f * 5 + 5, seed + 691u);

                        float wx = originX + 2.5f + hx * (CHUNK_SZ - 5.0f);
                        float wz = originZ + 2.5f + hz * (CHUNK_SZ - 5.0f);
                        if (ProcMath::sampleBasinWallFactor(wx, wz, seed) > 0.64f) continue;

                        float wy = data.collision.sampleHeight(wx, wz);
                        if (wy < -32.0f || wy > 22.8f) continue;

                        QVector3D norm = data.collision.sampleNormal(wx, wz);
                        if (norm.y() < 0.50f) continue;

                        if (hk < 0.56f) {
                            float sc = 0.75f + hs * 0.88f;
                            float scY = sc * (0.85f + hk * 0.35f);
                            if (wy + scY * 2.25f > MAX_UNDERWATER_FLORA_TOP_Y) continue;

                            QVector3D tint = (hc < 0.30f) ? QVector3D(0.98f, 0.42f, 0.58f)
                                             : (hc < 0.56f) ? QVector3D(0.98f, 0.72f, 0.28f)
                                             : (hc < 0.80f) ? QVector3D(0.76f, 0.40f, 0.96f)
                                                            : QVector3D(0.96f, 0.30f, 0.34f);
                            float yaw = 0.0f, pitch = 0.0f;
                            alignUpAxisToDir((norm * 0.55f + QVector3D(0.0f, 0.45f, 0.0f)).normalized(), yaw, pitch);

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(wx, wy - 0.18f, wz, sc);
                            inst.rotScaleY  = QVector4D(yaw, pitch, 0.0f, scY);
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                            chunk.propInstances[PROP_CORAL_BUSH].push_back(inst);
                        } else if (norm.y() >= 0.62f) {
                            // Пластинчатый коралл: проверяем опору вокруг ножки, чтобы не висел на обрыве
                            float sc = 0.80f + hs * 0.85f;
                            float footR = 0.65f * sc;
                            float hL = data.collision.sampleHeight(wx - footR, wz);
                            float hR = data.collision.sampleHeight(wx + footR, wz);
                            float hD = data.collision.sampleHeight(wx, wz - footR);
                            float hU = data.collision.sampleHeight(wx, wz + footR);
                            float minFootY = std::min({wy, hL, hR, hD, hU});
                            if ((wy - minFootY) > 0.68f) continue;
                            if (minFootY + sc * 1.45f > MAX_UNDERWATER_FLORA_TOP_Y) continue;

                            QVector3D tint = (hc < 0.35f) ? QVector3D(0.92f, 0.54f, 0.34f)
                                             : (hc < 0.70f) ? QVector3D(0.78f, 0.44f, 0.88f)
                                                            : QVector3D(0.36f, 0.84f, 0.58f);

                            float yaw = 0.0f, pitch = 0.0f;
                            alignUpAxisToDir((norm * 0.28f + QVector3D(0.0f, 0.72f, 0.0f)).normalized(), yaw, pitch);
                            QVector3D pos(wx, minFootY + 0.04f, wz);

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(pos.x(), pos.y(), pos.z(), sc);
                            inst.rotScaleY  = QVector4D(yaw + hc * TWO_PI * 0.2f, pitch, 0.0f, sc * 0.92f);
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                            chunk.propInstances[PROP_TABLE_FAN].push_back(inst);
                        }
                    }
                }

                // ------------------------------------------------------------
                // 6. Семейки толстостенных полых губок (PROP_BARREL_SPONGE) + абиссальные перья
                // ------------------------------------------------------------
                if (centerWallF < 0.70f) {
                    for (int sp = 0; sp < 14; ++sp) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, sp * 5 + 1, seed + 509u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, sp * 5 + 2, seed + 509u);
                        float ht = ProcMath::hash3(chunk.cx, chunk.cz, sp * 5 + 3, seed + 509u);
                        float hc = ProcMath::hash3(chunk.cx, chunk.cz, sp * 5 + 4, seed + 509u);

                        float wx = originX + 3.0f + hx * (CHUNK_SZ - 6.0f);
                        float wz = originZ + 3.0f + hz * (CHUNK_SZ - 6.0f);
                        float wy = data.collision.sampleHeight(wx, wz);

                        if (wy <= -30.0f && wy >= -72.0f) {
                            QVector3D n = data.collision.sampleNormal(wx, wz);
                            if (n.y() > 0.45f) {
                                uint32_t dSeed = seed + static_cast<uint32_t>(ci * 173 + sp * 31);
                                appendAbyssalSeaPen(tmp.detailVerts, tmp.detailInds, QVector3D(wx, wy, wz), 0.85f + ht * 0.75f, dSeed);
                            }
                            continue;
                        }

                        if (wy > 22.8f) continue;

                        QVector3D cNorm = data.collision.sampleNormal(wx, wz);
                        if (cNorm.y() < 0.68f) continue;

                        constexpr float checkR = 1.15f;
                        float hL = data.collision.sampleHeight(wx - checkR, wz);
                        float hR = data.collision.sampleHeight(wx + checkR, wz);
                        float hD = data.collision.sampleHeight(wx, wz - checkR);
                        float hU = data.collision.sampleHeight(wx, wz + checkR);
                        float avgH = 0.25f * (hL + hR + hD + hU);
                        if ((wy - avgH) > 0.30f) continue;

                        QVector3D tint = (hc < 0.36f) ? QVector3D(0.92f, 0.56f, 0.42f)
                                         : (hc < 0.68f) ? QVector3D(0.94f, 0.82f, 0.46f)
                                                        : QVector3D(0.86f, 0.56f, 0.88f);

                        int groupSize = (ht > 0.45f) ? 3 : 2;
                        float hBase  = 0.50f + ProcMath::hash3(ci, sp * 11,     3, seed + 521u) * 0.14f;
                        float hStep1 = 1.68f + ProcMath::hash3(ci, sp * 11 + 1, 3, seed + 521u) * 0.14f;
                        float hStep2 = 1.68f + ProcMath::hash3(ci, sp * 11 + 2, 3, seed + 521u) * 0.14f;

                        float scYs[3] = {};
                        if (groupSize == 3) {
                            scYs[0] = hBase * hStep1 * hStep2;
                            scYs[1] = hBase * hStep1;
                            scYs[2] = hBase;
                        } else {
                            scYs[0] = hBase * hStep1 * 1.15f;
                            scYs[1] = hBase * 1.05f;
                        }

                        float baseAng = ht * TWO_PI;
                        float angOffsets[3] = {
                            0.0f,
                            1.85f + ProcMath::hash3(ci, sp * 11 + 1, 4, seed + 521u) * 0.65f,
                            3.90f + ProcMath::hash3(ci, sp * 11 + 2, 4, seed + 521u) * 0.75f
                        };

                        for (int k = 0; k < groupSize; ++k) {
                            float kh1 = ProcMath::hash3(ci, sp * 11 + k, 1, seed + 521u);
                            float kh2 = ProcMath::hash3(ci, sp * 11 + k, 2, seed + 521u);

                            float ang = baseAng + angOffsets[k];
                            float offR = (k == 0) ? 0.0f : (0.78f + kh1 * 0.55f);
                            float sx = wx + std::cos(ang) * offR;
                            float sz = wz + std::sin(ang) * offR;
                            float sy = data.collision.sampleHeight(sx, sz);

                            float scY  = scYs[k];
                            if (sy + scY * 1.50f > MAX_UNDERWATER_FLORA_TOP_Y) continue;

                            float scXZ = ProcMath::clamp(scY * (0.72f + kh2 * 0.26f), 0.42f, 1.32f);

                            float yaw = 0.0f, pitch = 0.0f;
                            QVector3D leanDir = (cNorm + QVector3D(std::cos(ang) * 0.22f, 0.25f, std::sin(ang) * 0.22f)).normalized();
                            alignUpAxisToDir(leanDir, yaw, pitch);

                            float sink = (0.20f + (1.0f - cNorm.y()) * 0.75f) * std::max(scXZ, 0.55f);
                            QVector3D subTint = tint * (0.86f + kh2 * 0.26f);

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(sx, sy - sink, sz, scXZ);
                            inst.rotScaleY  = QVector4D(yaw, pitch, 0.0f, scY);
                            inst.tintFlags  = QVector4D(subTint.x(), subTint.y(), subTint.z(), 0.0f);
                            chunk.propInstances[PROP_BARREL_SPONGE].push_back(inst);
                        }
                    }
                }

                // ------------------------------------------------------------
                // 7. Кустистые водоросли, анемоны и островки зелени на верхних плато (PROP_ANEMONE_PEN)
                // ------------------------------------------------------------
                if (centerWallF < 0.72f) {
                    for (int a = 0; a < 16; ++a) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, a * 4 + 1, seed + 829u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, a * 4 + 2, seed + 829u);
                        float hs = ProcMath::hash3(chunk.cx, chunk.cz, a * 4 + 3, seed + 829u);
                        float hc = ProcMath::hash3(chunk.cx, chunk.cz, a * 4 + 4, seed + 829u);

                        float wx = originX + 3.0f + hx * (CHUNK_SZ - 6.0f);
                        float wz = originZ + 3.0f + hz * (CHUNK_SZ - 6.0f);
                        float wy = data.collision.sampleHeight(wx, wz);
                        if (wy < -31.0f || wy > 23.0f) continue;

                        QVector3D norm = data.collision.sampleNormal(wx, wz);
                        if (norm.y() < 0.58f) continue;

                        float scXZ = 0.75f + hs * 0.85f;
                        float scY  = 0.75f + hc * 0.85f;
                        if (wy + scY * 1.95f > MAX_UNDERWATER_FLORA_TOP_Y) continue;

                        QVector3D tint = (hc < 0.42f) ? QVector3D(0.32f, 0.86f, 0.44f)
                                         : (hc < 0.72f) ? QVector3D(0.84f, 0.72f, 0.24f)
                                                        : QVector3D(0.92f, 0.42f, 0.76f);

                        PropGpuInstance inst;
                        inst.posScaleXZ = QVector4D(wx, wy - 0.16f, wz, scXZ);
                        inst.rotScaleY  = QVector4D(hc * TWO_PI, (hs - 0.5f) * 0.20f, 0.0f, scY);
                        inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                        chunk.propInstances[PROP_ANEMONE_PEN].push_back(inst);
                    }

                    if (chunk.maxBounds.y() > 31.0f) {
                        for (int isl = 0; isl < 12; ++isl) {
                            float hx = ProcMath::hash3(chunk.cx, chunk.cz, isl * 5 + 1, seed + 1811u);
                            float hz = ProcMath::hash3(chunk.cx, chunk.cz, isl * 5 + 2, seed + 1811u);
                            float hr = ProcMath::hash3(chunk.cx, chunk.cz, isl * 5 + 3, seed + 1811u);

                            float cxIsl = originX + 5.0f + hx * (CHUNK_SZ - 10.0f);
                            float czIsl = originZ + 5.0f + hz * (CHUNK_SZ - 10.0f);
                            if (ProcMath::sampleBasinWallFactor(cxIsl, czIsl, seed) > 0.72f) continue;

                            float cyIsl = data.collision.sampleHeight(cxIsl, czIsl);
                            if (cyIsl < 2.0f || cyIsl > 25.4f) continue;

                            QVector3D cNorm = data.collision.sampleNormal(cxIsl, czIsl);
                            if (cNorm.y() < 0.56f) continue;
                            if (samplePlateauIslandMask(cxIsl, czIsl, seed) < 0.20f) continue;

                            int tufts = 6 + static_cast<int>(hr * 3.9f);
                            float islandR = 3.6f + hr * 3.4f;
                            for (int t = 0; t < tufts; ++t) {
                                float th1 = ProcMath::hash3(ci, isl * 13 + t, 1, seed + 1823u);
                                float th2 = ProcMath::hash3(ci, isl * 13 + t, 2, seed + 1823u);
                                float th3 = ProcMath::hash3(ci, isl * 13 + t, 3, seed + 1823u);

                                float ang = th1 * TWO_PI;
                                float rad = std::sqrt(th2) * islandR;
                                float wx = cxIsl + std::cos(ang) * rad;
                                float wz = czIsl + std::sin(ang) * rad;
                                float wy = data.collision.sampleHeight(wx, wz);
                                if (wy < 1.0f || wy > MAX_UNDERWATER_FLORA_TOP_Y - 0.55f) continue;

                                QVector3D n = data.collision.sampleNormal(wx, wz);
                                if (n.y() < 0.50f) continue;

                                float clearance = MAX_UNDERWATER_FLORA_TOP_Y - wy;
                                float scXZ = (0.78f + th3 * 0.68f) * (1.12f - 0.30f * (rad / islandR));
                                float scY  = std::min(scXZ * 1.05f, clearance / 1.85f);
                                if (scY < 0.25f) continue;

                                QVector3D lushTint = (th3 < 0.55f)
                                                         ? QVector3D(0.36f, 0.94f, 0.34f)
                                                         : QVector3D(0.60f, 0.92f, 0.26f);

                                PropGpuInstance inst;
                                inst.posScaleXZ = QVector4D(wx, wy - 0.14f, wz, scXZ);
                                inst.rotScaleY  = QVector4D(th1 * TWO_PI, (th2 - 0.5f) * 0.16f, 0.0f, scY);
                                inst.tintFlags  = QVector4D(lushTint.x(), lushTint.y(), lushTint.z(), 0.0f);
                                chunk.propInstances[PROP_ANEMONE_PEN].push_back(inst);
                            }
                        }
                    }
                }

                // ------------------------------------------------------------
                // 8. Надводная колышущаяся трава на уступах кекуров и скалах кальдеры (PROP_CLIFF_GRASS)
                // ------------------------------------------------------------
                // chunk.maxBounds.y() хранит maxY + 32.0f, поэтому порог +33.5f быстро отсекает чисто подводные чанки
                if (chunk.maxBounds.y() > ProcMath::WATER_SURFACE_Y + 33.5f) {
                    for (int p = 0; p < 28; ++p) {
                        float hx = ProcMath::hash3(chunk.cx, chunk.cz, p * 5 + 1, seed + 3907u);
                        float hz = ProcMath::hash3(chunk.cx, chunk.cz, p * 5 + 2, seed + 3907u);
                        float hr = ProcMath::hash3(chunk.cx, chunk.cz, p * 5 + 3, seed + 3907u);
                        float hc = ProcMath::hash3(chunk.cx, chunk.cz, p * 5 + 4, seed + 3907u);

                        float cxP = originX + 4.0f + hx * (CHUNK_SZ - 8.0f);
                        float czP = originZ + 4.0f + hz * (CHUNK_SZ - 8.0f);
                        float cyP = data.collision.sampleHeight(cxP, czP);
                        if (cyP < ProcMath::WATER_SURFACE_Y + 2.0f || cyP > 74.0f) continue;

                        QVector3D cNorm = data.collision.sampleNormal(cxP, czP);
                        if (cNorm.y() < 0.58f) continue;

                        float patchMask = sampleCliffGrassPatchMask(cxP, czP, seed);
                        if (patchMask < 0.22f) continue;

                        int tufts = 4 + static_cast<int>(hr * 3.9f);
                        float patchR = 3.2f + hr * 3.8f;

                        for (int t = 0; t < tufts; ++t) {
                            float th1 = ProcMath::hash3(ci, p * 17 + t, 1, seed + 3923u);
                            float th2 = ProcMath::hash3(ci, p * 17 + t, 2, seed + 3923u);
                            float th3 = ProcMath::hash3(ci, p * 17 + t, 3, seed + 3923u);
                            float th4 = ProcMath::hash3(ci, p * 17 + t, 4, seed + 3923u);

                            float ang = th1 * TWO_PI;
                            float rad = std::sqrt(th2) * patchR;
                            float wx = cxP + std::cos(ang) * rad;
                            float wz = czP + std::sin(ang) * rad;
                            float wy = data.collision.sampleHeight(wx, wz);
                            if (wy < ProcMath::WATER_SURFACE_Y + 1.85f || wy > 74.0f) continue;

                            QVector3D norm = data.collision.sampleNormal(wx, wz);
                            if (norm.y() < 0.54f) continue;

                            // Проверяем, чтобы куст не висел над самым краем отвесного обрыва
                            float footR = 1.15f;
                            float minFootY = std::min({
                                wy,
                                data.collision.sampleHeight(wx - footR, wz),
                                data.collision.sampleHeight(wx + footR, wz),
                                data.collision.sampleHeight(wx, wz - footR),
                                data.collision.sampleHeight(wx, wz + footR)
                            });
                            if ((wy - minFootY) > 1.25f) continue;

                            float edgeAtten = 1.08f - 0.30f * (rad / patchR);
                            float scXZ = (0.88f + th3 * 0.78f) * edgeAtten;
                            float scY  = (0.84f + th4 * 0.72f) * (0.78f + 0.42f * patchMask);

                            float yaw = 0.0f, pitch = 0.0f;
                            QVector3D tiltUp = (norm * 0.42f + QVector3D(0.0f, 0.58f, 0.0f)).normalized();
                            alignUpAxisToDir(tiltUp, yaw, pitch);

                            // Естественная палитра прибрежных скал: от сочной зелени до подсушенной солнцем оливы
                            QVector3D tint = (hc < 0.52f)
                                                 ? QVector3D(0.86f + th3 * 0.24f, 0.96f + th4 * 0.18f, 0.78f + th1 * 0.18f)
                                                 : QVector3D(1.04f + th3 * 0.18f, 0.96f + th4 * 0.14f, 0.72f + th1 * 0.16f);

                            float sink = 0.16f + (1.0f - norm.y()) * 0.55f;

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(wx, wy - sink, wz, scXZ);
                            inst.rotScaleY  = QVector4D(yaw + th1 * TWO_PI, pitch, 0.0f, scY);
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                            chunk.propInstances[PROP_CLIFF_GRASS].push_back(inst);
                        }
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    // Объединяем статические абиссальные перья (detailRange) и коллайдеры
    size_t totalVerts = 0;
    size_t totalInds  = 0;
    for (const auto& t : chunkTemps) {
        totalVerts += t.detailVerts.size();
        totalInds  += t.detailInds.size();
    }
    data.propVertices.reserve(totalVerts + 80000);
    data.propIndices.reserve(totalInds + 140000);

    for (int ci = 0; ci < TOTAL_CHUNKS; ++ci) {
        WorldChunk& chunk = data.chunks[ci];
        const ChunkPropTemp& tmp = chunkTemps[ci];

        uint32_t vBase = static_cast<uint32_t>(data.propVertices.size());
        data.propVertices.insert(data.propVertices.end(), tmp.detailVerts.begin(), tmp.detailVerts.end());

        chunk.detailRange.indexOffset = static_cast<uint32_t>(data.propIndices.size());
        chunk.detailRange.indexCount  = static_cast<uint32_t>(tmp.detailInds.size());
        for (uint32_t idx : tmp.detailInds) {
            data.propIndices.push_back(vBase + idx);
        }

        for (const auto& sph : tmp.spheres) {
            uint32_t colId = static_cast<uint32_t>(data.collision.spheres.size());
            data.collision.spheres.push_back(sph);
            chunk.sphereColliderIds.push_back(colId);
        }
    }
}
