#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "LoudnessMeter.h"
#include <memory>

// A decoded reference track, resampled to the session's sample rate.
// Immutable once loaded: shared between threads via std::shared_ptr.
struct RefTrack
{
    juce::File file;
    juce::String name;
    juce::AudioBuffer<float> audio;       // always 2 channels
    double sampleRate = 44100.0;          // playback (session) rate
    double sourceSampleRate = 44100.0;
    float lufs = LoudnessMeter::silence;  // integrated loudness of the whole file
    float maxPeak = LoudnessMeter::silence;
    std::vector<float> wavPeak, wavRms;   // overview for the waveform display

    double getLengthSeconds() const noexcept { return audio.getNumSamples() / sampleRate; }

    // Integrated loudness of a region [startSec, endSec)
    float measureRegion (double startSec, double endSec) const
    {
        const int s = juce::jlimit (0, audio.getNumSamples(), (int) (startSec * sampleRate));
        const int e = juce::jlimit (s, audio.getNumSamples(), (int) (endSec * sampleRate));
        if (e - s < (int) (0.4 * sampleRate)) return LoudnessMeter::silence;

        LoudnessMeter m;
        m.prepare (sampleRate);
        m.process (audio.getReadPointer (0) + s, audio.getReadPointer (1) + s, e - s);
        return m.getIntegrated();
    }
};

static constexpr int refWaveformBins = 2048;

inline std::shared_ptr<RefTrack> loadRefTrack (const juce::File& file, double targetRate, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
    {
        error = file.existsAsFile() ? "Unsupported format" : "File not found";
        return {};
    }

    const auto len = reader->lengthInSamples;
    if (len <= 0 || reader->sampleRate <= 0)
    {
        error = "Empty file";
        return {};
    }
    if (len > (juce::int64) (reader->sampleRate * 60.0 * 20.0))
    {
        error = "Longer than 20 min";
        return {};
    }

    const int srcLen = (int) len;
    constexpr int pad = 512;
    juce::AudioBuffer<float> src (2, srcLen + pad);
    src.clear();

    if (reader->numChannels == 1)
    {
        reader->read (&src, 0, srcLen, 0, true, false);
        src.copyFrom (1, 0, src, 0, 0, srcLen);
    }
    else
    {
        reader->read (&src, 0, srcLen, 0, true, true);
    }

    auto track = std::make_shared<RefTrack>();
    track->file = file;
    track->name = file.getFileNameWithoutExtension();
    track->sampleRate = targetRate;
    track->sourceSampleRate = reader->sampleRate;

    const double ratio = reader->sampleRate / targetRate;

    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        track->audio.setSize (2, srcLen);
        for (int ch = 0; ch < 2; ++ch)
            track->audio.copyFrom (ch, 0, src, ch, 0, srcLen);
    }
    else
    {
        const int outLen = (int) std::floor (srcLen / ratio);
        // compensate the interpolator's delay so sync stays sample-accurate
        const int latencyOut = (int) std::round (juce::WindowedSincInterpolator::getBaseLatency() / ratio);
        juce::AudioBuffer<float> tmp (1, outLen + latencyOut);
        track->audio.setSize (2, outLen);

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::WindowedSincInterpolator interp;
            interp.process (ratio, src.getReadPointer (ch), tmp.getWritePointer (0), outLen + latencyOut);
            track->audio.copyFrom (ch, 0, tmp, 0, latencyOut, outLen);
        }
    }

    const int n = track->audio.getNumSamples();
    const float* L = track->audio.getReadPointer (0);
    const float* R = track->audio.getReadPointer (1);

    {
        LoudnessMeter m;
        m.prepare (targetRate);
        m.process (L, R, n);
        track->lufs = m.getIntegrated();
        track->maxPeak = m.getMaxPeak();
    }

    track->wavPeak.assign (refWaveformBins, 0.0f);
    track->wavRms.assign (refWaveformBins, 0.0f);

    for (int b = 0; b < refWaveformBins; ++b)
    {
        const int s = (int) ((juce::int64) n * b / refWaveformBins);
        const int e = juce::jmax (s + 1, (int) ((juce::int64) n * (b + 1) / refWaveformBins));
        float pk = 0.0f;
        double sq = 0.0;
        for (int i = s; i < juce::jmin (e, n); ++i)
        {
            pk = juce::jmax (pk, std::abs (L[i]), std::abs (R[i]));
            sq += 0.5 * ((double) L[i] * L[i] + (double) R[i] * R[i]);
        }
        track->wavPeak[(size_t) b] = pk;
        track->wavRms[(size_t) b] = (float) std::sqrt (sq / juce::jmax (1, e - s));
    }

    return track;
}

// Lock-free ring of the most recent stereo samples (single writer, readers may see a torn
// block occasionally, which is fine for display).
struct ScopeRing
{
    static constexpr int size = 1 << 15;

    ScopeRing() : left ((size_t) size, 0.0f), right ((size_t) size, 0.0f) {}

    void push (const float* l, const float* r, int n) noexcept
    {
        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            left[(size_t) w] = l[i];
            right[(size_t) w] = r[i];
            w = (w + 1) & (size - 1);
        }
        writePos.store (w, std::memory_order_release);
    }

    void readLatest (float* l, float* r, int n) const noexcept
    {
        int pos = (writePos.load (std::memory_order_acquire) - n) & (size - 1);
        for (int i = 0; i < n; ++i)
        {
            l[i] = left[(size_t) pos];
            r[i] = right[(size_t) pos];
            pos = (pos + 1) & (size - 1);
        }
    }

    void clear() noexcept
    {
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);
    }

    std::vector<float> left, right;
    std::atomic<int> writePos { 0 };
};
