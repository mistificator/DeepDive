#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <QVector3D>

namespace ProcMath {

inline float fract(float x) { return x - std::floor(x); }
inline float mix(float a, float b, float t) { return a + t * (b - a); }
inline QVector3D mix(const QVector3D& a, const QVector3D& b, float t) { return a * (1.0f - t) + b * t; }
inline float clamp(float x, float a, float b) { return std::max(a, std::min(b, x)); }
inline float smoothstep(float e0, float e1, float x) {
    float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Быстрый целочисленный хэш без тригонометрических артефактов
inline uint32_t hashInt(uint32_t x, uint32_t y, uint32_t z, uint32_t seed = 1337u) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u + seed * 1664525u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return h ^ (h >> 16u);
}

inline float hash3(int x, int y, int z, uint32_t seed = 1337u) {
    return static_cast<float>(hashInt(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z), seed) & 0x00FFFFFFu) / 16777215.0f;
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
        // Поворот координат между октавами убирает осевую сетку
        float nx = x * 1.62f + z * 1.21f;
        float nz = -x * 1.21f + z * 1.62f;
        x = nx;
        y *= 2.02f;
        z = nz;
        amplitude *= 0.49f;
    }
    return value;
}

// Бесшовный (тайлящийся по периоду) 2D-шум для генерации mipmap-текстур высокого разрешения
inline float tileableNoise2D(float u, float v, int period, uint32_t seed) {
    float x = u * period;
    float y = v * period;
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);

    // Пятистепенная кривая Перлина (C2-гладкость для чистых карт нормалей)
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

// Бесшовный клеточный шум Вороного (трещины и поры в скальной породе)
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
    // Возвращает профиль каменных граней и расщелин (md2 - md1)
    return clamp(md2 - md1, 0.0f, 1.0f);
}

inline float sampleTerrainHeight(float x, float z, uint32_t seed) {
    float canyonCurve = std::sin(z * 0.018f + fbm(x * 0.01f, 0.0f, z * 0.01f, 2, seed) * 2.5f) * 28.0f;
    float distToCenter = std::abs(x - canyonCurve);

    float canyonWall = smoothstep(10.0f, 55.0f, distToCenter) * 38.0f;
    float macroRocks = fbm(x * 0.024f, 0.0f, z * 0.024f, 5, seed) * 15.0f;
    float ridge = std::abs(fbm(x * 0.045f, 1.3f, z * 0.045f, 4, seed)) * 11.0f * smoothstep(8.0f, 35.0f, distToCenter);
    float sandWaves = fbm(x * 0.18f, 0.0f, z * 0.18f, 2, seed + 77u) * 0.45f * (1.0f - smoothstep(5.0f, 22.0f, distToCenter));

    return -22.0f + canyonWall + macroRocks + ridge + sandWaves;
}

} // namespace ProcMath
