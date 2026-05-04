# AGENTS.md — Queen Mary BPM Standalone Library

## What This Project Is

A fully standalone C++ library and CLI tool implementing the **Queen Mary
University London BPM beat detection algorithm**.  It extracts the algorithm
from [mixxx](https://github.com/mixxxdj/mixxx) (GPL-2.0) along with its
vendored `qm-dsp` and `kissfft` dependencies into a self-contained project
that compiles with zero external dependencies beyond C++17.

## Relationship to Mixxx

| Aspect        | Mixxx                                      | This Project                         |
|-----          | ------                                    | ---------                            |
| **algorithm** | Queen Mary beats detector                   | Same algorithm (unchanged)           |
| **source**    | `src/analyzer/plugins/analyzerqueenmarybeats.cpp` + header | `src/analyze_audio.cpp` (thin wrapper) |
| **dependencies** | Qt, Mixxx engine, waveform API           | None (stdlib only)                   |
| **purpose**   | Beat grid analysis for DJ track import       | CLI testing / standalone library       |
| **qmdsp**     | Vendored in `lib/qm-dsp/`                  | Copied + include-paths rewritten       |
| **kissfft**   | Vendored in `lib/qm-dsp/ext/kissfft/`      | Copied to `lib/kissfft/`             |
| **build**     | CMake (full mixxx tree)                     | Make file (or CMake)                 |

### Code provenance

The wrapper file **mirrors** `AnalyzerQueenMaryBeats` almost line-by-line:

| Mixxx step                                   | Wrapper step                                  |
|----                                          | ------                                       |
| `kStepSecs = 0.01161f`                         | `stepSizeSamples = round(sRate × 0.01161)`     |
| `nextPow2(sRate / 50)`                         | `windowSize = nextPow2(sRate / 50)`            |
| `DFConfig{DF_COMPLEXSD, 512, 1024, 3, …}`    | Same                                       |
| Downmix stereo → mono per-window               | Identical                                   |
| `pDF->processTimeDomain(window)`              | Identical                                   |
| Skip first 2 frames + trim trailing zeros     | Identical                                   |
| `TempoTrackV2(sRate, stepSize)`               | Identical                                   |
| `calculateBeatPeriod(df, beatPeriod, 120.0, false)` | Identical                           |
| `calculateBeats(df, beatPeriod, beats, 0.9, 4.0)`   | Identical                        |
| Frame → time: `(beats[i] × stepSize) + stepSize/2` | Identical                              |
| Frame → time: `(beats[i] × stepSize) + stepSize/2` | Identical                              |
| **Both BPM modes** | Both implemented                          |

### What the wrapper adds beyond AnalyzerQueenMaryBeats

The wrapper adds these capabilities that `AnalyzerQueenMaryBeats` does not provide:

- WAV file reader (mixxx streams from `QAudioSource`)
- Stereo → mono pre-downmix (`wav.samples[j + c]` averaged)
- Console output
- **`--queen-mary` flag**: Queen Mary VAMP's dominant-period contour BPM
  (the resonator comb filter output — this is how the original VAMP plugin
  computed BPM, but Mixxx discarded it)

The default BPM calculation replicates Mixxx exactly**:

    `analyze_audio <file.wav>` → "musical BPM" (80, 96, 120, 144, etc.)
    `analyze_audio --queen-mary <file.wav>` → raw contour BPM (~119.7)

The **default** path replicates Mixxx's downstream processing chain:

    ```
    beats[] (from TempoTrackV2)
      → retrieveConstRegions()    ← "iron" the beat grid
      → find longest constant region
      → centreBPM = 60 × sr / beatLength
      → roundBpmWithinRange()     ← snap to musical BPM
                                   ← fractions: 1, 2, 2/3, 3, 1/12
    ```

This is **exactly** what Mixxx does in `BeatFactory::makePreferredBeats()`
→ `BeatUtils::retrieveConstRegions()` → `BeatUtils::makeConstBpm()`.

The **--queen-mary** path uses the resonator comb filter output
(`beatPeriod` from `calculateBeatPeriod()`), which is how the original
Queen Mary VAMP plugin computed BPM — but Mixxx discarded this value.
`AnalyzerQueenMaryBeats` only used it internally to produce beat positions;
it never exposed the period-to-BPM mapping.

## Directory Layout

```
extracted_qm_bpm/
├── src/
│   └── analyze_audio.cpp   ← thin wrapper (mirrors AnalyzerQueenMaryBeats)
├── lib/
│   ├── qm-dsp/             ← vendored Queen Mary DSP library
│   │   ├── maths/          ← MathUtilities, MathAliases, nan-inf
│   │   ├── dsp/
│   │   │   ├── onsets/     ← DetectionFunction (DF_COMPLEXSD)
│   │   │   ├── phasevocoder/ ← PhaseVocoder
│   │   │   ├── tempo-tracking/ ← TempoTrackV2 (Viterbi + DP)
│   │   │   └── transforms/   ← FFT, FFTReal
│   │   └── base/           ← Window (Hann)
│   └── kissfft/            ← kissFFT (C implementation)
├── include/
│   └── qm_bpm.h            ← public standalone API header
├── Makefile / CMakeLists.txt  ← build files
├── README.md               ← usage (how to build, run tests)
├── PLAN.md                 ← original extraction plan
└── AGENTS.md               ← this file (history & provenance)
```

## Algorithm Flow

Beat generation follows the Queen Mary VAMP pipeline. BPM estimation
then diverges into two modes:

### Beat grid (identical for both modes)

```
WAV → downmix stereo→mono → Hann window → FFT magnitude phase
  → DF_COMPLEXSD (magnitude diff + phase dev)
  → Butterworth filter (bidirectional 2nd order)
  → Resonator comb filter bank (128 period indices)
  → Viterbi decoding (σ=8, transition Gaussian)
  → Dynamic programming beat tracking (α=0.9, tightness=4)
```

### Default mode: Mixxx-style "musical BPM" snapping

```
beats[] (from beat tracking)
  → retrieveConstRegions()    ← "iron" the beat grid, find constant-spacing regions
  → find longest constant region
  → centreBPM = 60 × sr / beatLength  (beatLength in SAMPLE frames)
  → roundBpmWithinRange()     ← snap to nearest musical BPM
                               ← fractions: 1, 2, 2/3, 3, 1/12
  → output: 80, 96, 106.7, 120, 144, …
```

### --queen-mary mode: original VAMP contour BPM

```
beatPeriod[] (from resonator comb filter)
  → find dominant period with highest detection function score
  → frameBPM = (60 × sRate / stepSize) / (period + 1)
  → output: raw float (~117.3, 120.7, 127.5, …)
```

### Key params (from mixxx defaults)

| Param               | Value        | Source                          |
|-----                 | -----        | -----                           |
| Sample rate          | 44100 Hz     | mixxx default                   |
| Step size            | 512 frames   | ≈ 11.6 ms @ 44100 Hz          |
| Window size          | 1024         | nextPow2(44100/50)            |
| DF type              | DF_COMPLEXSD | Queen Mary default              |
| Butterworth coeffs   | [0.2066, 0.4131, 0.2066]/[1, -0.3695, 0.1958] | (2nd order) |
| RCF period range     | 20–128 df increments | TempoTrackV2::wv_len=128 |
| Viterbi σ            | 8.0          | TempoTrackV2::viterbi_decode  |
| DP α (prev beat weight) | 0.9       | TempoTrackV2::calculateBeats  |

## Testing

### Quick smoke test

The project ships a 120 BPM metronome test file:

```bash
./analyze_audio metronome-ticks_4-4_120-BPM.wav
# Expected: ~120 BPM, beats every 500 ms
```

### Regression test suite

A Python 3 (unmodified, system stock — **no virtualenv required**) regression
test suite lives at `tests/test_default_bpm.py`.  It runs `analyze_audio`
in its **default mode** (no `--queen-mary` flag) on a curated set of WAV
files and checks the reported BPM against known reference values with
2-decimal-place accuracy (`unittest.assertAlmostEqual`).

**Run all tests from the project root:**

```bash
cd /home/ian/mixxx/extracted_qm_bpm
python3 tests/test_default_bpm.py
```

Or equivalently (the script resolves all paths relative to its own location):

```bash
python3 tests/test_default_bpm.py
```

**Run a subset by name:**

```bash
python3 tests/test_default_bpm.py -k metronome     # only metronome files
python3 tests/test_default_bpm.py -k Duvet          # only Duvet tracks
python3 tests/test_default_bpm.py -k aNYway         # only aNYway tracks
```

**Verbosity and output modes:**

```bash
python3 tests/test_default_bpm.py -v               # verbose pass/fail listing
python3 tests/test_default_bpm.py --tb=long        # include tracebacks on failure
```

### What the test covers

| Test group | Files | Purpose |
|---|---|---|
| Metronome ticks | 5 WAV (60, 88, 120, 129, 144 BPM) | Crisp test tones — the analyzer should return the exact expected BPM |
| Song excerpts | 7 WAV | Short clips (≈6 s each) for fast CI-like runs |
| Full tracks | 5 WAV | Longer source tracks (up to ~9 M frames) |

Each file in the table maps to a test method named
`test_bpm_<stem>` (e.g. `test_bpm_01 - Duvet`).  Filenames are aligned to
the **actual on-disk names** in the `metronome-ticks/` and `song-tests/`
directories (e.g. `01` not `1`, `06` not `6`, `manaya` not `manyana`).

### Adding a new reference file

Append a `(relative_path, expected_bpm)` tuple to the `TESTS` list in
`tests/test_default_bpm.py`.  The path is relative to `tests/` and the
BPM should be the known-correct musical BPM to two decimal places.

## License

- **qm-dsp**: GPL-2.0 — see `lib/qm-dsp/`
- **kissfft**: BSD-3-Clause — see `lib/kissfft/`
- **Wrapper code** (`analyze_audio.cpp`): GPL-2.0-inherited (mirrors mixxx)
