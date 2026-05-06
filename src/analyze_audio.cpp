/* analyze_audio.cpp — Standalone Queen Mary BPM detector
 *
 * Reads a WAV file, runs the Queen Mary onset detection + tempo tracking
 * pipeline (mirroring AnalyzerQueenMaryBeats in the mixxx codebase), and
 * prints the estimated BPM and beat locations.
 *
 * BPM logic: BeatUtils from src/beatutils_standalone.h
 *   (clean-room copy of Mixxx src/track/beatutils.h + beatutils.cpp)
 *   Uses double-precision FramePos and FrameDiff_t per Mixxx source.
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

/* BPM logic from Mixxx: src/track/beatutils.h + beatutils.cpp + bpm.h */
#include "src/beatutils_standalone.h"

/* === WAV loading via libsndfile === */

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

/* BPM types: Bpm, FramePos, FrameDiff_t, BeatUtils (from beatutils_standalone.h) */

/* BPM uses BeatUtils from beatutils_standalone.h
 * double-precision FramePos / FrameDiff_t per Mixxx source.
 */

struct ConstRegion {
    // Uses BeatUtils::ConstRegion (audio::FramePos, audio::FrameDiff_t)
};

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

    /* Step 1: DF config — match Mixxx exactly: truncation (not rounding) */
    int stepSizeSamples = static_cast<int>(sampleRate * 0.01161f);
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
        double framePos = beats[i2] * stepSizeSamples + stepSizeSamples / 2;
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

        /* Use BeatUtils from beatutils_standalone.h (double-precision per Mixxx) */
        fprintf(stderr, "[BEATS_DEBUG] First 10 beats (framePos):\n");
        for (size_t j = 0; j < std::min((size_t)10, result.beatFramePositions.size()); ++j) {
            fprintf(stderr, "  beat[%zu] = %.6f\n", j, result.beatFramePositions[j]);
        }

        std::vector<audio::FramePos> framePosBeats;
        framePosBeats.reserve(result.beatFramePositions.size());
        for (double fp : result.beatFramePositions) {
            framePosBeats.push_back(audio::FramePos(fp));
        }

        auto regions = BeatUtils::retrieveConstRegions(framePosBeats, sampleRate);

        /* Debug: print all regions */
        fprintf(stderr, "[REGION_DEBUG] regions.size()=%zu\n", regions.size());
        for (size_t i = 0; i < regions.size(); ++i) {
            fprintf(stderr, "  region[%zu]: firstBeat=%.6f beatLength=%.6f\n",
                i, regions[i].firstBeat.value(), regions[i].beatLength);
        }
        fprintf(stderr, "  beats.size()=%zu beatSize=%zu\n", framePosBeats.size(), framePosBeats.back().value());

        const Bpm musical = BeatUtils::makeConstBpm(regions, sampleRate, nullptr);
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

/* ============================================================================
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
