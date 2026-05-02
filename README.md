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
./analyze_audio <file.wav>
```

### Output example

```
Input: two.wav
  Sample rate: 44100 Hz
  Channels: 2
  Frames: 705600
  Duration: 16.0 s

DF frames collected: 1377
Step size (samples): 512
Window size (samples): 1024
DF values used (after skip): 1375
Number of beats detected: 31
Estimated BPM: 117.5
Beat interval: 511 ms

First 10 beats:
  #0    frame=21248  time=0.482s
  ...
```

### Expected results

| Test file                  | Expected BPM | Beat interval |
|----------------------------|------------- |---------------|
| metronome ticks at 120 BPM | **120 ± 3**  | **~500 ms**    |

The wrapper is a thin C++ entry-point mirroring mixxx's
`AnalyzerQueenMaryBeats` plugin line-by-line.  It:

1. Reads a WAV file (PCM 8/16/24/32-bit, mono or stereo)
2. Downmixes stereo → mono (averaging channels)
3. Applies the Hann window and frames with 512-sample hop
4. Runs the Queen Mary DF_COMPLEXSD detection function
5. Feeds detection function values into TempoTrackV2
6. Outputs estimated BPM + beat positions

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
the qm-dsp pieces together into a working beat detector.  It follows
`mixxx/src/analyzer/plugins/analyzerqueenmarybeats.cpp` as the gold
standard, mirroring every step:

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
- **BPM conversion** — `BPM = (60 × sRate / hop) / (period + 1)`

### Phase III — Build and verify

The result is verified against a 120 BPM metronome test file
(`~/Documents/two.wav`):

```
./analyze_audio /home/ian/Documents/two.wav
# Expected: ~120 BPM, beats every 500 ms
```

## License

The qm-dsp library is licensed under the **GNU GPL v2 or later**.
See `lib/qm-dsp/` for full license text.
