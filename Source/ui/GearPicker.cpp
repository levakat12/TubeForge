#include "GearPicker.h"
#include "../TubeForgeTheme.h"

#include <algorithm>
#include <cmath>

namespace theme = tf::theme;

namespace tf::ui
{
namespace
{
/// How often the animation watchdog looks. Well below the display rate, so it costs nothing
/// while frames are arriving and still takes over inside a couple of frames if they stop.
constexpr int watchdogHz = 30;

constexpr int panelInset = 24;
constexpr int panelPadding = 22;
constexpr int headingHeight = 26;
constexpr int railWidth = 168;
constexpr int railRowHeight = 30;

/// Deep enough for a name and three lines of blurb. Two lines truncated every description
/// that actually said something, which defeats the point of showing one.
constexpr int tileTextHeight = 58;
constexpr int tileGap = 16;
constexpr int sectionHeaderHeight = 30;

/// Where the animation spends its time: fast out of the gate, settling into the final shape.
float easeOutCubic(float t) noexcept
{
    const auto remaining = 1.0f - juce::jlimit(0.0f, 1.0f, t);
    return 1.0f - remaining * remaining * remaining;
}

juce::Rectangle<float> interpolate(juce::Rectangle<float> from, juce::Rectangle<float> to,
                                   float amount) noexcept
{
    return { juce::jmap(amount, from.getX(), to.getX()),
             juce::jmap(amount, from.getY(), to.getY()),
             juce::jmap(amount, from.getWidth(), to.getWidth()),
             juce::jmap(amount, from.getHeight(), to.getHeight()) };
}

/** The scrolling half of the panel: every tile, grouped under its category.

    One component rather than a component per tile. A tile has no state a `juce::Component`
    would carry for it -- no focus of its own, no children, no mouse capture -- and seven or
    seventy of them are the same amount of painting either way, while a component each would be
    seventy accessibility handlers and seventy hit-test walks per mouse move.
*/
class TileGrid final : public juce::Component
{
public:
    explicit TileGrid(const GearCatalogue& catalogueToShow)
        : catalogue(catalogueToShow),
          tileWidth(std::max(80, catalogueToShow.tileWidth)),
          tileFaceHeight(std::max(40, catalogueToShow.tileFaceHeight)),
          tileHeight(tileFaceHeight + tileTextHeight)
    {
    }

    /// Called with a tile's parameter index when it is clicked.
    std::function<void(int)> onChosen;
    /// Called when the highlighted tile changes, so the picker can scroll it into view.
    std::function<void(juce::Rectangle<int>)> onHighlightMoved;
    /// Called with what a screen reader should say about the newly highlighted tile.
    std::function<void(juce::String)> onHighlightAnnounced;

    void setCurrentIndex(int index) { currentIndex = index; repaint(); }
    void setHighlight(int slot)
    {
        // A negative slot means "nothing under the pointer", and also means an arrow key ran
        // off the first tile. Both should leave the highlight where it is rather than snapping
        // it to the start.
        if (slot < 0 || slot == highlighted || placements.empty()) return;
        highlighted = juce::jlimit(0, static_cast<int>(placements.size()) - 1, slot);
        if (onHighlightMoved) onHighlightMoved(placements[static_cast<std::size_t>(highlighted)].bounds);
        if (onHighlightAnnounced) onHighlightAnnounced(describe(highlighted));
        repaint();
    }

    /** What a screen reader says about a tile: its name, its shelf, and whether it is the one
        currently loaded. Everything a sighted user gets from the accent border and the section
        heading above it.
    */
    [[nodiscard]] juce::String describe(int slot) const
    {
        if (slot < 0 || slot >= static_cast<int>(placements.size())) return {};
        const auto& tile = catalogue.tiles[placements[static_cast<std::size_t>(slot)].tile];
        auto text = tile.name;
        if (tile.category >= 0 && tile.category < static_cast<int>(catalogue.categories.size()))
            text += ", " + catalogue.categories[static_cast<std::size_t>(tile.category)];
        if (tile.parameterIndex == currentIndex) text += ", currently loaded";
        return text + ". " + tile.blurb;
    }

    [[nodiscard]] int highlight() const noexcept { return highlighted; }

    /// The slot showing a given parameter value, or -1. Used to open with the loaded gear
    /// already highlighted, so the first arrow key steps from it rather than from the start.
    [[nodiscard]] int slotOf(int parameterIndex) const
    {
        for (std::size_t slot = 0; slot < placements.size(); ++slot)
            if (catalogue.tiles[placements[slot].tile].parameterIndex == parameterIndex)
                return static_cast<int>(slot);
        return -1;
    }
    [[nodiscard]] int tileCount() const noexcept { return static_cast<int>(placements.size()); }
    [[nodiscard]] int highlightedParameterIndex() const
    {
        if (highlighted < 0 || highlighted >= static_cast<int>(placements.size())) return -1;
        return catalogue.tiles[placements[static_cast<std::size_t>(highlighted)].tile].parameterIndex;
    }

    /// Lays the grid out for `width` and returns the height it needs.
    int layOutFor(int width)
    {
        placements.clear();
        headers.clear();

        columns = std::max(1, (width + tileGap) / (tileWidth + tileGap));
        auto y = 0;

        const auto addRun = [&](int category, const juce::String& heading)
        {
            std::vector<std::size_t> members;
            for (std::size_t index = 0; index < catalogue.tiles.size(); ++index)
                if (catalogue.tiles[index].category == category) members.push_back(index);

            if (! heading.isEmpty())
            {
                headers.push_back({ heading, y, ! members.empty() });
                y += sectionHeaderHeight;
            }

            // An empty shelf still takes a row, and says so. A category that silently vanished
            // would hide the fact that there is nothing in it yet.
            if (members.empty())
            {
                y += heading.isEmpty() ? 0 : emptyRowHeight;
                return;
            }

            for (std::size_t position = 0; position < members.size(); ++position)
            {
                const auto column = static_cast<int>(position) % columns;
                const auto row = static_cast<int>(position) / columns;
                placements.push_back({ members[position],
                                       { column * (tileWidth + tileGap),
                                         y + row * (tileHeight + tileGap),
                                         tileWidth, tileHeight } });
            }
            const auto rows = (static_cast<int>(members.size()) + columns - 1) / columns;
            y += rows * (tileHeight + tileGap);
        };

        addRun(-1, {});
        for (std::size_t category = 0; category < catalogue.categories.size(); ++category)
            addRun(static_cast<int>(category), catalogue.categories[category]);

        setSize(width, std::max(y, 1));
        return y;
    }

    /// The y offset of a category heading, for the rail to scroll to.
    [[nodiscard]] int categoryTop(int category) const
    {
        return category >= 0 && category < static_cast<int>(headers.size())
            ? headers[static_cast<std::size_t>(category)].y : 0;
    }

    void paint(juce::Graphics& graphics) override
    {
        for (const auto& header : headers)
        {
            graphics.setColour(header.populated ? theme::textSecondary : theme::textTertiary);
            graphics.setFont(theme::font(9.0f, true));
            theme::tracked(graphics, header.text.toUpperCase(),
                           { 0, header.y, getWidth(), sectionHeaderHeight },
                           juce::Justification::centredLeft);
            if (header.populated) continue;
            graphics.setColour(theme::textTertiary);
            graphics.setFont(theme::font(11.0f));
            graphics.drawText("Nothing here yet.",
                              juce::Rectangle<int> { 0, header.y + sectionHeaderHeight,
                                                     getWidth(), emptyRowHeight },
                              juce::Justification::topLeft);
        }

        for (std::size_t slot = 0; slot < placements.size(); ++slot)
        {
            const auto& placement = placements[slot];
            const auto& tile = catalogue.tiles[placement.tile];
            const auto isCurrent = tile.parameterIndex == currentIndex;
            const auto isHighlighted = static_cast<int>(slot) == highlighted;

            auto bounds = placement.bounds;
            // The lift is the whole hover affordance: two pixels, and no colour change, so a
            // grid of faces does not flash as the pointer crosses it.
            if (isHighlighted) bounds = bounds.translated(0, -2);

            auto face = bounds.removeFromTop(tileFaceHeight).toFloat();
            if (isHighlighted)
            {
                graphics.setColour(juce::Colours::black.withAlpha(0.45f));
                graphics.fillRoundedRectangle(face.translated(0.0f, 3.0f).expanded(1.0f), 6.0f);
            }
            if (tile.paintFace) tile.paintFace(graphics, face);
            else
            {
                graphics.setColour(theme::panel);
                graphics.fillRoundedRectangle(face, 5.0f);
            }

            graphics.setColour(isCurrent ? theme::accent
                                         : theme::hairline.withAlpha(isHighlighted ? 0.9f : 0.5f));
            graphics.drawRoundedRectangle(face.reduced(0.5f), 5.0f, isCurrent ? 1.8f : 1.0f);

            auto text = bounds.reduced(2, 4);
            graphics.setColour(isCurrent ? theme::accent : theme::textPrimary);
            graphics.setFont(theme::font(12.0f, true));
            graphics.drawText(tile.name, text.removeFromTop(15), juce::Justification::centredLeft, true);
            graphics.setColour(theme::textTertiary);
            graphics.setFont(theme::font(10.0f));
            graphics.drawFittedText(tile.blurb, text, juce::Justification::topLeft, 3);
        }
    }

    void mouseMove(const juce::MouseEvent& event) override { setHighlight(slotAt(event.getPosition())); }

    void mouseDown(const juce::MouseEvent& event) override
    {
        const auto slot = slotAt(event.getPosition());
        if (slot < 0 || ! onChosen) return;
        onChosen(catalogue.tiles[placements[static_cast<std::size_t>(slot)].tile].parameterIndex);
    }

    /// Screen-reader and keyboard callers need the highlighted tile's rectangle to scroll to.
    [[nodiscard]] juce::Rectangle<int> highlightBounds() const
    {
        if (highlighted < 0 || highlighted >= static_cast<int>(placements.size())) return {};
        return placements[static_cast<std::size_t>(highlighted)].bounds;
    }

    /// The width one tile needs, so the viewport can be sized against it.
    [[nodiscard]] int widthOfOneTile() const noexcept { return tileWidth; }

    /** Tiles per row, so the arrow keys can move by a row rather than by one.

        The width the grid was laid out for, not the length of the first row. Reading it off
        the first row meant a shelf with two entries above a shelf with three made Down move by
        two everywhere -- so the last tile of every wider shelf was unreachable from above.
    */
    [[nodiscard]] int columnsPerRow() const noexcept { return std::max(1, columns); }

private:
    static constexpr int emptyRowHeight = 26;

    struct Placement { std::size_t tile; juce::Rectangle<int> bounds; };
    struct Header { juce::String text; int y; bool populated; };

    [[nodiscard]] int slotAt(juce::Point<int> point) const
    {
        for (std::size_t slot = 0; slot < placements.size(); ++slot)
            if (placements[slot].bounds.contains(point)) return static_cast<int>(slot);
        return -1;
    }

    const GearCatalogue& catalogue;
    const int tileWidth;
    const int tileFaceHeight;
    const int tileHeight;
    std::vector<Placement> placements;
    std::vector<Header> headers;
    /// Set by `layOutFor`; every shelf uses the same column count.
    int columns { 1 };
    int currentIndex { -1 };
    int highlighted { -1 };
};
} // namespace

/** The panel: heading, category rail, and the scrolling grid.

    Laid out for the size the panel will *finish* at, whatever size it currently is. The panel
    grows out of the chip, and re-flowing the grid at every intermediate width would both cost a
    layout per frame and make the text visibly reflow. Growing a window over a fixed layout is
    also what the animation is meant to look like.
*/
class GearPicker::Content final : public juce::Component
{
public:
    explicit Content(const GearCatalogue& catalogueToShow)
        : catalogue(catalogueToShow), grid(catalogueToShow)
    {
        viewport.setViewedComponent(&grid, false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);
        setInterceptsMouseClicks(false, true);
    }

    TileGrid& tiles() noexcept { return grid; }

    void setFinalSize(juce::Rectangle<int> size)
    {
        finalSize = size;
        layOut();
    }

    /// Deliberately ignores the component's own bounds; see the class comment.
    void resized() override { layOut(); }

    void scrollToCategory(int category)
    {
        selectedCategory = category;
        viewport.setViewPosition(0, std::max(0, grid.categoryTop(category)));
        repaint();
    }

    void scrollToShow(juce::Rectangle<int> bounds)
    {
        const auto top = viewport.getViewPositionY();
        const auto height = viewport.getMaximumVisibleHeight();
        if (bounds.getY() < top) viewport.setViewPosition(0, std::max(0, bounds.getY() - tileGap));
        else if (bounds.getBottom() > top + height)
            viewport.setViewPosition(0, bounds.getBottom() - height + tileGap);
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto area = getLocalBounds().toFloat();
        graphics.setColour(theme::shell);
        graphics.fillRoundedRectangle(area, 10.0f);
        graphics.setColour(theme::hairline);
        graphics.drawRoundedRectangle(area.reduced(0.5f), 10.0f, 1.0f);

        auto inner = finalSize.reduced(panelPadding);
        theme::caption(graphics, inner.removeFromTop(headingHeight), catalogue.heading,
                       theme::textSecondary);

        auto rail = inner.removeFromLeft(railWidth);
        for (std::size_t index = 0; index < catalogue.categories.size(); ++index)
        {
            auto row = rail.removeFromTop(railRowHeight);
            const auto selected = static_cast<int>(index) == selectedCategory;
            if (selected)
            {
                graphics.setColour(theme::accentWash);
                graphics.fillRoundedRectangle(row.toFloat().reduced(0.0f, 2.0f), 4.0f);
            }
            graphics.setColour(selected ? theme::accent : theme::textSecondary);
            graphics.setFont(theme::font(12.0f, selected));
            graphics.drawText(catalogue.categories[index], row.reduced(10, 0),
                              juce::Justification::centredLeft, true);
        }

        rail.removeFromTop(10);
        graphics.setColour(theme::textTertiary);
        graphics.setFont(theme::font(10.0f));
        graphics.drawFittedText("Arrow keys move, Return chooses, Escape closes.",
                                rail.removeFromTop(46).reduced(2, 0), juce::Justification::topLeft, 3);
    }

    /// The rail is painted rather than built from buttons, so its clicks are resolved here.
    [[nodiscard]] int categoryAt(juce::Point<int> point) const
    {
        auto inner = finalSize.reduced(panelPadding);
        inner.removeFromTop(headingHeight);
        auto rail = inner.removeFromLeft(railWidth);
        for (std::size_t index = 0; index < catalogue.categories.size(); ++index)
            if (rail.removeFromTop(railRowHeight).contains(point)) return static_cast<int>(index);
        return -1;
    }

private:
    void layOut()
    {
        auto inner = finalSize.reduced(panelPadding);
        inner.removeFromTop(headingHeight);
        inner.removeFromLeft(railWidth + 14);
        viewport.setBounds(inner);
        grid.layOutFor(std::max(grid.widthOfOneTile(), inner.getWidth() - 14));
    }

    const GearCatalogue& catalogue;
    juce::Rectangle<int> finalSize;
    juce::Viewport viewport;
    TileGrid grid;
    int selectedCategory { -1 };
};

GearPicker::GearPicker(juce::Rectangle<int> fromBounds, GearCatalogue catalogueToShow,
                       int currentIndex)
    : catalogue(std::move(catalogueToShow)), origin(fromBounds)
{
    // Opaque to the mouse: this sits over a page whose controls must not be reachable while it
    // is up, and letting a click through to a slider underneath would be worse than modal.
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(true);

    content = std::make_unique<Content>(catalogue);
    addAndMakeVisible(*content);

    auto& grid = content->tiles();
    grid.setCurrentIndex(currentIndex);
    grid.onChosen = [this](int index) { commit(index); };
    grid.onHighlightMoved = [this](juce::Rectangle<int> bounds) { content->scrollToShow(bounds); };
    grid.onHighlightAnnounced = [](juce::String text)
    {
        juce::AccessibilityHandler::postAnnouncement(
            text, juce::AccessibilityHandler::AnnouncementPriority::medium);
    };

    setTitle(catalogue.heading);
    setDescription("A grid of gear grouped by the character of the sound.");
    // Help text as well as description: the two land in different UI Automation properties,
    // and the keyboard instructions are the half a screen-reader user most needs.
    setHelpText("Arrow keys move between entries, Return chooses, Escape closes without "
                "changing anything.");

    vblank = juce::VBlankAttachment(this, [this](double timestamp) { tick(timestamp); });
    startTimerHz(watchdogHz);
}

std::unique_ptr<juce::AccessibilityHandler> GearPicker::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::dialogWindow);
}

GearPicker::~GearPicker() = default;

void GearPicker::openOver(juce::Component& parent, juce::Rectangle<int> fromBounds,
                          GearCatalogue catalogue, int currentIndex,
                          std::function<void(int)> onChosen)
{
    // Raw `new`, as in CapturePicker: the picker owns itself and deletes itself on dismissal,
    // so making every caller hold a pointer it must remember to null would be a dangling one
    // waiting to happen.
    auto* picker = new GearPicker(fromBounds, std::move(catalogue), currentIndex);
    picker->chosen = std::move(onChosen);
    parent.addAndMakeVisible(picker);
    picker->setBounds(parent.getLocalBounds());
    picker->toFront(true);
    picker->grabKeyboardFocus();
    // Opens on what is already loaded, so the first arrow key steps away from the current
    // choice rather than from the top of the list. Done after setBounds, because the grid has
    // no placements until it has been laid out.
    picker->content->tiles().setHighlight(picker->content->tiles().slotOf(currentIndex));
}

juce::Rectangle<float> GearPicker::panelBounds() const
{
    return getLocalBounds().reduced(panelInset).toFloat();
}

juce::Rectangle<float> GearPicker::currentPanelBounds() const
{
    return interpolate(origin.toFloat(), panelBounds(), easeOutCubic(expansion));
}

void GearPicker::resized()
{
    if (content == nullptr) return;
    content->setFinalSize(panelBounds().toNearestInt().withZeroOrigin());
    content->setBounds(currentPanelBounds().toNearestInt());
}

void GearPicker::tick(double timestampSeconds)
{
    vblankSeen = true;

    // Clamped, because a stalled frame -- a resize, a modal file chooser -- would otherwise
    // deliver one enormous delta and make the panel jump instead of animate.
    const auto delta = lastTimestamp > 0.0
        ? juce::jlimit(0.0, 0.1, timestampSeconds - lastTimestamp)
        : 1.0 / 60.0;
    lastTimestamp = timestampSeconds;
    advance(delta);
}

void GearPicker::timerCallback()
{
    // Watchdog, not a second animator. If the display link is delivering, this sees the flag
    // set and does nothing. If it stops -- or never starts, which is what happens when the
    // editor is not on a peer that reports vertical blanks -- the panel would otherwise sit at
    // zero expansion for ever: invisible, full-size, and swallowing every click meant for the
    // page underneath. A worse animation is an infinitely better failure than that.
    if (vblankSeen) { vblankSeen = false; return; }
    advance(1.0 / static_cast<double>(watchdogHz));
}

void GearPicker::advance(double deltaSeconds)
{
    if (dismissing) return;
    // Fully open and not going anywhere: stop repainting. The page behind this is a whole
    // faceplate render, and redrawing it sixty times a second to animate nothing is the most
    // expensive way to do nothing available here.
    if (! collapsing && expansion >= 1.0f) return;

    constexpr double expandSeconds = 0.18;
    constexpr double collapseSeconds = 0.14;
    const auto step = static_cast<float>(deltaSeconds / (collapsing ? collapseSeconds : expandSeconds));
    expansion = collapsing ? std::max(0.0f, expansion - step) : std::min(1.0f, expansion + step);

    content->setBounds(currentPanelBounds().toNearestInt());
    // Text arrives in the last stretch, once the shape is nearly the size it was laid out for.
    content->setAlpha(juce::jlimit(0.0f, 1.0f, (expansion - 0.6f) / 0.4f));
    repaint();

    if (collapsing && expansion <= 0.0f)
    {
        // Deferred, and not optionally: this runs from a vblank callback, and JUCE keeps
        // touching the component after the callback returns. The flag stops a second frame
        // from queueing a second delete.
        dismissing = true;
        stopTimer();
        juce::MessageManager::callAsync(
            [safe = juce::Component::SafePointer<GearPicker>(this)]
            {
                auto* picker = safe.getComponent();
                if (picker == nullptr) return;
                if (auto* parent = picker->getParentComponent()) parent->removeChildComponent(picker);
                delete picker;
            });
    }
}

void GearPicker::paint(juce::Graphics& graphics)
{
    // The page underneath stays visible but is clearly not the thing being used. The scrim
    // arrives with the panel rather than before it, so opening does not flash.
    graphics.fillAll(theme::backdropBottom.withAlpha(0.88f * easeOutCubic(expansion)));
}

void GearPicker::mouseDown(const juce::MouseEvent& event)
{
    const auto inPanel = content->getBounds().contains(event.getPosition());
    if (! inPanel) { beginCollapse(); return; }

    const auto category = content->categoryAt(event.getPosition() - content->getPosition());
    if (category >= 0) content->scrollToCategory(category);
}

bool GearPicker::keyPressed(const juce::KeyPress& key)
{
    auto& grid = content->tiles();
    if (key == juce::KeyPress::escapeKey) { beginCollapse(); return true; }
    if (key == juce::KeyPress::returnKey)
    {
        const auto index = grid.highlightedParameterIndex();
        if (index >= 0) commit(index);
        return true;
    }

    const auto columns = grid.columnsPerRow();
    const auto start = std::max(0, grid.highlight());
    if (key == juce::KeyPress::leftKey)  { grid.setHighlight(start - 1); return true; }
    if (key == juce::KeyPress::rightKey) { grid.setHighlight(start + 1); return true; }
    if (key == juce::KeyPress::upKey)    { grid.setHighlight(start - columns); return true; }
    if (key == juce::KeyPress::downKey)  { grid.setHighlight(start + columns); return true; }
    if (key == juce::KeyPress::homeKey)  { grid.setHighlight(0); return true; }
    if (key == juce::KeyPress::endKey)   { grid.setHighlight(grid.tileCount() - 1); return true; }
    return false;
}

void GearPicker::beginCollapse()
{
    if (collapsing) return;
    collapsing = true;
}

void GearPicker::commit(int parameterIndex)
{
    if (collapsing) return;
    content->tiles().setCurrentIndex(parameterIndex);
    // Written before the collapse rather than after it, so the page behind the panel re-skins
    // while the panel is folding back into the chip. That moment is most of the point.
    if (chosen) chosen(parameterIndex);
    beginCollapse();
}

GearChip::GearChip(juce::RangedAudioParameter& parameterToDrive, juce::String captionText)
    : juce::Button("gear"), parameter(parameterToDrive), caption(std::move(captionText))
{
    setWantsKeyboardFocus(true);
    setTooltip("Browse amplifiers by the character of the sound. "
               "Return or Space opens it; Escape closes without changing anything.");
}

int GearChip::parameterIndex() const
{
    return static_cast<int>(std::lround(parameter.convertFrom0to1(parameter.getValue())));
}

void GearChip::rebuild()
{
    if (buildCatalogue) catalogue = buildCatalogue();
    shownIndex = parameterIndex();
    auto loaded = juce::String {};
    for (const auto& tile : catalogue.tiles)
        if (tile.parameterIndex == shownIndex) loaded = tile.name;

    // Title rather than only name: this is what a screen reader reads out, and a chip that
    // announced "button" alone would be the one control on the page that said nothing about
    // what it currently holds.
    setName(caption + ": " + loaded);
    setTitle(caption + ", " + loaded);
    setDescription("Opens a grid of gear grouped by the character of the sound.");
    setHelpText(getTooltip());
    repaint();
}

void GearChip::refresh()
{
    if (parameterIndex() != shownIndex) rebuild();
}

void GearChip::invalidate()
{
    shownIndex = -1;
    repaint();
}

void GearChip::paintButton(juce::Graphics& graphics, bool isMouseOver, bool isButtonDown)
{
    if (shownIndex < 0) rebuild();

    const GearTile* current = nullptr;
    for (const auto& tile : catalogue.tiles)
        if (tile.parameterIndex == shownIndex) current = &tile;

    auto area = getLocalBounds().toFloat().reduced(0.5f);
    theme::glass(graphics, area, 6.0f, isMouseOver || isButtonDown);

    auto inner = area.reduced(5.0f, 4.0f);
    auto face = inner.removeFromLeft(inner.getHeight() * faceAspect);
    if (current != nullptr && current->paintFace) current->paintFace(graphics, face);
    graphics.setColour(theme::hairline);
    graphics.drawRoundedRectangle(face.reduced(0.5f), 3.0f, 1.0f);

    auto chevron = inner.removeFromRight(16.0f);
    inner.removeFromLeft(9.0f);
    graphics.setColour(theme::textPrimary);
    graphics.setFont(theme::font(12.0f, true));
    graphics.drawText(current != nullptr ? current->name : juce::String("--"),
                      inner.toNearestInt(), juce::Justification::centredLeft, true);

    // A chevron drawn rather than a glyph asset, like everything else in the shell.
    const auto centre = chevron.getCentre();
    juce::Path arrow;
    arrow.startNewSubPath(centre.x - 3.5f, centre.y - 2.0f);
    arrow.lineTo(centre.x, centre.y + 2.0f);
    arrow.lineTo(centre.x + 3.5f, centre.y - 2.0f);
    graphics.setColour(theme::textSecondary);
    graphics.strokePath(arrow, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

    if (hasKeyboardFocus(false))
    {
        graphics.setColour(theme::accent.withAlpha(0.75f));
        graphics.drawRoundedRectangle(area.reduced(1.0f), 6.0f, 1.4f);
    }
}

void GearChip::clicked()
{
    auto* host = overlayHost != nullptr ? overlayHost : getTopLevelComponent();
    if (host == nullptr) return;

    rebuild();
    const auto from = host->getLocalArea(this, getLocalBounds());
    GearPicker::openOver(*host, from, catalogue, shownIndex,
                         [safe = juce::Component::SafePointer<GearChip>(this)](int index)
                         {
                             auto* chip = safe.getComponent();
                             if (chip == nullptr) return;
                             // The gesture pair is not decoration: a host recording automation
                             // needs the touch boundaries, and a choice written without them
                             // shows up as an un-writable lane in several DAWs.
                             auto& target = chip->parameter;
                             target.beginChangeGesture();
                             target.setValueNotifyingHost(target.convertTo0to1(static_cast<float>(index)));
                             target.endChangeGesture();
                             chip->rebuild();
                             if (chip->onChosen) chip->onChosen(index);
                         });
}
} // namespace tf::ui
