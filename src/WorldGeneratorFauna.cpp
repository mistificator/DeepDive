#include "WorldGenerator.h"
#include "Noise.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

// Гарантированный обход против часовой стрелки (CCW) по направлению вершинных нормалей
inline void emitTriCCW(const std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                       uint32_t i0, uint32_t i1, uint32_t i2) {
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
}

void appendFaunaCone(std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                     const QVector3D& baseCenter, const QVector3D& tipPos,
                     float baseRadius, int sides,
                     const QVector3D& baseCol, const QVector3D& tipCol) {
    QVector3D axis = tipPos - baseCenter;
    float len = axis.length();
    if (len < 1e-4f || sides < 3) return;
    QVector3D dir = axis / len;

    QVector3D u = QVector3D::crossProduct(dir, QVector3D(0.0f, 1.0f, 0.0f));
    if (u.lengthSquared() < 1e-4f) u = QVector3D::crossProduct(dir, QVector3D(1.0f, 0.0f, 0.0f));
    u.normalize();
    QVector3D v = QVector3D::crossProduct(dir, u).normalized();

    uint32_t ringStart = static_cast<uint32_t>(verts.size());
    for (int s = 0; s < sides; ++s) {
        float ang = (TWO_PI * s) / sides;
        QVector3D rad = u * std::cos(ang) + v * std::sin(ang);
        QVector3D n = (rad * len + dir * baseRadius).normalized();
        verts.push_back({baseCenter + rad * baseRadius, n, baseCol});
    }
    uint32_t tipIdx = static_cast<uint32_t>(verts.size());
    verts.push_back({tipPos, dir, tipCol});

    for (int s = 0; s < sides; ++s) {
        int sNext = (s + 1) % sides;
        emitTriCCW(verts, inds, ringStart + s, ringStart + sNext, tipIdx);
    }
}

void appendFaunaSphere(std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                       const QVector3D& center, const QVector3D& radii,
                       int stacks, int slices, const QVector3D& color) {
    if (stacks < 2 || slices < 3) return;
    const uint32_t baseIdx = static_cast<uint32_t>(verts.size());
    const float invRx = 1.0f / std::max(radii.x(), 1e-3f);
    const float invRy = 1.0f / std::max(radii.y(), 1e-3f);
    const float invRz = 1.0f / std::max(radii.z(), 1e-3f);

    for (int st = 0; st <= stacks; ++st) {
        float phi = PI * static_cast<float>(st) / static_cast<float>(stacks);
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int sl = 0; sl <= slices; ++sl) {
            int slWrap = sl % slices;
            float theta = TWO_PI * static_cast<float>(slWrap) / static_cast<float>(slices);
            QVector3D dir(sp * std::cos(theta), cp, sp * std::sin(theta));
            QVector3D p = center + QVector3D(dir.x() * radii.x(), dir.y() * radii.y(), dir.z() * radii.z());
            QVector3D n(dir.x() * invRx, dir.y() * invRy, dir.z() * invRz);
            if (n.lengthSquared() < 1e-6f) n = QVector3D(0.0f, 1.0f, 0.0f);
            n.normalize();
            verts.push_back({p, n, color});
        }
    }
    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            uint32_t i00 = baseIdx + st * (slices + 1) + sl;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + (slices + 1);
            uint32_t i11 = i01 + 1;
            if (st > 0)          emitTriCCW(verts, inds, i00, i01, i10);
            if (st + 1 < stacks) emitTriCCW(verts, inds, i10, i01, i11);
        }
    }
}

void appendFaunaTube(std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                     const std::vector<QVector3D>& spine, const std::vector<float>& radii,
                     int radialSegs, const QVector3D& colA, const QVector3D& colB,
                     bool verticalColorSplit = false, bool closeTip = false) {
    const int nPts = static_cast<int>(spine.size());
    if (nPts < 2 || static_cast<int>(radii.size()) < nPts || radialSegs < 3) return;
    const uint32_t baseIdx = static_cast<uint32_t>(verts.size());

    QVector3D prevU;
    bool hasPrevFrame = false;

    for (int i = 0; i < nPts; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(nPts - 1);
        QVector3D tang;
        if (i == 0) tang = spine[1] - spine[0];
        else if (i + 1 == nPts) tang = spine[i] - spine[i - 1];
        else tang = spine[i + 1] - spine[i - 1];
        if (tang.lengthSquared() < 1e-8f) tang = QVector3D(0.0f, 0.0f, 1.0f);
        tang.normalize();

        // Параллельный перенос локального базиса вдоль кривой (без перекручивания сечений)
        QVector3D u;
        if (hasPrevFrame) {
            u = prevU - tang * QVector3D::dotProduct(prevU, tang);
        }
        if (!hasPrevFrame || u.lengthSquared() < 1e-4f) {
            u = QVector3D::crossProduct(tang, QVector3D(0.0f, 1.0f, 0.0f));
            if (u.lengthSquared() < 1e-4f) {
                u = QVector3D::crossProduct(tang, QVector3D(1.0f, 0.0f, 0.0f));
            }
        }
        u.normalize();
        prevU = u;
        hasPrevFrame = true;
        QVector3D v = QVector3D::crossProduct(tang, u).normalized();

        float r = radii[i];
        QVector3D lengthCol = ProcMath::mix(colA, colB, t);
        for (int j = 0; j < radialSegs; ++j) {
            float a = (TWO_PI * j) / radialSegs;
            QVector3D n = (u * std::cos(a) + v * std::sin(a)).normalized();
            QVector3D col = verticalColorSplit
                                ? ProcMath::mix(colB, colA, ProcMath::smoothstep(-0.35f, 0.35f, n.y()))
                                : lengthCol;
            verts.push_back({spine[i] + n * r, n, col});
        }
    }

    for (int i = 0; i + 1 < nPts; ++i) {
        uint32_t r0 = baseIdx + i * radialSegs;
        uint32_t r1 = baseIdx + (i + 1) * radialSegs;
        for (int j = 0; j < radialSegs; ++j) {
            int jNext = (j + 1) % radialSegs;
            emitTriCCW(verts, inds, r0 + j, r1 + j, r0 + jNext);
            emitTriCCW(verts, inds, r0 + jNext, r1 + j, r1 + jNext);
        }
    }

    if (closeTip) {
        QVector3D tipDir = (spine.back() - spine[nPts - 2]);
        if (tipDir.lengthSquared() < 1e-8f) tipDir = QVector3D(0.0f, 0.0f, -1.0f);
        tipDir.normalize();
        uint32_t tipIdx = static_cast<uint32_t>(verts.size());
        QVector3D tipCol = verticalColorSplit ? colA : colB;
        verts.push_back({spine.back() + tipDir * (radii.back() * 2.2f), tipDir, tipCol});
        uint32_t lastR = baseIdx + (nPts - 1) * radialSegs;
        for (int j = 0; j < radialSegs; ++j) {
            int jNext = (j + 1) % radialSegs;
            emitTriCCW(verts, inds, lastR + j, tipIdx, lastR + jNext);
        }
    }
}
} // namespace

// ============================================================================
// ПРОЦЕДУРНЫЕ 3D-МОДЕЛИ ФАУНЫ (РЫБЫ, СКАТЫ-МАНТЫ, ЧАЙКИ, ГОРБАТЫЙ КИТ, УДИЛЬЩИК)
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
                int jWrap = j % segs;
                float a = (TWO_PI * jWrap) / segs;
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
                if (i > 0)           emitTriCCW(verts, inds, i00, i01, i10);
                if (i + 1 < rings)   emitTriCCW(verts, inds, i10, i01, i11);
            }
        }

        // Двусторонний хвостовой плавник с основанием внутри хвостового стебля (тело доходит до z = -0.75)
        const QVector3D tailCol(0.98f, 0.44f, 0.16f);
        for (float side : {1.0f, -1.0f}) {
            const float xOff = side * 0.004f;
            const QVector3D fn(side, 0.0f, 0.0f);
            uint32_t tb = static_cast<uint32_t>(verts.size());
            verts.push_back({QVector3D(xOff,  0.05f, -0.64f), fn, tailCol});
            verts.push_back({QVector3D(xOff, -0.05f, -0.64f), fn, tailCol});
            verts.push_back({QVector3D(xOff,  0.34f, -1.08f), fn, tailCol});
            verts.push_back({QVector3D(xOff, -0.34f, -1.08f), fn, tailCol});
            verts.push_back({QVector3D(xOff,  0.00f, -0.90f), fn, tailCol});
            emitTriCCW(verts, inds, tb + 0, tb + 2, tb + 4);
            emitTriCCW(verts, inds, tb + 0, tb + 4, tb + 1);
            emitTriCCW(verts, inds, tb + 1, tb + 4, tb + 3);
        }

        if (withFins) {
            for (float side : {1.0f, -1.0f}) {
                const float xOff = side * 0.004f;
                const QVector3D fn(side, 0.0f, 0.0f);
                uint32_t db = static_cast<uint32_t>(verts.size());
                verts.push_back({QVector3D(xOff, 0.24f,  0.15f), fn, tailCol});
                verts.push_back({QVector3D(xOff, 0.48f, -0.18f), fn, tailCol});
                verts.push_back({QVector3D(xOff, 0.16f, -0.38f), fn, tailCol});
                emitTriCCW(verts, inds, db, db + 1, db + 2);
            }
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
                        float wingFade = ProcMath::smoothstep(0.15f, 0.95f, absU);
                        col = ProcMath::mix(dorsalMid, dorsalDark, wingFade * 0.75f + v * 0.25f);

                        float chevU = (absU - 0.25f) / 0.16f;
                        float chevV = (v - (0.20f + absU * 0.35f)) / 0.13f;
                        float chevron = std::exp(-(chevU * chevU + chevV * chevV));
                        chevron *= ProcMath::smoothstep(0.05f, 0.14f, absU)
                                   * (1.0f - ProcMath::smoothstep(0.42f, 0.58f, absU));
                        col = ProcMath::mix(col, chevronCol, chevron * 0.72f);
                    } else {
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
                    QVector3D n;
                    if (iz == 0) {
                        n = QVector3D(-tX.z(), 0.0f, tX.x());
                    } else if (iz == chordSegs) {
                        n = QVector3D(tX.z(), 0.0f, -tX.x());
                    } else {
                        n = QVector3D::crossProduct(tX, tZ) * ySign;
                    }
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
                    emitTriCCW(verts, inds, i00, i10, i01);
                    emitTriCCW(verts, inds, i10, i11, i01);
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
            appendFaunaTube(verts, inds, lobeSpine, lobeRadii, highLod ? 6 : 4, dorsalDark, bellyBorder, true, true);
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

            emitTriCCW(verts, inds, base + 0, base + 3, base + 1);
            emitTriCCW(verts, inds, base + 3, base + 2, base + 1);
            emitTriCCW(verts, inds, base + 0, base + 1, base + 4);
            emitTriCCW(verts, inds, base + 4, base + 1, base + 2);
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
            appendFaunaTube(verts, inds, tailSpine, tailRadii, highLod ? 6 : 4, dorsalDark, bellyBorder, true, true);
        }

        // 5. Глаза у основания головных плавников (на LOD0)
        if (highLod) {
            for (float side : {-1.0f, 1.0f}) {
                appendFaunaSphere(verts, inds,
                                  QVector3D(side * 0.41f, 0.025f, 0.66f),
                                  QVector3D(0.032f, 0.032f, 0.032f),
                                  4, 6, eyeCol);
            }
        }
    };

    buildManta(data.mantaLod0Verts, data.mantaLod0Inds, 24, 12, true);
    buildManta(data.mantaLod1Verts, data.mantaLod1Inds, 10, 5, false);
}

void WorldGenerator::generateSeagullMeshes(WorldData& data) {
    auto buildSeagull = [](std::vector<FaunaVertex>& verts, std::vector<uint32_t>& inds,
                           int spanSegs, int chordSegs, int bodyRings, int bodySegs, bool highLod) {
        const QVector3D whitePlumage(0.95f, 0.96f, 0.98f);
        const QVector3D gullGrey(0.66f, 0.72f, 0.78f);
        const QVector3D darkWingTip(0.09f, 0.11f, 0.14f);
        const QVector3D underTipDark(0.16f, 0.18f, 0.22f);
        const QVector3D beakYellow(0.96f, 0.76f, 0.16f);
        const QVector3D beakTipCol(0.94f, 0.36f, 0.12f);
        const QVector3D eyeCol(0.02f, 0.025f, 0.04f);

        // 1. Обтекаемый корпус, шея и голова (от лба z = +0.74 до основания хвоста z = -0.78)
        {
            const uint32_t bodyBase = static_cast<uint32_t>(verts.size());
            std::vector<QVector3D> gridPos((bodyRings + 1) * bodySegs);
            std::vector<QVector3D> gridCol((bodyRings + 1) * bodySegs);

            for (int r = 0; r <= bodyRings; ++r) {
                float t = static_cast<float>(r) / static_cast<float>(bodyRings);
                float z = 0.74f - t * 1.52f;
                float env = std::max(0.0f, std::sin(std::pow(t, 0.78f) * PI));

                float neckDip = 1.0f - 0.24f * std::exp(-std::pow((t - 0.16f) / 0.08f, 2.0f));
                float headBump = 1.0f + 0.22f * std::exp(-std::pow((t - 0.07f) / 0.06f, 2.0f));
                float rx = 0.19f * std::pow(env, 0.72f) * neckDip * headBump;
                float ry = 0.17f * std::pow(env, 0.68f) * neckDip * headBump;
                float cy = 0.12f * (1.0f - ProcMath::smoothstep(0.02f, 0.34f, t))
                           - 0.03f * std::sin(t * PI);

                for (int j = 0; j < bodySegs; ++j) {
                    float a = (TWO_PI * j) / bodySegs;
                    float ca = std::cos(a), sa = std::sin(a);
                    gridPos[r * bodySegs + j] = QVector3D(ca * rx, cy + sa * ry, z);

                    float mantleMask = ProcMath::smoothstep(0.20f, 0.34f, t)
                                       * (1.0f - ProcMath::smoothstep(0.76f, 0.90f, t))
                                       * ProcMath::smoothstep(0.15f, 0.55f, sa);
                    gridCol[r * bodySegs + j] = ProcMath::mix(whitePlumage, gullGrey, mantleMask);
                }
            }

            const QVector3D headFront(0.0f, 0.12f, 0.76f);
            const QVector3D tailRoot (0.0f, -0.01f, -0.80f);

            for (int r = 0; r <= bodyRings; ++r) {
                for (int j = 0; j < bodySegs; ++j) {
                    const QVector3D& pPrev = (r > 0) ? gridPos[(r - 1) * bodySegs + j] : headFront;
                    const QVector3D& pNext = (r < bodyRings) ? gridPos[(r + 1) * bodySegs + j] : tailRoot;
                    int jPrev = (j - 1 + bodySegs) % bodySegs;
                    int jNext = (j + 1) % bodySegs;
                    QVector3D n = QVector3D::crossProduct(pNext - pPrev,
                                                          gridPos[r * bodySegs + jNext] - gridPos[r * bodySegs + jPrev]);
                    if (n.lengthSquared() < 1e-6f) n = QVector3D(0.0f, 1.0f, 0.0f);
                    n.normalize();
                    verts.push_back({gridPos[r * bodySegs + j], n, gridCol[r * bodySegs + j]});
                }
            }

            uint32_t frontIdx = static_cast<uint32_t>(verts.size());
            verts.push_back({headFront, QVector3D(0.0f, 0.15f, 1.0f).normalized(), whitePlumage});
            uint32_t backIdx = static_cast<uint32_t>(verts.size());
            verts.push_back({tailRoot, QVector3D(0.0f, 0.0f, -1.0f), whitePlumage});

            for (int j = 0; j < bodySegs; ++j) {
                int jNext = (j + 1) % bodySegs;
                emitTriCCW(verts, inds, frontIdx, bodyBase + j, bodyBase + jNext);
                uint32_t lastR = bodyBase + bodyRings * bodySegs;
                emitTriCCW(verts, inds, backIdx, lastR + jNext, lastR + j);
            }
            for (int r = 0; r < bodyRings; ++r) {
                uint32_t r0 = bodyBase + r * bodySegs;
                uint32_t r1 = bodyBase + (r + 1) * bodySegs;
                for (int j = 0; j < bodySegs; ++j) {
                    int jNext = (j + 1) % bodySegs;
                    emitTriCCW(verts, inds, r0 + j, r1 + j, r0 + jNext);
                    emitTriCCW(verts, inds, r0 + jNext, r1 + j, r1 + jNext);
                }
            }
        }

        // 2. Жёлтый клюв с лёгким оранжевым кончиком
        appendFaunaCone(verts, inds,
                        QVector3D(0.0f, 0.105f, 0.70f),
                        QVector3D(0.0f, 0.075f, 1.00f),
                        0.052f, highLod ? 6 : 4, beakYellow, beakTipCol);

        // 3. Глаза на LOD0
        if (highLod) {
            for (float side : {-1.0f, 1.0f}) {
                appendFaunaSphere(verts, inds,
                                  QVector3D(side * 0.088f, 0.155f, 0.59f),
                                  QVector3D(0.024f, 0.024f, 0.024f),
                                  4, 6, eyeCol);
            }
        }

        // 4. Веерный белый хвост (двусторонняя замкнутая пластина)
        {
            auto addTailSurface = [&](float ySign) {
                uint32_t tb = static_cast<uint32_t>(verts.size());
                const int wSegs = highLod ? 4 : 2;
                for (int ix = 0; ix <= wSegs; ++ix) {
                    float u = (static_cast<float>(ix) / wSegs) * 2.0f - 1.0f;
                    QVector3D pRoot(u * 0.08f,  0.01f + ySign * 0.018f, -0.66f);
                    QVector3D pEnd (u * 0.29f, -0.02f + ySign * 0.008f, -1.16f + std::abs(u) * 0.06f);
                    QVector3D n(0.0f, ySign, 0.0f);
                    verts.push_back({pRoot, n, whitePlumage});
                    verts.push_back({pEnd,  n, whitePlumage});
                }
                for (int ix = 0; ix < wSegs; ++ix) {
                    uint32_t i0 = tb + ix * 2;
                    emitTriCCW(verts, inds, i0, i0 + 2, i0 + 1);
                    emitTriCCW(verts, inds, i0 + 2, i0 + 3, i0 + 1);
                }
            };
            addTailSurface( 1.0f);
            addTailSurface(-1.0f);
        }

        // 5. Изящные крылья чайки с характерным изгибом в запястье и чёрными кончиками
        auto addWingSurface = [&](float ySign) {
            const int cols = chordSegs + 1;
            const int rows = spanSegs + 1;
            std::vector<QVector3D> gridPos(rows * cols);
            std::vector<QVector3D> gridCol(rows * cols);

            for (int ix = 0; ix <= spanSegs; ++ix) {
                float u = (static_cast<float>(ix) / spanSegs) * 2.0f - 1.0f;
                float absU = std::abs(u);
                float x = u * 1.62f;

                float wristArc = std::sin(ProcMath::clamp(absU / 0.45f, 0.0f, 1.0f) * PI * 0.5f);
                float outerT   = ProcMath::smoothstep(0.42f, 1.0f, absU);

                float leadZ  = 0.24f + 0.14f * wristArc - 0.54f * std::pow(outerT, 1.25f);
                float trailZ = -0.34f + 0.06f * wristArc + 0.04f * outerT;
                float chordLen = std::max(0.04f, leadZ - trailZ);

                float gullArchY = 0.05f + 0.13f * std::sin(ProcMath::clamp(absU / 0.52f, 0.0f, 1.0f) * PI * 0.5f)
                                  - 0.08f * outerT;
                float thick = (0.048f * (1.0f - absU * 0.85f) + 0.008f) * ProcMath::clamp(chordLen / 0.45f, 0.35f, 1.2f);

                for (int iz = 0; iz <= chordSegs; ++iz) {
                    float v = static_cast<float>(iz) / chordSegs;
                    float z = ProcMath::mix(leadZ, trailZ, v);
                    float airfoil = std::sin(std::pow(ProcMath::clamp(v, 0.0f, 1.0f), 0.68f) * PI);
                    float y = gullArchY + ySign * thick * airfoil * (ySign > 0.0f ? 1.0f : 0.45f);

                    QVector3D col;
                    float tipBlack = ProcMath::smoothstep(0.71f, 0.84f, absU + (1.0f - v) * 0.08f);
                    if (ySign > 0.0f) {
                        float whiteEdge = ProcMath::smoothstep(0.78f, 0.96f, v) * (1.0f - tipBlack);
                        col = ProcMath::mix(gullGrey, whitePlumage, whiteEdge * 0.85f);
                        col = ProcMath::mix(col, darkWingTip, tipBlack);
                        float mirrorSpot = std::exp(-std::pow((absU - 0.91f) / 0.035f, 2.0f)
                                                    - std::pow((v - 0.52f) / 0.18f, 2.0f));
                        col = ProcMath::mix(col, whitePlumage, mirrorSpot * 0.72f);
                    } else {
                        col = ProcMath::mix(whitePlumage, underTipDark, tipBlack * 0.92f);
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
                    QVector3D n;
                    if (iz == 0) {
                        n = QVector3D(-tX.z(), 0.0f, tX.x());
                    } else if (iz == chordSegs) {
                        n = QVector3D(tX.z(), 0.0f, -tX.x());
                    } else {
                        n = QVector3D::crossProduct(tX, tZ) * ySign;
                    }
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
                    emitTriCCW(verts, inds, i00, i10, i01);
                    emitTriCCW(verts, inds, i10, i11, i01);
                }
            }
        };

        addWingSurface( 1.0f);
        addWingSurface(-1.0f);
    };

    buildSeagull(data.seagullLod0Verts, data.seagullLod0Inds, 18, 6, 10, 10, true);
    buildSeagull(data.seagullLod1Verts, data.seagullLod1Inds,  8, 3,  5,  6, false);
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
            emitTriCCW(verts, inds, noseIdx, bodyBase + j, bodyBase + jNext);
            uint32_t lastRing = bodyBase + (numRings - 1) * radialSegs;
            emitTriCCW(verts, inds, tailIdx, lastRing + jNext, lastRing + j);
        }
        for (int r = 0; r + 1 < numRings; ++r) {
            uint32_t r0 = bodyBase + r * radialSegs;
            uint32_t r1 = bodyBase + (r + 1) * radialSegs;
            for (int j = 0; j < radialSegs; ++j) {
                int jNext = (j + 1) % radialSegs;
                emitTriCCW(verts, inds, r0 + j, r1 + j, r0 + jNext);
                emitTriCCW(verts, inds, r0 + jNext, r1 + j, r1 + jNext);
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

            appendFaunaSphere(verts, inds, skinP - outDir * 0.025f, QVector3D(0.115f, 0.100f, 0.165f),
                              highLod ? 6 : 4, highLod ? 8 : 5, backCol * 0.72f);
            appendFaunaSphere(verts, inds, skinP + outDir * 0.028f, QVector3D(0.082f, 0.074f, 0.094f),
                              highLod ? 7 : 4, highLod ? 9 : 6, eyeSclera);
            appendFaunaSphere(verts, inds, skinP + outDir * 0.076f, QVector3D(0.052f, 0.050f, 0.056f),
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
                    emitTriCCW(verts, inds, r0 + j, r1 + j, r0 + jNext);
                    emitTriCCW(verts, inds, r0 + jNext, r1 + j, r1 + jNext);
                }
            }
            uint32_t lastR = fBase + (nSec - 1) * ringPts;
            for (int j = 0; j < ringPts; ++j) {
                int jNext = (j + 1) % ringPts;
                emitTriCCW(verts, inds, lastR + j, tipIdx, lastR + jNext);
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
        const QVector3D skinDark(0.11f, 0.09f, 0.12f);
        const QVector3D skinMottled(0.20f, 0.15f, 0.16f);
        const QVector3D mouthDark(0.06f, 0.02f, 0.04f);
        const QVector3D toothBase(0.76f, 0.72f, 0.66f);
        const QVector3D toothTip(0.98f, 0.96f, 0.90f);
        const QVector3D finCol(0.16f, 0.13f, 0.18f);
        const QVector3D lureGlow(0.85f, 2.45f, 2.25f);
        const QVector3D eyeGlow(1.38f, 1.56f, 0.72f);

        const uint32_t bodyBase = static_cast<uint32_t>(verts.size());
        for (int i = 0; i <= bodySegs; ++i) {
            float t = static_cast<float>(i) / bodySegs;
            float z = 0.72f - t * 2.08f;

            float env = std::max(0.0f, std::sin(std::pow(t, 0.52f) * PI));
            float rx = 0.08f + 0.70f * std::pow(env, 0.85f) * (1.0f - 0.35f * t);
            float ryUp = 0.08f + 0.56f * std::pow(env, 0.80f);
            float ryDown = 0.08f + 0.74f * std::pow(env, 0.75f) * (1.0f - 0.28f * t);

            for (int j = 0; j <= radialSegs; ++j) {
                int jWrap = j % radialSegs;
                float a = (TWO_PI * jWrap) / radialSegs;
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
                uint32_t i00 = bodyBase + i * (radialSegs + 1) + j;
                uint32_t i10 = i00 + 1;
                uint32_t i01 = i00 + (radialSegs + 1);
                uint32_t i11 = i01 + 1;
                emitTriCCW(verts, inds, i00, i01, i10);
                emitTriCCW(verts, inds, i10, i01, i11);
            }
        }

        // Заглушки переднего (внутри пасти) и заднего торцов тела
        uint32_t frontCapIdx = static_cast<uint32_t>(verts.size());
        verts.push_back({QVector3D(0.0f, -0.02f, 0.42f), QVector3D(0.0f, 0.0f, 1.0f), mouthDark});
        uint32_t backCapIdx = static_cast<uint32_t>(verts.size());
        verts.push_back({QVector3D(0.0f, 0.0f, -1.42f), QVector3D(0.0f, 0.0f, -1.0f), skinDark});
        const uint32_t lastRingBase = bodyBase + bodySegs * (radialSegs + 1);
        for (int j = 0; j < radialSegs; ++j) {
            emitTriCCW(verts, inds, frontCapIdx, bodyBase + j, bodyBase + j + 1);
            emitTriCCW(verts, inds, backCapIdx, lastRingBase + j + 1, lastRingBase + j);
        }

        // Нижняя челюсть
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

        // Зубы нижней и верхней челюстей
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

        // Светящиеся глаза
        const QVector3D pupilCol(0.02f, 0.02f, 0.03f);
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

        // Иллиций (удочка) и светящаяся эска-фонарик
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

        // Двусторонние плавники с лёгкой толщиной и корректными нормалями для обеих сторон
        auto addFinQuad = [&](const QVector3D& p0, const QVector3D& p1, const QVector3D& p2, const QVector3D& p3,
                              const QVector3D& refNormal, const QVector3D& col) {
            QVector3D faceN = QVector3D::crossProduct(p1 - p0, p2 - p0)
            + QVector3D::crossProduct(p2 - p0, p3 - p0);
            if (QVector3D::dotProduct(faceN, refNormal) < 0.0f) faceN = -faceN;
            const QVector3D n = (faceN.lengthSquared() > 1e-6f) ? faceN.normalized() : refNormal.normalized();

            for (float side : {1.0f, -1.0f}) {
                const QVector3D sn = n * side;
                const QVector3D off = sn * 0.006f;
                uint32_t b = static_cast<uint32_t>(verts.size());
                verts.push_back({p0 + off, sn, col});
                verts.push_back({p1 + off, sn, col});
                verts.push_back({p2 + off, sn, col});
                verts.push_back({p3 + off, sn, col});
                emitTriCCW(verts, inds, b, b + 1, b + 2);
                emitTriCCW(verts, inds, b, b + 2, b + 3);
            }
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
// ЗАСЕЛЕНИЕ ФАУНОЙ (РЫБЫ, СКАТЫ-МАНТЫ, ГЛУБОКОВОДНЫЕ УДИЛЬЩИКИ, ПАРЯЩИЕ ЧАЙКИ)
// ============================================================================

void WorldGenerator::generateFaunaAgents(WorldData& data, uint32_t seed) {
    const QVector3D schoolTints[] = {
        {1.12f, 0.96f, 0.80f},
        {0.56f, 1.14f, 1.24f},
        {1.24f, 0.56f, 0.74f},
        {0.76f, 1.18f, 0.66f},
        {1.20f, 0.92f, 0.36f}
    };

    // 1. Стаи рифовых рыб
    constexpr int SCHOOL_COUNT    = 34;
    constexpr int FISH_PER_SCHOOL = 26;
    data.fishAgents.clear();
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
        float floorY = data.collision.sampleHeight(sx, sz);
        float safeFloor = std::max(floorY, -24.0f);
        float baseY = ProcMath::clamp(safeFloor + 7.0f + ProcMath::hash3(sc, 5, 6, seed) * 10.5f, -18.0f, 16.5f);
        QVector3D schoolCenter(sx, baseY, sz);
        data.collision.resolveSphereCollision(schoolCenter, nullptr, 4.0f, 3.0f, 4.0f);
        sx = schoolCenter.x();
        sz = schoolCenter.z();
        baseY = schoolCenter.y();
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
            agent.preferredDepth = ProcMath::clamp(std::max(baseY, data.collision.sampleHeight(pos.x(), pos.z()) + 3.5f),
                                                   -18.0f, 16.5f);
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

    // 2. Парящие скаты-манты
    constexpr int MANTA_COUNT = 32;
    data.mantaAgents.clear();
    data.mantaAgents.reserve(MANTA_COUNT);

    for (int m = 0; m < MANTA_COUNT; ++m) {
        float h1 = ProcMath::hash3(m, 1, 7, seed + 907u);
        float h2 = ProcMath::hash3(m, 2, 8, seed + 907u);
        float h3 = ProcMath::hash3(m, 3, 9, seed + 907u);

        float a = (TWO_PI * m) / static_cast<float>(MANTA_COUNT) + h1 * 0.35f;
        float dist = (m < 4) ? (30.0f + m * 14.0f) : (70.0f + h2 * 375.0f);
        float mx = std::cos(a) * dist;
        float mz = std::sin(a) * dist;
        float floorY = data.collision.sampleHeight(mx, mz);
        float my = ProcMath::clamp(std::max(floorY + 10.0f, -2.0f + h3 * 16.0f), -10.0f, 20.5f);

        QVector3D pos(mx, my, mz);
        data.collision.resolveSphereCollision(pos, nullptr, 3.6f, 2.5f, 4.0f);

        FaunaAgent manta;
        manta.pos = pos;
        manta.homeCenter = pos;
        manta.homeRadius = 55.0f + h1 * 50.0f;
        manta.preferredDepth = pos.y();
        manta.cruiseSpeed = 4.6f + h2 * 2.2f;
        manta.scale = 1.15f + h3 * 0.78f;
        manta.boundingRadius = 2.6f * manta.scale;
        manta.phase = h1 * 10.0f;
        manta.type = 1.0f;
        manta.vel = QVector3D(-std::sin(a), 0.0f, std::cos(a)) * manta.cruiseSpeed;
        manta.yaw = std::atan2(manta.vel.x(), manta.vel.z());
        data.mantaAgents.push_back(manta);
    }

    // 3. Глубоководные удильщики в абиссальных ямах
    const auto& pits = ProcMath::getAbyssalPits(seed);
    const int pitCount = std::min<int>(ProcMath::ABYSSAL_PIT_COUNT, static_cast<int>(pits.size()));
    data.anglerAgents.clear();
    data.anglerAgents.reserve(pitCount);

    for (int i = 0; i < pitCount; ++i) {
        const auto& pit = pits[i];
        float h1 = ProcMath::hash3(i, 11, 21, seed + 3313u);
        float h2 = ProcMath::hash3(i, 12, 22, seed + 3313u);

        float ang = h1 * TWO_PI;
        float ax = pit.x + std::cos(ang) * 6.0f;
        float az = pit.z + std::sin(ang) * 6.0f;
        float bottomY = data.collision.sampleHeight(ax, az);
        float ay = bottomY + 5.2f;

        QVector3D pos(ax, ay, az);
        data.collision.resolveSphereCollision(pos, nullptr, 3.0f, 2.6f, 6.0f);

        FaunaAgent angler;
        angler.pos = pos;
        angler.homeCenter = QVector3D(pit.x, std::max(bottomY + 5.5f, pos.y()), pit.z);
        angler.homeRadius = pit.radius * 0.34f;
        angler.preferredDepth = pos.y();
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

    // 4. Парящие в вышине чайки (часть — над центральной лагуной, остальные — вокруг кекуров и скал)
    constexpr int SEAGULL_COUNT = 34;
    data.seagullAgents.clear();
    data.seagullAgents.reserve(SEAGULL_COUNT);
    const auto& spires = ProcMath::getCentralSpires(seed);

    for (int g = 0; g < SEAGULL_COUNT; ++g) {
        float h1 = ProcMath::hash3(g, 1, 13, seed + 4457u);
        float h2 = ProcMath::hash3(g, 2, 17, seed + 4457u);
        float h3 = ProcMath::hash3(g, 3, 19, seed + 4457u);
        float h4 = ProcMath::hash3(g, 4, 23, seed + 4457u);

        float homeX = 0.0f, homeZ = 0.0f, orbitR = 55.0f;
        float baseAlt = ProcMath::WATER_SURFACE_Y + 18.0f + h3 * 34.0f;

        if (g < 10) {
            // Ближняя стая над центром лагуны — сразу видна при выныривании
            float ang = (TWO_PI * static_cast<float>(g)) / 10.0f + h1 * 0.45f;
            float rCenter = 24.0f + h2 * 85.0f;
            homeX = std::cos(ang) * rCenter;
            homeZ = 10.0f + std::sin(ang) * rCenter;
            orbitR = 42.0f + h4 * 48.0f;
            baseAlt = ProcMath::WATER_SURFACE_Y + 16.0f + h3 * 22.0f;
        } else if (g < 24 && !spires.empty()) {
            // Чайки, парящие в восходящих потоках у высоких скальных кекуров
            const auto& sp = spires[(g - 10) % spires.size()];
            homeX = sp.x;
            homeZ = sp.z;
            orbitR = sp.radius + 26.0f + h4 * 42.0f;
            baseAlt = ProcMath::WATER_SURFACE_Y + 24.0f + h3 * 30.0f;
        } else {
            // Дальние чайки по всей акватории кальдеры
            float ang = h1 * TWO_PI;
            float rCenter = 150.0f + h2 * 260.0f;
            homeX = std::cos(ang) * rCenter;
            homeZ = std::sin(ang) * rCenter;
            orbitR = 65.0f + h4 * 75.0f;
            baseAlt = ProcMath::WATER_SURFACE_Y + 22.0f + h3 * 36.0f;
        }

        float startAng = h2 * TWO_PI;
        float gx = homeX + std::cos(startAng) * (orbitR * 0.78f);
        float gz = homeZ + std::sin(startAng) * (orbitR * 0.78f);
        float groundY = data.collision.sampleHeight(gx, gz);
        float gy = std::max(baseAlt, groundY + 10.0f);

        float dirSign = (g % 2 == 0) ? 1.0f : -1.0f;
        QVector3D tangent(-std::sin(startAng) * dirSign, 0.0f, std::cos(startAng) * dirSign);

        FaunaAgent gull;
        gull.pos = QVector3D(gx, gy, gz);
        gull.homeCenter = QVector3D(homeX, baseAlt, homeZ);
        gull.homeRadius = orbitR;
        gull.preferredDepth = gy; // для чаек хранит целевую высоту парения
        gull.cruiseSpeed = 8.5f + h4 * 4.2f;
        gull.scale = 0.88f + h1 * 0.44f;
        gull.boundingRadius = 1.65f * gull.scale;
        gull.phase = h3 * 16.0f + static_cast<float>(g) * 0.73f;
        gull.type = 4.0f;
        gull.tint = QVector3D(0.96f + h2 * 0.08f, 0.96f + h3 * 0.07f, 0.95f + h4 * 0.08f);
        gull.vel = tangent * gull.cruiseSpeed;
        gull.yaw = std::atan2(gull.vel.x(), gull.vel.z());
        data.seagullAgents.push_back(gull);
    }
}
