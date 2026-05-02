/* qm_bpm.cpp - Queen Mary BPM beat detection, standalone implementation
 * Bridges qm-dsp classes (DetectionFunction, TempoTrackV2, FFT, Window,
 * PhaseVocoder, MathUtilities, kissfft) into a single public API.
 */

#include "qm_bpm.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <complex>
#include <cstdio>
#include <functional>

// ---- Bring in all qm-dsp internals ----
#include "lib/qm-dsp/maths/MathUtilities.h"
#include "lib/qm-dsp/maths/nan-inf.h"
#include "lib/qm-dsp/maths/MathAliases.h"
#include "lib/qm-dsp/base/Window.h"
#include "lib/qm-dsp/dsp/transforms/FFT.h"
#include "lib/qm-dsp/dsp/phasevocoder/PhaseVocoder.h"
#include "lib/qm-dsp/dsp/onsets/DetectionFunction.h"
#include "lib/qm-dsp/dsp/tempotracking/TempoTrackV2.h"

// ======================================================================
// Hanning window (precomputed for a given size)
// ======================================================================
static std::vector<double> hanningWindow(size_t size) {
    std::vector<double> w(size);
    for (size_t i = 0; i < size; ++i)
        w[i] = 0.5 * (1.0 - cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(size - 1)));;
    return w;
}

// ======================================================================
// Overlap-add framing engine (simplified DownmixAndOverlapHelper)
// ======================================================================
class OverlapFrameEngine {
public:
    using WindowCallback = std::function<bool(double*, size_t)>;

    void initialize(int frameSize, int hop, WindowCallback cb) {
        m_frameSize = frameSize;
        m_hop = hop;
        m_callback = std::move(cb);
        m_buf.assign(frameSize, 0.0);
        m_pos = 0;
        m_window = hanningWindow(frameSize);
    }

    // Interleaved stereo: L0, R0, L1, R1, ...
    bool pushStereo(const float* samples, size_t nFrames) {
        size_t nCh = 2;
        size_t totalSamples = nFrames * nCh;
        for (size_t i = 0; i < totalSamples; ) {
            size_t avail = static_cast<size_t>(m_frameSize) - m_pos;
            size_t nAvail = totalSamples - i;
            size_t take = std::min(avail, nAvail);
            if (take == 0) {
                processWindow();
                if (!m_callback) return false;
                continue;
            }
            for (size_t j = 0; j < take; ++j) {
                size_t si = i + j;
                float ch0 = (si < nFrames) ? samples[si * 2] : 0.0f;
                float ch1 = (si + 1 < nFrames) ? samples[si * 2 + 1] : 0.0f;
                m_buf[m_pos + j] = static_cast<double>(0.5f * (ch0 + ch1));
            }
            i += take;
            m_pos += take;

            if (m_pos >= static_cast<size_t>(m_frameSize)) {
                processWindow();
                if (!m_callback) return false;
            }
        }
        return true;
    }

    // Mono audio
    bool pushMono(const float* samples, size_t nFrames) {
        for (size_t i = 0; i < nFrames; ) {
            size_t avail = static_cast<size_t>(m_frameSize) - m_pos;
            size_t nAvail = nFrames - i;
            size_t take = std::min(avail, nAvail);
            if (take == 0) {
                processWindow();
                if (!m_callback) return false;
                continue;
            }
            for (size_t j = 0; j < take; ++j)
                m_buf[m_pos + j] = static_cast<double>(samples[i + j]);
            i += take;
            m_pos += take;

            if (m_pos >= static_cast<size_t>(m_frameSize)) {
                processWindow();
                if (!m_callback) return false;
            }
        }
        return true;
    }

    void finalize() {
        // Zero-pad remaining buffer
        memset(m_buf.data() + m_pos, 0,
               (m_frameSize - m_pos) * sizeof(double));
        m_pos = static_cast<size_t>(m_frameSize);
        processWindow();
    }

private:
    void processWindow() {
        // Apply Hann window and callback
        if (m_callback) {
            for (size_t i = 0; i < static_cast<size_t>(m_frameSize); ++i)
                m_buf[i] *= m_window[i];
            m_callback(m_buf.data(), static_cast<size_t>(m_frameSize));
        }
        // Shift: overlap-add
        memmove(m_buf.data(), m_buf.data() + m_hop,
                (m_frameSize - m_hop) * sizeof(double));
        m_pos -= m_hop;
        memset(m_buf.data() + m_frameSize - m_hop - m_pos, 0,
               m_hop * sizeof(double));
    }

    int  m_frameSize = 0;
    int  m_hop       = 0;
    std::vector<double> m_buf;
    size_t m_pos    = 0;
    std::vector<double> m_window;
    WindowCallback m_callback;
};

// ======================================================================
// QueenMaryBeatDetector::Impl
// ======================================================================
class QueenMaryBeatDetector::Impl {
public:
    QueenMaryBeatConfig m_cfg;
    double m_sampleRate;
    int m_stepSize;        // DF hop in samples
    int m_frameLength;     // DF window size in samples
    std::vector<double> m_dfValues;
    OverlapFrameEngine m_frameEngine;
    std::unique_ptr<DetectionFunction> m_detector;
    std::vector<int> m_beatPeriods;
    std::vector<double> m_beatFrames;
    std::vector<BeatEvent> m_beats;
    double m_estimatedBPM = 0.0;

    Impl(const QueenMaryBeatConfig& cfg) : m_cfg(cfg), m_sampleRate(cfg.sampleRate) {
        // Compute parameters per Mixxx defaults:
        m_stepSize = cfg.dfStepSamples ? cfg.dfStepSamples :
            static_cast<int>(cfg.sampleRate * 0.01161);
        m_frameLength = cfg.dfFrameLength ? cfg.dfFrameLength :
            static_cast<int>(MathUtilities::nextPowerOfTwo(static_cast<int>(cfg.sampleRate / 50.0)));

        DFConfig dfCfg;
        dfCfg.stepSize      = m_stepSize;
        dfCfg.frameLength   = m_frameLength;
        dfCfg.DFType        = cfg.dfType;
        dfCfg.dbRise        = 3.0;
        dfCfg.adaptiveWhitening = false;
        dfCfg.whiteningRelaxCoeff = -1.0;
        dfCfg.whiteningFloor      = -1.0;

        m_detector = std::make_unique<DetectionFunction>(dfCfg);

        // Set up framing engine with callback to DetectionFunction
        m_frameEngine.initialize(m_frameLength, m_stepSize,
            [this](double* window, size_t) -> bool {
                m_dfValues.push_back(m_detector->processTimeDomain(window));
                return true;
            });
    }

    void addStereo(const float* left, const float* right, size_t nFrames) {
        // Downmix stereo → mono
        for (size_t i = 0; i < nFrames; ++i) {
            float ch0 = (left ? left[i] : 0.0f);
            float ch1 = (right ? right[i] : 0.0f);
            float mono = 0.5f * (ch0 + ch1);
            m_frameEngine.pushMono(&mono, 1);
        }
    }

    void addMono(const float* mono, size_t nFrames) {
        m_frameEngine.pushMono(mono, nFrames);
    }

    bool finalize() {
        m_frameEngine.finalize();

        size_t nzCount = m_dfValues.size();
        while (nzCount > 0 && m_dfValues[nzCount - 1] <= 0.0)
            --nzCount;

        if (nzCount < 4) return false;  // not enough data

        // Skip first 2 (transient noise)
        std::size_t required_size = std::max(std::size_t(2), nzCount) - 2;
        std::vector<double> df;
        df.reserve(required_size);
        for (std::size_t i = 2; i < nzCount; ++i)
            df.push_back(m_dfValues[i]);

        // Tempo tracking
        TempoTrackV2 tt(static_cast<float>(m_sampleRate), m_stepSize);
        tt.calculateBeatPeriod(df, m_beatPeriods, m_cfg.inputTempo, m_cfg.constrainTempo);

        // Beat positions (df increment units)
        tt.calculateBeats(df, m_beatPeriods, m_beatFrames, 0.9, 4.0);

        // Convert to frames and seconds
        m_beats.clear();
        m_beats.reserve(static_cast<int>(m_beatFrames.size()));
        for (size_t i = 0; i < m_beatFrames.size(); ++i) {
            double framePos = m_beatFrames[i] * m_stepSize + m_stepSize / 2;
            BeatEvent ev;
            ev.frameOffset = framePos / m_sampleRate;
            ev.timeSec = framePos / m_sampleRate;
            m_beats.push_back(ev);
        }

        // Estimate BPM: use average period from the contour
        // Period index p → beat every (p+1) DF increments
        // BPM = 60 / ((p+1) * stepSize / sampleRate)
        // Weight by detection function strength
        double maxDF = 0, sumW = 0, sumWbpm = 0;
        for (size_t i = 0; i < m_dfValues.size(); ++i)
            if (m_dfValues[i] > maxDF) maxDF = m_dfValues[i];

        for (size_t i = 0; i < m_beatPeriods.size(); ++i) {
            int p = m_beatPeriods[i];
            if (p < 10) continue;  // skip unrealistically short periods
            double bpm = (60.0 * m_sampleRate) / ((p + 1) * m_stepSize);
            double weight = m_dfValues[i] / (maxDF + 1e-6);
            sumW += weight;
            sumWbpm += weight * bpm;
        }

        m_estimatedBPM = (sumW > 0) ? sumWbpm / sumW : m_cfg.inputTempo;
        return true;
    }
};

// ======================================================================
// QueenMaryBeatDetector public API
// ======================================================================
QueenMaryBeatDetector::QueenMaryBeatDetector(const QueenMaryBeatConfig& cfg)
    : m_impl(std::make_unique<Impl>(cfg)) {}

QueenMaryBeatDetector::~QueenMaryBeatDetector() = default;

void QueenMaryBeatDetector::addStereo(const float* left, const float* right, size_t nFrames) {
    m_impl->addStereo(left, right, nFrames);
}

void QueenMaryBeatDetector::addMono(const float* mono, size_t nFrames) {
    m_impl->addMono(mono, nFrames);
}

bool QueenMaryBeatDetector::finalize() {
    return m_impl->finalize();
}

std::vector<BeatEvent> QueenMaryBeatDetector::getBeats() const {
    return m_impl->m_beats;
}

double QueenMaryBeatDetector::getEstimatedBPM() const {
    return m_impl->m_estimatedBPM;
}

// ======================================================================
// Minimal WAV file reader
// ======================================================================
WavFile loadWavFile(const char* path) {
    WavFile wav;
    wav.error.clear();

    FILE* f = std::fopen(path, "rb");
    if (!f) { wav.error = std::string("Cannot open file: ") + path; return wav; }

    // Read RIFF header
    char riff[4];
    if (std::fread(riff, 1, 4, f) != 4 ||
        std::memcmp(riff, "RIFF", 4) != 0) {
        wav.error = "Not a RIFF file";
        std::fclose(f); return wav;
    }
    uint32_t fileSize = 0;
    std::fread(&fileSize, 4, 1, f);
    (void)fileSize;

    char wave[4];
    if (std::fread(wave, 1, 4, f) != 4 ||
        std::memcmp(wave, "WAVE", 4) != 0) {
        wav.error = "Not a WAVE file";
        std::fclose(f); return wav;
    }

    // Parse chunks looking for 'fmt ' and 'data'
    uint32_t dataOffset = 0, dataSize = 0;
    uint16_t fmtTag = 0, nch = 0, bps = 0;
    uint32_t sampleRate = 0;

    while (true) {
        char chunk[4];
        uint32_t chunkSize = 0;
        if (std::fread(chunk, 1, 4, f) != 4) break;
        if (std::fread(&chunkSize, 4, 1, f) != 1) break;
        if (chunkSize > 100 * 1024 * 1024U) {
            wav.error = "Chunk too large";
            std::fclose(f); return wav;
        }

        if (std::memcmp(chunk, "data", 4) == 0) {
            dataOffset = static_cast<uint32_t>(std::ftell(f));
            dataSize = chunkSize;
            break;
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            std::fread(&fmtTag, sizeof(uint16_t), 1, f);
            std::fread(&nch, sizeof(uint16_t), 1, f);
            std::fread(&sampleRate, sizeof(uint32_t), 1, f);
            std::fread(&dataOffset /* bitrate */, 4, 1, f);  // skip bitrate
            uint16_t blockAlign = 0;
            std::fread(&blockAlign, sizeof(uint16_t), 1, f);
            std::fread(&bps, sizeof(uint16_t), 1, f);
            // Skip remaining fmt bytes
            if (chunkSize > 16) std::fseek(f, chunkSize - 16, SEEK_CUR);
        } else {
            // Skip
            uint32_t toSkip = chunkSize;
            if (toSkip == 0) break;
            std::fseek(f, toSkip, SEEK_CUR);
        }
    }

    if (dataOffset == 0 || dataSize == 0) {
        wav.error = "No data chunk found";
        std::fclose(f); return wav;
    }

    wav.sampleRate = static_cast<double>(sampleRate);
    wav.channels = nch;
    wav.bitsPerSample = bps;

    // Read sample data into a temporary buffer based on bit depth
    std::vector<float> rawSamp(dataSize / (bps / 8));
    switch (bps) {
    case 16: {
        std::int16_t s16;
        for (size_t i = 0; i < rawSamp.size(); ++i) {
            if (std::fread(&s16, sizeof(std::int16_t), 1, f) != 1) break;
            rawSamp[i] = static_cast<float>(s16) / 32768.0f;
        }
        break;
    }
    case 24: {
        uint8_t b[3];
        std::memset(b, 0, sizeof(b));
        for (size_t i = 0; i < dataSize; i += 3) {
            size_t got = std::fread(b, 1, 3, f);
            if (got < 3) break;
            std::int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(b[0]) |
                (static_cast<uint32_t>(b[1]) << 8) |
                (static_cast<uint32_t>(b[2]) << 16));
            // Sign extend 24-bit to 32-bit
            if (sample & (1 << 23)) sample |= 0xFF000000;
            rawSamp[i / 3] = static_cast<float>(sample) / 8388608.0f;
        }
        break;
    }
    case 32: {
        std::int32_t s32;
        for (size_t i = 0; i < rawSamp.size(); ++i) {
            if (std::fread(&s32, sizeof(std::int32_t), 1, f) != 1) break;
            rawSamp[i] = static_cast<float>(s32) / 2147483648.0f;
        }
        break;
    }
    default:
        wav.error = "Unsupported bit depth (only 16/24/32 supported)";
        break;
    }

    std::fclose(f);

    if (rawSamp.empty()) return wav;

    wav.samples = std::move(rawSamp);

    // Downmix to mono
    if (nch > 1) {
        std::vector<float> mono(wav.samples.size() / nch);
        for (size_t i = 0; i < mono.size(); ++i) {
            float sum = 0;
            for (int c = 0; c < nch; ++c)
                sum += wav.samples[i * nch + static_cast<size_t>(c)];
            mono[i] = sum / static_cast<float>(nch);
        }
        wav.samples = std::move(mono);
    }

    wav.valid = true;
    return wav;
}
