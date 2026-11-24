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

    void setParameters(float depthFactor, float swimSpeed, int biomeIndex,
                       bool aboveWater = false, float timeScale = 1.0f);
    void setMasterVolume(float volume);
    void setMuted(bool muted);
    void triggerWhaleCall();
    void triggerSplash(bool exitingWater, float intensity);

protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char* data, qint64 len) override;
    qint64 bytesAvailable() const override;

private:
    struct BiquadSVF {
        float low = 0.0f;
        float band = 0.0f;
        void process(float input, float cutoffNorm, float q,
                     float& outLow, float& outBand, float* outHigh = nullptr);
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

    struct SeagullVoice {
        bool active = false;
        float age = 0.0f;
        float duration = 0.55f;
        float pauseTimer = 0.0f;
        int syllablesLeft = 0;
        float phase = 0.0f;
        float raspPhase = 0.0f;
        float startFreq = 2200.0f;
        float endFreq = 1450.0f;
        float pan = 0.5f;
        float gain = 0.10f;
        BiquadSVF formant;
    };

    struct SplashVoice {
        bool active = false;
        bool exitingWater = true;
        float age = 0.0f;
        float duration = 0.85f;
        float intensity = 0.8f;
        float thumpPhase = 0.0f;
        BiquadSVF filterL;
        BiquadSVF filterR;
    };

    void spawnBubble(float swimSpeed);
    void startWhaleCall();
    void startSeagullCall();
    void startSplash(bool exitingWater, float intensity);
    float nextRandom01();

    QAudioFormat m_format;
    int m_sampleRate = 48000;
    uint32_t m_rngState = 0xA341316Cu;

    // Атомарные параметры связи с потоком рендеринга
    std::atomic<float> m_depthFactor{0.5f};
    std::atomic<float> m_swimSpeed{0.0f};
    std::atomic<int>   m_biomeIndex{0};
    std::atomic<float> m_aboveWater{0.0f};
    std::atomic<float> m_timeScale{1.0f};
    std::atomic<float> m_masterVolume{0.75f};
    std::atomic<bool>  m_muted{false};
    std::atomic<bool>  m_manualWhaleReq{false};
    std::atomic<float> m_splashReqIntensity{0.8f};
    std::atomic<int>   m_splashReqType{0}; // 0=none, 1=breach exit, 2=splashdown enter

    // Сглаженные DSP-состояния
    float m_smoothDepth     = 0.5f;
    float m_smoothSpeed     = 0.0f;
    float m_smoothVol       = 0.75f;
    float m_smoothAir       = 0.0f;
    float m_smoothTimeScale = 1.0f;
    bool  m_wasAboveWater   = false;
    double m_dspTime        = 0.0;
    double m_surfWavePhase  = 0.0;

    // Фильтры подводного гула и обтекания воды
    float m_brownL = 0.0f;
    float m_brownR = 0.0f;
    BiquadSVF m_rumbleFilterL;
    BiquadSVF m_rumbleFilterR;
    BiquadSVF m_whooshFilterL;
    BiquadSVF m_whooshFilterR;

    // Фильтры надводного прибоя и морского ветра
    BiquadSVF m_surfBodyFilterL;
    BiquadSVF m_surfBodyFilterR;
    BiquadSVF m_surfFoamFilterL;
    BiquadSVF m_surfFoamFilterR;
    BiquadSVF m_windFilterL;
    BiquadSVF m_windFilterR;

    // Пулы голосов: пузырьки, кит, чайки и всплески
    std::array<BubbleVoice, 12> m_bubbles;
    WhaleVoice m_whale;
    float m_nextWhaleTimer = 4.5f;

    std::array<SeagullVoice, 2> m_seagulls;
    float m_nextGullTimer = 0.35f;

    std::array<SplashVoice, 2> m_splashes;

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

    void updateState(float camY, float swimSpeed, int biomeIndex,
                     bool aboveWater = false, float timeScale = 1.0f);
    void toggleMute();
    void adjustVolume(float delta);
    void triggerWhaleCall();
    void triggerSplash(bool exitingWater, float intensity);

    bool isMuted() const { return m_muted; }
    float volume() const { return m_volume; }

private:
    QAudioSink* m_audioSink = nullptr;
    UnderwaterSynthDevice* m_synthDevice = nullptr;
    bool m_muted = false;
    float m_volume = 0.75f;
};
