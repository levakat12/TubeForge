#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace tf::ui
{
/** The question asked when a control Auto Match is holding gets moved by hand.

    A question rather than a lock, and the reason is the same one the song-match veil states:
    disabling a control someone is already using is a worse surprise than letting them use it.
    So all three answers are real, and one of them is always "change it anyway".

    Non-blocking. A plug-in that spins a modal loop inside a host is a plug-in that hangs the
    host, so this enters an asynchronous modal state and reports back through `onAnswer`; the
    window owns itself until the callback has run.
*/
class AutoMatchDialog
{
public:
    enum class Answer
    {
        /// Put the analyzer's value back and carry on holding it.
        keepMatched,
        /// Keep what the user just dialled and stop holding this one control.
        changeAnyway,
        /// Stop holding everything. Nothing is reverted.
        turnOff
    };

    /** @param parameterName  The control, named as the host names it.
        @param matchedValue   What the match put on it, formatted as the panel shows it.
        @param source         The song the match came from, or empty.
        @param onAnswer       Called on the message thread with the answer and whether the user
                              asked not to be warned again.
    */
    static void show(juce::Component* parent, const juce::String& parameterName,
                     const juce::String& matchedValue, const juce::String& source,
                     std::function<void(Answer, bool)> onAnswer);
};
} // namespace tf::ui
