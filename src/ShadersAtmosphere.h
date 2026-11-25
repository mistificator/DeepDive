#pragma once

namespace Shaders {

// ============================================================================
// 3D WATER SURFACE (TWO-SIDED: BELOW & ABOVE, GL 3.3 + GL 4.0 TESSELLATION)
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

    // 4 non-orthogonal wind-fanned ocean swells synchronized 1:1 with ProcMath::sampleWaterWaveOffset
    {
        vec2 k = vec2(0.112, 0.068);
        float a = 0.36 * edgeDamp;
        float ph = dot(wxz, k) + uTime * 2.10;
        h += sin(ph) * a;
        grad += k * (cos(ph) * a);
    }
    {
        vec2 k = vec2(0.082, 0.165);
        float a = 0.24 * edgeDamp;
        float ph = dot(wxz, k) + uTime * 2.65;
        h += sin(ph) * a;
        grad += k * (cos(ph) * a);
    }
    {
        vec2 k = vec2(0.255, -0.035);
        float a = 0.14 * edgeDamp;
        float ph = dot(wxz, k) + uTime * 3.25;
        h += cos(ph) * a;
        grad += k * (-sin(ph) * a);
    }
    {
        vec2 k = vec2(-0.125, 0.370);
        float a = 0.07 * edgeDamp;
        float ph = dot(wxz, k) - uTime * 4.10;
        h += sin(ph) * a;
        grad += k * (cos(ph) * a);
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
    float nearChop = (1.0 - smoothstep(20.0, 130.0, camDist)) * edgeDamp;

    float h = 0.0;
    vec2 grad = vec2(0.0);

    // 4 non-orthogonal wind-fanned ocean swells synchronized 1:1 with ProcMath::sampleWaterWaveOffset
    {
        vec2 k = vec2(0.112, 0.068);
        float a = 0.36 * edgeDamp;
        float ph = dot(wxz, k) + uTime * 2.10;
        h += sin(ph) * a;
        grad += k * (cos(ph) * a);
    }
    {
        vec2 k = vec2(0.082, 0.165);
        float a = 0.24 * edgeDamp;
        float ph = dot(wxz, k) + uTime * 2.65;
        h += sin(ph) * a;
        grad += k * (cos(ph) * a);
    }
    {
        vec2 k = vec2(0.255, -0.035);
        float a = 0.14 * edgeDamp;
        float ph = dot(wxz, k) + uTime * 3.25;
        h += cos(ph) * a;
        grad += k * (-sin(ph) * a);
    }
    {
        vec2 k = vec2(-0.125, 0.370);
        float a = 0.07 * edgeDamp;
        float ph = dot(wxz, k) - uTime * 4.10;
        h += sin(ph) * a;
        grad += k * (cos(ph) * a);
    }
    // Near-field oblique geometric chop for tessellated 3D crests
    if (nearChop > 0.005) {
        {
            vec2 k = vec2(0.52, 0.34);
            float a = 0.042 * nearChop;
            float ph = dot(wxz, k) + uTime * 4.40;
            h += sin(ph) * a;
            grad += k * (cos(ph) * a);
        }
        {
            vec2 k = vec2(0.24, -0.78);
            float a = 0.026 * nearChop;
            float ph = dot(wxz, k) - uTime * 5.20;
            h += sin(ph) * a;
            grad += k * (cos(ph) * a);
        }
    }

    vec3 worldPos = vec3(wxz.x, uWaterY + h, wxz.y);
    vWorldPos = worldPos;
    vWaveGrad = grad;
    vWaveH = h;
    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)";

#define DEEPDIVE_WATER_FRAG_PART1 R"(
in vec3 vWorldPos;
in vec2 vWaveGrad;
in float vWaveH;

uniform sampler2D uRockTex;
uniform sampler2D uSceneDepthTex;
uniform sampler2D uCloudWeatherTex;
uniform vec2 uInvResolution;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uFogScale;
uniform float uTime;
uniform int uCamAboveWater;
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

vec3 sampleDaySkyWater(vec3 dir, vec3 L, bool includeSunDisc) {
    float elev = clamp(dir.y, 0.0, 1.0);
    vec3 zenith  = vec3(0.10, 0.38, 0.82);
    vec3 midSky  = vec3(0.28, 0.65, 0.96);
    vec3 horizon = vec3(0.74, 0.90, 0.99);

    vec3 sky = mix(horizon, midSky, smoothstep(0.0, 0.28, elev));
    sky = mix(sky, zenith, pow(elev, 0.62));

    float sunDot = clamp(dot(dir, L), 0.0, 1.0);
    vec3 middaySun = vec3(1.18, 1.10, 0.94);
    sky += middaySun * (pow(sunDot, 5.0) * 0.16 + pow(sunDot, 48.0) * 0.34);
    if (includeSunDisc) {
        sky += middaySun * (smoothstep(0.9988, 0.9996, sunDot) * 2.2);
    }

    // Distant clouds reflected on the water surface (matched to the volumetric weather map)
    if (elev > 0.025) {
        vec2 cloudUV = dir.xz / (elev + 0.14);
        vec2 wUV = cloudUV * 0.145 + vec2(uTime * 0.0014, uTime * 0.0009);
        vec4 wMap = texture(uCloudWeatherTex, wUV);
        float puff = smoothstep(0.18, 0.72, wMap.r);
        float cloudMask = puff * smoothstep(0.025, 0.18, elev);
        vec3 cloudCol = mix(vec3(0.68, 0.80, 0.94), vec3(1.18, 1.15, 1.09), 0.45 + 0.55 * wMap.g);
        cloudCol += middaySun * pow(sunDot, 10.0) * 0.35;
        sky = mix(sky, cloudCol, cloudMask * 0.78);
    }
    return sky;
}
)"

#define DEEPDIVE_WATER_FRAG_PART2 R"(
void main() {
    vec3 toCam = uCamPos - vWorldPos;
    float dist = length(toCam);
    vec3 V = dist > 0.0001 ? (toCam / dist) : vec3(0.0, 1.0, 0.0);
    vec3 rayDir = -V;
    vec3 L = normalize(uSunDir);

    vec2 p = vWorldPos.xz;
    mat2 rot1 = mat2(0.80, -0.60, 0.60, 0.80);
    mat2 rot2 = mat2(-0.60, -0.80, 0.80, -0.60);

    // Living liquid domain-warping: large drifting eddies continuously morph the finer swirl layers
    vec2 warpA = texture(uRockTex, p * 0.032 + vec2( uTime * 0.048,  uTime * 0.029)).rg * 2.0 - 1.0;
    vec2 warpB = texture(uRockTex, (rot1 * p) * 0.046 + vec2(-uTime * 0.038, uTime * 0.052)).rg * 2.0 - 1.0;
    vec2 pMorph = p + (warpA + warpB * 0.70) * 1.55;

    vec4 s1 = texture(uRockTex, pMorph * 0.056 + vec2( uTime * 0.092,  uTime * 0.054));
    vec4 s2 = texture(uRockTex, (rot1 * pMorph) * 0.128 + vec2(-uTime * 0.078, uTime * 0.108));
    vec4 s3 = texture(uRockTex, (rot2 * (p + warpA * 0.85)) * 0.285 + vec2( uTime * 0.145, -uTime * 0.105));

    float fineWeight = exp(-dist * 0.026);
    vec4 s4 = (fineWeight > 0.02)
                  ? texture(uRockTex, (rot1 * (p - warpB * 0.55)) * 0.64 + vec2(-uTime * 0.195, uTime * 0.172))
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
        float shoreZone = 1.0 - smoothstep(0.0, 4.4, dist3D);
        float tightSeam = 1.0 - smoothstep(0.0, 1.25, dist3D);

        if (shoreZone > 0.005) {
            vec4 f1 = texture(uRockTex, p * 0.36 + vec2(uTime * 0.045, -uTime * 0.036));
            vec4 f2 = texture(uRockTex, (rot1 * p) * 0.95 + vec2(-uTime * 0.075, uTime * 0.058));
            vec4 f3 = texture(uRockTex, (rot2 * p) * 2.15 + vec2(uTime * 0.115, uTime * 0.092));

            float swirl = f1.a * 0.58 + f2.a * 0.42;
            float ripplePhase = dist3D * 6.4 - uTime * 4.2 + swirl * 4.8;
            float rippleRing = sin(ripplePhase) * 0.5 + 0.5;

            vec2 radialXZ = p - contactOriginXZ;
            float radLen = length(radialXZ);
            vec2 rDir = (radLen > 0.01) ? (radialXZ / radLen) : vec2(1.0, 0.0);
            shoreRippleGrad = rDir * (cos(ripplePhase) * 0.34 * shoreZone);

            float laceMask = smoothstep(0.46 - tightSeam * 0.34, 0.74 - tightSeam * 0.24, swirl) * shoreZone;
            float ringMask = smoothstep(0.42, 0.92, rippleRing) * shoreZone * smoothstep(0.28, 0.68, f1.a) * 0.68;
            shoreBubbles = smoothstep(0.72, 0.90, f3.a) * shoreZone * (0.65 + 0.65 * tightSeam);
            shoreFoam = clamp(tightSeam * (0.65 + 0.35 * f2.a) + laceMask * 0.85 + ringMask + shoreBubbles * 0.75, 0.0, 1.0);
        }
    }

    vec2 texSlope = (s1.rg * 2.0 - 1.0) * 0.30
                  + (s2.rg * 2.0 - 1.0) * 0.25
                  + (s3.rg * 2.0 - 1.0) * (0.18 * (0.35 + 0.65 * fineWeight))
                  + (s4.rg * 2.0 - 1.0) * (0.13 * fineWeight);

    float waveCrest = clamp(0.46 + vWaveH * 0.78 + (s1.a - 0.5) * 0.42 + (s2.a - 0.5) * 0.34, 0.0, 1.0);

    // ========================================================================
    // ABOVE-WATER VIEW: Rolling 3D swells + morphing organic swirls & dancing glints
    // ========================================================================
    if (uCamAboveWater == 1) {
        vec2 swellSlope  = vWaveGrad * 2.05 + (warpA * 0.14 + warpB * 0.11);
        vec2 rippleSlope = swellSlope + texSlope * 0.68 + shoreRippleGrad * 0.85;

        vec3 Nswell = normalize(vec3(-swellSlope.x, 1.0, -swellSlope.y));
        vec3 Nup    = normalize(vec3(-rippleSlope.x, 1.0, -rippleSlope.y));

        float cosV = clamp(dot(Nup, V), 0.0, 1.0);
        float fresnel = 0.02 + 0.98 * pow(1.0 - cosV, 5.0);

        vec3 R = reflect(rayDir, Nup);
        R.y = max(R.y, 0.02);
        vec3 reflCol = sampleDaySkyWater(normalize(R), L, true);

        // Refract underwater depth lookup along live wave normals so reefs shimmer beneath the waves
        vec2 refractOff = Nup.xz * (0.026 * clamp(1.0 - dist * 0.012, 0.15, 1.0));
        vec2 refrUV = clamp(screenUV + refractOff, 0.001, 0.999);
        float rawRefrDepth = texture(uSceneDepthTex, refrUV).r;
        float refrLin = linearizeDepth(rawRefrDepth);
        bool validRefr = (rawRefrDepth < 0.9992 && refrLin > waterLin);
        float useSceneLin = validRefr ? refrLin : sceneLin;

        float waterThickness = (rawSceneDepth < 0.9992 && useSceneLin > waterLin)
                                   ? max(useSceneLin - waterLin, 0.0) : 80.0;
        float shallowMask = exp(-waterThickness * 0.052);
        vec3 deepOcean   = mix(uWaterColorDeep * 0.70, vec3(0.010, 0.10, 0.24), 0.50);
        vec3 shallowTurq = mix(uWaterColorShallow * 1.18, vec3(0.06, 0.60, 0.72), 0.35);
        vec3 waterBody   = mix(deepOcean, shallowTurq, shallowMask);

        // Translucent emerald-turquoise subsurface glow through sunlit rolling crests
        float sss = pow(clamp(dot(V, -L + Nswell * 0.52), 0.0, 1.0), 2.8) * smoothstep(0.35, 0.95, waveCrest);
        float crestLight = smoothstep(0.42, 0.94, waveCrest) * clamp(dot(Nswell, L) * 0.5 + 0.5, 0.0, 1.0);
        waterBody += shallowTurq * (sss * 0.62 + crestLight * 0.20);

        vec3 surfaceCol = mix(waterBody, reflCol, clamp(fresnel * 1.02, 0.02, 0.95));

        // Dual-scale moving solar specular: broad rolling swell sheen + dancing fine sparkles
        vec3 H = normalize(L + V + vec3(0.0001));
        float ndhSwell  = clamp(dot(Nswell, H), 0.0, 1.0);
        float ndhRipple = clamp(dot(Nup, H), 0.0, 1.0);
        vec3 sunGlintCol = vec3(1.24, 1.15, 0.96);
        float glintSparkle = pow(max(ndhRipple, 0.0001), 220.0) * 3.8 * (0.45 + 0.55 * s3.a);
        float glintSheen   = pow(max(ndhSwell,  0.0001), 28.0)  * 0.28;
        surfaceCol += sunGlintCol * (glintSparkle + glintSheen);

        // White shoreline / whale foam + wind-swept crest foam wisps
        float crestFoam = smoothstep(0.76, 0.96, waveCrest) * smoothstep(0.48, 0.84, s2.a * 0.6 + s3.a * 0.4) * 0.44;
        float totalFoam = clamp(shoreFoam * 0.92 + crestFoam, 0.0, 1.0);
        vec3 dayFoamCol = vec3(0.97, 0.99, 1.02) * (0.86 + 0.24 * clamp(dot(Nup, L), 0.0, 1.0));
        surfaceCol = mix(surfaceCol, dayFoamCol, totalFoam);
        surfaceCol += vec3(1.0, 1.04, 1.08) * shoreBubbles * 0.35;

        // Daylight aerial perspective toward the horizon
        float airFog = 1.0 - exp(-dist * 0.00145);
        vec3 hazeDir = normalize(vec3(rayDir.x, 0.04, rayDir.z));
        vec3 horizonCol = sampleDaySkyWater(hazeDir, L, false);
        vec3 outCol = mix(surfaceCol, horizonCol, clamp(airFog * 0.78, 0.0, 0.85));

        FragColor = vec4(clamp(outCol, 0.0, 8.0), 1.0);
        return;
    }

    // ========================================================================
    // UNDERWATER VIEW: Snell's Window, TIR & Spectral Underwater Fog
    // ========================================================================
    vec2 totalSlope = vWaveGrad * 1.90 + texSlope * 0.85 + shoreRippleGrad;
    vec3 N = normalize(vec3(totalSlope.x, -1.0, totalSlope.y));

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
        float s2d = sunDot * sunDot;
        float s4d = s2d * s2d;
        float s16 = s4d * s4d * s4d * s4d;
        float s64 = s16 * s16 * s16 * s16;
        refractedCol += uSunColor * (s4d * 0.36 + s16 * 0.85 + s64 * 1.85);

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
    DEEPDIVE_WATER_FRAG_PART1
        DEEPDIVE_WATER_FRAG_PART2;

inline const char* WaterSurfaceFrag400 =
    "#version 400 core\n"
    DEEPDIVE_WATER_FRAG_PART1
        DEEPDIVE_WATER_FRAG_PART2;

#undef DEEPDIVE_WATER_FRAG_PART1
#undef DEEPDIVE_WATER_FRAG_PART2

// ============================================================================
// DUAL-MEDIUM SKYBOX (UNDERWATER GRADIENT + MIDDAY SUN & DISTANT CLOUDS)
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

uniform sampler3D uCloudShapeTex;
uniform sampler3D uCloudDetailTex;
uniform sampler2D uCloudWeatherTex;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uWaterColorDeep;
uniform vec3 uWaterColorShallow;
uniform float uWaterY;
uniform float uTime;
uniform int uCamAboveWater;

out vec4 FragColor;

const float EARTH_RADIUS = 60000.0;
const float CLOUD_BOTTOM = 1200.0;
const float CLOUD_TOP    = 3200.0;

float remap(float v, float lo1, float hi1, float lo2, float hi2) {
    return lo2 + (v - lo1) * (hi2 - lo2) / max(hi1 - lo1, 1e-5);
}

// Dual-lobe Henyey-Greenstein phase function
float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 0.0001);
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(denom, 1.5));
}

// Positive distance along ray (ro, rd) to sphere of radius R centered at origin
float intersectSphereFar(vec3 ro, vec3 rd, float R) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    float h = b * b - c;
    if (h < 0.0) return -1.0;
    return -b + sqrt(h);
}

// Tall cumulus tower profile: flat condensation base + rounded tapering dome
float cloudHeightGradient(float h, float cloudType) {
    float topH = mix(0.42, 0.98, cloudType);
    float botFade = smoothstep(0.0, 0.08, h);
    float topFade = 1.0 - smoothstep(topH * 0.36, topH, h);
    return botFade * topFade;
}

// 3D volumetric cloud density (strictly textureLod to prevent mip-explosion blur)
float sampleCloudDensity(vec3 pWorld, float h, float dist, float sunClear, bool highDetail) {
    const vec2 windDir = vec2(0.84, 0.54);

    // Subtle downwind shear gives towering cumulus natural vertical depth
    vec3 p = pWorld;
    p.xz += windDir * (h * 260.0 + uTime * 22.0);

    vec2 weatherUV = p.xz * 0.000072 + vec2(0.18, 0.31);
    vec4 weather = textureLod(uCloudWeatherTex, weatherUV, 0.0);
    float coverage  = weather.r * sunClear;
    float cloudType = weather.g;
    float moisture  = weather.a;

    if (coverage < 0.04) return 0.0;

    float hGrad = cloudHeightGradient(h, cloudType);
    if (hGrad <= 0.001) return 0.0;

    // Taper horizontal footprint with altitude so clouds rise as 3D domes, not flat slabs
    float effectiveCov = coverage * mix(1.0, 0.62, h * h);

    float shapeLod = clamp((dist - 5500.0) * 0.00010, 0.0, 1.2);
    vec4 shape = textureLod(uCloudShapeTex, p * 0.00055, shapeLod);
    float lowFreqFBM = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    float baseCloud = clamp(remap(shape.r, (1.0 - lowFreqFBM) * 0.44, 1.0, 0.0, 1.0), 0.0, 1.0);
    baseCloud *= hGrad;

    float covThresh = mix(0.76, 0.24, effectiveCov);
    float cloudCov = clamp(remap(baseCloud, covThresh, 1.0, 0.0, 1.0), 0.0, 1.0);
    if (cloudCov <= 0.001) return 0.0;

    if (highDetail) {
        // Crisp 3D cauliflower erosion along lobes and edges
        float detailLod = clamp((dist - 3200.0) * 0.00016, 0.0, 1.6);
        vec3 detailPos = p * 0.0029 - vec3(windDir.x, 0.12, windDir.y) * (uTime * 0.030);
        vec3 detail = textureLod(uCloudDetailTex, detailPos, detailLod).rgb;
        float highFreqFBM = detail.r * 0.625 + detail.g * 0.25 + detail.b * 0.125;
        float highFreqMod = mix(highFreqFBM, 1.0 - highFreqFBM, clamp(h * 3.8, 0.0, 1.0));
        cloudCov = clamp(remap(cloudCov, highFreqMod * 0.34, 1.0, 0.0, 1.0), 0.0, 1.0);
    }

    return cloudCov * moisture * mix(0.65, 1.15, smoothstep(0.0, 0.30, h));
}

// 6-step cone light marching toward the sun for sculpted 3D self-shadowing
float marchCloudLight(vec3 pos, vec3 planetCenter, vec3 L, float viewDist, float sunClear) {
    const vec3 coneOffsets[6] = vec3[6](
        vec3( 0.16,  0.10, -0.12),
        vec3(-0.20,  0.08,  0.17),
        vec3( 0.09, -0.15,  0.22),
        vec3(-0.12,  0.22, -0.10),
        vec3( 0.22, -0.05, -0.18),
        vec3( 0.00,  0.08,  0.00)
    );

    float lightStep = (CLOUD_TOP - CLOUD_BOTTOM) / 15.0;
    float totalDensity = 0.0;
    vec3 p = pos;

    for (int i = 0; i < 6; ++i) {
        float stepMul = (i == 5) ? 2.8 : 1.0;
        p += (L + coneOffsets[i] * (0.18 * float(i + 1))) * (lightStep * stepMul);

        float alt = length(p - planetCenter) - EARTH_RADIUS;
        float h = (alt - CLOUD_BOTTOM) / (CLOUD_TOP - CLOUD_BOTTOM);
        if (h < 0.0 || h > 1.0) break;

        totalDensity += sampleCloudDensity(p, h, viewDist, sunClear, i < 2) * (lightStep * stepMul);
    }

    return totalDensity * 0.0085;
}

void main() {
    vec4 worldTarget = uInvViewProj * vec4(vNDC, 1.0, 1.0);
    vec3 dir = normalize(worldTarget.xyz / max(abs(worldTarget.w), 0.0001) - uCamPos);
    vec3 L = normalize(uSunDir);

    if (uCamAboveWater == 1) {
        float elev = clamp(dir.y, 0.0, 1.0);
        vec3 zenith  = vec3(0.07, 0.32, 0.76);
        vec3 midSky  = vec3(0.24, 0.60, 0.94);
        vec3 horizon = vec3(0.75, 0.91, 0.99);

        vec3 skyCol = mix(horizon, midSky, smoothstep(0.0, 0.26, elev));
        skyCol = mix(skyCol, zenith, pow(elev, 0.58));

        if (dir.y < 0.0) {
            skyCol = mix(horizon * 0.86, vec3(0.06, 0.24, 0.42), smoothstep(0.0, -0.18, dir.y));
        }

        // Midday sun glow, corona & crisp solar disc
        float sunDot = clamp(dot(dir, L), 0.0, 1.0);
        vec3 middaySun = vec3(1.24, 1.15, 0.96);
        float mieWide  = pow(sunDot, 5.0)   * 0.16;
        float mieTight = pow(sunDot, 32.0)  * 0.44;
        float sunHalo  = pow(sunDot, 160.0) * 1.55;
        float sunDisc  = smoothstep(0.9985, 0.99945, sunDot) * 8.5;

        vec3 col = skyCol + middaySun * (mieWide + mieTight) * smoothstep(-0.04, 0.06, dir.y);

        // Subtle high-altitude cirrus streaks
        if (dir.y > 0.03) {
            vec2 cirrusUV = dir.xz / (dir.y + 0.16) * 0.12 + vec2(uTime * 0.00045, uTime * 0.00028);
            float cirrus = textureLod(uCloudWeatherTex, cirrusUV, 0.0).b;
            float cirrusMask = cirrus * smoothstep(0.03, 0.22, dir.y) * 0.14;
            vec3 cirrusCol = mix(vec3(0.86, 0.93, 1.00), vec3(1.08, 1.06, 1.02), 0.5 + 0.5 * sunDot);
            col = mix(col, cirrusCol, cirrusMask);
        }

        // Volumetric HZD / Nubis cloud raymarching inside spherical atmospheric shell
        float cloudTransmittance = 1.0;
        if (dir.y > 0.010) {
            vec3 planetCenter = vec3(0.0, uWaterY - EARTH_RADIUS, 0.0);
            vec3 ro = uCamPos - planetCenter;

            float tStart = intersectSphereFar(ro, dir, EARTH_RADIUS + CLOUD_BOTTOM);
            float tEnd   = intersectSphereFar(ro, dir, EARTH_RADIUS + CLOUD_TOP);

            if (tStart > 0.0 && tEnd > tStart) {
                tEnd = min(tEnd, tStart + 22000.0);

                int steps = int(mix(64.0, 48.0, smoothstep(0.08, 0.65, dir.y)));
                float stepLen = (tEnd - tStart) / float(steps);

                // Interleaved Gradient Noise ray jitter prevents slice banding
                float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
                float t = tStart + ign * stepLen;

                // Keep a soft organic opening around the midday sun so it gleams cleanly in zenith
                float sunClear = mix(0.22, 1.0, smoothstep(0.996, 0.945, sunDot));

                float cosTheta  = dot(dir, L);
                float hgForward = henyeyGreenstein(cosTheta, 0.66);
                float hgBack    = henyeyGreenstein(cosTheta, -0.25);
                float phase     = clamp(mix(hgBack, hgForward, 0.62) * 3.14159265, 0.50, 1.75);
                float silverRim = clamp(henyeyGreenstein(cosTheta, 0.86) * 0.45, 0.0, 0.85);

                vec3 scatteredLight = vec3(0.0);
                float firstHitT = tEnd;
                bool hasHit = false;

                for (int i = 0; i < 64; ++i) {
                    if (i >= steps || cloudTransmittance < 0.015) break;

                    vec3 pos = uCamPos + dir * t;
                    float alt = length(pos - planetCenter) - EARTH_RADIUS;
                    float h = clamp((alt - CLOUD_BOTTOM) / (CLOUD_TOP - CLOUD_BOTTOM), 0.0, 1.0);

                    // Two-stage Nubis lookup: skip detail & light march in clear blue sky
                    float baseCheck = sampleCloudDensity(pos, h, t, sunClear, false);
                    if (baseCheck > 0.003) {
                        float dens = sampleCloudDensity(pos, h, t, sunClear, true);
                        if (dens > 0.003) {
                            if (!hasHit) {
                                firstHitT = t;
                                hasHit = true;
                            }

                            float lightTau = marchCloudLight(pos, planetCenter, L, t, sunClear);

                            // Calibrated Beer-Powder + multi-scattering (strong 3D shadow contrast, no white clip)
                            float beer1  = exp(-lightTau);
                            float beer2  = exp(-lightTau * 0.25);
                            float powder = 1.0 - exp(-lightTau * 1.9);
                            float powderFactor = mix(0.52 + 0.68 * powder, 1.0, clamp(cosTheta * 0.5 + 0.5, 0.0, 1.0));

                            float directEnergy = beer1 * (phase * 0.72 + silverRim * 0.35) * powderFactor
                                               + beer2 * 0.26;

                            // Deep cool blue-grey shadowed undersides vs bright crisp sunlit upper domes
                            vec3 ambBottom   = vec3(0.20, 0.34, 0.54);
                            vec3 ambTop      = vec3(0.66, 0.78, 0.94);
                            vec3 skyFill     = mix(ambBottom, ambTop, smoothstep(0.0, 0.85, h));
                            vec3 oceanBounce = mix(uWaterColorShallow, vec3(0.18, 0.54, 0.68), 0.5) * (1.0 - h) * 0.08;
                            float heightAO   = mix(0.34, 1.0, pow(h, 0.75));
                            float depthAO    = mix(0.58, 1.0, exp(-dens * 1.6));

                            vec3 lum = middaySun * (directEnergy * 0.92)
                                     + (skyFill + oceanBounce) * (heightAO * depthAO * 0.48);

                            float stepExt = dens * stepLen * 0.0068;
                            float stepTrans = exp(-stepExt);

                            scatteredLight += cloudTransmittance * lum * (1.0 - stepTrans);
                            cloudTransmittance *= stepTrans;
                        }
                    }

                    t += stepLen;
                }

                float cloudAlpha = 1.0 - cloudTransmittance;
                if (cloudAlpha > 0.001) {
                    vec3 cloudCol = scatteredLight / max(cloudAlpha, 0.0001);
                    vec3 hazeCol  = mix(horizon, skyCol, smoothstep(0.02, 0.25, dir.y));

                    float distVis   = exp(-max(firstHitT - 6000.0, 0.0) * 0.000038);
                    float horizMask = smoothstep(0.012, 0.075, dir.y);

                    cloudCol = mix(hazeCol, cloudCol, distVis);
                    float effAlpha = clamp(cloudAlpha * horizMask, 0.0, 1.0);

                    col = mix(col, cloudCol, effAlpha);
                    cloudTransmittance = 1.0 - effAlpha;
                }
            }
        }

        // Midday sun disc & corona gleaming cleanly in the zenith
        float sunVis = pow(clamp(cloudTransmittance, 0.0, 1.0), 0.75);
        col += middaySun * (sunHalo * (0.28 + 0.72 * sunVis) + sunDisc * sunVis)
             * smoothstep(-0.04, 0.06, dir.y);

        FragColor = vec4(clamp(col, 0.0, 8.0), 1.0);
        return;
    }

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
// VOLUMETRIC GOD RAYS, PARTICLES, AIRBORNE SPRAY & POST-PROCESSING
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
uniform float uWaterY;

out vec3 vColor;
out float vType;
out float vClipped;

void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
    float dist = max(length(uCamPos - aPos), 1.0);
    float size = abs(aColorSize.a);
    gl_PointSize = clamp((size * uViewportHeight * 0.6) / dist, 1.5, 42.0);
    vColor = aColorSize.rgb;
    vType = aColorSize.a < 0.0 ? 1.0 : 0.0;
    vClipped = (aPos.y > uWaterY - 0.35) ? 1.0 : 0.0;
}
)";

inline const char* ParticleFrag = R"(#version 330 core
in vec3 vColor;
in float vType;
in float vClipped;
out vec4 FragColor;

void main() {
    if (vClipped > 0.5) discard;
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

inline const char* SprayVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColorSize; // rgb: tint * alpha, a: >0 mist puff, <0 droplet

uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform float uViewportHeight;

out vec3 vWorldPos;
out vec3 vTint;
out float vAlpha;
out float vIsDrop;
out float vSeed;

void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
    float dist = max(length(uCamPos - aPos), 0.75);
    bool isDrop = (aColorSize.a < 0.0);
    float size = abs(aColorSize.a);
    float maxPx = isDrop ? 30.0 : 92.0;
    float scaleMul = isDrop ? 0.58 : 0.78;
    gl_PointSize = clamp((size * uViewportHeight * scaleMul) / dist, 2.0, maxPx);

    float alpha = clamp(max(aColorSize.r, max(aColorSize.g, aColorSize.b)), 0.0, 1.0);
    vWorldPos = aPos;
    vTint     = aColorSize.rgb / max(alpha, 0.001);
    vAlpha    = alpha;
    vIsDrop   = isDrop ? 1.0 : 0.0;
    vSeed     = fract(sin(float(gl_VertexID) * 12.9898 + 4.1414) * 43758.5453);
}
)";

inline const char* SprayFrag = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vTint;
in float vAlpha;
in float vIsDrop;
in float vSeed;

uniform sampler2D uRockTex;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform float uTime;

out vec4 FragColor;

void main() {
    if (vAlpha <= 0.005) discard;
    vec2 uv = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(uv, uv);
    if (r2 > 1.0) discard;

    vec3 viewVec = vWorldPos - uCamPos;
    float dist = length(viewVec);
    vec3 D = dist > 0.0001 ? (viewVec / dist) : vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(uSunDir);

    // Sunlit forward scattering ("silver lining" when spray is backlit by midday sun)
    float sunDot = clamp(dot(D, L), 0.0, 1.0);
    float forwardScatter = pow(sunDot, 5.0) * 0.55;
    vec3 middaySun = vec3(1.18, 1.12, 0.98);

    vec3 col;
    float alpha;

    if (vIsDrop > 0.5) {
        // Crisp glistening water droplet with bright solar specular glint
        float core = smoothstep(1.0, 0.25, r2);
        float rim  = smoothstep(0.35, 0.88, r2) * (1.0 - smoothstep(0.88, 1.0, r2));
        float glint = pow(clamp(1.0 - length(uv - vec2(-0.30, -0.32)) * 1.65, 0.0, 1.0), 3.0);

        col = mix(vec3(0.76, 0.92, 0.99), vec3(1.04, 1.06, 1.08), core * 0.65) * vTint;
        col += middaySun * (glint * 0.95 + forwardScatter * 0.70);
        alpha = clamp((core * 0.68 + rim * 0.45 + glint * 0.45) * vAlpha, 0.0, 0.96);
    } else {
        // Soft sunlit mist / whale blow vapour puff broken up by rotated noise
        float ang = vSeed * 6.2831853 + uTime * 0.25 * (vSeed - 0.5);
        float c = cos(ang), s = sin(ang);
        vec2 rotUV = mat2(c, -s, s, c) * (uv * 0.32) + vec2(vSeed, fract(vSeed * 7.13));
        float n = texture(uRockTex, rotUV).a;

        float radial = (1.0 - r2);
        float puff = radial * radial * smoothstep(0.18, 0.86, n * 0.65 + radial * 0.55);
        if (puff <= 0.005) discard;

        // Subtle vertical sky/shadow gradient across the puff
        float topShade = clamp(0.78 - uv.y * 0.22, 0.55, 1.05);
        col = mix(vec3(0.78, 0.89, 0.96), vec3(1.06, 1.07, 1.05), topShade) * vTint;
        col += middaySun * (forwardScatter * 0.65);
        alpha = clamp(puff * vAlpha * 0.78, 0.0, 0.85);
    }

    // Subtle daylight aerial fade for distant whale blow spouts
    float airFog = 1.0 - exp(-dist * 0.00145);
    vec3 horizonHaze = vec3(0.75, 0.91, 0.99);
    col = mix(col, horizonHaze, clamp(airFog * 0.68, 0.0, 0.78));

    FragColor = vec4(clamp(col, 0.0, 6.0), alpha);
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
uniform sampler2D uRockTex;

uniform vec2 uInvResolution;
uniform vec3 uSunScreenPos;
uniform vec3 uWaterTint;
uniform float uSurfaceProximity;
uniform float uTime;
uniform int uEnablePost;
uniform int uCamAboveWater;
uniform float uSplashAmount;
uniform float uLensWetness;
uniform float uBulletTime;

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

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Procedural sliding water droplets on the camera lens when breaching into air
vec3 evalLensDroplets(vec2 uv, float aspect, float wetness, float t) {
    if (wetness <= 0.005) return vec3(0.0);

    vec2 gridScale = vec2(9.5 * aspect, 9.5);
    vec2 gUV = uv * gridScale;
    gUV.y += t * 0.55; // droplets slowly slide down the lens

    vec2 cell = floor(gUV);
    vec2 f = fract(gUV) - 0.5;

    float h1 = hash21(cell);
    float h2 = hash21(cell + 17.3);
    float h3 = hash21(cell + 43.7);
    if (h1 < 0.42) return vec3(0.0);

    vec2 center = vec2((h2 - 0.5) * 0.52, (h3 - 0.5) * 0.48);
    vec2 d = f - center;
    d.y *= 0.80; // slight vertical teardrop elongation
    float r = 0.11 + 0.12 * fract(h1 * 7.31);
    float dist = length(d);
    float mask = smoothstep(r, r * 0.35, dist) * wetness;
    if (mask <= 0.001) return vec3(0.0);

    vec2 refractOff = -(d / max(r, 0.01)) * 0.018 * mask;
    float spec = pow(clamp(1.0 - length(d - vec2(-0.04, 0.05)) / max(r * 0.75, 0.01), 0.0, 1.0), 3.0) * mask;
    return vec3(refractOff, spec);
}

void main() {
    if (uEnablePost == 0) {
        FragColor = vec4(clamp(sanitizeHDR(texture(uSceneColorTex, vUV).rgb), 0.0, 1.0), 1.0);
        return;
    }

    float aspect = uInvResolution.y / max(uInvResolution.x, 1e-6);
    vec2 edgeVec = vUV - 0.5;
    float radialDist = length(edgeVec);

    vec3 mapped;

    if (uCamAboveWater == 1) {
        // Above-water: crisp sunlit air + dripping lens droplets + soft solar bloom
        vec3 drop = evalLensDroplets(vUV, aspect, uLensWetness, uTime);
        vec2 uv = clamp(vUV + drop.xy, 0.001, 0.999);

        float caAmp = 0.0010 + uBulletTime * 0.0022;
        vec2 caShift = edgeVec * (caAmp * radialDist);

        vec3 sceneCol;
        sceneCol.r = sanitizeHDR(texture(uSceneColorTex, clamp(uv + caShift, 0.001, 0.999)).rgb).r;
        sceneCol.g = sanitizeHDR(texture(uSceneColorTex, uv).rgb).g;
        sceneCol.b = sanitizeHDR(texture(uSceneColorTex, clamp(uv - caShift, 0.001, 0.999)).rgb).b;

        vec3 bloom = sanitizeHDR(texture(uBloomTex, uv).rgb);
        sceneCol += bloom * 0.18;

        // Subtle warm solar glare when looking toward midday sun
        if (uSunScreenPos.z > 0.0) {
            float sunDist = length(uv - uSunScreenPos.xy);
            float glare = exp(-sunDist * 2.8) * clamp(uSunScreenPos.z, 0.0, 1.0);
            sceneCol += vec3(1.05, 0.98, 0.84) * glare * 0.14;
        }

        // Droplet specular glint
        sceneCol += vec3(0.95, 1.02, 1.08) * drop.z * 0.65;

        mapped = acesToneMapping(sceneCol * (1.06 + uBulletTime * 0.05));
        mapped = pow(max(mapped, vec3(0.0001)), vec3(0.97));

        float vigStrength = mix(0.84, 0.68, uBulletTime);
        float vignette = smoothstep(1.08, 0.30, radialDist);
        mapped *= mix(vigStrength, 1.0, vignette);
    } else {
        // Underwater: subtle optical refraction, depth turbidity blur, bloom & god rays
        vec2 waveOffset = vec2(
            sin(vUV.y * 13.0 + uTime * 1.45) * cos(vUV.x * 10.0 + uTime * 0.95),
            cos(vUV.x * 12.0 - uTime * 1.15) * sin(vUV.y * 11.0 + uTime * 0.85)
        ) * 0.0012;
        vec2 uv = clamp(vUV + waveOffset, 0.001, 0.999);

        float rawDepth = texture(uSceneDepthTex, uv).r;
        float linDepth = linearizeDepth(rawDepth);
        float turbidityBlur = clamp((linDepth - 14.0) / 90.0, 0.05, 1.0);

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

        mapped = acesToneMapping(sceneCol * 1.18);
        mapped = pow(max(mapped, vec3(0.0001)), vec3(0.96));

        float vignette = smoothstep(1.02, 0.28, radialDist);
        mapped *= mix(0.60, 1.0, vignette);
    }

    // Screen-space white foam & radial sea spray burst when crossing water/air boundary
    if (uSplashAmount > 0.002) {
        float sp = clamp(uSplashAmount, 0.0, 1.0);
        vec2 aspectUV = vec2(edgeVec.x * aspect, edgeVec.y);
        float rNorm = length(aspectUV);
        vec2 radialDir = (rNorm > 0.001) ? (aspectUV / rNorm) : vec2(0.0, 1.0);

        float n1 = texture(uRockTex, vUV * vec2(2.6, 2.2) - radialDir * (1.0 - sp) * 0.45 + vec2(0.0, uTime * 0.14)).a;
        float n2 = texture(uRockTex, vUV * vec2(6.5, 5.5) + vec2(uTime * 0.18, -uTime * 0.24)).a;
        float foamNoise = n1 * 0.62 + n2 * 0.38;

        // Strong full-screen froth right at impact (sp > 0.65), clearing from center outward
        float edgeBias = smoothstep(0.05, 0.72, rNorm);
        float surgeCore = smoothstep(0.52, 0.94, sp);
        float sprayVeil = smoothstep(0.66 - sp * 0.56 - edgeBias * 0.22, 0.88 - sp * 0.36, foamNoise);
        float droplets  = smoothstep(0.76 - sp * 0.22, 0.90 - sp * 0.12, n2) * sp;

        float foamAlpha = clamp(surgeCore * (0.58 + 0.38 * n1) + sprayVeil * sp * 0.78 + droplets * 0.45, 0.0, 0.94);
        vec3 foamCol = mix(vec3(0.84, 0.95, 0.99), vec3(0.99, 1.0, 1.0), smoothstep(0.35, 0.80, foamNoise));
        mapped = mix(mapped, foamCol, foamAlpha);
    }

    FragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
)";

} // namespace Shaders
