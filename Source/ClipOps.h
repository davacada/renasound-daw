#pragma once
#include "Model.h"

// =============================================================================
//  Clip editing that goes beyond moving numbers around: reversing and joining
//  (audio is rendered to a new file in the Recordings folder), and the clipboard.
//  Every function takes the project lock itself; call project.beginEdit() first.
// =============================================================================

namespace rs::clipops
{

// Reverses audio clips (a new reversed file) and MIDI clips (notes mirrored). Locked clips are skipped.
// Returns how many clips were reversed.
int reverse (Project&, const std::vector<int>& clipIds);

// Joins the clips into one per track: MIDI notes are merged, audio is mixed into a new file.
// Returns the ids of the joined clips.
std::vector<int> join (Project&, const std::vector<int>& clipIds);

// Copies of clips with the track they came from, positioned relative to the earliest one
struct Clipboard
{
    struct Item { int trackId; int trackIndex; Clip clip; };
    std::vector<Item> items;
    double span = 0;   // beats from the first start to the last end
    bool empty() const { return items.empty(); }
};

Clipboard copy (const Project&, const std::vector<int>& clipIds);
// Pastes at a beat. Clips go back to the tracks they came from; if they all came from one
// track and another track of the same kind is selected, they go there. Returns the new ids.
std::vector<int> paste (Project&, const Clipboard&, double atBeat, int selectedTrackId);

} // namespace rs::clipops
