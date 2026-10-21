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

    lastSlot = -1;
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

bool RefABProcessor::renderReference (float* outL, float* outR, int n, bool playing, double hostTime)
{
    juce::ScopedTryLock sl (trackLock);
    if (! sl.isLocked())
        return false;

    const int slot = juce::jlimit (0, numSlots - 1, (int) pSlot->load());
    const auto* track = tracks[(size_t) slot].get();
    const auto mode = (Mode) juce::jlimit (0, 2, (int) pMode->load());
    auto& ss = slotSettings[(size_t) slot];

    if (track == nullptr)
    {
        lastSlot = slot;
        return false;
    }

    const double sr = track->sampleRate;
    const int len = track->audio.getNumSamples();
    const double cue = ss.cue.load() * sr;
    const double ls = juce::jlimit (0.0, (double) len, ss.loopStart.load() * sr);
    const double le = juce::jlimit (0.0, (double) len, ss.loopEnd.load() * sr);
    const bool loopOn = ss.loopOn.load() && (le - ls) > 0.05 * sr;
    const double startPoint = loopOn ? ls : cue;

    auto wrap = [&] (double p)
    {
        if (! loopOn) return p;
        const double l = le - ls;
        double r = std::fmod (p - ls, l);
        if (r < 0) r += l;
        return ls + r;
    };

    if (slot != lastSlot || (int) mode != lastMode)
    {
        refPos = startPoint;
        lastSlot = slot;
        lastMode = (int) mode;
    }

    const double seek = seekRequest.exchange (-1.0);
    if (seek >= 0.0)
        refPos = seek * sr;

    bool active = false;
    double pos = 0.0;

    switch (mode)
    {
        case Mode::sync:
            active = playing;
            pos = cue + (hostTime - ss.anchor.load()) * sr;
            if (! playing) refPos = wrap (pos);
            break;

        case Mode::restart:
            if (playing && ! wasHostPlaying)
                refPos = startPoint;
            active = playing;
            pos = refPos;
            break;

        case Mode::free:
            active = freePlaying.load();
            pos = refPos;
            break;
    }

    wasHostPlaying = playing;

    if (! active)
    {
        refDisplayPos.store (wrap (mode == Mode::sync ? pos : refPos) / sr);
        return false;
    }

    pos = wrap (pos);
    const float* srcL = track->audio.getReadPointer (0);
    const float* srcR = track->audio.getReadPointer (1);
    auto ip = (juce::int64) std::floor (pos + 0.5); // nearest sample: host times are not exact in floating point

    for (int i = 0; i < n; ++i)
    {
        if (loopOn && ip >= (juce::int64) le)
            ip = (juce::int64) ls;

        if (ip >= 0 && ip < len)
        {
            outL[i] = srcL[ip];
            outR[i] = srcR[ip];
        }
        else
        {
            outL[i] = outR[i] = 0.0f;
        }
        ++ip;
    }

    if (mode != Mode::sync)
    {
        refPos = wrap (refPos + n);
        if (! loopOn && refPos >= len && mode == Mode::free)
        {
            freePlaying.store (false);
            refPos = cue;
        }
    }

    refDisplayPos.store (juce::jlimit (0.0, (double) len, (double) ip) / sr);
    return true;
}

void RefABProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (n == 0 || numCh == 0)
        return;

    if (refBuffer.getNumSamples() < n)
        refBuffer.setSize (2, n, false, false, true);

    float* L = buffer.getWritePointer (0);
    float* R = numCh > 1 ? buffer.getWritePointer (1) : L;

    // Host transport
    bool playing = false;
    double hostTime = 0.0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto ts = pos->getTimeInSamples())
                hostTime = (double) *ts / getSampleRate();
            else if (auto t = pos->getTimeInSeconds())
                hostTime = *t;
        }
    }
    hostPlaying.store (playing);
    lastHostTime.store (hostTime);

    // Measure the mix
    mixMeter.process (L, R, n);
    mixScope.push (L, R, n);

    // Render the reference
    float* rL = refBuffer.getWritePointer (0);
    float* rR = refBuffer.getWritePointer (1);
    const bool refOn = renderReference (rL, rR, n, playing, hostTime);
    refAudible.store (refOn);

    if (! refOn)
        juce::FloatVectorOperations::clear (rL, n), juce::FloatVectorOperations::clear (rR, n);
    else
        refMeter.process (rL, rR, n);

    // Loudness match
    const auto match = (Match) juce::jlimit (0, 2, (int) pMatch->load());
    float gainDb = 0.0f;
    bool measuring = false;

    if (match == Match::integrated)
    {
        float refL = LoudnessMeter::silence;
        {
            const int slot = juce::jlimit (0, numSlots - 1, (int) pSlot->load());
            juce::ScopedTryLock sl (trackLock);
            if (sl.isLocked())
                if (auto* t = tracks[(size_t) slot].get())
                {
                    auto& ss = slotSettings[(size_t) slot];
                    const float loopL = ss.loopLufs.load();
                    refL = (ss.loopOn.load() && loopL > -70.0f) ? loopL : t->lufs;
                }
        }

        float mixL = mixMeter.getIntegrated();
        if (mixL <= -70.0f)
            mixL = mixMeter.getShortTerm();

        if (mixL > -70.0f && refL > -70.0f)
            gainDb = mixL - refL;
        else
            measuring = true;
    }
    else if (match == Match::live)
    {
        const float mixS = mixMeter.getShortTerm();
        const float refS = refMeter.getShortTerm();
        if (refOn && mixS > -60.0f && refS > -60.0f)
        {
            const float coeff = 1.0f - std::exp (-(float) n / (float) (getSampleRate() * 1.5));
            liveMatchDb += (mixS - refS - liveMatchDb) * coeff;
        }
        else if (! refOn || mixS <= -60.0f)
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

    onMessageThread (alive, [this, slots]
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
