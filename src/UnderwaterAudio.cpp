#include "UnderwaterAudio.h"
#include <QMediaDevices>
#include <QAudioDevice>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
constexpr float TWO_PI = 6.28318530717958647692f;
}

UnderwaterSynthDevice::UnderwaterSynthDevice(const QAudioFormat& format, QObject* parent)
    : QIODevice(parent), m_format(format) {
    m_sampleRate = std::max(8000, format.sampleRate());

    // Буферы задержки для подводного эха (~390 мс левый, ~530 мс правый)
    m_delayBufL.assign(static_cast<size_t>(m_sampleRate * 0.39f), 0.0f);
    m_delayBufR.assign(static_cast<size_t>(m_sampleRate * 0.53f), 0.0f);
}

void UnderwaterSynthDevice::setParameters(float depthFactor, float swimSpeed, int biomeIndex) {
    m_depthFactor.store(std::clamp(depthFactor, 0.0f, 1.0f), std::memory_order_relaxed);
    m_swimSpeed.store(std::clamp(swimSpeed, 0.0f, 1.0f), std::memory_order_relaxed);
    m_biomeIndex.store(std::clamp(biomeIndex, 0, 2), std::memory_order_relaxed);
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

float UnderwaterSynthDevice::nextRandom01() {
    m_rngState ^= m_rngState << 13u;
    m_rngState ^= m_rngState >> 17u;
    m_rngState ^= m_rngState << 5u;
    return static_cast<float>(m_rngState & 0x00FFFFFFu) / 16777215.0f;
}

void UnderwaterSynthDevice::BiquadSVF::process(float input, float cutoffNorm, float q, float& outLow, float& outBand) {
    cutoffNorm = std::clamp(cutoffNorm, 0.001f, 0.45f);
    low += cutoffNorm * band;
    float high = input - low - q * band;
    band += cutoffNorm * high;
    outLow = low;
    outBand = band;
}

void UnderwaterSynthDevice::spawnBubble(float swimSpeed) {
    for (auto& b : m_bubbles) {
        if (!b.active) {
            b.active = true;
            b.phase = 0.0f;
            b.freq = 220.0f + nextRandom01() * 340.0f;
            b.chirpRate = 9.0f + nextRandom01() * 14.0f;
            b.env = 1.0f;
            float durationSec = 0.035f + nextRandom01() * 0.055f;
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

    float basePitch = 135.0f + nextRandom01() * 115.0f;
    m_whale.startFreq = basePitch;
    m_whale.midFreq   = basePitch * (1.28f + nextRandom01() * 0.35f);
    m_whale.endFreq   = basePitch * (0.68f + nextRandom01() * 0.20f);
    m_whale.pan       = 0.15f + nextRandom01() * 0.70f;
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
    const float targetDepth = m_depthFactor.load(std::memory_order_relaxed);
    const float targetSpeed = m_swimSpeed.load(std::memory_order_relaxed);
    const int   biomeIdx    = m_biomeIndex.load(std::memory_order_relaxed);
    const float targetVol   = m_muted.load(std::memory_order_relaxed)
                                ? 0.0f
                                : m_masterVolume.load(std::memory_order_relaxed);

    if (m_manualWhaleReq.exchange(false, std::memory_order_relaxed)) {
        startWhaleCall();
    }

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
        m_smoothDepth += (targetDepth - m_smoothDepth) * 0.0005f;
        m_smoothSpeed += (targetSpeed - m_smoothSpeed) * 0.0008f;
        m_smoothVol   += (targetVol   - m_smoothVol)   * 0.0015f;

        // --------------------------------------------------------------------
        // 1. Глубоководный шум океана и подводного течения (Brown Noise + SVF)
        // --------------------------------------------------------------------
        float whiteL = nextRandom01() * 2.0f - 1.0f;
        float whiteR = nextRandom01() * 2.0f - 1.0f;
        m_brownL = (m_brownL + 0.025f * whiteL) * 0.975f;
        m_brownR = (m_brownR + 0.025f * whiteR) * 0.975f;

        float waveLfoL = 0.5f + 0.5f * std::sin(static_cast<float>(m_dspTime * 0.55));
        float waveLfoR = 0.5f + 0.5f * std::cos(static_cast<float>(m_dspTime * 0.43 + 1.2));

        // На большой глубине фильтр закрывается, оставляя низкий суб-басовый гул
        float rumbleCutoffHz = (65.0f + (1.0f - m_smoothDepth) * 95.0f) + waveLfoL * 45.0f;
        float rumbleCutNorm  = TWO_PI * rumbleCutoffHz / m_sampleRate;

        float rumLowL = 0.0f, rumBandL = 0.0f;
        float rumLowR = 0.0f, rumBandR = 0.0f;
        m_rumbleFilterL.process(m_brownL * 2.8f, rumbleCutNorm, 0.65f, rumLowL, rumBandL);
        m_rumbleFilterR.process(m_brownR * 2.8f, rumbleCutNorm * (0.9f + 0.2f * waveLfoR), 0.65f, rumLowR, rumBandR);

        // --------------------------------------------------------------------
        // 2. Шум обтекания воды при плавании дайвера (Swim Whoosh)
        // --------------------------------------------------------------------
        float whooshHz = 160.0f + m_smoothSpeed * 320.0f;
        float whooshCutNorm = TWO_PI * whooshHz / m_sampleRate;
        float whLowL = 0.0f, whBandL = 0.0f;
        float whLowR = 0.0f, whBandR = 0.0f;
        m_whooshFilterL.process(whiteL, whooshCutNorm, 0.45f, whLowL, whBandL);
        m_whooshFilterR.process(whiteR, whooshCutNorm, 0.45f, whLowR, whBandR);

        float swimGain = m_smoothSpeed * 0.16f;
        float dryL = rumLowL * 0.38f + (whBandL * 0.7f + whLowL * 0.3f) * swimGain;
        float dryR = rumLowR * 0.38f + (whBandR * 0.7f + whLowR * 0.3f) * swimGain;

        // --------------------------------------------------------------------
        // 3. Полифонические пузырьки воздуха
        // --------------------------------------------------------------------
        float bubbleRatePerSec = 1.6f + m_smoothSpeed * 5.5f;
        if (nextRandom01() < bubbleRatePerSec * dt) {
            spawnBubble(m_smoothSpeed);
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
            // Экспоненциальный рост высоты тона пузырька к моменту схлопывания
            float currentFreq = b.freq * (1.0f + (1.0f - b.env) * b.chirpRate * 0.08f);
            b.phase += TWO_PI * currentFreq * dt;
            if (b.phase > TWO_PI) b.phase -= TWO_PI;

            float attack = std::min(1.0f, (1.0f - b.env) * 14.0f);
            float sig = (std::sin(b.phase) + 0.25f * std::sin(b.phase * 2.0f)) * b.env * attack * b.gain;

            dryL    += sig * (1.0f - b.pan);
            dryR    += sig * b.pan;
            fxSendL += sig * (1.0f - b.pan) * 0.65f;
            fxSendR += sig * b.pan * 0.65f;
        }

        // --------------------------------------------------------------------
        // 4. Пение китов и эхолокация в каньоне
        // --------------------------------------------------------------------
        m_nextWhaleTimer -= dt;
        if (m_nextWhaleTimer <= 0.0f && !m_whale.active) {
            startWhaleCall();
            m_nextWhaleTimer = 11.0f + nextRandom01() * 11.0f;
        }

        if (m_whale.active) {
            m_whale.age += dt;
            float u = m_whale.age / m_whale.duration;
            if (u >= 1.0f) {
                m_whale.active = false;
            } else {
                // Плавная огибающая громкости голоса кита
                float env = std::sin(u * 3.14159265f);
                env = env * env * std::min(1.0f, u * 5.0f);

                // Интонационный контур песни
                float pitch = m_whale.startFreq;
                if (m_whale.contourType == 0) {
                    pitch = m_whale.startFreq + (m_whale.endFreq - m_whale.startFreq) * std::pow(u, 0.7f);
                } else if (m_whale.contourType == 1) {
                    float arch = std::sin(u * 3.14159265f);
                    pitch = m_whale.startFreq * (1.0f - arch) + m_whale.midFreq * arch;
                } else {
                    pitch = m_whale.startFreq + std::sin(u * TWO_PI * 1.5f) * 45.0f - u * 35.0f;
                }

                // Вибрато
                m_whale.vibratoPhase += TWO_PI * 4.8f * dt;
                pitch += std::sin(m_whale.vibratoPhase) * 3.5f * u;

                m_whale.phase1 += TWO_PI * pitch * dt;
                m_whale.phase2 += TWO_PI * (pitch * 2.003f) * dt;
                m_whale.phase3 += TWO_PI * (pitch * 3.01f) * dt;
                if (m_whale.phase1 > TWO_PI) m_whale.phase1 -= TWO_PI;
                if (m_whale.phase2 > TWO_PI) m_whale.phase2 -= TWO_PI;
                if (m_whale.phase3 > TWO_PI) m_whale.phase3 -= TWO_PI;

                // Формантный тембр кита (основной тон + мягкая FM-модуляция и обертоны)
                float mod = std::sin(m_whale.phase2) * (0.45f + 0.35f * std::sin(u * 3.14159f));
                float whaleSig = (std::sin(m_whale.phase1 + mod) * 0.65f
                                  + std::sin(m_whale.phase2) * 0.25f
                                  + std::sin(m_whale.phase3) * 0.10f) * env * 0.14f;

                dryL    += whaleSig * (1.0f - m_whale.pan) * 0.5f;
                dryR    += whaleSig * m_whale.pan * 0.5f;
                fxSendL += whaleSig * (1.0f - m_whale.pan) * 1.15f;
                fxSendR += whaleSig * m_whale.pan * 1.15f;
            }
        }

        // --------------------------------------------------------------------
        // 5. Мягкий биолюминесцентный гармонический фон
        // --------------------------------------------------------------------
        float padL = 0.0f, padR = 0.0f;
        for (int i = 0; i < 4; ++i) {
            m_padPhases[i] += TWO_PI * chord[i] * dt;
            if (m_padPhases[i] > TWO_PI) m_padPhases[i] -= TWO_PI;
            float lfo = 0.55f + 0.45f * std::sin(static_cast<float>(m_dspTime * (0.17 + i * 0.07) + i * 1.5));
            float voice = std::sin(m_padPhases[i]) * lfo * 0.016f;
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

        // --------------------------------------------------------------------
        // 6. Стерео-реверберация каньона (Ping-Pong Delay с ФНЧ-затуханием)
        // --------------------------------------------------------------------
        float delayedL = m_delayBufL[m_delayIdxL];
        float delayedR = m_delayBufR[m_delayIdxR];

        // Подводное поглощение высоких частот при каждом отражении эха
        m_delayLpL += 0.12f * (delayedL - m_delayLpL);
        m_delayLpR += 0.12f * (delayedR - m_delayLpR);

        m_delayBufL[m_delayIdxL] = fxSendL + m_delayLpR * 0.56f;
        m_delayBufR[m_delayIdxR] = fxSendR + m_delayLpL * 0.56f;

        if (++m_delayIdxL >= m_delayBufL.size()) m_delayIdxL = 0;
        if (++m_delayIdxR >= m_delayBufR.size()) m_delayIdxR = 0;

        // Финальное микширование и мягкое насыщение (tanh-лимитер)
        float mixL = (dryL + padL + m_delayLpL * 0.65f) * m_smoothVol;
        float mixR = (dryR + padR + m_delayLpR * 0.65f) * m_smoothVol;

        mixL = std::tanh(mixL);
        mixR = std::tanh(mixR);

        // Запись сэмплов в зависимости от формата аудиоустройства (Float или Int16)
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

void UnderwaterAudioEngine::updateState(float camY, float swimSpeed, int biomeIndex) {
    if (!m_synthDevice) return;
    // Нормализуем глубину: 0.0 у поверхности (+15), 1.0 на дне каньона (-25)
    float depthFactor = std::clamp((15.0f - camY) / 40.0f, 0.0f, 1.0f);
    float normSpeed   = std::clamp(swimSpeed / 12.0f, 0.0f, 1.0f);
    m_synthDevice->setParameters(depthFactor, normSpeed, biomeIndex);
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
