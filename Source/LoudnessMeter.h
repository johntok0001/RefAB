#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// ITU-R BS.1770-4 / EBU R128 loudness meter (stereo).
// process() is called from one thread (audio thread or an offline job);
// the getters are safe to call from any thread.
class LoudnessMeter
{
public:
    static constexpr float silence = -100.0f;

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0 ? sampleRate : 44100.0;
        setupFilters();
        subLen = juce::jmax (1, (int) std::round (sr * 0.1)); // 100 ms sub-blocks
        resetAll();
    }

    void resetAll()
    {
        for (auto& f : pre) f.clear();
        for (auto& f : rlb) f.clear();
        subPos = 0;
        subEnergy = subSq = 0.0;
        ring.fill (0.0);
        sqRing.fill (0.0);
        ringPos = ringFilled = 0;
        doResetIntegrated();
        momentary.store (silence);
        shortTerm.store (silence);
        rms.store (silence);
        peakSinceRead.store (0.0f);
    }

    // Thread-safe: the actual reset happens on the processing thread.
    void resetIntegrated() noexcept { resetRequest.store (true); }

    void process (const float* left, const float* right, int n) noexcept
    {
        if (resetRequest.exchange (false))
            doResetIntegrated();

        float pk = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float l = left[i];
            const float r = right[i];
            pk = juce::jmax (pk, std::abs (l), std::abs (r));

            const double yl = rlb[0].process (pre[0].process (l));
            const double yr = rlb[1].process (pre[1].process (r));
            subEnergy += yl * yl + yr * yr;
            subSq += 0.5 * ((double) l * l + (double) r * r);

            if (++subPos >= subLen)
                finishSubBlock();
        }

        if (pk > maxPeakLinear)
        {
            maxPeakLinear = pk;
            maxPeak.store (gainToDb (pk));
        }

        auto prev = peakSinceRead.load();
        while (pk > prev && ! peakSinceRead.compare_exchange_weak (prev, pk)) {}
    }

    float getMomentary() const noexcept   { return momentary.load(); }
    float getShortTerm() const noexcept   { return shortTerm.load(); }
    float getIntegrated() const noexcept  { return integrated.load(); }
    float getRms() const noexcept         { return rms.load(); }
    float getMaxPeak() const noexcept     { return maxPeak.load(); }
    float takePeakDb() noexcept           { return gainToDb (peakSinceRead.exchange (0.0f)); }

    static float energyToLufs (double e) noexcept
    {
        if (e <= 1.0e-12) return silence;
        return (float) juce::jmax ((double) silence, -0.691 + 10.0 * std::log10 (e));
    }

    static float gainToDb (float g) noexcept
    {
        return g > 1.0e-5f ? 20.0f * std::log10 (g) : silence;
    }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void clear() noexcept { z1 = z2 = 0; }
    };

    void setupFilters()
    {
        // K-weighting coefficients for any sample rate (same derivation as libebur128)
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double Vh = std::pow (10.0, G / 20.0);
            const double Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;

            for (auto& f : pre)
            {
                f.b0 = (Vh + Vb * K / Q + K * K) / a0;
                f.b1 = 2.0 * (K * K - Vh) / a0;
                f.b2 = (Vh - Vb * K / Q + K * K) / a0;
                f.a1 = 2.0 * (K * K - 1.0) / a0;
                f.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;

            for (auto& f : rlb)
            {
                f.b0 = 1.0;
                f.b1 = -2.0;
                f.b2 = 1.0;
                f.a1 = 2.0 * (K * K - 1.0) / a0;
                f.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
    }

    double meanOfLast (const std::array<double, 30>& r, int count) const noexcept
    {
        count = juce::jmin (count, ringFilled);
        if (count <= 0) return 0.0;
        double s = 0;
        for (int j = 0; j < count; ++j)
            s += r[(size_t) ((ringPos - 1 - j + 30) % 30)];
        return s / count;
    }

    void finishSubBlock() noexcept
    {
        ring[(size_t) ringPos] = subEnergy / subLen;
        sqRing[(size_t) ringPos] = subSq / subLen;
        ringPos = (ringPos + 1) % 30;
        ringFilled = juce::jmin (30, ringFilled + 1);
        subPos = 0;
        subEnergy = subSq = 0.0;

        const double eM = meanOfLast (ring, 4);
        momentary.store (energyToLufs (eM));
        shortTerm.store (energyToLufs (meanOfLast (ring, 30)));

        const double sq = meanOfLast (sqRing, 3);
        rms.store (sq > 1.0e-12 ? (float) juce::jmax ((double) silence, 10.0 * std::log10 (sq)) : silence);

        if (ringFilled >= 4)
        {
            const float lm = energyToLufs (eM);
            if (lm >= -70.0f)
            {
                const int bin = juce::jlimit (0, numBins - 1, (int) ((lm + 70.0f) * 10.0f));
                histEnergy[(size_t) bin] += eM;
                histCount[(size_t) bin] += 1.0;
                integrated.store (computeIntegrated());
            }
        }
    }

    float computeIntegrated() const noexcept
    {
        double e = 0, c = 0;
        for (int b = 0; b < numBins; ++b) { e += histEnergy[(size_t) b]; c += histCount[(size_t) b]; }
        if (c <= 0) return silence;

        const float relGate = energyToLufs (e / c) - 10.0f;
        const int start = juce::jlimit (0, numBins - 1, (int) std::floor ((relGate + 70.0f) * 10.0f + 0.5f));

        e = c = 0;
        for (int b = start; b < numBins; ++b) { e += histEnergy[(size_t) b]; c += histCount[(size_t) b]; }
        return c > 0 ? energyToLufs (e / c) : silence;
    }

    void doResetIntegrated() noexcept
    {
        std::fill (histEnergy.begin(), histEnergy.end(), 0.0);
        std::fill (histCount.begin(), histCount.end(), 0.0);
        integrated.store (silence);
        maxPeakLinear = 0.0f;
        maxPeak.store (silence);
    }

    static constexpr int numBins = 750; // -70 .. +5 LUFS in 0.1 LU steps

    double sr = 44100.0;
    std::array<Biquad, 2> pre, rlb;
    int subLen = 4410, subPos = 0;
    double subEnergy = 0, subSq = 0;
    std::array<double, 30> ring {}, sqRing {};
    int ringPos = 0, ringFilled = 0;
    std::vector<double> histEnergy = std::vector<double> (numBins, 0.0);
    std::vector<double> histCount = std::vector<double> (numBins, 0.0);
    float maxPeakLinear = 0.0f;

    std::atomic<bool> resetRequest { false };
    std::atomic<float> momentary { silence }, shortTerm { silence }, integrated { silence },
                       rms { silence }, maxPeak { silence }, peakSinceRead { 0.0f };
};
