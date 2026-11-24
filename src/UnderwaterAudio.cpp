#include "UnderwaterAudio.h"
#include <QMediaDevices>
#include <QAudioDevice>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
constexpr float PI     = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;
}

UnderwaterSynthDevice::UnderwaterSynthDevice(const QAudioFormat& format, QObject* parent)
    : QIODevice(parent), m_format(format) {
    m_sampleRate = std::max(8000, format.sampleRate());

    // Буферы задержки для подводного эха (~390 мс левый, ~530 мс правый)
    m_delayBufL.assign(static_cast<size_t>(m_sampleRate * 0.39f), 0.0f);
    m_delayBufR.assign(static_cast<size_t>(m_sampleRate * 0.53f), 0.0f);
}

void UnderwaterSynthDevice::setParameters(float depthFactor, float swimSpeed, int biomeIndex,
                                          bool aboveWater, float timeScale) {
    m_depthFactor.store(std::clamp(depthFactor, 0.0f, 1.0f), std::memory_order_relaxed);
    m_swimSpeed.store(std::clamp(swimSpeed, 0.0f, 1.0f), std::memory_order_relaxed);
    m_biomeIndex.store(std::clamp(biomeIndex, 0, 2), std::memory_order_relaxed);
    m_aboveWater.store(aboveWater ? 1.0f : 0.0f, std::memory_order_relaxed);
    m_timeScale.store(std::clamp(timeScale, 0.2f, 1.0f), std::memory_order_relaxed);
}

void UnderwaterSynthDevice::setMasterVolume(float volume) {
    m_masterVolume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

void UnderwaterSynthDevice::setMuted(bool muted) {
    m_muted.store(muted, std::memory_order_relaxed);
}

void UnderwaterSynthDevice::triggerWhaleCall() {
    m_manualWhaleReq.store(true, std::memory_order_relaxed);
}

void UnderwaterSynthDevice::triggerSplash(bool exitingWater, float intensity) {
    m_splashReqIntensity.store(std::clamp(intensity, 0.2f, 1.0f), std::memory_order_relaxed);
    m_splashReqType.store(exitingWater ? 1 : 2, std::memory_order_release);
}

float UnderwaterSynthDevice::nextRandom01() {
    m_rngState ^= m_rngState << 13u;
    m_rngState ^= m_rngState >> 17u;
    m_rngState ^= m_rngState << 5u;
    return static_cast<float>(m_rngState & 0x00FFFFFFu) / 16777215.0f;
}

void UnderwaterSynthDevice::BiquadSVF::process(float input, float cutoffNorm, float q,
                                               float& outLow, float& outBand, float* outHigh) {
    cutoffNorm = std::clamp(cutoffNorm, 0.001f, 0.46f);
    q = std::max(q, 0.35f);
    low += cutoffNorm * band;
    const float high = input - low - q * band;
    band += cutoffNorm * high;
    outLow = low;
    outBand = band;
    if (outHigh) *outHigh = high;
}

void UnderwaterSynthDevice::spawnBubble(float swimSpeed) {
    for (auto& b : m_bubbles) {
        if (!b.active) {
            b.active = true;
            b.phase = 0.0f;
            b.freq = 220.0f + nextRandom01() * 340.0f;
            b.chirpRate = 9.0f + nextRandom01() * 14.0f;
            b.env = 1.0f;
            const float durationSec = 0.035f + nextRandom01() * 0.055f;
            b.decay = std::exp(-4.5f / (durationSec * m_sampleRate));
            b.pan = 0.15f + nextRandom01() * 0.70f;
            b.gain = (0.045f + nextRandom01() * 0.055f) * (1.0f + swimSpeed * 0.5f);
            break;
        }
    }
}

void UnderwaterSynthDevice::startWhaleCall() {
    m_whale.active = true;
    m_whale.age = 0.0f;
    m_whale.duration = 3.2f + nextRandom01() * 2.4f;
    m_whale.phase1 = 0.0f;
    m_whale.phase2 = 0.0f;
    m_whale.phase3 = 0.0f;
    m_whale.vibratoPhase = 0.0f;
    m_whale.contourType = static_cast<int>(nextRandom01() * 3.0f) % 3;

    const float basePitch = 135.0f + nextRandom01() * 115.0f;
    m_whale.startFreq = basePitch;
    m_whale.midFreq   = basePitch * (1.28f + nextRandom01() * 0.35f);
    m_whale.endFreq   = basePitch * (0.68f + nextRandom01() * 0.20f);
    m_whale.pan       = 0.15f + nextRandom01() * 0.70f;
}

void UnderwaterSynthDevice::startSeagullCall() {
    for (auto& g : m_seagulls) {
        if (!g.active) {
            g.active = true;
            g.age = 0.0f;
            g.pauseTimer = 0.0f;
            g.syllablesLeft = 1 + (static_cast<int>(nextRandom01() * 3.0f) % 3); // 1..3 слога
            g.duration = (g.syllablesLeft == 1)
                             ? (0.48f + nextRandom01() * 0.28f)
                             : (0.24f + nextRandom01() * 0.14f);
            g.phase = 0.0f;
            g.raspPhase = 0.0f;
            g.startFreq = 1950.0f + nextRandom01() * 580.0f;
            g.endFreq   = g.startFreq * (0.62f + nextRandom01() * 0.12f);
            g.pan       = 0.10f + nextRandom01() * 0.80f;
            g.gain      = 0.085f + nextRandom01() * 0.060f;
            g.formant   = {};
            break;
        }
    }
}

void UnderwaterSynthDevice::startSplash(bool exitingWater, float intensity) {
    // Выбираем свободный слот, либо переиспользуем самый старый всплеск
    SplashVoice* slot = &m_splashes[0];
    for (auto& s : m_splashes) {
        if (!s.active) {
            slot = &s;
            break;
        }
        if (s.age > slot->age) {
            slot = &s;
        }
    }

    slot->active = true;
    slot->exitingWater = exitingWater;
    slot->age = 0.0f;
    slot->duration = exitingWater ? (0.72f + intensity * 0.28f) : (0.82f + intensity * 0.32f);
    slot->intensity = std::clamp(intensity, 0.25f, 1.0f);
    slot->thumpPhase = 0.0f;
    slot->filterL = {};
    slot->filterR = {};

    // При приводнении сразу порождаем вихрь подводных пузырьков
    if (!exitingWater) {
        const int burst = 3 + static_cast<int>(intensity * 4.0f);
        for (int i = 0; i < burst; ++i) {
            spawnBubble(0.95f);
        }
    }
}

qint64 UnderwaterSynthDevice::writeData(const char* /*data*/, qint64 /*len*/) {
    return 0;
}

qint64 UnderwaterSynthDevice::bytesAvailable() const {
    return 65536 + QIODevice::bytesAvailable();
}

qint64 UnderwaterSynthDevice::readData(char* data, qint64 maxlen) {
    if (maxlen <= 0) return 0;

    const int channels = std::max(1, m_format.channelCount());
    const int bytesPerSample = std::max(2, m_format.bytesPerSample());
    const int frameBytes = channels * bytesPerSample;
    const int framesToRender = static_cast<int>(maxlen) / frameBytes;
    if (framesToRender <= 0) return 0;

    const float dt = 1.0f / static_cast<float>(m_sampleRate);
    const float targetDepth     = m_depthFactor.load(std::memory_order_relaxed);
    const float targetSpeed     = m_swimSpeed.load(std::memory_order_relaxed);
    const int   biomeIdx        = m_biomeIndex.load(std::memory_order_relaxed);
    const float targetAir       = m_aboveWater.load(std::memory_order_relaxed);
    const float targetTimeScale = m_timeScale.load(std::memory_order_relaxed);
    const float targetVol       = m_muted.load(std::memory_order_relaxed)
                                ? 0.0f
                                : m_masterVolume.load(std::memory_order_relaxed);

    if (m_manualWhaleReq.exchange(false, std::memory_order_relaxed)) {
        startWhaleCall();
    }

    const int splashReq = m_splashReqType.exchange(0, std::memory_order_acquire);
    if (splashReq != 0) {
        const float splashInt = m_splashReqIntensity.load(std::memory_order_relaxed);
        startSplash(splashReq == 1, splashInt);
    }

    // Резкий (~35 мс) безщелчковый кроссфейд между подводным и воздушным миксом
    const float airSmoothAlpha = 1.0f - std::exp(-dt / 0.035f);
    const float tsSmoothAlpha  = 1.0f - std::exp(-dt / 0.080f);

    // Аккорды биолюминесцентного пэда для трех биомов (Гц)
    static const float biomeChords[3][4] = {
        {110.0f, 164.81f, 220.0f, 246.94f}, // Изумрудный риф (A2 Asus2)
        {98.0f,  146.83f, 220.0f, 293.66f}, // Фиолетовый грот (G2 / Dsus4)
        {73.42f, 110.0f,  146.83f, 164.81f} // Глубоководная бездна (D2 темный квинтаккорд)
    };
    const float* chord = biomeChords[biomeIdx % 3];

    char* outPtr = data;

    for (int f = 0; f < framesToRender; ++f) {
        m_dspTime += dt;

        // Плавная интерполяция параметров без щелчков
        m_smoothDepth     += (targetDepth     - m_smoothDepth)     * 0.0005f;
        m_smoothSpeed     += (targetSpeed     - m_smoothSpeed)     * 0.0008f;
        m_smoothVol       += (targetVol       - m_smoothVol)       * 0.0015f;
        m_smoothAir       += (targetAir       - m_smoothAir)       * airSmoothAlpha;
        m_smoothTimeScale += (targetTimeScale - m_smoothTimeScale) * tsSmoothAlpha;

        const float airMix = std::clamp(m_smoothAir, 0.0f, 1.0f);
        const float uwMix  = 1.0f - airMix;

        // В момент выхода в воздух гарантируем скорый крик чайки (даже при коротком прыжке)
        const bool nowAbove = (airMix > 0.45f);
        if (nowAbove && !m_wasAboveWater) {
            m_nextGullTimer = std::min(m_nextGullTimer, 0.16f + nextRandom01() * 0.34f);
        }
        m_wasAboveWater = nowAbove;

        const float whiteL = nextRandom01() * 2.0f - 1.0f;
        const float whiteR = nextRandom01() * 2.0f - 1.0f;

        // ====================================================================
        // ПОДВОДНЫЙ КОНТУР (утробный гул, обтекание, пузырьки, кит, пэд, эхо)
        // ====================================================================
        m_brownL = (m_brownL + 0.025f * whiteL) * 0.975f;
        m_brownR = (m_brownR + 0.025f * whiteR) * 0.975f;

        const float waveLfoL = 0.5f + 0.5f * std::sin(static_cast<float>(m_dspTime * 0.55));
        const float waveLfoR = 0.5f + 0.5f * std::cos(static_cast<float>(m_dspTime * 0.43 + 1.2));

        const float rumbleCutoffHz = (65.0f + (1.0f - m_smoothDepth) * 95.0f) + waveLfoL * 45.0f;
        const float rumbleCutNorm  = TWO_PI * rumbleCutoffHz / m_sampleRate;

        float rumLowL = 0.0f, rumBandL = 0.0f;
        float rumLowR = 0.0f, rumBandR = 0.0f;
        m_rumbleFilterL.process(m_brownL * 2.8f, rumbleCutNorm, 0.65f, rumLowL, rumBandL);
        m_rumbleFilterR.process(m_brownR * 2.8f, rumbleCutNorm * (0.9f + 0.2f * waveLfoR), 0.65f, rumLowR, rumBandR);

        const float whooshHz = 160.0f + m_smoothSpeed * 320.0f;
        const float whooshCutNorm = TWO_PI * whooshHz / m_sampleRate;
        float whLowL = 0.0f, whBandL = 0.0f;
        float whLowR = 0.0f, whBandR = 0.0f;
        m_whooshFilterL.process(whiteL, whooshCutNorm, 0.45f, whLowL, whBandL);
        m_whooshFilterR.process(whiteR, whooshCutNorm, 0.45f, whLowR, whBandR);

        const float swimGain = m_smoothSpeed * 0.16f;
        float uwDryL = rumLowL * 0.38f + (whBandL * 0.7f + whLowL * 0.3f) * swimGain;
        float uwDryR = rumLowR * 0.38f + (whBandR * 0.7f + whLowR * 0.3f) * swimGain;

        // Полифонические пузырьки (спавнятся только под водой)
        if (uwMix > 0.25f) {
            const float bubbleRatePerSec = (1.6f + m_smoothSpeed * 5.5f) * uwMix;
            if (nextRandom01() < bubbleRatePerSec * dt) {
                spawnBubble(m_smoothSpeed);
            }
        }

        float fxSendL = 0.0f;
        float fxSendR = 0.0f;

        for (auto& b : m_bubbles) {
            if (!b.active) continue;
            b.env *= b.decay;
            if (b.env < 0.0008f) {
                b.active = false;
                continue;
            }
            const float currentFreq = b.freq * (1.0f + (1.0f - b.env) * b.chirpRate * 0.08f);
            b.phase += TWO_PI * currentFreq * dt;
            if (b.phase > TWO_PI) b.phase -= TWO_PI;

            const float attack = std::min(1.0f, (1.0f - b.env) * 14.0f);
            const float sig = (std::sin(b.phase) + 0.25f * std::sin(b.phase * 2.0f)) * b.env * attack * b.gain;

            uwDryL  += sig * (1.0f - b.pan);
            uwDryR  += sig * b.pan;
            fxSendL += sig * (1.0f - b.pan) * 0.65f;
            fxSendR += sig * b.pan * 0.65f;
        }

        // Пение китов (приглушённо продолжается под водой)
        m_nextWhaleTimer -= dt;
        if (m_nextWhaleTimer <= 0.0f && !m_whale.active) {
            startWhaleCall();
            m_nextWhaleTimer = 11.0f + nextRandom01() * 11.0f;
        }

        if (m_whale.active) {
            m_whale.age += dt;
            const float u = m_whale.age / m_whale.duration;
            if (u >= 1.0f) {
                m_whale.active = false;
            } else {
                float env = std::sin(u * PI);
                env = env * env * std::min(1.0f, u * 5.0f);

                float pitch = m_whale.startFreq;
                if (m_whale.contourType == 0) {
                    pitch = m_whale.startFreq + (m_whale.endFreq - m_whale.startFreq) * std::pow(u, 0.7f);
                } else if (m_whale.contourType == 1) {
                    const float arch = std::sin(u * PI);
                    pitch = m_whale.startFreq * (1.0f - arch) + m_whale.midFreq * arch;
                } else {
                    pitch = m_whale.startFreq + std::sin(u * TWO_PI * 1.5f) * 45.0f - u * 35.0f;
                }

                m_whale.vibratoPhase += TWO_PI * 4.8f * dt;
                pitch += std::sin(m_whale.vibratoPhase) * 3.5f * u;

                m_whale.phase1 += TWO_PI * pitch * dt;
                m_whale.phase2 += TWO_PI * (pitch * 2.003f) * dt;
                m_whale.phase3 += TWO_PI * (pitch * 3.01f) * dt;
                if (m_whale.phase1 > TWO_PI) m_whale.phase1 -= TWO_PI;
                if (m_whale.phase2 > TWO_PI) m_whale.phase2 -= TWO_PI;
                if (m_whale.phase3 > TWO_PI) m_whale.phase3 -= TWO_PI;

                const float mod = std::sin(m_whale.phase2) * (0.45f + 0.35f * std::sin(u * PI));
                const float whaleSig = (std::sin(m_whale.phase1 + mod) * 0.65f
                                        + std::sin(m_whale.phase2) * 0.25f
                                        + std::sin(m_whale.phase3) * 0.10f) * env * 0.14f;

                uwDryL  += whaleSig * (1.0f - m_whale.pan) * 0.5f;
                uwDryR  += whaleSig * m_whale.pan * 0.5f;
                fxSendL += whaleSig * (1.0f - m_whale.pan) * 1.15f;
                fxSendR += whaleSig * m_whale.pan * 1.15f;
            }
        }

        // Биолюминесцентный гармонический пэд
        float padL = 0.0f, padR = 0.0f;
        for (int i = 0; i < 4; ++i) {
            m_padPhases[i] += TWO_PI * chord[i] * dt;
            if (m_padPhases[i] > TWO_PI) m_padPhases[i] -= TWO_PI;
            const float lfo = 0.55f + 0.45f * std::sin(static_cast<float>(m_dspTime * (0.17 + i * 0.07) + i * 1.5));
            const float voice = std::sin(m_padPhases[i]) * lfo * 0.016f;
            if (i % 2 == 0) {
                padL += voice * 0.8f;
                padR += voice * 0.4f;
            } else {
                padL += voice * 0.4f;
                padR += voice * 0.8f;
            }
        }
        fxSendL += padL;
        fxSendR += padR;

        // Подводная стерео-реверберация каньона
        const float delayedL = m_delayBufL[m_delayIdxL];
        const float delayedR = m_delayBufR[m_delayIdxR];

        m_delayLpL += 0.12f * (delayedL - m_delayLpL);
        m_delayLpR += 0.12f * (delayedR - m_delayLpR);

        m_delayBufL[m_delayIdxL] = fxSendL + m_delayLpR * 0.56f;
        m_delayBufR[m_delayIdxR] = fxSendR + m_delayLpL * 0.56f;

        if (++m_delayIdxL >= m_delayBufL.size()) m_delayIdxL = 0;
        if (++m_delayIdxR >= m_delayBufR.size()) m_delayIdxR = 0;

        const float underwaterBusL = uwDryL + padL + m_delayLpL * 0.65f;
        const float underwaterBusR = uwDryR + padR + m_delayLpR * 0.65f;

        // ====================================================================
        // НАДВОДНЫЙ КОНТУР (чистый шум прибоя/волн, морской ветер, крики чаек)
        // ====================================================================
        m_surfWavePhase += dt * (0.45f + 0.55f * m_smoothTimeScale);

        const float swellL = 0.5f + 0.5f * std::sin(static_cast<float>(m_surfWavePhase * 0.85));
        const float swellR = 0.5f + 0.5f * std::sin(static_cast<float>(m_surfWavePhase * 0.71 + 1.65));
        const float surfEnvL = 0.38f + 0.62f * (swellL * swellL);
        const float surfEnvR = 0.38f + 0.62f * (swellR * swellR);

        // 1. Тело океанских волн (тёплый средне-низкий прибой)
        const float surfBodyCutL = TWO_PI * (240.0f + surfEnvL * 320.0f) / m_sampleRate;
        const float surfBodyCutR = TWO_PI * (240.0f + surfEnvR * 320.0f) / m_sampleRate;
        float sbLowL = 0.0f, sbBandL = 0.0f;
        float sbLowR = 0.0f, sbBandR = 0.0f;
        m_surfBodyFilterL.process(whiteL, surfBodyCutL, 0.85f, sbLowL, sbBandL);
        m_surfBodyFilterR.process(whiteR, surfBodyCutR, 0.85f, sbLowR, sbBandR);

        // 2. Чистое шипение пенных гребней и наката волн
        const float surfFoamCutL = TWO_PI * (1250.0f + surfEnvL * 1450.0f) / m_sampleRate;
        const float surfFoamCutR = TWO_PI * (1250.0f + surfEnvR * 1450.0f) / m_sampleRate;
        float sfLowL = 0.0f, sfBandL = 0.0f, sfHighL = 0.0f;
        float sfLowR = 0.0f, sfBandR = 0.0f, sfHighR = 0.0f;
        m_surfFoamFilterL.process(whiteL, surfFoamCutL, 0.72f, sfLowL, sfBandL, &sfHighL);
        m_surfFoamFilterR.process(whiteR, surfFoamCutR, 0.72f, sfLowR, sfBandR, &sfHighR);

        // 3. Свежий морской бриз / свист воздуха в полёте
        const float windGust = 0.60f + 0.40f * std::sin(static_cast<float>(m_dspTime * 0.37));
        const float windCut = TWO_PI * (620.0f + m_smoothSpeed * 1150.0f + windGust * 380.0f) / m_sampleRate;
        float wdLowL = 0.0f, wdBandL = 0.0f, wdHighL = 0.0f;
        float wdLowR = 0.0f, wdBandR = 0.0f, wdHighR = 0.0f;
        m_windFilterL.process(whiteL, windCut, 0.58f, wdLowL, wdBandL, &wdHighL);
        m_windFilterR.process(whiteR, windCut * 1.07f, 0.58f, wdLowR, wdBandR, &wdHighR);

        const float windGain = 0.055f + m_smoothSpeed * 0.095f;
        float airBusL = (sbLowL * 0.24f + sbBandL * 0.16f) * surfEnvL
                        + (sfBandL * 0.14f + sfHighL * 0.045f) * (0.45f + 0.55f * surfEnvL)
                        + (wdBandL * 0.80f + wdHighL * 0.18f) * windGain;
        float airBusR = (sbLowR * 0.24f + sbBandR * 0.16f) * surfEnvR
                        + (sfBandR * 0.14f + sfHighR * 0.045f) * (0.45f + 0.55f * surfEnvR)
                        + (wdBandR * 0.80f + wdHighR * 0.18f) * windGain;

        // 4. Крики чаек (2 голоса, активны над водой)
        if (airMix > 0.15f) {
            m_nextGullTimer -= dt;
            if (m_nextGullTimer <= 0.0f) {
                startSeagullCall();
                m_nextGullTimer = 1.9f + nextRandom01() * 3.4f;
            }
        }

        for (auto& g : m_seagulls) {
            if (!g.active) continue;

            if (g.pauseTimer > 0.0f) {
                g.pauseTimer -= dt;
                continue;
            }

            g.age += dt;
            const float u = g.age / g.duration;
            if (u >= 1.0f) {
                --g.syllablesLeft;
                if (g.syllablesLeft > 0) {
                    g.age = 0.0f;
                    g.pauseTimer = 0.09f + nextRandom01() * 0.10f;
                    g.duration = 0.22f + nextRandom01() * 0.16f;
                    g.startFreq *= (0.94f + nextRandom01() * 0.06f);
                    g.endFreq   *= (0.94f + nextRandom01() * 0.06f);
                } else {
                    g.active = false;
                }
                continue;
            }

            // Характерный контур крика чайки: резкий подхват вверх, затем протяжный спад
            float freq;
            if (u < 0.18f) {
                const float r = u / 0.18f;
                freq = g.startFreq * (0.90f + 0.22f * std::sin(r * PI * 0.5f));
            } else {
                const float d = (u - 0.18f) / 0.82f;
                freq = g.startFreq * 1.12f + (g.endFreq - g.startFreq * 1.12f) * std::pow(d, 0.82f);
            }

            // Лёгкая природная хрипотца в голосе птицы + едва заметное дыхание bullet-time
            g.raspPhase += TWO_PI * 58.0f * dt;
            if (g.raspPhase > TWO_PI) g.raspPhase -= TWO_PI;
            const float rasp = std::sin(g.raspPhase);
            const float pitchFactor = 0.93f + 0.07f * m_smoothTimeScale;
            const float instFreq = freq * pitchFactor * (1.0f + 0.022f * rasp);

            g.phase += TWO_PI * instFreq * dt;
            if (g.phase > TWO_PI) g.phase -= TWO_PI;

            // Резкий тембр с выраженными 2-й и 3-й гармониками + формантный резонанс
            const float rawBird = std::sin(g.phase) * 0.52f
                                  + std::sin(g.phase * 2.0f) * 0.34f
                                  + std::sin(g.phase * 3.0f) * 0.18f
                                  + std::sin(g.phase * 4.0f) * 0.08f;

            float gullLow = 0.0f, gullBand = 0.0f;
            const float formCut = TWO_PI * std::clamp(instFreq * 1.18f, 1400.0f, 3300.0f) / m_sampleRate;
            g.formant.process(rawBird * (1.0f + 0.16f * rasp), formCut, 0.52f, gullLow, gullBand);

            const float attack = std::min(1.0f, u * 14.0f);
            const float release = std::pow(std::max(0.0f, 1.0f - u), 1.35f);
            const float gullSig = (gullBand * 0.82f + rawBird * 0.18f) * attack * release * g.gain;

            airBusL += gullSig * (1.0f - g.pan);
            airBusR += gullSig * g.pan;
        }

        // ====================================================================
        // ВСПЛЕСКИ ПРОРЫВА ВОДЫ И ПРИВОДНЕНИЯ (поверх обоих миксов)
        // ====================================================================
        float splashBusL = 0.0f;
        float splashBusR = 0.0f;

        const float splashStepDt = dt * (0.68f + 0.32f * m_smoothTimeScale);
        for (auto& sp : m_splashes) {
            if (!sp.active) continue;
            sp.age += splashStepDt;
            const float u = sp.age / sp.duration;
            if (u >= 1.0f) {
                sp.active = false;
                continue;
            }

            const float attack = std::min(1.0f, sp.age * 95.0f);
            if (sp.exitingWater) {
                // Выныривание: яркий срыв водной плёнки + каскад шипящих брызг
                const float sprayEnv = attack * std::exp(-u * 4.4f);
                const float cutHz = 2600.0f * (1.0f - 0.62f * u) + 550.0f;
                const float cutNorm = TWO_PI * cutHz / m_sampleRate;
                float lL = 0.0f, bL = 0.0f, hL = 0.0f;
                float lR = 0.0f, bR = 0.0f, hR = 0.0f;
                sp.filterL.process(whiteL, cutNorm, 0.62f, lL, bL, &hL);
                sp.filterR.process(whiteR, cutNorm * 1.06f, 0.62f, lR, bR, &hR);

                const float popEnv = attack * std::exp(-sp.age * 24.0f);
                const float popFreq = 115.0f + std::min(sp.age * 1600.0f, 175.0f);
                sp.thumpPhase += TWO_PI * popFreq * dt;
                const float popSig = std::sin(sp.thumpPhase) * popEnv * 0.24f;

                const float gain = sp.intensity * 0.52f;
                splashBusL += ((bL * 0.65f + hL * 0.38f + lL * 0.15f) * sprayEnv + popSig) * gain;
                splashBusR += ((bR * 0.65f + hR * 0.38f + lR * 0.15f) * sprayEnv + popSig) * gain;
            } else {
                // Приводнение: тяжёлый басовый удар о воду + плотный пенный шлейф
                const float foamEnv = attack * std::exp(-u * 3.8f);
                const float cutHz = 1150.0f * std::exp(-u * 2.2f) + 190.0f;
                const float cutNorm = TWO_PI * cutHz / m_sampleRate;
                float lL = 0.0f, bL = 0.0f;
                float lR = 0.0f, bR = 0.0f;
                sp.filterL.process(whiteL, cutNorm, 0.58f, lL, bL);
                sp.filterR.process(whiteR, cutNorm * 0.94f, 0.58f, lR, bR);

                const float thumpEnv = attack * std::exp(-sp.age * 11.5f);
                const float thumpFreq = 48.0f + 125.0f * std::exp(-sp.age * 16.0f);
                sp.thumpPhase += TWO_PI * thumpFreq * dt;
                const float thumpSig = std::sin(sp.thumpPhase) * thumpEnv * 0.55f;

                const float gain = sp.intensity * 0.58f;
                splashBusL += ((lL * 0.55f + bL * 0.48f) * foamEnv + thumpSig) * gain;
                splashBusR += ((lR * 0.55f + bR * 0.48f) * foamEnv + thumpSig) * gain;
            }
        }

        // Финальное сведение двух сред + мягкое насыщение (tanh-лимитер)
        float mixL = (underwaterBusL * uwMix + airBusL * airMix + splashBusL) * m_smoothVol;
        float mixR = (underwaterBusR * uwMix + airBusR * airMix + splashBusR) * m_smoothVol;

        mixL = std::tanh(mixL);
        mixR = std::tanh(mixR);

        if (m_format.sampleFormat() == QAudioFormat::Float) {
            float* fltOut = reinterpret_cast<float*>(outPtr);
            fltOut[0] = mixL;
            if (channels > 1) fltOut[1] = mixR;
        } else if (m_format.sampleFormat() == QAudioFormat::Int16) {
            qint16* intOut = reinterpret_cast<qint16*>(outPtr);
            intOut[0] = static_cast<qint16>(std::clamp(mixL, -1.0f, 1.0f) * 32760.0f);
            if (channels > 1) intOut[1] = static_cast<qint16>(std::clamp(mixR, -1.0f, 1.0f) * 32760.0f);
        }
        outPtr += frameBytes;
    }

    return static_cast<qint64>(framesToRender) * frameBytes;
}

// ============================================================================
// UnderwaterAudioEngine
// ============================================================================

UnderwaterAudioEngine::UnderwaterAudioEngine(QObject* parent) : QObject(parent) {
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return;

    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);

    if (!dev.isFormatSupported(fmt)) {
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(fmt)) {
            fmt = dev.preferredFormat();
        }
    }

    m_synthDevice = new UnderwaterSynthDevice(fmt, this);
    m_synthDevice->open(QIODevice::ReadOnly);

    m_audioSink = new QAudioSink(dev, fmt, this);
    m_audioSink->setBufferSize(fmt.bytesForDuration(60000)); // 60 мс буфер для стабильности без задержек
    m_audioSink->start(m_synthDevice);
}

UnderwaterAudioEngine::~UnderwaterAudioEngine() {
    if (m_audioSink) {
        m_audioSink->stop();
    }
}

void UnderwaterAudioEngine::updateState(float camY, float swimSpeed, int biomeIndex,
                                        bool aboveWater, float timeScale) {
    if (!m_synthDevice) return;
    // Нормализуем глубину: 0.0 у поверхности (+28), 1.0 в глубоких каньонах (-24)
    const float depthFactor = std::clamp((28.0f - camY) / 52.0f, 0.0f, 1.0f);
    const float normSpeed   = std::clamp(swimSpeed / 16.0f, 0.0f, 1.0f);
    m_synthDevice->setParameters(depthFactor, normSpeed, biomeIndex, aboveWater, timeScale);
}

void UnderwaterAudioEngine::toggleMute() {
    m_muted = !m_muted;
    if (m_synthDevice) m_synthDevice->setMuted(m_muted);
}

void UnderwaterAudioEngine::adjustVolume(float delta) {
    m_volume = std::clamp(m_volume + delta, 0.0f, 1.0f);
    if (m_synthDevice) m_synthDevice->setMasterVolume(m_volume);
}

void UnderwaterAudioEngine::triggerWhaleCall() {
    if (m_synthDevice) m_synthDevice->triggerWhaleCall();
}

void UnderwaterAudioEngine::triggerSplash(bool exitingWater, float intensity) {
    if (m_synthDevice) m_synthDevice->triggerSplash(exitingWater, intensity);
}
