#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"

namespace ui
{
    namespace col
    {
        const juce::Colour bg       { 0xff0d0f13 };
        const juce::Colour panel    { 0xff15181e };
        const juce::Colour panel2   { 0xff1b1f27 };
        const juce::Colour line     { 0xff272c36 };
        const juce::Colour grid     { 0xff20242d };
        const juce::Colour text     { 0xffe2e6ee };
        const juce::Colour dim      { 0xff7c8495 };
        const juce::Colour faint    { 0xff4a5160 };
        const juce::Colour mix      { 0xff4aa3ff };
        const juce::Colour ref      { 0xffff8c42 };
        const juce::Colour good     { 0xff5fd38d };
        const juce::Colour warn     { 0xffffc24a };
        const juce::Colour bad      { 0xffff5a5a };
    }

    inline juce::Font font (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Font mono (float h)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain));
    }

    inline juce::String fmtDb (float v, int decimals = 1)
    {
        if (v <= LoudnessMeter::silence + 0.5f) return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
        return juce::String (v, decimals);
    }

    inline juce::String fmtTime (double s)
    {
        s = juce::jmax (0.0, s);
        const int m = (int) (s / 60.0);
        const double r = s - m * 60.0;
        return juce::String (m) + ":" + juce::String (r, 1).paddedLeft ('0', 4);
    }

    //==============================================================================
    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            setColour (juce::ResizableWindow::backgroundColourId, col::bg);
            setColour (juce::TextButton::buttonColourId, col::panel2);
            setColour (juce::TextButton::buttonOnColourId, col::ref.withAlpha (0.85f));
            setColour (juce::TextButton::textColourOffId, col::text);
            setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            setColour (juce::ComboBox::backgroundColourId, col::panel2);
            setColour (juce::ComboBox::outlineColourId, col::line);
            setColour (juce::ComboBox::textColourId, col::text);
            setColour (juce::ComboBox::arrowColourId, col::dim);
            setColour (juce::PopupMenu::backgroundColourId, col::panel2);
            setColour (juce::PopupMenu::textColourId, col::text);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, col::line);
            setColour (juce::PopupMenu::highlightedTextColourId, col::text);
            setColour (juce::Slider::trackColourId, col::ref.withAlpha (0.7f));
            setColour (juce::Slider::backgroundColourId, col::line);
            setColour (juce::Slider::thumbColourId, col::text);
            setColour (juce::Slider::textBoxTextColourId, col::text);
            setColour (juce::Slider::textBoxBackgroundColourId, col::panel2);
            setColour (juce::Slider::textBoxOutlineColourId, col::line);
            setColour (juce::Label::textColourId, col::text);
            setColour (juce::TooltipWindow::backgroundColourId, col::panel2);
            setColour (juce::TooltipWindow::textColourId, col::text);
            setColour (juce::TooltipWindow::outlineColourId, col::line);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgc,
                                   bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bgc;
            if (! b.isEnabled()) c = c.withMultipliedAlpha (0.4f);
            else if (down) c = c.brighter (0.15f);
            else if (over) c = c.brighter (0.07f);
            g.setColour (c);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (col::line);
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int h) override { return font (juce::jmin (14.0f, h * 0.5f)); }
        juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.0f); }
        juce::Font getPopupMenuFont() override { return font (14.0f); }
        juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }

        void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
        {
            auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
            g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (col::line);
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
            juce::Path p;
            const float cx = (float) w - 12.0f, cy = h * 0.5f;
            p.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
            g.setColour (col::dim);
            g.fillPath (p);
        }
    };

    //==============================================================================
    // A row of buttons bound to a choice parameter
    class SegmentedChoice : public juce::Component
    {
    public:
        SegmentedChoice (juce::RangedAudioParameter& p, juce::StringArray labelsIn, juce::Colour accentIn)
            : labels (std::move (labelsIn)), accent (accentIn),
              attachment (p, [this] (float v) { index = (int) std::round (v); repaint(); })
        {
            attachment.sendInitialUpdate();
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (col::panel2);
            g.fillRoundedRectangle (r, 5.0f);

            const float w = r.getWidth() / labels.size();
            for (int i = 0; i < labels.size(); ++i)
            {
                auto cell = juce::Rectangle<float> (r.getX() + i * w, r.getY(), w, r.getHeight());
                if (i == index)
                {
                    g.setColour (accent.withAlpha (0.9f));
                    g.fillRoundedRectangle (cell.reduced (2.0f), 4.0f);
                }
                else if (i == hover)
                {
                    g.setColour (col::line);
                    g.fillRoundedRectangle (cell.reduced (2.0f), 4.0f);
                }
                g.setColour (i == index ? juce::Colours::black : col::text);
                g.setFont (font (12.5f, i == index));
                g.drawText (labels[i], cell, juce::Justification::centred);
            }
            g.setColour (col::line);
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }

        void mouseMove (const juce::MouseEvent& e) override { setHover (cellAt (e.x)); }
        void mouseExit (const juce::MouseEvent&) override { setHover (-1); }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = cellAt (e.x);
            if (i >= 0)
                attachment.setValueAsCompleteGesture ((float) i);
        }

    private:
        int cellAt (int x) const { return juce::jlimit (0, labels.size() - 1, x * labels.size() / juce::jmax (1, getWidth())); }
        void setHover (int h) { if (h != hover) { hover = h; repaint(); } }

        juce::StringArray labels;
        juce::Colour accent;
        int index = 0, hover = -1;
        juce::ParameterAttachment attachment;
    };

    //==============================================================================
    class ABButton : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        explicit ABButton (juce::RangedAudioParameter& p)
            : attachment (p, [this] (float v) { isB = v > 0.5f; repaint(); })
        {
            attachment.sendInitialUpdate();
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip ("Switch between your mix (A) and the reference (B). Map the 'A/B' parameter to a MIDI key for hands-free switching.");
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (col::panel2);
            g.fillRoundedRectangle (r, 8.0f);

            auto left = r.withWidth (r.getWidth() * 0.5f);
            auto right = left.withX (left.getRight());
            auto active = isB ? right : left;
            const auto c = isB ? col::ref : col::mix;

            g.setColour (c);
            g.fillRoundedRectangle (active.reduced (3.0f), 6.0f);

            auto drawHalf = [&] (juce::Rectangle<float> area, const juce::String& big, const juce::String& small, bool on)
            {
                g.setColour (on ? juce::Colours::black : col::dim);
                g.setFont (font (24.0f, true));
                g.drawText (big, area.withTrimmedBottom (area.getHeight() * 0.3f), juce::Justification::centredBottom);
                g.setFont (font (11.0f, on));
                g.drawText (small, area.withTrimmedTop (area.getHeight() * 0.66f), juce::Justification::centredTop);
            };
            drawHalf (left, "A", "MIX", ! isB);
            drawHalf (right, "B", "REFERENCE", isB);

            g.setColour (col::line);
            g.drawRoundedRectangle (r, 8.0f, 1.0f);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            attachment.setValueAsCompleteGesture (isB ? 0.0f : 1.0f);
        }

    private:
        bool isB = false;
        juce::ParameterAttachment attachment;
    };

    //==============================================================================
    class SlotList : public juce::Component, private juce::ChangeListener
    {
    public:
        explicit SlotList (RefABProcessor& p) : proc (p)
        {
            proc.slotsChanged.addChangeListener (this);
        }

        ~SlotList() override { proc.slotsChanged.removeChangeListener (this); }

        int slotAt (juce::Point<int> pt) const
        {
            for (int i = 0; i < RefABProcessor::numSlots; ++i)
                if (rowBounds (i).contains (pt))
                    return i;
            return -1;
        }

        void setDropHighlight (int slot) { if (slot != dropSlot) { dropSlot = slot; repaint(); } }

        void openChooser (int slot)
        {
            chooser = std::make_unique<juce::FileChooser> ("Load reference track", juce::File(),
                                                           "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.aac");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::canSelectMultipleItems,
                                  [this, slot] (const juce::FileChooser& fc)
                                  {
                                      int s = slot;
                                      for (const auto& f : fc.getResults())
                                      {
                                          if (s >= RefABProcessor::numSlots) break;
                                          proc.loadSlot (s++, f);
                                      }
                                      if (! fc.getResults().isEmpty())
                                          proc.setSelectedSlot (slot);
                                  });
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (col::panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

            g.setColour (col::dim);
            g.setFont (font (11.5f, true));
            g.drawText ("REFERENCES", getLocalBounds().reduced (14, 0).withHeight (34), juce::Justification::centredLeft);
            g.setFont (font (11.0f));
            g.drawText ("LUFS", getLocalBounds().reduced (14, 0).withHeight (34), juce::Justification::centredRight);

            const int sel = proc.getSelectedSlot();

            for (int i = 0; i < RefABProcessor::numSlots; ++i)
            {
                auto r = rowBounds (i).toFloat();
                const auto& info = proc.getSlotInfo (i);
                auto track = proc.getTrack (i);
                const bool isSel = i == sel;

                if (isSel)
                {
                    g.setColour (col::ref.withAlpha (0.14f));
                    g.fillRoundedRectangle (r, 6.0f);
                    g.setColour (col::ref.withAlpha (0.9f));
                    g.fillRoundedRectangle (r.withWidth (3.0f).reduced (0, 6), 1.5f);
                }
                else if (i == hover)
                {
                    g.setColour (col::panel2);
                    g.fillRoundedRectangle (r, 6.0f);
                }

                if (i == dropSlot)
                {
                    g.setColour (col::ref);
                    g.drawRoundedRectangle (r.reduced (1.0f), 6.0f, 1.5f);
                }

                auto inner = r.reduced (12.0f, 0.0f);
                g.setFont (mono (12.0f));
                g.setColour (isSel ? col::ref : col::faint);
                g.drawText (juce::String (i + 1), inner.removeFromLeft (16.0f), juce::Justification::centredLeft);

                juce::String name, right;
                juce::Colour nameCol = col::text;

                switch (info.status)
                {
                    case RefABProcessor::SlotInfo::Status::empty:
                        name = "Drop or click to load";
                        nameCol = col::faint;
                        break;
                    case RefABProcessor::SlotInfo::Status::loading:
                        name = info.file.getFileNameWithoutExtension();
                        right = "loading";
                        nameCol = col::dim;
                        break;
                    case RefABProcessor::SlotInfo::Status::error:
                        name = info.file.getFileNameWithoutExtension();
                        right = info.error;
                        nameCol = col::bad;
                        break;
                    case RefABProcessor::SlotInfo::Status::ready:
                        name = info.file.getFileNameWithoutExtension();
                        if (track != nullptr) right = fmtDb (track->lufs);
                        break;
                }

                if (track != nullptr && info.status == RefABProcessor::SlotInfo::Status::loading)
                    right = fmtDb (track->lufs);

                g.setFont (mono (12.0f));
                g.setColour (info.status == RefABProcessor::SlotInfo::Status::error ? col::bad : col::dim);
                const float rw = info.status == RefABProcessor::SlotInfo::Status::error ? 90.0f : 46.0f;
                g.drawText (right, inner.removeFromRight (rw), juce::Justification::centredRight);

                g.setFont (font (13.0f, isSel));
                g.setColour (nameCol);
                g.drawText (name, inner.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
            }

            auto hint = getLocalBounds().reduced (14, 10);
            hint = hint.removeFromBottom (54);
            g.setColour (col::faint);
            g.setFont (font (11.5f));
            g.drawFittedText ("Drag audio files here (WAV, AIFF, FLAC, MP3). Right-click a slot to replace or remove it.",
                              hint, juce::Justification::bottomLeft, 3);
        }

        void mouseMove (const juce::MouseEvent& e) override { setHover (slotAt (e.getPosition())); }
        void mouseExit (const juce::MouseEvent&) override { setHover (-1); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int s = slotAt (e.getPosition());
            if (s < 0) return;

            const bool empty = proc.getSlotInfo (s).status == RefABProcessor::SlotInfo::Status::empty;

            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu m;
                m.addItem (1, empty ? "Load file..." : "Replace file...");
                m.addItem (2, "Remove", ! empty);
                if (! empty)
                    m.addItem (3, "Show file location");
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                                 [this, s] (int r)
                                 {
                                     if (r == 1) openChooser (s);
                                     if (r == 2) proc.clearSlot (s);
                                     if (r == 3) proc.getSlotInfo (s).file.revealToUser();
                                 });
                return;
            }

            if (empty)
                openChooser (s);
            else
                proc.setSelectedSlot (s);
        }

    private:
        juce::Rectangle<int> rowBounds (int i) const
        {
            return { 6, 36 + i * 34, getWidth() - 12, 32 };
        }

        void setHover (int h) { if (h != hover) { hover = h; repaint(); } }
        void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

        RefABProcessor& proc;
        int hover = -1, dropSlot = -1;
        std::unique_ptr<juce::FileChooser> chooser;
    };

    //==============================================================================
    class SpectrumView : public juce::Component
    {
    public:
        static constexpr int fftOrder = 13;
        static constexpr int fftSize = 1 << fftOrder;
        static constexpr int numPoints = 320;
        static constexpr float minDb = -84.0f, maxDb = 0.0f;

        explicit SpectrumView (RefABProcessor& p)
            : proc (p), fft (fftOrder), window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false)
        {
            fftData.resize ((size_t) fftSize * 2);
            tmpL.resize ((size_t) fftSize);
            tmpR.resize ((size_t) fftSize);
            prefix.resize ((size_t) fftSize / 2 + 2);
            mixDb.assign (numPoints, -120.0f);
            refDb.assign (numPoints, -120.0f);
            diffDb.assign (numPoints, 0.0f);
            for (int i = 0; i < numPoints; ++i)
                freqs.push_back (20.0f * std::pow (1000.0f, (float) i / (numPoints - 1)));
        }

        void setDifferenceMode (bool d) { diffMode = d; repaint(); }
        bool isDifferenceMode() const { return diffMode; }

        void update()
        {
            const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 44100.0;
            const bool refOn = proc.isReferenceAudible();
            if (refOn) refHold = 6; else if (refHold > 0) --refHold;

            analyse (proc.mixScope, mixDb, sr, true);
            if (refHold > 0) analyse (proc.refScope, refDb, sr, true);
            else for (auto& v : refDb) v += (-120.0f - v) * 0.15f;

            // difference, smoothed over ~1/3 octave
            for (int i = 0; i < numPoints; ++i)
            {
                double s = 0; int c = 0;
                for (int j = juce::jmax (0, i - 9); j <= juce::jmin (numPoints - 1, i + 9); ++j)
                    if (mixDb[(size_t) j] > -80.0f && refDb[(size_t) j] > -80.0f)
                    { s += refDb[(size_t) j] - mixDb[(size_t) j]; ++c; }
                const float target = c > 0 ? (float) (s / c) : 0.0f;
                diffDb[(size_t) i] += (target - diffDb[(size_t) i]) * 0.2f;
            }

            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            g.setColour (col::panel);
            g.fillRoundedRectangle (bounds, 8.0f);

            plot = bounds.reduced (12.0f).withTrimmedBottom (14.0f).withTrimmedLeft (22.0f);

            // grid
            g.setFont (font (10.5f));
            for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            {
                const float x = xForFreq (f);
                g.setColour (col::grid);
                g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
                g.setColour (col::faint);
                g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                            juce::Rectangle<float> (x - 20, plot.getBottom() + 1, 40, 13), juce::Justification::centred);
            }

            if (diffMode)
            {
                for (float d : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
                {
                    const float y = yForDiff (d);
                    g.setColour (d == 0.0f ? col::line.brighter (0.2f) : col::grid);
                    g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
                    g.setColour (col::faint);
                    g.drawText ((d > 0 ? "+" : "") + juce::String ((int) d), juce::Rectangle<float> (plot.getX() - 24, y - 7, 20, 14),
                                juce::Justification::centredRight);
                }
                drawDiff (g);
            }
            else
            {
                for (float d = maxDb; d >= minDb; d -= 12.0f)
                {
                    const float y = yForDb (d);
                    g.setColour (col::grid);
                    g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
                    g.setColour (col::faint);
                    g.drawText (juce::String ((int) d), juce::Rectangle<float> (plot.getX() - 24, y - 7, 20, 14),
                                juce::Justification::centredRight);
                }
                drawCurve (g, mixDb, col::mix, 0.20f, 1.6f);
                drawCurve (g, refDb, col::ref, 0.10f, 1.6f);
            }

            // legend
            auto leg = plot.reduced (8.0f).removeFromTop (16.0f);
            g.setFont (font (11.5f, true));
            if (diffMode)
            {
                g.setColour (col::text);
                g.drawText ("REFERENCE minus MIX  (above 0 = reference has more)", leg, juce::Justification::centredRight);
            }
            else
            {
                g.setColour (col::ref);
                g.drawText ("REF", leg.removeFromRight (34.0f), juce::Justification::centredRight);
                g.setColour (col::mix);
                g.drawText ("MIX", leg.removeFromRight (40.0f), juce::Justification::centredRight);
            }

            // hover readout
            if (hoverX >= plot.getX() && hoverX <= plot.getRight())
            {
                const float f = freqForX (hoverX);
                const int i = juce::jlimit (0, numPoints - 1, (int) std::round (std::log (f / 20.0f) / std::log (1000.0f) * (numPoints - 1)));
                g.setColour (col::dim.withAlpha (0.5f));
                g.drawVerticalLine ((int) hoverX, plot.getY(), plot.getBottom());

                juce::String txt = (f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz" : juce::String ((int) f) + " Hz");
                if (diffMode)
                    txt << "    diff " << (diffDb[(size_t) i] >= 0 ? "+" : "") << juce::String (diffDb[(size_t) i], 1) << " dB";
                else
                    txt << "    mix " << juce::String (mixDb[(size_t) i], 1) << "    ref " << juce::String (refDb[(size_t) i], 1);

                auto box = juce::Rectangle<float> (plot.getX() + 8, plot.getY() + 6, 300, 18);
                g.setColour (col::text);
                g.setFont (mono (11.5f));
                g.drawText (txt, box, juce::Justification::centredLeft);
            }
        }

        void mouseMove (const juce::MouseEvent& e) override { hoverX = (float) e.x; }
        void mouseExit (const juce::MouseEvent&) override { hoverX = -1.0f; }

    private:
        void analyse (const ScopeRing& ring, std::vector<float>& out, double sr, bool)
        {
            ring.readLatest (tmpL.data(), tmpR.data(), fftSize);
            for (int i = 0; i < fftSize; ++i)
                fftData[(size_t) i] = 0.5f * (tmpL[(size_t) i] + tmpR[(size_t) i]);
            std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

            window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
            fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

            // prefix sum of power, normalised so that a full-scale sine reads 0 dB
            const int half = fftSize / 2;
            const double norm = 4.0 / fftSize;
            prefix[0] = 0.0;
            for (int k = 0; k <= half; ++k)
            {
                const double a = fftData[(size_t) k] * norm;
                prefix[(size_t) k + 1] = prefix[(size_t) k] + a * a / 1.5; // Hann ENBW
            }

            const double binHz = sr / fftSize;
            const double bw = std::pow (2.0, 1.0 / 12.0); // +/- 1/12 octave => 1/6 octave band

            auto cumAt = [&] (double bin)
            {
                bin = juce::jlimit (0.0, (double) half, bin + 0.5);
                const int i0 = (int) bin;
                const double fr = bin - i0;
                return prefix[(size_t) i0] + (prefix[(size_t) juce::jmin (i0 + 1, half + 1)] - prefix[(size_t) i0]) * fr;
            };

            for (int i = 0; i < numPoints; ++i)
            {
                const double f = freqs[(size_t) i];
                const double p = cumAt (f * bw / binHz) - cumAt (f / bw / binHz);
                const float db = p > 1.0e-14 ? (float) (10.0 * std::log10 (p)) : -140.0f;
                float& v = out[(size_t) i];
                v += (db - v) * (db > v ? 0.55f : 0.14f);
            }
        }

        float xForFreq (float f) const { return plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
        float freqForX (float x) const { return 20.0f * std::pow (1000.0f, (x - plot.getX()) / plot.getWidth()); }
        float yForDb (float d) const { return juce::jmap (juce::jlimit (minDb, maxDb, d), minDb, maxDb, plot.getBottom(), plot.getY()); }
        float yForDiff (float d) const { return juce::jmap (juce::jlimit (-15.0f, 15.0f, d), -15.0f, 15.0f, plot.getBottom(), plot.getY()); }

        void drawCurve (juce::Graphics& g, const std::vector<float>& v, juce::Colour c, float fillAlpha, float thickness)
        {
            juce::Path p;
            for (int i = 0; i < numPoints; ++i)
            {
                const float x = xForFreq (freqs[(size_t) i]);
                const float y = yForDb (v[(size_t) i]);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }

            juce::Path fill (p);
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.lineTo (plot.getX(), plot.getBottom());
            fill.closeSubPath();
            g.setGradientFill (juce::ColourGradient (c.withAlpha (fillAlpha), 0, plot.getY(), c.withAlpha (0.0f), 0, plot.getBottom(), false));
            g.fillPath (fill);

            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved));
        }

        void drawDiff (juce::Graphics& g)
        {
            juce::Path p;
            for (int i = 0; i < numPoints; ++i)
            {
                const float x = xForFreq (freqs[(size_t) i]);
                const float y = yForDiff (diffDb[(size_t) i]);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            const float y0 = yForDiff (0.0f);
            juce::Path fill (p);
            fill.lineTo (plot.getRight(), y0);
            fill.lineTo (plot.getX(), y0);
            fill.closeSubPath();

            g.saveState();
            g.reduceClipRegion (plot.withBottom (y0).toNearestInt());
            g.setColour (col::ref.withAlpha (0.25f));
            g.fillPath (fill);
            g.restoreState();

            g.saveState();
            g.reduceClipRegion (plot.withTop (y0).toNearestInt());
            g.setColour (col::mix.withAlpha (0.25f));
            g.fillPath (fill);
            g.restoreState();

            g.setColour (col::text);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
        }

        RefABProcessor& proc;
        juce::dsp::FFT fft;
        juce::dsp::WindowingFunction<float> window;
        std::vector<float> fftData, tmpL, tmpR, mixDb, refDb, diffDb, freqs;
        std::vector<double> prefix;
        juce::Rectangle<float> plot;
        bool diffMode = false;
        int refHold = 0;
        float hoverX = -1.0f;
    };

    //==============================================================================
    class WaveformView : public juce::Component
    {
    public:
        explicit WaveformView (RefABProcessor& p) : proc (p) {}

        void update()
        {
            auto t = proc.getTrack (proc.getSelectedSlot());
            if (t != track) { track = t; }
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (col::panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

            area = getLocalBounds().toFloat().reduced (12.0f, 10.0f).withTrimmedBottom (12.0f);

            if (track == nullptr)
            {
                g.setColour (col::faint);
                g.setFont (font (13.0f));
                g.drawText ("Load a reference track: drag WAV / AIFF / FLAC / MP3 files onto the plugin",
                            area, juce::Justification::centred);
                return;
            }

            const auto& ss = proc.getSlotSettings (proc.getSelectedSlot());
            const double len = track->getLengthSeconds();
            const float midY = area.getCentreY();
            const float halfH = area.getHeight() * 0.5f;

            // loop region
            const bool loopOn = ss.loopOn.load();
            const double ls = ss.loopStart.load(), le = ss.loopEnd.load();
            if (le > ls)
            {
                auto lr = juce::Rectangle<float>::leftTopRightBottom (xFor (ls, len), area.getY(), xFor (le, len), area.getBottom());
                g.setColour (col::ref.withAlpha (loopOn ? 0.13f : 0.05f));
                g.fillRect (lr);
                g.setColour (col::ref.withAlpha (loopOn ? 0.8f : 0.3f));
                g.fillRect (lr.withWidth (2.0f));
                g.fillRect (lr.withLeft (lr.getRight() - 2.0f));
            }

            // waveform
            const int n = (int) track->wavPeak.size();
            const int w = (int) area.getWidth();
            for (int x = 0; x < w; ++x)
            {
                const int b0 = x * n / w, b1 = juce::jmax (b0 + 1, (x + 1) * n / w);
                float pk = 0, rm = 0;
                for (int b = b0; b < juce::jmin (b1, n); ++b)
                {
                    pk = juce::jmax (pk, track->wavPeak[(size_t) b]);
                    rm = juce::jmax (rm, track->wavRms[(size_t) b]);
                }
                const float px = area.getX() + x;
                g.setColour (col::ref.withAlpha (0.35f));
                g.drawVerticalLine ((int) px, midY - pk * halfH, midY + pk * halfH);
                g.setColour (col::ref.withAlpha (0.8f));
                g.drawVerticalLine ((int) px, midY - rm * halfH, midY + rm * halfH);
            }

            // cue marker
            const float cx = xFor (ss.cue.load(), len);
            g.setColour (col::text.withAlpha (0.8f));
            juce::Path tri;
            tri.addTriangle (cx - 5, area.getY() - 4, cx + 5, area.getY() - 4, cx, area.getY() + 4);
            g.fillPath (tri);
            g.setColour (col::text.withAlpha (0.35f));
            g.drawVerticalLine ((int) cx, area.getY(), area.getBottom());

            // playhead
            const float px = xFor (proc.getRefPositionSeconds(), len);
            g.setColour (proc.isReferenceAudible() ? col::text : col::dim);
            g.fillRect (juce::Rectangle<float> (px - 1.0f, area.getY() - 2.0f, 2.0f, area.getHeight() + 4.0f));

            // time ruler
            g.setFont (font (10.5f));
            g.setColour (col::faint);
            const double step = len > 300 ? 60.0 : len > 120 ? 30.0 : len > 40 ? 10.0 : 5.0;
            for (double t = 0; t < len; t += step)
                g.drawText (fmtTime (t).upToLastOccurrenceOf (".", false, false),
                            juce::Rectangle<float> (xFor (t, len) + 2, area.getBottom() + 1, 40, 12), juce::Justification::centredLeft);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (track == nullptr) return;
            const auto& ss = proc.getSlotSettings (proc.getSelectedSlot());
            const double len = track->getLengthSeconds();
            dragMode = DragMode::none;
            dragStartSec = secFor ((float) e.x, len);

            if (ss.loopEnd.load() > ss.loopStart.load())
            {
                if (std::abs (e.x - xFor (ss.loopStart.load(), len)) < 6) dragMode = DragMode::start;
                else if (std::abs (e.x - xFor (ss.loopEnd.load(), len)) < 6) dragMode = DragMode::end;
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (track == nullptr) return;
            auto& ss = proc.getSlotSettings (proc.getSelectedSlot());
            const double len = track->getLengthSeconds();
            const double t = secFor ((float) e.x, len);

            if (dragMode == DragMode::none && e.getDistanceFromDragStart() > 4)
                dragMode = DragMode::create;

            switch (dragMode)
            {
                case DragMode::create:
                    ss.loopStart = juce::jmin (dragStartSec, t);
                    ss.loopEnd = juce::jmax (dragStartSec, t);
                    ss.loopOn = true;
                    break;
                case DragMode::start:
                    ss.loopStart = juce::jmin (t, ss.loopEnd.load() - 0.1);
                    break;
                case DragMode::end:
                    ss.loopEnd = juce::jmax (t, ss.loopStart.load() + 0.1);
                    break;
                case DragMode::none:
                    break;
            }
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (track == nullptr) return;
            const int slot = proc.getSelectedSlot();
            auto& ss = proc.getSlotSettings (slot);

            if (dragMode == DragMode::none)
            {
                const double t = secFor ((float) e.x, track->getLengthSeconds());
                ss.cue = t;
                proc.seekReference (t);
            }
            else
            {
                proc.updateLoopLoudness (slot);
                if (onLoopChanged) onLoopChanged();
            }
            dragMode = DragMode::none;
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            if (track == nullptr) return;
            const int slot = proc.getSelectedSlot();
            auto& ss = proc.getSlotSettings (slot);
            ss.loopOn = false;
            ss.loopStart = 0.0;
            ss.loopEnd = 0.0;
            proc.updateLoopLoudness (slot);
            if (onLoopChanged) onLoopChanged();
        }

        std::function<void()> onLoopChanged;

    private:
        enum class DragMode { none, create, start, end };

        float xFor (double t, double len) const { return area.getX() + (float) (t / juce::jmax (0.001, len)) * area.getWidth(); }
        double secFor (float x, double len) const { return juce::jlimit (0.0, len, (double) ((x - area.getX()) / area.getWidth()) * len); }

        RefABProcessor& proc;
        std::shared_ptr<const RefTrack> track;
        juce::Rectangle<float> area;
        DragMode dragMode = DragMode::none;
        double dragStartSec = 0.0;
    };

    //==============================================================================
    struct StereoStats
    {
        float correlation = 0.0f, width = 0.0f;
        bool valid = false;

        static StereoStats compute (const ScopeRing& ring, std::vector<float>& l, std::vector<float>& r)
        {
            const int n = (int) l.size();
            ring.readLatest (l.data(), r.data(), n);
            double lr = 0, ll = 0, rr = 0, m2 = 0, s2 = 0;
            for (int i = 0; i < n; ++i)
            {
                const double a = l[(size_t) i], b = r[(size_t) i];
                lr += a * b; ll += a * a; rr += b * b;
                const double m = 0.5 * (a + b), s = 0.5 * (a - b);
                m2 += m * m; s2 += s * s;
            }
            StereoStats st;
            if (ll + rr < 1.0e-7) return st;
            st.valid = true;
            st.correlation = (float) (lr / std::sqrt (juce::jmax (1.0e-20, ll * rr)));
            st.width = (float) (200.0 * s2 / (m2 + s2));
            return st;
        }
    };

    class MeterPanel : public juce::Component
    {
    public:
        explicit MeterPanel (RefABProcessor& p) : proc (p) {}

        void setStats (StereoStats a, StereoStats b) { mixStats = a; refStats = b; }

        void paint (juce::Graphics& g) override
        {
            g.setColour (col::panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

            auto r = getLocalBounds().reduced (14, 10);
            auto head = r.removeFromTop (22);
            g.setFont (font (11.5f, true));
            g.setColour (col::dim);
            g.drawText ("LOUDNESS", head, juce::Justification::centredLeft);

            const float mS = proc.mixMeter.getShortTerm(), rS = proc.refMeter.getShortTerm();
            const bool refOn = proc.isReferenceAudible();

            auto bigRow = r.removeFromTop (62);
            drawBig (g, bigRow.removeFromLeft (bigRow.getWidth() / 2).reduced (0, 2), "MIX", mS, col::mix, true);
            drawBig (g, bigRow.reduced (0, 2), "REF", rS, col::ref, refOn);

            r.removeFromTop (6);
            auto colHead = r.removeFromTop (18);
            g.setFont (font (11.0f, true));
            g.setColour (col::ref);
            g.drawText ("B  REF", colHead.removeFromRight (70), juce::Justification::centredRight);
            g.setColour (col::mix);
            g.drawText ("A  MIX", colHead.removeFromRight (78), juce::Justification::centredRight);

            auto row = [&] (const juce::String& label, const juce::String& unit, const juce::String& a, const juce::String& b, const juce::String& tooltip)
            {
                juce::ignoreUnused (tooltip);
                auto line = r.removeFromTop (21);
                g.setColour (col::grid);
                g.drawHorizontalLine (line.getBottom(), (float) line.getX(), (float) line.getRight());
                g.setFont (font (12.5f));
                g.setColour (col::dim);
                g.drawText (label, line, juce::Justification::centredLeft);
                g.setColour (col::faint);
                g.setFont (font (10.5f));
                g.drawText (unit, line.withTrimmedLeft (86), juce::Justification::centredLeft);
                g.setFont (mono (13.0f));
                g.setColour (refOn ? col::text : col::dim);
                g.drawText (b, line.removeFromRight (70), juce::Justification::centredRight);
                g.setColour (col::text);
                g.drawText (a, line.removeFromRight (78), juce::Justification::centredRight);
            };

            auto& A = proc.mixMeter;
            auto& B = proc.refMeter;
            const float plrA = (A.getIntegrated() > -70.0f && A.getMaxPeak() > -99.0f) ? A.getMaxPeak() - A.getIntegrated() : LoudnessMeter::silence;
            const float plrB = (B.getIntegrated() > -70.0f && B.getMaxPeak() > -99.0f) ? B.getMaxPeak() - B.getIntegrated() : LoudnessMeter::silence;
            const float psrA = (A.getShortTerm() > -70.0f) ? juce::jmax (A.getMaxPeak(), -99.0f) - A.getShortTerm() : LoudnessMeter::silence;
            juce::ignoreUnused (psrA);

            row ("Momentary", "LUFS", fmtDb (A.getMomentary()), fmtDb (B.getMomentary()), {});
            row ("Short-term", "LUFS", fmtDb (A.getShortTerm()), fmtDb (B.getShortTerm()), {});
            row ("Integrated", "LUFS", fmtDb (A.getIntegrated()), fmtDb (B.getIntegrated()), {});
            row ("RMS", "dBFS", fmtDb (A.getRms()), fmtDb (B.getRms()), {});
            row ("Peak max", "dBFS", fmtDb (A.getMaxPeak()), fmtDb (B.getMaxPeak()), {});
            row ("PLR", "LU", fmtDb (plrA), fmtDb (plrB), {});
            row ("Correlation", "", mixStats.valid ? juce::String (mixStats.correlation, 2) : "-",
                 refStats.valid && refOn ? juce::String (refStats.correlation, 2) : "-", {});
            row ("Width", "%", mixStats.valid ? juce::String ((int) std::round (mixStats.width)) : "-",
                 refStats.valid && refOn ? juce::String ((int) std::round (refStats.width)) : "-", {});

            r.removeFromTop (10);
            auto mg = r.removeFromTop (20);
            g.setFont (font (12.5f));
            g.setColour (col::dim);
            g.drawText ("Ref gain", mg, juce::Justification::centredLeft);
            const float gdb = proc.getMatchGainDb();
            g.setFont (mono (13.0f));
            g.setColour (proc.isMatchMeasuring() ? col::warn : col::text);
            g.drawText (proc.isMatchMeasuring() ? "measuring..." : ((gdb >= 0 ? "+" : "") + juce::String (gdb, 1) + " dB"),
                        mg, juce::Justification::centredRight);

            controlsArea = r.removeFromTop (r.getHeight());
        }

        juce::Rectangle<int> getControlsArea() const { return controlsArea; }

    private:
        void drawBig (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label, float v, juce::Colour c, bool on)
        {
            auto rf = r.toFloat().reduced (2.0f, 0.0f);
            g.setColour (col::panel2);
            g.fillRoundedRectangle (rf, 6.0f);

            // bar: -40 .. 0 LUFS
            const float frac = juce::jlimit (0.0f, 1.0f, (v + 40.0f) / 40.0f);
            auto bar = rf.reduced (6.0f).removeFromBottom (5.0f);
            g.setColour (col::line);
            g.fillRoundedRectangle (bar, 2.5f);
            g.setColour (on ? c : c.withAlpha (0.3f));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * frac), 2.5f);

            auto top = rf.reduced (8.0f, 4.0f).withTrimmedBottom (10.0f);
            g.setFont (font (10.5f, true));
            g.setColour (c);
            g.drawText (label, top.removeFromTop (14.0f), juce::Justification::centredLeft);
            g.setFont (mono (24.0f));
            g.setColour (on ? col::text : col::dim);
            g.drawText (fmtDb (v), top, juce::Justification::centredLeft);
        }

        RefABProcessor& proc;
        StereoStats mixStats, refStats;
        juce::Rectangle<int> controlsArea;
    };

    //==============================================================================
    class Goniometer : public juce::Component
    {
    public:
        explicit Goniometer (RefABProcessor& p) : proc (p)
        {
            l.resize (1536); r.resize (1536);
        }

        void update()
        {
            const int s = juce::jmin (getWidth(), getHeight()) - 20;
            if (s <= 10) return;
            if (img.getWidth() != s)
                img = juce::Image (juce::Image::ARGB, s, s, true);

            juce::Graphics g (img);
            g.setColour (col::panel.withAlpha (0.38f));
            g.fillAll();

            draw (g, proc.mixScope, col::mix, s);
            if (proc.isReferenceAudible())
                draw (g, proc.refScope, col::ref, s);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (col::panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
            const int s = img.getWidth();
            auto area = juce::Rectangle<int> (s, s).withCentre (getLocalBounds().getCentre());

            g.setColour (col::grid);
            const auto c = area.getCentre().toFloat();
            const float h = s * 0.5f;
            g.drawLine (c.x - h, c.y + h, c.x + h, c.y - h, 1.0f);
            g.drawLine (c.x - h, c.y - h, c.x + h, c.y + h, 1.0f);
            g.drawVerticalLine ((int) c.x, c.y - h, c.y + h);
            g.drawEllipse (area.toFloat().reduced (1.0f), 1.0f);

            if (img.isValid())
                g.drawImageAt (img, area.getX(), area.getY());

            g.setFont (font (10.5f, true));
            g.setColour (col::faint);
            g.drawText ("M", area.withHeight (14).translated (0, 2), juce::Justification::centred);
            g.drawText ("L", area.withWidth (14).withHeight (14).translated (6, 6), juce::Justification::centred);
            g.drawText ("R", area.withLeft (area.getRight() - 20).withHeight (14).translated (0, 6), juce::Justification::centred);
            g.drawText ("STEREO", getLocalBounds().reduced (12, 8), juce::Justification::topLeft);
        }

    private:
        void draw (juce::Graphics& g, const ScopeRing& ring, juce::Colour c, int s)
        {
            ring.readLatest (l.data(), r.data(), (int) l.size());
            float pk = 1.0e-4f;
            for (size_t i = 0; i < l.size(); ++i)
                pk = juce::jmax (pk, std::abs (l[i]), std::abs (r[i]));

            float& zoom = (c == col::mix) ? zoomMix : zoomRef;
            const float target = juce::jlimit (1.0f, 30.0f, 0.7f / pk);
            zoom += (target - zoom) * 0.08f;

            const float h = s * 0.5f;
            g.setColour (c.withAlpha (0.55f));
            for (size_t i = 0; i < l.size(); ++i)
            {
                const float x = (r[i] - l[i]) * 0.7071f * zoom;
                const float y = (l[i] + r[i]) * 0.7071f * zoom;
                const float px = h + juce::jlimit (-1.0f, 1.0f, x) * h * 0.96f;
                const float py = h - juce::jlimit (-1.0f, 1.0f, y) * h * 0.96f;
                g.fillRect (px, py, 1.3f, 1.3f);
            }
        }

        RefABProcessor& proc;
        juce::Image img;
        std::vector<float> l, r;
        float zoomMix = 1.0f, zoomRef = 1.0f;
    };
}
