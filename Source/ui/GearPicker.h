#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace tf::ui
{
/** One selectable piece of gear.

    `paintFace` rather than a `juce::Image` so the caller decides whether a face is drawn live
    or served from a cache, and so the picker never learns what an amplifier is.
*/
struct GearTile
{
    /// Index into the choice parameter. This *is* the value that gets written.
    int parameterIndex {};
    juce::String name;
    /// One line saying what it sounds like. The whole point of the picker is that a player
    /// choosing between seven unfamiliar names gets told something before they commit.
    juce::String blurb;
    /// Index into `GearCatalogue::categories`, or -1 to pin the tile above the rail.
    int category { -1 };
    std::function<void(juce::Graphics&, juce::Rectangle<float>)> paintFace;
};

/** Everything a picker shows. Built by the page that opens it; the picker keeps a copy.

    Categories are listed even when empty -- see `GearPicker`'s handling of them. A shelf a
    player can see is empty is information; a shelf that silently is not there is not.
*/
struct GearCatalogue
{
    juce::String heading;
    std::vector<juce::String> categories;
    std::vector<GearTile> tiles;

    /** The tile's face box. Defaults suit an amplifier faceplate, which is wide.

        A stompbox is roughly 2:3, so drawing one into a wide box centres it between two large
        empty margins and fits three per row where seven would go. Sizing the tile to the gear
        is the difference between a grid and a list with pictures on it.
    */
    int tileWidth { 220 };
    int tileFaceHeight { 118 };
};

/** The panel that expands out of a `GearChip`.

    Modelled on `CapturePicker`: it fills its parent, owns itself, and deletes itself when it is
    dismissed. The deferred delete is not a style choice -- this dismisses from inside its own
    mouse handler, and JUCE keeps touching a component after the handler returns, so deleting
    synchronously is a use-after-free that survives most of the time.

    Nothing here knows about amplifiers, pedals or parameters beyond the index it writes back.
*/
class GearPicker final : public juce::Component,
                         private juce::Timer
{
public:
    ~GearPicker() override;

    /** Opens a picker over `parent`, growing out of `fromBounds`.

        `fromBounds` is in `parent`'s coordinates and is normally the chip that was clicked.
        `onChosen` is called with the chosen tile's `parameterIndex` after the panel has begun
        collapsing, and is not called at all if the picker is cancelled.
    */
    static void openOver(juce::Component& parent, juce::Rectangle<int> fromBounds,
                         GearCatalogue catalogue, int currentIndex,
                         std::function<void(int)> onChosen);

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;
    /** Announces itself as a dialogue rather than as an anonymous component.

        The tiles are painted, not child components, so there is nothing for a screen reader to
        walk. What it gets instead is this handler for the panel as a whole plus an announcement
        each time the highlight moves -- which is what a user driving the grid from the arrow
        keys actually needs to hear.
    */
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    class Content;

    GearPicker(juce::Rectangle<int> fromBounds, GearCatalogue catalogue, int currentIndex);

    void tick(double timestampSeconds);
    void timerCallback() override;
    void advance(double deltaSeconds);
    void beginCollapse();
    void commit(int parameterIndex);
    [[nodiscard]] juce::Rectangle<float> panelBounds() const;
    [[nodiscard]] juce::Rectangle<float> currentPanelBounds() const;

    GearCatalogue catalogue;
    juce::Rectangle<int> origin;
    std::unique_ptr<Content> content;
    std::function<void(int)> chosen;

    /// 0 is folded into the chip, 1 is the full panel. Eased, not linear -- see `tick`.
    float expansion {};
    bool collapsing {};
    /// Set once the delete is queued, so a second click cannot queue a second one.
    bool dismissing {};
    double lastTimestamp {};
    /// Cleared by the watchdog each time it looks; set by every frame the display link
    /// delivers. See `timerCallback`.
    bool vblankSeen {};
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GearPicker)
};

/** The collapsed control: a face, a name, and a chevron.

    A `juce::Button` rather than a bare component, which is most of this feature's keyboard
    story for free -- focus traversal, a focus outline, and Return or Space to open.

    The chip never caches a selection. Its parameter can move underneath it from a preset
    recall, from a recovered rig, or from host automation, so the owning page calls `refresh`
    from its own tick and the chip re-reads. That is the same route `AmplifierPage` already uses
    to keep the faceplate in step.
*/
class GearChip final : public juce::Button
{
public:
    GearChip(juce::RangedAudioParameter& parameterToDrive, juce::String captionText);

    /// Rebuilt whenever the chip is opened or its parameter moves. Message thread only.
    std::function<GearCatalogue()> buildCatalogue;
    /// Where the panel opens. Defaults to the chip's top-level component if left null.
    juce::Component* overlayHost {};
    /** Called with the chosen index *after* the parameter has been written.

        For work that follows from the choice rather than being the choice: picking a capture
        slot has no model in it yet, and the page that owns the slot is the only thing that
        knows what to do about that. The panel is still collapsing when this runs -- anything
        that opens another overlay must wait for it.
    */
    std::function<void(int)> onChosen;
    /** Width-to-height ratio of the face swatch. Amplifiers are wide; pedals are not.

        A stompbox drawn into a landscape swatch reads as a generic box, which defeats the
        point of drawing it at all.
    */
    float faceAspect { 1.9f };

    /// Re-reads the parameter and repaints if it moved. Call from the owning page's `refresh`.
    void refresh();

    /** Forces the catalogue to be rebuilt on the next paint.

        For everything the chip cannot see by watching its own parameter. The amp chip's faces
        depend on the *instrument* as well as the voicing, and a bass rig wears a different
        cabinet -- without this the chip would keep showing the guitar face until the voicing
        happened to change.
    */
    void invalidate();

    void paintButton(juce::Graphics& graphics, bool isMouseOver, bool isButtonDown) override;

private:
    void clicked() override;
    [[nodiscard]] int parameterIndex() const;
    void rebuild();

    juce::RangedAudioParameter& parameter;
    juce::String caption;
    GearCatalogue catalogue;
    int shownIndex { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GearChip)
};
} // namespace tf::ui
