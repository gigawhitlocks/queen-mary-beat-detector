cmake_minimum_required(VERSION 3.16)
project(qm_bpm LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# qm-dsp source files — all includes are relative to lib/ or lib/qm-dsp/
set(QM_DSPLIB_SRCS
  lib/qm-dsp/maths/MathUtilities.cpp
  lib/qm-dsp/dsp/transforms/FFT.cpp
  lib/qm-dsp/dsp/onsets/DetectionFunction.cpp
  lib/qm-dsp/dsp/phasevocoder/PhaseVocoder.cpp
  lib/qm-dsp/dsp/tempotracking/TempoTrackV2.cpp
  lib/kissfft/kiss_fft.c
  lib/kissfft/kiss_fftr.c
)

# Build static library
add_library(qm_bpm STATIC ${QM_DSPLIB_SRCS} src/qm_bpm.cpp)
target_include_directories(qm_bpm
  PUBLIC include
  PRIVATE . lib lib/qm-dsp lib/kissfft
)
target_compile_options(qm_bpm PRIVATE -O2)
set_target_properties(qm_bpm PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Build test executable
add_executable(test_bpm src/test_bpm.cpp)
target_link_libraries(test_bpm PRIVATE qm_bpm)
target_include_directories(test_bpm PRIVATE . lib lib/qm-dsp)
