// qm_bpm.h — Standalone Queen Mary University BPM Beat Detection Algorithm
//
// Extracted from mixxx (https://github.com/mixxxdj/mixxx) which bundles
// the QM DSP Library from Centre for Digital Music, Queen Mary University
// of London (https://www.vamp-plugins.org/plugin-doc/qm-vamp-plugins.html).
//
// The algorithm follows the hybrid approach from:
//   - Davies (2007): Two-state beat tracking with tempo contour estimation
//   - Ellis (2007):  Dynamic programming beat position recovery
//
// ARCHITECTURE SUMMARY
// ====================
//
// Pipeline (per audio frame):
//  ┌──────────────┐     ┌──────────────┐     ┌─────────────────┐
//  │ Raw Stereo   │────▶│ Downmix to   │────▶│ Frame & Window  │
//  │ Audio Buffer │     │ Mono         │     │ (Hann window)   │
//  └──────────────┘     └──────────────┘     └────────┬────────┘
//                                                      │
//                                               ┌──────▼──────┐
//                                               │ Phase       │
//                                               │ Vocoder     │
//                                               │ (FFT +      │
//                                               │  phase voc) │
//                                               └──────┬──────┘
//                                                      │
//                                               ┌──────▼──────┐
//                                               │ Complex     │
//                                               │ Spectral    │
//                                               │ Difference  │
//                                               │ (onset DF)  │
//                                               └──────┬──────┘
//                                                      │
//                                            onset values over time
//
// Post-processing (after all frames processed):
//  ┌──────────────────┐     ┌─────────────────────┐     ┌──────────────┐
//  │ Butterworth LP   │────▶│ Resonator Comb      │────▶│ Viterbi      │
//  │ Filter (FiltFilt)│     │ Filter (RCF) Bank   │     │ Tempo Decoding│
//  └──────────────────┘     └─────────────────────┘     └──────┬───────┘
//                                                               │
//                                                       ┌───────▼────────┐
//                                                       │ Dynamic       │
//                                                       │ Program       │
//                                                       │ Beat Tracking │
//                                                       └───────┬────────┘
//                                                               │
//                                                    Beat positions + BPM
//
// COMPLEX SPECTRAL DIFFERENCE (DF_COMPLEXSD)
// ============================================
// For each frame, computes the spectral distance between the current and
// previous frame using both magnitude and phase information:
//
//   For each bin i:
//     Δφ_i = unwrapped_phase_i - 2·phase_prev_i + phase_prevprev_i
//     Δφ_i = princarg(Δφ_i)          (wrap to [-π, π))
//     complex_diff_i = mag_prev_i - mag_i · exp(j·Δφ_i)
//     onset(i) = |Δφ_i|
//     onset(i) += |complex_diff_i|
//
// This detects spectral "onsets" — sudden changes in the spectrum that
// correlate with musical beats (transients, attacks).
//
// TEMPO TRACKING (RESOnator COMB FILTER BANK + VITERBI)
// ======================================================
// For each analysis window:
//   1. Compute autocorrelation of the detection function
//   2. Apply comb filter bank (resonator comb filters) tuned to different
//      beat periods (20–128 steps)
//   3. Apply adaptive threshold
//   4. Normalize
//   5. Stack results into a matrix
//   6. Viterbi decode to find most likely beat period path through time
//
// BEAT POSITION (DYNAMIC PROGRAMMING)
// ====================================
// Using the tempo contour from Viterbi:
//   1. Compute cumulative score with weighted backtracking
//   2. Start from strongest point near end of track
//   3. Backtrack through cumulative score using transition weights
//   4. Output beat frame positions
//
// LICENSE
// =======
// Original qm-dsp code: GPL v2+ (Centre for Digital Music, QMUL)
// This extraction preserves the original licenses.

#ifndef QM_BPM_H
#define QM_BPM_H

#include <cmath>
#include <complex>
#include <deque>
#include <functional>
#include <vector>
#include <string>
#include <memory>

// ─────────────────────────────────────────────────────
// 1. NaN/Inf utilities (from nan-inf.h)
// ─────────────────────────────────────────────────────
static inline int ISNANd(double x) { return x != x; }
static inline int ISNANf(float x)  { return x != x; }

static inline int ISINFd(double x) {
    return !ISNANd(x) && ISNANd(x - x);
}
static inline int ISINFf(float x) {
    return !ISNANf(x) && ISNANf(x - x);
}

// ─────────────────────────────────────────────────────
// 2. MathUtilities (from maths/MathUtilities.h/cpp)
// ─────────────────────────────────────────────────────
class QmMath {
public:
    // Principle argument: map angle to [-π, π)
    static double princarg(double ang);

    // Mean of a range
    static double mean(const std::vector<double>& data, int start, int count);

    // Adaptive threshold: subtract moving-mean, clamp negative to zero
    static void adaptiveThreshold(std::vector<double>& data);

    // Power of two helpers
    static bool isPowerOfTwo(int x);
    static int  nextPowerOfTwo(int x);

    // GCD
    static int gcd(int a, int b);
};

// ─────────────────────────────────────────────────────
// 3. FFT: Real FFT via kissfft (from dsp/transforms)
// ─────────────────────────────────────────────────────

// Forward declarations for kissfft types
struct kiss_fft_state;
struct kiss_fftr_state;

// Minimal kissfft interface (see ext/kissfft/kiss_fft.h)
extern "C" {
    typedef struct { float r, i; } kiss_fft_cpx_f;
    typedef struct kiss_fft_state* kiss_fft_cfg_f;
    typedef struct kiss_fftr_state* kiss_fftr_cfg_f;

    kiss_fft_cfg_f kiss_fft_alloc_f(int nfft, int inverse_fft, void* mem, size_t* lenmem);
    void kiss_fft_f(kiss_fft_cfg_f cfg, const kiss_fft_cpx_f* fin, kiss_fft_cpx_f* fout);
    void kiss_fft_free_f(void* cfg);

    kiss_fftr_cfg_f kiss_fftr_alloc_f(int nfft, int inverse_fft, void* mem, size_t* lenmem);
    void kiss_fftr_f(kiss_fftr_cfg_f cfg, const float* timedata, kiss_fft_cpx_f* freqdata);
    void kiss_fftri_f(kiss_fftr_cfg_f cfg, const kiss_fft_cpx_f* freqdata, float* timedata);
    void kiss_fftr_free_f(void* cfg);
}

class QmRealFFT {
public:
    explicit QmRealFFT(int n);
    ~QmRealFFT();
    // Forward: real input → complex output (m_n/2+1 bins)
    void forward(const double* realIn, double* realOut, double* imagOut);
    inline int size() const { return m_n; }
private:
    int m_n;
    kiss_fftr_cfg_f m_cfg = nullptr;
};

// ─────────────────────────────────────────────────────
// 4. Window functions (from base/Window.h)
// ─────────────────────────────────────────────────────
enum class WindowType {
    Rectangular,
    Bartlett,
    Hamming,
    Hanning,
    Blackman,
    BlackmanHarris
};

class QmWindow {
public:
    QmWindow(WindowType type, int size);
    // Multiply src by window, write to dst (both must be 'size' long)
    void apply(const double* src, double* dst) const;
    inline int size() const { return m_size; }

private:
    WindowType m_type;
    int m_size = 0;
    std::vector<double> m_cache;

    void buildCache();
};

// ─────────────────────────────────────────────────────
// 5. Phase Vocoder (from dsp/phasevocoder/PhaseVocoder.h/cpp)
// ─────────────────────────────────────────────────────
class QmPhaseVocoder {
public:
    QmPhaseVocoder(int frameSize, int hopSize);
    ~QmPhaseVocoder();

    // Process a windowed time-domain frame → magnitude + instantaneous phase
    void processTimeDomain(const double* windowed,
                           double* mag, double* phase, double* unwrapped);

    // Reset accumulated phases (for numerical stability)
    void reset();

    inline int frameSize() const { return m_n; }
    inline int hopSize()   const { return m_hop; }
private:
    int m_n, m_hop;
    QmRealFFT* m_fft = nullptr;
    std::vector<double> m_time, m_real, m_imag;
    std::vector<double> m_phase, m_unwrapped;

    void getMagnitudes(double* mag);
    void getPhases(double* theta);
    void unwrapPhases(double* theta, double* unwrapped);
    void fftShift(double* src);
};

// ─────────────────────────────────────────────────────
// 6. Onset Detection Function (from dsp/onsets/DetectionFunction.h/cpp)
// ─────────────────────────────────────────────────────

// Detection function types
enum DfType {
    DF_HFC = 1,
    DF_SPECDIFF = 2,
    DF_PHASEDEV = 3,
    DF_COMPLEXSD = 4,  // Queen Mary's default
    DF_BROADBAND = 5
};

struct DfConfig {
    int stepSize;          // Frame hop in samples
    int frameLength;       // Analysis window (power of 2, must be even)
    DfType dfType = DF_COMPLEXSD;
    double dbRise = 3.0;          // For broadband only
    bool adaptiveWhitening = false;
    double whiteningRelaxCoeff = -1.0;
    double whiteningFloor = -1.0;
};

class QmOnsetDetector {
public:
    explicit QmOnsetDetector(const DfConfig& config);
    ~QmOnsetDetector();

    // Process one time-domain frame (frameLength samples, windowed by caller)
    // Returns the onset detection function value (larger = stronger onset)
    double processFrame(const double* samples);

    // Get the magnitude spectrum of the last frame (for inspection)
    const double* getMagnitude() const { return m_mag.data(); }

    inline DfType getType() const { return m_config.dfType; }
    inline int halfLength() const { return m_halfLength; }

private:
    void initialize(const DfConfig& config);
    void cleanup();
    void whiten();
    double runDF();

    // Detection function implementations
    double hfc(int length, const double* src);
    double specDiff(int length, const double* src);
    double phaseDev(int length, const double* srcPhase);
    double complexSD(int length, const double* srcMag, const double* srcPhase);
    double broadband(int length, const double* src);

    DfConfig m_config;
    int m_dataLength = 0;
    int m_halfLength = 0;
    int m_stepSize = 0;
    bool m_whiten = false;
    double m_whitenRelaxCoeff = 0.9997;
    double m_whitenFloor = 0.01;

    std::vector<double> m_magHistory;
    std::vector<double> m_phaseHistory;
    std::vector<double> m_phaseHistoryOld;
    std::vector<double> m_magPeaks;

    std::unique_ptr<QmPhaseVocoder> m_phaseVoc;
    std::vector<double> m_mag;
    std::vector<double> m_thetaAngle;
    std::vector<double> m_unwrapped;

    std::unique_ptr<QmWindow> m_window;
    std::vector<double> m_windowed;
};

// ─────────────────────────────────────────────────────
// 7. Resonator Comb Filter Bank + Viterbi Tempo Tracking (from dsp/tempotracking/TempoTrackV2.h/cpp)
// ─────────────────────────────────────────────────────

/**
 * Queen Mary beat period estimation and beat position tracker.
 *
 * This implements:
 *   1. Resonator Comb Filter (RCF) bank — autocorrelation-based comb filtering
 *   2. Viterbi decoding — find best tempo contour
 *   3. Dynamic programming beat tracking — find exact beat locations
 *
 * All indices are in "df increment" units (the frame hop size of the onset
 * detector), NOT in samples or seconds.
 */
class QmTempoTracker {
public:
    /**
     * Construct. sampleRate and frameIncrement are used only for BPM
     * frequency conversion (BPM = 60 * frameIncrement / period_in_df_units).
     */
    QmTempoTracker(double sampleRate, int frameIncrement);

    // ── Phase 1: Compute resonator comb filter responses + Viterbi → beat period path

    /**
     * Calculate the beat period contour from onset detection function values.
     *
     * beatPeriod will be filled with one value per analysis window. Each value
     * is an index: 1 = fastest (20 BPM), 128 = slowest (~4.6 BPM at 44.1kHz).
     * The predominant period can be found by weighted averaging the period values.
     *
     * @param df           Onset detection function values (after smoothing)
     * @param beatPeriod   Output: period index per window
     * @param inputTempo   Assumed input BPM (default 120). Used for weighting.
     * @param constrainToInput  If true, use Gaussian around inputTempo instead of
     *                         Rayleigh distribution for periodicity weighting.
     */
    void calculateBeatPeriod(const std::vector<double>& df,
                             std::vector<int>& beatPeriod,
                             double inputTempo = 120.0,
                             bool constrainToInput = false);

    // ── Phase 2: Compute beat positions from period contour

    /**
     * Calculate beat positions using dynamic programming.
     *
     * @param df             Onset detection function values
     * @param beatPeriod     Period contour from calculateBeatPeriod()
     * @param beats          Output: beat positions in df increment units
     * @param alpha          Backtracking weight (0.9 = prefer previous beat)
     * @param tightness      Tempo change tightness (4.0 = gentle changes only)
     */
    void calculateBeats(const std::vector<double>& df,
                        const std::vector<int>& beatPeriod,
                        std::vector<double>& beats,
                        double alpha = 0.9,
                        double tightness = 4.0);

    // ── Helpers ──

    /** Convert a beat period index to BPM (for a given reference tempo). */
    static double periodIndexToBpm(int periodIndex, double referenceTempo, double sampleRate, int frameIncrement);

    /** Convert a beat period index to seconds. */
    static double periodIndexToSeconds(int periodIndex, double sampleRate, int frameIncrement);

    /** Apply a bidirectional Butterworth lowpass to the detection function. */
    static void filterDF(std::vector<double>& df);

private:
    typedef std::vector<int>    i_vec;
    typedef std::vector<double> d_vec;
    typedef std::vector<d_vec>  d_mat;

    double m_rate;
    int m_inc;

    static constexpr int kMaxPeriod = 128;  // Max beat period in df increments
    static constexpr int kWindowLen = 512;  // RCF window length
    static constexpr int kHopSize   = 128;  // RCF hop size
    static constexpr double kEPS    = 0.0000008;

    void getRCF(const d_vec& frame, const d_vec& weights, d_vec& rcf);
    void viterbiDecode(const d_mat& rcfMat, const d_vec& weights, i_vec& beatPeriod);
    double getMaxVal(const d_vec& df);
    int    getMaxInd(const d_vec& df);
};

// ─────────────────────────────────────────────────────
// 8. Beat Frame Downmix & Overlap Helper (from buffer utils)
// ─────────────────────────────────────────────────────

/**
 * Utility that takes a stream of stereo audio samples, downmixes to mono,
 * frames them into overlapping Hann windows, and delivers each frame to a
 * callback via std::function.
 *
 * This is the framing engine that feeds audio into QmOnsetDetector.
 */
class QmFramingHelper {
public:
    using WindowReadyCallback = std::function<bool(double* window, size_t frames)>;

    bool initialize(size_t windowSize, size_t stepSize, const WindowReadyCallback& callback);
    bool processSamples(const float* leftChannel, const float* rightChannel, size_t numFrames);
    bool processSamplesMono(const float* mono, size_t numFrames);
    bool finalize();

private:
    bool processInner(const float* mono, size_t numFrames);

    std::vector<double> m_buffer;
    size_t m_windowSize = 0;
    size_t m_stepSize = 0;
    size_t m_writePos = 0;
    WindowReadyCallback m_callback;
};

// ─────────────────────────────────────────────────────
// 9. Public API — Complete Queen Mary BPM Beat Detector
// ─────────────────────────────────────────────────────

/** Beat event with frame position and time in seconds. */
struct QmBeat {
    double framePosition;   // In onset detection frame increments
    double timeSeconds;     // In seconds (requires sample rate)
    double frequencyHz;     // Beat rate at this point
};

/**
 * Complete Queen Mary BPM beat detection system.
 *
 * Usage:
 *   QmQueenMaryBPM detector(sampleRate);
 *   detector.detect(leftChannel, rightChannel, numFrames);
 *   auto results = detector.getResults();
 */
class QmQueenMaryBPM {
public:
    /**
     * @param sampleRate       Audio sample rate (e.g. 44100.0)
     * @param inputTempo       Assumed tempo for weighting (120.0 = neutral)
     * @param constrainTempo   If true, constrain to inputTempo with Gaussian
     */
    QmQueenMaryBPM(double sampleRate,
                   double inputTempo = 120.0,
                   bool constrainTempo = false);

    /**
     * Run beat detection on a stereo audio buffer.
     * @param left     Left channel samples (float, interleaved with right)
     * @param right    Right channel samples
     * @param numFrames Number of frames (NOT samples)
     */
    bool detect(const float* left, const float* right, size_t numFrames);

    /**
     * @return Beat events detected across the entire audio buffer.
     */
    std::vector<QmBeat> getBeats() const;

    /**
     * @return Estimated BPM (weighted-average of the tempo contour).
     */
    double getEstimatedBPM() const;

    /**
     * @return Beat period contour (index per window).
     */
    std::vector<int> getPeriodContour() const;

    /**
     * @return Onset detection function values.
     */
    std::vector<double> getDetectionFunction() const;

    // ── Configuration ──
    struct Config {
        double  sampleRate     = 44100.0;
        double  inputTempo     = 120.0;
        bool    constrainTempo = false;
        int     frameHopSamples = 0;  // 0 = auto (512 samples at 44.1kHz)
        int     windowSize     = 0;  // 0 = auto (power of 2, ~50 Hz resolution)
        DfType  detectionType  = DF_COMPLEXSD;
        bool    smoothDF       = true;  // Apply Butterworth lowpass before RCF
    };

    const Config& getConfig() const { return m_config; }

private:
    void onWindowReady(const double* window, size_t frames);

    Config m_config;
    int          m_frameHop = 0;
    int          m_windowSize = 0;
    std::unique_ptr<QmFramingHelper> m_framing;
    std::unique_ptr<QmOnsetDetector> m_detector;
    std::vector<double>              m_dfValues;
    std::vector<int>                 m_periodContour;
    std::vector<double>              m_beatDF;  // smoothed detection function
    std::vector<double>              m_beatFrames;  // in DF increment units
    double                           m_inputTempo = 120.0;
    bool                             m_constrainTempo = false;
};

#endif  // QM_BPM_H
