#pragma once

#include "PluginProcessor.h"
#include "Components.h"

class RefABEditor : public juce::AudioProcessorEditor,
                    public juce::FileDragAndDropTarget,
                    private juce::Timer
{
public:
    explicit RefABEditor (RefABProcessor&);
    ~RefABEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void updateTransport();
    void toggleLoop();

    RefABProcessor& proc;
    ui::LookAndFeel lnf;

    ui::ABButton abButton;
    ui::SegmentedChoice monitorChoice, bandChoice;
    ui::SlotList slotList;
    ui::SpectrumView spectrum;
    ui::WaveformView waveform;
    ui::MeterPanel meters;
    ui::Goniometer gonio;

    juce::ComboBox matchBox, modeBox;
    juce::Slider trimSlider;
    juce::TextButton resetButton { "Reset meters" }, playButton { "Play" }, loopButton { "Loop" },
                     alignButton { "Align to DAW" }, diffButton { "Difference" };
    juce::Label posLabel, matchLabel { {}, "LOUDNESS MATCH" }, trimLabel { {}, "REF TRIM" };
    int lastSelected = -1;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> matchAttach, modeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> trimAttach;

    juce::TooltipWindow tooltips { this, 700 };

    std::vector<float> statL = std::vector<float> (4096), statR = std::vector<float> (4096);
    juce::Rectangle<int> monitorCaption, bandCaption, titleArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RefABEditor)
};
