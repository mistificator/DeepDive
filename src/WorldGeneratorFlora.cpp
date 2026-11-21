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
        0, 1, 2,  0, 2, 3,  0, 3, 4,  0, 4, 1,
        5, 2, 1,  5, 3, 2,  5, 4, 3,  5, 1, 4
    };
    for (uint32_t idx : tris) inds.push_back(b + idx);
}

// ============================================================================
// ПРОТОТИП 0: УГЛОВАТЫЙ СЛОИСТЫЙ ВАЛУН (PROP_BOULDER_CHUNK)
// ============================================================================

void buildBoulderPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                           int stacks, int slices, uint32_t rockSeed) {
    std::vector<QVector3D> pts((stacks + 1) * (slices + 1));

    for (int st = 0; st <= stacks; ++st) {
        float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int sl = 0; sl <= slices; ++sl) {
            int slWrap = sl % slices;
            float theta = TWO_PI * static_cast<float>(slWrap) / static_cast<float>(slices);
            QVector3D dir(sp * std::cos(theta), cp, sp * std::sin(theta));

            float macro   = ProcMath::fbm(dir.x() * 1.65f, dir.y() * 1.65f, dir.z() * 1.65f, 3, rockSeed);
            float ridged  = ProcMath::ridgedFbm(dir.x() * 2.4f + 1.3f, dir.y() * 2.4f, dir.z() * 2.4f - 0.9f, 3, rockSeed + 41u);
            float cracks  = ProcMath::voronoiCracks2D(dir.x() * 2.6f + dir.y() * 1.1f, dir.z() * 2.6f - dir.y() * 0.9f, rockSeed + 83u);
            float crackDip = (1.0f - ProcMath::smoothstep(0.03f, 0.22f, cracks)) * 0.14f;

            // Лёгкая ступенчатость и приплюснутое основание для естественной посадки
            float r = 0.88f + macro * 0.26f + ridged * 0.24f - crackDip;
            float yMod = (dir.y() < -0.35f) ? (0.82f + 0.18f * (dir.y() + 1.0f)) : 1.0f;
            float flatTop = (dir.y() > 0.55f) ? (1.0f - 0.14f * ProcMath::smoothstep(0.55f, 0.95f, dir.y())) : 1.0f;
            pts[st * (slices + 1) + sl] = QVector3D(dir.x() * r, dir.y() * r * yMod * flatTop, dir.z() * r);
        }
    }

    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    for (int st = 0; st <= stacks; ++st) {
        for (int sl = 0; sl <= slices; ++sl) {
            QVector3D p = pts[st * (slices + 1) + sl];
            int stU = std::max(0, st - 1), stD = std::min(stacks, st + 1);
            int slL = (sl - 1 + slices) % slices, slR = (sl + 1) % slices;
            QVector3D t1 = pts[stD * (slices + 1) + sl] - pts[stU * (slices + 1) + sl];
            QVector3D t2 = pts[st * (slices + 1) + slR] - pts[st * (slices + 1) + slL];
            QVector3D n = QVector3D::crossProduct(t2, t1);
            if (n.lengthSquared() < 1e-6f) n = p;
            n.normalize();

            float moss = ProcMath::smoothstep(0.32f, 0.82f, n.y());
            float shade = ProcMath::clamp(0.76f + (p.length() - 0.78f) * 0.75f, 0.55f, 1.08f);
            QVector3D stoneCol = QVector3D(0.70f, 0.74f, 0.77f) * shade;
            QVector3D mossCol  = QVector3D(0.42f, 0.72f, 0.54f) * shade;
            QVector3D col = ProcMath::mix(stoneCol, mossCol, moss * 0.52f);
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

// ============================================================================
// ПРОТОТИП 1: ВЫСТУПАЮЩИЙ СКАЛЬНЫЙ БЛОК ДЛЯ ОТВЕСНЫХ СТЕН (PROP_CLIFF_CRAG)
// ============================================================================

void buildCliffCragPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds,
                             int stacks, int slices, uint32_t cragSeed) {
    std::vector<QVector3D> pts((stacks + 1) * (slices + 1));

    for (int st = 0; st <= stacks; ++st) {
        float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int sl = 0; sl <= slices; ++sl) {
            int slWrap = sl % slices;
            float theta = TWO_PI * static_cast<float>(slWrap) / static_cast<float>(slices);
            QVector3D dir(sp * std::cos(theta), cp, sp * std::sin(theta));

            float crag = ProcMath::ridgedFbm(dir.x() * 2.2f + 3.1f, dir.y() * 2.8f, dir.z() * 2.1f, 3, cragSeed);
            float strata = std::sin(dir.y() * 8.5f + crag * 2.4f) * 0.08f;

            // Вытягиваем блок вдоль +Z (выступ из стены), с глубоким корнем в -Z и плоской полкой сверху
            float sx = 1.18f;
            float sy = 0.78f;
            float sz = (dir.z() >= 0.0f) ? 1.32f : 0.85f;
            float r = 0.84f + crag * 0.30f + strata;

            float x = dir.x() * sx * r;
            float y = dir.y() * sy * r;
            float z = dir.z() * sz * r + 0.18f;

            // Создаём нависающий каменный карниз на переднем крае
            if (z > 0.2f && y > 0.0f) {
                y *= 0.88f;
                z += 0.14f * ProcMath::smoothstep(0.0f, 0.75f, dir.y());
            }
            pts[st * (slices + 1) + sl] = QVector3D(x, y, z);
        }
    }

    uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    for (int st = 0; st <= stacks; ++st) {
        for (int sl = 0; sl <= slices; ++sl) {
            QVector3D p = pts[st * (slices + 1) + sl];
            int stU = std::max(0, st - 1), stD = std::min(stacks, st + 1);
            int slL = (sl - 1 + slices) % slices, slR = (sl + 1) % slices;
            QVector3D t1 = pts[stD * (slices + 1) + sl] - pts[stU * (slices + 1) + sl];
            QVector3D t2 = pts[st * (slices + 1) + slR] - pts[st * (slices + 1) + slL];
            QVector3D n = QVector3D::crossProduct(t2, t1);
            if (n.lengthSquared() < 1e-6f) n = p;
            n.normalize();

            float topShelf = ProcMath::smoothstep(0.28f, 0.78f, n.y());
            float underDark = ProcMath::smoothstep(-0.15f, -0.75f, n.y());
            QVector3D baseRock(0.60f, 0.68f, 0.72f);
            QVector3D mossTop(0.46f, 0.76f, 0.56f);
            QVector3D col = ProcMath::mix(baseRock, mossTop, topShelf * 0.65f);
            col *= (1.0f - underDark * 0.34f);
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

        // Профиль кроны: густая колонна с чуть более пышной серединой и макушкой
        float canopyProfile = 0.68f + 0.48f * std::sin(std::pow(t, 0.72f) * PI * 0.92f);
        float leafLen = (1.65f + h2 * 1.15f) * canopyProfile;
        float leafMaxW = (highLod ? 0.36f : 0.58f) * (0.78f + h3 * 0.42f);
        float riseAmp = (0.42f + h1 * 0.52f) * (0.55f + 0.65f * t);
        float droopAmp = 0.38f + (1.0f - t) * 0.55f;

        QVector3D root = dir * 0.08f + QVector3D(0.0f, yRoot, 0.0f);
        QVector3D baseCol = ProcMath::mix(leafBaseCol, leafMidCol, t * 0.75f + h3 * 0.25f);
        QVector3D tipCol  = ProcMath::mix(leafMidCol, leafTipCol, 0.35f + 0.65f * t);

        if (highLod) {
            // 3 вершины на поперечное сечение (V-ребро в центре для объёмного блика)
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

    // Профиль замкнутой толстостенной чаши: низ снаружи -> бугристый бок -> закруглённый мясистый край -> глубокое тёмное нутро
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
            // Закруглённый толстый мясистый обод сверху
            {0.58f,  1.46f, 1.08f, 0.10f, false},
            {0.47f,  1.45f, 0.96f, 0.09f, true},
            // Внутренняя полость уходит в глубину и темнеет
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

            // Бугристые органические ребра и волнистая кромка отверстия
            float nBump = ProcMath::fbm(ca * 2.1f + 1.5f, rd.y * 2.4f, sa * 2.1f - 0.8f, 3, 5209u);
            float flute = std::sin(a * 6.0f + rd.y * 2.0f) * 0.055f;
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

    for (int r = 0; r + 1 < rings; ++r) {
        for (int sl = 0; sl < slices; ++sl) {
            uint32_t i00 = baseIdx + r * (slices + 1) + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + (slices + 1);
            uint32_t i11 = i01 + 1;
            inds.insert(inds.end(), {i00, i10, i01, i10, i11, i01});
        }
    }

    // Тёмное дно внутри полости губки
    uint32_t centerBottom = static_cast<uint32_t>(verts.size());
    float innerFloorY = profile.back().y - 0.08f;
    verts.push_back({QVector3D(0.0f, innerFloorY, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
                     neutralWarm * 0.16f, QVector2D(2.0f, 0.0f)});
    uint32_t lastRing = baseIdx + (rings - 1) * (slices + 1);
    for (int sl = 0; sl < slices; ++sl) {
        inds.insert(inds.end(), {lastRing + sl, lastRing + sl + 1, centerBottom});
    }
}

// ============================================================================
// ПРОТОТИП 4: ПЛОТНЫЙ КОВЁР ПРИДОННОЙ ТРАВЫ (PROP_GROUND_CARPET)
// ============================================================================

void buildGroundCarpetPrototype(std::vector<EnvVertex>& verts, std::vector<uint32_t>& inds, bool highLod) {
    const int blades = highLod ? 26 : 9;
    const int segs   = highLod ? 3  : 2;

    // Светлый нейтральный градиент — окрашивается биомным uCarpetTint (изумруд / пурпурно-розовый грот)
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

    QVector3D root(0.0f, -0.24f, 0.0f);
    int trunks = highLod ? 5 : 4;
    int sides  = highLod ? 5 : 3;

    // Небольшой каменный корень
    appendTaperedTube(verts, inds, root, QVector3D(0.0f, 0.16f, 0.0f),
                      0.32f, 0.22f, sides, baseCol * 0.78f, baseCol, 2.0f, 0.0f, 0.0f, false);

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
    const QVector3D underCol(0.45f, 0.42f, 0.46f);

    int tiers = highLod ? 3 : 2;
    int segs  = highLod ? 14 : 7;

    appendTaperedTube(verts, inds, QVector3D(0.0f, -0.32f, 0.0f), QVector3D(0.0f, 0.48f, 0.0f),
                      0.28f, 0.20f, highLod ? 6 : 4, underCol, innerCol, 2.0f, 0.0f, 0.0f, false);

    for (int tier = 0; tier < tiers; ++tier) {
        float rad = 1.38f - tier * 0.34f;
        float yBase = 0.42f + tier * 0.40f;
        float offX = (tier == 1) ? 0.24f : (tier == 2 ? -0.18f : 0.0f);
        float offZ = (tier == 1) ? -0.14f : (tier == 2 ? 0.20f : 0.0f);
        QVector3D discC(offX, yBase, offZ);

        // Верхняя волнистая поверхность пластины
        uint32_t topC = static_cast<uint32_t>(verts.size());
        verts.push_back({discC + QVector3D(0.0f, 0.14f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f), innerCol, QVector2D(2.0f, 0.0f)});
        uint32_t topRim = static_cast<uint32_t>(verts.size());

        // Нижняя поверхность (чтобы коралл был объёмным при взгляде снизу)
        uint32_t botC = topRim + segs + 1;
        uint32_t botRim = botC + 1;

        for (int s = 0; s <= segs; ++s) {
            float a = (TWO_PI * s) / segs;
            float wave = 1.0f + 0.18f * std::sin(a * 5.0f + tier * 1.4f) + 0.08f * std::cos(a * 3.0f);
            float curl = 0.10f * std::sin(a * 4.0f + tier);
            QVector3D p = discC + QVector3D(std::cos(a) * rad * wave, curl + 0.06f, std::sin(a) * rad * wave);
            QVector3D n(std::cos(a) * 0.22f, 0.97f, std::sin(a) * 0.22f);
            n.normalize();
            verts.push_back({p, n, rimCol, QVector2D(2.0f, 0.0f)});
        }

        verts.push_back({discC - QVector3D(0.0f, 0.12f, 0.0f), QVector3D(0.0f, -1.0f, 0.0f), underCol, QVector2D(2.0f, 0.0f)});
        for (int s = 0; s <= segs; ++s) {
            float a = (TWO_PI * s) / segs;
            float wave = 1.0f + 0.18f * std::sin(a * 5.0f + tier * 1.4f) + 0.08f * std::cos(a * 3.0f);
            float curl = 0.10f * std::sin(a * 4.0f + tier);
            QVector3D p = discC + QVector3D(std::cos(a) * rad * wave, curl - 0.04f, std::sin(a) * rad * wave);
            QVector3D n(std::cos(a) * 0.25f, -0.96f, std::sin(a) * 0.25f);
            n.normalize();
            verts.push_back({p, n, underCol * 1.25f, QVector2D(2.0f, 0.0f)});
        }

        for (int s = 0; s < segs; ++s) {
            inds.insert(inds.end(), {topC, topRim + s, topRim + s + 1});
            inds.insert(inds.end(), {botC, botRim + s + 1, botRim + s});
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

    QVector3D rootBase(0.0f, -0.18f, 0.0f);

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
} // namespace

// ============================================================================
// ГЕНЕРАЦИЯ МИРА (точка входа + рельеф + прототипы + плотное заселение флорой)
// ============================================================================

WorldData WorldGenerator::generate(uint32_t seed) {
    WorldData data;
    data.collision.seed = seed;
    data.collision.chunksPerSide = WorldData::CHUNKS_PER_SIDE;
    data.collision.chunkSize = WorldData::CHUNK_SIZE;
    data.collision.worldHalfSize = ProcMath::WORLD_HALF_SIZE;

    buildTerrainLodIndices(data);
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

                        QVector3D sandCol(0.58f, 0.56f, 0.45f);
                        QVector3D mossRock(0.20f, 0.38f, 0.30f);
                        QVector3D sharpCrag(0.21f, 0.25f, 0.29f);
                        QVector3D cliffBasalt(0.15f, 0.19f, 0.24f);
                        QVector3D abyssSlate(0.08f, 0.11f, 0.16f);
                        QVector3D grottoTint(0.36f, 0.22f, 0.44f);

                        float grottoMask = ProcMath::smoothstep(0.18f, 0.68f,
                                                                ProcMath::fbm(wx * 0.006f, 0.0f, wz * 0.006f, 2, seed + 444u) * 0.5f + 0.5f);

                        QVector3D col = ProcMath::mix(sandCol, mossRock, ProcMath::clamp(slope * 2.4f + heightMix * 0.52f, 0.0f, 1.0f));
                        col = ProcMath::mix(col, sharpCrag, ProcMath::clamp(ProcMath::smoothstep(0.24f, 0.62f, slope) * 0.85f + spireMix * 0.65f, 0.0f, 1.0f));
                        col = ProcMath::mix(col, grottoTint, grottoMask * 0.32f * (1.0f - wallF) * (1.0f - abyssMix));
                        col = ProcMath::mix(col, cliffBasalt, ProcMath::clamp(wallF * 0.85f + ProcMath::smoothstep(22.0f, 36.0f, wy) * 0.55f, 0.0f, 1.0f));

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

                        float maxEdgeSlope = std::max({
                                                 std::abs(wy - hL), std::abs(wy - hR),
                                                 std::abs(wy - hD), std::abs(wy - hU)
                                             }) / CELL_SZ;
                        float localFlatness = 1.0f / std::sqrt(1.0f + maxEdgeSlope * maxEdgeSlope);
                        float steepness = ProcMath::clamp(1.0f - localFlatness, 0.0f, 1.0f);

                        EnvVertex& v = dst[iz * G + ix];
                        v.pos    = QVector3D(wx, wy, wz);
                        v.normal = norm;
                        v.color  = col;
                        v.param  = QVector2D(0.0f, steepness);
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
                chunk.maxBounds = QVector3D(originX + CHUNK_SZ, maxY + 32.0f, originZ + CHUNK_SZ);
                chunk.center    = (chunk.minBounds + chunk.maxBounds) * 0.5f;
                chunk.boundingRadius = (chunk.maxBounds - chunk.center).length();
            }
        });
    }
    for (auto& th : workers) th.join();
}

void WorldGenerator::generatePropPrototypes(WorldData& data) {
    // 0: Слоистый угловатый валун
    buildBoulderPrototype(data.propPrototypes[PROP_BOULDER_CHUNK].lod0Verts,
                          data.propPrototypes[PROP_BOULDER_CHUNK].lod0Inds, 11, 13, 3011u);
    buildBoulderPrototype(data.propPrototypes[PROP_BOULDER_CHUNK].lod1Verts,
                          data.propPrototypes[PROP_BOULDER_CHUNK].lod1Inds, 5, 6, 3011u);
    data.propPrototypes[PROP_BOULDER_CHUNK].baseRadius = 1.35f;

    // 1: Выступающий скальный блок для крутых обрывов
    buildCliffCragPrototype(data.propPrototypes[PROP_CLIFF_CRAG].lod0Verts,
                            data.propPrototypes[PROP_CLIFF_CRAG].lod0Inds, 10, 12, 3191u);
    buildCliffCragPrototype(data.propPrototypes[PROP_CLIFF_CRAG].lod1Verts,
                            data.propPrototypes[PROP_CLIFF_CRAG].lod1Inds, 5, 6, 3191u);
    data.propPrototypes[PROP_CLIFF_CRAG].baseRadius = 1.75f;

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
}

// ============================================================================
// ПЛОТНОЕ ЭКОЛОГИЧЕСКОЕ ЗАСЕЛЕНИЕ ЧАНКОВ (СКАЛЫ, ЛАМИНАРИЯ, КОВЁР, ГУБКИ, КОРАЛЛЫ)
// ============================================================================

void WorldGenerator::generateChunkProps(WorldData& data, uint32_t seed) {
    constexpr int C = WorldData::CHUNKS_PER_SIDE;
    constexpr int TOTAL_CHUNKS = C * C;
    constexpr float CHUNK_SZ = WorldData::CHUNK_SIZE;
    constexpr float HALF_W = ProcMath::WORLD_HALF_SIZE;
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
                // 1. Валуны и россыпи камней на дне и рифах (крупные + средние + мелкие)
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

                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                        if (wy > 22.0f) continue;

                        float scaleXZ = isHeroBoulder
                                            ? (2.5f + hs * 4.8f + wallF * 3.2f)
                                            : (0.75f + hs * 1.75f);
                        float scaleY = scaleXZ * (0.64f + hr * 0.52f);

                        // Надёжное заглубление основания в грунт
                        QVector3D center(wx, wy + scaleY * 0.24f, wz);
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
                // 2. 3D-наращивание скал на отвесных стенах каньона, утёсов и кальдеры
                // ------------------------------------------------------------
                for (int c = 0; c < 44; ++c) {
                    float hx = ProcMath::hash3(chunk.cx, chunk.cz, c * 4 + 1, seed + 257u);
                    float hz = ProcMath::hash3(chunk.cx, chunk.cz, c * 4 + 2, seed + 257u);
                    float hs = ProcMath::hash3(chunk.cx, chunk.cz, c * 4 + 3, seed + 257u);
                    float hr = ProcMath::hash3(chunk.cx, chunk.cz, c * 4 + 4, seed + 257u);

                    float wx = originX + 2.0f + hx * (CHUNK_SZ - 4.0f);
                    float wz = originZ + 2.0f + hz * (CHUNK_SZ - 4.0f);
                    float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                    if (wy < -58.0f || wy > 36.0f) continue;

                    QVector3D norm = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.5f);
                    // Встраиваем только в крутые склоны и отвесные обрывы
                    if (norm.y() > 0.72f) continue;

                    float scaleXZ = 1.65f + hs * 3.40f;
                    float scaleY  = 1.30f + hr * 2.45f;

                    float yaw = 0.0f, pitch = 0.0f;
                    alignForwardAxisToDir(norm, yaw, pitch);
                    float roll = (hr - 0.5f) * 0.85f;

                    // Заглубляем блок вглубь породы на 0.45 * scaleXZ, чтобы не было щелей
                    QVector3D pos = QVector3D(wx, wy, wz) - norm * (scaleXZ * 0.36f);
                    float abyssDark = ProcMath::smoothstep(-24.0f, -50.0f, wy);
                    QVector3D tint = ProcMath::mix(
                        QVector3D(0.36f + hs * 0.14f, 0.46f + hr * 0.14f, 0.47f + hs * 0.10f),
                        QVector3D(0.14f, 0.18f, 0.24f), abyssDark);

                    PropGpuInstance inst;
                    inst.posScaleXZ = QVector4D(pos.x(), pos.y(), pos.z(), scaleXZ);
                    inst.rotScaleY  = QVector4D(yaw, pitch, roll, scaleY);
                    inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                    chunk.propInstances[PROP_CLIFF_CRAG].push_back(inst);

                    if (scaleXZ >= 2.8f) {
                        tmp.spheres.push_back({pos + norm * (scaleXZ * 0.45f), scaleXZ * 0.82f});
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

                        int stalksInGrove = 4 + static_cast<int>(grH * 4.9f); // 4..8 колонн в плотной роще
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

                            float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                            if (wy > 4.0f || wy < -36.0f) continue;

                            QVector3D n = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.4f);
                            if (n.y() < 0.56f) continue;

                            float maxH = std::min(25.0f, (ProcMath::WATER_SURFACE_Y - 2.2f) - wy);
                            if (maxH < 7.5f) continue;
                            float height = 8.5f + kh3 * (maxH - 8.5f);

                            float scaleY  = height / 14.0f;
                            float scaleXZ = 0.85f + kh2 * 0.52f;

                            QVector3D tint(0.86f + kh1 * 0.28f, 0.90f + kh3 * 0.22f, 0.82f + kh2 * 0.24f);
                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(wx, wy - 0.30f, wz, scaleXZ);
                            inst.rotScaleY  = QVector4D(kh1 * TWO_PI, (kh2 - 0.5f) * 0.08f, (kh3 - 0.5f) * 0.08f, scaleY);
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 2.0f); // w=2 -> биомный тинт ламинарии
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

                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                        if (wy < -33.0f || wy > 23.5f) continue;

                        QVector3D norm = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.3f);
                        if (norm.y() < 0.64f) continue;

                        float yaw = 0.0f, pitch = 0.0f;
                        // Частично наклоняем куст по склону, сохраняя стремление вверх
                        QVector3D tiltN = (norm * 0.65f + QVector3D(0.0f, 0.35f, 0.0f)).normalized();
                        alignUpAxisToDir(tiltN, yaw, pitch);

                        float scaleXZ = 1.05f + hs * 0.85f;
                        float scaleY  = 0.72f + hr * 0.68f;
                        QVector3D tint(0.84f + hs * 0.30f, 0.88f + hr * 0.24f, 0.84f + hx * 0.28f);

                        PropGpuInstance inst;
                        inst.posScaleXZ = QVector4D(wx, wy - 0.10f, wz, scaleXZ);
                        inst.rotScaleY  = QVector4D(yaw + hr * TWO_PI * 0.15f, pitch, 0.0f, scaleY);
                        inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 1.0f); // w=1 -> биомный ковёр
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

                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                        if (wy < -32.0f || wy > 23.0f) continue;

                        QVector3D norm = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.3f);
                        if (norm.y() < 0.34f) continue;

                        if (hk < 0.56f && norm.y() > 0.48f) {
                            // Ветвистый коралл (розовый, кораллово-красный, золотистый, фиолетовый)
                            QVector3D tint = (hc < 0.30f) ? QVector3D(0.98f, 0.42f, 0.58f)
                                             : (hc < 0.56f) ? QVector3D(0.98f, 0.72f, 0.28f)
                                             : (hc < 0.80f) ? QVector3D(0.76f, 0.40f, 0.96f)
                                                            : QVector3D(0.96f, 0.30f, 0.34f);
                            float yaw = 0.0f, pitch = 0.0f;
                            alignUpAxisToDir((norm * 0.55f + QVector3D(0.0f, 0.45f, 0.0f)).normalized(), yaw, pitch);
                            float sc = 0.75f + hs * 0.88f;

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(wx, wy - 0.14f, wz, sc);
                            inst.rotScaleY  = QVector4D(yaw, pitch, 0.0f, sc * (0.85f + hk * 0.35f));
                            inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                            chunk.propInstances[PROP_CORAL_BUSH].push_back(inst);
                        } else {
                            // Пластинчатый / столовый коралл (отлично сидит на скальных уступах и склонах)
                            QVector3D tint = (hc < 0.35f) ? QVector3D(0.92f, 0.54f, 0.34f)
                                             : (hc < 0.70f) ? QVector3D(0.78f, 0.44f, 0.88f)
                                                            : QVector3D(0.36f, 0.84f, 0.58f);
                            float sc = 0.80f + hs * 0.90f;
                            QVector3D pos(wx + norm.x() * 0.20f, wy - 0.15f, wz + norm.z() * 0.20f);

                            PropGpuInstance inst;
                            inst.posScaleXZ = QVector4D(pos.x(), pos.y(), pos.z(), sc);
                            inst.rotScaleY  = QVector4D(hc * TWO_PI, (hs - 0.5f) * 0.18f, (hk - 0.5f) * 0.18f, sc * 0.92f);
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
                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);

                        // Во впадинах растут светящиеся абиссальные перья
                        if (wy <= -30.0f && wy >= -72.0f) {
                            QVector3D n = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.4f);
                            if (n.y() > 0.45f) {
                                uint32_t dSeed = seed + static_cast<uint32_t>(ci * 173 + sp * 31);
                                appendAbyssalSeaPen(tmp.detailVerts, tmp.detailInds, QVector3D(wx, wy, wz), 0.85f + ht * 0.75f, dSeed);
                            }
                            continue;
                        }

                        if (wy > 24.0f) continue;

                        QVector3D cNorm = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.2f);
                        if (cNorm.y() < 0.68f) continue;

                        // Защита от острых пиков и обрывов
                        constexpr float checkR = 1.15f;
                        float hL = ProcMath::sampleTerrainHeight(wx - checkR, wz, seed);
                        float hR = ProcMath::sampleTerrainHeight(wx + checkR, wz, seed);
                        float hD = ProcMath::sampleTerrainHeight(wx, wz - checkR, seed);
                        float hU = ProcMath::sampleTerrainHeight(wx, wz + checkR, seed);
                        float avgH = 0.25f * (hL + hR + hD + hU);
                        if ((wy - avgH) > 0.30f) continue;

                        // Палитра как на референсах: терракота, тёплая охра/жёлтый, лавандово-розовый
                        QVector3D tint = (hc < 0.36f) ? QVector3D(0.92f, 0.56f, 0.42f)
                                         : (hc < 0.68f) ? QVector3D(0.94f, 0.82f, 0.46f)
                                                        : QVector3D(0.86f, 0.56f, 0.88f);

                        // Кластер из 2–3 губок разного размера, слегка расходящихся веером
                        int groupSize = (ht > 0.45f) ? 3 : 2;
                        for (int k = 0; k < groupSize; ++k) {
                            float kh1 = ProcMath::hash3(ci, sp * 11 + k, 1, seed + 521u);
                            float kh2 = ProcMath::hash3(ci, sp * 11 + k, 2, seed + 521u);

                            float ang = (TWO_PI * k) / groupSize + ht * TWO_PI;
                            float offR = (k == 0) ? 0.0f : (0.65f + kh1 * 0.60f);
                            float sx = wx + std::cos(ang) * offR;
                            float sz = wz + std::sin(ang) * offR;
                            float sy = ProcMath::sampleTerrainHeight(sx, sz, seed);
                            if (sy > ProcMath::WATER_SURFACE_Y - 1.5f) continue;

                            float scXZ = (k == 0) ? (0.72f + ht * 0.68f) : (0.44f + kh2 * 0.45f);
                            float scY  = scXZ * (0.78f + kh1 * 0.68f);

                            float yaw = 0.0f, pitch = 0.0f;
                            QVector3D leanDir = (cNorm + QVector3D(std::cos(ang) * 0.22f, 0.25f, std::sin(ang) * 0.22f)).normalized();
                            alignUpAxisToDir(leanDir, yaw, pitch);

                            float sink = 0.22f + (1.0f - cNorm.y()) * 0.85f;
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
                        float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                        if (wy < -31.0f || wy > 23.0f) continue;

                        QVector3D norm = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.3f);
                        if (norm.y() < 0.58f) continue;

                        QVector3D tint = (hc < 0.42f) ? QVector3D(0.32f, 0.86f, 0.44f)
                                         : (hc < 0.72f) ? QVector3D(0.84f, 0.72f, 0.24f)
                                                        : QVector3D(0.92f, 0.42f, 0.76f);
                        float scXZ = 0.75f + hs * 0.85f;
                        float scY  = 0.75f + hc * 0.85f;

                        PropGpuInstance inst;
                        inst.posScaleXZ = QVector4D(wx, wy - 0.14f, wz, scXZ);
                        inst.rotScaleY  = QVector4D(hc * TWO_PI, (hs - 0.5f) * 0.20f, 0.0f, scY);
                        inst.tintFlags  = QVector4D(tint.x(), tint.y(), tint.z(), 0.0f);
                        chunk.propInstances[PROP_ANEMONE_PEN].push_back(inst);
                    }

                    // Кластерные островки сочной зелени на солнечных неглубоких плато (y: +2 .. +26.2 м)
                    if (chunk.maxBounds.y() > 31.0f) {
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
                                float wy = ProcMath::sampleTerrainHeight(wx, wz, seed);
                                if (wy < 1.0f || wy > ProcMath::WATER_SURFACE_Y - 1.15f) continue;

                                QVector3D n = ProcMath::sampleTerrainNormal(wx, wz, seed, 1.2f);
                                if (n.y() < 0.50f) continue;

                                float clearance = (ProcMath::WATER_SURFACE_Y - 0.90f) - wy;
                                float scXZ = (0.78f + th3 * 0.68f) * (1.12f - 0.30f * (rad / islandR));
                                float scY  = std::min(scXZ * 1.05f, clearance / 1.65f);
                                if (scY < 0.25f) continue;

                                QVector3D lushTint = (th3 < 0.55f)
                                                         ? QVector3D(0.36f, 0.94f, 0.34f)
                                                         : QVector3D(0.60f, 0.92f, 0.26f);

                                PropGpuInstance inst;
                                inst.posScaleXZ = QVector4D(wx, wy - 0.12f, wz, scXZ);
                                inst.rotScaleY  = QVector4D(th1 * TWO_PI, (th2 - 0.5f) * 0.16f, 0.0f, scY);
                                inst.tintFlags  = QVector4D(lushTint.x(), lushTint.y(), lushTint.z(), 0.0f);
                                chunk.propInstances[PROP_ANEMONE_PEN].push_back(inst);
                            }
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
