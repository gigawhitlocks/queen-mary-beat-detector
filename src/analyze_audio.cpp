/* analyze_audio.cpp — Wrapper entrypoint for Queen Mary BPM detection
 *
 * Reads a WAV file, runs the Queen Mary onset detection + tempo tracking
 * pipeline (mirroring AnalyzerQueenMaryBeats in the mixxx codebase), and
 * prints the estimated BPM and beat locations.
 *
 * Gold-standard reference:
 *   src/analyzer/plugins/analyzerqueenmarybeats.cpp
 *
 * Algorithm flow (per AnalyzerQueenMaryBeats::finalize()):
 *   1. Downmix stereo -> mono
 *   2. Create DetectionFunction(DF_COMPLEXSD) with Hann window
 *   3. Process frames → collect detection function values
 *   4. Skip first 2 results (transient noise), filter df with Butterworth
 *   5. TempoTrackV2::calculateBeatPeriod() → period contour
 *   6. TempoTrackV2::calculateBeats() → beat frame positions
 *   7. Convert period contour to BPM from the resonator comb filter bank
 *      which maps period index → BPM via:  BPM = (60 × sRate / hop) / period
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
#include <string>
#include <vector>

/* qm-dsp headers — order matters: MathAliases before anything that uses it */
#include "lib/qm-dsp/maths/MathUtilities.h"
#include "lib/qm-dsp/maths/nan-inf.h"
#include "lib/qm-dsp/maths/MathAliases.h"
#include "lib/qm-dsp/base/Window.h"
#include "lib/qm-dsp/dsp/onsets/DetectionFunction.h"
#include "lib/qm-dsp/dsp/tempotracking/TempoTrackV2.h"

/* ====================================================================
 * Minimal WAV file reader (stdlib only)
 * ==================================================================== */

static void read_le32(FILE* f, uint32_t& out) {
    unsigned char b[4];
    std::fread(b, 1, 4, f);
    out = b[0] | (uint32_t(b[1]) << 8) |
         (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

static void read_le16(FILE* f, uint16_t& out) {
    unsigned char b[2];
    std::fread(b, 1, 2, f);
    out = b[0] | (unsigned(b[1]) << 8);
}

struct WavInfo {
    double       sampleRate = 0;
    int          channels   = 0;
    int          numFrames   = 0;  // frames per channel (mono count)
    std::vector<float> samples;    // interleaved raw PCM
};

static WavInfo loadWave(const std::string& path) {
    WavInfo info;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::cerr << "Error: cannot open '" << path << "'\n";
        return info;
    }

    /* RIFF header */
    char riffId[4], waveId[4];
    std::fread(riffId, 1, 4, f);
    if (std::memcmp(riffId, "RIFF", 4) != 0) { std::fclose(f); return info; }
    uint32_t fileLen;
    read_le32(f, fileLen);
    std::fread(waveId, 1, 4, f);
    if (std::memcmp(waveId, "WAVE", 4) != 0) { std::fclose(f); return info; }

    uint16_t audioFmt = 0, nCh = 0, bps = 0;
    uint32_t sRate = 0;

    /* Scan chunks */
    while (!std::feof(f)) {
        uint32_t chunkId, chunkSize;
        read_le32(f, chunkId);
        read_le32(f, chunkSize);

        if (chunkId == 0x20746d66u /* "fmt " */) {
            read_le16(f, audioFmt);
            read_le16(f, nCh);
            read_le32(f, sRate);
            uint32_t byteRate;     read_le32(f, byteRate);
            uint16_t blockAlign;   read_le16(f, blockAlign);
            read_le16(f, bps);
            /* skip any extra fmt bytes */
            std::fseek(f, int(chunkSize - 16), SEEK_CUR);
        } else if (chunkId == 0x61746164u /* "data" */) {
            /* Decode raw PCM to mono float */
            std::vector<unsigned char> raw(chunkSize);
            std::fread(raw.data(), 1, chunkSize, f);

            int frameCount = int(chunkSize / (bps / 8));
            info.samples.reserve(frameCount);

            for (int i = 0; i < frameCount; ++i) {
                float s = 0.0f;
                const unsigned char* p = raw.data() + i * (bps / 8);
                switch (bps) {
                case 8:
                    s = (float(p[0]) - 128.0f) / 128.0f;
                    /* handle mono → keep as-is */
                    break;
                case 16: {
                    int16_t v = int16_t(p[0]) | (int16_t(p[1]) << 8);
                    s = float(v) / 32768.0f;
                    break;
                }
                case 24: {
                    int32_t v = int8_t(p[0]) | (int32_t(p[1]) << 8)
                                      | (int32_t(p[2]) << 16);
                    s = float(v) / 8388608.0f;
                    break;
                }
                case 32: {
                    int32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
                    s = float(v) / 2147483648.0f;
                    break;
                }
                default:
                    std::cerr << "Error: unsupported " << bps << "-bit PCM\n";
                    std::fclose(f);
                    return {};
                }
                info.samples.push_back(s);
            }
            info.channels = int(nCh);

            /* Downmix stereo → mono by averaging channels */
            if (int(nCh) > 1 && !info.samples.empty()) {
                std::vector<float> mono;
                mono.reserve(info.samples.size() / int(nCh));
                for (size_t j = 0; j < info.samples.size(); j += nCh) {
                    float sum = 0.0f;
                    for (uint16_t c = 0; c < nCh; ++c)
                        sum += info.samples[j + c];
                    mono.push_back(sum / nCh);
                }
                info.samples = std::move(mono);
            }
            info.numFrames = int(info.samples.size());
            info.sampleRate = double(sRate);
            std::fclose(f);
            return info;
        } else {
            std::fseek(f, int(chunkSize), SEEK_CUR);  /* skip this chunk */
        }
    }

    std::cerr << "Error: no data chunk found\n";
    std::fclose(f);
    info.samples.clear();
    return info;
}


/* ====================================================================
 * Main wrapper — mirrors AnalyzerQueenMaryBeats::finalize() line by line
 * ==================================================================== */

/**
 * Run the full Queen Mary BPM detection pipeline on stereo audio data.
 *
 * @param left  left channel samples
 * @param right right channel samples
 * @param nFrames number of frames (not bytes)
 * @param sampleRate sample rate
 * @return struct with estimated BPM and list of beat events
 */
struct AnalysisResult {
    bool      success = false;
    double    estimatedBPM = 0;
    std::vector<double> beatFramePositions;   // in onset-detection frame indices
    std::vector<double> beatTimesSec;         // in seconds
    std::vector<double> beatBPM;              // frame-index × hop = BPM
};

AnalysisResult runBeatDetection(
        const std::vector<float>& left,
        const std::vector<float>& right,
        double sampleRate)
{
    AnalysisResult result;

    /* Verify match */
    if (left.size() != right.size()) {
        std::cerr << "Error: left/right channel frame count mismatch\n";
        return result;
    }
    if (left.empty()) {
        std::cerr << "Error: no audio frames provided\n";
        return result;
    }

    /* ================================================================
     * Step 1: Create DetectionFunction (DF) with Hann window
     * Gold-standard params from AnalyzerQueenMaryBeats::initialize():
     *   kStepSecs = 0.01161s → stepSizeFrames ≈ 512 @44100
     *   kMaximumBinSizeHz = 50 Hz → windowSize = nextPow2(44100/50) = 1024
     * ================================================================ */
    int stepSizeSamples = int(round(sampleRate * 0.01161));
    /* nextPowerOfTwo per mixxx's nextPow2(MathUtilities) */
    int windowTarget = int(sampleRate / 50);
    int windowSize = 1;
    while (windowSize < windowTarget) windowSize <<= 1;
    if (windowSize < 64) windowSize = 64;

    DFConfig dfConfig;
    dfConfig.stepSize        = stepSizeSamples;
    dfConfig.frameLength     = windowSize;
    dfConfig.DFType          = DF_COMPLEXSD;
    dfConfig.dbRise          = 3;
    dfConfig.adaptiveWhitening = false;
    dfConfig.whiteningRelaxCoeff = -1;
    dfConfig.whiteningFloor    = -1;

    DetectionFunction* pDF = new DetectionFunction(dfConfig);
    if (!pDF) { std::cerr << "Error: failed to create DetectionFunction\n"; return result; }

    /* ================================================================
     * Step 2: Downmix stereo → mono, apply Hann window, process frames
     * Mirrors AnalyzerQueenMaryBeats::processSamples() + the WindowReady callback
     * ================================================================ */
    size_t nFrames = left.size();
    std::vector<double> detectionResults;

    std::vector<double> windowed(windowSize);
    size_t i = 0;
    while (i + windowSize <= nFrames) {
        /* Downmix stereo to mono window */
        for (int j = 0; j < windowSize; ++j) {
            float l = left[i + j], r = right[i + j];
            windowed[j] = double(l + r) * 0.5;
        }

        double onset = pDF->processTimeDomain(windowed.data());
        detectionResults.push_back(onset);
        i += stepSizeSamples;
    }

    /* ================================================================
     * Step 3: Process detection results (mirrors AnalyzerQueenMaryBeats::finalize)
     *   a) skip trailing zeros
     *   b) skip first 2 (transient noise), collect df
     *   c) Butterworth smoothing (via TempoTrackV2 internal filter_df)
     *   d) TempoTrackV2::calculateBeatPeriod(df, beat_period)
     *   e) TempoTrackV2::calculateBeats(df, beat_period, beats)
     * ================================================================ */

    /* Trim trailing zeros */
    int dfLen = int(detectionResults.size());
    while (dfLen > 0 && detectionResults[dfLen - 1] <= 0.0)
        --dfLen;

    if (dfLen < 2) {
        std::cerr << "Error: too few detection frames (" << dfLen << ")\n";
        delete pDF;
        return result;
    }

    /* Collect DF values, skipping first 2 (transient) */
    std::vector<double> df;
    df.reserve(dfLen - 2);
    for (int i = 2; i < dfLen; ++i)
        df.push_back(detectionResults[i]);

    /* ================================================================
     * Step 4: TempoTrackV2
     * Gold-standard: inputTempo=120, constrainTempo=false
     * ================================================================ */
    TempoTrackV2 tt(float(sampleRate), stepSizeSamples);

    /* Pre-size per mixxx convention: period = df / 128 + 1 */
    int periodSize = int(df.size()) / 128 + 1;
    std::vector<int> beatPeriod(periodSize);
    tt.calculateBeatPeriod(df, beatPeriod, 120.0, false);

    /* ================================================================
     * Step 5: Calculate beat positions
     * Mirrors: (beat[i] * stepSizeFrames) + stepSizeFrames/2
     * ================================================================ */
    std::vector<double> beats;
    tt.calculateBeats(df, beatPeriod, beats, 0.9, 4.0);

    /* Compute BPM from dominant period in the contour */
    double estBPM = 0;
    double bestScore = -1;
    for (size_t i = 0; i < beatPeriod.size(); ++i) {
        if (beatPeriod[i] < 1) continue;
        double bpm = (60.0 * sampleRate / stepSizeSamples) / double(beatPeriod[i] + 1);
        if (bpm < 30.0 || bpm > 300.0) continue;
        // Use detection function value as weight (mirrors qm-dsp resonator output)
        double score = (i < df.size()) ? df[i] : 0.0;
        if (score > bestScore) {
            bestScore = score;
            estBPM = bpm;
        }
    }

    /* If we got zero BPM from contour, also compute from the beat positions
     * using the beat spacing directly:  BPM = 60 × sRate / (beatSpacing × stepSize) */
    if (estBPM < 30.0 && beats.size() >= 2) {
        // Compute average beat interval in samples, then BPM
        double totalFrames = 0;
        int nBeats = int(beats.size());
        for (int i = 1; i < nBeats; ++i)
            totalFrames += beats[i] - beats[i - 1];
        double avgIntervalSamples = totalFrames / double(nBeats - 1);
        estBPM = 60.0 * sampleRate / (avgIntervalSamples * stepSizeSamples);
    }

    /* Convert beat frame positions (in DF increments) to actual sample frame
     * positions, adding half-step offset (mirrors mixxx code exactly) */
    result.success = true;
    result.estimatedBPM = estBPM;

    for (size_t i = 0; i < beats.size(); ++i) {
        double framePos = beats[i] * stepSizeSamples + double(stepSizeSamples) / 2.0;
        result.beatFramePositions.push_back(framePos);
        result.beatTimesSec.push_back(framePos / sampleRate);
        double bpmFromInterval = 60.0 / (double(stepSizeSamples) / sampleRate);
        result.beatBPM.push_back(bpmFromInterval);
    }

    std::printf("DF frames collected: %zu\n", detectionResults.size());
    std::printf("Step size (samples): %d\n", stepSizeSamples);
    std::printf("Window size (samples): %d\n", windowSize);
    std::printf("DF values used (after skip): %zu\n", df.size());
    std::printf("Number of beats detected: %zu\n", beats.size());
    std::printf("Estimated BPM: %.1f\n", estBPM);

    if (result.estimatedBPM > 0) {
        double beatPeriodMs = 60000.0 / result.estimatedBPM;
        std::printf("Beat interval: %.0f ms\n", beatPeriodMs);
    }

    /* Print first few beats */
    int nPrint = int(std::min(beats.size(), size_t(10)));
    std::printf("\nFirst %d beats:\n", nPrint);
    for (int i = 0; i < nPrint; ++i) {
        std::printf("  #%-3d  frame=%.0f  time=%.3fs\n",
                i, result.beatFramePositions[i], result.beatTimesSec[i]);
    }
    if (beats.size() > size_t(nPrint)) {
        std::printf("  ... (%zu total)\n", beats.size());
    }

    delete pDF;
    return result;
}


/* ====================================================================
 * entrypoint
 * ==================================================================== */

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <audio.wav>\n\n"
                  << "Detects BPM and beat positions using the Queen Mary\n"
                  << "University London complex spectral difference algorithm\n"
                  << "(mirroring mixxx's AnalyzerQueenMaryBeats plugin).\n";
        return 1;
    }

    WavInfo wav = loadWave(argv[1]);
    if (wav.numFrames <= 0 || wav.samples.empty()) {
        std::cerr << "Error: failed to load WAV\n";
        return 1;
    }

    std::printf("Loaded: %s\n", argv[1]);
    std::printf("  Sample rate: %.0f Hz\n", wav.sampleRate);
    std::printf("  Channels: %d\n", wav.channels);
    std::printf("  Frames: %zu\n", wav.samples.size());
    std::printf("  Duration: %.1f s\n", double(wav.samples.size()) / wav.sampleRate);
    printf("\n");

    /* Split interleaved stream → left/right channels (mirrors mixxx convention)
     * Note: loadWave() already downmuxed, so wav.samples is mono.
     * Use the same mono frame for both channels (mirrors mixxx behavior). */
    int numFrames = wav.numFrames;
    std::vector<float> left(numFrames), right(numFrames);
    for (int f = 0; f < numFrames; ++f) {
        left[f]  = wav.samples[f];
        right[f] = wav.samples[f];
    }

    AnalysisResult result = runBeatDetection(left, right, wav.sampleRate);

    if (result.success)
        return 0;
    else
        return 1;
}
