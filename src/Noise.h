#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <array>
#include <QVector3D>

namespace ProcMath {

inline constexpr float WORLD_HALF_SIZE      = 800.0f; // Мир 1600 x 1600 м
inline constexpr float WATER_SURFACE_Y      = 28.0f;  // Уровень водной глади
inline constexpr float WATER_MAX_WAVE_AMP   = 0.65f;  // Максимальная амплитуда гребня волны
inline constexpr float BASIN_INNER_RADIUS   = 500.0f; // Начало подъема кольцевых утесов
inline constexpr float BASIN_RIM_RADIUS     = 670.0f; // Гребень замкнутых скал
inline constexpr float PLAYABLE_HARD_RADIUS = 720.0f; // Страховочный лимит за видимыми утесами
inline constexpr int   ABYSSAL_PIT_COUNT    = 5;
inline constexpr int   CENTRAL_SPIRE_COUNT  = 22;

struct AbyssalPit {
    float x = 0.0f;
    float z = 0.0f;
    float radius = 48.0f;
    float depth = 46.0f;
};

struct CentralSpire {
    float x = 0.0f;
    float z = 0.0f;
    float radius = 24.0f;
    float height = 34.0f;
};

inline float fract(float x) { return x - std::floor(x); }
inline float mix(float a, float b, float t) { return a + t * (b - a); }
inline QVector3D mix(const QVector3D& a, const QVector3D& b, float t) { return a * (1.0f - t) + b * t; }
inline float clamp(float x, float a, float b) { return std::max(a, std::min(b, x)); }
inline float smoothstep(float e0, float e1, float x) {
    float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline uint32_t hashInt(uint32_t x, uint32_t y, uint32_t z, uint32_t seed = 1337u) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u + seed * 1664525u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return h ^ (h >> 16u);
}

inline float hash3(int x, int y, int z, uint32_t seed = 1337u) {
    return static_cast<float>(hashInt(static_cast<uint32_t>(x),
                                      static_cast<uint32_t>(y),
                                      static_cast<uint32_t>(z), seed) & 0x00FFFFFFu) / 16777215.0f;
}

inline float noise3D(float x, float y, float z, uint32_t seed = 1337u) {
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    int iz = static_cast<int>(std::floor(z));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);
    float fz = z - std::floor(z);

    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);
    float uz = fz * fz * (3.0f - 2.0f * fz);

    float n000 = hash3(ix, iy, iz, seed);
    float n100 = hash3(ix + 1, iy, iz, seed);
    float n010 = hash3(ix, iy + 1, iz, seed);
    float n110 = hash3(ix + 1, iy + 1, iz, seed);
    float n001 = hash3(ix, iy, iz + 1, seed);
    float n101 = hash3(ix + 1, iy, iz + 1, seed);
    float n011 = hash3(ix, iy + 1, iz + 1, seed);
    float n111 = hash3(ix + 1, iy + 1, iz + 1, seed);

    float nx00 = mix(n000, n100, ux);
    float nx10 = mix(n010, n110, ux);
    float nx01 = mix(n001, n101, ux);
    float nx11 = mix(n011, n111, ux);

    return mix(mix(nx00, nx10, uy), mix(nx01, nx11, uy), uz) * 2.0f - 1.0f;
}

inline float fbm(float x, float y, float z, int octaves = 5, uint32_t seed = 1337u) {
    float value = 0.0f;
    float amplitude = 0.5f;
    for (int i = 0; i < octaves; ++i) {
        value += amplitude * noise3D(x, y, z, seed + i * 31u);
        float nx = x * 1.62f + z * 1.21f;
        float nz = -x * 1.21f + z * 1.62f;
        x = nx;
        y *= 2.02f;
        z = nz;
        amplitude *= 0.49f;
    }
    return value;
}

// Острые скальные гребни (Ridged Multifractal)
inline float ridgedFbm(float x, float y, float z, int octaves = 4, uint32_t seed = 2741u) {
    float sum = 0.0f;
    float amp = 0.54f;
    float weight = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        float n = 1.0f - std::abs(noise3D(x, y, z, seed + i * 53u));
        n = n * n * weight;
        weight = clamp(n * 2.1f, 0.18f, 1.0f);
        sum += n * amp;
        float nx = x * 1.74f + z * 1.18f;
        float nz = -x * 1.18f + z * 1.74f;
        x = nx;
        y *= 2.05f;
        z = nz;
        amp *= 0.48f;
    }
    return sum;
}

// Апериодический 2D-шум Вороного (F2 - F1): острые расщелины и гранёные блоки скал
inline float voronoiCracks2D(float x, float z, uint32_t seed) {
    int ix = static_cast<int>(std::floor(x));
    int iz = static_cast<int>(std::floor(z));
    float fx = x - std::floor(x);
    float fz = z - std::floor(z);

    float md1 = 8.0f;
    float md2 = 8.0f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = ix + dx;
            int cz = iz + dz;
            float rx = dx + 0.12f + 0.76f * hash3(cx, cz, 17, seed) - fx;
            float rz = dz + 0.12f + 0.76f * hash3(cx, cz, 29, seed) - fz;
            float d = std::sqrt(rx * rx + rz * rz);
            if (d < md1) {
                md2 = md1;
                md1 = d;
            } else if (d < md2) {
                md2 = d;
            }
        }
    }
    return clamp(md2 - md1, 0.0f, 1.0f);
}

// Ступенчатое геологическое террасирование (резкие уступы и полки)
inline float terraceHeight(float h, float stepH, float sharpness) {
    float scaled = h / stepH;
    float fl = std::floor(scaled);
    float fr = scaled - fl;
    float t = smoothstep(0.5f - sharpness, 0.5f + sharpness, fr);
    return (fl + t) * stepH;
}

inline float tileableNoise2D(float u, float v, int period, uint32_t seed) {
    float x = u * period;
    float y = v * period;
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);

    float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

    int mask = period - 1;
    int x0 = ix & mask, x1 = (ix + 1) & mask;
    int y0 = iy & mask, y1 = (iy + 1) & mask;

    float n00 = hash3(x0, y0, 0, seed);
    float n10 = hash3(x1, y0, 0, seed);
    float n01 = hash3(x0, y1, 0, seed);
    float n11 = hash3(x1, y1, 0, seed);

    return mix(mix(n00, n10, ux), mix(n01, n11, ux), uy);
}

inline float tileableVoronoi2D(float u, float v, int period, uint32_t seed) {
    float x = u * period;
    float y = v * period;
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);

    int mask = period - 1;
    float md1 = 8.0f, md2 = 8.0f;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = (ix + dx) & mask;
            int cy = (iy + dy) & mask;
            float rx = dx + hash3(cx, cy, 11, seed) - fx;
            float ry = dy + hash3(cx, cy, 23, seed) - fy;
            float d = std::sqrt(rx * rx + ry * ry);
            if (d < md1) {
                md2 = md1;
                md1 = d;
            } else if (d < md2) {
                md2 = d;
            }
        }
    }
    return clamp(md2 - md1, 0.0f, 1.0f);
}

// Синхронизированное с шейдером воды вертикальное смещение волн (4 направленных гармоники)
inline float sampleWaterWaveOffset(float x, float z, float t) {
    const float w1 = std::sin(x *  0.048f + z *  0.032f + t * 1.15f) * 0.26f;
    const float w2 = std::sin(x * -0.062f + z *  0.054f + t * 1.48f) * 0.18f;
    const float w3 = std::cos(x *  0.115f - z *  0.095f + t * 2.05f) * 0.11f;
    const float w4 = std::sin(x *  0.185f + z *  0.165f - t * 2.75f) * 0.06f;
    return w1 + w2 + w3 + w4;
}

inline float sampleWaterWaveHeight(float x, float z, float t) {
    return WATER_SURFACE_Y + sampleWaterWaveOffset(x, z, t);
}

inline QVector3D sampleWaterWaveNormal(float x, float z, float t) {
    const float c1 = std::cos(x *  0.048f + z *  0.032f + t * 1.15f) * 0.26f;
    const float c2 = std::cos(x * -0.062f + z *  0.054f + t * 1.48f) * 0.18f;
    const float s3 = -std::sin(x * 0.115f - z *  0.095f + t * 2.05f) * 0.11f;
    const float c4 = std::cos(x *  0.185f + z *  0.165f - t * 2.75f) * 0.06f;

    const float dhdx = c1 * 0.048f + c2 * (-0.062f) + s3 * 0.115f + c4 * 0.185f;
    const float dhdz = c1 * 0.032f + c2 *   0.054f  + s3 * (-0.095f) + c4 * 0.165f;
    return QVector3D(-dhdx, 1.0f, -dhdz).normalized();
}

// 5 глубоких абиссальных впадин (ближайшая ~135 м впереди-слева от старта)
inline const std::array<AbyssalPit, ABYSSAL_PIT_COUNT>& getAbyssalPits(uint32_t seed) {
    static thread_local uint32_t cachedSeed = 0xFFFFFFFFu;
    static thread_local std::array<AbyssalPit, ABYSSAL_PIT_COUNT> pits{};
    if (cachedSeed != seed) {
        cachedSeed = seed;
        constexpr float TWO_PI = 6.28318530718f;

        float a0 = -2.28f + (hash3(0, 1, 2, seed + 1901u) - 0.5f) * 0.35f;
        float r0 = 132.0f + hash3(0, 3, 4, seed + 1901u) * 28.0f;
        pits[0] = {std::cos(a0) * r0, std::sin(a0) * r0, 48.0f, 46.0f};

        for (int i = 1; i < ABYSSAL_PIT_COUNT; ++i) {
            float baseAng = a0 + (TWO_PI * static_cast<float>(i)) / static_cast<float>(ABYSSAL_PIT_COUNT);
            float ang = baseAng + (hash3(i, 1, 5, seed + 1901u) - 0.5f) * 0.42f;
            float rad = 235.0f + hash3(i, 2, 6, seed + 1901u) * 155.0f;
            float pitR = 46.0f + hash3(i, 3, 7, seed + 1901u) * 16.0f;
            float pitD = 44.0f + hash3(i, 4, 8, seed + 1901u) * 14.0f;
            pits[i] = {std::cos(ang) * rad, std::sin(ang) * rad, pitR, pitD};
        }
    }
    return pits;
}

// 22 скальных пика, кекура и рифовых утёса внутри центральной акватории
inline const std::array<CentralSpire, CENTRAL_SPIRE_COUNT>& getCentralSpires(uint32_t seed) {
    static thread_local uint32_t cachedSeed = 0xFFFFFFFFu;
    static thread_local std::array<CentralSpire, CENTRAL_SPIRE_COUNT> spires{};
    if (cachedSeed != seed) {
        cachedSeed = seed;
        constexpr float TWO_PI = 6.28318530718f;
        const auto& pits = getAbyssalPits(seed);

        for (int i = 0; i < CENTRAL_SPIRE_COUNT; ++i) {
            float sx = 0.0f, sz = 0.0f, rad = 22.0f, h = 34.0f;
            for (int attempt = 0; attempt < 14; ++attempt) {
                float h1 = hash3(i, attempt, 1, seed + 2411u);
                float h2 = hash3(i, attempt, 2, seed + 2411u);
                float h3 = hash3(i, attempt, 3, seed + 2411u);
                float h4 = hash3(i, attempt, 4, seed + 2411u);

                float ang = (TWO_PI * static_cast<float>(i)) / static_cast<float>(CENTRAL_SPIRE_COUNT)
                            + (h1 - 0.5f) * 0.55f + attempt * 0.37f;
                // Первые 6 утёсов ближе к центру (58..155 м), остальные — до 430 м
                float dist = (i < 6) ? (58.0f + h2 * 98.0f) : (130.0f + h2 * 300.0f);
                sx = std::cos(ang) * dist;
                sz = std::sin(ang) * dist;
                rad = 18.0f + h3 * 24.0f;
                bool seaStack = (i % 4 == 1);
                h = seaStack ? (52.0f + h4 * 18.0f) : (28.0f + h4 * 24.0f);

                float dSpawn = std::sqrt(sx * sx + (sz - 20.0f) * (sz - 20.0f));
                float dShip0 = std::sqrt((sx - 32.0f) * (sx - 32.0f) + (sz + 34.0f) * (sz + 34.0f));
                if (dSpawn < rad + 34.0f || dShip0 < rad + 26.0f) continue;

                bool inPit = false;
                for (const auto& pit : pits) {
                    float dp = std::sqrt((sx - pit.x) * (sx - pit.x) + (sz - pit.z) * (sz - pit.z));
                    if (dp < pit.radius + rad * 0.62f) {
                        inPit = true;
                        break;
                    }
                }
                if (!inPit) break;
            }
            spires[i] = {sx, sz, rad, h};
        }
    }
    return spires;
}

// Фактор близости к замкнутому скальному кольцу по периметру
inline float sampleBasinWallFactor(float x, float z, uint32_t seed) {
    float r = std::sqrt(x * x + z * z);
    float invR = (r > 0.001f) ? (1.0f / r) : 0.0f;
    float ux = x * invR;
    float uz = z * invR;
    float rimWarp = fbm(ux * 2.2f, 0.5f, uz * 2.2f, 3, seed + 503u) * 62.0f
                    + fbm(x * 0.005f, 1.2f, z * 0.005f, 2, seed + 611u) * 32.0f;
    float effectiveR = r + rimWarp;
    return smoothstep(BASIN_INNER_RADIUS, BASIN_RIM_RADIUS, effectiveR);
}

// Выразительный рельеф без суб-сеточного алиасинга и одноточечных пиков
inline float sampleTerrainHeight(float x, float z, uint32_t seed) {
    // 1. Извилистые каньоны с крутыми, но хорошо дискретизируемыми стенками
    float warp = fbm(x * 0.0062f, 0.0f, z * 0.0062f, 3, seed) * 3.4f;
    float mainCanyonX = std::sin(z * 0.0092f + warp) * 108.0f;
    float distMain = std::abs(x - mainCanyonX);

    float sideCanyonZ = std::cos(x * 0.0098f - warp * 0.85f) * 116.0f + 42.0f;
    float distSide = std::abs(z - sideCanyonZ);

    float canyonDist = std::min(distMain, distSide * 1.12f);
    float canyonWall = smoothstep(14.0f, 66.0f, canyonDist) * 24.0f;

    float startDist = std::sqrt(x * x + (z - 20.0f) * (z - 20.0f));
    float spawnClear = 1.0f - smoothstep(12.0f, 34.0f, startDist);

    // 2. Domain-warping для естественных изломанных геологических структур
    float dwX = fbm(x * 0.014f, 0.2f, z * 0.014f, 2, seed + 223u) * 24.0f;
    float dwZ = fbm(x * 0.014f + 7.1f, 0.8f, z * 0.014f - 4.3f, 2, seed + 229u) * 24.0f;
    float wx = x + dwX;
    float wz = z + dwZ;

    // Тектонические ступени с переходом шире 2–3 ячеек (убирает «пилу» на диагоналях)
    float faultNoise = fbm(wx * 0.0092f + 1.7f, 0.3f, wz * 0.0092f - 2.1f, 3, seed + 131u);
    float shelfStep1 = smoothstep(-0.18f, 0.08f, faultNoise) * 15.0f;
    float shelfStep2 = smoothstep(0.14f,  0.34f, faultNoise) * 12.0f;

    // Сглаженные скальные гряды (без высокочастотных суб-сеточных зубцов)
    float macroRidges = ridgedFbm(wx * 0.0145f, 0.4f, wz * 0.0145f, 3, seed + 41u) * 24.5f;
    float medCrags    = ridgedFbm(x * 0.031f,   1.1f, z * 0.031f,   2, seed + 89u) * 8.2f;
    float softDetail  = fbm(x * 0.055f, 2.3f, z * 0.055f, 2, seed + 163u) * 2.1f;

    // Широкие геологические разломы (Вороной макро-масштаба без 1-ячеечных щелей)
    float vorLarge = voronoiCracks2D(wx * 0.020f, wz * 0.020f, seed + 331u);
    float riftCarve = (1.0f - smoothstep(0.05f, 0.28f, vorLarge)) * 7.8f;

    float rockyMask = clamp(smoothstep(12.0f, 44.0f, canyonDist) * 0.75f + 0.35f, 0.0f, 1.0f);
    float rawInterior = -34.0f
                        + (canyonWall + shelfStep1 + shelfStep2 + macroRidges) * (1.0f - spawnClear * 0.72f)
                        + (medCrags - riftCarve) * rockyMask * (1.0f - spawnClear * 0.65f)
                        + softDetail * (1.0f - spawnClear * 0.45f);

    // Более мягкое террасирование (sharpness = 0.31) — чёткие каменные полки без резкого скачка в 1 вершине
    float terraced = terraceHeight(rawInterior, 7.6f, 0.31f);
    float terraceWeight = rockyMask * (1.0f - spawnClear) * 0.48f;
    float interiorHeight = mix(rawInterior, terraced, terraceWeight);
    interiorHeight = clamp(interiorHeight, -38.0f, 20.0f);

    // 3. 22 скальных массива и кекура с массивными вершинными плечами (вместо конусов)
    const auto& spires = getCentralSpires(seed);
    for (const auto& sp : spires) {
        float dx = x - sp.x;
        float dz = z - sp.z;
        float d2 = dx * dx + dz * dz;
        float maxR = sp.radius * 1.55f;
        if (d2 < maxR * maxR) {
            float d = std::sqrt(d2);
            float angWarp = fbm(dx * 0.055f, 1.4f, dz * 0.055f, 3, seed + 773u) * (sp.radius * 0.34f);
            float effD = std::max(0.0f, d + angWarp);
            float t = 1.0f - smoothstep(0.0f, sp.radius, effD);
            if (t > 0.001f) {
                // Куполообразно-ступенчатый профиль с плоской скальной вершиной (производная = 0 на макушке)
                float peakDome = t * t * (3.0f - 2.0f * t);
                float shoulder = smoothstep(0.0f, 0.78f, t);
                float peakShape = mix(peakDome, shoulder, 0.32f);
                float facet = ridgedFbm(x * 0.034f, 2.0f, z * 0.034f, 2, seed + 887u) * 7.2f * peakDome;
                float spireLift = sp.height * peakShape + facet;
                spireLift = mix(spireLift, terraceHeight(spireLift, 6.8f, 0.30f), 0.38f);
                interiorHeight += spireLift;
            }
        }
    }

    // 4. 5 глубоких абиссальных впадин со ступенчатыми обрывами
    const auto& pits = getAbyssalPits(seed);
    for (const auto& pit : pits) {
        float dx = x - pit.x;
        float dz = z - pit.z;
        float d2 = dx * dx + dz * dz;
        float outerR = pit.radius * 1.55f;
        if (d2 < outerR * outerR) {
            float d = std::sqrt(d2);
            float rimJitter = fbm(dx * 0.044f, 0.8f, dz * 0.044f, 3, seed + 1237u) * (pit.radius * 0.26f);
            float effD = std::max(0.0f, d + rimJitter);

            float rimRing = std::exp(-std::pow((effD - pit.radius * 1.04f) / (pit.radius * 0.26f), 2.0f));
            interiorHeight += rimRing * (8.5f + ridgedFbm(x * 0.036f, 0.0f, z * 0.036f, 2, seed + 1291u) * 4.0f);

            float pitMask = 1.0f - smoothstep(pit.radius * 0.22f, pit.radius * 0.96f, effD);
            if (pitMask > 0.001f) {
                float wallSteep = pitMask * pitMask * (3.0f - 2.0f * pitMask);
                float pitCrags = fbm(x * 0.052f, 3.1f, z * 0.052f, 2, seed + 1451u) * 3.4f;
                float targetBottom = -23.0f - pit.depth + pitCrags;
                float mixedPit = mix(interiorHeight, targetBottom, wallSteep);
                float wallBand = 4.0f * wallSteep * (1.0f - wallSteep);
                interiorHeight = mix(mixedPit, terraceHeight(mixedPit, 9.0f, 0.29f), wallBand * 0.45f);
            }
        }
    }

    // 5. Замкнутое скальное кольцо кальдеры по периметру
    float wallFactor = sampleBasinWallFactor(x, z, seed);
    float crags = ridgedFbm(x * 0.024f, 2.4f, z * 0.024f, 3, seed + 919u) * 26.0f
                  + fbm(x * 0.052f, 0.0f, z * 0.052f, 2, seed + 313u) * 6.0f;
    float cliffHeight = terraceHeight(42.0f + crags, 8.5f, 0.30f);

    float steepBlend = wallFactor * wallFactor * (3.0f - 2.0f * wallFactor);
    float h = mix(interiorHeight, cliffHeight, steepBlend);

    float boxEdge = std::max(std::abs(x), std::abs(z));
    float hardEdge = smoothstep(WORLD_HALF_SIZE - 90.0f, WORLD_HALF_SIZE - 15.0f, boxEdge);
    h = std::max(h, mix(-78.0f, 56.0f, hardEdge));

    return h;
}

inline QVector3D sampleTerrainNormal(float x, float z, uint32_t seed, float eps = 1.2f) {
    float hL = sampleTerrainHeight(x - eps, z, seed);
    float hR = sampleTerrainHeight(x + eps, z, seed);
    float hD = sampleTerrainHeight(x, z - eps, seed);
    float hU = sampleTerrainHeight(x, z + eps, seed);
    return QVector3D(hL - hR, 2.0f * eps, hD - hU).normalized();
}

} // namespace ProcMath
