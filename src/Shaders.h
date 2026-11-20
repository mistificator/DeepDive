#pragma once

namespace Shaders {

// ============================================================================
// ШЕЙДЕРЫ ПРОХОДА КАРТЫ ТЕНЕЙ
// ============================================================================

inline const char* ShadowEnvVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec2 aParam; // x: 0=rock, 1=wood, 2=reef/grass, 3=kelp, 4=abyssalGlow; y: sway/heightFactor

uniform mat4 uLightSpaceMatrix;
uniform float uTime;

void main() {
    vec3 pos = aPos;

    if (aParam.x > 3.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.45 + pos.x * 0.25 + pos.z * 0.22;
        pos.x += sin(phase + h * 2.4) * 0.24 * (h * h);
        pos.z += cos(phase * 0.85 + h * 2.8) * 0.20 * (h * h);
    } else if (aParam.x > 2.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.15 + pos.x * 0.12 + pos.z * 0.10;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 3.0) * 1.40 * hCurve;
        float swayZ = cos(phase * 0.8 + h * 4.2) * 0.85 * (h * h);
        float flutter = sin(uTime * 2.8 + pos.y * 1.1 + pos.x * 0.5) * 0.14 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
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
        float phase = aInstRotPhase.y + uTime * 2.6;
        float wingDist = abs(localPos.x);
        float wf = clamp(wingDist / 1.8, 0.0, 1.2);
        localPos.y += sin(phase - wingDist * 1.15 - localPos.z * 0.35) * 0.42 * (wf * sqrt(wf));
    } else if (aInstRotPhase.z < 2.5) {
        float phase = aInstRotPhase.y + uTime * 1.35;
        float tailWeight = smoothstep(1.2, -5.2, localPos.z);
        float spineWave = sin(phase - localPos.z * 0.55) * 0.55 * (tailWeight * tailWeight);
        float finSway = sin(phase - abs(localPos.x) * 0.6) * 0.12 * clamp((abs(localPos.x) - 0.8) / 2.0, 0.0, 1.0);
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
// ОСНОВНЫЕ ШЕЙДЕРЫ ОКРУЖЕНИЯ
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

    if (aParam.x > 3.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.45 + pos.x * 0.25 + pos.z * 0.22;
        float swayX = sin(phase + h * 2.4) * 0.24 * (h * h);
        float swayZ = cos(phase * 0.85 + h * 2.8) * 0.20 * (h * h);
        pos.x += swayX;
        pos.z += swayZ;
        norm = normalize(norm + vec3(swayX * 0.25, 0.0, swayZ * 0.25));
    } else if (aParam.x > 2.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.15 + pos.x * 0.12 + pos.z * 0.10;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 3.0) * 1.40 * hCurve;
        float swayZ = cos(phase * 0.8 + h * 4.2) * 0.85 * (h * h);
        float flutter = sin(uTime * 2.8 + pos.y * 1.1 + pos.x * 0.5) * 0.14 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
        norm = normalize(norm + vec3(swayX * 0.15, 0.0, swayZ * 0.15));
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

inline const char* EnvironmentFrag = R"(#version 330 core
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

vec4 sampleTriplanar(sampler2D tex, vec3 p, vec3 blend) {
    vec4 tx = texture(tex, p.yz);
    vec4 ty = texture(tex, p.xz);
    vec4 tz = texture(tex, p.xy);
    return tx * blend.x + ty * blend.y + tz * blend.z;
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

float calcUnderwaterShadow(vec4 lightSpacePos, vec3 geomN, vec3 faceN, vec3 L) {
    if (uEnableShadows == 0) return 1.0;

    vec3 projCoords = lightSpacePos.xyz / max(lightSpacePos.w, 0.0001);
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 0.999 || projCoords.x < 0.001 || projCoords.x > 0.999 || projCoords.y < 0.001 || projCoords.y > 0.999) {
        return 1.0;
    }

    vec2 edgeDist = smoothstep(vec2(0.0), vec2(0.08), projCoords.xy) * (1.0 - smoothstep(vec2(0.92), vec2(1.0), projCoords.xy));
    float cascadeFade = edgeDist.x * edgeDist.y;

    float cosTheta = clamp(min(dot(geomN, L), dot(faceN, L)), 0.0, 1.0);
    float bias = max(0.0046 * (1.0 - cosTheta), 0.0014);

    float rndAngle = fract(sin(dot(vWorldPos.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float s = sin(rndAngle);
    float c = cos(rndAngle);
    mat2 rot = mat2(c, -s, s, c);

    float avgBlockerDepth = 0.0;
    int blockers = 0;
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));

    for (int i = 0; i < 4; ++i) {
        float fi = float(i) * 4.0 + 1.0;
        float r = sqrt(fi / 16.0) * 3.5;
        float theta = fi * 2.3999632;
        vec2 offset = (rot * vec2(cos(theta), sin(theta))) * r * texelSize;
        float d = texture(uShadowMap, projCoords.xy + offset).r;
        if (projCoords.z - bias > d) {
            avgBlockerDepth += d;
            blockers++;
        }
    }

    float penumbraRadius = 2.4;
    if (blockers > 0) {
        avgBlockerDepth /= float(blockers);
        float blockerDist = max(projCoords.z - avgBlockerDepth, 0.0);
        penumbraRadius = clamp(1.8 + blockerDist * 55.0, 1.8, 7.5);
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

void main() {
    vec3 safeN = length(vNormal) > 0.0001 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    vec3 geomN = gl_FrontFacing ? safeN : -safeN;

    vec3 rawFaceN = cross(dFdx(vWorldPos), dFdy(vWorldPos));
    float faceLen = length(rawFaceN);
    vec3 trueFaceN = (faceLen > 1e-6) ? (rawFaceN / faceLen) : geomN;
    if (dot(trueFaceN, geomN) < 0.0) trueFaceN = -trueFaceN;

    vec3 viewVec = uCamPos - vWorldPos;
    float dist = length(viewVec);
    vec3 V = dist > 0.0001 ? (viewVec / dist) : vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(uSunDir);

    vec3 triN = geomN;
    if (vParam.x < 0.5) {
        float steepMismatch = smoothstep(0.08, 0.32, geomN.y - max(trueFaceN.y, 0.0));
        triN = normalize(mix(geomN, trueFaceN, steepMismatch));
    }

    vec3 blend = max(abs(triN), vec3(0.0001));
    blend = blend * blend;
    blend = blend * blend;
    blend /= (blend.x + blend.y + blend.z);

    vec3 N = triN;
    float vibStrength = (vParam.x > 1.5) ? uVibrance : mix(1.0, uVibrance, 0.50);
    vec3 baseCol = applyVibrance(clamp(vColor, 0.0, 1.0), vibStrength);
    float ao = 1.0;

    if (vParam.x < 1.5) {
        vec4 macroSample = sampleTriplanar(uRockTex, vWorldPos * 0.045, blend);
        vec4 microSample = sampleTriplanar(uRockTex, vWorldPos * 0.24, blend);
        float fineWeight = 1.0 - smoothstep(95.0, 225.0, dist);
        vec4 fineSample  = (fineWeight > 0.01) ? sampleTriplanar(uRockTex, vWorldPos * 0.95, blend) : microSample;

        vec3 nMacro = macroSample.rgb * 2.0 - 1.0;
        vec3 nMicro = microSample.rgb * 2.0 - 1.0;
        vec3 nFine  = fineSample.rgb  * 2.0 - 1.0;

        vec3 perturb = (nMacro * 0.55 + nMicro * 0.44 + nFine * (0.24 * fineWeight));
        N = normalize(triN + perturb * 0.72);

        float strata = sin(vWorldPos.y * 1.65 + macroSample.a * 4.8) * 0.075;
        float detailHeight = macroSample.a * 0.52 + microSample.a * 0.34 + fineSample.a * 0.14;
        ao = smoothstep(0.10, 0.78, detailHeight) * 0.48 + 0.52;

        baseCol = clamp(baseCol * (0.74 + detailHeight * 0.52 + strata), 0.0, 1.25);
    } else {
        vec4 orgSample = sampleTriplanar(uRockTex, vWorldPos * 0.40, blend);
        N = normalize(geomN + (orgSample.rgb * 2.0 - 1.0) * 0.20);
        baseCol *= (0.88 + orgSample.a * 0.30);
    }

    float shadow = calcUnderwaterShadow(vLightSpacePos, triN, trueFaceN, L);
    float directShadow = mix(0.20, 1.0, shadow);

    float abyssDark = smoothstep(-22.0, -56.0, vWorldPos.y);
    float sunSurvival = mix(1.0, 0.03, abyssDark);
    float ambSurvival = mix(1.0, 0.08, abyssDark);

    float wrapDiff = clamp((dot(N, L) + 0.48) / 1.48, 0.0, 1.0);
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 H = normalize(L + V + vec3(0.0001));
    float specBase = clamp(dot(N, H), 0.0, 1.0);
    float spec = pow(max(specBase, 0.0001), 26.0) * step(0.0002, specBase) * 0.22 * ao * shadow * sunSurvival;

    float rimBase = clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0);
    float rim = rimBase * rimBase * rimBase * 0.30 * mix(0.45, 1.0, shadow) * ambSurvival;

    float sss = 0.0;
    if (vParam.x > 1.5) {
        float backLight = clamp(dot(V, -L + N * 0.30), 0.0, 1.0);
        sss = ((backLight * backLight) * 0.78 + 0.18) * mix(0.40, 1.0, shadow) * sunSurvival;
    }

    vec2 caustProj = vWorldPos.xz - vWorldPos.y * (L.xz / max(L.y, 0.45));
    float caust = organicCaustics(caustProj, uTime);
    float depthAtten = clamp((vWorldPos.y + 34.0) / 58.0, 0.08, 1.0);
    float underwaterMask = 1.0 - smoothstep(26.4, 28.4, vWorldPos.y);
    float slopeUp = min(max(triN.y, 0.0), max(trueFaceN.y, 0.0) + 0.08);
    float slopeCaustFade = smoothstep(0.35, 0.80, slopeUp);
    float sunFaceFade = smoothstep(0.08, 0.46, dot(trueFaceN, L));
    float caustMask = slopeCaustFade * sunFaceFade * depthAtten * ao * shadow * underwaterMask * (1.0 - abyssDark);

    vec3 sunColor = uSunColor;
    vec3 ambientColor = mix(uWaterColorDeep * 0.95, uWaterColorShallow * 1.08,
                            clamp(hemi * 0.55 + depthAtten * 0.45, 0.0, 1.0) * mix(0.60, 1.0, shadow));
    ambientColor *= ambSurvival;

    vec3 litColor = baseCol * ao * (ambientColor * 0.80 + sunColor * wrapDiff * 1.08 * directShadow * sunSurvival)
                  + sunColor * spec
                  + uWaterColorShallow * rim * depthAtten;
    litColor += sunColor * caust * caustMask * 0.90;
    litColor += mix(vec3(0.28, 1.00, 0.68), baseCol * 1.12, step(1.5, vParam.x) * (1.0 - step(2.5, vParam.x))) * sss;

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
    if (vParam.x > 3.5) {
        float tip = clamp(vParam.y, 0.0, 1.0);
        float pulse = 0.68 + 0.32 * sin(uTime * 2.2 + vWorldPos.x * 0.35 + vWorldPos.z * 0.35 + tip * 3.0);
        litColor += vColor * (0.35 + tip * tip * 1.75) * pulse;
        bioFogScale = 1.0 - tip * 0.52;
    } else if (vParam.x > 2.5) {
        vec4 bioSample = sampleTriplanar(uRockTex, vWorldPos * 1.35, blend);
        float glowMask = smoothstep(0.77, 0.87, bioSample.a);
        float pulse = 0.55 + 0.45 * sin(uTime * 2.8 + vWorldPos.y * 1.5 + vWorldPos.x);
        litColor += vec3(0.42, 1.28, 0.82) * glowMask * pulse * 1.10;
    }

    float fogDensity = mix(0.0155 * uFogScale, 0.0220, abyssDark);
    vec3 transmittance = exp(-dist * bioFogScale * fogDensity * vec3(1.85, 1.06, 0.86));

    float camHeightFactor = clamp((uCamPos.y + 34.0) / 62.0, 0.06, 1.0);
    float fogHeightFactor = mix(camHeightFactor, depthAtten, 0.35);
    vec3 fogColor = computeUnderwaterFogColor(-V, L, fogHeightFactor, abyssDark);

    vec3 outCol = litColor * transmittance + fogColor * (1.0 - transmittance);
    FragColor = vec4(clamp(outCol, 0.0, 8.0), 1.0);
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
        float phase = aInstRotPhase.y + uTime * 2.6;
        float wingDist = abs(localPos.x);
        float wf = clamp(wingDist / 1.8, 0.0, 1.2);
        float wingFactor = wf * sqrt(wf);
        float waveArg = phase - wingDist * 1.15 - localPos.z * 0.35;
        float flap = sin(waveArg) * 0.42 * wingFactor;
        float dFlapDx = -1.15 * sign(localPos.x) * cos(waveArg) * 0.42 * wingFactor;
        localPos.y += flap;
        localN = normalize(vec3(localN.x - dFlapDx * localN.y, localN.y, localN.z));
    } else if (aInstRotPhase.z < 2.5) {
        float phase = aInstRotPhase.y + uTime * 1.35;
        float tailWeight = smoothstep(1.2, -5.2, localPos.z);
        float w2 = tailWeight * tailWeight;
        float waveArg = phase - localPos.z * 0.55;
        float spineWave = sin(waveArg) * 0.55 * w2;
        float dWaveDz = -0.55 * cos(waveArg) * 0.55 * w2;

        float flipperWeight = clamp((abs(localPos.x) - 0.8) / 2.0, 0.0, 1.0);
        float finSway = sin(phase - abs(localPos.x) * 0.6) * 0.12 * flipperWeight;

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
    float caust = pow(clamp(1.0 - abs(h1 - h2) * 2.1, 0.0, 1.0), 4.0) * clamp(N.y, 0.0, 1.0) * shadow * (1.0 - abyssDark);

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
// 3D-ПОВЕРХНОСТЬ МОРЯ ИЗНУТРИ (Снеллиус + TIR + Пена, разводы и пузырьки на скалах)
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

inline const char* WaterSurfaceFrag = R"(#version 330 core
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

    // ------------------------------------------------------------------------
    // КОНТАКТ СО СКАЛАМИ: точное 3D-расстояние до геометрии по буферу глубины
    // ------------------------------------------------------------------------
    vec2 screenUV = clamp(gl_FragCoord.xy * uInvResolution, 0.001, 0.999);
    float rawSceneDepth = texture(uSceneDepthTex, screenUV).r;
    float waterLin = linearizeDepth(gl_FragCoord.z);
    float sceneLin = linearizeDepth(rawSceneDepth);

    float shoreFoam = 0.0;
    float shoreBubbles = 0.0;
    vec2 shoreRippleGrad = vec2(0.0);

    if (rawSceneDepth < 0.9992 && sceneLin >= waterLin - 0.08) {
        float ratio = sceneLin / max(waterLin, 0.001);
        vec3 rockPos = uCamPos + (vWorldPos - uCamPos) * ratio;
        float dist3D = length(rockPos - vWorldPos);

        // Зона прибойных разводов (до 4.2 м) и плотная пенная кромка у самой скалы (до 1.15 м)
        float shoreZone = 1.0 - smoothstep(0.0, 4.2, dist3D);
        float tightSeam = 1.0 - smoothstep(0.0, 1.15, dist3D);

        if (shoreZone > 0.005) {
            vec4 f1 = texture(uRockTex, p * 0.36 + vec2(uTime * 0.035, -uTime * 0.028));
            vec4 f2 = texture(uRockTex, (rot1 * p) * 0.95 + vec2(-uTime * 0.062, uTime * 0.048));
            vec4 f3 = texture(uRockTex, (rot2 * p) * 2.15 + vec2(uTime * 0.095, uTime * 0.075));

            float swirl = f1.a * 0.58 + f2.a * 0.42;
            float ripplePhase = dist3D * 6.4 - uTime * 3.6 + swirl * 4.8;
            float rippleRing = sin(ripplePhase) * 0.5 + 0.5;

            // Возмущение нормали волн от отражённой ряби у скалы
            vec2 radialXZ = p - rockPos.xz;
            float radLen = length(radialXZ);
            vec2 rDir = (radLen > 0.01) ? (radialXZ / radLen) : vec2(1.0, 0.0);
            shoreRippleGrad = rDir * (cos(ripplePhase) * 0.38 * shoreZone);

            // Кружевные разводы пены + расходящиеся белые кольца + плотная кромка прибоя
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

    // Закон Снеллиуса (вода n=1.333 -> воздух n=1.0)
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

    // Вплетаем пену и искрящиеся пузырьки прибоя в поверхность воды
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
)";

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
// ЛУЧИ, ЧАСТИЦЫ И ПОСТПРОДАКШЕН (PostFX ON/OFF — заметная разница)
// ============================================================================

inline const char* TranslucentVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;

uniform mat4 uViewProj;
out vec3 vWorldPos;
out vec4 vColor;
out vec2 vUV;

void main() {
    vWorldPos = aPos;
    vColor = aColor;
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

inline const char* TranslucentFrag = R"(#version 330 core
in vec3 vWorldPos;
in vec4 vColor;
in vec2 vUV;

uniform vec3 uCamPos;
uniform float uTime;

out vec4 FragColor;

void main() {
    float dist = length(uCamPos - vWorldPos);
    float fogAtten = exp(-dist * 0.0110);
    float abyssFade = 1.0 - smoothstep(-22.0, -44.0, vWorldPos.y);

    float uClamped = clamp(vUV.x, 0.0, 1.0);
    float vClamped = clamp(vUV.y, 0.0, 1.0);

    float s = clamp(sin(uClamped * 3.14159265), 0.0, 1.0);
    float edgeFade = s * s;
    float verticalFade = smoothstep(0.0, 0.22, vClamped) * (1.0 - smoothstep(0.72, 1.0, vClamped));
    float shimmer = 0.72 + 0.28 * sin(uTime * 1.5 + vWorldPos.x * 0.25 + vWorldPos.z * 0.25);

    float alpha = clamp(vColor.a * 1.65 * edgeFade * verticalFade * shimmer * fogAtten * abyssFade, 0.0, 0.42);
    FragColor = vec4(clamp(vColor.rgb * 1.30, 0.0, 3.5), alpha);
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
        // Сырой рендер без Bloom, God Rays, искажения линзы, размытия мутности, ACES и виньетки
        FragColor = vec4(clamp(sanitizeHDR(texture(uSceneColorTex, vUV).rgb), 0.0, 1.0), 1.0);
        return;
    }

    // 1. Мягкая подводная волна преломления линзы
    vec2 waveOffset = vec2(
        sin(vUV.y * 13.0 + uTime * 1.45) * cos(vUV.x * 10.0 + uTime * 0.95),
        cos(vUV.x * 12.0 - uTime * 1.15) * sin(vUV.y * 11.0 + uTime * 0.85)
    ) * 0.0012;
    vec2 uv = clamp(vUV + waveOffset, 0.001, 0.999);

    // 2. Размытие мутности воды по реальной глубине
    float rawDepth = texture(uSceneDepthTex, uv).r;
    float linDepth = linearizeDepth(rawDepth);
    float turbidityBlur = clamp((linDepth - 14.0) / 90.0, 0.05, 1.0);

    // 3. Хроматическая аберрация по краям стекла подводной маски
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

    // 4. Рассеяние Ми и HDR Bloom (подсвечивает пену прибоя, солнце и биолюминесценцию)
    vec3 scatteredBloom = sanitizeHDR(texture(uBloomTex, uv).rgb);
    sceneCol = mix(sceneCol, scatteredBloom, 0.16) + scatteredBloom * 0.34;

    // 5. Экранные солнечные лучи (Screen-Space God Rays)
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

    // 6. Верхний световой ореол и кинематографический ACES + виньетирование маски
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
