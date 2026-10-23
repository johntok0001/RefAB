#include "PluginEditor.h"

using namespace ui;

RefABEditor::RefABEditor (RefABProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      abButton (*p.apvts.getParameter ("ab")),
      monitorChoice (*p.apvts.getParameter ("monitor"), { "Stereo", "Mono", "Side", "L", "R" }, col::text),
      bandChoice (*p.apvts.getParameter ("band"), { "Full", "Low", "Lo-Mid", "Hi-Mid", "High" }, col::text),
      slotList (p), spectrum (p), waveform (p), meters (p), gonio (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (abButton);
    abButton.aLabel = [this]
    {
        const int a = proc.getASlot();
        return a < 0 ? juce::String ("DAW MIX") : proc.getSlotInfo (a).file.getFileNameWithoutExtension();
    };
    abButton.bLabel = [this]
    {
        const auto& info = proc.getSlotInfo (proc.getSelectedSlot());
        return info.file == juce::File() ? juce::String ("REFERENCE") : info.file.getFileNameWithoutExtension();
    };
    addAndMakeVisible (monitorChoice);
    addAndMakeVisible (bandChoice);
    addAndMakeVisible (slotList);
    addAndMakeVisible (spectrum);
    addAndMakeVisible (waveform);
    addAndMakeVisible (meters);
    addAndMakeVisible (gonio);

    monitorChoice.setHelpText ("Applies to whatever you are listening to, mix or reference.");
    bandChoice.setHelpText ("Solo a frequency band: Low <120 Hz, Lo-Mid 120 Hz-1 kHz, Hi-Mid 1-6 kHz, High >6 kHz.");

    matchBox.addItemList ({ "Match: Off", "Match: Integrated", "Match: Live" }, 1);
    matchBox.setTooltip ("Loudness match for the reference.\n"
                         "Integrated: reference (or its loop) integrated LUFS is matched to your mix's integrated LUFS.\n"
                         "Live: continuously matches short-term loudness while both are playing.");
    addAndMakeVisible (matchBox);
    matchAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "match", matchBox);

    trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 22);
    trimSlider.setTextValueSuffix (" dB");
    trimSlider.setDoubleClickReturnValue (true, 0.0);
    trimSlider.setTooltip ("Extra gain on the reference, on top of loudness matching. Double-click to reset.");
    addAndMakeVisible (trimSlider);
    trimAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "trim", trimSlider);

    resetButton.setTooltip ("Reset integrated loudness and peak hold for both mix and reference.");
    resetButton.onClick = [this] { proc.resetMeters(); };
    addAndMakeVisible (resetButton);

    modeBox.addItemList ({ "Sync", "Restart", "Free" }, 1);
    modeBox.setTooltip ("Sync: the reference follows the DAW timeline (use 'Align to DAW' to line it up).\n"
                        "Restart: the reference starts from the cue point / loop start every time the DAW starts.\n"
                        "Free: the reference has its own Play button and ignores the DAW transport.");
    addAndMakeVisible (modeBox);
    modeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "mode", modeBox);

    playButton.onClick = [this] { proc.setFreePlaying (! proc.isFreePlaying()); };
    playButton.setTooltip ("Free mode: play / stop the reference.");
    addAndMakeVisible (playButton);

    loopButton.setClickingTogglesState (false);
    loopButton.onClick = [this] { toggleLoop(); };
    loopButton.setTooltip ("Loop a section of the reference. Drag on the waveform to set the loop, double-click to clear it.");
    addAndMakeVisible (loopButton);

    alignButton.onClick = [this]
    {
        // every track's cue point lines up with the current DAW position
        for (int i = 0; i < RefABProcessor::numSlots; ++i)
            proc.getSlotSettings (i).anchor = proc.getLastHostTime();
    };
    alignButton.setTooltip ("Sync mode: the cue point (white triangle) is aligned to the current DAW position.");
    addAndMakeVisible (alignButton);

    diffButton.setClickingTogglesState (true);
    diffButton.onClick = [this] { spectrum.setDifferenceMode (diffButton.getToggleState()); };
    diffButton.setTooltip ("Show the spectral difference between the reference and your mix.");
    addAndMakeVisible (diffButton);

    posLabel.setFont (ui::mono (11.5f));
    posLabel.setColour (juce::Label::textColourId, col::dim);
    posLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (posLabel);

    for (auto* l : { &matchLabel, &trimLabel })
    {
        l->setFont (ui::font (10.5f, true));
        l->setColour (juce::Label::textColourId, col::dim);
        l->setBorderSize ({});
        addAndMakeVisible (*l);
    }

    waveform.onLoopChanged = [this] { updateTransport(); };

    setResizable (true, true);
    setResizeLimits (1000, 640, 2400, 1600);
    setSize (1180, 740);

    startTimerHz (30);
}

RefABEditor::~RefABEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void RefABEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    g.setColour (col::text);
    g.setFont (ui::font (24.0f, true));
    auto t = titleArea;
    g.drawText ("Ref", t.removeFromLeft (44), juce::Justification::centredLeft);
    g.setColour (col::ref);
    g.drawText ("AB", t.removeFromLeft (40), juce::Justification::centredLeft);
    g.setColour (col::faint);
    g.setFont (ui::font (11.0f));
    g.drawText ("reference A/B", titleArea.withTrimmedTop (34), juce::Justification::centredLeft);

    g.setFont (ui::font (10.5f, true));
    g.setColour (col::dim);
    g.drawText ("MONITOR", monitorCaption, juce::Justification::centredLeft);
    g.drawText ("BAND", bandCaption, juce::Justification::centredLeft);
}

void RefABEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Header
    auto header = area.removeFromTop (62);
    titleArea = header.removeFromLeft (150).withTrimmedTop (4);
    abButton.setBounds (header.removeFromLeft (230).reduced (0, 2));
    header.removeFromLeft (24);

    auto group = [&] (int w, juce::Rectangle<int>& caption, juce::Component& c)
    {
        auto g = header.removeFromLeft (w);
        caption = g.removeFromTop (22).withTrimmedTop (6);
        c.setBounds (g.removeFromTop (32));
        header.removeFromLeft (18);
    };
    group (280, monitorCaption, monitorChoice);
    group (330, bandCaption, bandChoice);

    diffButton.setBounds (header.removeFromRight (110).withTrimmedTop (22).removeFromTop (32));

    area.removeFromTop (12);

    // Columns
    auto left = area.removeFromLeft (250);
    area.removeFromLeft (12);
    auto right = area.removeFromRight (300);
    area.removeFromRight (12);

    slotList.setBounds (left);

    auto meterArea = right.removeFromTop (juce::jmin (500, right.getHeight() - 180));
    meters.setBounds (meterArea);
    right.removeFromTop (12);
    gonio.setBounds (right);

    {
        auto c = meterArea.reduced (14, 12);
        c = c.removeFromBottom (128);
        matchLabel.setBounds (c.removeFromTop (16));
        matchBox.setBounds (c.removeFromTop (28));
        c.removeFromTop (6);
        trimLabel.setBounds (c.removeFromTop (16));
        trimSlider.setBounds (c.removeFromTop (26));
        c.removeFromTop (6);
        resetButton.setBounds (c.removeFromTop (28));
    }

    // Centre
    auto transport = area.removeFromBottom (34);
    area.removeFromBottom (10);
    waveform.setBounds (area.removeFromBottom (juce::jmax (110, area.getHeight() / 4)));
    area.removeFromBottom (12);
    spectrum.setBounds (area);

    modeBox.setBounds (transport.removeFromLeft (110));
    transport.removeFromLeft (8);
    playButton.setBounds (transport.removeFromLeft (76));
    transport.removeFromLeft (8);
    loopButton.setBounds (transport.removeFromLeft (68));
    transport.removeFromLeft (8);
    alignButton.setBounds (transport.removeFromLeft (112));
    transport.removeFromLeft (12);
    posLabel.setBounds (transport);
}

void RefABEditor::toggleLoop()
{
    const int slot = proc.getDisplaySlot();
    auto& ss = proc.getSlotSettings (slot);
    auto track = proc.getTrack (slot);
    if (track == nullptr) return;

    if (! ss.loopOn.load() && ss.loopEnd.load() <= ss.loopStart.load())
    {
        const double len = track->getLengthSeconds();
        const double s = juce::jlimit (0.0, len, ss.cue.load());
        ss.loopStart = s;
        ss.loopEnd = juce::jmin (len, s + 16.0);
    }

    ss.loopOn = ! ss.loopOn.load();
    proc.updateLoopLoudness (slot);
    updateTransport();
}

void RefABEditor::updateTransport()
{
    const auto mode = (RefABProcessor::Mode) modeBox.getSelectedItemIndex();
    const int slot = proc.getDisplaySlot();
    auto& ss = proc.getSlotSettings (slot);
    auto track = proc.getTrack (slot);
    const bool anyTrack = track != nullptr || proc.getTrack (proc.getSelectedSlot()) != nullptr;

    playButton.setEnabled (mode == RefABProcessor::Mode::free && anyTrack);
    playButton.setButtonText (proc.isFreePlaying() ? "Stop" : "Play");
    playButton.setToggleState (proc.isFreePlaying() && mode == RefABProcessor::Mode::free, juce::dontSendNotification);
    alignButton.setEnabled (mode == RefABProcessor::Mode::sync && track != nullptr);
    loopButton.setEnabled (track != nullptr);
    loopButton.setToggleState (ss.loopOn.load(), juce::dontSendNotification);

    juce::String s;
    if (track != nullptr)
    {
        s << fmtTime (proc.getPositionSeconds (slot)) << " / " << fmtTime (track->getLengthSeconds());
        if (ss.loopOn.load())
        {
            s << "    loop " << fmtTime (ss.loopStart.load()) << " - " << fmtTime (ss.loopEnd.load());
            const float l = ss.loopLufs.load();
            if (l > -70.0f) s << "  (" << juce::String (l, 1) << " LUFS)";
        }
    }
    posLabel.setText (s, juce::dontSendNotification);
}

void RefABEditor::timerCallback()
{
    spectrum.update();
    waveform.update();

    const auto a = StereoStats::compute (proc.mixScope, statL, statR);
    const auto b = StereoStats::compute (proc.refScope, statL, statR);
    meters.setStats (a, b);
    meters.repaint();

    gonio.update();
    updateTransport();
    abButton.repaint();

    if (proc.getSelectedSlot() != lastSelected)
    {
        lastSelected = proc.getSelectedSlot();
        slotList.repaint();
    }
}

//==============================================================================
static bool isAudioFile (const juce::String& path)
{
    return juce::File (path).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg;m4a;aac;caf;wma");
}

bool RefABEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void RefABEditor::fileDragMove (const juce::StringArray&, int x, int y)
{
    slotList.setDropHighlight (slotList.slotAt (slotList.getLocalPoint (this, juce::Point<int> (x, y))));
}

void RefABEditor::fileDragExit (const juce::StringArray&)
{
    slotList.setDropHighlight (-1);
}

void RefABEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    slotList.setDropHighlight (-1);

    int target = slotList.slotAt (slotList.getLocalPoint (this, juce::Point<int> (x, y)));
    if (target < 0)
    {
        const int sel = proc.getSelectedSlot();
        target = sel;
        if (proc.getSlotInfo (sel).status != RefABProcessor::SlotInfo::Status::empty)
            for (int i = 0; i < RefABProcessor::numSlots; ++i)
                if (proc.getSlotInfo (i).status == RefABProcessor::SlotInfo::Status::empty) { target = i; break; }
    }

    int slot = target;
    bool first = true;
    for (auto& f : files)
    {
        if (! isAudioFile (f)) continue;
        if (! first)
        {
            // further files go to the next empty slots
            int next = -1;
            for (int i = slot + 1; i < RefABProcessor::numSlots; ++i)
                if (proc.getSlotInfo (i).status == RefABProcessor::SlotInfo::Status::empty) { next = i; break; }
            if (next < 0) break;
            slot = next;
        }
        proc.loadSlot (slot, juce::File (f));
        first = false;
    }

    proc.setSelectedSlot (target);
}
