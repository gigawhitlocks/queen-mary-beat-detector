# Makefile for standalone Queen Mary BPM algorithm
# Builds the qm_bpm static library and the analyze_audio wrapper binary

CXX     := g++
CXXSTD  := -std=c++17
CXXFLAGS := -O2 -Wall -Wextra $(CXXSTD)
LDFLAGS  := -lm

# Include paths: repo root, lib/, lib/qm-dsp/, lib/kissfft/
INC := -I. -Ilib -I lib/qm-dsp -I lib/kissfft

# qm-dsp sources (all includes are relative to repo root)
QM_SRCS := \
	lib/qm-dsp/maths/MathUtilities.cpp \
	lib/qm-dsp/dsp/transforms/FFT.cpp \
	lib/qm-dsp/dsp/onsets/DetectionFunction.cpp \
	lib/qm-dsp/dsp/phasevocoder/PhaseVocoder.cpp \
	lib/qm-dsp/dsp/tempotracking/TempoTrackV2.cpp \
	lib/kissfft/kiss_fft.c \
	lib/kissfft/kiss_fftr.c

# Object files
OBJS := $(QM_SRCS:.cpp=.o) $(QM_SRCS:.c=.o)

# Targets
LIB    := libqm_bpm.a
TARGET := analyze_audio

# ---- Phony rules ----
.PHONY: all clean

all: $(TARGET)

# ---- Library ----
$(LIB): $(OBJS)
	ar rcs $@ $^

# ---- Wrapper binary ----
$(TARGET): src/analyze_audio.cpp $(LIB)
	$(CXX) $(CXXFLAGS) $(INC) \
		src/analyze_audio.cpp \
		$(LIB) \
		-o $@ $(LDFLAGS)

# ---- Compile rules ----
lib/qm-dsp/%.o: lib/qm-dsp/%.cpp
	$(CXX) $(CXXFLAGS) $(INC) -fPIC -c $< -o $@

lib/kissfft/%.o: lib/kissfft/%.c
	$(CXX) $(CXXFLAGS) $(INC) -fPIC -c $< -o $@

# Build artifacts to remove.  Carefully listed to exclude .cpp/.c sources.
ARTIFACTS := \
	lib/qm-dsp/maths/MathUtilities.o \
	lib/qm-dsp/dsp/transforms/FFT.o \
	lib/qm-dsp/dsp/onsets/DetectionFunction.o \
	lib/qm-dsp/dsp/phasevocoder/PhaseVocoder.o \
	lib/qm-dsp/dsp/tempotracking/TempoTrackV2.o \
	lib/kissfft/kiss_fft.o \
	lib/kissfft/kiss_fftr.o \
	$(LIB) \
	analyze_audio \
	analyze_audio_debug

clean:
	rm -f $(ARTIFACTS)
