#include "AutoMatchDialog.h"

#include <memory>
#include <utility>

namespace tf::ui
{
namespace
{
/** The window, subclassed only so it can own the checkbox.

    `AlertWindow::addCustomComponent` borrows rather than owns, so a checkbox created beside the
    window outlives it or dies before it depending on which local goes out of scope first --
    which is exactly the kind of thing that works on the developer's machine. Making the window
    the owner removes the question.
*/
class WarningWindow final : public juce::AlertWindow
{
public:
    WarningWindow(const juce::String& title, const juce::String& message)
        : juce::AlertWindow(title, message, juce::MessageBoxIconType::QuestionIcon)
    {
        suppress.setSize(340, 24);
        addCustomComponent(&suppress);
        // Ordered by how much they cost the user, cheapest first: the default answer keeps the
        // match, and the one that throws the whole thing away is last and has to be aimed at.
        addButton("Keep the matched value", 1, juce::KeyPress(juce::KeyPress::returnKey));
        addButton("Change it anyway", 2, juce::KeyPress(juce::KeyPress::escapeKey));
        addButton("Turn Auto Match off", 3);
    }

    /// Persisted, so the label does not promise less than it does. The Song Match page carries
    /// the way back, because a preference with no way to undo it is a trap rather than a choice.
    juce::ToggleButton suppress { "Don't warn me again" };
};
} // namespace

void AutoMatchDialog::show(juce::Component* parent, const juce::String& parameterName,
                           const juce::String& matchedValue, const juce::String& source,
                           std::function<void(Answer, bool)> onAnswer)
{
    auto message = parameterName + " is currently set by the song match";
    if (source.isNotEmpty()) message += " of " + source;
    message += ".";
    if (matchedValue.isNotEmpty())
        message += "\n\nThe match put it at " + matchedValue + " to get the tone it was scoring "
                   "against.";
    message += "\n\nChanging it by hand moves the sound away from the match, and Auto Match will "
               "not move it back -- this control stops being part of the matched rig. That is a "
               "perfectly reasonable thing to do; it is only worth knowing that the analysis "
               "already had an answer here.";

    auto window = std::make_shared<WarningWindow>("Auto Match is already setting this", message);
    auto* raw = window.get();
    if (parent != nullptr) parent->addChildComponent(*raw);
    raw->setVisible(true);
    raw->centreAroundComponent(parent, raw->getWidth(), raw->getHeight());

    /* `deleteWhenDismissed` is false and the window is kept alive by the shared pointer the
       callback captures, so the checkbox is still readable while the answer is being handled and
       the window goes when the callback does. Letting the modal manager delete it would mean
       reading `suppress` out of an object that is being destroyed. */
    raw->enterModalState(true, juce::ModalCallbackFunction::create(
        [window, callback = std::move(onAnswer)](int result)
        {
            if (! callback) return;
            const auto suppressed = window->suppress.getToggleState();
            switch (result)
            {
                case 2:  callback(Answer::changeAnyway, suppressed); break;
                case 3:  callback(Answer::turnOff, suppressed); break;
                // 1, and also 0 -- a window dismissed without an answer keeps the match, which
                // is the conservative direction: it changes nothing the user has not asked for.
                default: callback(Answer::keepMatched, suppressed); break;
            }
        }), false);
}
} // namespace tf::ui
