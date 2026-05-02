// qm_bpm.h - Queen Mary BPM Beat Detection (standalone)
// Wraps qm-dsp (Queen Mary DSP Library, C4DM) for beat/BPM detection

#ifndef QM_BPM_H
#define QM_BPM_H

#include <vector>
#include <memory>
#include <string>
#include <cstdint>

// ---- Onset Detection Function types (from qm-dsp) ----
enum DfType {
    DF_HFC           = 1,
    DF_SPECDIFF      = 2,
    DF_PHASEDEV      = 3,
    DF_COMPLEXSD     = 4,
    DF_BROADBAND     = 5
};

// ---- Configuration ----
struct QueenMaryBeatConfig {
    double sampleRate = 44100.0;
    int    dfStepSamples = 0;       // 0 = auto (sampleRate * 0.01161)
    int    dfFrameLength = 0;       // 0 = auto (nextPow2(sampleRate / 50))
    DfType dfType = DF_COMPLEXSD;
    double inputTempo = 120.0;
    bool   constrainTempo = false;
};

// ---- Beat event ----
struct BeatEvent {
    double frameOffset;   // onset-frame index
    double timeSec;       // seconds from start
};

// ---- Main class ----
class QueenMaryBeatDetector {
public:
    explicit QueenMaryBeatDetector(const QueenMaryBeatConfig& cfg = {});
    ~QueenMaryBeatDetector();

    // Feed audio samples (not byte count)
    void addStereo(const float* left, const float* right, size_t nFrames);
    void addMono(const float* mono, size_t nFrames);

    // Run analysis — must call after all audio has been fed
    bool finalize();

    // Query results
    std::vector<BeatEvent> getBeats()        const;
    double                   getEstimatedBPM() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---- Minimal WAV loader (no external deps) ----
struct WavFile {
    double sampleRate = 0;
    int    channels   = 1;
    int    bitsPerSample = 16;
    std::vector<float> samples;   // mono, normalized to [-1, 1]
    bool   valid = false;
    std::string error;
};
WavFile loadWavFile(const char* path);

#endif // QM_BPM_H
