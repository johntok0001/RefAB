// Offline test harness for RefAB: runs the processor without a DAW.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <iostream>

static int failures = 0;

static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

struct MockPlayHead : juce::AudioPlayHead
{
    bool playing = true;
    double time = 0.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setTimeInSeconds (time);
        return p;
    }
};

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, (unsigned) b.getNumChannels(), 24, {}, 0));
    if (w != nullptr)
    {
        os.release();
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
}

static juce::AudioBuffer<float> makeSine (double sr, double secs, float amp, double hz)
{
    juce::AudioBuffer<float> b (2, (int) (sr * secs));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

static juce::AudioBuffer<float> makeNoise (double sr, double secs, float amp, int seed)
{
    juce::Random rnd (seed);
    juce::AudioBuffer<float> b (2, (int) (sr * secs));
    float s0 = 0, s1 = 0;
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        // gently low-passed noise, different per channel
        s0 = 0.7f * s0 + 0.3f * (rnd.nextFloat() * 2 - 1);
        s1 = 0.7f * s1 + 0.3f * (rnd.nextFloat() * 2 - 1);
        b.setSample (0, i, amp * s0);
        b.setSample (1, i, amp * s1);
    }
    return b;
}

static void pump (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

static bool waitReady (RefABProcessor& p, int slot)
{
    for (int i = 0; i < 200; ++i)
    {
        pump (20);
        if (p.getSlotInfo (slot).status == RefABProcessor::SlotInfo::Status::ready)
            return true;
        if (p.getSlotInfo (slot).status == RefABProcessor::SlotInfo::Status::error)
            return false;
    }
    return false;
}

static void setParam (RefABProcessor& p, const juce::String& id, float plainValue)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plainValue));
}

// Runs the processor over `input`, writes the output into `output`.
static void run (RefABProcessor& p, MockPlayHead& ph, const juce::AudioBuffer<float>& input,
                 juce::AudioBuffer<float>& output, int block = 512)
{
    output.makeCopyOf (input);
    juce::MidiBuffer midi;
    const double sr = p.getSampleRate();
    for (int pos = 0; pos < input.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, input.getNumSamples() - pos);
        juce::AudioBuffer<float> view (output.getArrayOfWritePointers(), 2, pos, n);
        p.processBlock (view, midi);
        if (ph.playing) ph.time += n / sr;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 48000.0;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("refab_test");
    dir.createDirectory();

    std::cout << "LoudnessMeter" << std::endl;
    {
        // EBU Tech 3341 case: stereo 1 kHz sine at -20 dBFS => -20.0 LUFS
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            auto b = makeSine (rate, 10.0, juce::Decibels::decibelsToGain (-20.0f), 1000.0);
            LoudnessMeter m;
            m.prepare (rate);
            m.process (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples());
            check (std::abs (m.getIntegrated() + 20.0f) < 0.1f,
                   "1 kHz -20 dBFS @" + juce::String (rate) + " -> " + juce::String (m.getIntegrated(), 2) + " LUFS");
            check (std::abs (m.getShortTerm() + 20.0f) < 0.1f, "  short-term " + juce::String (m.getShortTerm(), 2));
            check (std::abs (m.getMaxPeak() + 20.0f) < 0.05f, "  peak " + juce::String (m.getMaxPeak(), 2));
        }

        // Relative gate: 20 s at -20 dBFS + 20 s at -40 dBFS => roughly -23 LUFS (quiet part is below rel. gate? no, -40 is 20 LU below)
        auto loud = makeSine (sr, 20.0, juce::Decibels::decibelsToGain (-20.0f), 1000.0);
        auto quiet = makeSine (sr, 20.0, juce::Decibels::decibelsToGain (-40.0f), 1000.0);
        LoudnessMeter m;
        m.prepare (sr);
        m.process (loud.getReadPointer (0), loud.getReadPointer (1), loud.getNumSamples());
        m.process (quiet.getReadPointer (0), quiet.getReadPointer (1), quiet.getNumSamples());
        check (std::abs (m.getIntegrated() + 20.0f) < 0.2f, "relative gate ignores the -40 dB part -> " + juce::String (m.getIntegrated(), 2));
    }

    RefABProcessor p;
    MockPlayHead ph;
    p.setPlayHead (&ph);
    p.setRateAndBufferSizeDetails (sr, 512);
    p.prepareToPlay (sr, 512);
    pump (50);

    std::cout << "Loading" << std::endl;
    auto refFile44 = dir.getChildFile ("ref44.wav");
    writeWav (refFile44, makeSine (44100.0, 30.0, juce::Decibels::decibelsToGain (-14.0f), 1000.0), 44100.0);
    p.loadSlot (0, refFile44);
    check (waitReady (p, 0), "44.1 kHz WAV loads");
    {
        auto t = p.getTrack (0);
        check (t != nullptr && std::abs (t->getLengthSeconds() - 30.0) < 0.01, "resampled to 48 kHz, length " + juce::String (t ? t->getLengthSeconds() : 0, 3));
        check (t != nullptr && std::abs (t->lufs + 14.0f) < 0.2f, "reference LUFS " + juce::String (t ? t->lufs : 0, 2));
    }

    auto badFile = dir.getChildFile ("not_audio.wav");
    badFile.replaceWithText ("hello");
    p.loadSlot (5, badFile);
    for (int i = 0; i < 50 && p.getSlotInfo (5).status == RefABProcessor::SlotInfo::Status::loading; ++i) pump (20);
    check (p.getSlotInfo (5).status == RefABProcessor::SlotInfo::Status::error, "bad file reports error: " + p.getSlotInfo (5).error);
    p.clearSlot (5);

    std::cout << "Loudness match + A/B" << std::endl;
    {
        setParam (p, "mode", 1);  // restart
        setParam (p, "match", 1); // integrated
        setParam (p, "ab", 1);
        setParam (p, "slot", 0);
        p.resetMeters();

        // mix at -20 LUFS (1 kHz sine at -20 dBFS stereo), ref at -14 => ref gain should be -6 dB
        auto mix = makeSine (sr, 8.0, juce::Decibels::decibelsToGain (-20.0f), 1000.0);
        juce::AudioBuffer<float> out;
        ph.playing = true; ph.time = 0.0;
        run (p, ph, mix, out);

        check (std::abs (p.getMatchGainDb() + 6.0f) < 0.3f, "match gain " + juce::String (p.getMatchGainDb(), 2) + " dB (expect -6)");

        LoudnessMeter m;
        m.prepare (sr);
        const int skip = (int) sr * 2;
        m.process (out.getReadPointer (0) + skip, out.getReadPointer (1) + skip, out.getNumSamples() - skip);
        check (std::abs (m.getIntegrated() + 20.0f) < 0.4f, "matched reference output " + juce::String (m.getIntegrated(), 2) + " LUFS (expect -20)");

        // switch back to A: output equals input after the crossfade
        setParam (p, "ab", 0);
        run (p, ph, mix, out);
        float maxErr = 0;
        for (int i = (int) (0.1 * sr); i < out.getNumSamples(); ++i)
            maxErr = juce::jmax (maxErr, std::abs (out.getSample (0, i) - mix.getSample (0, i)));
        check (maxErr < 1.0e-6f, "A = untouched mix (max err " + juce::String (maxErr) + ")");
    }

    std::cout << "Sync accuracy" << std::endl;
    {
        // The reference is the same audio as the mix: in Sync mode B must null against A.
        auto noise = makeNoise (sr, 12.0, 0.3f, 7);
        auto nf = dir.getChildFile ("noise48.wav");
        juce::AudioBuffer<float> noiseQ (noise); // 24-bit file: compare with tolerance
        writeWav (nf, noise, sr);
        p.loadSlot (1, nf);
        check (waitReady (p, 1), "48 kHz noise loads");

        setParam (p, "slot", 1);
        setParam (p, "mode", 0);  // sync
        setParam (p, "match", 0); // off
        setParam (p, "trim", 0);
        setParam (p, "ab", 1);

        ph.playing = true;
        ph.time = 0.0;
        juce::AudioBuffer<float> out;
        run (p, ph, noise, out, 480);

        double errE = 0, sigE = 0;
        for (int i = (int) (0.1 * sr); i < out.getNumSamples(); ++i)
        {
            const double d = out.getSample (0, i) - noise.getSample (0, i);
            errE += d * d;
            sigE += (double) noise.getSample (0, i) * noise.getSample (0, i);
        }
        const double nullDb = 10.0 * std::log10 (errE / sigE + 1e-30);
        {
            double oe = 0; int bestLag = 0; double best = -1;
            for (int i = (int) (0.1 * sr); i < out.getNumSamples(); ++i) oe += (double) out.getSample (0, i) * out.getSample (0, i);
            for (int lag = -2000; lag <= 2000; ++lag)
            {
                double c = 0;
                for (int i = 20000; i < 60000; ++i) c += (double) out.getSample (0, i) * noise.getSample (0, i + lag);
                if (c > best) { best = c; bestLag = lag; }
            }
            std::cout << "    debug: out/sig energy " << oe / sigE << " best lag " << bestLag << " gain " << p.getMatchGainDb() << std::endl;
        }
        check (nullDb < -90.0, "B nulls against A in Sync: " + juce::String (nullDb, 1) + " dB");

        // Seek mid-way: host jumps to 5.0 s, reference must follow
        ph.time = 5.0;
        juce::AudioBuffer<float> part (2, 4800);
        for (int ch = 0; ch < 2; ++ch) part.copyFrom (ch, 0, noise, ch, (int) (5.0 * sr), 4800);
        run (p, ph, part, out, 480);
        double e2 = 0;
        for (int i = 960; i < 4800; ++i) e2 += std::pow (out.getSample (0, i) - part.getSample (0, i), 2.0);
        check (e2 < 1e-6, "follows a DAW jump to 5.0 s");

        // Loop: 2.0 .. 3.0 s, restart mode => after 1.5 s of playback we are at 2.5 s
        auto& ss = p.getSlotSettings (1);
        ss.loopStart = 2.0; ss.loopEnd = 3.0; ss.loopOn = true;
        setParam (p, "mode", 1);
        ph.playing = false;
        juce::AudioBuffer<float> silence (2, 960); silence.clear();
        run (p, ph, silence, out, 480);
        ph.playing = true; ph.time = 100.0;
        juce::AudioBuffer<float> blk (2, (int) (1.5 * sr)); blk.clear();
        run (p, ph, blk, out, 480);
        check (std::abs (p.getRefPositionSeconds() - 2.5) < 0.02, "loop wraps: position " + juce::String (p.getRefPositionSeconds(), 3) + " s (expect 2.5)");
        ss.loopOn = false;
    }

    std::cout << "Monitoring" << std::endl;
    {
        setParam (p, "ab", 0);
        setParam (p, "monitor", 2); // side
        juce::AudioBuffer<float> mono (2, 4800);
        for (int i = 0; i < 4800; ++i) { mono.setSample (0, i, 0.5f); mono.setSample (1, i, 0.5f); }
        juce::AudioBuffer<float> out;
        run (p, ph, mono, out);
        check (std::abs (out.getSample (0, 4000)) < 1e-6f, "Side of a mono signal is silent");
        setParam (p, "monitor", 0);
        run (p, ph, mono, out); // settle
    }

    std::cout << "State" << std::endl;
    {
        p.getSlotSettings (0).cue = 12.5;
        juce::MemoryBlock mb;
        p.getStateInformation (mb);

        RefABProcessor q;
        q.setRateAndBufferSizeDetails (sr, 512);
        q.prepareToPlay (sr, 512);
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        check (waitReady (q, 0) && waitReady (q, 1), "both slots reload from saved state");
        check (std::abs (q.getSlotSettings (0).cue.load() - 12.5) < 1e-9, "cue point restored");
        check (q.getSelectedSlot() == 1, "selected slot restored");
    }

    std::cout << "Editor" << std::endl;
    {
        setParam (p, "slot", 0);
        setParam (p, "ab", 1);
        setParam (p, "match", 1);
        setParam (p, "mode", 0);
        auto& ss = p.getSlotSettings (0);
        ss.loopStart = 8.0; ss.loopEnd = 16.0; ss.loopOn = true;
        p.updateLoopLoudness (0);
        p.resetMeters();

        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (1180, 740);

        auto mix = makeNoise (sr, 0.2, 0.25f, 3);
        juce::AudioBuffer<float> out;
        ph.playing = true; ph.time = 9.0;
        for (int i = 0; i < 25; ++i)
        {
            run (p, ph, mix, out);
            pump (34);
        }

        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        auto png = juce::File (argc > 1 ? juce::String (argv[1]) : dir.getChildFile ("editor.png").getFullPathName());
        png.deleteFile();
        juce::FileOutputStream fos (png);
        juce::PNGImageFormat fmt;
        check (fmt.writeImageToStream (img, fos), "editor snapshot -> " + png.getFullPathName());
        ed.reset();
    }

    p.setPlayHead (nullptr);
    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
