#pragma once

namespace Shaders {

// ============================================================================
// ШЕЙДЕРЫ ПРОХОДА КАРТЫ ТЕНЕЙ (OpenGL 3.3 Core)
// ============================================================================

inline const char* ShadowEnvVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec2 aParam; // x: 0=rock, 1=wood, 2=reef/grass, 3=kelp, 4=abyssalGlow, 5=kelpVesicle; y: sway

uniform mat4 uLightSpaceMatrix;
uniform float uTime;

void main() {
    vec3 pos = aPos;

    if (aParam.x > 4.5 || (aParam.x > 2.5 && aParam.x < 3.5)) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.15 + pos.x * 0.12 + pos.z * 0.10;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 3.0) * 1.35 * hCurve;
        float swayZ = cos(phase * 0.8 + h * 4.2) * 0.82 * (h * h);
        float flutter = sin(uTime * 2.8 + pos.y * 1.1 + pos.x * 0.5) * 0.14 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
    } else if (aParam.x > 3.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.45 + pos.x * 0.25 + pos.z * 0.22;
        pos.x += sin(phase + h * 2.4) * 0.24 * (h * h);
        pos.z += cos(phase * 0.85 + h * 2.8) * 0.20 * (h * h);
    } else if (aParam.x > 1.5 && aParam.y > 0.01) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.85 + pos.x * 0.28 + pos.z * 0.24;
        float bend = h * sqrt(h);
        pos.x += sin(phase + h * 2.1) * 0.28 * bend;
        pos.z += cos(phase * 0.82 + h * 2.5) * 0.24 * bend;
    }

    gl_Position = uLightSpaceMatrix * vec4(pos, 1.0);
}
)";

// Инстансированный теневой шейдер для пропов окружения (валуны, скалы, ламинария, кораллы, губки)
inline const char* ShadowInstPropVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec2 aParam;
layout(location = 4) in vec4 aInstPosScaleXZ; // xyz: worldPos, w: scaleXZ
layout(location = 5) in vec4 aInstRotScaleY;  // x: yaw, y: pitch, z: roll, w: scaleY
layout(location = 6) in vec4 aInstTintFlags;

uniform mat4 uLightSpaceMatrix;
uniform float uTime;

void main() {
    float sx = aInstPosScaleXZ.w;
    float sy = aInstRotScaleY.w;
    vec3 localPos = vec3(aPos.x * sx, aPos.y * sy, aPos.z * sx);

    float yaw = aInstRotScaleY.x;
    float pitch = aInstRotScaleY.y;
    float roll = aInstRotScaleY.z;

    float cy = cos(yaw),   syw = sin(yaw);
    float cp = cos(pitch), sp  = sin(pitch);
    float cr = cos(roll),  sr  = sin(roll);

    mat3 rotY = mat3(cy, 0.0, -syw,  0.0, 1.0, 0.0,  syw, 0.0, cy);
    mat3 rotX = mat3(1.0, 0.0, 0.0,  0.0, cp, -sp,   0.0, sp, cp);
    mat3 rotZ = mat3(cr, sr, 0.0,   -sr, cr, 0.0,    0.0, 0.0, 1.0);

    vec3 pos = (rotY * rotX * rotZ) * localPos + aInstPosScaleXZ.xyz;

    if (aParam.x > 4.5 || (aParam.x > 2.5 && aParam.x < 3.5)) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.10 + aInstPosScaleXZ.x * 0.11 + aInstPosScaleXZ.z * 0.09;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 2.8) * 1.25 * hCurve;
        float swayZ = cos(phase * 0.82 + h * 3.8) * 0.78 * (h * h);
        float flutter = sin(uTime * 2.6 + pos.y * 0.9 + aPos.x * 1.4) * 0.12 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
    } else if (aParam.x > 1.5 && aParam.y > 0.01) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.85 + pos.x * 0.30 + pos.z * 0.26;
        float bend = h * sqrt(h);
        pos.x += sin(phase + h * 2.0) * 0.26 * bend;
        pos.z += cos(phase * 0.84 + h * 2.4) * 0.22 * bend;
    }

    gl_Position = uLightSpaceMatrix * vec4(pos, 1.0);
}
)";

inline const char* ShadowFaunaVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aInstPosScale;
layout(location = 4) in vec4 aInstRotPhase;  // x: yaw, y: phase, z: type (0=fish, 1=manta, 2=whale, 3=angler), w: bank
layout(location = 5) in vec4 aInstPitchTint; // x: pitch, yzw: tint

uniform mat4 uLightSpaceMatrix;
uniform float uTime;

void main() {
    vec3 localPos = aPos;

    if (aInstRotPhase.z < 0.5) {
        float phase = aInstRotPhase.y + uTime * 9.5;
        float tailWeight = smoothstep(0.35, -0.75, localPos.z);
        localPos.x += sin(phase - localPos.z * 3.8) * 0.14 * tailWeight;
    } else if (aInstRotPhase.z < 1.5) {
        float phase = aInstRotPhase.y + uTime * 2.4;
        float wingDist = abs(localPos.x);
        float wf = clamp(wingDist / 2.05, 0.0, 1.25);
        float flap = sin(phase - wingDist * 1.05 - localPos.z * 0.35) * 0.44 * (wf * sqrt(wf));
        float tailW = smoothstep(-0.95, -3.0, localPos.z);
        float tailWave = sin(phase - 1.4 - localPos.z * 1.15) * 0.20 * (tailW * tailW);
        localPos.y += flap + tailWave;
    } else if (aInstRotPhase.z < 2.5) {
        float phase = aInstRotPhase.y + uTime * 1.35;
        float tailWeight = smoothstep(1.2, -6.4, localPos.z);
        float spineWave = sin(phase - localPos.z * 0.50) * 0.58 * (tailWeight * tailWeight);
        float flipperWeight = clamp((abs(localPos.x) - 1.30) / 2.8, 0.0, 1.0)
                            * smoothstep(-1.4, 0.2, localPos.z);
        float finSway = sin(phase - abs(localPos.x) * 0.55) * 0.16 * flipperWeight;
        localPos.y += spineWave + finSway;
    } else {
        float phase = aInstRotPhase.y + uTime * 3.2;
        float tailW = smoothstep(-0.1, -1.6, localPos.z);
        localPos.x += sin(phase - localPos.z * 2.2) * 0.16 * tailW;
        float lureW = smoothstep(0.45, 1.35, localPos.z) * smoothstep(0.15, 0.85, localPos.y);
        localPos.x += sin(uTime * 2.1 + aInstRotPhase.y) * 0.09 * lureW;
        localPos.y += cos(uTime * 1.7 + aInstRotPhase.y) * 0.07 * lureW;
    }

    localPos *= aInstPosScale.w;

    float yaw = aInstRotPhase.x;
    float pitch = aInstPitchTint.x;
    float bank = aInstRotPhase.w;

    float cy = cos(yaw), sy = sin(yaw);
    float cp = cos(pitch), sp = sin(pitch);
    float cb = cos(bank), sb = sin(bank);

    mat3 rotY = mat3(cy, 0.0, -sy,  0.0, 1.0, 0.0,  sy, 0.0, cy);
    mat3 rotX = mat3(1.0, 0.0, 0.0,  0.0, cp, -sp,  0.0, sp, cp);
    mat3 rotZ = mat3(cb, sb, 0.0,   -sb, cb, 0.0,   0.0, 0.0, 1.0);

    vec3 worldPos = (rotY * rotX * rotZ) * localPos + aInstPosScale.xyz;
    gl_Position = uLightSpaceMatrix * vec4(worldPos, 1.0);
}
)";

inline const char* ShadowDepthFrag = R"(#version 330 core
void main() {}
)";

// ============================================================================
// ОСНОВНЫЕ ШЕЙДЕРЫ ОКРУЖЕНИЯ (Статика + GPU-инстансируемые пропы)
// ============================================================================

inline const char* EnvironmentVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aParam;

uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform float uTime;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec2 vParam;
out vec4 vLightSpacePos;

void main() {
    vec3 pos = aPos;
    vec3 norm = aNormal;

    if (aParam.x > 4.5 || (aParam.x > 2.5 && aParam.x < 3.5)) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.15 + pos.x * 0.12 + pos.z * 0.10;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 3.0) * 1.35 * hCurve;
        float swayZ = cos(phase * 0.8 + h * 4.2) * 0.82 * (h * h);
        float flutter = sin(uTime * 2.8 + pos.y * 1.1 + pos.x * 0.5) * 0.14 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
        norm = normalize(norm + vec3(swayX * 0.15, 0.0, swayZ * 0.15));
    } else if (aParam.x > 3.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.45 + pos.x * 0.25 + pos.z * 0.22;
        float swayX = sin(phase + h * 2.4) * 0.24 * (h * h);
        float swayZ = cos(phase * 0.85 + h * 2.8) * 0.20 * (h * h);
        pos.x += swayX;
        pos.z += swayZ;
        norm = normalize(norm + vec3(swayX * 0.25, 0.0, swayZ * 0.25));
    } else if (aParam.x > 1.5 && aParam.y > 0.01) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.85 + pos.x * 0.28 + pos.z * 0.24;
        float bend = h * sqrt(h);
        float swayX = sin(phase + h * 2.1) * 0.28 * bend;
        float swayZ = cos(phase * 0.82 + h * 2.5) * 0.24 * bend;
        pos.x += swayX;
        pos.z += swayZ;
        norm = normalize(norm + vec3(swayX * 0.22, 0.0, swayZ * 0.22));
    }

    vWorldPos = pos;
    vNormal = norm;
    vColor = aColor;
    vParam = aParam;
    vLightSpacePos = uLightSpaceMatrix * vec4(pos, 1.0);
    gl_Position = uViewProj * vec4(pos, 1.0);
}
)";

// Инстансированный вершинный шейдер пропов (линкуется с общим EnvironmentFrag)
inline const char* InstPropVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aParam;
layout(location = 4) in vec4 aInstPosScaleXZ; // xyz: worldPos, w: scaleXZ
layout(location = 5) in vec4 aInstRotScaleY;  // x: yaw, y: pitch, z: roll, w: scaleY
layout(location = 6) in vec4 aInstTintFlags;  // rgb: tint, w: 0=fixed, 1=carpetBiome, 2=kelpBiome

uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform float uTime;
uniform vec3 uCarpetTint;
uniform vec3 uKelpTint;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec2 vParam;
out vec4 vLightSpacePos;

void main() {
    float sx = max(aInstPosScaleXZ.w, 0.01);
    float sy = max(aInstRotScaleY.w,  0.01);

    vec3 localPos = vec3(aPos.x * sx, aPos.y * sy, aPos.z * sx);
    // Точное преобразование нормалей при неравномерном масштабе (sx, sy, sx)
    vec3 localN   = normalize(vec3(aNormal.x * sy, aNormal.y * sx, aNormal.z * sy));

    float yaw   = aInstRotScaleY.x;
    float pitch = aInstRotScaleY.y;
    float roll  = aInstRotScaleY.z;

    float cy = cos(yaw),   syw = sin(yaw);
    float cp = cos(pitch), sp  = sin(pitch);
    float cr = cos(roll),  sr  = sin(roll);

    mat3 rotY = mat3(cy, 0.0, -syw,  0.0, 1.0, 0.0,  syw, 0.0, cy);
    mat3 rotX = mat3(1.0, 0.0, 0.0,  0.0, cp, -sp,   0.0, sp, cp);
    mat3 rotZ = mat3(cr, sr, 0.0,   -sr, cr, 0.0,    0.0, 0.0, 1.0);
    mat3 rot  = rotY * rotX * rotZ;

    vec3 pos  = rot * localPos + aInstPosScaleXZ.xyz;
    vec3 norm = normalize(rot * localN);

    if (aParam.x > 4.5 || (aParam.x > 2.5 && aParam.x < 3.5)) {
        // Единая фаза по корню колонны ламинарии — листья и пузырьки не отрываются от стебля
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.10 + aInstPosScaleXZ.x * 0.11 + aInstPosScaleXZ.z * 0.09;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 2.8) * 1.25 * hCurve;
        float swayZ = cos(phase * 0.82 + h * 3.8) * 0.78 * (h * h);
        float flutter = sin(uTime * 2.6 + pos.y * 0.9 + aPos.x * 1.4) * 0.12 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
        norm = normalize(norm + vec3(swayX * 0.14, 0.0, swayZ * 0.14));
    } else if (aParam.x > 3.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.45 + aInstPosScaleXZ.x * 0.24 + aInstPosScaleXZ.z * 0.22;
        float swayX = sin(phase + h * 2.4) * 0.24 * (h * h);
        float swayZ = cos(phase * 0.85 + h * 2.8) * 0.20 * (h * h);
        pos.x += swayX;
        pos.z += swayZ;
        norm = normalize(norm + vec3(swayX * 0.22, 0.0, swayZ * 0.22));
    } else if (aParam.x > 1.5 && aParam.y > 0.01) {
        // Живая волна течения по ковру придонной травы и веерным кораллам
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.85 + pos.x * 0.30 + pos.z * 0.26;
        float bend = h * sqrt(h);
        float swayX = sin(phase + h * 2.0) * 0.26 * bend;
        float swayZ = cos(phase * 0.84 + h * 2.4) * 0.22 * bend;
        pos.x += swayX;
        pos.z += swayZ;
        norm = normalize(norm + vec3(swayX * 0.20, 0.0, swayZ * 0.20));
    }

    vec3 col = aColor * aInstTintFlags.rgb;
    if (aParam.x < 4.5) {
        if (aInstTintFlags.w > 1.5) {
            col = mix(col, col * uKelpTint, 0.58);
        } else if (aInstTintFlags.w > 0.5) {
            col = mix(col, col * uCarpetTint, 0.78);
        }
    }

    vWorldPos = pos;
    vNormal = norm;
    vColor = col;
    vParam = aParam;
    vLightSpacePos = uLightSpaceMatrix * vec4(pos, 1.0);
    gl_Position = uViewProj * vec4(pos, 1.0);
}
)";

// Общее тело EnvironmentFrag для GLSL 330 и GLSL 400 (разбито на 2 части для лимита строк MSVC)
#define DEEPDIVE_ENV_FRAG_PART1 R"(
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
in vec2 vParam;
in vec4 vLightSpacePos;

uniform sampler2D uRockTex;
uniform sampler2D uShadowMap;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uVibrance;
uniform float uFogScale;
uniform float uTime;
uniform int uEnableShadows;
uniform int uLureCount;
uniform vec3 uLurePos[8];

out vec4 FragColor;

vec3 applyVibrance(vec3 col, float vib) {
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(col.r, max(col.g, col.b)) - min(col.r, min(col.g, col.b));
    float boost = mix(vib, 1.0 + (vib - 1.0) * 0.45, sat);
    return clamp(mix(vec3(lum), col, boost), 0.0, 1.28);
}

vec3 computeUnderwaterFogColor(vec3 rayDir, vec3 L, float heightFactor, float abyssMask) {
    float viewUp = clamp(rayDir.y * 0.5 + 0.5, 0.0, 1.0);
    float upBlend = clamp(pow(viewUp, 1.15) * 0.70 + heightFactor * 0.34, 0.0, 1.0);
    vec3 fogCol = mix(uWaterColorDeep * 0.86, uWaterColorShallow * 1.14, upBlend);

    float sunDot = clamp(dot(rayDir, L), 0.0, 1.0);
    float sunGlow = (sunDot * sunDot * sunDot) * 0.38;
    float zenithGlow = smoothstep(0.04, 0.85, rayDir.y) * 0.24 * heightFactor;
    fogCol += uSunColor * (sunGlow + zenithGlow) * (1.0 - abyssMask * 0.92);

    return mix(fogCol, vec3(0.004, 0.014, 0.028), abyssMask * 0.92);
}

vec4 sampleTriplanar(sampler2D tex, vec3 p, vec3 blend) {
    vec4 tx = texture(tex, p.yz);
    vec4 ty = texture(tex, p.xz);
    vec4 tz = texture(tex, p.xy);
    return tx * blend.x + ty * blend.y + tz * blend.z;
}

void sampleTriplanarDetailRot(sampler2D tex, vec3 p, vec3 blend, mat2 rot, vec2 phase,
                              out vec3 outPerturb, out float outHeight, out float outCavity) {
    vec2 uvX = rot * p.yz + phase;
    vec2 uvY = rot * p.xz + phase * 1.37;
    vec2 uvZ = rot * p.xy - phase * 0.83;

    vec4 tx = texture(tex, uvX);
    vec4 ty = texture(tex, uvY);
    vec4 tz = texture(tex, uvZ);

    mat2 invRot = transpose(rot);
    vec2 nx = invRot * (tx.rg * 2.0 - 1.0);
    vec2 ny = invRot * (ty.rg * 2.0 - 1.0);
    vec2 nz = invRot * (tz.rg * 2.0 - 1.0);

    outPerturb = vec3(0.0, nx.x, nx.y) * blend.x
               + vec3(ny.x, 0.0, ny.y) * blend.y
               + vec3(nz.x, nz.y, 0.0) * blend.z;
    outHeight  = tx.a * blend.x + ty.a * blend.y + tz.a * blend.z;
    outCavity  = tx.b * blend.x + ty.b * blend.y + tz.b * blend.z;
}

float organicCaustics(vec2 p, float t) {
    mat2 rot = mat2(0.80, -0.60, 0.60, 0.80);
    vec2 uv1 = p * 0.10 + vec2(t * 0.06, -t * 0.04);
    vec2 uv2 = (rot * p) * 0.16 + vec2(-t * 0.05, t * 0.07);
    vec2 uv3 = (rot * (rot * p)) * 0.22 + vec2(t * 0.04, t * 0.05);

    float h1 = texture(uRockTex, uv1).a;
    float h2 = texture(uRockTex, uv2).a;
    float h3 = texture(uRockTex, uv3).a;

    float c = 1.0 - abs(h1 - h2) * 2.1;
    c = max(c, 1.0 - abs(h2 - h3) * 2.1);
    float cClamped = clamp(c, 0.0, 1.0);
    float c2 = cClamped * cClamped;
    return c2 * c2;
}

float calcUnderwaterShadow(vec4 lightSpacePos, vec3 geomN, vec3 L) {
    if (uEnableShadows == 0) return 1.0;

    vec3 projCoords = lightSpacePos.xyz / max(lightSpacePos.w, 0.0001);
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 0.999 || projCoords.x < 0.001 || projCoords.x > 0.999 || projCoords.y < 0.001 || projCoords.y > 0.999) {
        return 1.0;
    }

    vec2 edgeDist = smoothstep(vec2(0.0), vec2(0.08), projCoords.xy) * (1.0 - smoothstep(vec2(0.92), vec2(1.0), projCoords.xy));
    float cascadeFade = edgeDist.x * edgeDist.y;

    float cosTheta = clamp(dot(geomN, L), 0.0, 1.0);
    float bias = max(0.0042 * (1.0 - cosTheta), 0.0015);

    float rndAngle = fract(sin(dot(vWorldPos.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float s = sin(rndAngle);
    float c = cos(rndAngle);
    mat2 rot = mat2(c, -s, s, c);

    float avgBlockerDepth = 0.0;
    int blockers = 0;
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));

    for (int i = 0; i < 4; ++i) {
        float fi = float(i) * 4.0 + 1.0;
        float r = sqrt(fi / 16.0) * 3.2;
        float theta = fi * 2.3999632;
        vec2 offset = (rot * vec2(cos(theta), sin(theta))) * r * texelSize;
        float d = texture(uShadowMap, projCoords.xy + offset).r;
        if (projCoords.z - bias > d) {
            avgBlockerDepth += d;
            blockers++;
        }
    }

    float penumbraRadius = 2.2;
    if (blockers > 0) {
        avgBlockerDepth /= float(blockers);
        float blockerDist = max(projCoords.z - avgBlockerDepth, 0.0);
        penumbraRadius = clamp(1.6 + blockerDist * 52.0, 1.6, 6.8);
    }

    float shadowSum = 0.0;
    for (int i = 0; i < 16; ++i) {
        float fi = float(i) + 0.5;
        float r = sqrt(fi / 16.0) * penumbraRadius;
        float theta = fi * 2.3999632;
        vec2 offset = (rot * vec2(cos(theta), sin(theta))) * r * texelSize;
        float pcfDepth = texture(uShadowMap, projCoords.xy + offset).r;
        shadowSum += (projCoords.z - bias > pcfDepth) ? 0.0 : 1.0;
    }

    return mix(1.0, shadowSum / 16.0, cascadeFade);
}
)"

#define DEEPDIVE_ENV_FRAG_PART2 R"(
void main() {
    vec3 safeN = length(vNormal) > 0.0001 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    vec3 geomN = gl_FrontFacing ? safeN : -safeN;

    vec3 viewVec = uCamPos - vWorldPos;
    float dist = length(viewVec);
    vec3 V = dist > 0.0001 ? (viewVec / dist) : vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(uSunDir);

    float vertexFlatness = (vParam.x < 0.5) ? clamp(1.0 - vParam.y, 0.0, 1.0) : 1.0;

    vec3 blendN = abs(geomN);
    if (vParam.x < 0.5) {
        // Плавное подавление верхней проекции на крутых стенах без полигональных стыков
        float steepBlend = smoothstep(0.68, 0.25, vertexFlatness);
        blendN.y = mix(blendN.y, min(blendN.y, vertexFlatness), steepBlend * 0.85);
    }

    vec3 blend = max(blendN, vec3(0.0001));
    blend = blend * blend;
    blend = blend * blend;
    blend /= (blend.x + blend.y + blend.z);

    vec3 N = geomN;
    float vibStrength = (vParam.x > 1.5) ? uVibrance : mix(1.0, uVibrance, 0.50);
    vec3 baseCol = applyVibrance(clamp(vColor, 0.0, 1.0), vibStrength);
    float ao = 1.0;

    if (vParam.x < 1.5) {
        const mat2 rotA = mat2( 1.000,  0.000,  0.000,  1.000);
        const mat2 rotB = mat2( 0.800, -0.600,  0.600,  0.800);
        const mat2 rotC = mat2(-0.540, -0.841,  0.841, -0.540);

        vec3 pMacro, pMicro, pFine;
        float hMacro, hMicro, hFine;
        float cMacro, cMicro, cFine;

        sampleTriplanarDetailRot(uRockTex, vWorldPos * 0.036, blend, rotA, vec2(0.0), pMacro, hMacro, cMacro);
        vec3 warpedPos = vWorldPos + pMacro * 1.45;

        sampleTriplanarDetailRot(uRockTex, warpedPos * 0.19, blend, rotB, vec2(0.37, 0.71), pMicro, hMicro, cMicro);

        float fineWeight = 1.0 - smoothstep(75.0, 195.0, dist);
        if (fineWeight > 0.01) {
            sampleTriplanarDetailRot(uRockTex, warpedPos * 0.88, blend, rotC, vec2(0.61, 0.19), pFine, hFine, cFine);
        } else {
            pFine = pMicro;
            hFine = hMicro;
            cFine = cMicro;
        }

        float strataCoord = vWorldPos.y * 1.55 + hMacro * 4.6 + hMicro * 1.3;
        float strataWave  = sin(strataCoord);
        float strataRidge = smoothstep(-0.35, 0.55, strataWave) * 0.14 - 0.06;
        vec3 strataPerturb = vec3(0.0, cos(strataCoord) * 0.22 * (1.0 - abs(geomN.y)), 0.0);

        vec3 perturb = pMacro * 0.52 + pMicro * 0.50 + pFine * (0.34 * fineWeight) + strataPerturb;
        N = normalize(geomN + perturb * 0.92);

        float detailHeight = hMacro * 0.44 + hMicro * 0.38 + hFine * 0.18;
        float cavity       = cMacro * 0.50 + cMicro * 0.35 + cFine * 0.15;
        float crevice      = smoothstep(0.10, 0.78, cavity * 0.62 + detailHeight * 0.38);
        ao = crevice * 0.62 + 0.38;

        if (vParam.x < 0.5) {
            float rockEdgeNoise = (detailHeight - 0.5) * 0.24 + (cavity - 0.5) * 0.16;
            float smoothUp = min(min(geomN.y, N.y), mix(geomN.y, vertexFlatness, 0.55));
            float wallMask = smoothstep(0.66, 0.34, smoothUp + rockEdgeNoise);
            float abyssM   = smoothstep(-24.0, -52.0, vWorldPos.y);
            vec3 mineralRock = mix(vec3(0.16, 0.22, 0.27), vec3(0.24, 0.30, 0.33), detailHeight);
            mineralRock = mix(mineralRock, vec3(0.07, 0.10, 0.15), abyssM);
            baseCol = mix(baseCol, mineralRock, wallMask * 0.82);

            float edgeWear = smoothstep(0.68, 0.92, detailHeight) * wallMask * (1.0 - abyssM);
            baseCol += vec3(0.055, 0.068, 0.072) * edgeWear;
        }

        vec3 creviceTint = mix(uWaterColorDeep * 0.46, vec3(1.0), crevice);
        baseCol = clamp(baseCol * creviceTint * (0.70 + detailHeight * 0.54 + strataRidge), 0.0, 1.25);
    } else if (vParam.x < 4.5) {
        const mat2 rotOrg = mat2(0.800, -0.600, 0.600, 0.800);
        vec3 pOrg;
        float hOrg, cOrg;
        sampleTriplanarDetailRot(uRockTex, vWorldPos * 0.46, blend, rotOrg, vec2(0.2, 0.4), pOrg, hOrg, cOrg);
        float orgRelief = (vParam.x < 2.5) ? 0.34 : 0.24;
        N = normalize(geomN + pOrg * orgRelief);
        ao = 0.78 + hOrg * 0.22;
        baseCol *= (0.82 + hOrg * 0.36);
    }

    float shadow = calcUnderwaterShadow(vLightSpacePos, geomN, L);
    float directShadow = mix(0.14, 1.0, shadow);

    float abyssDark = smoothstep(-22.0, -56.0, vWorldPos.y);
    float sunSurvival = mix(1.0, 0.03, abyssDark);
    float ambSurvival = mix(1.0, 0.08, abyssDark);

    float wrapDiff = clamp((dot(N, L) + 0.44) / 1.44, 0.0, 1.0);
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 H = normalize(L + V + vec3(0.0001));
    float specBase = clamp(dot(N, H), 0.0, 1.0);
    float spec = pow(max(specBase, 0.0001), 28.0) * step(0.0002, specBase) * 0.24 * ao * shadow * sunSurvival;

    float rimBase = clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0);
    float rim = rimBase * rimBase * rimBase * 0.28 * mix(0.40, 1.0, shadow) * ambSurvival;

    float sss = 0.0;
    if (vParam.x > 1.5 && vParam.x < 4.5) {
        float backLight = clamp(dot(V, -L + N * 0.32), 0.0, 1.0);
        sss = ((backLight * backLight) * 0.85 + 0.16) * mix(0.35, 1.0, shadow) * sunSurvival;
    }

    vec2 caustProj = vWorldPos.xz - vWorldPos.y * (L.xz / max(L.y, 0.45));
    float caust = organicCaustics(caustProj, uTime);
    float depthAtten = clamp((vWorldPos.y + 34.0) / 58.0, 0.08, 1.0);
    float underwaterMask = 1.0 - smoothstep(26.4, 28.4, vWorldPos.y);
    float slopeUp = min(max(geomN.y, 0.0), vertexFlatness);
    float slopeCaustFade = smoothstep(0.42, 0.82, slopeUp);
    float sunFaceFade = smoothstep(0.08, 0.46, dot(geomN, L));
    float caustMask = slopeCaustFade * sunFaceFade * depthAtten * ao * shadow * underwaterMask * (1.0 - abyssDark);

    vec3 sunColor = uSunColor;
    vec3 ambientColor = mix(uWaterColorDeep * 0.82, uWaterColorShallow * 1.04,
                            clamp(hemi * 0.55 + depthAtten * 0.45, 0.0, 1.0) * mix(0.48, 1.0, shadow));
    ambientColor *= ambSurvival;

    vec3 litColor = baseCol * ao * (ambientColor * 0.70 + sunColor * wrapDiff * 1.14 * directShadow * sunSurvival)
                  + sunColor * spec
                  + uWaterColorShallow * rim * depthAtten;
    litColor += sunColor * caust * caustMask * 0.94;

    if (vParam.x > 2.5 && vParam.x < 3.5) {
        vec3 kelpSssCol = mix(baseCol * 1.25, vec3(0.56, 0.94, 0.34), 0.45);
        litColor += kelpSssCol * sss * 0.95;
    } else if (vParam.x > 1.5 && vParam.x <= 2.5) {
        litColor += baseCol * 1.10 * sss * 0.72;
    }

    // Полоса прибойной пены и поднимающихся пузырьков на скалах прямо под урезом воды
    if (vParam.x < 1.5) {
        float waveSurfY = 28.0
                        + sin(dot(vWorldPos.xz, vec2(0.82, 0.57)) * 0.17 + uTime * 1.15) * 0.17
                        + sin(dot(vWorldPos.xz, vec2(-0.64, 0.77)) * 0.27 + uTime * 1.48) * 0.11;
        float belowSurf = waveSurfY - vWorldPos.y;
        if (belowSurf > -0.35 && belowSurf < 2.6) {
            float band = (1.0 - smoothstep(0.0, 2.5, max(belowSurf, 0.0))) * smoothstep(-0.35, 0.08, belowSurf);
            vec4 b1 = sampleTriplanar(uRockTex, vWorldPos * 0.48 + vec3(0.0, -uTime * 0.18, 0.0), blend);
            vec4 b2 = sampleTriplanar(uRockTex, vWorldPos * 1.35 + vec3(0.0, -uTime * 0.34, 0.0), blend);

            float foamRibbon = (1.0 - smoothstep(0.0, 0.55, abs(belowSurf))) * (0.48 + 0.52 * b1.a);
            float bubbleVeil = smoothstep(0.42, 0.82, b1.a * 0.55 + b2.a * 0.45) * band;
            float microBubbles = smoothstep(0.76, 0.91, b2.a) * band * 1.15;
            float cliffFoam = clamp(foamRibbon * 0.90 + bubbleVeil * 0.72 + microBubbles, 0.0, 1.0);

            vec3 surfFoamCol = mix(uWaterColorShallow * 1.30, vec3(0.95, 1.02, 1.05), 0.72) * mix(vec3(1.0), uSunColor, 0.55);
            litColor = mix(litColor, surfFoamCol, cliffFoam * 0.78);
        }
    }

    // Динамический биолюминесцентный свет от фонариков рыб-удильщиков
    vec3 lureCol = vec3(0.45, 1.25, 1.15);
    for (int i = 0; i < uLureCount; ++i) {
        vec3 toL = uLurePos[i] - vWorldPos;
        float d2 = dot(toL, toL);
        if (d2 < 1150.0) {
            float d = sqrt(max(d2, 0.01));
            vec3 lDir = toL / d;
            float pulse = 0.84 + 0.16 * sin(uTime * 3.1 + float(i) * 1.9);
            float atten = pulse / (1.0 + 0.018 * d2);
            float ndl = clamp((dot(N, lDir) + 0.25) / 1.25, 0.0, 1.0);
            litColor += baseCol * lureCol * (ndl * atten * 3.4);
        }
    }

    float bioFogScale = 1.0;
    if (vParam.x > 4.5) {
        float pulse = 0.72 + 0.28 * sin(uTime * 3.0 + vWorldPos.y * 1.3 + vWorldPos.x * 0.8 + vWorldPos.z * 0.7);
        litColor = vColor * (1.35 + 0.75 * pulse);
        bioFogScale = 0.58;
    } else if (vParam.x > 3.5) {
        float tip = clamp(vParam.y, 0.0, 1.0);
        float pulse = 0.68 + 0.32 * sin(uTime * 2.2 + vWorldPos.x * 0.35 + vWorldPos.z * 0.35 + tip * 3.0);
        litColor += vColor * (0.35 + tip * tip * 1.75) * pulse;
        bioFogScale = 1.0 - tip * 0.52;
    } else if (vParam.x > 2.5) {
        vec4 bioSample = sampleTriplanar(uRockTex, vWorldPos * 1.45, blend);
        float glowMask = smoothstep(0.78, 0.88, bioSample.a);
        float pulse = 0.55 + 0.45 * sin(uTime * 2.8 + vWorldPos.y * 1.5 + vWorldPos.x);
        litColor += vec3(0.58, 1.25, 0.76) * glowMask * pulse * 0.95;
    }

    float fogDensity = mix(0.0155 * uFogScale, 0.0220, abyssDark);
    vec3 transmittance = exp(-dist * bioFogScale * fogDensity * vec3(1.85, 1.06, 0.86));

    float camHeightFactor = clamp((uCamPos.y + 34.0) / 62.0, 0.06, 1.0);
    float fogHeightFactor = mix(camHeightFactor, depthAtten, 0.35);
    vec3 fogColor = computeUnderwaterFogColor(-V, L, fogHeightFactor, abyssDark);

    vec3 outCol = litColor * transmittance + fogColor * (1.0 - transmittance);
    FragColor = vec4(clamp(outCol, 0.0, 8.0), 1.0);
}
)"

inline const char* EnvironmentFrag =
    "#version 330 core\n"
    DEEPDIVE_ENV_FRAG_PART1
        DEEPDIVE_ENV_FRAG_PART2;

inline const char* EnvironmentFrag400 =
    "#version 400 core\n"
    DEEPDIVE_ENV_FRAG_PART1
        DEEPDIVE_ENV_FRAG_PART2;

// ============================================================================
// АППАРАТНАЯ ТЕССЕЛЯЦИЯ ОКРУЖЕНИЯ (OpenGL 4.0 Core — рельеф LOD0 + скалы LOD0)
// ============================================================================

inline const char* EnvTessVert = R"(#version 400 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aParam;

out vec3 vPos_TCS;
out vec3 vNormal_TCS;
out vec3 vColor_TCS;
out vec2 vParam_TCS;

void main() {
    vPos_TCS    = aPos;
    vNormal_TCS = aNormal;
    vColor_TCS  = aColor;
    vParam_TCS  = aParam;
}
)";

inline const char* EnvTessControl = R"(#version 400 core
layout(vertices = 3) out;

in vec3 vPos_TCS[];
in vec3 vNormal_TCS[];
in vec3 vColor_TCS[];
in vec2 vParam_TCS[];

out vec3 vPos_TES[];
out vec3 vNormal_TES[];
out vec3 vColor_TES[];
out vec2 vParam_TES[];

uniform vec3 uCamPos;
uniform float uMaxTessLevel;

float calcEdgeTess(vec3 pA, vec3 pB) {
    vec3 mid = 0.5 * (pA + pB);
    float dist = length(uCamPos - mid);
    // Сводим уровень разбиения к 1.0 на 135 м (задолго до границы LOD0->LOD1 на 202 м),
    // чтобы между чанками разных LOD не возникало трещин
    float t = 1.0 - smoothstep(16.0, 135.0, dist);
    float maxL = min(uMaxTessLevel, 6.0);
    return mix(1.0, maxL, t * t);
}

void main() {
    vPos_TES[gl_InvocationID]    = vPos_TCS[gl_InvocationID];
    vNormal_TES[gl_InvocationID] = vNormal_TCS[gl_InvocationID];
    vColor_TES[gl_InvocationID]  = vColor_TCS[gl_InvocationID];
    vParam_TES[gl_InvocationID]  = vParam_TCS[gl_InvocationID];

    if (gl_InvocationID == 0) {
        float e0 = calcEdgeTess(vPos_TCS[1], vPos_TCS[2]);
        float e1 = calcEdgeTess(vPos_TCS[2], vPos_TCS[0]);
        float e2 = calcEdgeTess(vPos_TCS[0], vPos_TCS[1]);

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = max(1.0, (e0 + e1 + e2) * 0.3333333);
    }
}
)";

inline const char* EnvTessEval = R"(#version 400 core
layout(triangles, fractional_even_spacing, ccw) in;

in vec3 vPos_TES[];
in vec3 vNormal_TES[];
in vec3 vColor_TES[];
in vec2 vParam_TES[];

uniform sampler2D uRockTex;
uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform vec3 uCamPos;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec2 vParam;
out vec4 vLightSpacePos;

float sampleTriplanarHeightLod(vec3 p, vec3 blend, float lod) {
    float hx = textureLod(uRockTex, p.yz, lod).a;
    float hy = textureLod(uRockTex, p.xz, lod).a;
    float hz = textureLod(uRockTex, p.xy, lod).a;
    return hx * blend.x + hy * blend.y + hz * blend.z;
}

void main() {
    vec3 b = gl_TessCoord;

    vec3 pos = b.x * vPos_TES[0] + b.y * vPos_TES[1] + b.z * vPos_TES[2];
    vec3 nSum = b.x * vNormal_TES[0] + b.y * vNormal_TES[1] + b.z * vNormal_TES[2];
    vec3 norm = (length(nSum) > 0.0001) ? normalize(nSum) : vec3(0.0, 1.0, 0.0);
    vec3 col = b.x * vColor_TES[0] + b.y * vColor_TES[1] + b.z * vColor_TES[2];
    vec2 param = b.x * vParam_TES[0] + b.y * vParam_TES[1] + b.z * vParam_TES[2];

    float dist = length(uCamPos - pos);
    float dispFade = 1.0 - smoothstep(22.0, 120.0, dist);

    if (dispFade > 0.001 && param.x < 0.5) {
        vec3 blend = max(abs(norm), vec3(0.0001));
        blend = blend * blend;
        blend = blend * blend;
        blend /= (blend.x + blend.y + blend.z);

        float lod = clamp(dist * 0.025, 0.5, 3.0);
        float hMacro = sampleTriplanarHeightLod(pos * 0.036, blend, lod);
        float hMicro = sampleTriplanarHeightLod(pos * 0.19,  blend, lod + 0.5);
        float h = hMacro * 0.62 + hMicro * 0.38;

        // Усиленный рельеф на скальных стенках и мягкое смещение на песке
        float amp = mix(0.22, 0.42, clamp(param.y, 0.0, 1.0));
        pos += norm * ((h - 0.5) * amp * dispFade);
    }

    vWorldPos      = pos;
    vNormal        = norm;
    vColor         = col;
    vParam         = param;
    vLightSpacePos = uLightSpaceMatrix * vec4(pos, 1.0);
    gl_Position    = uViewProj * vec4(pos, 1.0);
}
)";

// Инстансированная тесселяция валунов и скальных уступов (Phong + displacement)
inline const char* InstPropTessVert = R"(#version 400 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aParam;
layout(location = 4) in vec4 aInstPosScaleXZ; // xyz: worldPos, w: scaleXZ
layout(location = 5) in vec4 aInstRotScaleY;  // x: yaw, y: pitch, z: roll, w: scaleY
layout(location = 6) in vec4 aInstTintFlags;  // rgb: tint, w: flags

uniform vec3 uCarpetTint;
uniform vec3 uKelpTint;

out vec3 vPos_TCS;
out vec3 vNormal_TCS;
out vec3 vColor_TCS;
out vec2 vParam_TCS;

void main() {
    float sx = max(aInstPosScaleXZ.w, 0.01);
    float sy = max(aInstRotScaleY.w,  0.01);

    vec3 localPos = vec3(aPos.x * sx, aPos.y * sy, aPos.z * sx);
    vec3 localN   = normalize(vec3(aNormal.x * sy, aNormal.y * sx, aNormal.z * sy));

    float yaw   = aInstRotScaleY.x;
    float pitch = aInstRotScaleY.y;
    float roll  = aInstRotScaleY.z;

    float cy = cos(yaw),   syw = sin(yaw);
    float cp = cos(pitch), sp  = sin(pitch);
    float cr = cos(roll),  sr  = sin(roll);

    mat3 rotY = mat3(cy, 0.0, -syw,  0.0, 1.0, 0.0,  syw, 0.0, cy);
    mat3 rotX = mat3(1.0, 0.0, 0.0,  0.0, cp, -sp,   0.0, sp, cp);
    mat3 rotZ = mat3(cr, sr, 0.0,   -sr, cr, 0.0,    0.0, 0.0, 1.0);
    mat3 rot  = rotY * rotX * rotZ;

    vec3 col = aColor * aInstTintFlags.rgb;
    if (aParam.x < 4.5) {
        if (aInstTintFlags.w > 1.5) {
            col = mix(col, col * uKelpTint, 0.58);
        } else if (aInstTintFlags.w > 0.5) {
            col = mix(col, col * uCarpetTint, 0.78);
        }
    }

    vPos_TCS    = rot * localPos + aInstPosScaleXZ.xyz;
    vNormal_TCS = normalize(rot * localN);
    vColor_TCS  = col;
    vParam_TCS  = aParam;
}
)";

inline const char* InstPropTessControl = R"(#version 400 core
layout(vertices = 3) out;

in vec3 vPos_TCS[];
in vec3 vNormal_TCS[];
in vec3 vColor_TCS[];
in vec2 vParam_TCS[];

out vec3 vPos_TES[];
out vec3 vNormal_TES[];
out vec3 vColor_TES[];
out vec2 vParam_TES[];

uniform vec3 uCamPos;
uniform float uMaxTessLevel;

float calcEdgeTess(vec3 pA, vec3 pB) {
    vec3 mid = 0.5 * (pA + pB);
    float dist = length(uCamPos - mid);
    float t = 1.0 - smoothstep(14.0, 115.0, dist);
    float maxL = min(uMaxTessLevel, 5.0);
    return mix(1.0, maxL, t * t);
}

void main() {
    vPos_TES[gl_InvocationID]    = vPos_TCS[gl_InvocationID];
    vNormal_TES[gl_InvocationID] = vNormal_TCS[gl_InvocationID];
    vColor_TES[gl_InvocationID]  = vColor_TCS[gl_InvocationID];
    vParam_TES[gl_InvocationID]  = vParam_TCS[gl_InvocationID];

    if (gl_InvocationID == 0) {
        float e0 = calcEdgeTess(vPos_TCS[1], vPos_TCS[2]);
        float e1 = calcEdgeTess(vPos_TCS[2], vPos_TCS[0]);
        float e2 = calcEdgeTess(vPos_TCS[0], vPos_TCS[1]);

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = max(1.0, (e0 + e1 + e2) * 0.3333333);
    }
}
)";

inline const char* InstPropTessEval = R"(#version 400 core
layout(triangles, fractional_even_spacing, ccw) in;

in vec3 vPos_TES[];
in vec3 vNormal_TES[];
in vec3 vColor_TES[];
in vec2 vParam_TES[];

uniform sampler2D uRockTex;
uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform vec3 uCamPos;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec2 vParam;
out vec4 vLightSpacePos;

vec3 safeNorm(vec3 v) {
    float l = length(v);
    return (l > 0.0001) ? (v / l) : vec3(0.0, 1.0, 0.0);
}

float sampleTriplanarHeightLod(vec3 p, vec3 blend, float lod) {
    float hx = textureLod(uRockTex, p.yz, lod).a;
    float hy = textureLod(uRockTex, p.xz, lod).a;
    float hz = textureLod(uRockTex, p.xy, lod).a;
    return hx * blend.x + hy * blend.y + hz * blend.z;
}

void main() {
    vec3 b = gl_TessCoord;

    vec3 p0 = vPos_TES[0], p1 = vPos_TES[1], p2 = vPos_TES[2];
    vec3 n0 = safeNorm(vNormal_TES[0]);
    vec3 n1 = safeNorm(vNormal_TES[1]);
    vec3 n2 = safeNorm(vNormal_TES[2]);

    vec3 pLin  = b.x * p0 + b.y * p1 + b.z * p2;
    vec3 norm  = safeNorm(b.x * n0 + b.y * n1 + b.z * n2);
    vec3 col   = b.x * vColor_TES[0] + b.y * vColor_TES[1] + b.z * vColor_TES[2];
    vec2 param = b.x * vParam_TES[0] + b.y * vParam_TES[1] + b.z * vParam_TES[2];

    float dist = length(uCamPos - pLin);
    float fade = 1.0 - smoothstep(18.0, 105.0, dist);

    vec3 pos = pLin;
    if (fade > 0.001) {
        // Phong-тесселяция сглаживает силуэт валунов и карнизов без разрыва общих рёбер
        vec3 q0 = pLin - dot(pLin - p0, n0) * n0;
        vec3 q1 = pLin - dot(pLin - p1, n1) * n1;
        vec3 q2 = pLin - dot(pLin - p2, n2) * n2;
        vec3 pPhong = b.x * q0 + b.y * q1 + b.z * q2;
        pos = mix(pLin, pPhong, 0.55 * fade);

        // Каменное смещение вдоль нормали для объёмных сколов
        vec3 blend = max(abs(norm), vec3(0.0001));
        blend = blend * blend;
        blend = blend * blend;
        blend /= (blend.x + blend.y + blend.z);

        float lod = clamp(dist * 0.025, 0.5, 3.0);
        float hMacro = sampleTriplanarHeightLod(pos * 0.055, blend, lod);
        float hMicro = sampleTriplanarHeightLod(pos * 0.24,  blend, lod + 0.5);
        float h = hMacro * 0.60 + hMicro * 0.40;
        pos += norm * ((h - 0.5) * 0.26 * fade);
    }

    vWorldPos      = pos;
    vNormal        = norm;
    vColor         = col;
    vParam         = param;
    vLightSpacePos = uLightSpaceMatrix * vec4(pos, 1.0);
    gl_Position    = uViewProj * vec4(pos, 1.0);
}
)";

// ============================================================================
// ШЕЙДЕРЫ ФАУНЫ (Рыбы, Скаты-манты, Кит, Рыба-удильщик)
// ============================================================================

inline const char* FaunaVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec4 aInstPosScale;
layout(location = 4) in vec4 aInstRotPhase;  // x: yaw, y: phase, z: type (0=fish, 1=manta, 2=whale, 3=angler), w: bank
layout(location = 5) in vec4 aInstPitchTint; // x: pitch, yzw: colorTint

uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform float uTime;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec4 vLightSpacePos;
out float vEmissive;

void main() {
    vec3 localPos = aPos;
    vec3 localN = aNormal;

    if (aInstRotPhase.z < 0.5) {
        float phase = aInstRotPhase.y + uTime * 9.5;
        float tailWeight = smoothstep(0.35, -0.75, localPos.z);
        float wave = sin(phase - localPos.z * 3.8) * 0.14 * tailWeight;
        float dWaveDz = -3.8 * cos(phase - localPos.z * 3.8) * 0.14 * tailWeight;
        localPos.x += wave;
        localN = normalize(vec3(localN.x - dWaveDz * localN.z, localN.y, localN.z));
    } else if (aInstRotPhase.z < 1.5) {
        float phase = aInstRotPhase.y + uTime * 2.4;
        float wingDist = abs(localPos.x);
        float wf = clamp(wingDist / 2.05, 0.0, 1.25);
        float wingFactor = wf * sqrt(wf);
        float waveArg = phase - wingDist * 1.05 - localPos.z * 0.35;
        float sW = sin(waveArg);
        float cW = cos(waveArg);
        float flap = sW * 0.44 * wingFactor;

        float dWfDx = (wingDist > 0.001 && wf < 1.25) ? (sign(localPos.x) / 2.05) : 0.0;
        float dFactorDx = 1.5 * sqrt(max(wf, 0.0001)) * dWfDx;
        float dFlapDx = 0.44 * (-1.05 * sign(localPos.x) * cW * wingFactor + sW * dFactorDx);
        float dFlapDz = 0.44 * (-0.35 * cW * wingFactor);

        float tailW = smoothstep(-0.95, -3.0, localPos.z);
        float tailArg = phase - 1.4 - localPos.z * 1.15;
        float tailWave = sin(tailArg) * 0.20 * (tailW * tailW);
        float dTailDz = -1.15 * cos(tailArg) * 0.20 * (tailW * tailW);

        localPos.y += flap + tailWave;
        localN = normalize(vec3(localN.x - dFlapDx * localN.y,
                                localN.y,
                                localN.z - (dFlapDz + dTailDz) * localN.y));
    } else if (aInstRotPhase.z < 2.5) {
        float phase = aInstRotPhase.y + uTime * 1.35;
        float tailWeight = smoothstep(1.2, -6.4, localPos.z);
        float w2 = tailWeight * tailWeight;
        float waveArg = phase - localPos.z * 0.50;
        float spineWave = sin(waveArg) * 0.58 * w2;
        float dWaveDz = -0.50 * cos(waveArg) * 0.58 * w2;

        float flipperWeight = clamp((abs(localPos.x) - 1.30) / 2.8, 0.0, 1.0)
                            * smoothstep(-1.4, 0.2, localPos.z);
        float finSway = sin(phase - abs(localPos.x) * 0.55) * 0.16 * flipperWeight;

        localPos.y += spineWave + finSway;
        localN = normalize(vec3(localN.x, localN.y, localN.z - dWaveDz * localN.y));
    } else {
        float phase = aInstRotPhase.y + uTime * 3.2;
        float tailW = smoothstep(-0.1, -1.6, localPos.z);
        localPos.x += sin(phase - localPos.z * 2.2) * 0.16 * tailW;

        float finW = smoothstep(0.65, 1.25, abs(localPos.x));
        localPos.y += sin(uTime * 4.2 + aInstRotPhase.y) * 0.08 * finW;

        float lureW = smoothstep(0.45, 1.35, localPos.z) * smoothstep(0.15, 0.85, localPos.y);
        localPos.x += sin(uTime * 2.1 + aInstRotPhase.y) * 0.09 * lureW;
        localPos.y += cos(uTime * 1.7 + aInstRotPhase.y) * 0.07 * lureW;
    }

    localPos *= aInstPosScale.w;

    float yaw = aInstRotPhase.x;
    float pitch = aInstPitchTint.x;
    float bank = aInstRotPhase.w;

    float cy = cos(yaw), sy = sin(yaw);
    float cp = cos(pitch), sp = sin(pitch);
    float cb = cos(bank), sb = sin(bank);

    mat3 rotY = mat3(cy, 0.0, -sy,  0.0, 1.0, 0.0,  sy, 0.0, cy);
    mat3 rotX = mat3(1.0, 0.0, 0.0,  0.0, cp, -sp,  0.0, sp, cp);
    mat3 rotZ = mat3(cb, sb, 0.0,   -sb, cb, 0.0,   0.0, 0.0, 1.0);
    mat3 rot = rotY * rotX * rotZ;

    vec3 worldPos = rot * localPos + aInstPosScale.xyz;
    vWorldPos = worldPos;
    vNormal = normalize(rot * localN);

    float maxC = max(aColor.r, max(aColor.g, aColor.b));
    vEmissive = smoothstep(1.20, 1.65, maxC);
    vColor = aColor * aInstPitchTint.yzw;
    vLightSpacePos = uLightSpaceMatrix * vec4(worldPos, 1.0);

    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)";

inline const char* FaunaFrag = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
in vec4 vLightSpacePos;
in float vEmissive;

uniform sampler2D uRockTex;
uniform sampler2D uShadowMap;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uVibrance;
uniform float uFogScale;
uniform float uTime;
uniform int uEnableShadows;
uniform int uLureCount;
uniform vec3 uLurePos[8];

out vec4 FragColor;

vec4 sampleTriplanar(sampler2D tex, vec3 p, vec3 blend) {
    vec4 tx = texture(tex, p.yz);
    vec4 ty = texture(tex, p.xz);
    vec4 tz = texture(tex, p.xy);
    return tx * blend.x + ty * blend.y + tz * blend.z;
}

vec3 applyVibrance(vec3 col, float vib) {
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(col.r, max(col.g, col.b)) - min(col.r, min(col.g, col.b));
    float boost = mix(vib, 1.0 + (vib - 1.0) * 0.45, sat);
    return clamp(mix(vec3(lum), col, boost), 0.0, 1.25);
}

vec3 computeUnderwaterFogColor(vec3 rayDir, vec3 L, float heightFactor, float abyssMask) {
    float viewUp = clamp(rayDir.y * 0.5 + 0.5, 0.0, 1.0);
    float upBlend = clamp(pow(viewUp, 1.15) * 0.70 + heightFactor * 0.34, 0.0, 1.0);
    vec3 fogCol = mix(uWaterColorDeep * 0.86, uWaterColorShallow * 1.14, upBlend);

    float sunDot = clamp(dot(rayDir, L), 0.0, 1.0);
    float sunGlow = (sunDot * sunDot * sunDot) * 0.38;
    float zenithGlow = smoothstep(0.04, 0.85, rayDir.y) * 0.24 * heightFactor;
    fogCol += uSunColor * (sunGlow + zenithGlow) * (1.0 - abyssMask * 0.92);

    return mix(fogCol, vec3(0.004, 0.014, 0.028), abyssMask * 0.92);
}

float sampleFaunaShadow(vec4 lightSpacePos, vec3 N, vec3 L) {
    if (uEnableShadows == 0) return 1.0;
    vec3 projCoords = lightSpacePos.xyz / max(lightSpacePos.w, 0.0001);
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 0.999 || projCoords.x < 0.001 || projCoords.x > 0.999 || projCoords.y < 0.001 || projCoords.y > 0.999) {
        return 1.0;
    }

    float bias = max(0.0038 * (1.0 - clamp(dot(N, L), 0.0, 1.0)), 0.0014);
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float d = texture(uShadowMap, projCoords.xy + vec2(x, y) * texelSize * 2.0).r;
            shadow += (projCoords.z - bias > d) ? 0.22 : 1.0;
        }
    }
    return shadow / 9.0;
}

void main() {
    vec3 N = length(vNormal) > 0.0001 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    if (!gl_FrontFacing) N = -N;
    vec3 viewVec = uCamPos - vWorldPos;
    float dist = length(viewVec);
    vec3 V = dist > 0.0001 ? (viewVec / dist) : vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(uSunDir);

    float shadow = sampleFaunaShadow(vLightSpacePos, N, L);
    float abyssDark = smoothstep(-22.0, -56.0, vWorldPos.y);
    float sunSurvival = mix(1.0, 0.04, abyssDark);
    float ambSurvival = mix(1.0, 0.08, abyssDark);

    float diff = clamp((dot(N, L) + 0.42) / 1.42, 0.0, 1.0) * shadow * sunSurvival;
    vec3 H = normalize(L + V + vec3(0.0001));
    float specBase = clamp(dot(N, H), 0.0, 1.0);
    float spec = pow(max(specBase, 0.0001), 30.0) * step(0.0002, specBase) * 0.42 * shadow * sunSurvival;
    float rimBase = clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0);
    float rim = rimBase * rimBase * sqrt(rimBase) * 0.34 * ambSurvival;

    vec2 caustUV = vWorldPos.xz * 0.12 + vec2(uTime * 0.05, -uTime * 0.04);
    float h1 = texture(uRockTex, caustUV).a;
    float h2 = texture(uRockTex, caustUV * 1.4 - vec2(uTime * 0.04)).a;
    float underwaterMask = 1.0 - smoothstep(26.4, 28.4, vWorldPos.y);
    float caust = pow(clamp(1.0 - abs(h1 - h2) * 2.1, 0.0, 1.0), 4.0)
                * clamp(N.y, 0.0, 1.0) * shadow * underwaterMask * (1.0 - abyssDark);

    float depthAtten = clamp((vWorldPos.y + 34.0) / 58.0, 0.08, 1.0);
    vec3 ambient = mix(uWaterColorDeep, uWaterColorShallow * 1.05, depthAtten) * ambSurvival;
    vec3 sunColor = uSunColor;

    vec3 baseCol = applyVibrance(clamp(vColor, 0.0, 1.0), uVibrance);
    vec3 lit = baseCol * (ambient * 0.78 + sunColor * diff * 0.98) + sunColor * (spec + rim + caust * 0.55);

    vec3 lureCol = vec3(0.45, 1.25, 1.15);
    for (int i = 0; i < uLureCount; ++i) {
        vec3 toL = uLurePos[i] - vWorldPos;
        float d2 = dot(toL, toL);
        if (d2 < 900.0) {
            float d = sqrt(max(d2, 0.01));
            vec3 lDir = toL / d;
            float pulse = 0.84 + 0.16 * sin(uTime * 3.1 + float(i) * 1.9);
            float atten = pulse / (1.0 + 0.032 * d2);
            float ndl = clamp((dot(N, lDir) + 0.35) / 1.35, 0.0, 1.0);
            vec3 hL = normalize(lDir + V + vec3(0.0001));
            float spL = pow(max(clamp(dot(N, hL), 0.0, 1.0), 0.0001), 24.0) * 0.55;
            lit += (baseCol * ndl + vec3(spL)) * lureCol * (atten * 3.2);
        }
    }

    // Полоса прибойной пены и поднимающихся пузырьков на теле кита у уреза воды
    float waveSurfY = 28.0
                    + sin(dot(vWorldPos.xz, vec2(0.82, 0.57)) * 0.17 + uTime * 1.15) * 0.17
                    + sin(dot(vWorldPos.xz, vec2(-0.64, 0.77)) * 0.27 + uTime * 1.48) * 0.11;
    float belowSurf = waveSurfY - vWorldPos.y;
    if (belowSurf > -0.35 && belowSurf < 2.2) {
        vec3 fBlend = max(abs(N), vec3(0.0001));
        fBlend = fBlend * fBlend;
        fBlend /= (fBlend.x + fBlend.y + fBlend.z);

        float band = (1.0 - smoothstep(0.0, 2.1, max(belowSurf, 0.0))) * smoothstep(-0.35, 0.08, belowSurf);
        vec4 b1 = sampleTriplanar(uRockTex, vWorldPos * 0.48 + vec3(0.0, -uTime * 0.18, 0.0), fBlend);
        vec4 b2 = sampleTriplanar(uRockTex, vWorldPos * 1.35 + vec3(0.0, -uTime * 0.34, 0.0), fBlend);

        float foamRibbon   = (1.0 - smoothstep(0.0, 0.55, abs(belowSurf))) * (0.48 + 0.52 * b1.a);
        float bubbleVeil   = smoothstep(0.42, 0.82, b1.a * 0.55 + b2.a * 0.45) * band;
        float microBubbles = smoothstep(0.76, 0.91, b2.a) * band * 1.15;
        float surfFoam     = clamp(foamRibbon * 0.90 + bubbleVeil * 0.72 + microBubbles, 0.0, 1.0);

        vec3 surfFoamCol = mix(uWaterColorShallow * 1.30, vec3(0.95, 1.02, 1.05), 0.72)
                         * mix(vec3(1.0), uSunColor, 0.55);
        lit = mix(lit, surfFoamCol, surfFoam * 0.78);
    }

    float pulse = 0.82 + 0.18 * sin(uTime * 3.4 + vWorldPos.x * 0.2);
    vec3 emissiveTerm = vColor * vEmissive * pulse * 1.65;
    lit += emissiveTerm;

    float fogDensity = mix(0.0155 * uFogScale, 0.0220, abyssDark);
    float emScale = mix(1.0, 0.38, vEmissive);
    vec3 transmittance = exp(-dist * emScale * fogDensity * vec3(1.85, 1.06, 0.86));

    float camHeightFactor = clamp((uCamPos.y + 34.0) / 62.0, 0.06, 1.0);
    float fogHeightFactor = mix(camHeightFactor, depthAtten, 0.35);
    vec3 fogColor = computeUnderwaterFogColor(-V, L, fogHeightFactor, abyssDark);

    vec3 outCol = lit * transmittance + fogColor * (1.0 - transmittance);
    FragColor = vec4(clamp(outCol, 0.0, 8.0), 1.0);
}
)";

// ============================================================================
// 3D-ПОВЕРХНОСТЬ МОРЯ ИЗНУТРИ (GL 3.3 + аппаратная тесселяция GL 4.0)
// ============================================================================

inline const char* WaterSurfaceVert = R"(#version 330 core
layout(location = 0) in vec2 aLocalXZ;

uniform mat4 uViewProj;
uniform vec2 uWaterCenter;
uniform float uWaterY;
uniform float uTime;

out vec3 vWorldPos;
out vec2 vWaveGrad;
out float vWaveH;

void main() {
    vec2 wxz = uWaterCenter + aLocalXZ;
    float r = length(aLocalXZ);
    float edgeDamp = 1.0 - smoothstep(340.0, 445.0, r);

    float h = 0.0;
    vec2 grad = vec2(0.0);

    {
        vec2 d = vec2(0.82, 0.57);
        float k = 0.17, a = 0.17 * edgeDamp;
        float ph = dot(wxz, d) * k + uTime * 1.15;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }
    {
        vec2 d = vec2(-0.64, 0.77);
        float k = 0.27, a = 0.11 * edgeDamp;
        float ph = dot(wxz, d) * k + uTime * 1.48;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }
    {
        vec2 d = vec2(0.31, -0.95);
        float k = 0.44, a = 0.06 * edgeDamp;
        float ph = dot(wxz, d) * k - uTime * 1.90;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }
    {
        vec2 d = vec2(-0.92, -0.39);
        float k = 0.72, a = 0.035 * edgeDamp;
        float ph = dot(wxz, d) * k + uTime * 2.35;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }

    vec3 worldPos = vec3(wxz.x, uWaterY + h, wxz.y);
    vWorldPos = worldPos;
    vWaveGrad = grad;
    vWaveH = h;
    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)";

inline const char* WaterTessVert = R"(#version 400 core
layout(location = 0) in vec2 aLocalXZ;

out vec2 vLocalXZ_TCS;

void main() {
    vLocalXZ_TCS = aLocalXZ;
}
)";

inline const char* WaterTessControl = R"(#version 400 core
layout(vertices = 3) out;

in vec2 vLocalXZ_TCS[];
out vec2 vLocalXZ_TES[];

uniform vec2 uWaterCenter;
uniform float uWaterY;
uniform vec3 uCamPos;
uniform float uMaxTessLevel;

float calcEdgeTess(vec2 aXZ, vec2 bXZ) {
    vec2 midXZ = uWaterCenter + 0.5 * (aXZ + bXZ);
    float dist = length(uCamPos - vec3(midXZ.x, uWaterY, midXZ.y));
    float t = 1.0 - smoothstep(20.0, 150.0, dist);
    float maxL = min(uMaxTessLevel, 6.0);
    return mix(1.0, maxL, t * t);
}

void main() {
    vLocalXZ_TES[gl_InvocationID] = vLocalXZ_TCS[gl_InvocationID];

    if (gl_InvocationID == 0) {
        float e0 = calcEdgeTess(vLocalXZ_TCS[1], vLocalXZ_TCS[2]);
        float e1 = calcEdgeTess(vLocalXZ_TCS[2], vLocalXZ_TCS[0]);
        float e2 = calcEdgeTess(vLocalXZ_TCS[0], vLocalXZ_TCS[1]);

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = max(1.0, (e0 + e1 + e2) * 0.3333333);
    }
}
)";

inline const char* WaterTessEval = R"(#version 400 core
layout(triangles, fractional_even_spacing, ccw) in;

in vec2 vLocalXZ_TES[];

uniform mat4 uViewProj;
uniform vec2 uWaterCenter;
uniform float uWaterY;
uniform vec3 uCamPos;
uniform float uTime;

out vec3 vWorldPos;
out vec2 vWaveGrad;
out float vWaveH;

void main() {
    vec3 b = gl_TessCoord;
    vec2 localXZ = b.x * vLocalXZ_TES[0] + b.y * vLocalXZ_TES[1] + b.z * vLocalXZ_TES[2];
    vec2 wxz = uWaterCenter + localXZ;

    float r = length(localXZ);
    float edgeDamp = 1.0 - smoothstep(340.0, 445.0, r);
    float camDist = length(uCamPos - vec3(wxz.x, uWaterY, wxz.y));
    float nearChop = (1.0 - smoothstep(18.0, 120.0, camDist)) * edgeDamp;

    float h = 0.0;
    vec2 grad = vec2(0.0);

    {
        vec2 d = vec2(0.82, 0.57);
        float k = 0.17, a = 0.17 * edgeDamp;
        float ph = dot(wxz, d) * k + uTime * 1.15;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }
    {
        vec2 d = vec2(-0.64, 0.77);
        float k = 0.27, a = 0.11 * edgeDamp;
        float ph = dot(wxz, d) * k + uTime * 1.48;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }
    {
        vec2 d = vec2(0.31, -0.95);
        float k = 0.44, a = 0.06 * edgeDamp;
        float ph = dot(wxz, d) * k - uTime * 1.90;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }
    {
        vec2 d = vec2(-0.92, -0.39);
        float k = 0.72, a = 0.035 * edgeDamp;
        float ph = dot(wxz, d) * k + uTime * 2.35;
        h += sin(ph) * a;
        grad += d * (cos(ph) * a * k);
    }
    // Дополнительные короткие геометрические волны для тесселированной ближней глади
    if (nearChop > 0.005) {
        {
            vec2 d = vec2(0.55, -0.83);
            float k = 1.28, a = 0.022 * nearChop;
            float ph = dot(wxz, d) * k + uTime * 2.95;
            h += sin(ph) * a;
            grad += d * (cos(ph) * a * k);
        }
        {
            vec2 d = vec2(-0.78, -0.62);
            float k = 1.85, a = 0.014 * nearChop;
            float ph = dot(wxz, d) * k - uTime * 3.40;
            h += sin(ph) * a;
            grad += d * (cos(ph) * a * k);
        }
    }

    vec3 worldPos = vec3(wxz.x, uWaterY + h, wxz.y);
    vWorldPos = worldPos;
    vWaveGrad = grad;
    vWaveH = h;
    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)";

#define DEEPDIVE_WATER_FRAG_BODY R"(
in vec3 vWorldPos;
in vec2 vWaveGrad;
in float vWaveH;

uniform sampler2D uRockTex;
uniform sampler2D uSceneDepthTex;
uniform vec2 uInvResolution;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uFogScale;
uniform float uTime;
uniform vec4 uWhaleSurfA;
uniform vec4 uWhaleSurfB;

out vec4 FragColor;

float linearizeDepth(float d) {
    float zNear = 0.25;
    float zFar  = 460.0;
    float z_n = 2.0 * clamp(d, 0.0, 1.0) - 1.0;
    return (2.0 * zNear * zFar) / max(zFar + zNear - z_n * (zFar - zNear), 0.0001);
}

vec3 computeUnderwaterFogColor(vec3 rayDir, vec3 L, float heightFactor, float abyssMask) {
    float viewUp = clamp(rayDir.y * 0.5 + 0.5, 0.0, 1.0);
    float upBlend = clamp(pow(viewUp, 1.15) * 0.70 + heightFactor * 0.34, 0.0, 1.0);
    vec3 fogCol = mix(uWaterColorDeep * 0.86, uWaterColorShallow * 1.14, upBlend);

    float sunDot = clamp(dot(rayDir, L), 0.0, 1.0);
    float sunGlow = (sunDot * sunDot * sunDot) * 0.38;
    float zenithGlow = smoothstep(0.04, 0.85, rayDir.y) * 0.24 * heightFactor;
    fogCol += uSunColor * (sunGlow + zenithGlow) * (1.0 - abyssMask * 0.92);

    return mix(fogCol, vec3(0.004, 0.014, 0.028), abyssMask * 0.92);
}

void main() {
    vec3 toCam = uCamPos - vWorldPos;
    float dist = length(toCam);
    vec3 V = dist > 0.0001 ? (toCam / dist) : vec3(0.0, -1.0, 0.0);
    vec3 rayDir = -V;
    vec3 L = normalize(uSunDir);

    vec2 p = vWorldPos.xz;
    mat2 rot1 = mat2(0.80, -0.60, 0.60, 0.80);
    mat2 rot2 = mat2(-0.60, -0.80, 0.80, -0.60);

    vec4 s1 = texture(uRockTex, p * 0.055 + vec2(uTime * 0.026, -uTime * 0.019));
    vec4 s2 = texture(uRockTex, (rot1 * p) * 0.135 + vec2(-uTime * 0.034, uTime * 0.028));
    vec4 s3 = texture(uRockTex, (rot2 * p) * 0.310 + vec2(uTime * 0.052, uTime * 0.044));

    float fineWeight = exp(-dist * 0.032);
    vec4 s4 = (fineWeight > 0.02)
                  ? texture(uRockTex, (rot1 * p) * 0.72 + vec2(-uTime * 0.085, uTime * 0.072))
                  : s3;

    vec2 screenUV = clamp(gl_FragCoord.xy * uInvResolution, 0.001, 0.999);
    float rawSceneDepth = texture(uSceneDepthTex, screenUV).r;
    float waterLin = linearizeDepth(gl_FragCoord.z);
    float sceneLin = linearizeDepth(rawSceneDepth);

    float shoreFoam = 0.0;
    float shoreBubbles = 0.0;
    vec2 shoreRippleGrad = vec2(0.0);

    float contactDist3D = 1e6;
    vec2 contactOriginXZ = p;
    bool hasContact = false;

    if (rawSceneDepth < 0.9992 && sceneLin >= waterLin - 0.08) {
        float ratio = sceneLin / max(waterLin, 0.001);
        vec3 rockPos = uCamPos + (vWorldPos - uCamPos) * ratio;
        contactDist3D = length(rockPos - vWorldPos);
        contactOriginXZ = rockPos.xz;
        hasContact = true;
    }

    float whaleScale = uWhaleSurfA.w;
    if (whaleScale > 0.1) {
        vec3 dW = vWorldPos - uWhaleSurfA.xyz;
        float maxR = 6.5 * whaleScale + 5.0;
        if (dot(dW, dW) < maxR * maxR) {
            float sy = uWhaleSurfB.x, cy = uWhaleSurfB.y;
            float sp = uWhaleSurfB.z, cp = uWhaleSurfB.w;
            vec3 r1 = vec3(cy * dW.x - sy * dW.z, dW.y, sy * dW.x + cy * dW.z);
            vec3 lp = vec3(r1.x, cp * r1.y - sp * r1.z, sp * r1.y + cp * r1.z) / whaleScale;

            float zClamped = clamp(lp.z, -5.05, 4.95);
            float tBody = clamp((4.95 - zClamped) / 10.05, 0.0, 1.0);
            float rx, ry;
            if (tBody < 0.34) {
                float u = tBody / 0.34;
                float s = sin(u * 1.5707963);
                rx = 1.24 * pow(max(s, 0.001), 0.80);
                ry = (lp.y >= 0.0 ? 0.88 : 1.12) * pow(max(s, 0.001), 0.85);
            } else {
                float u = (tBody - 0.34) / 0.66;
                rx = 1.24 * pow(max(1.0 - u, 0.0), 1.55) + 0.09 * (1.0 - u);
                ry = (lp.y >= 0.0 ? 0.88 : 1.12) * pow(max(1.0 - u, 0.0), 1.25) + 0.13 * (1.0 - u);
            }

            float aboveBack  = (lp.y - ry) * whaleScale;
            float belowBelly = (-ry - lp.y) * whaleScale;
            float pierceFade = (1.0 - smoothstep(0.0, 0.85, aboveBack))
                             * (1.0 - smoothstep(0.0, 0.85, belowBelly));
            if (pierceFade > 0.005) {
                float sliceFrac = clamp(lp.y / max(ry, 0.05), -0.99, 0.99);
                float halfW = rx * sqrt(max(1.0 - sliceFrac * sliceFrac, 0.0));
                float dxOut = max(abs(lp.x) - halfW, 0.0);
                float dzEnd = lp.z - zClamped;
                float whaleDist = length(vec2(dxOut, dzEnd)) * whaleScale + (1.0 - pierceFade) * 4.5;
                if (whaleDist < contactDist3D) {
                    contactDist3D = whaleDist;
                    contactOriginXZ = uWhaleSurfA.xz + vec2(sy, cy) * (cp * zClamped * whaleScale);
                    hasContact = true;
                }
            }
        }
    }

    if (hasContact) {
        float dist3D = contactDist3D;
        float shoreZone = 1.0 - smoothstep(0.0, 4.2, dist3D);
        float tightSeam = 1.0 - smoothstep(0.0, 1.15, dist3D);

        if (shoreZone > 0.005) {
            vec4 f1 = texture(uRockTex, p * 0.36 + vec2(uTime * 0.035, -uTime * 0.028));
            vec4 f2 = texture(uRockTex, (rot1 * p) * 0.95 + vec2(-uTime * 0.062, uTime * 0.048));
            vec4 f3 = texture(uRockTex, (rot2 * p) * 2.15 + vec2(uTime * 0.095, uTime * 0.075));

            float swirl = f1.a * 0.58 + f2.a * 0.42;
            float ripplePhase = dist3D * 6.4 - uTime * 3.6 + swirl * 4.8;
            float rippleRing = sin(ripplePhase) * 0.5 + 0.5;

            vec2 radialXZ = p - contactOriginXZ;
            float radLen = length(radialXZ);
            vec2 rDir = (radLen > 0.01) ? (radialXZ / radLen) : vec2(1.0, 0.0);
            shoreRippleGrad = rDir * (cos(ripplePhase) * 0.38 * shoreZone);

            float laceMask = smoothstep(0.46 - tightSeam * 0.34, 0.74 - tightSeam * 0.24, swirl) * shoreZone;
            float ringMask = smoothstep(0.42, 0.92, rippleRing) * shoreZone * smoothstep(0.28, 0.68, f1.a) * 0.68;
            shoreBubbles = smoothstep(0.72, 0.90, f3.a) * shoreZone * (0.65 + 0.65 * tightSeam);
            shoreFoam = clamp(tightSeam * (0.65 + 0.35 * f2.a) + laceMask * 0.85 + ringMask + shoreBubbles * 0.75, 0.0, 1.0);
        }
    }

    vec2 texSlope = (s1.rg * 2.0 - 1.0) * 0.32
                  + (s2.rg * 2.0 - 1.0) * 0.26
                  + (s3.rg * 2.0 - 1.0) * (0.18 * (0.35 + 0.65 * fineWeight))
                  + (s4.rg * 2.0 - 1.0) * (0.14 * fineWeight);

    vec2 totalSlope = vWaveGrad * 1.90 + texSlope * 0.85 + shoreRippleGrad;
    vec3 N = normalize(vec3(totalSlope.x, -1.0, totalSlope.y));

    float waveCrest = clamp(0.5 + vWaveH * 1.15 + (s1.a - 0.5) * 0.45 + (s2.a - 0.5) * 0.35, 0.0, 1.0);
    float causticNet = clamp(1.0 - abs(s1.a - s2.a) * 2.3, 0.0, 1.0);
    causticNet = causticNet * causticNet * causticNet * causticNet;

    const float eta = 1.3333;
    float cosI = clamp(dot(N, V), 0.0, 1.0);
    float sin2I = max(0.0, 1.0 - cosI * cosI);
    float sin2T = eta * eta * sin2I;

    vec3 R = reflect(rayDir, N);
    float downF = clamp(-R.y, 0.0, 1.0);
    vec3 deepMirror = mix(uWaterColorShallow * 0.92, uWaterColorDeep * 0.64, pow(downF, 0.72));

    float slopeLight = clamp(dot(vec3(totalSlope.x, 0.42, totalSlope.y), L) * 0.95 + 0.45, 0.0, 1.0);
    vec3 tirCol = mix(deepMirror * 0.65,
                      deepMirror * 1.35 + uWaterColorShallow * 0.32,
                      smoothstep(0.18, 0.82, slopeLight * 0.60 + waveCrest * 0.40));

    vec3 H = normalize(L - rayDir + vec3(0.0001));
    float specTIR = pow(clamp(dot(-N, H), 0.0, 1.0), 34.0);
    vec3 sunHoriz = normalize(vec3(L.x, 0.35, L.z));
    float sunAzimuthGlow = pow(clamp(dot(normalize(vec3(rayDir.x, 0.25, rayDir.z)), sunHoriz), 0.0, 1.0), 2.5);
    tirCol += uSunColor * (specTIR * 0.75 + causticNet * 0.28 + sunAzimuthGlow * 0.30);

    float fresnel = 1.0;
    vec3 refractedCol = tirCol;

    if (sin2T < 0.999) {
        float cosT = sqrt(max(1.0 - sin2T, 0.0));
        float rs = (eta * cosI - cosT) / max(eta * cosI + cosT, 0.0001);
        float rp = (cosI - eta * cosT) / max(cosI + eta * cosT, 0.0001);
        fresnel = clamp(0.5 * (rs * rs + rp * rp), 0.02, 1.0);

        vec3 T = normalize(eta * rayDir + (eta * cosI - cosT) * N);
        float skyElev = clamp(T.y, 0.0, 1.0);

        vec3 zenithCol = mix(uWaterColorShallow * 1.15, vec3(0.46, 0.92, 1.10), 0.70);
        vec3 rimSkyCol = mix(uWaterColorShallow * 1.25, uSunColor * 1.22, 0.56);
        refractedCol = mix(rimSkyCol, zenithCol, pow(skyElev, 0.58));

        float sunDot = clamp(dot(T, L), 0.0, 1.0);
        float s2 = sunDot * sunDot;
        float s4 = s2 * s2;
        float s16 = s4 * s4 * s4 * s4;
        float s64 = s16 * s16 * s16 * s16;
        refractedCol += uSunColor * (s4 * 0.36 + s16 * 0.85 + s64 * 1.85);

        refractedCol *= (0.76 + 0.44 * waveCrest);
        refractedCol += uSunColor * causticNet * 0.36 * (0.45 + 0.55 * fineWeight);
    }

    vec3 surfaceCol = mix(refractedCol, tirCol, fresnel);

    float criticalBand = exp(-pow((sin2T - 0.98) / 0.14, 2.0));
    surfaceCol += mix(uWaterColorShallow * 1.15, uSunColor * 1.25, 0.55) * criticalBand * (0.28 + 0.36 * causticNet);

    vec3 foamColor = mix(uWaterColorShallow * 1.35, vec3(0.96, 1.04, 1.08), 0.76) * mix(vec3(1.0), uSunColor, 0.45);
    surfaceCol = mix(surfaceCol, foamColor, shoreFoam * 0.86);
    surfaceCol += vec3(0.95, 1.08, 1.12) * shoreBubbles * 0.55;

    float camAbyss = smoothstep(-20.0, -54.0, uCamPos.y);
    float fogDensity = mix(0.0128 * uFogScale, 0.0200, camAbyss);
    vec3 transmittance = exp(-dist * fogDensity * vec3(1.85, 1.06, 0.86));

    float camHeightFactor = clamp((uCamPos.y + 34.0) / 62.0, 0.06, 1.0);
    vec3 fogColor = computeUnderwaterFogColor(rayDir, L, camHeightFactor, camAbyss);

    vec3 outCol = surfaceCol * transmittance + fogColor * (1.0 - transmittance);
    FragColor = vec4(clamp(outCol, 0.0, 8.0), 1.0);
}
)"

inline const char* WaterSurfaceFrag =
    "#version 330 core\n"
    DEEPDIVE_WATER_FRAG_BODY;

inline const char* WaterSurfaceFrag400 =
    "#version 400 core\n"
    DEEPDIVE_WATER_FRAG_BODY;

#undef DEEPDIVE_ENV_FRAG_PART1
#undef DEEPDIVE_ENV_FRAG_PART2
#undef DEEPDIVE_WATER_FRAG_BODY

// ============================================================================
// SKYBOX
// ============================================================================

inline const char* SkyboxVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vNDC;
void main() {
    vNDC = aPos;
    gl_Position = vec4(aPos, 0.9999, 1.0);
}
)";

inline const char* SkyboxFrag = R"(#version 330 core
in vec2 vNDC;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;

out vec4 FragColor;

void main() {
    vec4 worldTarget = uInvViewProj * vec4(vNDC, 1.0, 1.0);
    vec3 dir = normalize(worldTarget.xyz / max(abs(worldTarget.w), 0.0001) - uCamPos);
    vec3 L = normalize(uSunDir);

    float camAbyss = smoothstep(-20.0, -54.0, uCamPos.y);
    float camHeightFactor = clamp((uCamPos.y + 34.0) / 62.0, 0.06, 1.0);

    float viewUp = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    float upBlend = clamp(pow(viewUp, 1.15) * 0.70 + camHeightFactor * 0.34, 0.0, 1.0);
    vec3 col = mix(uWaterColorDeep * 0.86, uWaterColorShallow * 1.14, upBlend);

    float sunDot = clamp(dot(dir, L), 0.0, 1.0);
    float sunGlow = (sunDot * sunDot * sunDot) * 0.38;
    float zenithGlow = smoothstep(0.04, 0.85, dir.y) * 0.24 * camHeightFactor;
    col += uSunColor * (sunGlow + zenithGlow) * (1.0 - camAbyss * 0.92);
    col = mix(col, vec3(0.004, 0.014, 0.028), camAbyss * 0.92);

    FragColor = vec4(clamp(col, 0.0, 6.0), 1.0);
}
)";

// ============================================================================
// ЛУЧИ, ЧАСТИЦЫ И ПОСТПРОДАКШЕН
// ============================================================================

inline const char* TranslucentVert = R"(#version 330 core
layout(location = 0) in vec3 aLocalPos;
layout(location = 1) in vec4 aInstTop;    // xyz: topCenter, w: rTop
layout(location = 2) in vec4 aInstBot;    // xyz: botCenter, w: rBot
layout(location = 3) in vec4 aInstParams; // x: intensity, y: phase, z: freqScale

uniform mat4 uViewProj;

out vec3 vWorldPos;
flat out vec3 vTopCenter;
flat out vec3 vAxis;
flat out float vLength;
flat out vec2 vRadii;
flat out vec3 vParams;

void main() {
    vec3 topC = aInstTop.xyz;
    vec3 botC = aInstBot.xyz;
    vec3 axisVec = botC - topC;
    float len = max(length(axisVec), 0.001);
    vec3 A = axisVec / len;

    vec3 ref = (abs(A.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 U = normalize(cross(A, ref));
    vec3 W = cross(U, A);

    float hNorm = mix(-0.02, 1.02, aLocalPos.y);
    float rProxy = mix(aInstTop.w, aInstBot.w, clamp(aLocalPos.y, 0.0, 1.0)) * 1.04;

    vec3 worldPos = topC
                  + A * (hNorm * len)
                  + (U * aLocalPos.x + W * aLocalPos.z) * rProxy;

    vWorldPos  = worldPos;
    vTopCenter = topC;
    vAxis      = A;
    vLength    = len;
    vRadii     = vec2(aInstTop.w, aInstBot.w);
    vParams    = aInstParams.xyz;

    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)";

inline const char* TranslucentFrag = R"(#version 330 core
in vec3 vWorldPos;
flat in vec3 vTopCenter;
flat in vec3 vAxis;
flat in float vLength;
flat in vec2 vRadii;
flat in vec3 vParams;

uniform sampler2D uRockTex;
uniform sampler2D uSceneDepthTex;
uniform sampler2D uShadowMap;
uniform mat4 uLightSpaceMatrix;
uniform vec2 uInvResolution;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColorShallow;
uniform float uFogScale;
uniform float uTime;
uniform int uEnableShadows;

out vec4 FragColor;

float linearizeDepth(float d) {
    float zNear = 0.25;
    float zFar  = 460.0;
    float z_n = 2.0 * clamp(d, 0.0, 1.0) - 1.0;
    return (2.0 * zNear * zFar) / max(zFar + zNear - z_n * (zFar - zNear), 0.0001);
}

void main() {
    vec3 viewVec = vWorldPos - uCamPos;
    float fragDist = length(viewVec);
    if (fragDist < 0.001) discard;
    vec3 D = viewVec / fragDist;

    vec2 screenUV = clamp(gl_FragCoord.xy * uInvResolution, 0.001, 0.999);
    float rawSceneDepth = texture(uSceneDepthTex, screenUV).r;
    float tScene = 1e5;
    if (rawSceneDepth < 0.9996) {
        float fragLin  = linearizeDepth(gl_FragCoord.z);
        float sceneLin = linearizeDepth(rawSceneDepth);
        tScene = fragDist * (sceneLin / max(fragLin, 0.0001));
    }

    vec3 relO = uCamPos - vTopCenter;
    float h0 = dot(relO, vAxis);
    float da = dot(D, vAxis);

    float s0 = 0.0;
    float s1 = tScene;
    if (abs(da) > 1e-5) {
        float tA = (0.0 - h0) / da;
        float tB = (vLength - h0) / da;
        s0 = max(s0, min(tA, tB));
        s1 = min(s1, max(tA, tB));
    } else if (h0 < 0.0 || h0 > vLength) {
        discard;
    }
    if (s1 <= s0) discard;

    float k  = (vRadii.y - vRadii.x) / max(vLength, 0.001);
    float r0 = vRadii.x + k * h0;
    float rd = k * da;

    vec3 q0 = relO - h0 * vAxis;
    vec3 dp = D - da * vAxis;

    float A = dot(dp, dp) - rd * rd;
    float B = dot(q0, dp) - r0 * rd;
    float C = dot(q0, q0) - r0 * r0;

    float t0 = s0;
    float t1 = s1;

    if (A > 1e-5) {
        float disc = B * B - A * C;
        if (disc <= 0.0) discard;
        float sq = sqrt(disc);
        t0 = max(s0, (-B - sq) / A);
        t1 = min(s1, (-B + sq) / A);
    } else if (A < -1e-5) {
        float f0 = (A * s0 + 2.0 * B) * s0 + C;
        float f1 = (A * s1 + 2.0 * B) * s1 + C;
        if (f0 > 0.0 && f1 > 0.0) discard;
        if (f0 > 0.0 || f1 > 0.0) {
            float disc = max(B * B - A * C, 0.0);
            float sq = sqrt(disc);
            if (f0 <= 0.0) t1 = min(s1, (-B + sq) / A);
            else           t0 = max(s0, (-B - sq) / A);
        }
    } else {
        if (abs(B) > 1e-5) {
            float tRoot = -C / (2.0 * B);
            if (B > 0.0) t1 = min(s1, tRoot);
            else         t0 = max(s0, tRoot);
        } else if (C > 0.0) {
            discard;
        }
    }

    float segLen = t1 - t0;
    if (segLen <= 0.01) discard;

    const int STEPS = 10;
    float dt = segLen / float(STEPS);
    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

    vec3 L = normalize(uSunDir);
    float cosSun = dot(D, L);
    float phase = 0.72 + 0.48 * pow(clamp(cosSun * 0.5 + 0.5, 0.0, 1.0), 2.2);

    float normScale = 1.0 / max(vRadii.x + vRadii.y, 1.0);
    float accum = 0.0;

    for (int i = 0; i < STEPS; ++i) {
        float t = t0 + (float(i) + ign) * dt;
        vec3 p = uCamPos + D * t;

        float h = h0 + t * da;
        float v = clamp(h / max(vLength, 0.001), 0.0, 1.0);
        float radLocal = max(vRadii.x + k * h, 0.05);

        vec3 q = q0 + t * dp;
        float r2 = clamp(dot(q, q) / (radLocal * radLocal), 0.0, 1.0);

        float oneMinusR2 = 1.0 - r2;
        float radial = oneMinusR2 * oneMinusR2;

        float topFade   = smoothstep(0.0, 0.08, v);
        float botFade   = 1.0 - smoothstep(0.72, 1.0, v);
        float depthExp  = exp(-v * 1.35);
        float abyssFade = 1.0 - smoothstep(-18.0, -44.0, p.y);

        vec2 sunProj = p.xz - p.y * (L.xz / max(L.y, 0.35));
        float fScale = vParams.z;
        float n1 = texture(uRockTex, sunProj * (0.042 * fScale) + vec2(uTime * 0.018, -uTime * 0.013)).a;
        float n2 = texture(uRockTex, sunProj * (0.095 * fScale) + vec2(-uTime * 0.024, uTime * 0.020) + vec2(vParams.y)).a;
        float filaments = 0.50 + 0.82 * smoothstep(0.24, 0.80, n1 * 0.62 + n2 * 0.38);

        float mist = 0.80 + 0.20 * sin(h * 0.22 - uTime * 0.65 + vParams.y * 6.2831 + n1 * 3.0);

        float shadowVis = 1.0;
        if (uEnableShadows == 1) {
            vec4 lsPos = uLightSpaceMatrix * vec4(p, 1.0);
            vec3 proj = lsPos.xyz / max(lsPos.w, 0.0001);
            proj = proj * 0.5 + 0.5;
            if (proj.x > 0.002 && proj.x < 0.998 && proj.y > 0.002 && proj.y < 0.998 && proj.z < 0.998) {
                vec2 edge = smoothstep(vec2(0.0), vec2(0.08), proj.xy)
                          * (1.0 - smoothstep(vec2(0.92), vec2(1.0), proj.xy));
                float mapD = texture(uShadowMap, proj.xy).r;
                float occ = (proj.z - 0.0035 > mapD) ? 0.08 : 1.0;
                shadowVis = mix(1.0, occ, edge.x * edge.y);
            }
        }

        float viewFog  = exp(-t * 0.0115 * uFogScale);
        float nearFade = smoothstep(0.5, 4.0, t);

        accum += radial * topFade * botFade * depthExp * abyssFade * filaments * mist * shadowVis * viewFog * nearFade * dt;
    }

    float density = accum * normScale * vParams.x * phase;
    if (density < 0.0005) discard;

    float alpha = 0.34 * (1.0 - exp(-density * 2.8));
    vec3 beamCol = mix(uWaterColorShallow * 1.15, uSunColor * 1.28, 0.72);
    FragColor = vec4(clamp(beamCol, 0.0, 3.0), alpha);
}
)";

inline const char* ParticleVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColorSize;

uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform float uViewportHeight;

out vec3 vColor;
out float vType;

void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
    float dist = max(length(uCamPos - aPos), 1.0);
    float size = abs(aColorSize.a);
    gl_PointSize = clamp((size * uViewportHeight * 0.6) / dist, 1.5, 42.0);
    vColor = aColorSize.rgb;
    vType = aColorSize.a < 0.0 ? 1.0 : 0.0;
}
)";

inline const char* ParticleFrag = R"(#version 330 core
in vec3 vColor;
in float vType;
out vec4 FragColor;

void main() {
    vec2 uv = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(uv, uv);
    if (r2 > 1.0) discard;

    if (vType > 0.5) {
        float rim = smoothstep(0.45, 0.92, r2) * (1.0 - smoothstep(0.92, 1.0, r2));
        float hBase = max(0.0, 1.0 - length(uv - vec2(-0.35, -0.35)) * 1.8);
        float highlight = hBase * hBase * hBase;
        float alpha = clamp(rim * 0.75 + highlight * 0.9, 0.05, 0.95);
        FragColor = vec4(clamp(vColor + vec3(highlight * 0.55), 0.0, 3.0), alpha);
    } else {
        float oneMinusR2 = clamp(1.0 - r2, 0.0, 1.0);
        float glow = oneMinusR2 * oneMinusR2;
        FragColor = vec4(clamp(vColor * 1.25, 0.0, 3.0), glow * 0.85);
    }
}
)";

inline const char* PostQuadVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

inline const char* ScatterBlurFrag = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uSourceTex;
uniform vec2 uTexelStep;
uniform int uExtractBright;

out vec4 FragColor;

vec3 sanitizeHDR(vec3 c) {
    if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);
    return clamp(c, vec3(0.0), vec3(10.0));
}

vec3 extractScatteredLight(vec3 c) {
    vec3 safeC = sanitizeHDR(c);
    float lum = dot(safeC, vec3(0.2126, 0.7152, 0.0722));
    float softKnee = smoothstep(0.40, 1.25, lum);
    return safeC * (0.12 + softKnee * 0.95);
}

void main() {
    const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec3 sum = sanitizeHDR(texture(uSourceTex, vUV).rgb);
    if (uExtractBright == 1) sum = extractScatteredLight(sum);
    sum *= weights[0];

    for (int i = 1; i < 5; ++i) {
        vec2 off = uTexelStep * float(i) * 1.75;
        vec3 c1 = sanitizeHDR(texture(uSourceTex, clamp(vUV + off, 0.001, 0.999)).rgb);
        vec3 c2 = sanitizeHDR(texture(uSourceTex, clamp(vUV - off, 0.001, 0.999)).rgb);
        if (uExtractBright == 1) {
            c1 = extractScatteredLight(c1);
            c2 = extractScatteredLight(c2);
        }
        sum += (c1 + c2) * weights[i];
    }

    FragColor = vec4(sanitizeHDR(sum), 1.0);
}
)";

inline const char* PostProcessFrag = R"(#version 330 core
in vec2 vUV;

uniform sampler2D uSceneColorTex;
uniform sampler2D uSceneDepthTex;
uniform sampler2D uBloomTex;

uniform vec2 uInvResolution;
uniform vec3 uSunScreenPos;
uniform vec3 uWaterTint;
uniform float uSurfaceProximity;
uniform float uTime;
uniform int uEnablePost;

out vec4 FragColor;

vec3 sanitizeHDR(vec3 c) {
    if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);
    return clamp(c, vec3(0.0), vec3(10.0));
}

float linearizeDepth(float d) {
    float zNear = 0.25;
    float zFar  = 460.0;
    float z_n = 2.0 * clamp(d, 0.0, 1.0) - 1.0;
    return (2.0 * zNear * zFar) / max(zFar + zNear - z_n * (zFar - zNear), 0.0001);
}

vec3 acesToneMapping(vec3 x) {
    vec3 s = sanitizeHDR(x);
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((s * (a * s + b)) / (s * (c * s + d) + e), 0.0, 1.0);
}

void main() {
    if (uEnablePost == 0) {
        FragColor = vec4(clamp(sanitizeHDR(texture(uSceneColorTex, vUV).rgb), 0.0, 1.0), 1.0);
        return;
    }

    vec2 waveOffset = vec2(
        sin(vUV.y * 13.0 + uTime * 1.45) * cos(vUV.x * 10.0 + uTime * 0.95),
        cos(vUV.x * 12.0 - uTime * 1.15) * sin(vUV.y * 11.0 + uTime * 0.85)
    ) * 0.0012;
    vec2 uv = clamp(vUV + waveOffset, 0.001, 0.999);

    float rawDepth = texture(uSceneDepthTex, uv).r;
    float linDepth = linearizeDepth(rawDepth);
    float turbidityBlur = clamp((linDepth - 14.0) / 90.0, 0.05, 1.0);

    vec2 edgeVec = uv - 0.5;
    float radialDist = length(edgeVec);
    vec2 caShift = edgeVec * (0.0030 * radialDist);

    vec3 centerCol;
    centerCol.r = sanitizeHDR(texture(uSceneColorTex, clamp(uv + caShift, 0.001, 0.999)).rgb).r;
    centerCol.g = sanitizeHDR(texture(uSceneColorTex, uv).rgb).g;
    centerCol.b = sanitizeHDR(texture(uSceneColorTex, clamp(uv - caShift, 0.001, 0.999)).rgb).b;

    vec2 blurStep = uInvResolution * (1.2 + turbidityBlur * 2.4);
    vec3 ringCol = (
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2( blurStep.x,  blurStep.y), 0.001, 0.999)).rgb) +
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2(-blurStep.x,  blurStep.y), 0.001, 0.999)).rgb) +
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2( blurStep.x, -blurStep.y), 0.001, 0.999)).rgb) +
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2(-blurStep.x, -blurStep.y), 0.001, 0.999)).rgb)
    ) * 0.25;

    vec3 sceneCol = mix(centerCol, ringCol, turbidityBlur * 0.46);

    vec3 scatteredBloom = sanitizeHDR(texture(uBloomTex, uv).rgb);
    sceneCol = mix(sceneCol, scatteredBloom, 0.16) + scatteredBloom * 0.34;

    if (uSunScreenPos.z > 0.0) {
        vec2 sunUV = uSunScreenPos.xy;
        vec2 deltaUV = (uv - sunUV);
        float sunDist = length(deltaUV);
        vec2 stepUV = deltaUV / 24.0 * min(1.0, 0.85 / max(sunDist, 0.001));

        vec2 sampleUV = uv;
        float illuminationDecay = 1.0;
        vec3 godRayAcc = vec3(0.0);

        for (int i = 0; i < 24; ++i) {
            sampleUV -= stepUV;
            vec2 clampedUV = clamp(sampleUV, 0.002, 0.998);
            vec3 sColor = sanitizeHDR(texture(uBloomTex, clampedUV).rgb);
            godRayAcc += sColor * illuminationDecay;
            illuminationDecay *= 0.935;
        }

        float rayFade = clamp(uSunScreenPos.z, 0.0, 1.0) * exp(-sunDist * 1.05);
        sceneCol += godRayAcc * (0.044 * rayFade);
    }

    float topGrad = clamp(vUV.y, 0.0, 1.0);
    sceneCol += uWaterTint * (topGrad * topGrad) * (0.055 + 0.055 * clamp(uSurfaceProximity, 0.0, 1.0));

    vec3 mapped = acesToneMapping(sceneCol * 1.18);
    mapped = pow(max(mapped, vec3(0.0001)), vec3(0.96));

    float vignette = smoothstep(1.02, 0.28, radialDist);
    mapped *= mix(0.60, 1.0, vignette);

    FragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
)";

} // namespace Shaders
