// qm_bpm.h - Standalone Queen Mary University BPM Beat Detection
// Extracted from mixxx/lib/qm-dsp/
// Algorithm: Queen Mary beat tracking (VAMP plugin)
// Pipeline: Complex Spectral Difference → RCF bank → Viterbi → DP beat tracking

#ifndef QM_BPM_H
#define QM_BPM_H

#include <cmath>
#include <complex>
#include <functional>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

#define QM_PI 3.14159265358979323846

// ====== NAN/INF ======
static inline int ISNANd(double x) { return x != x; }

// ====== QM DSP includes (internal) ======
#include "../lib/qm-dsp/maths/MathUtilities.h"
#include "../lib/qm-dsp/maths/nan-inf.h"
#include "../lib/qm-dsp/maths/MathAliases.h"
#include "../lib/qm-dsp/base/Window.h"
#include "../lib/qm-dsp/dsp/phasevocoder/PhaseVocoder.h"
#include "../lib/qm-dsp/dsp/onsets/DetectionFunction.h"
#include "../lib/qm-dsp/dsp/tempotracking/TempoTrackV2.h"
#include "../lib/kissfft/kiss_fft.h"
#include "../lib/kissfft/kiss_fftr.h"

// ====== PUBLIC API ======

struct DfConfig {
    int stepSize = 512;
    int frameLength = 2048;
    DfType dfType = DF_COMPLEXSD;
    double dbRise = 3.0;
    bool adaptiveWhitening = false;
    double whiteningRelaxCoeff = -1.0;
    double whiteningFloor = -1.0;
};

struct QueenMaryBPMConfig {
    double sampleRate = 44100.0;
    int stepSizeFrames = 0;   // 0 = auto (512 at 44100 Hz = 11.6ms)
    int windowSize = 0;       // 0 = auto (nextPowerOfTwo(sampleRate/50))
    DfType detectionType = DF_COMPLEXSD;
    double inputTempo = 120.0;
    bool constrainTempo = false;
    bool smoothDF = true;
};

struct BeatEvent {
    double frameOffset;   // offset in onset detection frame increments
    double timeSec;       // absolute time in seconds
};

// Queen Mary BPM beat detection (the full pipeline)
class QueenMaryBeatDetector {
public:
    explicit QueenMaryBeatDetector(const QueenMaryBPMConfig& cfg = {});
    
    // Feed stereo audio samples (interleaved: L0 R0 L1 R1 ...)
    // Returns true on success
    bool feedStereo(const float* leftChannel, const float* rightChannel,
                    size_t numFrames);
    
    // Feed mono audio
    bool feedMono(const float* mono, size_t numFrames);
    
    // Finalize and compute beats
    bool finalize();
    
    // Results
    std::vector<BeatEvent> getBeats() const;
    double getEstimatedBPM() const;
    const std::vector<double>& getDetectionFunction() const;
    const std::vector<int>& getPeriodContour() const;

private:
    void onWindowReady(double* window, size_t frames);
    void computePeriodContour();
    void computeBeatsFromPeriods();
    
    QueenMaryBPMConfig m_cfg;
    double m_sampleRate;
    int m_stepSize = 0;
    int m_windowSize = 0;
    
    // Framing buffer
    std::vector<double> m_frameBuffer;
    size_t m_writePos = 0;
    std::function<bool(double*, size_t)> m_windowCallback;
    bool m_framed = false;
    
    // Onset detection
    QueenMaryBeatDetector_onsets_DetectionFunction* m_detector = nullptr;
    
    // Detection function values
    std::vector<double> m_dfpvalues;
    
    // Smoothed detection function
    std::vector<double> m_smoothedDF;
    int m_halfWindow = 0;
    
    // Tempo tracking
    QueenMaryBeatDetector_tempotracking_TempoTrackV2* m_tempoTracker = nullptr;
    std::vector<int> m_periodContour;
    
    // Beat output
    std::vector<BeatEvent> m_beats;
    double m_estimatedBPM = 0.0;
    bool m_finalized = false;
};

// ======== Minimal WAV file reader ========

struct WavFile {
    double sampleRate = 0;
    int channels = 1;
    int bitsPerSample = 16;
    std::vector<float> samples;  // mono, interleaved downmix
    bool valid = false;
    std::string error;
};

// Read a WAV file; returns valid mono float samples at the given sample rate
WavFile loadWavFile(const char* path);

#endif // QM_BPM_H
