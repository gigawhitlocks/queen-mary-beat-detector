# NOTICE: this program was extracted from [Mixxx](https://github.com/mixxxdj/mixxx) by Qwen3.6:35B as an experiment
## Everything that follows is robot-written. NO GUARANTEES

# Queen Mary BPM — Standalone Beat Detection

Standalone C++ library implementing the **Queen Mary University
(London) BPM beat detection algorithm** (complex spectral difference
onset detection → resonator comb filter bank → Viterbi tempo contour →
dynamic programming beat tracking).

Extracted from the [mixxx](https://github.com/mixxxdj/mixxx) source
tree with the vendored `qm-dsp` dependency.

## Build

```bash
cd extracted_qm_bpm
make
```

Produces two artifacts:

| Artifact       | Description                                          |
|----------------|------------------------------------------------------|
| `libqm_bpm.a` | Static library of qm-dsp sources (kissfft, …)       |
| `analyze_audio` | Wrapper binary that runs detection on a WAV file   |

You can also run `make clean` to remove objects, the library, and the
binary.

### Dependencies

Zero.  Everything compiles with only:

- a C++17 compiler (gcc, clang, …)
- the standard C/C++ libraries (`libm`)

All qm-dsp and kissfft sources are vendored under `lib/`.

## Usage

```bash
# Default: Mixxx-style constant-region BPM snapping (musical BPM)
./analyze_audio <file.wav>

# Queen Mary VAMP original: dominant period from resonator comb filter
./analyze_audio --queen-mary <file.wav>
```

### Output example (default — musical BPM)

```
=== RESULT ===
BPM: 120  (method: musical)
Beat interval: 500 ms

DF frames: 1375  |  Step: 512  |  Window: 1024
Beats detected: 31

First 10 beats:
  #0    frame=21248  time=0.482s
  ...
```

### Output example (--queen-mary — contour BPM)

```
=== RESULT ===
BPM: 118  (method: queen-mary)
Beat interval: 508 ms

DF frames: 1375  |  Step: 512  |  Window: 1024
Beats detected: 31

First 10 beats:
  #0    frame=21248  time=0.482s
  ...
```

### Expected results

| Test file                  | Default (musical) | --queen-mary  |
|---------------------------|-------------------|---------------|
| metronome ticks at 120 BPM | **120**           | ~117–119      |

### Default mode: Mixxx-style "musical BPM" snapping

By default, the wrapper replicates Mixxx's full BPM derivation pipeline:

```
beats[] (from TempoTrackV2)
  → retrieveConstRegions()    ← "iron" the beat grid, find constant-spacing regions
  → find longest region
  → centreBPM = 60 × sr / beatLength  (beatLength in SAMPLE frames)
  → roundBpmWithinRange()     ← snap to nearest musical BPM
                               ← fractions: 1, 2, 2/3, 3, 1/12
  → output: 80, 96, 106.7, 120, 144, … (integer or 1-digit decimal)
```

This matches Mixxx's `BeatFactory::makePreferredBeats()` →
`BeatUtils::retrieveConstRegions()` → `BeatUtils::makeConstBpm()`.

### --queen-mary mode: original VAMP contour BPM

The `--queen-mary` flag uses the resonator comb filter output
(`beatPeriod` from `calculateBeatPeriod()`) to compute BPM via the
dominant period — this is how the original Queen Mary VAMP plugin computed
BPM. Mixxx discarded this value and replaced it with the constant-region
approach because it produces "hard" musical BPM values (80, 96, 120, 144)
that are more suitable for DJ grid alignment.

### What the wrapper adds beyond AnalyzerQueenMaryBeats

The wrapper adds these capabilities that `AnalyzerQueenMaryBeats` does not
provide:

- WAV file reader (mixxx streams from `QAudioSource`)
- Stereo → mono pre-downmix (`wav.samples[j + c]` averaged)
- **Two BPM modes**:
  - **Default** = Mixxx-style constant-region snapping → musical BPM
  - **--queen-mary** = original VAMP dominant-period contour BPM
- Console output

## Source layout

```
extracted_qm_bpm/
├── src/
│   └── analyze_audio.cpp      ← wrapper entrypoint
├── lib/
│   ├── qm-dsp/                ← vendored qm-dsp library
│   │   ├── maths/
│   │   ├── dsp/
│   │   └── base/
│   └── kissfft/               ← vendored kissFFT
├── include/
│   └── qm_bpm.h               ← public API header (standalone)
├── CMakeLists.txt             ← CMake build (alternative)
├── Makefile                   ← Make build (this file)
└── README.md                  ← this file
```

## How it was created

This project was extracted from the mixxx repository by following a
step-by-step plan (see `PLAN.md`):

### Phase I — Copy qm-dsp libraries

40 files were copied from mixxx's `lib/qm-dsp/` tree into
`extracted_qm_bpm/lib/`:

| Source (mixxx)                     | Destination              |
|------------------------------------|--------------------------|
| `lib/qm-dsp/ext/kissfft/*`         | `lib/kissfft/`           |
| `lib/qm-dsp/maths/*`               | `lib/qm-dsp/maths/`      |
| `lib/qm-dsp/base/Window.h`         | `lib/qm-dsp/base/Window.h` |
| `lib/qm-dsp/dsp/phasevocoder/*`    | `lib/qm-dsp/phasevocoder/` |
| `lib/qm-dsp/dsp/onsets/*`          | `lib/qm-dsp/dsp/onsets/`   |
| `lib/qm-dsp/dsp/tempotracking/*`   | `lib/qm-dsp/dsp/tempotracking/` |
| `lib/qm-dsp/dsp/transforms/FFT.*`  | `lib/qm-dsp/dsp/transforms/` |

Each file had its `#include` paths adjusted to be relative to the new
layout (e.g. `"dsp/transforms/FFT.h"` →
`"lib/qm-dsp/dsp/transforms/FFT.h"`).

### Phase II — Write the wrapper

A single C++ file (`src/analyze_audio.cpp`) was hand-written to glue
the qm-dsp pieces together into a working beat detector, then
refactored to add Mixxx-style BPM snapping.

**Part A — Beat detection (unchanged from mixxx):**

The wrapper mirrors `mixxx/src/analyzer/plugins/analyzerqueenmarybeats.cpp`
line-by-line through beat frame generation. It:

- **Detection function config** — `DF_COMPLEXSD`, step size 512
  frames (11.6 ms @ 44.1 kHz), frame length 1024 samples
- **Downmix-and-frame** — stereo → mono, Hann window, 512-sample hop
- **DF_COMPLEXSD processing** — one detection value per window
- **Butterworth smoothing** — bidirectional 2nd-order filter inside
  TempoTrackV2
- **Resonator comb filter bank** — period indices 1…128 at 128-frame
  hop
- **Viterbi decoding** — dynamic programming for best tempo contour
  (gaussian transition σ = 8, α = 0.9 tightness)
- **Beat tracking** — backtracking with dynamic programming (α = 0.9,
  tightness = 4)
- **Frame conversion** → `beatFramePos = beats[i] × stepSize + stepSize/2`

**Part B — BPM estimation (two modes):**

| Mode | Method | BPM values |
|------|--------|------------|
| **Default** | Const-region snapping (Mixxx) | 80, 96, 106.7, 120, 144… |
| **--queen-mary** | Resonator contour (VAMP) | 117.3, 120.7, 127.5… |

The **default** mode adds `BeatUtils::retrieveConstRegions()`,
`makeConstBpm()`, and `roundBpmWithinRange()` implemented in C++
(from `mixxx/src/track/beatutils.h` + `beatutils.cpp`). This produces
the same "hard" musical BPM values that Mixxx outputs — ideal for DJ
grid alignment.

### Phase III — Build and verify

The result is verified against a 120 BPM metronome test file
(`./metronome-ticks_4-4_120-BPM.wav`):

```
./analyze_audio ./metronome-ticks_4-4_120-BPM.wav
# Expected: ~120 BPM, beats every 500 ms
```

## License

The qm-dsp library is licensed under the **GNU GPL v2 or later**.
See `lib/qm-dsp/` for full license text.
