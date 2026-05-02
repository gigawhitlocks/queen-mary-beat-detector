/* test_bpm.cpp — Standalone test: load WAV, run QM BPM detection, print result */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "include/qm_bpm.h"

static void print_usage(const char* prog) {
    std::fprintf(stderr, "Usage: %s <input.wav> [options]\n", prog);
    std::fprintf(stderr, "Options:\n");
    std::fprintf(stderr, "  --sample-rate Hz      Audio sample rate (default: 44100)\n");
    std::fprintf(stderr, "  --df-step N           DF hop in samples (default: auto)\n");
    std::fprintf(stderr, "  --df-frame-length N   Analysis window size (default: auto)\n");
    std::fprintf(stderr, "  --input-tempo BPM     Input tempo guess (default: 120)\n");
    std::fprintf(stderr, "  --constrain-tempo     Constrain to input tempo\n");
    std::exit(1);
}

int main(int argc, char** argv) {
    if (argc < 2) print_usage(argv[0]);

    // Parse WAV file path (first non-option argument)
    const char* wavPath = nullptr;
    QueenMaryBeatConfig cfg;
    cfg.sampleRate = 44100.0;
    cfg.dfStepSamples = 0;
    cfg.dfFrameLength = 0;
    cfg.dfType = DF_COMPLEXSD;
    cfg.inputTempo = 120.0;
    cfg.constrainTempo = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc)
            cfg.sampleRate = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--df-step") == 0 && i + 1 < argc)
            cfg.dfStepSamples = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--df-frame-length") == 0 && i + 1 < argc)
            cfg.dfFrameLength = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--input-tempo") == 0 && i + 1 < argc)
            cfg.inputTempo = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--constrain-tempo") == 0)
            cfg.constrainTempo = true;
        else if (argv[i][0] != '-')
            wavPath = argv[i];
        else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
        }
    }

    if (!wavPath) {
        std::fprintf(stderr, "Error: no WAV file path provided\n");
        print_usage(argv[0]);
    }

    // Load WAV file
    WavFile wav = loadWavFile(wavPath);
    if (!wav.valid) {
        std::fprintf(stderr, "Error loading WAV: %s\n", wav.error.c_str());
        return 1;
    }
    std::printf("Loaded: %s  (%.0f Hz, %d ch, %d-bit, %.3f s, %zu samples)\n",
                wavPath, wav.sampleRate, wav.channels, wav.bitsPerSample,
                static_cast<double>(wav.samples.size()) / wav.sampleRate,
                wav.samples.size());

    // Create and run beat detector
    // Override sample rate if it differs from WAV's actual rate
    if (cfg.sampleRate == 44100.0 && wav.sampleRate != 44100.0)
        cfg.sampleRate = wav.sampleRate;

    QueenMaryBeatDetector detector(cfg);

    // Feed all audio
    detector.addMono(wav.samples.data(), wav.samples.size());

    // Finalize
    if (!detector.finalize()) {
        std::fprintf(stderr, "Beat detection: not enough data\n");
        return 1;
    }

    // Print results
    double bpm = detector.getEstimatedBPM();
    std::printf("\n=== Queen Mary BPM Result ===\n");
    std::printf("Estimated BPM: %.2f\n", bpm);

    if (cfg.inputTempo > 0) {
        double error = std::abs(bpm - cfg.inputTempo);
        std::printf("Input tempo:   %.2f BPM\n", cfg.inputTempo);
        std::printf("Deviation:     %.2f%%\n",
                    (error / cfg.inputTempo) * 100.0);
    }

    std::printf("\nBeats (%zu)\n", detector.getBeats().size());
    const auto& beats = detector.getBeats();
    for (size_t i = 0; i < beats.size() && i < 20; ++i) {
        std::printf("  beat %3zu: t = %.4f s  (frame offset: %.1f)\n",
                    i, beats[i].timeSec, beats[i].frameOffset);
    }
    if (beats.size() > 20)
        std::printf("  ... (%zu more beats)\n", beats.size() - 20);
    std::printf("================================\n");

    return 0;
}
