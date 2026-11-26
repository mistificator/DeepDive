#include "WorldGenerator.h"
#include "Noise.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

// Маска естественных лужаек и карманов травы на надводных скалах и верхушках кекуров
inline float sampleCliffGrassPatchMask(float wx, float wz, uint32_t seed) {
    float n1 = ProcMath::fbm(wx * 0.022f - 4.7f, 0.6f, wz * 0.022f + 2.3f, 3, seed + 3701u);
    float n2 = ProcMath::fbm(wx * 0.064f + 1.9f, 1.3f, wz * 0.064f - 5.1f, 2, seed + 3719u);
    return ProcMath::smoothstep(-0.36f, 0.16f, n1 * 0.72f + n2 * 0.28f);
}

// Согласованная с InstPropVert ориентация: направляет локальную ось +Y вдоль единичного вектора dir.
// На почти плоской поверхности (уклон < ~1.7°) использует произвольный разворот flatYaw без наклона.
inline void alignUpAxisToDir(const QVector3D& dir, float& outYaw, float& outPitch, float flatYaw = 0.0f) {
    QVector3D d = dir.normalized();
    float hLen = std::sqrt(d.x() * d.x() + d.z() * d.z());
    if (hLen < 0.03f && d.y() > 0.0f) {
        outPitch = 0.0f;
        outYaw = flatYaw;
        return;
    }
    outPitch = std::acos(ProcMath::clamp(d.y(), -1.0f, 1.0f));
    outYaw = (hLen > 1e-4f) ? std::atan2(-d.x(), -d.z()) : flatYaw;
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
    // Плотная прибрежная кочка из тонких травинок (верхний ярус + густой подшёрсток)
    const int blades = highLod ? 76 : 26;
    const int segs   = highLod ? 3  : 2;

    const QVector3D rootCol(0.09f, 0.22f, 0.06f);
    const QVector3D midCol(0.22f, 0.50f, 0.13f);
    const QVector3D tipCol(0.54f, 0.67f, 0.20f);
    const QVector3D dryTipCol(0.74f, 0.65f, 0.25f);

    for (int b = 0; b < blades; ++b) {
        float h1 = ProcMath::hash3(b, 1, 17, 9103u);
        float h2 = ProcMath::hash3(b, 2, 23, 9103u);
        float h3 = ProcMath::hash3(b, 3, 29, 9103u);
        float h4 = ProcMath::hash3(b, 4, 31, 9103u);

        float posAng = (TWO_PI * static_cast<float>(b)) / static_cast<float>(blades) + (h1 - 0.5f) * 0.55f;
        float posRad = std::sqrt((static_cast<float>(b) + 0.30f) / static_cast<float>(blades)) * 0.95f;
        // Внешние корни уходят глубже, чтобы края дернины плотно сидели даже на выпуклой скале
        float rootY  = -0.32f - posRad * 0.26f;
        QVector3D root(std::cos(posAng) * posRad, rootY, std::sin(posAng) * posRad);

        float bladeAng = posAng + (h2 - 0.5f) * 1.55f;
        bool tallPlume  = (b % 5 == 0);
        bool underbrush = (b % 3 == 1);
        float height = tallPlume ? (1.72f + h3 * 0.72f)
                                 : (underbrush ? (0.98f + h3 * 0.45f) : (1.28f + h3 * 0.58f));
        float width  = (highLod ? (0.022f + h2 * 0.014f) : (0.044f + h2 * 0.018f)) * (tallPlume ? 1.08f : 0.96f);
        float lean   = (tallPlume ? 0.66f : 0.38f) + h4 * 0.36f;

        QVector3D dir(std::cos(bladeAng), 0.0f, std::sin(bladeAng));
        QVector3D side(-dir.z(), 0.0f, dir.x());
        QVector3D endTipCol = (h4 > 0.68f) ? dryTipCol : tipCol;

        uint32_t vb = static_cast<uint32_t>(verts.size());
        for (int s = 0; s <= segs; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(segs);
            float w = std::max(0.002f, width * (1.0f - t * 0.94f));
            float archY = height * (t * (1.18f - 0.24f * t));
            float archR = lean * (t * t);

            QVector3D p = root + dir * archR + QVector3D(0.0f, archY, 0.0f);
            QVector3D col = (t < 0.48f)
                                ? ProcMath::mix(rootCol, midCol, t / 0.48f)
                                : ProcMath::mix(midCol, endTipCol, (t - 0.48f) / 0.52f);
            col *= (0.84f + h1 * 0.28f);

            QVector3D norm = (QVector3D(0.0f, 0.85f - t * 0.25f, 0.0f) + dir * (0.32f + t * 0.45f)).normalized();
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

    int stalks = 2 + static_cast<int>(penSeed % 2u);
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
// ПРОТОТИПЫ ФЛОРЫ И РИФОВОЙ БИОТЫ (АРХЕТИПЫ 2..8, LOD0 + LOD1)
// ============================================================================

void WorldGenerator::generateFloraPrototypes(WorldData& data) {
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
    data.propPrototypes[PROP_CLIFF_GRASS].baseRadius = 1.15f;
}

// ============================================================================
// ЭКОЛОГИЧЕСКОЕ ЗАСЕЛЕНИЕ ФЛОРОЙ (ЛАМИНАРИЯ, КОВЁР, КОРАЛЛЫ, ГУБКИ, НАДВОДНАЯ ТРАВА)
// ============================================================================

void WorldGenerator::generateChunkFlora(WorldData& data, uint32_t seed) {
    constexpr int C = WorldData::CHUNKS_PER_SIDE;
    constexpr int TOTAL_CHUNKS = C * C;
    constexpr float CHUNK_SZ = WorldData::CHUNK_SIZE;
    constexpr float HALF_W = ProcMath::WORLD_HALF_SIZE;
    constexpr float TERRAIN_BOUNDS_Y_PAD = 32.0f; // совпадает с запасом в generateTerrainChunks
    constexpr float MAX_UNDERWATER_FLORA_TOP_Y = ProcMath::WATER_SURFACE_Y - 1.65f;
    const auto& spires = ProcMath::getCentralSpires(seed);

    struct ChunkFloraTemp {
        std::vector<EnvVertex> detailVerts;
        std::vector<uint32_t>  detailInds;
    };

    std::vector<ChunkFloraTemp> chunkTemps(TOTAL_CHUNKS);
    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (unsigned int th = 0; th < numThreads; ++th) {
        int startChunk = (th * TOTAL_CHUNKS) / numThreads;
        int endChunk   = ((th + 1) * TOTAL_CHUNKS) / numThreads;

        workers.emplace_back([&, startChunk, endChunk]() {
            for (int ci = startChunk; ci < endChunk; ++ci) {
                WorldChunk& chunk = data.chunks[ci];
                ChunkFloraTemp& tmp = chunkTemps[ci];

                float originX = -HALF_W + chunk.cx * CHUNK_SZ;
                float originZ = -HALF_W + chunk.cz * CHUNK_SZ;
                float chunkCenterX = originX + CHUNK_SZ * 0.5f;
                float chunkCenterZ = originZ + CHUNK_SZ * 0.5f;
                float centerWallF  = ProcMath::sampleBasinWallFactor(chunkCenterX, chunkCenterZ, seed);
                float terrainMaxY  = chunk.maxBounds.y() - TERRAIN_BOUNDS_Y_PAD;

                // ------------------------------------------------------------
                // 1. Рощи гигантских пышных колонн ламинарии (кластерные аллеи)
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

                            constexpr float MIN_KELP_H = 7.5f;
                            float maxH = std::min(25.0f, (ProcMath::WATER_SURFACE_Y - 2.4f) - wy);
                            if (maxH < MIN_KELP_H) continue;
                            float height = MIN_KELP_H + kh3 * (maxH - MIN_KELP_H);

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
                // 2. Сплошной придонный ковёр травы и водорослей (PROP_GROUND_CARPET)
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
                        alignUpAxisToDir(tiltN, yaw, pitch, hr * TWO_PI);

                        float scaleXZ = 1.05f + hs * 0.85f;
                        float scaleY  = 0.72f + hr * 0.68f;
                        if (wy + scaleY * 1.80f > MAX_UNDERWATER_FLORA_TOP_Y) continue;

                        QVector3D tint(0.84f + hs * 0.30f, 0.88f + hr * 0.24f, 0.84f + hx * 0.28f);

                        PropGpuInstance inst;
                        inst.posScaleXZ = QVector4D(wx, wy - 0.14f, wz, scaleXZ);
                        inst.rotScaleY  = QVector4D(yaw, pitch, 0.0f, scaleY);
                        inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 1.0f);
                        chunk.propInstances[PROP_GROUND_CARPET].push_back(inst);
                    }
                }

                // ------------------------------------------------------------
                // 3. Рифовые кораллы: ветвистые кусты (PROP_CORAL_BUSH) и пластинчатые (PROP_TABLE_FAN)
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
                            alignUpAxisToDir((norm * 0.55f + QVector3D(0.0f, 0.45f, 0.0f)).normalized(),
                                             yaw, pitch, hc * TWO_PI);

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
                            alignUpAxisToDir((norm * 0.28f + QVector3D(0.0f, 0.72f, 0.0f)).normalized(),
                                             yaw, pitch, hc * TWO_PI);
                            QVector3D pos(wx, minFootY + 0.04f, wz);

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(pos.x(), pos.y(), pos.z(), sc);
                            inst.rotScaleY  = QVector4D(yaw, pitch, 0.0f, sc * 0.92f);
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                            chunk.propInstances[PROP_TABLE_FAN].push_back(inst);
                        }
                    }
                }

                // ------------------------------------------------------------
                // 4. Семейки толстостенных полых губок (PROP_BARREL_SPONGE) + абиссальные перья
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
                            if (sy <= -30.0f) continue;

                            QVector3D sNorm = (k == 0) ? cNorm : data.collision.sampleNormal(sx, sz);
                            if (sNorm.y() < 0.55f) continue;

                            float scY  = scYs[k];
                            if (sy + scY * 1.50f > MAX_UNDERWATER_FLORA_TOP_Y) continue;

                            float scXZ = ProcMath::clamp(scY * (0.72f + kh2 * 0.26f), 0.42f, 1.32f);

                            float yaw = 0.0f, pitch = 0.0f;
                            QVector3D leanDir = (sNorm + QVector3D(std::cos(ang) * 0.22f, 0.25f, std::sin(ang) * 0.22f)).normalized();
                            alignUpAxisToDir(leanDir, yaw, pitch, ang);

                            float sink = (0.20f + (1.0f - sNorm.y()) * 0.75f) * std::max(scXZ, 0.55f);
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
                // 5. Кустистые водоросли, анемоны и островки зелени на верхних плато (PROP_ANEMONE_PEN)
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

                    if (terrainMaxY > -1.0f) {
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
                // 6. Надводная густая колышущаяся трава на кекурах, уступах и скалах кальдеры (PROP_CLIFF_GRASS)
                // ------------------------------------------------------------
                if (terrainMaxY > ProcMath::WATER_SURFACE_Y + 0.5f) {
                    auto tryPlaceGrassTuft = [&](float wx, float wz, float patchMask,
                                                 float hScaleXZ, float hScaleY, float hTint) {
                        if (wx < originX || wx >= originX + CHUNK_SZ ||
                            wz < originZ || wz >= originZ + CHUNK_SZ) {
                            return;
                        }
                        float wy = data.collision.sampleHeight(wx, wz);
                        if (wy < ProcMath::WATER_SURFACE_Y + 0.95f || wy > 78.0f) return;

                        QVector3D norm = data.collision.sampleNormal(wx, wz);
                        if (norm.y() < 0.30f) return;

                        float scXZ = 0.85f + hScaleXZ * 0.65f;
                        float scY  = (0.76f + hScaleY * 0.72f) * (0.80f + 0.38f * patchMask);

                        float footR = 0.88f * scXZ;
                        float hL = data.collision.sampleHeight(wx - footR, wz);
                        float hR = data.collision.sampleHeight(wx + footR, wz);
                        float hD = data.collision.sampleHeight(wx, wz - footR);
                        float hU = data.collision.sampleHeight(wx, wz + footR);
                        float avgFootY = 0.25f * (hL + hR + hD + hU);
                        float curvDrop = std::max(0.0f, wy - avgFootY);
                        if (curvDrop > 1.55f) return;

                        float yaw = 0.0f, pitch = 0.0f;
                        QVector3D tiltUp = (norm * 0.70f + QVector3D(0.0f, 0.30f, 0.0f)).normalized();
                        alignUpAxisToDir(tiltUp, yaw, pitch, hTint * TWO_PI);

                        float sinkAlongN = 0.18f + curvDrop * 0.90f + (1.0f - norm.y()) * 0.44f;
                        QVector3D pos = QVector3D(wx, wy, wz) - norm * sinkAlongN;

                        QVector3D tint = (hTint < 0.58f)
                                             ? QVector3D(0.84f + hScaleXZ * 0.22f, 0.96f + hScaleY * 0.16f, 0.72f + hTint * 0.20f)
                                             : QVector3D(1.02f + hScaleXZ * 0.16f, 0.95f + hScaleY * 0.14f, 0.66f + hTint * 0.18f);

                        PropGpuInstance inst;
                        inst.posScaleXZ = QVector4D(pos.x(), pos.y(), pos.z(), scXZ);
                        inst.rotScaleY  = QVector4D(yaw, pitch, 0.0f, scY);
                        inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                        chunk.propInstances[PROP_CLIFF_GRASS].push_back(inst);
                    };

                    // 6.1. Плотная сетка по всем надводным скалам кальдеры и крупным массивам
                    constexpr int GRASS_GRID = 28;
                    for (int gz = 0; gz < GRASS_GRID; ++gz) {
                        for (int gx = 0; gx < GRASS_GRID; ++gx) {
                            int idx = gz * GRASS_GRID + gx;
                            float jx = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 1, seed + 3907u);
                            float jz = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 2, seed + 3907u);
                            float hr = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 3, seed + 3907u);
                            float hc = ProcMath::hash3(chunk.cx, chunk.cz, idx * 4 + 4, seed + 3907u);

                            float cxP = originX + ((static_cast<float>(gx) + 0.08f + jx * 0.84f) / GRASS_GRID) * CHUNK_SZ;
                            float czP = originZ + ((static_cast<float>(gz) + 0.08f + jz * 0.84f) / GRASS_GRID) * CHUNK_SZ;
                            float cyP = data.collision.sampleHeight(cxP, czP);
                            if (cyP < ProcMath::WATER_SURFACE_Y + 1.00f || cyP > 78.0f) continue;

                            QVector3D cNorm = data.collision.sampleNormal(cxP, czP);
                            if (cNorm.y() < 0.30f) continue;

                            float patchMask = sampleCliffGrassPatchMask(cxP, czP, seed);
                            if (patchMask < 0.10f) continue;

                            // Густая поляна из 4–7 перекрывающихся дернин
                            int tufts = 4 + static_cast<int>(hr * 3.9f);
                            float spreadR = 1.65f + hr * 1.65f;
                            for (int t = 0; t < tufts; ++t) {
                                float th1 = ProcMath::hash3(ci, idx * 11 + t, 1, seed + 3923u);
                                float th2 = ProcMath::hash3(ci, idx * 11 + t, 2, seed + 3923u);
                                float th3 = ProcMath::hash3(ci, idx * 11 + t, 3, seed + 3923u);
                                float th4 = ProcMath::hash3(ci, idx * 11 + t, 4, seed + 3923u);

                                float ang = th1 * TWO_PI;
                                float rad = (t == 0) ? 0.0f : (std::sqrt(th2) * spreadR);
                                tryPlaceGrassTuft(cxP + std::cos(ang) * rad,
                                                  czP + std::sin(ang) * rad,
                                                  patchMask, th3, th4, hc);
                            }
                        }
                    }

                    // 6.2. Густое озеленение шапок, плеч и уступов всех надводных центральных кекуров
                    for (size_t sIdx = 0; sIdx < spires.size(); ++sIdx) {
                        const auto& sp = spires[sIdx];
                        float padR = sp.radius * 1.35f;
                        if (sp.x + padR < originX || sp.x - padR > originX + CHUNK_SZ ||
                            sp.z + padR < originZ || sp.z - padR > originZ + CHUNK_SZ) {
                            continue;
                        }

                        constexpr int SPIRE_TUFT_PROBES = 140;
                        for (int k = 0; k < SPIRE_TUFT_PROBES; ++k) {
                            float h1 = ProcMath::hash3(static_cast<int>(sIdx), k, 1, seed + 4019u);
                            float h2 = ProcMath::hash3(static_cast<int>(sIdx), k, 2, seed + 4019u);
                            float h3 = ProcMath::hash3(static_cast<int>(sIdx), k, 3, seed + 4019u);
                            float h4 = ProcMath::hash3(static_cast<int>(sIdx), k, 4, seed + 4019u);

                            float ang = (TWO_PI * static_cast<float>(k)) / SPIRE_TUFT_PROBES + (h1 - 0.5f) * 0.35f;
                            float rad = std::sqrt(h2) * (sp.radius * 1.02f);
                            float baseX = sp.x + std::cos(ang) * rad;
                            float baseZ = sp.z + std::sin(ang) * rad;

                            float patchMask = sampleCliffGrassPatchMask(baseX, baseZ, seed);
                            if (patchMask < 0.04f && h3 < 0.25f) continue;

                            // 2–3 плотно сцепленных куста в каждой точке для сплошного ковра
                            int subTufts = (h4 > 0.45f) ? 3 : 2;
                            for (int st = 0; st < subTufts; ++st) {
                                float sh1 = ProcMath::hash3(static_cast<int>(sIdx), k * 5 + st, 1, seed + 4057u);
                                float sh2 = ProcMath::hash3(static_cast<int>(sIdx), k * 5 + st, 2, seed + 4057u);
                                float sh3 = ProcMath::hash3(static_cast<int>(sIdx), k * 5 + st, 3, seed + 4057u);
                                float offA = sh1 * TWO_PI;
                                float offR = (st == 0) ? 0.0f : (0.55f + sh2 * 0.95f);
                                tryPlaceGrassTuft(baseX + std::cos(offA) * offR,
                                                  baseZ + std::sin(offA) * offR,
                                                  std::max(patchMask, 0.42f), sh2, sh3, h1);
                            }
                        }
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    // Объединяем статические абиссальные перья (detailRange) в общий буфер пропов
    size_t totalVerts = 0;
    size_t totalInds  = 0;
    for (const auto& t : chunkTemps) {
        totalVerts += t.detailVerts.size();
        totalInds  += t.detailInds.size();
    }
    data.propVertices.reserve(data.propVertices.size() + totalVerts + 80000);
    data.propIndices.reserve(data.propIndices.size() + totalInds + 140000);

    for (int ci = 0; ci < TOTAL_CHUNKS; ++ci) {
        WorldChunk& chunk = data.chunks[ci];
        const ChunkFloraTemp& tmp = chunkTemps[ci];

        uint32_t vBase = static_cast<uint32_t>(data.propVertices.size());
        data.propVertices.insert(data.propVertices.end(), tmp.detailVerts.begin(), tmp.detailVerts.end());

        chunk.detailRange.indexOffset = static_cast<uint32_t>(data.propIndices.size());
        chunk.detailRange.indexCount  = static_cast<uint32_t>(tmp.detailInds.size());
        for (uint32_t idx : tmp.detailInds) {
            data.propIndices.push_back(vBase + idx);
        }
    }
}
