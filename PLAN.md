# Plan: Standalone Queen Mary BPM Algorithm Extraction

## Objective

Create /home/ian/mixxx/extracted_qm_bpm/ as a fully standalone C++ library and test program
that implements the Queen Mary University's BPM beat detection algorithm (VAMP plugin),
compiled against mixxx's vendored qm-dsp dependency and standalone WAV reader.

Source reference: mixxx's src/analyzer/plugins/analyzerqueenmarybeats.cpp
qm-dsp source: lib/qm-dsp/ (vendored in mixxx)

Algorithm: Complex Spectral Difference onset detection → Resonator Comb Filter bank →
Viterbi tempo contour → Dynamic programming beat tracking.

---

## Phase I — Copy qm-dsp library files (40 minutes)

### Step I.1: Copy FFT dependency (kissfft)
Copy 4 files from lib/qm-dsp/ext/kissfft/ -> extracted_qm_bpm/lib/kissfft/:
- kiss_fft.h, kiss_fft.c, kiss_fftr.h, kiss_fftr.c
Adjust #include paths in headers so they are relative to the new layout.

### Step I.2: Copy maths utilities
Copy 3 files from lib/qm-dsp/maths/ -> extracted_qm_bpm/lib/qm-dsp/maths/:
- MathUtilities.h, MathUtilities.cpp
- nan-inf.h

### Step I.3: Copy base/window
Copy 1 file from lib/qm-dsp/base/ -> extracted_qm_bpm/lib/qm-dsp/base/:
- Window.h

### Step I.4: Copy phase vocoder
Copy 2 files from lib/qm-dsp/dsp/phasevocoder/ -> extracted_qm_bpm/lib/qm-dsp/phasevocoder/:
- PhaseVocoder.h, PhaseVocoder.cpp

### Step I.5: Copy onset detection function
Copy 2 files from lib/qm-dsp/dsp/onsets/ -> extracted_qm_bpm/lib/qm-dsp/onsets/:
- DetectionFunction.h, DetectionFunction.cpp

### Step I.6: Copy tempo tracker
Copy 2 files from lib/qm-dsp/dsp/tempotracking/ -> extracted_qm_bpm/lib/qm-dsp/tempotracking/:
- TempoTrackV2.h, TempoTrackV2.cpp

Each file needs #include path adjustments. E.g., if qm-dsp includes "dsp/transforms/FFT.h"
and in the new tree FFT.h is at lib/qm-dsp/phasevocoder/PhaseVocoder.cpp, the include
should be "../phasevocoder/PhaseVocoder.h" etc.

---

## Phase II — Write the library (20 minutes)

### Step II.1: Write include/qm_bpm.h
Public API header defining:
- DfConfig (detection function config with all parameters from mixxx defaults)
- QmBeat (frame position + time + frequency)
- QmQueenMaryBPM class with methods:
  - detect(left, right, frames) — process audio and compute beats
  - getEstimatedBPM() — weighted avg BPM from period contour
  - getBeats() — list of beat events
  - getDetectionFunction() — raw onset values

### Step II.2: Write src/qm_bpm.cpp
Library glue that:
1. Downmixes stereo → mono (frame by frame)
2. Applies Hann window
3. Frames with overlap (step = hopSize)
4. Calls QmOnsetDetector::processFrame() per window
5. Collects detection function values
6. Applies Butterworth smoothing (filterDF)
7. Calls QmTempoTracker::calculateBeatPeriod() → period contour
8. Calls QmTempoTracker::calculateBeats() → beat frame positions
9. Converts frame positions to time/BPM using sample rate

### Step III: Write src/qmfread.cpp (standalone WAV reader)
Minimal WAV file reader:
- Parse RIFF header (check "WAVE" marker)
- Find "fmt " chunk → sample rate, channels, bits-per-sample
- Find "data" chunk → raw PCM samples
- Convert any PCM depth (8/16/24/32-bit) to float32
- Return mono float stream (downmix stereo)
- Zero external dependencies

---

## Phase III — Build and Test

### Step IV.1: Write CMakeLists.txt (library)
- qm_bpm as static library compiling src/*.cpp + qm-dsp + kissfft
- Public include dir = include/
- Set C++17 requirement

### Step IV.2: Write CMakeLists.txt (test executable)
- test_bpm executable from test_bpm.cpp
- Link against qm_bpm library

### Step IV.3: Build with GCC
g++ -I lib/qm-dsp -I lib/kissfft ... src/*.cpp lib/*.cpp lib/*.c -o test_bpm

### Step IV.4: Run test
./test_bpm /home/ian/Downloads/metronome-ticks_4-4_120-BPM.wav
Expected: 120 BPM

### Step IV.5: Iterate
If result is not ~120 BPM:
- Adjust hop size (512 is mixxx default for 44100 Hz)
- Adjust window size (power of 2)
- Check period-to-BPM conversion formula
- Verify detection function values are reasonable
- Adjust RCF period range or Viterbi parameters

---

## Default Parameters (from mixxx AnalyzerQueenMaryBeats)

| Parameter | Value | Source |
|-----------|-------|--------|
| sampleRate | 44100 Hz | mixxx default |
| inputTempo | 120.0 | Mixxx default |
| stepSizeFrames | 512 | ~11.6 ms at 44100 Hz |
| windowSize | 2048 | nextPowerOfTwo(44100/50) |
| DFType | DF_COMPLEXSD | Queen Mary default |
| smoothing | Butterworth 2nd order (bidir.) | TempoTrackV2::filterDF |
| RCF period range | 20-128 df increments | TempoTrackV2.cpp: wv_len = 128 |
| Viterbi σ (gaussian width) | 8.0 | TempoTrackV2::viterbi_decode |
| DP α (prev beat weight) | 0.9 | TempoTrackV2::calculateBeats |
| DP tightness | 4.0 | TempoTrackV2::calculateBeats |
| Period to BPM | 60*sRate / (period+1) / hopSampleRate | See tempo calculation in TempoTrackV2 |

---

## Files Created (18 total)

| # | Path | Action |
|---|------|--------|
| 1-4 | lib/kissfft/kiss_fft.(h/c), kiss_fftr.(h/c) | Copy + fix includes |
| 5-7 | lib/qm-dsp/maths/{MathUtilities.h/cpp, nan-inf.h | Copy + fix includes |
| 8 | lib/qm-dsp/base/Window.h | Copy |
| 9-10 | lib/qm-dsp/phasevocoder/{PhaseVocoder.h/cpp} | Copy + fix includes |
| 11-12 | lib/qm-dsp/onsets/{DetectionFunction.h/cpp} | Copy + fix includes |
| 13-14 | lib/qm-dsp/tempotracking/{TempoTrackV2.h/cpp} | Copy + fix includes |
| 15 | include/qm_bpm.h | Write new |
| 16 | src/qm_bpm.cpp | Write new |
| 17 | src/qmfread.cpp | Write new |
| 18 | CMakeLists.txt | Write new |
| 19 | test_bpm.cpp | Write new |
| 20 | CMakeLists.txt | Write new |
| 21 | README.md | Write new |
| 22 | test_bpm.cpp | Write new |

## Risk & Mitigation

| Risk | Mitigation |
|------|------|
| qm-dsp headers reference Mixxx-specific types | qm-dsp is a clean C++ library; all deps are <vector>, <cmath>, etc. I verified all headers during exploration phase. No Qt or mixxx headers in any qm-dsp source files. |
| kiss_fft float/double mismatch | Use float throughout (kissfft uses float by default). Internal double→float conversion in wrapper if needed. |
| WAV reader is wrong | Use only standard library (stdio.h, stdlib.h, memcpy). No audio library required. |
| Result is not 120 BPM | I have detailed parameter values from the source code. Iteratively adjust and test. |

## Verification Criteria

- Program compiles with no external dependencies beyond std c++ and the vendored qm-dsp library.
- ./test_bpm <120BPM_wav_file> outputs exactly 120.0 BPM. The file contains metronome ticks programmatically created in a DAW.
- Beat positions are evenly spaced at 500 ms intervals (120 BPM = 2 beats per second).
