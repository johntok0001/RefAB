#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "LoudnessMeter.h"
#include "RefTrack.h"

class RefABProcessor : public juce::AudioProcessor
{
public:
    static constexpr int numSlots = 8;

    enum class Monitor { stereo, mono, side, left, right };
    enum class Band    { full, low, lowMid, highMid, high };
    enum class Match   { off, integrated, live };
    enum class Mode    { sync, restart, free };

    RefABProcessor();
    ~RefABProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "RefAB"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Reference slots (message thread)
    struct SlotInfo
    {
        enum class Status { empty, loading, ready, error };
        juce::File file;
        Status status = Status::empty;
        juce::String error;
    };

    struct SlotSettings
    {
        std::atomic<double> cue { 0.0 }, loopStart { 0.0 }, loopEnd { 0.0 }, anchor { 0.0 };
        std::atomic<bool> loopOn { false };
        std::atomic<float> loopLufs { LoudnessMeter::silence };
    };

    void loadSlot (int slot, const juce::File& file);
    void clearSlot (int slot);
    const SlotInfo& getSlotInfo (int slot) const { return slotInfo[(size_t) slot]; }
    SlotSettings& getSlotSettings (int slot) { return slotSettings[(size_t) slot]; }
    std::shared_ptr<const RefTrack> getTrack (int slot) const;
    void updateLoopLoudness (int slot);

    int getSelectedSlot() const;
    void setSelectedSlot (int slot);

    juce::ChangeBroadcaster slotsChanged;

    //==============================================================================
    // Transport helpers (message thread)
    void setFreePlaying (bool shouldPlay) { freePlaying.store (shouldPlay); }
    bool isFreePlaying() const { return freePlaying.load(); }
    // Moves all tracks together so that `slot` plays from `seconds` (Free / Restart modes).
    void seekTo (int slot, double seconds) { seekSlot.store (slot); seekRequest.store (seconds); }
    double getLastHostTime() const { return lastHostTime.load(); }
    bool isHostPlaying() const { return hostPlaying.load(); }

    // State for the UI (any thread)
    double getPositionSeconds (int slot) const { return posSeconds[(size_t) juce::jlimit (0, numSlots - 1, slot)].load(); }
    bool isReferenceAudible() const { return refAudible.load(); }
    bool isAFileAudible() const { return aAudible.load(); }

    // Side A source: -1 = the DAW input (your mix), otherwise a slot index (message thread)
    int getASlot() const { return aSlot.load(); }
    void setASlot (int slot);

    // The slot whose waveform the UI should show: the one being heard
    int getDisplaySlot() const
    {
        const int a = aSlot.load();
        return (a >= 0 && *pAB < 0.5f) ? a : getSelectedSlot();
    }
    bool isListeningToB() const { return *pAB > 0.5f; }
    float getMatchGainDb() const { return matchGainDb.load(); }
    bool isMatchMeasuring() const { return matchMeasuring.load(); }
    void resetMeters() { mixMeter.resetIntegrated(); refMeter.resetIntegrated(); }

    LoudnessMeter mixMeter, refMeter;
    ScopeRing mixScope, refScope;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    struct Geometry { double len, ls, le, start; bool loopOn; };
    static Geometry geometryFor (const SlotSettings&, const RefTrack&) noexcept;
    double slotPosition (int slot, const Geometry&, double sampleRate, bool sync, double hostTime) const noexcept;
    static bool renderSlot (const RefTrack&, const Geometry&, double pos, float* outL, float* outR, int n) noexcept;
    void configureBand (Band b);
    void reloadAllForSampleRate();

    // Parameters
    std::atomic<float>* pAB = nullptr;
    std::atomic<float>* pMonitor = nullptr;
    std::atomic<float>* pBand = nullptr;
    std::atomic<float>* pMatch = nullptr;
    std::atomic<float>* pTrim = nullptr;
    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pSlot = nullptr;

    // Tracks: audio thread reads under tryLock, other threads copy the shared_ptr under lock
    juce::CriticalSection trackLock;
    std::array<std::shared_ptr<const RefTrack>, numSlots> tracks;
    std::array<SlotInfo, numSlots> slotInfo;
    std::array<SlotSettings, numSlots> slotSettings;
    std::array<std::atomic<int>, numSlots> loadGeneration {};

    juce::ThreadPool loader { 1 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::atomic<double> loadedRate { 0.0 };

    // Playback (audio thread)
    double elapsed = 0.0; // shared playback clock in samples (Restart / Free modes)
    int lastMode = -1;
    bool wasHostPlaying = false;
    std::atomic<bool> freePlaying { false };
    std::atomic<double> seekRequest { -1.0 };
    std::atomic<int> seekSlot { 0 };
    std::atomic<int> aSlot { -1 };
    std::atomic<bool> aAudible { false };
    std::array<std::atomic<double>, numSlots> posSeconds {};
    std::atomic<double> lastHostTime { 0.0 };
    std::atomic<bool> hostPlaying { false };
    std::atomic<bool> refAudible { false };
    std::atomic<float> matchGainDb { 0.0f };
    std::atomic<bool> matchMeasuring { false };

    juce::AudioBuffer<float> refBuffer, aBuffer;
    juce::SmoothedValue<float> abSmoothed, refGain;
    float liveMatchDb = 0.0f;

    juce::dsp::LinkwitzRileyFilter<float> hpFilter, lpFilter;
    Band currentBand = Band::full;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RefABProcessor)
};
