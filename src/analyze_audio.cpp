/* analyze_audio.cpp — Standalone Queen Mary BPM detector
 *
 * Reads a WAV file, runs the Queen Mary onset detection + tempo tracking
 * pipeline (mirroring AnalyzerQueenMaryBeats in the mixxx codebase), and
 * prints the estimated BPM and beat locations.
 *
 * Gold-standard reference:
 *   mixxx/src/analyzer/plugins/analyzerqueenmarybeats.cpp
 *   mixxx/src/track/beatutils.h
 *   mixxx/src/track/beatutils.cpp
 *
 * Two BPM estimation modes:
 *   Default (Mixxx):    Beat grid → constant regions → musical BPM
 *   --queen-mary:       Dominant period contour (VAMP original)
 */

#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sndfile.h>
#include <string>
#include <vector>

/* qm-dsp headers */
#include "lib/qm-dsp/maths/MathUtilities.h"
#include "lib/qm-dsp/maths/nan-inf.h"
#include "lib/qm-dsp/maths/MathAliases.h"
#include "lib/qm-dsp/base/Window.h"
#include "lib/qm-dsp/dsp/onsets/DetectionFunction.h"
#include "lib/qm-dsp/dsp/tempotracking/TempoTrackV2.h"

/* === WAv loading via libsnDFe === */

struct WavInfo {
    double       sampleRate;
    int          channels;
    int          numFrames;
    std::vector<float> samples;  /* interleaved */
};

static WavInfo loadWave(const std::string& path) {
    SF_INFO sfinfo{};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &sfinfo);
    if (!sf) {
        std::fprintf(stderr, "Error: cannot open '%s': %s\n", path.c_str(), sf_strerror(nullptr));
        return WavInfo{};
    }

    WavInfo info;
    info.sampleRate    = sfinfo.samplerate;
    info.channels      = sfinfo.channels;
    info.numFrames     = sfinfo.frames;

    /* read all frames at once */
    std::vector<float> buf(static_cast<size_t>(sfinfo.frames) * sfinfo.channels);
    const auto nread = sf_readf_float(sf, buf.data(), static_cast<int>(buf.size()));
    sf_close(sf);

    if (nread < 0) {
        std::fprintf(stderr, "Error: failed to read '%s'\n", path.c_str());
        return WavInfo{};
    }
    info.samples = std::move(buf);
    return info;
}

/* ====================================================================
 * Mixxx-style BeatUtils from mixxx/src/track/beatutils.h + beatutils.cpp
 *
 * Copied verbatim from Mixxx source, only Qt types replaced by stdlib.
 */

namespace mixxx_impl {

namespace audio {
    using FrameDiff_t = int64_t;
    using FramePos    = FrameDiff_t;
    using SampleRate  = int64_t;
}

/** BPM stored as raw double value. Matches mixxx/src/track/bpm.h */
class Bpm {
public:
    static constexpr double kValueUndefined = 0.0;
    static constexpr double kValueMin       = 0.0;   // exclusive
    static constexpr double kValueMax       = 500.0; // inclusive

    constexpr Bpm() noexcept : m_value(kValueUndefined) {}
    explicit constexpr Bpm(double value) : m_value(value) {}
    constexpr Bpm(int64_t v) : m_value(static_cast<double>(v)) {}

    static bool isValidValue(double value) {
        return std::isfinite(value) && value > kValueMin;
    }

    bool isValid() const { return isValidValue(m_value); }
    double value() const { return m_value; }

    Bpm operator*(double f) const  { return Bpm(m_value * f); }
    Bpm operator/(double f) const  { return Bpm(m_value / f); }
    Bpm operator+(double f) const  { return Bpm(m_value + f); }
    Bpm operator-(double f) const  { return Bpm(m_value - f); }
    bool operator>(const Bpm& o) const  { return m_value > o.value(); }
    bool operator<(const Bpm& o) const  { return m_value < o.value(); }

private:
    double m_value;
};

namespace {

static constexpr double kMaxSecsPhaseErr   = 0.025;
static constexpr double kMaxSecsPhaseErrSum = 0.1;
static constexpr int    kMaxOutliers       = 1;
static constexpr int    kMinRegionBeats    = 16;

struct ConstRegion {
    mixxx_impl::audio::FramePos firstBeat;
    int64_t beatLength;
};

/** Copied from mixxx/src/track/beatutils.cpp retrieveConstRegions */
std::vector<ConstRegion> retrieveConstRegions(
    const std::vector<double>& coarseBeats,
    mixxx_impl::audio::SampleRate sr)
{
    if (coarseBeats.size() < 2) return {};

    const int64_t maxPhase  = static_cast<int64_t>(kMaxSecsPhaseErr   * sr);
    const int64_t maxPhaseS = static_cast<int64_t>(kMaxSecsPhaseErrSum * sr);
    int L = 0, R = static_cast<int>(coarseBeats.size()) - 1;
    std::vector<ConstRegion> regions;

    while (L < static_cast<int>(coarseBeats.size()) - 1) {
        const int64_t mBL = (coarseBeats[R] - coarseBeats[L]) /
                            (static_cast<int64_t>(R) - static_cast<int64_t>(L));
        int outliers = 0;
        // ironedBeat is FramePos (int64), NOT double
        int64_t ironedBeat = static_cast<int64_t>(coarseBeats[L]);
        // phaseErrorSum is FrameDiff_t (int64), NOT double
        int64_t phaseErrorSum = 0;
        int i = L + 1;
        for (; i <= R; ++i) {
            ironedBeat += mBL;
            const int64_t phaseError = ironedBeat - static_cast<int64_t>(coarseBeats[i]);
            phaseErrorSum += phaseError;
            if (std::abs(phaseError) > maxPhase) {
                outliers++;
                if (outliers > kMaxOutliers || i == L + 1) break;
            }
            if (std::abs(phaseErrorSum) > maxPhaseS) break;
        }
        if (i > R) {
            int64_t bErr = 0;
            if (R > L + 2) {
                const int64_t fBL = static_cast<int64_t>(coarseBeats[L+1]) - static_cast<int64_t>(coarseBeats[L]);
                const int64_t lBL = static_cast<int64_t>(coarseBeats[R]) - static_cast<int64_t>(coarseBeats[R-1]);
                bErr = std::abs(fBL + lBL - 2 * mBL);
            }
            if (bErr < maxPhase / 2) {
                regions.push_back({static_cast<int64_t>(coarseBeats[L]), mBL});
                L = R;
                R = static_cast<int>(coarseBeats.size()) - 1;
                continue;
            }
        }
        R--;
    }
    regions.push_back({static_cast<int64_t>(coarseBeats.back()), 0});

    if (sr == 44100) {
        std::fprintf(stderr, "[DEBUG] retrieveConstRegions: regions.size()=%zu\n", regions.size());
        for (size_t i = 0; i < regions.size(); ++i) {
            std::fprintf(stderr, "  region[%zu]: firstBeat=%lld, beatLength=%lld\n",
                i, (long long)regions[i].firstBeat, (long long)regions[i].beatLength);
        }
    }
    return regions;
}

/** Copied from beatutils.cpp trySnap */
std::optional<mixxx_impl::Bpm> trySnap(
        mixxx_impl::Bpm minBpm, mixxx_impl::Bpm centerBpm,
        mixxx_impl::Bpm maxBpm, double fraction) {
    const double snapped = round(centerBpm.value() * fraction) / fraction;
    mixxx_impl::Bpm s(snapped);
    if (s.value() > minBpm.value() && s.value() < maxBpm.value()) return s;
    return std::nullopt;
}

/** Copied from beatutils.cpp roundBpmWithinRange */
mixxx_impl::Bpm roundBpmWithinRange(
        mixxx_impl::Bpm minBpm, mixxx_impl::Bpm centerBpm, mixxx_impl::Bpm maxBpm) {
    auto snap = trySnap(minBpm, centerBpm, maxBpm, 1.0);
    if (snap) return *snap;
    if (centerBpm.value() < Bpm(85.0).value()) {
        snap = trySnap(minBpm, centerBpm, maxBpm, 2.0);
        if (snap) return *snap;
    }
    if (centerBpm.value() > Bpm(127.0).value()) {
        snap = trySnap(minBpm, centerBpm, maxBpm, 2.0/3.0);
        if (snap) return *snap;
    }
    snap = trySnap(minBpm, centerBpm, maxBpm, 3.0);
    if (snap) return *snap;
    snap = trySnap(minBpm, centerBpm, maxBpm, 12.0);
    if (snap) return *snap;
    return centerBpm;
}

/**
 * Copied from beatutils.cpp makeConstBpm.
 *
 * Find the longest constant-spacing region, compute centre BPM,
 * then snap to the nearest musical BPM value using the same
 * fractions (1, 2, 2/3, 3, 1/12) as Mixxx.
 */
mixxx_impl::Bpm makeConstBpm(
        const std::vector<ConstRegion>& regions,
        mixxx_impl::audio::SampleRate sr) {
    if (regions.empty()) return mixxx_impl::Bpm();

    int midIdx   = 0;
    int64_t longestLen   = 0;
    int64_t longestBL    = 0;

    for (int i = 0; i < static_cast<int>(regions.size()) - 1; ++i) {
        const int64_t len = regions[i+1].firstBeat - regions[i].firstBeat;
        if (len > longestLen) {
            longestLen  = len;
            longestBL   = regions[i].beatLength;
            midIdx      = i;
        }
    }

    if (longestLen == 0) return mixxx_impl::Bpm();

    const int numBeats = static_cast<int>(
            (longestLen / longestBL) + 0.5);
    // beatLengthMin/Max are int64 (FrameDiff_t) — truncated per Mixxx
    int64_t longestBLMin = longestBL
            - static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numBeats);
    int64_t longestBLMax = longestBL
            + static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numBeats);

    if (sr == 44100) {
        std::fprintf(stderr, "[DEBUG] makeConstBpm: regions.size()=%zu\n", regions.size());
        for (size_t i = 0; i < regions.size(); ++i) {
            std::fprintf(stderr, "  region[%zu]: firstBeat=%lld, beatLength=%lld\n",
                i, (long long)regions[i].firstBeat, (long long)regions[i].beatLength);
        }
        std::fprintf(stderr, "  midIdx=%d longestLen=%lld longestBL=%lld\n",
            midIdx, (long long)longestLen, (long long)longestBL);
        std::fprintf(stderr, "  numBeats=%d longestBLMin=%lld longestBLMax=%lld\n",
            numBeats, (long long)longestBLMin, (long long)longestBLMax);
    }
    int startIdx = midIdx;

    /* Expand forward */
    for (int i = 0; i < midIdx; ++i) {
        const int64_t len = regions[i+1].firstBeat - regions[i].firstBeat;
        const int numberOfBeats = static_cast<int>(
                (len / regions[i].beatLength) + 0.5);
        if (numberOfBeats < kMinRegionBeats) continue;
        const int64_t thisBLMin = regions[i].beatLength
                - static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numberOfBeats);
        const int64_t thisBLMax = regions[i].beatLength
                + static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numberOfBeats);
        if (longestBL > thisBLMin && longestBL < thisBLMax) {
            const int64_t newLen = regions[midIdx+1].firstBeat - regions[i].firstBeat;
            const int64_t beatLenMin = std::max<long long>(longestBLMin, thisBLMin);
            const int64_t beatLenMax = std::min<long long>(longestBLMax, thisBLMax);
            const int maxN = static_cast<int>(
                    std::round(static_cast<double>(newLen) / static_cast<double>(beatLenMin)));
            const int minN = static_cast<int>(
                    std::round(static_cast<double>(newLen) / static_cast<double>(beatLenMax)));
            if (minN != maxN) continue;
            const int64_t newBeatLength = newLen / minN;
            if (newBeatLength > longestBLMin && newBeatLength < longestBLMax) {
                longestLen  = newLen;
                longestBL   = newBeatLength;
                longestBLMin = longestBL
                        - static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numBeats);
                longestBLMax = longestBL
                        + static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numBeats);
                startIdx    = i;
                break;
            }
        }
    }

    /* Expand backward */
    for (int i = static_cast<int>(regions.size()) - 2; i > midIdx; --i) {
        const int64_t len = regions[i+1].firstBeat - regions[i].firstBeat;
        const int numberOfBeats = static_cast<int>(
                (len / regions[i].beatLength) + 0.5);
        if (numberOfBeats < kMinRegionBeats) continue;
        const int64_t thisBLMin = regions[i].beatLength
                - static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numberOfBeats);
        const int64_t thisBLMax = regions[i].beatLength
                + static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numberOfBeats);
        if (longestBL > thisBLMin && longestBL < thisBLMax) {
            const int64_t newLen = regions[i+1].firstBeat - regions[startIdx].firstBeat;
            const int64_t beatLenMin = std::max<long long>(longestBLMin, thisBLMin);
            const int64_t beatLenMax = std::min<long long>(longestBLMax, thisBLMax);
            const int maxN = static_cast<int>(
                    std::round(static_cast<double>(newLen) / static_cast<double>(beatLenMin)));
            const int minN = static_cast<int>(
                    std::round(static_cast<double>(newLen) / static_cast<double>(beatLenMax)));
            if (minN != maxN) continue;
            const int64_t newBeatLength = newLen / minN;
            if (newBeatLength > longestBLMin && newBeatLength < longestBLMax) {
                longestLen  = newLen;
                longestBL   = newBeatLength;
                longestBLMin = longestBL
                        - static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numBeats);
                longestBLMax = longestBL
                        + static_cast<int64_t>((kMaxSecsPhaseErr * sr) / numBeats);
                break;
            }
        }
    }

    const mixxx_impl::Bpm lo(60.0 * sr / longestBLMax);
    const mixxx_impl::Bpm hi(60.0 * sr / longestBLMin);
    const mixxx_impl::Bpm center(60.0 * sr / longestBL);
    return roundBpmWithinRange(lo, center, hi);
}

} /* anonymous namespace */
} /* namespace mixxx_impl */

/* ====================================================================
 * Analysis pipeline
 */

struct AnalysisResult {
    bool success                 = false;
    double estimatedBPM          = 0;
    std::vector<double> beatFramePositions;
    std::vector<double> beatTimesSec;
    std::string bpmMethod;        /* "musical" | "queen-mary" */
};

AnalysisResult runBeatDetection(
        const std::vector<float>& left,
        const std::vector<float>& right,
        double sampleRate,
        bool queenMaryMode)
{
    AnalysisResult result;

    if (left.size() != right.size()) {
        std::cerr << "Error: left/right mismatch\n";
        return result;
    }
    if (left.empty()) {
        std::cerr << "Error: no audio frames provided\n";
        return result;
    }

    /* Step 1: DF config */
    int stepSizeSamples = int(round(sampleRate * 0.01161));
    int windowTarget = int(sampleRate / 50);
    int windowSize = 1;
    while (windowSize < windowTarget) windowSize <<= 1;
    if (windowSize < 64) windowSize = 64;

    DFConfig dfConfig;
    dfConfig.stepSize           = stepSizeSamples;
    dfConfig.frameLength        = windowSize;
    dfConfig.DFType             = DF_COMPLEXSD;
    dfConfig.dbRise             = 3;
    dfConfig.adaptiveWhitening  = false;
    dfConfig.whiteningRelaxCoeff = -1;
    dfConfig.whiteningFloor     = -1;

    DetectionFunction* pDF = new DetectionFunction(dfConfig);
    if (!pDF) return result;

    /* Step 2: Process samples */
    size_t nFrames = left.size();
    std::vector<double> detectionResults;
    std::vector<double> windowed(windowSize);
    size_t i = 0;
    while (i + windowSize <= nFrames) {
        for (int j = 0; j < windowSize && (i+j) < nFrames; ++j) {
            windowed[j] = (left[i+j] + right[i+j]) * 0.5;
        }
        detectionResults.push_back(pDF->processTimeDomain(windowed.data()));
        i += stepSizeSamples;
    }

    /* Step 3: Trim trailing zeros, skip first 2 */
    int dfLen = static_cast<int>(detectionResults.size());
    while (dfLen > 0 && detectionResults[dfLen - 1] <= 0.0) --dfLen;
    if (dfLen < 2) { std::cerr << "Error: too few detection frames (" << dfLen << ")\n"; delete pDF; return {}; }

    std::vector<double> df;
    df.reserve(dfLen - 2);
    for (int i2 = 2; i2 < dfLen; ++i2) df.push_back(detectionResults[i2]);

    /* Step 4: TempoTrackV2 */
    TempoTrackV2 tt(float(sampleRate), stepSizeSamples);
    int periodSize = static_cast<int>(df.size()) / 128 + 1;
    std::vector<int> beatPeriod(periodSize);
    tt.calculateBeatPeriod(df, beatPeriod, 120.0, false);

    /* Step 5: Calculate beat positions */
    std::vector<double> beats;
    tt.calculateBeats(df, beatPeriod, beats, 0.9, 4.0);

    /* Convert to actual frame positions (mirrors mixxx exactly) */
    result.success = true;
    for (size_t i2 = 0; i2 < beats.size(); ++i2) {
        double framePos = beats[i2] * stepSizeSamples + double(stepSizeSamples) / 2.0;
        result.beatFramePositions.push_back(framePos);
        result.beatTimesSec.push_back(framePos / sampleRate);
    }

    /* Step 6: Estimate BPM */
    if (queenMaryMode) {
        /* Queen Mary VAMP original: dominant period from resonator
         * comb filter — NOT what Mixxx does. */
        result.bpmMethod = "queen-mary";

        double estBPM = 0, bestScore = -1;
        for (size_t i2 = 0; i2 < beatPeriod.size(); ++i2) {
            if (beatPeriod[i2] < 1) continue;
            double bpm = (60.0 * sampleRate / stepSizeSamples) / double(beatPeriod[i2] + 1);
            if (bpm < 30.0 || bpm > 300.0) continue;
            double score = (i2 < df.size()) ? df[i2] : 0.0;
            if (score > bestScore) { bestScore = score; estBPM = bpm; }
        }

        /* Fallback to beat spacing if contour gave nothing */
        if (estBPM < 30.0 && beats.size() >= 2) {
            double total = 0;
            for (size_t b = 1; b < beats.size(); ++b) total += beats[b] - beats[b-1];
            estBPM = 60.0 * sampleRate / (total / double(beats.size() - 1) * stepSizeSamples);
        }
        result.estimatedBPM = estBPM;

    } else {
        /* Default: exactly what Mixxx does.
         *
         * Mixxx beat detection flow:
         *   1. TempoTrackV2::calculateBeats —> DF-frame positions (beats)
         *   2. Convert to sample-frame:
         *      framePos = beats[i] * stepSize + stepSize/2
         *   3. retrieveConstRegions(sampleFrame) —> ConstRegion[]
         *   4. find longest constant region —> beatLength (in SAMPLE frames)
         *   5. centreBPM = 60 * sr / beatLength   ← beatLength in SAMPLES
         *   6. snap to nearest musical BPM
         *
         * Critical: retrieveConstRegions must receive SAMPLE-frame positions,
         * not DF-frame positions (the latter gives 60*sr/43 = 61535).
         */
        result.bpmMethod = "musical";

        using namespace mixxx_impl;
        auto regions = retrieveConstRegions(
            result.beatFramePositions,       /* sample-frame, NOT beats[i] */
            static_cast<audio::SampleRate>(sampleRate));
        const Bpm musical = makeConstBpm(
            regions, static_cast<audio::SampleRate>(sampleRate));
        result.estimatedBPM = musical.isValid() ? musical.value() : 0.0;
    }

    /* Output */
    std::printf("\n=== RESULT ===\n");
    // Print BPM with 2 decimal places (e.g. 120.00, 115.33, 93.12).
    // The test script parses with r"BPM:\s*([\d.]+)", so keep the decimal.
    std::printf("BPM: %.2f  (method: %s)\n",
        result.estimatedBPM, result.bpmMethod.c_str());
    if (result.estimatedBPM > 0) {
        std::printf("Beat interval: %.0f ms\n", 60000.0 / result.estimatedBPM);
    }

    /* Diagnostic info */
    std::printf("\nDF frames: %zu  |  Step: %d  |  Window: %d\n",
        detectionResults.size(), stepSizeSamples, windowSize);
    std::printf("Beats detected: %zu\n", beats.size());
    if (!beats.empty()) {
        int nPrint = std::min(static_cast<size_t>(10), beats.size());
        std::printf("\nFirst %d beats:\n", nPrint);
        for (int i2 = 0; i2 < nPrint; ++i2) {
            std::printf("  #%-3d  frame=%.0f  time=%.3fs\n",
                i2, result.beatFramePositions[i2], result.beatTimesSec[i2]);
        }
        if (beats.size() > size_t(nPrint)) {
            std::printf("  ... (%zu total)\n", beats.size());
        }
    }

    delete pDF;
    return result;
}

/* ====================================================================
 * entrypoint
 */

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " [--queen-mary] <audio.wav>\n\n"
                  << "  --queen-mary  Queen Mary VAMP dominant-period estimation\n"
                  << "                (useful for comparing against Mixxx's\n"
                  << "                 beat-grid → musical-BPM approach).\n";
        return 1;
    }

    bool queenMary = false;
    std::string wavPath;
    for (int a = 1; a < argc; ++a) {
        if (std::string(argv[a]) == "--queen-mary") {
            queenMary = true;
        } else {
            wavPath = argv[a];
        }
    }

    WavInfo wav = loadWave(wavPath);
    if (wav.numFrames <= 0) {
        std::cerr << "Error: failed to load WAV\n";
        return 1;
    }

    std::printf("\nLoaded: %s\n", wavPath.c_str());
    std::printf("  Sample rate: %.0f Hz  |  Channels: %d  |  Duration: %.1f s\n\n",
        wav.sampleRate, wav.channels,
        double(wav.numFrames) / wav.sampleRate);

    /* Extract left/right from interleaved data (mirrors mixxx)
     * wav.samples[] is interleaved: L, R, L, R ... */
    int numFrames = wav.numFrames;
    std::vector<float> left(numFrames, 0), right(numFrames, 0);
    for (int f = 0; f < numFrames; ++f) {
        /* stereo: even indices = left, odd = right */
        left[f]  = (wav.channels >= 1) ? wav.samples[f * wav.channels]     : 0;
        right[f] = (wav.channels >= 2) ? wav.samples[f * wav.channels + 1] : 0;
    }

    AnalysisResult result = runBeatDetection(left, right, wav.sampleRate, queenMary);
    return result.success ? 0 : 1;
}
