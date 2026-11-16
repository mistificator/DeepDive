#pragma once
#include <QIODevice>
#include <QAudioSink>
#include <QAudioFormat>
#include <atomic>
#include <vector>
#include <array>
#include <cstdint>

class UnderwaterSynthDevice : public QIODevice {
    Q_OBJECT
public:
    explicit UnderwaterSynthDevice(const QAudioFormat& format, QObject* parent = nullptr);

    void setParameters(float depthFactor, float swimSpeed, int biomeIndex);
    void setMasterVolume(float volume);
    void setMuted(bool muted);
    void triggerWhaleCall();

protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char* data, qint64 len) override;
    qint64 bytesAvailable() const override;

private:
    struct BiquadSVF {
        float low = 0.0f;
        float band = 0.0f;
        void process(float input, float cutoffNorm, float q, float& outLow, float& outBand);
    };

    struct BubbleVoice {
        bool active = false;
        float phase = 0.0f;
        float freq = 300.0f;
        float chirpRate = 12.0f;
        float env = 0.0f;
        float decay = 0.999f;
        float pan = 0.5f;
        float gain = 0.1f;
    };

    struct WhaleVoice {
        bool active = false;
        float age = 0.0f;
        float duration = 3.5f;
        float phase1 = 0.0f;
        float phase2 = 0.0f;
        float phase3 = 0.0f;
        float vibratoPhase = 0.0f;
        float startFreq = 220.0f;
        float endFreq = 140.0f;
        float midFreq = 310.0f;
        float pan = 0.5f;
        int contourType = 0;
    };

    void spawnBubble(float swimSpeed);
    void startWhaleCall();
    float nextRandom01();

    QAudioFormat m_format;
    int m_sampleRate = 48000;
    uint32_t m_rngState = 0xA341316Cu;

    // Атомарные параметры связи с потоком рендеринга
    std::atomic<float> m_depthFactor{0.5f};
    std::atomic<float> m_swimSpeed{0.0f};
    std::atomic<int>   m_biomeIndex{0};
    std::atomic<float> m_masterVolume{0.75f};
    std::atomic<bool>  m_muted{false};
    std::atomic<bool>  m_manualWhaleReq{false};

    // Сглаженные DSP-состояния
    float m_smoothDepth = 0.5f;
    float m_smoothSpeed = 0.0f;
    float m_smoothVol   = 0.75f;
    double m_dspTime    = 0.0;

    // Фильтры глубоководного шума и обтекания воды
    float m_brownL = 0.0f;
    float m_brownR = 0.0f;
    BiquadSVF m_rumbleFilterL;
    BiquadSVF m_rumbleFilterR;
    BiquadSVF m_whooshFilterL;
    BiquadSVF m_whooshFilterR;

    // Пул полифонических пузырьков и голос кита
    std::array<BubbleVoice, 8> m_bubbles;
    WhaleVoice m_whale;
    float m_nextWhaleTimer = 4.5f;

    // Фазы биолюминесцентного гармонического аккорда
    std::array<float, 4> m_padPhases{0.0f, 0.0f, 0.0f, 0.0f};

    // Подводное стерео-эхо (Ping-Pong Delay с ФНЧ в обратной связи)
    std::vector<float> m_delayBufL;
    std::vector<float> m_delayBufR;
    size_t m_delayIdxL = 0;
    size_t m_delayIdxR = 0;
    float m_delayLpL = 0.0f;
    float m_delayLpR = 0.0f;
};

class UnderwaterAudioEngine : public QObject {
    Q_OBJECT
public:
    explicit UnderwaterAudioEngine(QObject* parent = nullptr);
    ~UnderwaterAudioEngine() override;

    void updateState(float camY, float swimSpeed, int biomeIndex);
    void toggleMute();
    void adjustVolume(float delta);
    void triggerWhaleCall();

    bool isMuted() const { return m_muted; }
    float volume() const { return m_volume; }

private:
    QAudioSink* m_audioSink = nullptr;
    UnderwaterSynthDevice* m_synthDevice = nullptr;
    bool m_muted = false;
    float m_volume = 0.75f;
};
