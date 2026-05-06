/* beatutils_standalone.cpp — Clean-room copy of Mixxx's
 * src/track/beatutils.h + beatutils.cpp + bpm.h + bpm.cpp + audio/frame.h
 *
 * Ported to stdlib (no Qt).  All types mapped from Mixxx:
 *   mixxx::Bpm                -> Bpm
 *   mixxx::audio::FramePos    -> FramePos  (double-based!)
 *   mixxx::audio::FrameDiff_t -> FrameDiff_t  (double!)
 *   mixxx::audio::SampleRate  -> SampleRate (double!)
 *
 * The key difference from earlier implementations: FramePos is a double
 * wrapper, NOT int64_t.  All arithmetic in retrieveConstRegions and
 * makeConstBpm uses floating-point throughout.
 */

#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

/* =================== Bpm (from bpm.h/cpp) =================== */

class Bpm {
public:
    static constexpr double kValueUndefined = 0.0;
    static constexpr double kValueMin       = 0.0;   /* exclusive */
    static constexpr double kValueMax       = 500.0; /* inclusive */

    Bpm() noexcept : m_value(kValueUndefined) {}
    explicit Bpm(double value) : m_value(value) {}

    static bool isValidValue(double value) {
        return std::isfinite(value) && value > kValueMin;
    }

    bool isValid() const { return isValidValue(m_value); }
    double value() const { return m_value; }

    void setValue(double v) { m_value = v; }

    Bpm& operator+=(double d)  { m_value += d; return *this; }
    Bpm& operator-=(double d)  { m_value -= d; return *this; }
    Bpm& operator*=(double d)  { m_value *= d; return *this; }
    Bpm& operator/=(double d)  { m_value /= d; return *this; }

    double operator-(const Bpm& other) const { return m_value - other.m_value; }

    friend Bpm operator+(Bpm b, double d) { return Bpm(b.m_value + d); }
    friend Bpm operator-(Bpm b, double d) { return Bpm(b.m_value - d); }
    friend Bpm operator*(Bpm b, double d) { return Bpm(b.m_value * d); }
    friend Bpm operator/(Bpm b, double d) { return Bpm(b.m_value / d); }
    friend double operator/(Bpm b1, Bpm b2) { return b1.m_value / b2.m_value; }

    friend bool operator==(Bpm a, Bpm b) {
        if (!a.isValid() && !b.isValid()) return true;
        return a.isValid() && b.isValid() && a.m_value == b.m_value;
    }
    friend bool operator!=(Bpm a, Bpm b) { return !(a == b); }
    friend bool operator< (Bpm a, Bpm b) { return a.m_value < b.m_value; }
    friend bool operator<=(Bpm a, Bpm b) { return a == b || a < b; }
    friend bool operator> (Bpm a, Bpm b) { return b < a; }
    friend bool operator>=(Bpm a, Bpm b) { return a == b || a > b; }

private:
    double m_value;
};

/* =================== FramePos (from audio/frame.h) =================== */

namespace audio {
    using FrameDiff_t = double;
    using SampleRate  = double;

    class FramePos final {
    public:
        using value_t = double;
        static constexpr value_t kStartValue    = 0;
        static constexpr value_t kInvalidValue  = std::numeric_limits<value_t>::quiet_NaN();

        constexpr FramePos() noexcept : m_framePosition(kInvalidValue) {}
        explicit constexpr FramePos(value_t framePosition) : m_framePosition(framePosition) {}

        value_t value() const { return m_framePosition; }
        bool isValid() const    { return m_framePosition != kInvalidValue; }

        FramePos& operator+=(value_t v)  { m_framePosition += v; return *this; }
        FramePos& operator-=(value_t v)  { m_framePosition -= v; return *this; }

        friend bool operator==(FramePos a, FramePos b) {
            if (!a.isValid() || !b.isValid()) return false;
            return a.m_framePosition == b.m_framePosition;
        }

    private:
        value_t m_framePosition;
    };
}

/* =================== BeatUtils (from beatutils.h/cpp) =================== */

class BeatUtils {
public:
    struct ConstRegion {
        audio::FramePos firstBeat;
        audio::FrameDiff_t beatLength;
    };

    static Bpm calculateAverageBpm(int numberOfBeats,
                                   audio::SampleRate sampleRate,
                                   audio::FramePos lowerFrame,
                                   audio::FramePos upperFrame) {
        audio::FrameDiff_t frames = upperFrame.value() - lowerFrame.value();
        if (numberOfBeats < 1) return {};
        return Bpm(60.0 * numberOfBeats * sampleRate / frames);
    }

    static Bpm calculateBpm(
            const std::vector<audio::FramePos>& beats,
            audio::SampleRate sampleRate) {
        if (beats.size() < 2) return {};

        constexpr int kMinRegionBeatCount = 16;
        if (beats.size() < kMinRegionBeatCount) {
            return calculateAverageBpm(static_cast<int>(beats.size()) - 1,
                                       sampleRate, beats.front(), beats.back());
        }

        std::vector<ConstRegion> constantRegions =
                retrieveConstRegions(beats, sampleRate);
        return makeConstBpm(constantRegions, sampleRate, nullptr);
    }

    static std::vector<ConstRegion> retrieveConstRegions(
            const std::vector<audio::FramePos>& coarseBeats,
            double sr) {
        if (coarseBeats.size() < 2) return {};

        constexpr double kMaxSecsPhaseError   = 0.025;
        constexpr double kMaxSecsPhaseErrSum = 0.1;
        constexpr int    kMaxOutliersCount     = 1;

        audio::FrameDiff_t maxPhaseError   = kMaxSecsPhaseError   * sr;
        audio::FrameDiff_t maxPhaseErrSum  = kMaxSecsPhaseErrSum  * sr;

        int leftIndex = 0;
        int rightIndex = static_cast<int>(coarseBeats.size()) - 1;

        std::vector<ConstRegion> constantRegions;

        while (leftIndex < static_cast<int>(coarseBeats.size()) - 1) {
            audio::FrameDiff_t meanBeatLength =
                    (coarseBeats[rightIndex].value() - coarseBeats[leftIndex].value()) /
                    (rightIndex - leftIndex);
            int outliersCount = 0;
            audio::FramePos ironedBeat(coarseBeats[leftIndex].value());
            audio::FrameDiff_t phaseErrSum = 0;
            int i = leftIndex + 1;

            for (; i <= rightIndex; ++i) {
                ironedBeat += meanBeatLength;
                audio::FrameDiff_t phaseErr = ironedBeat.value() - coarseBeats[i].value();
                phaseErrSum += phaseErr;

                if (fabs(phaseErr) > maxPhaseError) {
                    outliersCount++;
                    if (outliersCount > kMaxOutliersCount ||
                            i == leftIndex + 1) {
                        break;
                    }
                }
                if (fabs(phaseErrSum) > maxPhaseErrSum) {
                    break;
                }
            }

            if (i > rightIndex) {
                /* Verify that the first and last beat are not correction beats
                 * in the same direction that would bend meanBeatLength away
                 * from the optimum. */
                audio::FrameDiff_t borderErr = 0;
                if (rightIndex > leftIndex + 2) {
                    audio::FrameDiff_t firstBL =
                            coarseBeats[leftIndex + 1].value() - coarseBeats[leftIndex].value();
                    audio::FrameDiff_t lastBL =
                            coarseBeats[rightIndex].value() - coarseBeats[rightIndex - 1].value();
                    borderErr = fabs(firstBL + lastBL - (2 * meanBeatLength));
                }
                if (borderErr < maxPhaseError / 2) {
                    constantRegions.push_back({coarseBeats[leftIndex], meanBeatLength});
                    leftIndex = rightIndex;
                    rightIndex = static_cast<int>(coarseBeats.size()) - 1;
                    continue;
                }
            }
            rightIndex--;
        }

        /* Add a final region with zero length to mark the end. */
        constantRegions.push_back({coarseBeats.back(), 0});
        return constantRegions;
    }

    static Bpm makeConstBpm(
            const std::vector<ConstRegion>& constantRegions,
            double sr,
            audio::FramePos* pFirstBeat) {
        if (constantRegions.empty()) return {};

        int midRegionIndex = 0;
        audio::FrameDiff_t longestRegionLength = 0;
        audio::FrameDiff_t longestRegionBeatLength = 0;
        for (int i = 0; i < static_cast<int>(constantRegions.size()) - 1; ++i) {
            audio::FrameDiff_t length =
                    constantRegions[i + 1].firstBeat.value() -
                    constantRegions[i].firstBeat.value();
            if (length > longestRegionLength) {
                longestRegionLength          = length;
                longestRegionBeatLength      = constantRegions[i].beatLength;
                midRegionIndex               = i;
            }
        }

        if (longestRegionLength == 0) return {};

        int longestRegionNumberOfBeats = static_cast<int>(
                (longestRegionLength / longestRegionBeatLength) + 0.5);
        audio::FrameDiff_t longestRegionBeatLengthMin = longestRegionBeatLength -
                (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;
        audio::FrameDiff_t longestRegionBeatLengthMax = longestRegionBeatLength +
                (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;

        int startRegionIndex = midRegionIndex;

        /* Expand forward */
        for (int i = 0; i < midRegionIndex; ++i) {
            audio::FrameDiff_t length =
                    constantRegions[i + 1].firstBeat.value() -
                    constantRegions[i].firstBeat.value();
            int numberOfBeats = static_cast<int>((length / constantRegions[i].beatLength) + 0.5);
            if (numberOfBeats < kMinRegionBeatCount) continue;

            audio::FrameDiff_t thisRegionBLMin = constantRegions[i].beatLength -
                    (kMaxSecsPhaseError * sr) / numberOfBeats;
            audio::FrameDiff_t thisRegionBLMax = constantRegions[i].beatLength +
                    (kMaxSecsPhaseError * sr) / numberOfBeats;

            if (longestRegionBeatLength > thisRegionBLMin &&
                    longestRegionBeatLength < thisRegionBLMax) {
                audio::FrameDiff_t newLongestRegionLength =
                        constantRegions[midRegionIndex + 1].firstBeat.value() -
                        constantRegions[i].firstBeat.value();

                audio::FrameDiff_t blMin = (longestRegionBeatLengthMin > thisRegionBLMin
                                            ? longestRegionBeatLengthMin : thisRegionBLMin);
                audio::FrameDiff_t blMax = (longestRegionBeatLengthMax < thisRegionBLMax
                                            ? longestRegionBeatLengthMax : thisRegionBLMax);

                int maxNBeats = static_cast<int>(round(newLongestRegionLength / blMin));
                int minNBeats = static_cast<int>(round(newLongestRegionLength / blMax));

                if (minNBeats != maxNBeats) continue;

                const int nB = minNBeats;
                const audio::FrameDiff_t newBL = newLongestRegionLength / nB;
                if (newBL > longestRegionBeatLengthMin &&
                        newBL < longestRegionBeatLengthMax) {
                    longestRegionLength       = newLongestRegionLength;
                    longestRegionBeatLength   = newBL;
                    longestRegionNumberOfBeats = nB;
                    longestRegionBeatLengthMin = longestRegionBeatLength -
                            (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;
                    longestRegionBeatLengthMax = longestRegionBeatLength +
                            (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;
                    startRegionIndex = i;
                    break;
                }
            }
        }

        /* Recompute bounds after forward expansion (match Mixxx line 235-237) */
        longestRegionBeatLengthMin = longestRegionBeatLength -
                (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;
        longestRegionBeatLengthMax = longestRegionBeatLength +
                (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;

        /* Expand backward */

        for (int i = static_cast<int>(constantRegions.size()) - 2; i > midRegionIndex; --i) {
            audio::FrameDiff_t length =
                    constantRegions[i + 1].firstBeat.value() -
                    constantRegions[i].firstBeat.value();
            int numberOfBeats = static_cast<int>((length / constantRegions[i].beatLength) + 0.5);
            if (numberOfBeats < kMinRegionBeatCount) continue;

            audio::FrameDiff_t thisRegionBLMin = constantRegions[i].beatLength -
                    (kMaxSecsPhaseError * sr) / numberOfBeats;
            audio::FrameDiff_t thisRegionBLMax = constantRegions[i].beatLength +
                    (kMaxSecsPhaseError * sr) / numberOfBeats;

            if (longestRegionBeatLength > thisRegionBLMin &&
                    longestRegionBeatLength < thisRegionBLMax) {
                audio::FrameDiff_t newLongestRegionLength =
                        constantRegions[i + 1].firstBeat.value() -
                        constantRegions[startRegionIndex].firstBeat.value();

                audio::FrameDiff_t minBL = (longestRegionBeatLengthMin > thisRegionBLMin
                                            ? longestRegionBeatLengthMin : thisRegionBLMin);
                audio::FrameDiff_t maxBL = (longestRegionBeatLengthMax < thisRegionBLMax
                                            ? longestRegionBeatLengthMax : thisRegionBLMax);

                int maxNBeats = static_cast<int>(round(newLongestRegionLength / minBL));
                int minNBeats = static_cast<int>(round(newLongestRegionLength / maxBL));

                if (minNBeats != maxNBeats) continue;

                const int nB = minNBeats;
                const audio::FrameDiff_t newBL = newLongestRegionLength / nB;
                if (newBL > longestRegionBeatLengthMin &&
                        newBL < longestRegionBeatLengthMax) {
                    longestRegionLength       = newLongestRegionLength;
                    longestRegionBeatLength   = newBL;
                    longestRegionNumberOfBeats = nB;
                    break;
                }
            }
        }

        longestRegionBeatLengthMin = longestRegionBeatLength -
                (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;
        longestRegionBeatLengthMax = longestRegionBeatLength +
                (kMaxSecsPhaseError * sr) / longestRegionNumberOfBeats;

        /* Debug output for comparison */
        fprintf(stderr, "[MAKECONST_BPM_DEBUG]\n");
        fprintf(stderr, "  midRegionIndex=%d startRegionIndex=%d\n", midRegionIndex, startRegionIndex);
        fprintf(stderr, "  longestRegionLength=%f longestRegionBeatLength=%f longestRegionNumberOfBeats=%d\n",
                longestRegionLength, longestRegionBeatLength, longestRegionNumberOfBeats);
        fprintf(stderr, "  longestRegionBeatLengthMin=%f longestRegionBeatLengthMax=%f\n",
                longestRegionBeatLengthMin, longestRegionBeatLengthMax);
        fprintf(stderr, "  minRoundBpm=%f maxRoundBpm=%f centerBpm=%f\n",
                60.0 * sr / longestRegionBeatLengthMax,
                60.0 * sr / longestRegionBeatLengthMin,
                60.0 * sr / longestRegionBeatLength);
        fprintf(stderr, "  startRegionIndex=%d\n", startRegionIndex);

        const Bpm minRoundBpm = Bpm(60.0 * sr / longestRegionBeatLengthMax);
        const Bpm maxRoundBpm = Bpm(60.0 * sr / longestRegionBeatLengthMin);
        const Bpm centerBpm   = Bpm(60.0 * sr / longestRegionBeatLength);

        const Bpm roundBpm = roundBpmWithinRange(minRoundBpm, centerBpm, maxRoundBpm);

        if (pFirstBeat) {
            const double roundedBeatLength = 60.0 * sr / roundBpm.value();
            *pFirstBeat = audio::FramePos(
                    fmod(constantRegions[startRegionIndex].firstBeat.value(), roundedBeatLength));
        }
        return roundBpm;
    }

    static std::optional<Bpm> trySnap(Bpm minBpm,
                                      Bpm centerBpm,
                                      Bpm maxBpm,
                                      double fraction) {
        Bpm snapBpm(round(centerBpm.value() * fraction) / fraction);
        if (snapBpm > minBpm && snapBpm < maxBpm) return snapBpm;
        return std::nullopt;
    }

    static Bpm roundBpmWithinRange(Bpm minBpm,
                                   Bpm centerBpm,
                                   Bpm maxBpm) {
        /* First try to snap to a full integer BPM */
        auto snapBpm = trySnap(minBpm, centerBpm, maxBpm, 1.0);
        if (snapBpm) return *snapBpm;

        /* 0.5 BPM are only reasonable if the double value is not insane */
        if (centerBpm < Bpm(85.0)) {
            snapBpm = trySnap(minBpm, centerBpm, maxBpm, 2.0);
            if (snapBpm) return *snapBpm;
        }

        if (centerBpm > Bpm(127.0)) {
            snapBpm = trySnap(minBpm, centerBpm, maxBpm, 2.0 / 3.0);
            if (snapBpm) return *snapBpm;
        }

        /* try to snap to a 1/3 BPM */
        snapBpm = trySnap(minBpm, centerBpm, maxBpm, 3.0);
        if (snapBpm) return *snapBpm;

        /* try to snap to a 1/12 BPM */
        snapBpm = trySnap(minBpm, centerBpm, maxBpm, 12.0);
        if (snapBpm) return *snapBpm;

        /* else give up and use the original BPM value. */
        return centerBpm;
    }

private:
    static constexpr double kMaxSecsPhaseError   = 0.025;
    static constexpr int    kMinRegionBeatCount  = 16;

    static std::vector<double> retrieveConstRegions(
            const std::vector<double>& coarseBeats,
            double sr) {
        /* Stub — real code goes in .cpp below */
        return {};
    }
};
