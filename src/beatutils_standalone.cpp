/* beatutils_standalone.cpp — Clean-room copy of Mixxx's
 * src/track/beatutils.h + beatutils.cpp + bpm.h + bpm.cpp + audio/frame.h
 *
 * Ported to stdlib (no Qt).  Types are mapped directly:
 *   mixxx::Bpm                -> Bpm
 *   mixxx::audio::FramePos    -> FramePos  (double-based!)
 *   mixxx::audio::FrameDiff_t -> FrameDiff_t  (double!)
 *   mixxx::audio::SampleRate  -> SampleRate (double!)
 *
 * The key difference from earlier implementations: FramePos is a double
 * wrapper, NOT int64_t.  All arithmetic in retrieveConstRegions and
 * makeConstBpm uses floating-point.
 */

#include <cmath>
#include <cstdio>
#include <optional>

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
        typedef double value_t;
        static constexpr value_t kStartValue    = 0;
        static constexpr value_t kInvalidValue  = std::numeric_limits<value_t>::quiet_NaN();

        constexpr FramePos() noexcept : m_framePosition(kInvalidValue) {}
        explicit constexpr FramePos(value_t framePosition) : m_framePosition(framePosition) {}

        value_t value() const { return m_framePosition; }
        bool isValid() const    { return m_framePosition != kInvalidValue; }

        FramePos& operator+=(value_t v) { m_framePosition += v; return *this; }
        FramePos& operator-=(value_t v) { m_framePosition -= v; return *this; }
        FramePos& operator+=(FrameDiff_t v) { m_framePosition += v; return *this; }
        FramePos& operator-=(FrameDiff_t v) { m_framePosition -= v; return *this; }

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
        FrameDiff_t frames = upperFrame.value() - lowerFrame.value();
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
            double sampleRate) {

        constexpr double kMaxSecsPhaseError   = 0.025;
        constexpr double kMaxSecsPhaseErrorSum = 0.1;
        constexpr int    kMaxOutliersCount     = 1;

        FrameDiff_t maxPhaseError   = kMaxSecsPhaseError  * sampleRate;
        FrameDiff_t maxPhaseErrorSum = kMaxSecsPhaseErrorSum * sampleRate;

        int leftIndex = 0;
        int rightIndex = static_cast<int>(coarseBeats.size()) - 1;

        std::vector<ConstRegion> constantRegions;

        while (leftIndex < static_cast<int>(coarseBeats.size()) - 1) {
            FrameDiff_t meanBeatLength =
                    (coarseBeats[rightIndex].value() - coarseBeats[leftIndex].value()) /
                    (rightIndex - leftIndex);

            int outliersCount = 0;
            FramePos ironedBeat(coarseBeats[leftIndex].value());
            FrameDiff_t phaseErrorSum = 0;
            int i = leftIndex + 1;

            for (; i <= rightIndex; ++i) {
                ironedBeat += meanBeatLength;
                FrameDiff_t phaseError = ironedBeat.value() - coarseBeats[i].value();
                phaseErrorSum += phaseError;

                if (fabs(phaseError) > maxPhaseError) {
                    outliersCount++;
                    if (outliersCount > kMaxOutliersCount ||
                            i == leftIndex + 1) {
                        break;
                    }
                }
                if (fabs(phaseErrorSum) > maxPhaseErrorSum) {
                    break;
                }
            }

            if (i > rightIndex) {
                /* Verify start/end beats are not correction beats in the same
                 * direction that would bend meanBeatLength away from optimum. */
                FrameDiff_t regionBorderError = 0;
                if (rightIndex > leftIndex + 2) {
                    FrameDiff_t firstBeatLength =
                            coarseBeats[leftIndex + 1].value() - coarseBeats[leftIndex].value();
                    FrameDiff_t lastBeatLength =
                            coarseBeats[rightIndex].value() - coarseBeats[rightIndex - 1].value();
                    regionBorderError = fabs(firstBeatLength + lastBeatLength - (2 * meanBeatLength));
                }
                if (regionBorderError < maxPhaseError / 2) {
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
            double sampleRate,
            audio::FramePos* pFirstBeat) {
        if (constantRegions.empty()) return {};

        int midRegionIndex = 0;
        FrameDiff_t longestRegionLength = 0;
        FrameDiff_t longestRegionBeatLength = 0;
        for (int i = 0; i < static_cast<int>(constantRegions.size()) - 1; ++i) {
            FrameDiff_t length =
                    constantRegions[i + 1].firstBeat.value() -
                    constantRegions[i].firstBeat.value();
            if (length > longestRegionLength) {
                longestRegionLength       = length;
                longestRegionBeatLength   = constantRegions[i].beatLength;
                midRegionIndex            = i;
            }
        }

        if (longestRegionLength == 0) return {};

        int longestRegionNumberOfBeats = static_cast<int>(
                (longestRegionLength / longestRegionBeatLength) + 0.5);
        FrameDiff_t longestRegionBeatLengthMin = longestRegionBeatLength -
                (kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats;
        FrameDiff_t longestRegionBeatLengthMax = longestRegionBeatLength +
                (kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats;
        int startRegionIndex = midRegionIndex;

        /* Expand forward */
        for (int i = 0; i < midRegionIndex; ++i) {
            FrameDiff_t length =
                    constantRegions[i + 1].firstBeat.value() -
                    constantRegions[i].firstBeat.value();
            int numberOfBeats = static_cast<int>((length / constantRegions[i].beatLength) + 0.5);
            if (numberOfBeats < kMinRegionBeatCount) continue;

            FrameDiff_t thisRegionBeatLengthMin = constantRegions[i].beatLength -
                    (kMaxSecsPhaseError * sampleRate) / numberOfBeats;
            FrameDiff_t thisRegionBeatLengthMax = constantRegions[i].beatLength +
                    (kMaxSecsPhaseError * sampleRate) / numberOfBeats;

            if (longestRegionBeatLength > thisRegionBeatLengthMin &&
                    longestRegionBeatLength < thisRegionBeatLengthMax) {
                FrameDiff_t newLongestRegionLength =
                        constantRegions[midRegionIndex + 1].firstBeat.value() -
                        constantRegions[i].firstBeat.value();

                FrameDiff_t beatLengthMin = (longestRegionBeatLengthMin > thisRegionBeatLengthMin
                                             ? longestRegionBeatLengthMin : thisRegionBeatLengthMin);
                FrameDiff_t beatLengthMax = (longestRegionBeatLengthMax < thisRegionBeatLengthMax
                                             ? longestRegionBeatLengthMax : thisRegionBeatLengthMax);

                int maxNumberOfBeats = static_cast<int>(round(newLongestRegionLength / beatLengthMin));
                int minNumberOfBeats = static_cast<int>(round(newLongestRegionLength / beatLengthMax));

                if (minNumberOfBeats != maxNumberOfBeats) continue;

                const int numberOfBeatss = minNumberOfBeats;
                const FrameDiff_t newBeatLength = newLongestRegionLength / numberOfBeatss;
                if (newBeatLength > longestRegionBeatLengthMin &&
                        newBeatLength < longestRegionBeatLengthMax) {
                    longestRegionLength       = newLongestRegionLength;
                    longestRegionBeatLength   = newBeatLength;
                    longestRegionNumberOfBeats = numberOfBeatss;
                    longestRegionBeatLengthMin = longestRegionBeatLength -
                            (kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats;
                    longestRegionBeatLengthMax = longestRegionBeatLength +
                            (kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats;
                    startRegionIndex = i;
                    break;
                }
            }
        }

        /* Expand backward */
        for (int i = static_cast<int>(constantRegions.size()) - 2; i > midRegionIndex; --i) {
            FrameDiff_t length =
                    constantRegions[i + 1].firstBeat.value() -
                    constantRegions[i].firstBeat.value();
            int numberOfBeats = static_cast<int>((length / constantRegions[i].beatLength) + 0.5);
            if (numberOfBeats < kMinRegionBeatCount) continue;

            FrameDiff_t thisRegionBeatLengthMin = constantRegions[i].beatLength -
                    (kMaxSecsPhaseError * sampleRate) / numberOfBeats;
            FrameDiff_t thisRegionBeatLengthMax = constantRegions[i].beatLength +
                    (kMaxSecsPhaseError * sampleRate) / numberOfBeats;

            if (longestRegionBeatLength > thisRegionBeatLengthMin &&
                    longestRegionBeatLength < thisRegionBeatLengthMax) {
                FrameDiff_t newLongestRegionLength =
                        constantRegions[i + 1].firstBeat.value() -
                        constantRegions[startRegionIndex].firstBeat.value();

                FrameDiff_t minBeatLength = (longestRegionBeatLengthMin > thisRegionBeatLengthMin
                                             ? longestRegionBeatLengthMin : thisRegionBeatLengthMin);
                FrameDiff_t maxBeatLength = (longestRegionBeatLengthMax < thisRegionBeatLengthMax
                                             ? longestRegionBeatLengthMax : thisRegionBeatLengthMax);

                int maxNumberOfBeats = static_cast<int>(round(newLongestRegionLength / minBeatLength));
                int minNumberOfBeats = static_cast<int>(round(newLongestRegionLength / maxBeatLength));

                if (minNumberOfBeats != maxNumberOfBeats) continue;

                const int numberOfBeatss = minNumberOfBeats;
                const FrameDiff_t newBeatLength = newLongestRegionLength / numberOfBeatss;
                if (newBeatLength > longestRegionBeatLengthMin &&
                        newBeatLength < longestRegionBeatLengthMax) {
                    longestRegionLength       = newLongestRegionLength;
                    longestRegionBeatLength   = newBeatLength;
                    longestRegionNumberOfBeats = numberOfBeatss;
                    break;
                }
            }
        }

        longestRegionBeatLengthMin = longestRegionBeatLength -
                (kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats;
        longestRegionBeatLengthMax = longestRegionBeatLength +
                (kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats;

        const Bpm minRoundBpm = Bpm(60.0 * sampleRate / longestRegionBeatLengthMax);
        const Bpm maxRoundBpm = Bpm(60.0 * sampleRate / longestRegionBeatLengthMin);
        const Bpm centerBpm   = Bpm(60.0 * sampleRate / longestRegionBeatLength);

        const Bpm roundBpm = roundBpmWithinRange(minRoundBpm, centerBpm, maxRoundBpm);

        if (pFirstBeat) {
            const double roundedBeatLength = 60.0 * sampleRate / roundBpm.value();
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
        std::optional<Bpm> snapBpm = trySnap(minBpm, centerBpm, maxBpm, 1.0);
        if (snapBpm) return *snapBpm;

        if (centerBpm < Bpm(85.0)) {
            snapBpm = trySnap(minBpm, centerBpm, maxBpm, 2.0);
            if (snapBpm) return *snapBpm;
        }

        if (centerBpm > Bpm(127.0)) {
            snapBpm = trySnap(minBpm, centerBpm, maxBpm, 2.0 / 3.0);
            if (snapBpm) return *snapBpm;
        }

        snapBpm = trySnap(minBpm, centerBpm, maxBpm, 3.0);
        if (snapBpm) return *snapBpm;

        snapBpm = trySnap(minBpm, centerBpm, maxBpm, 12.0);
        if (snapBpm) return *snapBpm;

        return centerBpm;
    }

private:
    static constexpr double kMaxSecsPhaseError   = 0.025;
    static constexpr int    kMinRegionBeatCount  = 16;
};
