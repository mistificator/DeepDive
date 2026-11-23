#pragma once

namespace Shaders {

// ============================================================================
// SHADOW MAP PASS SHADERS (OpenGL 3.3 Core)
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
// ENVIRONMENT SHADERS (Terrain, Static Props, Instanced Props)
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

// Shared body for GLSL 330 and GLSL 400 environment fragment shaders (split for MSVC literal limit)
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

#undef DEEPDIVE_ENV_FRAG_PART1
#undef DEEPDIVE_ENV_FRAG_PART2

// ============================================================================
// HARDWARE TESSELLATION SHADERS (OpenGL 4.0 Core — Terrain LOD0 + Rock LOD0)
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
    // Fade tessellation down to 1.0 before the LOD0->LOD1 boundary to avoid cracks
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
out vec3 vSwayInfo_TCS; // xy: instance root XZ, z: local aPos.x

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

    vPos_TCS      = rot * localPos + aInstPosScaleXZ.xyz;
    vNormal_TCS   = normalize(rot * localN);
    vColor_TCS    = col;
    vParam_TCS    = aParam;
    vSwayInfo_TCS = vec3(aInstPosScaleXZ.x, aInstPosScaleXZ.z, aPos.x);
}
)";

inline const char* InstPropTessControl = R"(#version 400 core
layout(vertices = 3) out;

in vec3 vPos_TCS[];
in vec3 vNormal_TCS[];
in vec3 vColor_TCS[];
in vec2 vParam_TCS[];
in vec3 vSwayInfo_TCS[];

out vec3 vPos_TES[];
out vec3 vNormal_TES[];
out vec3 vColor_TES[];
out vec2 vParam_TES[];
out vec3 vSwayInfo_TES[];

uniform vec3 uCamPos;
uniform float uMaxTessLevel;

float calcEdgeTess(vec3 pA, vec3 pB, bool isFlora) {
    vec3 mid = 0.5 * (pA + pB);
    float dist = length(uCamPos - mid);
    float nearD = isFlora ? 8.0  : 14.0;
    float farD  = isFlora ? 62.0 : 115.0;
    float capL  = isFlora ? 4.0  : 5.0;
    float t = 1.0 - smoothstep(nearD, farD, dist);
    float maxL = min(uMaxTessLevel, capL);
    return mix(1.0, maxL, t * t);
}

void main() {
    vPos_TES[gl_InvocationID]      = vPos_TCS[gl_InvocationID];
    vNormal_TES[gl_InvocationID]   = vNormal_TCS[gl_InvocationID];
    vColor_TES[gl_InvocationID]    = vColor_TCS[gl_InvocationID];
    vParam_TES[gl_InvocationID]    = vParam_TCS[gl_InvocationID];
    vSwayInfo_TES[gl_InvocationID] = vSwayInfo_TCS[gl_InvocationID];

    if (gl_InvocationID == 0) {
        bool isFlora = max(vParam_TCS[0].x, max(vParam_TCS[1].x, vParam_TCS[2].x)) > 1.5;
        float e0 = calcEdgeTess(vPos_TCS[1], vPos_TCS[2], isFlora);
        float e1 = calcEdgeTess(vPos_TCS[2], vPos_TCS[0], isFlora);
        float e2 = calcEdgeTess(vPos_TCS[0], vPos_TCS[1], isFlora);

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
in vec3 vSwayInfo_TES[];

uniform sampler2D uRockTex;
uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform vec3 uCamPos;
uniform float uTime;

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

    vec3 pLin     = b.x * p0 + b.y * p1 + b.z * p2;
    vec3 norm     = safeNorm(b.x * n0 + b.y * n1 + b.z * n2);
    vec3 col      = b.x * vColor_TES[0] + b.y * vColor_TES[1] + b.z * vColor_TES[2];
    vec2 param    = b.x * vParam_TES[0] + b.y * vParam_TES[1] + b.z * vParam_TES[2];
    vec3 swayInfo = b.x * vSwayInfo_TES[0] + b.y * vSwayInfo_TES[1] + b.z * vSwayInfo_TES[2];

    float dist = length(uCamPos - pLin);
    float minDot = min(min(dot(n0, n1), dot(n1, n2)), dot(n2, n0));
    float creaseGuard = smoothstep(0.28, 0.72, minDot);

    vec3 pos = pLin;

    if (param.x < 1.5) {
        // Rocks & crags: Phong silhouette smoothing + triplanar displacement
        float fade = 1.0 - smoothstep(18.0, 105.0, dist);
        if (fade > 0.001) {
            vec3 q0 = pLin - dot(pLin - p0, n0) * n0;
            vec3 q1 = pLin - dot(pLin - p1, n1) * n1;
            vec3 q2 = pLin - dot(pLin - p2, n2) * n2;
            vec3 pPhong = b.x * q0 + b.y * q1 + b.z * q2;
            pos = mix(pLin, pPhong, 0.55 * fade * mix(0.25, 1.0, creaseGuard));

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
    } else {
        // Flora: gentle crease-safe Phong smoothing + smooth post-tessellation sway
        float fade = 1.0 - smoothstep(10.0, 56.0, dist);
        if (fade > 0.001 && creaseGuard > 0.001) {
            vec3 q0 = pLin - dot(pLin - p0, n0) * n0;
            vec3 q1 = pLin - dot(pLin - p1, n1) * n1;
            vec3 q2 = pLin - dot(pLin - p2, n2) * n2;
            vec3 pPhong = b.x * q0 + b.y * q1 + b.z * q2;
            pos = mix(pLin, pPhong, 0.42 * creaseGuard * fade);
        }

        if (param.x > 4.5 || (param.x > 2.5 && param.x < 3.5)) {
            float h = clamp(param.y, 0.0, 1.0);
            float phase = uTime * 1.10 + swayInfo.x * 0.11 + swayInfo.y * 0.09;
            float hCurve = h * sqrt(h);
            float swayX = sin(phase + h * 2.8) * 1.25 * hCurve;
            float swayZ = cos(phase * 0.82 + h * 3.8) * 0.78 * (h * h);
            float flutter = sin(uTime * 2.6 + pos.y * 0.9 + swayInfo.z * 1.4) * 0.12 * h;
            pos.x += swayX + flutter;
            pos.z += swayZ + flutter;
            norm = safeNorm(norm + vec3(swayX * 0.14, 0.0, swayZ * 0.14));
        } else if (param.x > 3.5) {
            float h = clamp(param.y, 0.0, 1.0);
            float phase = uTime * 1.45 + swayInfo.x * 0.24 + swayInfo.y * 0.22;
            float swayX = sin(phase + h * 2.4) * 0.24 * (h * h);
            float swayZ = cos(phase * 0.85 + h * 2.8) * 0.20 * (h * h);
            pos.x += swayX;
            pos.z += swayZ;
            norm = safeNorm(norm + vec3(swayX * 0.22, 0.0, swayZ * 0.22));
        } else if (param.x > 1.5 && param.y > 0.01) {
            float h = clamp(param.y, 0.0, 1.0);
            float phase = uTime * 1.85 + pos.x * 0.30 + pos.z * 0.26;
            float bend = h * sqrt(h);
            float swayX = sin(phase + h * 2.0) * 0.26 * bend;
            float swayZ = cos(phase * 0.84 + h * 2.4) * 0.22 * bend;
            pos.x += swayX;
            pos.z += swayZ;
            norm = safeNorm(norm + vec3(swayX * 0.20, 0.0, swayZ * 0.20));
        }
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
// MARINE FAUNA SHADERS (Fish, Manta Rays, Humpback Whale, Anglerfish)
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

#define DEEPDIVE_FAUNA_FRAG_BODY R"(
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

inline const char* FaunaFrag =
    "#version 330 core\n"
    DEEPDIVE_FAUNA_FRAG_BODY;

inline const char* FaunaFrag400 =
    "#version 400 core\n"
    DEEPDIVE_FAUNA_FRAG_BODY;

#undef DEEPDIVE_FAUNA_FRAG_BODY

// ============================================================================
// FAUNA HARDWARE TESSELLATION (OpenGL 4.0 Core — Local Phong + Smooth Swimming)
// ============================================================================

inline const char* FaunaTessVert = R"(#version 400 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec4 aInstPosScale;
layout(location = 4) in vec4 aInstRotPhase;  // x: yaw, y: phase, z: type, w: bank
layout(location = 5) in vec4 aInstPitchTint; // x: pitch, yzw: colorTint

out vec3 vLocalPos_TCS;
out vec3 vLocalNorm_TCS;
out vec3 vRawColor_TCS;
out vec4 vInstPosScale_TCS;
out vec4 vInstRotPhase_TCS;
out vec4 vInstPitchTint_TCS;

void main() {
    vLocalPos_TCS      = aPos;
    vLocalNorm_TCS     = aNormal;
    vRawColor_TCS      = aColor;
    vInstPosScale_TCS  = aInstPosScale;
    vInstRotPhase_TCS  = aInstRotPhase;
    vInstPitchTint_TCS = aInstPitchTint;
}
)";

inline const char* FaunaTessControl = R"(#version 400 core
layout(vertices = 3) out;

in vec3 vLocalPos_TCS[];
in vec3 vLocalNorm_TCS[];
in vec3 vRawColor_TCS[];
in vec4 vInstPosScale_TCS[];
in vec4 vInstRotPhase_TCS[];
in vec4 vInstPitchTint_TCS[];

out vec3 vLocalPos_TES[];
out vec3 vLocalNorm_TES[];
out vec3 vRawColor_TES[];

patch out vec4 pInstPosScale;
patch out vec4 pInstRotPhase;
patch out vec4 pInstPitchTint;
patch out mat3 pRot;

uniform vec3 uCamPos;
uniform float uMaxTessLevel;

float calcEdgeTess(vec3 wA, vec3 wB, float nearD, float farD, float capL) {
    vec3 mid = 0.5 * (wA + wB);
    float dist = length(uCamPos - mid);
    float t = 1.0 - smoothstep(nearD, farD, dist);
    float maxL = min(uMaxTessLevel, capL);
    return mix(1.0, maxL, t * t);
}

void main() {
    vLocalPos_TES[gl_InvocationID]  = vLocalPos_TCS[gl_InvocationID];
    vLocalNorm_TES[gl_InvocationID] = vLocalNorm_TCS[gl_InvocationID];
    vRawColor_TES[gl_InvocationID]  = vRawColor_TCS[gl_InvocationID];

    if (gl_InvocationID == 0) {
        vec4 instPosScale  = vInstPosScale_TCS[0];
        vec4 instRotPhase  = vInstRotPhase_TCS[0];
        vec4 instPitchTint = vInstPitchTint_TCS[0];

        float yaw   = instRotPhase.x;
        float pitch = instPitchTint.x;
        float bank  = instRotPhase.w;

        float cy = cos(yaw),   sy = sin(yaw);
        float cp = cos(pitch), sp = sin(pitch);
        float cb = cos(bank),  sb = sin(bank);

        mat3 rotY = mat3(cy, 0.0, -sy,  0.0, 1.0, 0.0,  sy, 0.0, cy);
        mat3 rotX = mat3(1.0, 0.0, 0.0,  0.0, cp, -sp,  0.0, sp, cp);
        mat3 rotZ = mat3(cb, sb, 0.0,   -sb, cb, 0.0,   0.0, 0.0, 1.0);
        mat3 rot  = rotY * rotX * rotZ;

        pInstPosScale  = instPosScale;
        pInstRotPhase  = instRotPhase;
        pInstPitchTint = instPitchTint;
        pRot           = rot;

        float nearD, farD, capL;
        if (instRotPhase.z < 0.5) {
            nearD = 4.0;  farD = 22.0;  capL = 3.0; // Reef fish (close-up only)
        } else if (instRotPhase.z < 1.5) {
            nearD = 10.0; farD = 75.0;  capL = 4.0; // Manta ray
        } else if (instRotPhase.z < 2.5) {
            nearD = 22.0; farD = 150.0; capL = 4.0; // Humpback whale
        } else {
            nearD = 10.0; farD = 65.0;  capL = 4.0; // Anglerfish
        }

        vec3 w0 = rot * (vLocalPos_TCS[0] * instPosScale.w) + instPosScale.xyz;
        vec3 w1 = rot * (vLocalPos_TCS[1] * instPosScale.w) + instPosScale.xyz;
        vec3 w2 = rot * (vLocalPos_TCS[2] * instPosScale.w) + instPosScale.xyz;

        float e0 = calcEdgeTess(w1, w2, nearD, farD, capL);
        float e1 = calcEdgeTess(w2, w0, nearD, farD, capL);
        float e2 = calcEdgeTess(w0, w1, nearD, farD, capL);

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = max(1.0, (e0 + e1 + e2) * 0.3333333);
    }
}
)";

inline const char* FaunaTessEval = R"(#version 400 core
layout(triangles, fractional_even_spacing, ccw) in;

in vec3 vLocalPos_TES[];
in vec3 vLocalNorm_TES[];
in vec3 vRawColor_TES[];

patch in vec4 pInstPosScale;
patch in vec4 pInstRotPhase;
patch in vec4 pInstPitchTint;
patch in mat3 pRot;

uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform vec3 uCamPos;
uniform float uTime;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec4 vLightSpacePos;
out float vEmissive;

vec3 safeNorm(vec3 v) {
    float l = length(v);
    return (l > 0.0001) ? (v / l) : vec3(0.0, 1.0, 0.0);
}

void main() {
    vec3 b = gl_TessCoord;

    vec3 p0 = vLocalPos_TES[0], p1 = vLocalPos_TES[1], p2 = vLocalPos_TES[2];
    vec3 n0 = safeNorm(vLocalNorm_TES[0]);
    vec3 n1 = safeNorm(vLocalNorm_TES[1]);
    vec3 n2 = safeNorm(vLocalNorm_TES[2]);

    vec3 pLin   = b.x * p0 + b.y * p1 + b.z * p2;
    vec3 localN = safeNorm(b.x * n0 + b.y * n1 + b.z * n2);
    vec3 rawCol = b.x * vRawColor_TES[0] + b.y * vRawColor_TES[1] + b.z * vRawColor_TES[2];

    // Smooth crease-guarded Phong interpolation in local model space before swimming animation
    vec3 approxWorld = pRot * (pLin * pInstPosScale.w) + pInstPosScale.xyz;
    float dist = length(uCamPos - approxWorld);
    float farFade = (pInstRotPhase.z < 0.5) ? 20.0
                  : (pInstRotPhase.z < 1.5) ? 70.0
                  : (pInstRotPhase.z < 2.5) ? 140.0 : 60.0;
    float fade = 1.0 - smoothstep(farFade * 0.25, farFade, dist);

    float minDot = min(min(dot(n0, n1), dot(n1, n2)), dot(n2, n0));
    float creaseGuard = smoothstep(0.32, 0.78, minDot);

    vec3 localPos = pLin;
    if (fade > 0.001 && creaseGuard > 0.001) {
        vec3 q0 = pLin - dot(pLin - p0, n0) * n0;
        vec3 q1 = pLin - dot(pLin - p1, n1) * n1;
        vec3 q2 = pLin - dot(pLin - p2, n2) * n2;
        vec3 pPhong = b.x * q0 + b.y * q1 + b.z * q2;
        localPos = mix(pLin, pPhong, 0.48 * creaseGuard * fade);
    }

    // Swimming / wing-flapping / lure animation evaluated on the tessellated surface
    if (pInstRotPhase.z < 0.5) {
        float phase = pInstRotPhase.y + uTime * 9.5;
        float tailWeight = smoothstep(0.35, -0.75, localPos.z);
        float wave = sin(phase - localPos.z * 3.8) * 0.14 * tailWeight;
        float dWaveDz = -3.8 * cos(phase - localPos.z * 3.8) * 0.14 * tailWeight;
        localPos.x += wave;
        localN = safeNorm(vec3(localN.x - dWaveDz * localN.z, localN.y, localN.z));
    } else if (pInstRotPhase.z < 1.5) {
        float phase = pInstRotPhase.y + uTime * 2.4;
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
        localN = safeNorm(vec3(localN.x - dFlapDx * localN.y,
                               localN.y,
                               localN.z - (dFlapDz + dTailDz) * localN.y));
    } else if (pInstRotPhase.z < 2.5) {
        float phase = pInstRotPhase.y + uTime * 1.35;
        float tailWeight = smoothstep(1.2, -6.4, localPos.z);
        float w2 = tailWeight * tailWeight;
        float waveArg = phase - localPos.z * 0.50;
        float spineWave = sin(waveArg) * 0.58 * w2;
        float dWaveDz = -0.50 * cos(waveArg) * 0.58 * w2;

        float flipperWeight = clamp((abs(localPos.x) - 1.30) / 2.8, 0.0, 1.0)
                            * smoothstep(-1.4, 0.2, localPos.z);
        float finSway = sin(phase - abs(localPos.x) * 0.55) * 0.16 * flipperWeight;

        localPos.y += spineWave + finSway;
        localN = safeNorm(vec3(localN.x, localN.y, localN.z - dWaveDz * localN.y));
    } else {
        float phase = pInstRotPhase.y + uTime * 3.2;
        float tailW = smoothstep(-0.1, -1.6, localPos.z);
        localPos.x += sin(phase - localPos.z * 2.2) * 0.16 * tailW;

        float finW = smoothstep(0.65, 1.25, abs(localPos.x));
        localPos.y += sin(uTime * 4.2 + pInstRotPhase.y) * 0.08 * finW;

        float lureW = smoothstep(0.45, 1.35, localPos.z) * smoothstep(0.15, 0.85, localPos.y);
        localPos.x += sin(uTime * 2.1 + pInstRotPhase.y) * 0.09 * lureW;
        localPos.y += cos(uTime * 1.7 + pInstRotPhase.y) * 0.07 * lureW;
    }

    vec3 worldPos  = pRot * (localPos * pInstPosScale.w) + pInstPosScale.xyz;
    vWorldPos      = worldPos;
    vNormal        = safeNorm(pRot * localN);
    float maxC     = max(rawCol.r, max(rawCol.g, rawCol.b));
    vEmissive      = smoothstep(1.20, 1.65, maxC);
    vColor         = rawCol * pInstPitchTint.yzw;
    vLightSpacePos = uLightSpaceMatrix * vec4(worldPos, 1.0);
    gl_Position    = uViewProj * vec4(worldPos, 1.0);
}
)";

} // namespace Shaders
