#ifndef SOURCE_NDLRCAPTURE_H_
#define SOURCE_NDLRCAPTURE_H_

#ifdef TRACKER_MINI

#include <stdint.h>

// Writes the notes the NDLR panel plays into the current pattern, one pass of the pattern long.
//
// Times are in NDLR pulses (48 per beat, 12 per Tracker step) counted from the first step of the
// pattern. A pulse between two steps goes on the earlier step with a micromove. Each part owns a
// run of tracks: a chord that matches one of the Tracker's Chord FX shapes takes one track (its
// lowest note plus the Chord FX), and other notes take one track each.
namespace ndlrCapture
{
constexpr int kParts = 4;
constexpr int kPulsesPerStep = 12;

struct PartLayout
{
    int8_t firstTrack; // -1 leaves the part out
    uint8_t tracks;
    uint8_t instrument;
};

struct Result
{
    uint16_t notes;   // notes written
    uint16_t dropped; // notes that did not fit on the part's tracks
    uint16_t lastStep; // the last step written, counted from 1, or zero
    uint16_t steps;    // the pattern's length
};

// Clears the parts' tracks in the current pattern, after storing an undo revision. With
// `setLength` the pattern is first set to `steps` long.
void begin(const PartLayout (&layout)[kParts], uint16_t steps, bool setLength);
bool active();

// Notes that start together. Sorted lowest first.
void chord(int part, int32_t pulse, const int16_t* notes, int count, uint8_t velocity);
// One note on the part's track `slot`, for strummed chords and the motifs.
void note(int part, int slot, int32_t pulse, int16_t note, uint8_t velocity);
// Ends the notes the part has open.
void release(int part, int32_t pulse);

Result finish();

// The Chord FX value for these notes (lowest first), or zero when no shape matches.
uint8_t chordFxFor(const int16_t* notes, int count);
} // namespace ndlrCapture

#endif
#endif
