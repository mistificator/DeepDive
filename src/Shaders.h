#pragma once

namespace Shaders {

// ============================================================================
// ШЕЙДЕРЫ ПРОХОДА КАРТЫ ТЕНЕЙ (Shadow Map Pass — учитывают анимацию водорослей и фауны)
// ============================================================================

inline const char* ShadowEnvVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec2 aParam;

uniform mat4 uLightSpaceMatrix;
uniform float uTime;

void main() {
    vec3 pos = aPos;

    if (aParam.x > 2.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.15 + pos.x * 0.12 + pos.z * 0.10;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 3.0) * 1.40 * hCurve;
        float swayZ = cos(phase * 0.8 + h * 4.2) * 0.85 * (h * h);
        float flutter = sin(uTime * 2.8 + pos.y * 1.1 + pos.x * 0.5) * 0.14 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
    } else if (aParam.x > 1.5 && aParam.y > 0.05) {
        float h = clamp(aParam.y, 0.0, 1.0);
        pos.x += sin(uTime * 1.8 + pos.y * 2.0 + pos.x) * 0.10 * h;
        pos.z += cos(uTime * 1.5 + pos.y * 2.0 + pos.z) * 0.10 * h;
    }

    gl_Position = uLightSpaceMatrix * vec4(pos, 1.0);
}
)";

inline const char* ShadowFaunaVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aInstPosScale;
layout(location = 4) in vec4 aInstRotPhase; // z: 0=fish, 1=manta, 2=whale

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
    } else {
        // Медленный вертикальный взмах хвоста гигантского кита
        float phase = aInstRotPhase.y + uTime * 1.35;
        float tailWeight = smoothstep(1.2, -5.2, localPos.z);
        float spineWave = sin(phase - localPos.z * 0.55) * 0.55 * (tailWeight * tailWeight);
        float finSway = sin(phase - abs(localPos.x) * 0.6) * 0.12 * clamp((abs(localPos.x) - 0.8) / 2.0, 0.0, 1.0);
        localPos.y += spineWave + finSway;
    }

    localPos *= aInstPosScale.w;

    float yaw = aInstRotPhase.x;
    float bank = aInstRotPhase.w;
    float cy = cos(yaw), sy = sin(yaw);
    float cb = cos(bank), sb = sin(bank);

    mat3 rotY = mat3(cy, 0.0, -sy,  0.0, 1.0, 0.0,  sy, 0.0, cy);
    mat3 rotZ = mat3(cb, sb, 0.0,  -sb, cb, 0.0,   0.0, 0.0, 1.0);
    vec3 worldPos = (rotY * rotZ) * localPos + aInstPosScale.xyz;

    gl_Position = uLightSpaceMatrix * vec4(worldPos, 1.0);
}
)";

inline const char* ShadowDepthFrag = R"(#version 330 core
void main() {
    // Запись глубины выполняется аппаратно
}
)";

// ============================================================================
// ОСНОВНЫЕ ШЕЙДЕРЫ ОКРУЖЕНИЯ С РАССЕЯННЫМИ ТЕНЯМИ
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

    if (aParam.x > 2.5) {
        float h = clamp(aParam.y, 0.0, 1.0);
        float phase = uTime * 1.15 + pos.x * 0.12 + pos.z * 0.10;
        float hCurve = h * sqrt(h);
        float swayX = sin(phase + h * 3.0) * 1.40 * hCurve;
        float swayZ = cos(phase * 0.8 + h * 4.2) * 0.85 * (h * h);
        float flutter = sin(uTime * 2.8 + pos.y * 1.1 + pos.x * 0.5) * 0.14 * h;
        pos.x += swayX + flutter;
        pos.z += swayZ + flutter;
        norm = normalize(norm + vec3(swayX * 0.15, 0.0, swayZ * 0.15));
    } else if (aParam.x > 1.5 && aParam.y > 0.05) {
        float h = clamp(aParam.y, 0.0, 1.0);
        pos.x += sin(uTime * 1.8 + pos.y * 2.0 + pos.x) * 0.10 * h;
        pos.z += cos(uTime * 1.5 + pos.y * 2.0 + pos.z) * 0.10 * h;
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
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uTime;
uniform int uEnableShadows;

out vec4 FragColor;

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

// 16-точечный фильтр Фогеля (Vogel Disk PCF) с учетом рассеяния полутени в толще воды
float calcUnderwaterShadow(vec4 lightSpacePos, vec3 N, vec3 L) {
    if (uEnableShadows == 0) return 1.0;

    vec3 projCoords = lightSpacePos.xyz / max(lightSpacePos.w, 0.0001);
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 0.999 || projCoords.x < 0.001 || projCoords.x > 0.999 || projCoords.y < 0.001 || projCoords.y > 0.999) {
        return 1.0;
    }

    // Плавное затухание карты теней у границ каскада
    vec2 edgeDist = smoothstep(vec2(0.0), vec2(0.08), projCoords.xy) * (1.0 - smoothstep(vec2(0.92), vec2(1.0), projCoords.xy));
    float cascadeFade = edgeDist.x * edgeDist.y;

    float cosTheta = clamp(dot(N, L), 0.0, 1.0);
    float bias = max(0.0035 * (1.0 - cosTheta), 0.0012);

    // Псевдослучайный поворот диска сэмплирования по мировым координатам
    float rndAngle = fract(sin(dot(vWorldPos.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float s = sin(rndAngle);
    float c = cos(rndAngle);
    mat2 rot = mat2(c, -s, s, c);

    // Оценка расстояния от преграды (Blocker Distance) для расширения подводной полутени
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

    // Если преграда высоко в воде (например, плывущий кит или верхушка ламинарии), тень сильнее размывается водой
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
        float theta = fi * 2.3999632; // золотой угол
        vec2 offset = (rot * vec2(cos(theta), sin(theta))) * r * texelSize;
        float pcfDepth = texture(uShadowMap, projCoords.xy + offset).r;
        shadowSum += (projCoords.z - bias > pcfDepth) ? 0.0 : 1.0;
    }

    float rawShadow = shadowSum / 16.0;
    return mix(1.0, rawShadow, cascadeFade);
}

void main() {
    vec3 safeN = length(vNormal) > 0.0001 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    vec3 geomN = gl_FrontFacing ? safeN : -safeN;
    vec3 viewVec = uCamPos - vWorldPos;
    float dist = length(viewVec);
    vec3 V = dist > 0.0001 ? (viewVec / dist) : vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(uSunDir);

    vec3 blend = max(abs(geomN), vec3(0.0001));
    blend = blend * blend;
    blend = blend * blend;
    blend /= (blend.x + blend.y + blend.z);

    vec3 N = geomN;
    vec3 baseCol = clamp(vColor, 0.0, 1.0);
    float ao = 1.0;

    if (vParam.x < 1.5) {
        vec4 macroSample = sampleTriplanar(uRockTex, vWorldPos * 0.06, blend);
        vec4 microSample = sampleTriplanar(uRockTex, vWorldPos * 0.30, blend);
        vec4 fineSample  = sampleTriplanar(uRockTex, vWorldPos * 1.05, blend);

        vec3 nMacro = macroSample.rgb * 2.0 - 1.0;
        vec3 nMicro = microSample.rgb * 2.0 - 1.0;
        vec3 nFine  = fineSample.rgb  * 2.0 - 1.0;

        vec3 perturb = (nMacro * 0.50 + nMicro * 0.40 + nFine * 0.22);
        N = normalize(geomN + perturb * 0.60);

        float strata = sin(vWorldPos.y * 1.6 + macroSample.a * 3.8) * 0.05;
        float detailHeight = macroSample.a * 0.5 + microSample.a * 0.35 + fineSample.a * 0.15;
        ao = smoothstep(0.10, 0.75, detailHeight) * 0.45 + 0.55;

        baseCol = clamp(baseCol * (0.74 + detailHeight * 0.52 + strata), 0.0, 1.0);
    } else {
        vec4 orgSample = sampleTriplanar(uRockTex, vWorldPos * 0.40, blend);
        N = normalize(geomN + (orgSample.rgb * 2.0 - 1.0) * 0.20);
        baseCol *= (0.85 + orgSample.a * 0.32);
    }

    // Вычисляем коэффициент рассеянной подводной тени (1.0 = свет, 0.0 = полная тень)
    float shadow = calcUnderwaterShadow(vLightSpacePos, geomN, L);
    // В воде даже в глубокой тени остается мягкое рассеянное свечение толщи океана
    float directShadow = mix(0.12, 1.0, shadow);

    float wrapDiff = clamp((dot(N, L) + 0.50) / 1.50, 0.0, 1.0);
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 H = normalize(L + V + vec3(0.0001));
    float specBase = clamp(dot(N, H), 0.0, 1.0);
    float spec = pow(max(specBase, 0.0001), 24.0) * step(0.0002, specBase) * 0.20 * ao * shadow;

    float rimBase = clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0);
    float rim = rimBase * rimBase * rimBase * 0.35 * mix(0.4, 1.0, shadow);

    float sss = 0.0;
    if (vParam.x > 1.5) {
        float backLight = clamp(dot(V, -L + N * 0.30), 0.0, 1.0);
        sss = ((backLight * backLight) * 0.80 + 0.18) * mix(0.35, 1.0, shadow);
    }

    // Солнечная каустика гасится внутри теней от скал, кораблей, водорослей и плывущего кита!
    float caust = organicCaustics(vWorldPos.xz + vWorldPos.y * 0.25, uTime);
    float depthAtten = clamp((vWorldPos.y + 30.0) / 45.0, 0.18, 1.0);
    float nyPos = clamp(N.y, 0.0, 1.0);
    float caustMask = sqrt(nyPos) * depthAtten * ao * shadow;

    vec3 sunColor = vec3(0.52, 0.96, 1.05);
    // В тени окружающий свет смещается в более глубокий сине-бирюзовый оттенок воды
    vec3 ambientColor = mix(uWaterColorDeep * 0.85, uWaterColorShallow,
                            clamp(hemi * 0.55 + depthAtten * 0.45, 0.0, 1.0) * mix(0.55, 1.0, shadow));

    vec3 litColor = baseCol * ao * (ambientColor * 0.72 + sunColor * wrapDiff * 0.98 * directShadow)
                  + sunColor * spec
                  + uWaterColorShallow * rim * depthAtten;
    litColor += sunColor * caust * caustMask * 0.85;
    litColor += vec3(0.32, 1.05, 0.72) * sss;

    if (vParam.x > 2.5) {
        vec4 bioSample = sampleTriplanar(uRockTex, vWorldPos * 1.35, blend);
        float glowMask = smoothstep(0.77, 0.87, bioSample.a);
        float pulse = 0.55 + 0.45 * sin(uTime * 2.8 + vWorldPos.y * 1.5 + vWorldPos.x);
        // Биолюминесценция еще ярче выделяется в затененных участках рифа
        litColor += vec3(0.45, 1.35, 0.85) * glowMask * pulse * 1.25;
    }

    float fogFactor = 1.0 - exp(-dist * 0.0145);
    float sunDot = clamp(dot(V, -L), 0.0, 1.0);
    float sunScatter = (sunDot * sunDot * sunDot * sunDot) * 0.35;
    float viewY = clamp(V.y * -0.5 + 0.5, 0.0, 1.0);
    vec3 fogColor = mix(uWaterColorShallow, uWaterColorDeep, clamp(viewY * 0.72 + (1.0 - depthAtten) * 0.35, 0.0, 1.0));
    fogColor += sunColor * sunScatter * depthAtten;

    vec3 outCol = mix(litColor, fogColor, clamp(fogFactor, 0.0, 1.0));
    FragColor = vec4(clamp(outCol, 0.0, 10.0), 1.0);
}
)";

// ============================================================================
// ШЕЙДЕРЫ ФАУНЫ (Рыбы, Скаты-манты и Гигантский Кит + Прием теней)
// ============================================================================

inline const char* FaunaVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec4 aInstPosScale;
layout(location = 4) in vec4 aInstRotPhase; // z: 0=fish, 1=manta, 2=whale

uniform mat4 uViewProj;
uniform mat4 uLightSpaceMatrix;
uniform float uTime;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec4 vLightSpacePos;

void main() {
    vec3 localPos = aPos;
    vec3 localN = aNormal;

    if (aInstRotPhase.z < 0.5) {
        // 0: Рыбы (горизонтальный изгиб хвоста)
        float phase = aInstRotPhase.y + uTime * 9.5;
        float tailWeight = smoothstep(0.35, -0.75, localPos.z);
        float wave = sin(phase - localPos.z * 3.8) * 0.14 * tailWeight;
        float dWaveDz = -3.8 * cos(phase - localPos.z * 3.8) * 0.14 * tailWeight;
        localPos.x += wave;
        localN = normalize(vec3(localN.x - dWaveDz * localN.z, localN.y, localN.z));
    } else if (aInstRotPhase.z < 1.5) {
        // 1: Скаты-манты (взмахи широких крыльев)
        float phase = aInstRotPhase.y + uTime * 2.6;
        float wingDist = abs(localPos.x);
        float wf = clamp(wingDist / 1.8, 0.0, 1.2);
        float wingFactor = wf * sqrt(wf);
        float waveArg = phase - wingDist * 1.15 - localPos.z * 0.35;
        float flap = sin(waveArg) * 0.42 * wingFactor;
        float dFlapDx = -1.15 * sign(localPos.x) * cos(waveArg) * 0.42 * wingFactor;
        localPos.y += flap;
        localN = normalize(vec3(localN.x - dFlapDx * localN.y, localN.y, localN.z));
    } else {
        // 2: Гигантский горбатый кит (мощная вертикальная волна позвоночника и хвостовых лопастей)
        float phase = aInstRotPhase.y + uTime * 1.35;
        float tailWeight = smoothstep(1.2, -5.2, localPos.z);
        float w2 = tailWeight * tailWeight;
        float waveArg = phase - localPos.z * 0.55;
        float spineWave = sin(waveArg) * 0.55 * w2;
        float dWaveDz = -0.55 * cos(waveArg) * 0.55 * w2;

        // Плавное покачивание длинных грудных плавников кита
        float flipperWeight = clamp((abs(localPos.x) - 0.8) / 2.0, 0.0, 1.0);
        float finSway = sin(phase - abs(localPos.x) * 0.6) * 0.12 * flipperWeight;

        localPos.y += spineWave + finSway;
        localN = normalize(vec3(localN.x, localN.y, localN.z - dWaveDz * localN.y));
    }

    localPos *= aInstPosScale.w;

    float yaw = aInstRotPhase.x;
    float bank = aInstRotPhase.w;
    float cy = cos(yaw), sy = sin(yaw);
    float cb = cos(bank), sb = sin(bank);

    mat3 rotY = mat3(
        cy, 0.0, -sy,
        0.0, 1.0, 0.0,
        sy, 0.0, cy
    );
    mat3 rotZ = mat3(
        cb, sb, 0.0,
        -sb, cb, 0.0,
        0.0, 0.0, 1.0
    );
    mat3 rot = rotY * rotZ;

    vec3 worldPos = rot * localPos + aInstPosScale.xyz;
    vWorldPos = worldPos;
    vNormal = normalize(rot * localN);
    vColor = aColor;
    vLightSpacePos = uLightSpaceMatrix * vec4(worldPos, 1.0);

    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)";

inline const char* FaunaFrag = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
in vec4 vLightSpacePos;

uniform sampler2D uRockTex;
uniform sampler2D uShadowMap;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uTime;
uniform int uEnableShadows;

out vec4 FragColor;

float sampleFaunaShadow(vec4 lightSpacePos, vec3 N, vec3 L) {
    if (uEnableShadows == 0) return 1.0;
    vec3 projCoords = lightSpacePos.xyz / max(lightSpacePos.w, 0.0001);
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 0.999 || projCoords.x < 0.001 || projCoords.x > 0.999 || projCoords.y < 0.001 || projCoords.y > 0.999) {
        return 1.0;
    }

    float bias = max(0.0035 * (1.0 - clamp(dot(N, L), 0.0, 1.0)), 0.0012);
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float d = texture(uShadowMap, projCoords.xy + vec2(x, y) * texelSize * 2.0).r;
            shadow += (projCoords.z - bias > d) ? 0.20 : 1.0;
        }
    }
    return shadow / 9.0;
}

void main() {
    vec3 N = length(vNormal) > 0.0001 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    vec3 viewVec = uCamPos - vWorldPos;
    float dist = length(viewVec);
    vec3 V = dist > 0.0001 ? (viewVec / dist) : vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(uSunDir);

    float shadow = sampleFaunaShadow(vLightSpacePos, N, L);

    float diff = clamp((dot(N, L) + 0.40) / 1.40, 0.0, 1.0) * shadow;
    vec3 H = normalize(L + V + vec3(0.0001));
    float specBase = clamp(dot(N, H), 0.0, 1.0);
    float spec = pow(max(specBase, 0.0001), 32.0) * step(0.0002, specBase) * 0.40 * shadow;
    float rimBase = clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0);
    float rim = rimBase * rimBase * sqrt(rimBase) * 0.38;

    // Проекция водной каустики на спину кита и скатов
    vec2 caustUV = vWorldPos.xz * 0.12 + vec2(uTime * 0.05, -uTime * 0.04);
    float h1 = texture(uRockTex, caustUV).a;
    float h2 = texture(uRockTex, caustUV * 1.4 - vec2(uTime * 0.04)).a;
    float caust = pow(clamp(1.0 - abs(h1 - h2) * 2.1, 0.0, 1.0), 4.0) * clamp(N.y, 0.0, 1.0) * shadow;

    float depthAtten = clamp((vWorldPos.y + 30.0) / 45.0, 0.18, 1.0);
    vec3 ambient = mix(uWaterColorDeep, uWaterColorShallow, depthAtten);
    vec3 sunColor = vec3(0.58, 0.98, 1.05);

    vec3 lit = vColor * (ambient * 0.72 + sunColor * diff * 0.88) + sunColor * (spec + rim + caust * 0.55);

    float fogFactor = 1.0 - exp(-dist * 0.0145);
    float viewY = clamp(V.y * -0.5 + 0.5, 0.0, 1.0);
    vec3 fogColor = mix(uWaterColorShallow, uWaterColorDeep, clamp(viewY * 0.75 + (1.0 - depthAtten) * 0.35, 0.0, 1.0));

    vec3 outCol = mix(lit, fogColor, clamp(fogFactor, 0.0, 1.0));
    FragColor = vec4(clamp(outCol, 0.0, 10.0), 1.0);
}
)";

// ============================================================================
// SKYBOX, ЛУЧИ, ЧАСТИЦЫ И ПОСТПРОДАКШЕН
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
uniform sampler2D uRockTex;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uTime;

out vec4 FragColor;

vec3 sampleWaveNormalAndFoam(vec2 uv, float t) {
    mat2 rot1 = mat2(0.80, -0.60, 0.60, 0.80);
    mat2 rot2 = mat2(-0.60, -0.80, 0.80, -0.60);

    vec2 p1 = uv * 0.042 + vec2(t * 0.025, -t * 0.018);
    vec2 p2 = (rot1 * uv) * 0.090 + vec2(-t * 0.032, t * 0.028);
    vec2 p3 = (rot2 * uv) * 0.195 + vec2(t * 0.045, t * 0.041);

    vec4 s1 = texture(uRockTex, p1);
    vec4 s2 = texture(uRockTex, p2);
    vec4 s3 = texture(uRockTex, p3);

    vec3 n = (s1.rgb * 2.0 - 1.0) * 0.55
           + (s2.rgb * 2.0 - 1.0) * 0.30
           + (s3.rgb * 2.0 - 1.0) * 0.15;
    float c = clamp(1.0 - abs(s1.a - s2.a) * 2.2, 0.0, 1.0);
    float c2 = c * c;
    return vec3(n.xy, c2 * c2);
}

void main() {
    vec4 worldTarget = uInvViewProj * vec4(vNDC, 1.0, 1.0);
    vec3 dir = normalize(worldTarget.xyz / max(abs(worldTarget.w), 0.0001) - uCamPos);

    float upFactor = clamp(dir.y * 0.55 + 0.45, 0.0, 1.0);
    vec3 col = mix(uWaterColorDeep * 0.45, uWaterColorShallow, upFactor * sqrt(upFactor));

    if (dir.y > 0.02) {
        float planeDist = 30.0 / max(dir.y, 0.02);
        vec2 surfPos = uCamPos.xz + dir.xz * planeDist;

        vec3 wave = sampleWaveNormalAndFoam(surfPos, uTime);
        float horizonFade = exp(-planeDist * 0.0085);

        float cosTheta = clamp(dir.y + wave.x * 0.12 * horizonFade, 0.0, 1.0);
        float snellWindow = smoothstep(0.50, 0.78, cosTheta);

        vec3 refractedDir = normalize(dir + vec3(wave.x, 0.0, wave.y) * 0.22);
        float sunDot = clamp(dot(refractedDir, normalize(uSunDir)), 0.0, 1.0);
        float s2 = sunDot * sunDot;
        float s4 = s2 * s2;
        float sunDisk = s4 * s4;

        vec3 skyBright = vec3(0.68, 1.08, 1.15);
        vec3 surfaceCol = mix(uWaterColorShallow * 0.85, skyBright, snellWindow * 0.70);
        surfaceCol += skyBright * (sunDisk * 0.90 + wave.z * 0.40 * snellWindow);

        col = mix(col, surfaceCol, horizonFade * smoothstep(0.02, 0.22, dir.y));
    }

    FragColor = vec4(clamp(col, 0.0, 10.0), 1.0);
}
)";

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
    float fogAtten = exp(-dist * 0.012);

    float uClamped = clamp(vUV.x, 0.0, 1.0);
    float vClamped = clamp(vUV.y, 0.0, 1.0);

    float s = clamp(sin(uClamped * 3.14159265), 0.0, 1.0);
    float edgeFade = s * s;
    float verticalFade = smoothstep(0.0, 0.25, vClamped) * (1.0 - smoothstep(0.65, 1.0, vClamped));
    float shimmer = 0.75 + 0.25 * sin(uTime * 1.4 + vWorldPos.x * 0.25 + vWorldPos.z * 0.25);

    float alpha = clamp(vColor.a * edgeFade * verticalFade * shimmer * fogAtten, 0.0, 1.0);
    FragColor = vec4(clamp(vColor.rgb, 0.0, 4.0), alpha);
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
        FragColor = vec4(clamp(vColor + vec3(highlight * 0.65), 0.0, 4.0), alpha);
    } else {
        float oneMinusR2 = clamp(1.0 - r2, 0.0, 1.0);
        float glow = oneMinusR2 * oneMinusR2;
        FragColor = vec4(clamp(vColor * 1.35, 0.0, 4.0), glow * 0.90);
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
    return clamp(c, vec3(0.0), vec3(12.0));
}

vec3 extractScatteredLight(vec3 c) {
    vec3 safeC = sanitizeHDR(c);
    float lum = dot(safeC, vec3(0.2126, 0.7152, 0.0722));
    float softKnee = smoothstep(0.28, 1.10, lum);
    return safeC * (0.22 + softKnee * 0.95);
}

void main() {
    const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec3 sum = sanitizeHDR(texture(uSourceTex, vUV).rgb);
    if (uExtractBright == 1) sum = extractScatteredLight(sum);
    sum *= weights[0];

    for (int i = 1; i < 5; ++i) {
        vec2 off = uTexelStep * float(i) * 1.65;
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
uniform float uTime;
uniform int uEnablePost;

out vec4 FragColor;

vec3 sanitizeHDR(vec3 c) {
    if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);
    return clamp(c, vec3(0.0), vec3(12.0));
}

float linearizeDepth(float d) {
    float zNear = 0.2;
    float zFar  = 220.0;
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
        FragColor = vec4(sanitizeHDR(texture(uSceneColorTex, vUV).rgb), 1.0);
        return;
    }

    vec2 waveOffset = vec2(
        sin(vUV.y * 14.0 + uTime * 1.3) * cos(vUV.x * 11.0 + uTime * 0.9),
        cos(vUV.x * 13.0 - uTime * 1.1) * sin(vUV.y * 12.0 + uTime * 0.8)
    ) * 0.0008;
    vec2 uv = clamp(vUV + waveOffset, 0.001, 0.999);

    float rawDepth = texture(uSceneDepthTex, uv).r;
    float linDepth = linearizeDepth(rawDepth);
    float turbidityBlur = clamp((linDepth - 10.0) / 55.0, 0.15, 1.0);

    vec2 edgeVec = uv - 0.5;
    float radialDist = length(edgeVec);

    vec2 caShift = edgeVec * (0.0018 * radialDist);
    vec3 centerCol;
    centerCol.r = sanitizeHDR(texture(uSceneColorTex, clamp(uv + caShift, 0.001, 0.999)).rgb).r;
    centerCol.g = sanitizeHDR(texture(uSceneColorTex, uv).rgb).g;
    centerCol.b = sanitizeHDR(texture(uSceneColorTex, clamp(uv - caShift, 0.001, 0.999)).rgb).b;

    vec2 blurStep = uInvResolution * (1.2 + turbidityBlur * 2.2);
    vec3 ringCol = (
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2( blurStep.x,  blurStep.y), 0.001, 0.999)).rgb) +
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2(-blurStep.x,  blurStep.y), 0.001, 0.999)).rgb) +
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2( blurStep.x, -blurStep.y), 0.001, 0.999)).rgb) +
        sanitizeHDR(texture(uSceneColorTex, clamp(uv + vec2(-blurStep.x, -blurStep.y), 0.001, 0.999)).rgb)
    ) * 0.25;

    vec3 sceneCol = mix(centerCol, ringCol, turbidityBlur * 0.42);

    vec3 scatteredBloom = sanitizeHDR(texture(uBloomTex, uv).rgb);
    sceneCol = mix(sceneCol, scatteredBloom, 0.24) + scatteredBloom * 0.34;

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
            float sDepth = texture(uSceneDepthTex, clampedUV).r;
            float skyMask = smoothstep(0.965, 0.9999, sDepth) * 0.75 + 0.25;
            vec3 sColor = sanitizeHDR(texture(uBloomTex, clampedUV).rgb);
            godRayAcc += sColor * skyMask * illuminationDecay;
            illuminationDecay *= 0.935;
        }

        float rayFade = clamp(uSunScreenPos.z, 0.0, 1.0) * exp(-sunDist * 1.15);
        sceneCol += godRayAcc * (0.026 * rayFade);
    }

    float topGrad = clamp(vUV.y, 0.0, 1.0);
    sceneCol += uWaterTint * (topGrad * topGrad) * 0.10;

    vec3 mapped = acesToneMapping(sceneCol * 1.12);
    mapped = pow(max(mapped, vec3(0.0001)), vec3(0.95));

    float vignette = smoothstep(1.08, 0.32, radialDist);
    mapped *= mix(0.68, 1.0, vignette);

    FragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
)";

} // namespace Shaders
