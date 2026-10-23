#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::StringArray monitorNames { "Stereo", "Mono", "Side", "Left", "Right" };
    const juce::StringArray bandNames    { "Full", "Low", "Low-Mid", "High-Mid", "High" };
    const juce::StringArray matchNames   { "Off", "Integrated", "Live" };
    const juce::StringArray modeNames    { "Sync", "Restart", "Free" };

    juce::StringArray slotNames()
    {
        juce::StringArray s;
        for (int i = 1; i <= RefABProcessor::numSlots; ++i)
            s.add ("Ref " + juce::String (i));
        return s;
    }

    template <typename Fn>
    void onMessageThread (std::shared_ptr<std::atomic<bool>> alive, Fn&& fn)
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            fn();
            return;
        }

        juce::MessageManager::callAsync ([alive, f = std::forward<Fn> (fn)]() mutable
        {
            if (alive->load())
                f();
        });
    }
}

//==============================================================================
RefABProcessor::RefABProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pAB      = apvts.getRawParameterValue ("ab");
    pMonitor = apvts.getRawParameterValue ("monitor");
    pBand    = apvts.getRawParameterValue ("band");
    pMatch   = apvts.getRawParameterValue ("match");
    pTrim    = apvts.getRawParameterValue ("trim");
    pMode    = apvts.getRawParameterValue ("mode");
    pSlot    = apvts.getRawParameterValue ("slot");

    for (auto& g : loadGeneration)
        g.store (0);
}

RefABProcessor::~RefABProcessor()
{
    alive->store (false);
    loader.removeAllJobs (true, 10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout RefABProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "ab", 1 }, "A/B (Reference)", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "slot", 1 }, "Reference Slot", slotNames(), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "monitor", 1 }, "Monitor", monitorNames, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "band", 1 }, "Band", bandNames, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "match", 1 }, "Loudness Match", matchNames, 1));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "trim", 1 }, "Reference Trim",
                                                       NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Playback Mode", modeNames, 1));
    return layout;
}

bool RefABProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

//==============================================================================
void RefABProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    mixMeter.prepare (sampleRate);
    refMeter.prepare (sampleRate);
    refBuffer.setSize (2, juce::jmax (samplesPerBlock, 512));

    abSmoothed.reset (sampleRate, 0.02);
    abSmoothed.setCurrentAndTargetValue (*pAB > 0.5f ? 1.0f : 0.0f);
    refGain.reset (sampleRate, 0.08);
    refGain.setCurrentAndTargetValue (1.0f);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (samplesPerBlock, 512), 2 };
    hpFilter.prepare (spec);
    lpFilter.prepare (spec);
    hpFilter.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
    lpFilter.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    currentBand = Band::full;

    wasHostPlaying = false;

    if (std::abs (loadedRate.load() - sampleRate) > 0.5)
    {
        loadedRate.store (sampleRate);
        onMessageThread (alive, [this] { reloadAllForSampleRate(); });
    }
}

void RefABProcessor::configureBand (Band b)
{
    currentBand = b;
    hpFilter.reset();
    lpFilter.reset();

    switch (b)
    {
        case Band::low:     lpFilter.setCutoffFrequency (120.0f); break;
        case Band::lowMid:  hpFilter.setCutoffFrequency (120.0f);  lpFilter.setCutoffFrequency (1000.0f); break;
        case Band::highMid: hpFilter.setCutoffFrequency (1000.0f); lpFilter.setCutoffFrequency (6000.0f); break;
        case Band::high:    hpFilter.setCutoffFrequency (6000.0f); break;
        case Band::full:    break;
    }
}

RefABProcessor::Geometry RefABProcessor::geometryFor (const SlotSettings& ss, const RefTrack& t) noexcept
{
    Geometry g;
    const double sr = t.sampleRate;
    g.len = t.audio.getNumSamples();
    g.ls = juce::jlimit (0.0, g.len, ss.loopStart.load() * sr);
    g.le = juce::jlimit (0.0, g.len, ss.loopEnd.load() * sr);
    g.loopOn = ss.loopOn.load() && (g.le - g.ls) > 0.05 * sr;
    g.start = g.loopOn ? g.ls : juce::jlimit (0.0, g.len, ss.cue.load() * sr);
    return g;
}

double RefABProcessor::slotPosition (int slot, const Geometry& g, double sr, bool sync, double hostTime) const noexcept
{
    const double offset = sync ? (hostTime - slotSettings[(size_t) slot].anchor.load()) * sr : elapsed;
    double p = g.start + offset;

    if (g.loopOn)
    {
        const double l = g.le - g.ls;
        double r = std::fmod (p - g.ls, l);
        if (r < 0) r += l;
        p = g.ls + r;
    }
    return p;
}

bool RefABProcessor::renderSlot (const RefTrack& t, const Geometry& g, double pos, float* outL, float* outR, int n) noexcept
{
    const float* srcL = t.audio.getReadPointer (0);
    const float* srcR = t.audio.getReadPointer (1);
    const auto len = (juce::int64) g.len;
    auto ip = (juce::int64) std::floor (pos + 0.5); // nearest sample: host times are not exact in floating point
    bool any = false;

    for (int i = 0; i < n; ++i)
    {
        if (g.loopOn && ip >= (juce::int64) g.le)
            ip = (juce::int64) g.ls;

        if (ip >= 0 && ip < len)
        {
            outL[i] = srcL[ip];
            outR[i] = srcR[ip];
            any = true;
        }
        else
        {
            outL[i] = outR[i] = 0.0f;
        }
        ++ip;
    }
    return any;
}

void RefABProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (n == 0 || numCh == 0)
        return;

    if (refBuffer.getNumSamples() < n) refBuffer.setSize (2, n, false, false, true);
    if (aBuffer.getNumSamples() < n)   aBuffer.setSize (2, n, false, false, true);

    float* L = buffer.getWritePointer (0);
    float* R = numCh > 1 ? buffer.getWritePointer (1) : L;
    const double sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;

    // Host transport
    bool playing = false;
    double hostTime = 0.0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto ts = pos->getTimeInSamples())
                hostTime = (double) *ts / sr;
            else if (auto t = pos->getTimeInSeconds())
                hostTime = *t;
        }
    }
    hostPlaying.store (playing);
    lastHostTime.store (hostTime);

    // Playback clock
    const auto mode = (Mode) juce::jlimit (0, 2, (int) pMode->load());
    const bool sync = mode == Mode::sync;
    if ((int) mode != lastMode)
    {
        elapsed = 0.0;
        lastMode = (int) mode;
    }

    bool running = false;
    switch (mode)
    {
        case Mode::sync:    running = playing; break;
        case Mode::restart: if (playing && ! wasHostPlaying) elapsed = 0.0; running = playing; break;
        case Mode::free:    running = freePlaying.load(); break;
    }
    wasHostPlaying = playing;

    const int bSlot = getSelectedSlot();
    const int aSel = aSlot.load();
    const bool aIsFile = aSel >= 0;

    float* rL = refBuffer.getWritePointer (0);
    float* rR = refBuffer.getWritePointer (1);
    float* aL = aBuffer.getWritePointer (0);
    float* aR = aBuffer.getWritePointer (1);
    bool bOn = false, aOn = false, anyLoop = false;
    float bLufs = LoudnessMeter::silence, aLufs = LoudnessMeter::silence;

    {
        juce::ScopedTryLock sl (trackLock);
        if (sl.isLocked())
        {
            const double seek = seekRequest.exchange (-1.0);
            if (seek >= 0.0 && ! sync)
            {
                const int s = juce::jlimit (0, numSlots - 1, seekSlot.load());
                if (auto* t = tracks[(size_t) s].get())
                    elapsed = juce::jmax (0.0, seek * sr - geometryFor (slotSettings[(size_t) s], *t).start);
            }

            auto loudnessOf = [this] (int s, const RefTrack& t)
            {
                auto& ss = slotSettings[(size_t) s];
                const float loopL = ss.loopLufs.load();
                return (ss.loopOn.load() && loopL > -70.0f) ? loopL : t.lufs;
            };

            if (auto* t = tracks[(size_t) bSlot].get())
            {
                const auto g = geometryFor (slotSettings[(size_t) bSlot], *t);
                anyLoop |= g.loopOn;
                bLufs = loudnessOf (bSlot, *t);
                if (running)
                    bOn = renderSlot (*t, g, slotPosition (bSlot, g, sr, sync, hostTime), rL, rR, n);
            }

            if (aIsFile)
                if (auto* t = tracks[(size_t) aSel].get())
                {
                    const auto g = geometryFor (slotSettings[(size_t) aSel], *t);
                    anyLoop |= g.loopOn;
                    aLufs = loudnessOf (aSel, *t);
                    if (running)
                        aOn = renderSlot (*t, g, slotPosition (aSel, g, sr, sync, hostTime), aL, aR, n);
                }

            if (running && ! sync)
            {
                elapsed += n;
                // Free mode stops by itself once everything has played to the end
                if (mode == Mode::free && ! anyLoop && ! bOn && ! aOn)
                {
                    freePlaying.store (false);
                    elapsed = 0.0;
                }
            }

            for (int s = 0; s < numSlots; ++s)
                if (auto* t = tracks[(size_t) s].get())
                {
                    const auto g = geometryFor (slotSettings[(size_t) s], *t);
                    posSeconds[(size_t) s].store (juce::jlimit (0.0, g.len, slotPosition (s, g, sr, sync, hostTime)) / sr);
                }
        }
    }

    if (! bOn)
    {
        juce::FloatVectorOperations::clear (rL, n);
        juce::FloatVectorOperations::clear (rR, n);
    }

    // Side A: either the DAW input, or a loaded file replacing it
    if (aIsFile)
    {
        if (! aOn)
        {
            juce::FloatVectorOperations::clear (aL, n);
            juce::FloatVectorOperations::clear (aR, n);
        }

        if (R != L)
        {
            juce::FloatVectorOperations::copy (L, aL, n);
            juce::FloatVectorOperations::copy (R, aR, n);
        }
        else
        {
            for (int i = 0; i < n; ++i) L[i] = 0.5f * (aL[i] + aR[i]);
        }

        for (int ch = 2; ch < numCh; ++ch)
            buffer.clear (ch, 0, n);
    }

    refAudible.store (bOn);
    aAudible.store (aOn);

    // Measure A
    mixMeter.process (L, R, n);
    mixScope.push (L, R, n);

    if (bOn)
        refMeter.process (rL, rR, n);

    // Loudness match (gain applied to B)
    const auto match = (Match) juce::jlimit (0, 2, (int) pMatch->load());
    float gainDb = 0.0f;
    bool measuring = false;

    if (match == Match::integrated)
    {
        float mixL;
        if (aIsFile)
        {
            mixL = aLufs; // known exactly from the file
        }
        else
        {
            mixL = mixMeter.getIntegrated();
            if (mixL <= -70.0f)
                mixL = mixMeter.getShortTerm();
        }

        if (mixL > -70.0f && bLufs > -70.0f)
            gainDb = mixL - bLufs;
        else
            measuring = true;
    }
    else if (match == Match::live)
    {
        const float mixS = mixMeter.getShortTerm();
        const float refS = refMeter.getShortTerm();
        if (bOn && mixS > -60.0f && refS > -60.0f)
        {
            const float coeff = 1.0f - std::exp (-(float) n / (float) (sr * 1.5));
            liveMatchDb += (mixS - refS - liveMatchDb) * coeff;
        }
        else if (! bOn || mixS <= -60.0f)
        {
            measuring = true;
        }
        gainDb = liveMatchDb;
    }

    gainDb = juce::jlimit (-36.0f, 12.0f, gainDb + pTrim->load());
    matchGainDb.store (gainDb);
    matchMeasuring.store (measuring);
    refGain.setTargetValue (juce::Decibels::decibelsToGain (gainDb));
    refGain.applyGain (refBuffer, n);

    refScope.push (rL, rR, n);

    // A/B crossfade
    abSmoothed.setTargetValue (*pAB > 0.5f ? 1.0f : 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const float x = abSmoothed.getNextValue();
        L[i] = L[i] * (1.0f - x) + rL[i] * x;
        if (R != L)
            R[i] = R[i] * (1.0f - x) + rR[i] * x;
    }

    // Monitoring
    if (R != L)
    {
        const auto mon = (Monitor) juce::jlimit (0, 4, (int) pMonitor->load());
        for (int i = 0; i < n && mon != Monitor::stereo; ++i)
        {
            const float l = L[i], r = R[i];
            float v = 0.0f;
            switch (mon)
            {
                case Monitor::mono:   v = 0.5f * (l + r); break;
                case Monitor::side:   v = 0.5f * (l - r); break;
                case Monitor::left:   v = l; break;
                case Monitor::right:  v = r; break;
                case Monitor::stereo: break;
            }
            L[i] = R[i] = v;
        }
    }

    // Band solo
    const auto band = (Band) juce::jlimit (0, 4, (int) pBand->load());
    if (band != currentBand)
        configureBand (band);

    if (band != Band::full)
    {
        const bool useHp = band != Band::low;
        const bool useLp = band != Band::high;
        for (int ch = 0; ch < juce::jmin (2, numCh); ++ch)
        {
            float* d = buffer.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                float v = d[i];
                if (useHp) v = hpFilter.processSample (ch, v);
                if (useLp) v = lpFilter.processSample (ch, v);
                d[i] = v;
            }
        }
    }
}

void RefABProcessor::setASlot (int slot)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());
    slot = (slot >= 0 && slot < numSlots) ? slot : -1;
    if (aSlot.exchange (slot) == slot)
        return;

    if (slot >= 0)
    {
        // Two files need no DAW transport: switch to Free so the Play button works
        if (auto* p = apvts.getParameter ("mode"))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) Mode::free));

        // Don't let A and B be the same track
        if (getSelectedSlot() == slot)
            for (int i = 0; i < numSlots; ++i)
                if (i != slot && slotInfo[(size_t) i].status == SlotInfo::Status::ready) { setSelectedSlot (i); break; }
    }

    resetMeters();
    slotsChanged.sendChangeMessage();
}

//==============================================================================
std::shared_ptr<const RefTrack> RefABProcessor::getTrack (int slot) const
{
    const juce::ScopedLock sl (trackLock);
    return tracks[(size_t) slot];
}

int RefABProcessor::getSelectedSlot() const
{
    return juce::jlimit (0, numSlots - 1, (int) pSlot->load());
}

void RefABProcessor::setSelectedSlot (int slot)
{
    if (auto* p = apvts.getParameter ("slot"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) slot));
        p->endChangeGesture();
    }
}

void RefABProcessor::loadSlot (int slot, const juce::File& file)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());
    if (slot < 0 || slot >= numSlots)
        return;

    auto& info = slotInfo[(size_t) slot];
    const bool sameFile = info.file == file;
    info.file = file;
    info.status = SlotInfo::Status::loading;
    info.error.clear();

    if (! sameFile)
    {
        auto& ss = slotSettings[(size_t) slot];
        ss.cue = 0.0; ss.loopStart = 0.0; ss.loopEnd = 0.0; ss.anchor = 0.0;
        ss.loopOn = false; ss.loopLufs = LoudnessMeter::silence;
    }

    slotsChanged.sendChangeMessage();

    const int gen = ++loadGeneration[(size_t) slot];
    const double rate = getSampleRate() > 0 ? getSampleRate() : 44100.0;
    auto aliveFlag = alive;

    loader.addJob ([this, slot, file, rate, gen, aliveFlag]
    {
        if (! aliveFlag->load()) return;

        juce::String error;
        auto track = loadRefTrack (file, rate, error);

        if (! aliveFlag->load() || loadGeneration[(size_t) slot].load() != gen)
            return;

        std::shared_ptr<const RefTrack> old;
        if (track != nullptr)
        {
            const juce::ScopedLock sl (trackLock);
            old = std::move (tracks[(size_t) slot]);
            tracks[(size_t) slot] = track;
        }
        old.reset(); // free the previous track on this background thread

        juce::MessageManager::callAsync ([this, slot, gen, error, ok = track != nullptr, aliveFlag]
        {
            if (! aliveFlag->load() || loadGeneration[(size_t) slot].load() != gen)
                return;

            auto& inf = slotInfo[(size_t) slot];
            inf.status = ok ? SlotInfo::Status::ready : SlotInfo::Status::error;
            inf.error = error;

            if (ok && slotSettings[(size_t) slot].loopOn.load())
                updateLoopLoudness (slot);

            slotsChanged.sendChangeMessage();
        });
    });
}

void RefABProcessor::clearSlot (int slot)
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());
    ++loadGeneration[(size_t) slot];

    std::shared_ptr<const RefTrack> old;
    {
        const juce::ScopedLock sl (trackLock);
        old = std::move (tracks[(size_t) slot]);
    }

    slotInfo[(size_t) slot] = {};
    if (aSlot.load() == slot)
        aSlot.store (-1);
    auto& ss = slotSettings[(size_t) slot];
    ss.cue = 0.0; ss.loopStart = 0.0; ss.loopEnd = 0.0; ss.anchor = 0.0;
    ss.loopOn = false; ss.loopLufs = LoudnessMeter::silence;

    if (old != nullptr)
        loader.addJob ([o = std::move (old)]() mutable { o.reset(); });

    slotsChanged.sendChangeMessage();
}

void RefABProcessor::updateLoopLoudness (int slot)
{
    auto track = getTrack (slot);
    auto& ss = slotSettings[(size_t) slot];
    ss.loopLufs = LoudnessMeter::silence;

    if (track == nullptr || ! ss.loopOn.load())
        return;

    const double s = ss.loopStart.load(), e = ss.loopEnd.load();
    const int gen = loadGeneration[(size_t) slot].load();
    auto aliveFlag = alive;

    loader.addJob ([this, track, s, e, slot, gen, aliveFlag]
    {
        const float l = track->measureRegion (s, e);
        if (aliveFlag->load() && loadGeneration[(size_t) slot].load() == gen)
        {
            auto& st = slotSettings[(size_t) slot];
            if (st.loopStart.load() == s && st.loopEnd.load() == e)
                st.loopLufs = l;
        }
    });
}

void RefABProcessor::reloadAllForSampleRate()
{
    for (int i = 0; i < numSlots; ++i)
    {
        auto t = getTrack (i);
        if (t != nullptr && std::abs (t->sampleRate - loadedRate.load()) > 0.5)
            loadSlot (i, slotInfo[(size_t) i].file);
        else if (t == nullptr && slotInfo[(size_t) i].status == SlotInfo::Status::loading)
            loadSlot (i, slotInfo[(size_t) i].file);
    }
}

//==============================================================================
void RefABProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("RefAB");
    root.setProperty ("version", 1, nullptr);
    root.appendChild (apvts.copyState(), nullptr);

    juce::ValueTree slots ("Slots");
    for (int i = 0; i < numSlots; ++i)
    {
        const auto& info = slotInfo[(size_t) i];
        if (info.file == juce::File())
            continue;

        auto& ss = slotSettings[(size_t) i];
        juce::ValueTree s ("Slot");
        s.setProperty ("index", i, nullptr);
        s.setProperty ("path", info.file.getFullPathName(), nullptr);
        s.setProperty ("cue", ss.cue.load(), nullptr);
        s.setProperty ("loopOn", ss.loopOn.load(), nullptr);
        s.setProperty ("loopStart", ss.loopStart.load(), nullptr);
        s.setProperty ("loopEnd", ss.loopEnd.load(), nullptr);
        s.setProperty ("anchor", ss.anchor.load(), nullptr);
        slots.appendChild (s, nullptr);
    }
    root.appendChild (slots, nullptr);
    root.setProperty ("aSlot", aSlot.load(), nullptr);

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void RefABProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("RefAB"))
        return;

    auto params = root.getChildWithName (apvts.state.getType());
    if (params.isValid())
        apvts.replaceState (params);

    auto slots = root.getChildWithName ("Slots");
    const int savedA = root.getProperty ("aSlot", -1);

    onMessageThread (alive, [this, slots, savedA]
    {
        for (int i = 0; i < numSlots; ++i)
            if (slotInfo[(size_t) i].file != juce::File())
                clearSlot (i);

        for (const auto& s : slots)
        {
            const int i = s.getProperty ("index", -1);
            const juce::File f (s.getProperty ("path").toString());
            if (i < 0 || i >= numSlots || f == juce::File())
                continue;

            loadSlot (i, f);

            auto& ss = slotSettings[(size_t) i];
            ss.cue = (double) s.getProperty ("cue", 0.0);
            ss.loopOn = (bool) s.getProperty ("loopOn", false);
            ss.loopStart = (double) s.getProperty ("loopStart", 0.0);
            ss.loopEnd = (double) s.getProperty ("loopEnd", 0.0);
            ss.anchor = (double) s.getProperty ("anchor", 0.0);
        }

        aSlot.store ((savedA >= 0 && savedA < numSlots && slotInfo[(size_t) savedA].file != juce::File()) ? savedA : -1);
        slotsChanged.sendChangeMessage();
    });
}

//==============================================================================
juce::AudioProcessorEditor* RefABProcessor::createEditor()
{
    return new RefABEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RefABProcessor();
}
