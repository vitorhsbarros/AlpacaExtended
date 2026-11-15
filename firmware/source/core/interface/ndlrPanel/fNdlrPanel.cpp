#ifdef TRACKER_MINI

#include "ndlrPanel.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "core/interfaceMenuPopup.h"
#include "core/songTimer.h"

#include "NdlrCapture.h"
#include "NdlrRecorder.h"
#include "Platform.h"
#include "fileManager.h"
#include "Synth.h"
#include "cr_section_macros.h"
#include "interruptsControl.h"
#include "mtMidi.h"
#include "mtSequencer.h"
#include "ndlrPanelControl.h"

// This build adds, from the NDLR manual: the Pad, Drone, T-K-M and Motif sub screens, forty
// pattern and rhythm slots with rests and ties, the Pattern Editor, Rhythm Editor and Chord
// Sequencer (Shift + Menu), accent patterns, tap tempo on the panel's SHIFT, and motifs that
// carry on from where they were paused. See NDLR11_NOTES.md.

cNdlrPanel ndlrPanel;
static cNdlrPanel* const P = &ndlrPanel;

//==============================================================================================
// Synth parts 1-4 listen on virtual MIDI channels 1-4.
//==============================================================================================
static constexpr uint8_t kChPad = 1;
static constexpr uint8_t kChDrone = 2;
static constexpr uint8_t kChMotif1 = 3;
static constexpr uint8_t kVelPad = 80;
static constexpr uint8_t kVelDrone = 90;
static constexpr uint8_t kVelMotif = 100;
static constexpr uint32_t kPulsesPerBeat = 48;
static constexpr uint32_t kPulsesPerBar = 4 * kPulsesPerBeat;
static constexpr uint32_t kMinuteMs = 60000;
static constexpr uint8_t kMaxPadNotes = 8;

static constexpr const char* kStrumNames[] = {
    "Off", "1/32", "1/16", "1/8T", "3 + 1/8T", "1/8", "3 + 1/8"
};
static constexpr uint8_t kStrumPulses[] = { 0, 6, 12, 16, 16, 24, 24 };
static constexpr const char* kSpreadNames[] = { "Close", "Open", "Low Root", "Root + 5th" };
static constexpr const char* kDroneNotesNames[] = { "Root", "Root + Oct", "Root + 5th", "Root 5th Oct" };
static constexpr const char* kTriggerNames[] = {
    "Chord", "Chord 1/4", "Chord 1/2", "Chord 1/1", "Key", "Key 1/4", "Key 1/2", "Key 1/1"
};
static constexpr const char* kVariationNames[] = { "Forward", "Backward", "Ping-Pong", "Ping-Pong 2", "Odd / Even", "Random" };
static constexpr const char* kVariationShort[] = { "Fwd", "Back", "Ping", "Ping 2", "Odd/Ev", "Random" };
static constexpr const char* kDivisionNames[] = { "1/1", "1/2", "1/3", "1/4", "1/6", "1/8" };
static constexpr uint8_t kDivisionPulses[] = { 192, 96, 64, 48, 32, 24 };
// A fixed velocity, the rhythm's own, the rhythm's with some chance, then seven patterns over the
// eighth notes of the bar, where o is a high velocity and . a low one.
static constexpr const char* kAccentNames[] = { "Fixed",    "Rhythm",   "Humanize", "o...o...", "o.o.o.o.",
                                                "o.......", "o..o..o.", "oo..o..o", ".o.o.o.o", "o..o.o.." };
static constexpr uint8_t kVelAccentHigh = 118;
static constexpr uint8_t kVelAccentLow = 88; // 75% of high
static constexpr const char* kQuantiseNames[] = { "Off", "Next Beat", "Next Bar" };
static constexpr const char* kQuantiseShort[] = { "Q:Off", "Q:Beat", "Q:Bar" };
static constexpr const char* kClockNames[] = { "Tracker", "Internal" };
static constexpr const char* kClockShort[] = { "CLK:Trk", "CLK:Int" };
static constexpr uint8_t kAllParts = 0x0f;

// Each part plays on its own synth (0) or on an external MIDI channel (1-16), through the
// Tracker's MIDI out (Config > MIDI > Notes out picks USB, the jack, or both).
static constexpr uint8_t kMidiChannels = 16;

// Capture writes the parts into the current pattern for one pass of it. Each part gets its own
// run of tracks from CAP TRK on: the Pad as many as PAD TRKS, the Drone two, each motif one.
static constexpr const char* kCaptureNames[] = { "All parts", "Pad", "Drone", "Motif 1", "Motif 2", "Motifs", "Pad + Drone" };
static constexpr uint8_t kCaptureParts[] = { 0x0f, 0x01, 0x02, 0x04, 0x08, 0x0c, 0x03 };
static constexpr uint8_t kDroneTracks = 2;
static constexpr uint8_t kMaxPadTracks = 8;
static constexpr uint8_t kPulsesPerStep = kPulsesPerBeat / 4;
enum enCaptureState : uint8_t
{
    capOff,
    capArmed,   // waits for the pattern to start again with a captured part playing
    capRunning, // writing until the end of the pattern
};

// Redraw the Motif sub screen and the editors on every motif step so they show the note being
// played. Set this to false if note timing becomes uneven while those screens are open.
static constexpr bool kShowPlayhead = true;

//==============================================================================================
// Patterns and rhythms
//==============================================================================================
// A pattern is sixteen note values. CHORD values count up the notes of the current chord. SCALE
// values count up the key's scale and CHROMATIC values count semitones, both from the key root,
// so those two do not move with the chord. Slots 1-20 are presets and 21-40 are the user's.
enum enPatternType : uint8_t
{
    ptChord,
    ptScale,
    ptChromatic,
    ptCount
};
static constexpr const char* kPatternTypeNames[ptCount] = { "CHORD", "SCALE", "CHROM" };
static constexpr uint8_t kPatternTypeMax[ptCount] = { 20, 40, 60 };
static constexpr uint8_t kPatternNotes = 16;
static constexpr uint8_t kRhythmBeats = 32;
static constexpr uint8_t kPresetCount = 20;
static constexpr uint8_t kSlotCount = 40;

struct MotifPattern
{
    uint8_t type;
    uint8_t autoLength; // length when Pattern Length is Auto; zero means twice the chord size
    uint8_t notes[kPatternNotes];
};

struct PatternPreset
{
    const char* name;
    uint8_t type;
    uint8_t autoLength;
    uint8_t count; // the notes repeat after this many to fill all sixteen
    uint8_t notes[kPatternNotes];
};
static constexpr PatternPreset kPatternPresets[kPresetCount] = {
    { "Arpeggio", ptChord, 0, 16, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } },
    { "Broken", ptChord, 8, 8, { 1, 3, 2, 4, 3, 5, 4, 2 } },
    { "Pendulum", ptChord, 8, 8, { 1, 3, 5, 3, 2, 4, 6, 4 } },
    { "Bass Jump", ptChord, 8, 8, { 1, 1, 4, 1, 3, 1, 5, 3 } },
    { "Scale Up", ptScale, 8, 8, { 1, 2, 3, 4, 5, 6, 7, 8 } },
    { "Scale Skip", ptScale, 8, 8, { 1, 3, 5, 7, 8, 6, 4, 2 } },
    { "Answer", ptChord, 8, 8, { 1, 2, 3, 5, 4, 3, 2, 1 } },
    { "Cascade", ptChord, 16, 16, { 1, 3, 4, 6, 2, 4, 5, 7, 3, 5, 6, 8, 7, 5, 4, 2 } },
    { "Up Down", ptChord, 10, 10, { 1, 2, 3, 4, 5, 6, 5, 4, 3, 2 } },
    { "Pedal", ptChord, 8, 8, { 1, 2, 1, 3, 1, 4, 1, 5 } },
    { "Thirds", ptScale, 8, 8, { 1, 3, 2, 4, 3, 5, 4, 6 } },
    { "Scale Down", ptScale, 8, 8, { 8, 7, 6, 5, 4, 3, 2, 1 } },
    { "Zigzag", ptChord, 8, 8, { 1, 4, 2, 5, 3, 6, 4, 7 } },
    { "Wide", ptChord, 8, 8, { 1, 5, 2, 6, 3, 7, 4, 8 } },
    { "Trill", ptScale, 8, 8, { 1, 2, 1, 2, 3, 2, 3, 4 } },
    { "Climb", ptChord, 16, 16, { 1, 2, 3, 2, 3, 4, 3, 4, 5, 4, 5, 6, 5, 6, 7, 6 } },
    { "Chromatic", ptChromatic, 12, 12, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 } },
    { "Dim Arp", ptChromatic, 8, 8, { 1, 4, 7, 10, 13, 10, 7, 4 } },
    { "Fifths", ptScale, 8, 8, { 1, 5, 2, 6, 3, 7, 4, 8 } },
    { "Oct Jump", ptChromatic, 8, 8, { 1, 13, 1, 13, 8, 20, 8, 20 } },
};

// A rhythm is thirty-two beats. A beat is a velocity (1-127), a rest, or a tie that lets the
// last note carry on. Slots 1-20 are presets and 21-40 are the user's.
static constexpr int8_t kRest = 0;
static constexpr int8_t kTie = -1;
struct RhythmPreset
{
    const char* name;
    uint8_t count; // the beats repeat after this many to fill all thirty-two
    int8_t steps[16];
};
static constexpr RhythmPreset kRhythmPresets[kPresetCount] = {
    { "Straight", 1, { 100 } },
    { "Offbeat", 4, { 0, 108, 0, 90 } },
    { "Syncopate", 8, { 115, 0, 84, 102, 0, 97, 0, 86 } },
    { "Tresillo", 8, { 114, 0, 0, 94, 0, 0, 103, 0 } },
    { "Gallop", 4, { 116, 82, 100, 0 } },
    { "Pulse", 8, { 118, 0, 0, 0, 95, 0, 0, 0 } },
    { "Stutter", 8, { 112, 84, 92, 0, 0, 103, 92, 0 } },
    { "Sparse", 8, { 113, 0, 0, 0, 0, 86, 0, 0 } },
    { "Long Short", 4, { 110, kTie, 90, 100 } },
    { "Dotted", 8, { 112, kTie, kTie, 96, kTie, kTie, 104, kTie } },
    { "Halves", 8, { 110, kTie, kTie, kTie, 98, kTie, kTie, kTie } },
    { "Push", 8, { 0, 100, 112, kTie, 0, 96, 108, kTie } },
    { "Lilt", 8, { 112, kTie, 88, 104, kTie, 84, 108, 92 } },
    { "Backbeat", 8, { 90, 0, 112, 0, 90, 84, 112, 0 } },
    { "Clave", 16, { 114, 0, 0, 100, 0, 0, 108, 0, 0, 0, 104, 0, 112, 0, 0, 0 } },
    { "Bossa", 16, { 114, 0, 0, 98, 0, 0, 106, 0, 0, 0, 102, 0, 0, 108, 0, 0 } },
    { "Rolling", 8, { 112, 90, kTie, 96, 84, kTie, 104, 88 } },
    { "Swell", 8, { 60, 70, 80, 90, 100, 110, 120, 127 } },
    { "Held", 8, { 110, kTie, kTie, kTie, kTie, kTie, kTie, kTie } },
    { "Hocket", 8, { 108, 0, 0, 96, 0, 104, 0, 0 } },
};

// The user's slots are kept until power-off; they are not written to the SD card.
static MotifPattern s_userPatterns[kSlotCount - kPresetCount];
static int8_t s_userRhythms[kSlotCount - kPresetCount][kRhythmBeats];

struct MotifSettings
{
    uint8_t octave;
    uint8_t patternLength; // zero uses the pattern's own length
    uint8_t variation;
    uint8_t pattern; // slot the working pattern was loaded from
    uint8_t division;
    uint8_t rhythmLength;
    uint8_t gate;
    uint8_t accent;
    uint8_t rhythm; // slot the working rhythm was loaded from
    MotifPattern patt;         // the working pattern, which the Pattern Editor changes
    int8_t rhy[kRhythmBeats];  // the working rhythm, which the Rhythm Editor changes
};

struct MotifPlayback
{
    uint32_t nextStep;
    uint32_t noteEnd;
    uint32_t patternStep;
    uint8_t rhythmStep;
    int16_t note;
    uint8_t velocity;
    bool started;
    int8_t pos;  // pattern note last played, for the sub screen
    int8_t beat; // rhythm beat last reached, for the sub screen
};

//==============================================================================================
// Chord sequences
//==============================================================================================
// Three sections of six steps and a song of eight slots. A step with no duration is skipped.
struct SeqStep
{
    uint8_t dur; // in eighth notes, 0-63
    uint8_t key;
    uint8_t mode;
    uint8_t degree;
    uint8_t type;
};
struct SeqSlot
{
    uint8_t section; // 0-2 for A, B, C
    uint8_t repeats; // 0-8, zero leaves the slot out
};
struct ChordSequence
{
    SeqStep sections[3][6];
    SeqSlot song[8];
};
static constexpr uint8_t kSequenceCount = 6; // 0 is always blank, 1-5 can be saved over
static ChordSequence s_sequences[kSequenceCount];
static bool s_storeReady = false;

//==============================================================================================
// Music tables
//==============================================================================================
struct strMode
{
    const char* name;
    uint8_t n; // notes per octave
    uint8_t iv[7];
};

// The NDLR also has Tonic 2nds/3rds/4ths/6ths; their note sets are not in the manual,
// so they are left out of this build.
static const strMode kModes[] = {
    { "MAJOR", 7, { 0, 2, 4, 5, 7, 9, 11 } },      { "DORIAN", 7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "PHRYGIAN", 7, { 0, 1, 3, 5, 7, 8, 10 } },   { "LYDIAN", 7, { 0, 2, 4, 6, 7, 9, 11 } },
    { "MIXOLYDIAN", 7, { 0, 2, 4, 5, 7, 9, 10 } }, { "MINOR", 7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "LOCRIAN", 7, { 0, 1, 3, 5, 6, 8, 10 } },    { "GYPSY MIN", 7, { 0, 2, 3, 6, 7, 8, 11 } },
    { "HARM MIN", 7, { 0, 2, 3, 5, 7, 8, 11 } },   { "MIN PENTA", 5, { 0, 3, 5, 7, 10, 0, 0 } },
    { "WHOLE TONE", 6, { 0, 2, 4, 6, 8, 10, 0 } },
};
static constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);

// Circle of fifths, as the NDLR steps through keys
static const char* const kKeyName[12] = { "C", "G", "D", "A", "E", "B", "F#", "Db", "Ab", "Eb", "Bb", "F" };
static const uint8_t kKeySemi[12] = { 0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5 };
static const char* const kNoteName[12] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
static const char* const kRomanUpper[7] = { "I", "II", "III", "IV", "V", "VI", "VII" };
static const char* const kRomanLower[7] = { "i", "ii", "iii", "iv", "v", "vi", "vii" };

// Chord types in chord-button order I..VII (as printed on the NDLR with Shift)
enum enChordType : uint8_t
{
    ctTriad,
    ct7th,
    ctSus2,
    ctAlt2,
    ctAlt1,
    ctSus4,
    ct6th,
    ctCount
};
static const char* const kTypeName[ctCount] = { "triad", "7th", "sus2", "alt2", "alt1", "sus4", "6th" };

// alt1 / alt2 resolve to a fixed type per chord degree (NDLR manual, p.13)
static const uint8_t kAlt1[7] = { ctTriad, ct7th, ctTriad, ctTriad, ct7th, ct7th, ct6th };
static const uint8_t kAlt2[7] = { ct7th, ct7th, ctTriad, ct7th, ct7th, ct6th, ct7th };

// Scale-step offsets stacked on the chord root
struct strShape
{
    uint8_t n;
    uint8_t step[4];
};
static const strShape kShape[ctCount] = {
    { 3, { 0, 2, 4, 0 } }, // triad
    { 4, { 0, 2, 4, 6 } }, // 7th
    { 3, { 0, 1, 4, 0 } }, // sus2
    { 4, { 0, 2, 4, 6 } }, // alt2 (replaced by its resolved type)
    { 3, { 0, 2, 4, 0 } }, // alt1 (replaced by its resolved type)
    { 3, { 0, 3, 4, 0 } }, // sus4
    { 4, { 0, 2, 4, 5 } }, // 6th
};

//==============================================================================================
// State
//==============================================================================================
enum enInfoMode : uint8_t
{
    imNormal,
    imShift,
    imMenu,
    imEditor,
    imView,
    imBoot,
};

struct strInfoRow
{
    uint8_t mode;
    const char* text;
};
struct strInfo
{
    const char* name;
    uint8_t rowCount;
    strInfoRow rows[4];
    const char* desc;
};

struct strState
{
    uint8_t sel;
    uint8_t shiftLatched;
    uint8_t previousShift;
    bool middleHandled;
    uint8_t menuMode;
    uint8_t partOn[4];
    uint8_t modRing[4];
    uint8_t motif; // 1 or 2
    uint8_t activeEnc;
    uint8_t encLabel[8];
    uint8_t degree;
    uint8_t type;        // chord type 0..6
    int8_t pendingType;  // -1 none
    uint16_t tempo;
    uint8_t key;         // index into circle of fifths
    uint8_t mode;
    uint8_t padOct;      // 1..6
    uint8_t droneOct;    // 1..4
    uint8_t padRange; // zero uses the chord's note count
    uint8_t padStrum;
    uint8_t padSpread;
    uint8_t droneNotesMode;
    uint8_t droneTrigger;
    uint8_t motifOffset;
    uint8_t quantise;
    uint8_t pendingPartToggles;
    uint8_t queuedChord; // 1..7, zero means none
    uint8_t queuedChordType;
    uint32_t launchPulse;
    uint32_t barPulse;
    uint32_t trackerRun;
    uint32_t trackerPulse;
    bool internalClock;
    bool trackerClock;
    uint16_t trackerTempo;
    MotifSettings motifSettings[2];
    MotifPlayback motifPlayback[2];
    uint8_t dirty;
    uint32_t lastRefresh;
    uint32_t toastUntil;
    char toast[32];
    uint32_t lastTickUpdate;
    uint32_t tickPhase;
    uint32_t pulse;
    uint32_t random;
    uint32_t padNext;
    uint32_t droneNext;
    int16_t padNotes[kMaxPadNotes];
    int16_t droneNotes[3];
    uint8_t padCount;
    uint8_t padPlayed;
    uint8_t droneCount;
    uint8_t voices[4];
    uint8_t synthChanges;
    bool recordToast;

    uint8_t editor;  // dial whose editor is open in Editor mode
    bool resync;     // the next launch restarts both motifs together
    uint8_t peStep;  // Pattern Editor: step 1-16, zero selects the note type
    uint8_t peSave;  // Pattern Editor: slot to save to, 20-39
    uint8_t reBeat;  // Rhythm Editor: beat 1-32
    uint8_t reSave;  // Rhythm Editor: slot to save to, 20-39
    uint32_t taps[4]; // times of the last presses on SHIFT, for tap tempo
    uint8_t tapCount;

    ChordSequence seq; // the sequence being edited and played
    uint8_t seqLoad;
    uint8_t seqSave;
    uint8_t seqStep;  // 0-17
    uint8_t seqAttr;  // 0 duration, 1 key, 2 mode, 3 degree, 4 type
    uint8_t seqField; // song slot * 2, +1 for its repeats
    bool seqArmed;
    bool seqRunning;
    uint32_t seqArmPulse; // when the sequence was armed
    uint32_t playStart;   // when the parts last started from silence
    int16_t seqIndex;
    uint32_t seqNext;
    int8_t seqPlaySection;
    int8_t seqPlayStep;
    int8_t seqPlaySlot;

    uint8_t partOut[4];   // 0 plays the part's synth, 1-16 a MIDI channel
    uint8_t capParts;     // index into kCaptureParts
    uint8_t capTrack;     // first track, 0-15
    uint8_t padTracks;    // tracks for the Pad, 1-8
    bool strumKeep;       // a strummed Pad keeps its timing, one note per track
    uint8_t capState;
    uint8_t capPattern;   // the pattern being written
    uint32_t capStart;    // the pulse of the pattern's first step
    uint32_t capLength;   // the pattern's length in pulses
    uint32_t capLastPos;  // where in the pattern the previous frame was, to see it wrap
    uint32_t trackerPos;  // the Tracker's position in pulses from its first step, with s.pulse
};

static strState s;
__NOINIT(EXTERNAL_RAM) static strNdlrPanelView s_view;

static uint32_t nowMs()
{
    return P->nowMs();
}

static bool shiftOn()
{
    return s.shiftLatched || cNdlrPanel::buttons->isButtonPressed(interfaceButtonShift);
}

static void toast(const char* msg, bool recordingMessage = false)
{
    s.recordToast = recordingMessage;
    snprintf(s.toast, sizeof(s.toast), "%s", msg);
    s.toastUntil = nowMs() + 1500;
    s.dirty = 1;
}

//==============================================================================================
// Sound
//==============================================================================================
static bool external(int part)
{
    return s.partOut[part] != 0;
}

// `channel` is the part's synth, 1-4. A part sent out plays on its MIDI channel instead.
static void synthNote(bool on, int note, uint8_t velocity, uint8_t channel)
{
    if (note < 0 || note > 127)
    {
        return;
    }
    const int part = channel - 1;
    DISABLE_AUDIO_INTERRUPTS();
    if (external(part))
    {
        if (on)
            sendMidiNoteOn(static_cast<uint8_t>(note), velocity, s.partOut[part], false);
        else
            sendMidiNoteOff(static_cast<uint8_t>(note), 0, s.partOut[part], false);
    }
    else
        sendNdlrSynthNote(on, static_cast<uint8_t>(note), velocity, channel);
    ENABLE_AUDIO_INTERRUPTS();
}

// Voices are only limited on the internal synths.
static int partVoices(int part)
{
    return external(part) ? kMaxPadNotes : s.voices[part];
}

static void wantVoices(int part, int wanted)
{
    P->reserveVoices(part, external(part) ? 0 : wanted);
}

// Capture: the pulse from the pattern's first step, and whether the part is being written.
static bool capturing(int part)
{
    return s.capState == capRunning && (kCaptureParts[s.capParts] >> part & 1);
}

static int32_t capturePulse(uint32_t pulse)
{
    return static_cast<int32_t>(pulse - s.capStart);
}

static uint8_t resolvedType()
{
    if (s.type == ctAlt1)
        return kAlt1[s.degree];
    if (s.type == ctAlt2)
        return kAlt2[s.degree];
    return s.type;
}

// MIDI note for scale step `stepIdx` (may exceed one octave) in the current key/mode
static int noteForStep(int stepIdx, int baseOctave)
{
    const strMode& m = kModes[s.mode];
    const int oct = stepIdx / m.n;
    const int idx = stepIdx % m.n;
    return 12 * (baseOctave + 1) + kKeySemi[s.key] + m.iv[idx] + 12 * oct;
}

static int buildChord(int baseOctave, int16_t* out, int maxNotes)
{
    const strShape& sh = kShape[resolvedType()];
    int n = 0;
    for (int i = 0; i < sh.n && n < maxNotes; i++)
    {
        out[n++] = (int16_t)noteForStep(s.degree + sh.step[i], baseOctave);
    }
    return n;
}

static bool due(uint32_t clock, uint32_t deadline)
{
    return static_cast<int32_t>(clock - deadline) >= 0;
}

static void queueTrackerRestart();
static bool hasQueuedLaunch();
static void applyLaunch(uint32_t pulse);

static void advanceClock(uint32_t now)
{
    const uint32_t elapsed = now - s.lastTickUpdate;
    s.lastTickUpdate = now;
    Sequencer::NdlrClock tracker{};
    float tempo = 0.0f;
    if (!s.internalClock)
    {
        DISABLE_AUDIO_INTERRUPTS();
        tracker = sequencer.getNdlrClock();
        if (tracker.running)
            tempo = sequencer.getCurrentTempo();
        ENABLE_AUDIO_INTERRUPTS();
    }
    if (tracker.running)
    {
        const auto tempoDisplay = static_cast<uint16_t>(clamp(tempo, 0.0f, 999.0f) + 0.5f);
        if (s.trackerTempo != tempoDisplay)
            s.dirty = 1;
        s.trackerTempo = tempoDisplay;
        if (!s.trackerClock || tracker.run != s.trackerRun || tracker.pulse < s.trackerPulse)
        {
            s.trackerClock = true;
            s.trackerRun = tracker.run;
            s.barPulse = (tracker.pulse + tracker.startStep * (kPulsesPerBeat / 4)) % kPulsesPerBar;
            queueTrackerRestart();
        }
        else
        {
            const uint32_t pulses = tracker.pulse - s.trackerPulse;
            s.pulse += pulses;
            s.barPulse = (s.barPulse + pulses) % kPulsesPerBar;
        }
        s.trackerPulse = tracker.pulse;
        s.trackerPos = tracker.startStep * kPulsesPerStep + tracker.pulse;
        return;
    }
    if (s.trackerClock)
    {
        s.trackerClock = false;
        s.tickPhase = 0;
        if (s.internalClock && s.quantise == 0 && hasQueuedLaunch())
            applyLaunch(s.pulse);
        s.dirty = 1;
        return;
    }
    // Bound catch-up after the UI stalls. Playback processes at most one step per part.
    const uint32_t phase = s.tickPhase + (elapsed > 1000 ? 1000 : elapsed) * s.tempo * kPulsesPerBeat;
    const auto pulses = static_cast<uint8_t>(phase / kMinuteMs);
    s.tickPhase = phase % kMinuteMs;
    s.pulse += pulses;
    s.barPulse = (s.barPulse + pulses) % kPulsesPerBar;
}

static int inMidiRange(int note)
{
    while (note > 127)
        note -= 12;
    return note;
}

static void releasePad(uint32_t pulse = s.pulse)
{
    for (int i = 0; i < s.padPlayed; i++)
        synthNote(false, s.padNotes[i], 0, kChPad);
    if (s.padPlayed && capturing(0))
        ndlrCapture::release(0, capturePulse(pulse));
    s.padCount = s.padPlayed = 0;
}

static void releaseDrone(uint32_t pulse = s.pulse)
{
    for (int i = 0; i < s.droneCount; i++)
        synthNote(false, s.droneNotes[i], 0, kChDrone);
    if (s.droneCount && capturing(1))
        ndlrCapture::release(1, capturePulse(pulse));
    s.droneCount = 0;
}

static void releaseMotif(int index)
{
    auto& voice = s.motifPlayback[index];
    if (voice.note >= 0)
    {
        synthNote(false, voice.note, 0, kChMotif1 + index);
        // The frame can come after the gate ended; the note ended at the gate.
        if (capturing(index + 2))
            ndlrCapture::release(index + 2, capturePulse(due(s.pulse, voice.noteEnd) ? voice.noteEnd : s.pulse));
    }
    voice.note = -1;
}

static bool strumOnTracks()
{
    return s.padStrum != 0 && s.strumKeep;
}

static void allNotesOff()
{
    releasePad();
    releaseDrone();
    releaseMotif(0);
    releaseMotif(1);
}

static void playPadNotes()
{
    while (s.padPlayed < s.padCount && due(s.pulse, s.padNext))
    {
        synthNote(true, s.padNotes[s.padPlayed], kVelPad, kChPad);
        if (capturing(0) && strumOnTracks())
            ndlrCapture::note(0, s.padPlayed, capturePulse(s.padNext), s.padNotes[s.padPlayed], kVelPad);
        ++s.padPlayed;
        const bool together = (s.padStrum == 4 || s.padStrum == 6) && s.padPlayed < 3;
        if (!together)
            s.padNext += kStrumPulses[s.padStrum];
    }
}

// The Pad's notes for the current chord, range and spread, lowest first.
static int padVoicing(int16_t* notes, int voices)
{
    int16_t chord[4];
    const int chordSize = buildChord(s.padOct, chord, 4);
    const int wanted = s.padRange ? s.padRange : chordSize;
    const int count = wanted < voices ? wanted : voices;
    const int fifth = noteForStep(s.degree + 4, s.padOct);
    int filled = 0;
    for (int i = 0; i < count; ++i)
    {
        int note = chord[i % chordSize] + 12 * (i / chordSize);
        switch (s.padSpread)
        {
            case 1: // Drop the second tone by an octave.
                if (i == 1)
                    note -= 12;
                break;
            case 2: // Put a low root underneath the close voicing.
                note = i == 0 ? chord[0] - 12
                              : chord[(i - 1) % chordSize] + 12 * ((i - 1) / chordSize);
                break;
            case 3:
                note = (i % 2 == 0 ? chord[0] : fifth) + 12 * (i / 2);
                break;
            default: break;
        }
        // Preserve an open chord at high octaves without doubling an existing pitch.
        note = inMidiRange(note);
        for (int j = 0; j < filled; ++j)
        {
            if (notes[j] == note)
            {
                note -= 12;
                j = -1;
            }
        }
        notes[filled++] = static_cast<int16_t>(note);
    }

    // Strum from the lowest to the highest note, including spread voicings.
    for (int i = 1; i < filled; ++i)
    {
        const int16_t note = notes[i];
        int j = i;
        while (j > 0 && notes[j - 1] > note)
        {
            notes[j] = notes[j - 1];
            --j;
        }
        notes[j] = note;
    }
    return filled;
}

static void voicePad(uint32_t pulse = s.pulse)
{
    releasePad(pulse);
    if (!s.partOn[0])
        return;

    const int wanted = s.padRange ? s.padRange : kShape[resolvedType()].n;
    wantVoices(0, wanted);
    s.padCount = static_cast<uint8_t>(padVoicing(s.padNotes, partVoices(0)));
    s.padNext = pulse;
    if (capturing(0) && !strumOnTracks())
        ndlrCapture::chord(0, capturePulse(pulse), s.padNotes, s.padCount, kVelPad);
    playPadNotes();
}

// The Drone's notes for the current trigger, octave and note choice.
static int droneVoicing(int16_t* notes, int voices)
{
    const int root = s.droneTrigger < 4 ? s.degree : 0;
    const int16_t tonic = static_cast<int16_t>(noteForStep(root, s.droneOct));
    int count = 0;
    if (voices > 0)
        notes[count++] = tonic;
    if (s.droneNotesMode >= 2 && count < voices)
        notes[count++] = static_cast<int16_t>(noteForStep(root + 4, s.droneOct));
    if ((s.droneNotesMode & 1) && count < voices)
        notes[count++] = tonic + 12;
    return count;
}

static void voiceDrone(uint32_t pulse = s.pulse)
{
    releaseDrone(pulse);
    if (!s.partOn[1])
        return;

    const int desired = s.droneNotesMode == 0 ? 1 : s.droneNotesMode == 3 ? 3 : 2;
    wantVoices(1, desired);
    s.droneCount = static_cast<uint8_t>(droneVoicing(s.droneNotes, partVoices(1)));
    for (int i = 0; i < s.droneCount; ++i)
        synthNote(true, s.droneNotes[i], kVelDrone, kChDrone);
    if (capturing(1))
    {
        // Lowest first, for the Chord FX.
        int16_t sorted[3];
        memcpy(sorted, s.droneNotes, sizeof(sorted));
        for (int i = 1; i < s.droneCount; ++i)
            for (int j = i; j > 0 && sorted[j - 1] > sorted[j]; --j)
                std::swap(sorted[j - 1], sorted[j]);
        ndlrCapture::chord(1, capturePulse(pulse), sorted, s.droneCount, kVelDrone);
    }
}

static uint32_t droneInterval()
{
    const uint8_t cadence = s.droneTrigger % 4;
    return kPulsesPerBeat * (1u << (cadence ? cadence - 1 : 0));
}

static void restartDrone(uint32_t pulse = s.pulse)
{
    voiceDrone(pulse);
    s.droneNext = pulse + droneInterval();
}

static void revoice()
{
    voicePad();
    restartDrone();
}

static void resetMotif(int index, uint32_t pulse = s.pulse)
{
    releaseMotif(index);
    auto& voice = s.motifPlayback[index];
    voice.patternStep = 0;
    voice.rhythmStep = 0;
    voice.pos = -1;
    voice.beat = -1;
    voice.nextStep = pulse + (index == 1 ? 12 * s.motifOffset : 0);
}

// A motif that was paused carries on from where it stopped.
static void resumeMotif(int index, uint32_t pulse)
{
    releaseMotif(index);
    s.motifPlayback[index].nextStep = pulse + (index == 1 ? 12 * s.motifOffset : 0);
}

static uint8_t playingParts()
{
    uint8_t parts = 0;
    for (int part = 0; part < 4; ++part)
        parts |= s.partOn[part] << part;
    return parts;
}

static bool hasQueuedLaunch()
{
    return s.pendingPartToggles || s.queuedChord;
}

static bool quantiseLaunch()
{
    return s.trackerClock || s.quantise != 0;
}

static int quantiseMode()
{
    return s.trackerClock && !s.quantise ? 1 : s.quantise;
}

static void setLaunchDeadline()
{
    const uint32_t grid = s.quantise == 2 ? kPulsesPerBar : kPulsesPerBeat;
    s.launchPulse = s.pulse + grid - s.barPulse % grid;
}

static void finishCapture(bool complete);

static void queueTrackerRestart()
{
    if (s.capState == capRunning)
        finishCapture(false);
    s.capLastPos = UINT32_MAX;
    const uint8_t requested = playingParts() ^ s.pendingPartToggles;
    allNotesOff();
    memset(s.partOn, 0, sizeof(s.partOn));
    s.pendingPartToggles = requested;
    s.resync = true;
    if (hasQueuedLaunch())
        setLaunchDeadline();
    s.dirty = 1;
}

static void applyLaunch(uint32_t pulse)
{
    const uint8_t changed = s.pendingPartToggles;
    const bool chordChanged = s.queuedChord != 0;
    const bool silent = playingParts() == 0;
    s.pendingPartToggles = 0;
    if (chordChanged)
    {
        s.degree = s.queuedChord - 1;
        s.type = s.queuedChordType;
        s.queuedChord = 0;
    }

    for (int part = 0; part < 4; ++part)
        s.partOn[part] ^= (changed >> part) & 1u;

    if ((changed & 1) && !s.partOn[0])
    {
        releasePad(pulse);
        P->reserveVoices(0, 0);
    }
    if ((changed & 2) && !s.partOn[1])
    {
        releaseDrone(pulse);
        P->reserveVoices(1, 0);
    }
    for (int index = 0; index < 2; ++index)
    {
        if (!(changed & (4 << index)))
            continue;
        // ALL restarts both motifs together; one motif on its own carries on where it paused.
        auto& voice = s.motifPlayback[index];
        if (s.resync || !voice.started)
            resetMotif(index, pulse);
        else
            resumeMotif(index, pulse);
        voice.started = true;
    }
    s.resync = false;
    if (silent && playingParts())
        s.playStart = pulse;
    if (s.partOn[0] && ((changed & 1) || chordChanged))
        voicePad(pulse);
    if (s.partOn[1] && ((changed & 2) || (chordChanged && s.droneTrigger < 4)))
        restartDrone(pulse);
    s.dirty = 1;
}

static void updateLaunch()
{
    if (hasQueuedLaunch() && due(s.pulse, s.launchPulse))
        applyLaunch(s.launchPulse);
}

static void togglePart(int part)
{
    advanceClock(nowMs());
    updateLaunch();
    if (quantiseLaunch() && !hasQueuedLaunch())
        setLaunchDeadline();
    s.pendingPartToggles ^= 1u << part;
    if (!quantiseLaunch())
        applyLaunch(s.pulse);
}

static void selectChord(int degree)
{
    advanceClock(nowMs());
    updateLaunch();
    const bool alreadyQueued = hasQueuedLaunch();
    if (quantiseLaunch() && !alreadyQueued)
        setLaunchDeadline();
    if (s.pendingType >= 0)
    {
        s.queuedChordType = s.pendingType;
        s.pendingType = -1;
    }
    else if (!s.queuedChord)
        s.queuedChordType = s.type;
    s.queuedChord = degree + 1;
    if (!quantiseLaunch() || (!alreadyQueued && !playingParts()))
        applyLaunch(s.pulse);
}

static uint32_t randomValue()
{
    s.random ^= s.random << 13;
    s.random ^= s.random >> 17;
    s.random ^= s.random << 5;
    return s.random;
}

static int motifPatternIndex(uint8_t variation, uint32_t step, int length)
{
    const int i = step % length;
    switch (variation)
    {
        case 1: return length - 1 - i;
        case 2:
        {
            if (length == 1)
                return 0;
            const int period = length * 2 - 2;
            const int pos = step % period;
            return pos < length ? pos : period - pos;
        }
        case 3:
        {
            const int pos = step % (length * 2);
            return pos < length ? pos : length * 2 - 1 - pos;
        }
        case 4: return i < (length + 1) / 2 ? 2 * i : 2 * (i - (length + 1) / 2) + 1;
        case 5: return randomValue() % length;
        default: return i;
    }
}

//==============================================================================================
// Patterns, rhythms and sequences: the store
//==============================================================================================
static void readPattern(int slot, MotifPattern& out)
{
    if (slot >= kPresetCount)
    {
        out = s_userPatterns[slot - kPresetCount];
        return;
    }
    const PatternPreset& preset = kPatternPresets[slot];
    out.type = preset.type;
    out.autoLength = preset.autoLength;
    for (int i = 0; i < kPatternNotes; ++i)
        out.notes[i] = preset.notes[i % preset.count];
}

static void readRhythm(int slot, int8_t* out)
{
    if (slot >= kPresetCount)
    {
        memcpy(out, s_userRhythms[slot - kPresetCount], kRhythmBeats);
        return;
    }
    const RhythmPreset& preset = kRhythmPresets[slot];
    for (int i = 0; i < kRhythmBeats; ++i)
        out[i] = preset.steps[i % preset.count];
}

static void slotName(char* text, size_t size, int slot, bool rhythm)
{
    if (slot >= kPresetCount)
        snprintf(text, size, "%d User", slot + 1);
    else
        snprintf(text, size, "%d %s", slot + 1, rhythm ? kRhythmPresets[slot].name : kPatternPresets[slot].name);
}

static void setSeqStep(ChordSequence& q, int section, int step, uint8_t dur, uint8_t key, uint8_t mode,
                       uint8_t degree, uint8_t type)
{
    q.sections[section][step] = { dur, key, mode, degree, type };
}

// User slots start as a plain rising pattern and a straight rhythm. Sequence 1 is I-V-vi-IV then
// ii-V-I in C major and sequence 2 is i-VI-III-VII in A minor, so there is something to arm.
static void initStore()
{
    if (s_storeReady)
        return;
    s_storeReady = true;
    for (auto& pattern : s_userPatterns)
    {
        pattern.type = ptChord;
        pattern.autoLength = 8;
        for (int i = 0; i < kPatternNotes; ++i)
            pattern.notes[i] = static_cast<uint8_t>(i % 8 + 1);
    }
    memset(s_userRhythms, 100, sizeof(s_userRhythms));
    memset(s_sequences, 0, sizeof(s_sequences));

    ChordSequence& a = s_sequences[1];
    setSeqStep(a, 0, 0, 8, 0, 0, 0, ctTriad);
    setSeqStep(a, 0, 1, 8, 0, 0, 4, ctTriad);
    setSeqStep(a, 0, 2, 8, 0, 0, 5, ctTriad);
    setSeqStep(a, 0, 3, 8, 0, 0, 3, ctTriad);
    setSeqStep(a, 1, 0, 8, 0, 0, 1, ct7th);
    setSeqStep(a, 1, 1, 8, 0, 0, 4, ct7th);
    setSeqStep(a, 1, 2, 16, 0, 0, 0, ctTriad);
    a.song[0] = { 0, 2 };
    a.song[1] = { 1, 1 };

    ChordSequence& b = s_sequences[2];
    setSeqStep(b, 0, 0, 8, 3, 5, 0, ctTriad);
    setSeqStep(b, 0, 1, 8, 3, 5, 5, ctTriad);
    setSeqStep(b, 0, 2, 8, 3, 5, 2, ctTriad);
    setSeqStep(b, 0, 3, 8, 3, 5, 6, ctTriad);
    b.song[0] = { 0, 1 };
}

//==============================================================================================
// Motifs
//==============================================================================================
static int effectivePatternLength(int index)
{
    const auto& m = s.motifSettings[index];
    if (m.patternLength)
        return m.patternLength;
    return m.patt.autoLength ? m.patt.autoLength : 2 * kShape[resolvedType()].n;
}

// Beats that sound a new note. Rests and ties do not take a note from the pattern.
static int noteBeats(int index)
{
    const auto& m = s.motifSettings[index];
    int count = 0;
    for (int i = 0; i < m.rhythmLength; ++i)
        count += m.rhy[i] > 0;
    return count;
}

static int patternNote(int index, int value)
{
    const auto& m = s.motifSettings[index];
    const int k = value - 1;
    if (m.patt.type == ptChord)
    {
        int16_t chord[4];
        const int size = buildChord(m.octave, chord, 4);
        return chord[k % size] + 12 * (k / size);
    }
    if (m.patt.type == ptScale)
        return noteForStep(k, m.octave);
    return 12 * (m.octave + 1) + kKeySemi[s.key] + k;
}

static void loadPattern(int index)
{
    readPattern(s.motifSettings[index].pattern, s.motifSettings[index].patt);
    s.motifPlayback[index].patternStep = 0;
}

static void loadRhythm(int index)
{
    readRhythm(s.motifSettings[index].rhythm, s.motifSettings[index].rhy);
    s.motifPlayback[index].rhythmStep = 0;
}

static uint8_t motifVelocity(int index, uint8_t rhythmVelocity, uint32_t stepStart)
{
    const uint8_t accent = s.motifSettings[index].accent;
    int velocity;
    if (accent == 0)
        velocity = kVelMotif;
    else if (accent == 1)
        velocity = rhythmVelocity;
    else if (accent == 2)
        velocity = rhythmVelocity + static_cast<int>(randomValue() % 17) - 8;
    else
    {
        // where this step falls in the bar, in eighth notes
        const uint32_t late = (s.pulse - stepStart) % kPulsesPerBar;
        const uint32_t barPos = (s.barPulse + kPulsesPerBar - late) % kPulsesPerBar;
        velocity = kAccentNames[accent][(barPos / 24) % 8] == 'o' ? kVelAccentHigh : kVelAccentLow;
    }
    return static_cast<uint8_t>(clamp(velocity, 1, 127));
}

static bool motifOnScreen(int index)
{
    if (!kShowPlayhead || index != s.motif - 1)
        return false;
    return s.menuMode == 2 ? s.editor != ndlrEnc1 : s.menuMode == 0 && s.activeEnc >= 4;
}

static void tickMotif(int index)
{
    if (!s.partOn[index + 2])
        return;
    auto& voice = s.motifPlayback[index];
    const auto& settings = s.motifSettings[index];
    if (voice.note >= 0 && due(s.pulse, voice.noteEnd))
        releaseMotif(index);
    if (!due(s.pulse, voice.nextStep))
        return;

    const uint32_t interval = kDivisionPulses[settings.division];
    const uint32_t skipped = (s.pulse - voice.nextStep) / interval;
    const uint32_t stepStart = voice.nextStep + skipped * interval;
    voice.nextStep = stepStart + interval;
    const uint8_t step = (voice.rhythmStep + skipped) % settings.rhythmLength;
    voice.rhythmStep = (step + 1) % settings.rhythmLength;
    voice.beat = static_cast<int8_t>(step);
    if (motifOnScreen(index))
        s.dirty = 1;

    const int8_t value = settings.rhy[step];
    if (value == kTie)
        return; // the held note carries on
    releaseMotif(index);
    if (value == kRest || partVoices(index + 2) == 0)
        return;

    const int length = effectivePatternLength(index);
    const int position = motifPatternIndex(settings.variation, voice.patternStep++, length);
    voice.pos = static_cast<int8_t>(position);
    const int note = inMidiRange(patternNote(index, settings.patt.notes[position % kPatternNotes]));
    const uint8_t velocity = motifVelocity(index, static_cast<uint8_t>(value), stepStart);
    synthNote(true, note, velocity, kChMotif1 + index);
    voice.note = static_cast<int16_t>(note);
    voice.velocity = velocity;
    if (capturing(index + 2))
        ndlrCapture::note(index + 2, 0, capturePulse(stepStart), static_cast<int16_t>(note), velocity);

    // Hold the note through any ties that follow it.
    uint32_t ties = 0;
    while (ties + 1 < settings.rhythmLength && settings.rhy[(step + 1 + ties) % settings.rhythmLength] == kTie)
        ++ties;
    voice.noteEnd = stepStart + ties * interval + (interval * settings.gate + 99) / 100;
    // Start a fresh gate if the frame arrived too late for this step's original gate.
    if (due(s.pulse, voice.noteEnd))
        voice.noteEnd = s.pulse + 1;
}

//==============================================================================================
// Chord sequencer
//==============================================================================================
// The song is its slots in order, each section repeated, leaving out steps with no duration.
// Returns how many steps that is, and the place of step `wanted` when one is asked for.
static int seqPlaylist(int wanted, int* slot, int* section, int* step)
{
    int count = 0;
    for (int i = 0; i < 8; ++i)
    {
        const SeqSlot& entry = s.seq.song[i];
        for (int repeat = 0; repeat < entry.repeats; ++repeat)
        {
            for (int j = 0; j < 6; ++j)
            {
                if (s.seq.sections[entry.section][j].dur == 0)
                    continue;
                if (count == wanted)
                {
                    *slot = i;
                    *section = entry.section;
                    *step = j;
                }
                ++count;
            }
        }
    }
    return count;
}

static void seqStop()
{
    s.seqRunning = false;
    s.seqPlaySection = s.seqPlayStep = s.seqPlaySlot = -1;
    s.dirty = 1;
}

static void seqAdvance(uint32_t pulse)
{
    int slot = 0, section = 0, step = 0;
    const int count = seqPlaylist(-1, &slot, &section, &step);
    if (count == 0)
    {
        seqStop();
        return;
    }
    s.seqIndex = static_cast<int16_t>((s.seqIndex + 1) % count);
    seqPlaylist(s.seqIndex, &slot, &section, &step);
    const SeqStep& st = s.seq.sections[section][step];
    s.seqPlaySlot = static_cast<int8_t>(slot);
    s.seqPlaySection = static_cast<int8_t>(section);
    s.seqPlayStep = static_cast<int8_t>(step);
    s.seqNext = pulse + st.dur * (kPulsesPerBeat / 2);
    s.key = st.key;
    s.mode = st.mode;
    s.degree = st.degree;
    s.type = st.type;
    s.queuedChord = 0;
    if (s.partOn[0])
        voicePad(pulse);
    if (s.partOn[1])
        restartDrone(pulse);
    s.dirty = 1;
}

static void seqTick()
{
    if (s.seqRunning && !playingParts() && !s.pendingPartToggles)
        seqStop();
    if (!s.seqRunning)
    {
        // An armed sequence starts with the parts, on the first downbeat once it is both armed
        // and playing. Frames can arrive a few pulses late, so compare against the bar's start.
        if (!s.seqArmed || !playingParts())
            return;
        const uint32_t waitFrom = due(s.playStart, s.seqArmPulse) ? s.playStart : s.seqArmPulse;
        const uint32_t barStart = s.pulse - s.barPulse;
        if (due(barStart, waitFrom))
        {
            s.seqRunning = true;
            s.seqIndex = -1;
            seqAdvance(barStart);
        }
        return;
    }
    for (int guard = 0; s.seqRunning && due(s.pulse, s.seqNext) && guard < 8; ++guard)
        seqAdvance(s.seqNext);
}

//==============================================================================================
// Capture into the pattern
//==============================================================================================
// The tracks each part writes to, from CAP TRK on. Returns false when they run past the last track.
static bool captureLayout(ndlrCapture::PartLayout (&layout)[ndlrCapture::kParts])
{
    const uint8_t parts = kCaptureParts[s.capParts];
    const uint8_t counts[4] = { s.padTracks, kDroneTracks, 1, 1 };
    int track = s.capTrack;
    for (int part = 0; part < 4; ++part)
    {
        layout[part] = { -1, 0, 0 };
        if (!(parts >> part & 1))
            continue;
        const int instrument = external(part) ? MIDI_INSTRUMENTS_START_IDX + s.partOut[part] - 1
                                              : SYNTH_INSTRUMENTS_START_IDX + part;
        layout[part] = { static_cast<int8_t>(track), counts[part], static_cast<uint8_t>(instrument) };
        track += counts[part];
    }
    return track <= kAllTracks;
}

static int captureLastTrack()
{
    ndlrCapture::PartLayout layout[ndlrCapture::kParts];
    captureLayout(layout);
    int last = s.capTrack;
    for (const auto& part : layout)
        if (part.firstTrack >= 0)
            last = part.firstTrack + part.tracks - 1;
    return last;
}

static void startCapture(uint32_t position)
{
    ndlrCapture::PartLayout layout[ndlrCapture::kParts];
    captureLayout(layout);
    DISABLE_AUDIO_INTERRUPTS();
    const auto steps = static_cast<uint16_t>(sequencer.getPatternLength());
    ENABLE_AUDIO_INTERRUPTS();
    ndlrCapture::begin(layout, steps);
    s.capState = capRunning;
    s.capStart = s.pulse - position;
    s.capLength = steps * kPulsesPerStep;
    s.capPattern = mtProject.values.actualPattern;

    // Notes still sounding from before the pattern started go on its first step.
    if (capturing(0) && s.padPlayed)
    {
        if (strumOnTracks())
            for (int i = 0; i < s.padPlayed; ++i)
                ndlrCapture::note(0, i, 0, s.padNotes[i], kVelPad);
        else
            ndlrCapture::chord(0, 0, s.padNotes, s.padPlayed, kVelPad);
    }
    if (capturing(1) && s.droneCount)
    {
        int16_t sorted[3];
        memcpy(sorted, s.droneNotes, sizeof(sorted));
        std::sort(sorted, sorted + s.droneCount);
        ndlrCapture::chord(1, 0, sorted, s.droneCount, kVelDrone);
    }
    for (int index = 0; index < 2; ++index)
    {
        const auto& voice = s.motifPlayback[index];
        if (capturing(index + 2) && voice.note >= 0)
            ndlrCapture::note(index + 2, 0, 0, voice.note, voice.velocity);
    }
    toast("Capturing the pattern");
}

static void finishCapture(bool complete)
{
    const uint8_t parts = kCaptureParts[s.capParts];
    const auto result = ndlrCapture::finish();
    s.capState = capOff;
    if (!complete)
    {
        toast("Capture stopped early");
        return;
    }

    // The pattern plays the parts from here, so the panel stops playing them.
    for (int part = 0; part < 4; ++part)
    {
        if (!(parts >> part & 1))
            continue;
        s.partOn[part] = 0;
        s.pendingPartToggles &= ~(1u << part);
        if (part == 0)
            releasePad();
        else if (part == 1)
            releaseDrone();
        else
            releaseMotif(part - 2);
        P->reserveVoices(part, 0);
    }
    char text[32];
    if (result.dropped)
        snprintf(text, sizeof(text), "Captured, %u notes left out", unsigned(result.dropped));
    else
        snprintf(text, sizeof(text), "Captured %u notes", unsigned(result.notes));
    toast(text);
}

static void toggleCapture()
{
    if (s.capState == capRunning)
    {
        finishCapture(false);
        return;
    }
    if (s.capState == capArmed)
    {
        s.capState = capOff;
        toast("Capture off");
        return;
    }
    ndlrCapture::PartLayout layout[ndlrCapture::kParts];
    if (!captureLayout(layout))
    {
        toast("Too many tracks: lower CAP TRK");
        return;
    }
    s.capState = capArmed;
    s.capLastPos = UINT32_MAX;
    toast(s.trackerClock ? "Armed: starts with the pattern" : "Armed: press Play");
}

// Runs before the parts so that notes on the pattern's first step are written.
static void captureTick()
{
    if (s.capState == capOff)
        return;
    if (!s.trackerClock)
    {
        if (s.capState == capRunning)
            finishCapture(false);
        s.capLastPos = UINT32_MAX;
        return;
    }
    if (s.capState == capRunning)
    {
        if (mtProject.values.actualPattern != s.capPattern)
            finishCapture(false);
        else if (due(s.pulse, s.capStart + s.capLength))
            finishCapture(true);
        return;
    }

    DISABLE_AUDIO_INTERRUPTS();
    const uint32_t length = sequencer.getPatternLength() * kPulsesPerStep;
    ENABLE_AUDIO_INTERRUPTS();
    const uint32_t position = s.trackerPos % length;
    const bool wrapped = s.capLastPos != UINT32_MAX && position < s.capLastPos;
    s.capLastPos = position;
    // Start on a pattern start with a captured part playing, so the pass is not silent.
    if (wrapped && (playingParts() & kCaptureParts[s.capParts]))
        startCapture(position);
}

static void musicTick()
{
    captureTick();
    updateLaunch();
    seqTick();
    playPadNotes();
    if (s.partOn[1] && s.droneTrigger % 4 != 0 && due(s.pulse, s.droneNext))
    {
        const uint32_t interval = droneInterval();
        restartDrone(s.droneNext + (s.pulse - s.droneNext) / interval * interval);
    }
    tickMotif(0);
    tickMotif(1);
}

//==============================================================================================
// Info text (from the NDLR Rev2 manual, FW 3.3.x)
//==============================================================================================
static const char* const kModeHeader[] = { "PRESS / TURN", "+ SHIFT", "IN MENU MODE",
                                           "IN EDITOR MODE", "ON SCREEN", "AT BOOT" };

static const strInfo kInfo[ndlrControlCount] = {
    /* E1 */ { "Enc 1 - PAD", 3,
              { { imNormal, "Turn: Pad POSITION / STRUM. Press: switch between the two." },
                { imMenu, "PAD OUT: turn for Synth 1 or MIDI channel 1-16." },
                { imEditor, "Chord Sequencer." }, { 0, "" } },
              "Position sets the octave. Strum spreads each new chord across the beat, from Off to an eighth-note interval." },
    /* E2 */ { "Enc 2 - PAD", 2,
              { { imNormal, "Turn: Pad RANGE / SPREAD. Press: switch between the two." },
                { imMenu, "DRN OUT: turn for Synth 2 or MIDI channel 1-16." }, { 0, "" }, { 0, "" } },
              "Range sets the note count, limited by available Synth 1 voices. Auto follows the chord. Spread changes the voicing." },
    /* E3 */ { "Enc 3 - DRONE", 2,
              { { imNormal, "Turn: Drone POSITION / NOTES / TRIGGER. Press: cycle the three." },
                { imMenu, "M1 OUT: turn for Synth 3 or MIDI channel 1-16." }, { 0, "" }, { 0, "" } },
              "Notes adds a fifth or octave when voices are available. Trigger follows the chord or holds the key root, with optional repeats." },
    /* E4 */ { "Enc 4 - T-K-M", 4,
              { { imNormal, "Press to cycle Tempo, Key, Mode, Quantise and Clock. Turn to adjust." },
                { imShift, "Physical Shift + +/- controls the Mini's master volume." },
                { imMenu, "M2 OUT: turn for Synth 4 or MIDI channel 1-16." },
                { imEditor, "MIDI Controller editor." } },
              "The keyboard shows the notes of the key and mode, with the chord in green. Tracker follows "
              "the running pattern. Internal uses NDLR tempo." },
    /* E5 */ { "Enc 5 - MOTIF", 3,
              { { imNormal, "Turn: Motif POSITION / PATT-LEN for the selected motif." },
                { imMenu, "CAPTURE: turn to pick the parts. Press to arm or stop." }, { imEditor, "Pattern Editor." }, { 0, "" } },
              "Position sets the octave. Pattern Length sets 1 to 16 notes, or Auto to follow the preset length." },
    /* E6 */ { "Enc 6 - MOTIF", 3,
              { { imNormal, "Turn: Motif VARIATION / PATTERN." },
                { imMenu, "CAP TRK: the first track written. Press to arm or stop." }, { imEditor, "Rhythm Editor." }, { 0, "" } },
              "Variation changes playback order. Pattern picks one of forty: 1 to 20 are presets, 21 to 40 "
              "are yours, written in the Pattern Editor." },
    /* E7 */ { "Enc 7 - MOTIF", 3,
              { { imNormal, "Turn: CLK DIV / RHY-LEN. Centre arrow (GATE) is not printed on the panel." },
                { imMenu, "PAD TRKS: tracks for the Pad, 1-8. Press to arm or stop." },
                { imEditor, "Perlin Noise generator." }, { 0, "" } },
              "Clock Division sets the step rate. Rhythm Length sets 4 to 32 steps. Gate sets the percentage of each step that a note sounds." },
    /* E8 */ { "Enc 8 - MOTIF", 2,
              { { imNormal, "Turn: ACCENT / RHYTHM / OFFSET." },
                { imMenu, "STRUM: keep a strummed Pad's timing, or play it as one chord." }, { 0, "" }, { 0, "" } },
              "Accent sets the velocities: fixed, the rhythm's own, humanised, or high (o) and low (.) eighth "
              "notes. Rhythm picks one of forty. Offset delays Motif 2 by up to three sixteenths." },
    /* ALL */ { "ALL  ||/>", 2,
               { { imNormal, "Play/pause all four parts. Restarts both motifs in sync." },
                 { imShift, "MOD: toggle ALL modulations (red rings; visual only here)." }, { 0, "" }, { 0, "" } },
               "Restarts both motifs on their first pattern note and first rhythm beat so they lock together." },
    /* 1/2 */ { "1/2 and PANIC", 2,
               { { imNormal, "Right-hand encoders edit Motif 1 or Motif 2." },
                 { imShift, "PANIC: stop all parts and release their notes." }, { 0, "" }, { 0, "" } },
               "The selected motif is highlighted on the NDLR screen. Panic releases the notes played by the panel." },
    /* chords: filled in dynamically */
    { nullptr, 0, { { 0, "" }, { 0, "" }, { 0, "" }, { 0, "" } }, nullptr },
    { nullptr, 0, { { 0, "" }, { 0, "" }, { 0, "" }, { 0, "" } }, nullptr },
    { nullptr, 0, { { 0, "" }, { 0, "" }, { 0, "" }, { 0, "" } }, nullptr },
    { nullptr, 0, { { 0, "" }, { 0, "" }, { 0, "" }, { 0, "" } }, nullptr },
    { nullptr, 0, { { 0, "" }, { 0, "" }, { 0, "" }, { 0, "" } }, nullptr },
    { nullptr, 0, { { 0, "" }, { 0, "" }, { 0, "" }, { 0, "" } }, nullptr },
    { nullptr, 0, { { 0, "" }, { 0, "" }, { 0, "" }, { 0, "" } }, nullptr },
    /* SCREEN */ { "NDLR SCREEN", 2,
                  { { imView, "Top stripes: Motif 1, Pad, Motif 2." },
                    { imView, "Side markers: selected encoder." }, { 0, "" }, { 0, "" } },
                  "Shows key and mode, BPM, chord degree + name + type, and the four play indicators. "
                  "The larger sub screen on the right follows the dial in use: Pad, Drone, T-K-M or Motif." },
    /* MENU */ { "MENU / EDITORS", 3,
                { { imNormal, "Toggle Menu mode: dials 1-4 set where the parts play, dials 5-8 capture." },
                  { imShift, "Toggle Editor mode. The last editor used is shown." },
                  { imBoot, "At power-on, Shift+Menu opens the Boot Menu." }, { 0, "" } },
                "Editors: Chord Sequencer on dial 1, Pattern Editor on dial 5, Rhythm Editor on dial 6. In "
                "Editor mode the dials turn the editor's settings and a press changes editor." },
    /* SHIFT */ { "SHIFT", 3,
                 { { imNormal, "Hold it with another control for the italic label. Middle latches it." },
                   { imNormal, "Tap it four times to set the tempo, 40 to 300 BPM." },
                   { imBoot, "At power-on: freeze the splash screen." }, { 0, "" } },
                 "Shifted functions: chord type on the 7 chord buttons, MOD on part buttons and ALL, "
                 "PANIC on 1/2, Editors on MENU. The Mini Shift button works as NDLR Shift too." },
    /* PAD */ { "PAD  ||/>", 2,
               { { imNormal, "Play / pause the Pad." },
                 { imShift, "MOD: toggle modulations that target the Pad (red ring)." }, { 0, "" }, { 0, "" } },
               "Pad plays every note of the current chord, on Synth 1 or the MIDI channel set in Menu mode." },
    /* DRONE */ { "DRONE  ||/>", 2,
                 { { imNormal, "Play / pause the Drone." },
                   { imShift, "MOD: toggle modulations that target the Drone (red ring)." }, { 0, "" }, { 0, "" } },
                 "Drone can hold the chord or key root, on Synth 2 or the MIDI channel set in Menu mode." },
    /* M1 */ { "MOTIF 1  ||/>", 2,
               { { imNormal, "Play / pause Motif 1 (up arpeggio)." },
                 { imShift, "MOD: toggle modulations that target Motif 1 (red ring)." }, { 0, "" }, { 0, "" } },
               "Defaults to an ascending eighth-note arpeggio. A paused motif carries on from where it "
               "stopped; ALL restarts both. Plays on Synth 3 or the MIDI channel set in Menu mode." },
    /* M2 */ { "MOTIF 2  ||/>", 2,
               { { imNormal, "Play / pause Motif 2 (down arpeggio)." },
                 { imShift, "MOD: toggle modulations that target Motif 2 (red ring)." }, { 0, "" }, { 0, "" } },
               "Defaults to a descending quarter-note arpeggio on Synth 4 or the MIDI channel set in Menu "
               "mode. A paused motif carries on from where it stopped; ALL restarts both." },
    /* REC */ { "RECORD", 1,
                { { imNormal, "Press Middle to start or stop recording." } },
                "Saves the stereo mix, including the Tracker pattern, to Recordings on the SD card." },
};

// What each dial turns in each editor, indexed by the dial that opens the editor.
struct strEditorHelp
{
    const char* title;
    const char* dial[8];
    const char* desc;
};
static const strEditorHelp kPatternHelp = {
    "Pattern Editor",
    { "Turn to load a pattern, 1 to 40, into the selected motif. Shift + press loads the stored one again.",
      "Turn to change the order the pattern is played in.",
      "Turn to change the motif's rhythm.",
      "Turn to choose a step. Turn back past step 1 to choose the note type instead.",
      "Turn to choose a user slot, 21 to 40. Shift + press saves the pattern there.",
      "Turn to change the clock division.",
      "Turn to set how many of the sixteen steps are played.",
      "Turn to change the chosen step's note, or the note type: Chord, Scale or Chromatic." },
    "The sixteen bars are the pattern's notes. Chord buttons, Shift types and the 1/2 button still work "
    "here, so you can hear the pattern as you change it."
};
static const strEditorHelp kRhythmHelp = {
    "Rhythm Editor",
    { "Turn to load a rhythm, 1 to 40, into the selected motif. Shift + press loads the stored one again.",
      "Turn to change the order the pattern is played in.",
      "Turn to change the motif's pattern.",
      "Turn to move round the circle and choose a beat.",
      "Turn to choose a user slot, 21 to 40. Shift + press saves the rhythm there.",
      "Turn to change the clock division.",
      "Turn to set how many of the thirty-two beats are used, 4 to 32.",
      "Turn to set the chosen beat: Rest, Tie, or a velocity from 1 to 127." },
    "Each dot is a beat and its line is the velocity. Red is a rest and green is a tie, which lets the "
    "last note carry on. Each sounding beat takes the next note of the pattern."
};
static const strEditorHelp kChordSeqHelp = {
    "Chord Sequencer",
    { "Turn to choose a sequence, 0 to 5 (0 is blank). Shift + press loads it.",
      "Turn to choose what to edit on the step: duration, key, mode, degree or type.",
      "Turn to choose one of the eighteen steps: six in each of sections A, B and C.",
      "Turn to choose a song slot's section or its repeats.",
      "Turn to choose a slot, 1 to 5. Shift + press saves the sequence there.",
      "Turn to change the chosen attribute. A step with no duration is skipped.",
      "Turn right to arm, left to disarm. An armed sequence starts with the parts, on the downbeat.",
      "Turn to set the slot's section (A, B or C) or its repeats (0 to 8, 0 clears it)." },
    "Each step has a duration, key, mode, degree and type. The song plays its slots in order and loops. "
    "Chord buttons set the chosen step's degree; Shift + chord sets its type and moves on."
};
static const char* const kEditorPress[8] = {
    "Press: Chord Sequencer. Shift + press loads.", "No editor here in this build.",
    "No editor here in this build.",                "MIDI Controller: not in this build.",
    "Press: Pattern Editor. Shift + press saves.",  "Press: Rhythm Editor.",
    "Perlin: not in this build.",                   "No editor here in this build."
};
static constexpr const char* kPatternDialName[8] = { "LOAD", "VAR", "RHY", "STEP", "SAVE", "DIV", "LEN", "VALUE" };
static constexpr const char* kRhythmDialName[8] = { "LOAD", "VAR", "PATT", "BEAT", "SAVE", "DIV", "LEN", "VALUE" };
static constexpr const char* kChordSeqDialName[8] = { "LOAD", "ATTR", "STEP", "SLOT", "SAVE", "VALUE", "ARM", "SONG" };

static const strEditorHelp& editorHelp()
{
    return s.editor == ndlrEnc6 ? kRhythmHelp : s.editor == ndlrEnc1 ? kChordSeqHelp : kPatternHelp;
}

// word-wrap `src` into fixed-stride lines; returns the number of lines written
static int wrap(const char* src, int font, int width, char* dst, int stride, int maxLines)
{
    int lines = 0;
    const char* p = src;
    while (*p && lines < maxLines)
    {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const int maxChars = getMaxTextLengthThatFitSpecificWidth(&fonts[font], p, width);
        int take = maxChars < stride ? maxChars : stride - 1;
        if (take == 0)
            take = 1;
        if (p[take] && p[take] != ' ')
        {
            int cut = take;
            while (cut > 0 && p[cut] != ' ')
                cut--;
            if (cut > 0)
                take = cut;
        }
        memcpy(dst + lines * stride, p, take);
        dst[lines * stride + take] = 0;
        p += take;
        lines++;
    }
    return lines;
}

static void addInfoLines(const char* text, uint8_t kind)
{
    if (s_view.infoCount >= kNdlrInfoLines)
        return;
    const int first = s_view.infoCount;
    const int n = wrap(text, kNdlrFontSmall, 166, &s_view.info[first][0],
                       kNdlrInfoLineLen, kNdlrInfoLines - first);
    for (int i = 0; i < n; i++)
        s_view.infoKind[first + i] = kind;
    s_view.infoCount = (uint8_t)(first + n);
}

static void chordLabels(char* roman, size_t romanSize, char* name, size_t nameSize, int key = s.key,
                        int mode = s.mode, int d = s.degree)
{
    const strMode& m = kModes[mode];
    const int rootSemi = (kKeySemi[key] + m.iv[d % m.n]) % 12;
    snprintf(name, nameSize, "%s", kNoteName[rootSemi]);
    const char* rom = kRomanUpper[d];
    if (m.n == 7)
    {
        const int third = (m.iv[(d + 2) % 7] - m.iv[d] + 12) % 12;
        const int fifth = (m.iv[(d + 4) % 7] - m.iv[d] + 12) % 12;
        if (third == 3)
        {
            snprintf(roman, romanSize, "%s%s", kRomanLower[d], fifth == 6 ? "o" : "");
            return;
        }
    }
    snprintf(roman, romanSize, "%s", rom);
}

// Sweep clockwise from 7:30 to 4:30. Use tenths of a degree for fine tempo changes.
static int16_t dialAngle(int value, int minimum, int maximum)
{
    constexpr int halfSweep = 1350;
    return -halfSweep + (value - minimum) * (2 * halfSweep) / (maximum - minimum);
}

template<typename T, size_t N>
static int16_t optionAngle(int index, const T (&)[N])
{
    return dialAngle(index, 0, N - 1);
}

static int16_t encoderAngle(int enc)
{
    const auto& motif = s.motifSettings[s.motif - 1];
    const int label = s.encLabel[enc];
    switch (enc)
    {
        case 0:
            return label == 0 ? dialAngle(s.padOct, 1, 6) : optionAngle(s.padStrum, kStrumNames);
        case 1:
            return label == 0 ? dialAngle(s.padRange, 0, kMaxPadNotes)
                              : optionAngle(s.padSpread, kSpreadNames);
        case 2:
            if (label == 0) return dialAngle(s.droneOct, 1, 4);
            if (label == 1) return optionAngle(s.droneNotesMode, kDroneNotesNames);
            return optionAngle(s.droneTrigger, kTriggerNames);
        case 3:
            if (label == 0) return dialAngle(clamp<int>(s.trackerClock ? s.trackerTempo : s.tempo, 10, 300), 10, 300);
            if (label == 1) return optionAngle(s.key, kKeyName);
            if (label == 2) return optionAngle(s.mode, kModes);
            if (label == 3) return optionAngle(quantiseMode(), kQuantiseNames);
            return optionAngle(s.internalClock, kClockNames);
        case 4:
            return label == 0 ? dialAngle(motif.octave, 2, 6) : dialAngle(motif.patternLength, 0, 16);
        case 5:
            return label == 0 ? optionAngle(motif.variation, kVariationNames)
                              : dialAngle(motif.pattern, 0, kSlotCount - 1);
        case 6:
            if (label == 0) return optionAngle(motif.division, kDivisionNames);
            if (label == 1) return dialAngle(motif.rhythmLength, 4, 32);
            return dialAngle(motif.gate, 5, 100);
        case 7:
            if (label == 0) return optionAngle(motif.accent, kAccentNames);
            if (label == 1) return dialAngle(motif.rhythm, 0, kSlotCount - 1);
            return dialAngle(s.motifOffset, 0, 3);
        default:
            return 0;
    }
}

// The full value goes in the right column. The miniature screen gets a short value.
static void encoderValue(char* text, size_t size, int enc)
{
    const auto& motif = s.motifSettings[s.motif - 1];
    const int label = s.encLabel[enc];
    switch (enc)
    {
        case 0:
            if (label == 0) snprintf(text, size, "Pad Oct %u", (unsigned)s.padOct);
            else snprintf(text, size, "%s", kStrumNames[s.padStrum]);
            break;
        case 1:
            if (label == 1) snprintf(text, size, "%s", kSpreadNames[s.padSpread]);
            else if (!s.padRange) snprintf(text, size, "Chord (Auto)");
            else if (s.partOn[0] && s.padCount < s.padRange)
                snprintf(text, size, "%u/%u voices", (unsigned)s.padCount, (unsigned)s.padRange);
            else snprintf(text, size, "%u notes", (unsigned)s.padRange);
            break;
        case 2:
            if (label == 0) snprintf(text, size, "Drone Oct %u", (unsigned)s.droneOct);
            else snprintf(text, size, "%s", label == 1 ? kDroneNotesNames[s.droneNotesMode]
                                                       : kTriggerNames[s.droneTrigger]);
            break;
        case 3:
            if (label == 0)
                snprintf(text, size, s.trackerClock ? "Tracker %u" : "Tempo %u",
                         unsigned(s.trackerClock ? s.trackerTempo : s.tempo));
            else if (label == 1) snprintf(text, size, "%s", kKeyName[s.key]);
            else if (label == 2) snprintf(text, size, "%s", kModes[s.mode].name);
            else if (label == 3) snprintf(text, size, "%s", kQuantiseNames[quantiseMode()]);
            else snprintf(text, size, "%s", kClockNames[s.internalClock]);
            break;
        case 4:
            if (label == 0) snprintf(text, size, "M%u Oct %u", (unsigned)s.motif, (unsigned)motif.octave);
            else if (!motif.patternLength) snprintf(text, size, "Length Auto");
            else snprintf(text, size, "%u notes", (unsigned)motif.patternLength);
            break;
        case 5:
            if (label == 0) snprintf(text, size, "%s", kVariationNames[motif.variation]);
            else slotName(text, size, motif.pattern, false);
            break;
        case 6:
            if (label == 0) snprintf(text, size, "Div %s", kDivisionNames[motif.division]);
            else if (label == 1) snprintf(text, size, "%u steps", (unsigned)motif.rhythmLength);
            else snprintf(text, size, "Gate %u%%", (unsigned)motif.gate);
            break;
        case 7:
            if (label == 0) snprintf(text, size, "%s", kAccentNames[motif.accent]);
            else if (label == 1) slotName(text, size, motif.rhythm, true);
            else snprintf(text, size, "%u/16", (unsigned)s.motifOffset);
            break;
    }
}

static void midiName(char* text, size_t size, int note)
{
    snprintf(text, size, "%s%d", kNoteName[note % 12], note / 12 - 1);
}

// True while the main panel is on this dial and label, so the sub screen can highlight it.
static bool editing(int enc, int label)
{
    return s.menuMode == 0 && s.activeEnc == enc && s.encLabel[enc] == label;
}

static const SeqStep& seqStepRef()
{
    return s.seq.sections[s.seqStep / 6][s.seqStep % 6];
}

static const char* beatText(char* text, size_t size, int8_t value)
{
    if (value == kRest)
        snprintf(text, size, "Rst");
    else if (value == kTie)
        snprintf(text, size, "Tie");
    else
        snprintf(text, size, "%d", value);
    return text;
}

// The pattern as sixteen bars up to `maxHeight` pixels tall.
static void fillPatternBars(strNdlrSubView& sub, int index, int maxHeight, int selStep)
{
    const auto& m = s.motifSettings[index];
    const int length = effectivePatternLength(index);
    const int playing = s.partOn[index + 2] ? s.motifPlayback[index].pos : -1;
    const int top = kPatternTypeMax[m.patt.type] - 1;
    sub.countA = kPatternNotes;
    for (int i = 0; i < kPatternNotes; ++i)
    {
        const int value = clamp<int>(m.patt.notes[i], 1, top + 1) - 1;
        sub.barA[i] = static_cast<uint8_t>(3 + (maxHeight - 3) * value / top);
        sub.barAState[i] = i + 1 == selStep ? ndlrBarSelected
                         : i >= length      ? ndlrBarDim
                         : i == playing     ? ndlrBarPlaying
                                            : ndlrBarNormal;
    }
}

static void fillRhythmBars(strNdlrSubView& sub, int index, int selBeat)
{
    const auto& m = s.motifSettings[index];
    const int playing = s.partOn[index + 2] ? s.motifPlayback[index].beat : -1;
    sub.countB = m.rhythmLength;
    for (int i = 0; i < kRhythmBeats; ++i)
    {
        const int8_t value = m.rhy[i];
        sub.barB[i] = value > 0 ? static_cast<uint8_t>(value) : 0;
        sub.barBState[i] = i + 1 == selBeat    ? ndlrBarSelected
                         : value == kRest      ? ndlrBarRest
                         : value == kTie       ? ndlrBarTie
                         : i >= m.rhythmLength ? ndlrBarDim
                         : i == playing        ? ndlrBarPlaying
                                               : ndlrBarNormal;
    }
}

#define SUBTEXT(slot, ...) snprintf(sub.text[slot], kNdlrSubTextLen, __VA_ARGS__)

static void buildPadSub(strNdlrSubView& sub)
{
    int16_t notes[kMaxPadNotes];
    int count = s.padCount;
    if (s.partOn[0] && count > 0)
        memcpy(notes, s.padNotes, sizeof(int16_t) * count);
    else
        count = padVoicing(notes, kMaxPadNotes); // what it will play once it is on
    sub.kind = ndlrSubPad;
    SUBTEXT(0, "PAD");
    SUBTEXT(1, "SYN 1");
    sub.countA = static_cast<uint8_t>(count);
    for (int i = 0; i < count; ++i)
    {
        sub.barA[i] = static_cast<uint8_t>((clamp<int>(notes[i], 24, 108) - 24) * 154 / 84);
        sub.barAState[i] = s.partOn[0] && i < s.padPlayed ? ndlrBarPlaying : ndlrBarNormal;
    }
    if (count > 0)
    {
        midiName(sub.text[2], kNdlrSubTextLen, notes[0]);
        midiName(sub.text[3], kNdlrSubTextLen, notes[count / 2]);
        midiName(sub.text[4], kNdlrSubTextLen, notes[count - 1]);
    }
    SUBTEXT(5, "%d %s%s", count, count == 1 ? "Note" : "Notes", s.padRange ? "" : " (Auto)");
    SUBTEXT(6, "SPD %s", kSpreadNames[s.padSpread]);
    SUBTEXT(7, "SRM %s", kStrumNames[s.padStrum]);
    if (editing(0, 0))
        sub.hiMask |= 1u << 2 | 1u << 3 | 1u << 4;
    if (editing(1, 0))
        sub.hiMask |= 1u << 5;
    if (editing(1, 1))
        sub.hiMask |= 1u << 6;
    if (editing(0, 1))
        sub.hiMask |= 1u << 7;
}

static void buildDroneSub(strNdlrSubView& sub)
{
    int16_t notes[3];
    int count = s.droneCount;
    if (s.partOn[1] && count > 0)
        memcpy(notes, s.droneNotes, sizeof(int16_t) * count);
    else
        count = droneVoicing(notes, 3);
    sub.kind = ndlrSubDrone;
    SUBTEXT(0, "DRONE");
    SUBTEXT(1, "SYN 2");
    SUBTEXT(2, "%s", s.droneTrigger >= 4 ? "KEY" : "CHORD");
    char name[3][6] = { "", "", "" };
    for (int i = 0; i < count; ++i)
        midiName(name[i], sizeof(name[i]), notes[i]);
    SUBTEXT(3, "%s %s %s", name[0], name[1], name[2]);
    sub.a = s.droneOct;
    sub.b = static_cast<uint8_t>(1 | (s.droneNotesMode >= 2 ? 2 : 0) | (s.droneNotesMode & 1 ? 4 : 0));
    static constexpr uint8_t kCadence[4] = { 0x0, 0xf, 0x5, 0x1 }; // held, every beat, 1 and 3, 1 only
    sub.c = kCadence[s.droneTrigger % 4];
    if (s.partOn[1])
        sub.onMask |= 1u << 3;
    if (editing(2, 0))
        sub.hiMask |= 1u << 12;
    if (editing(2, 1))
        sub.hiMask |= 1u << 13;
    if (editing(2, 2))
        sub.hiMask |= 1u << 14;
}

static void buildTkmSub(strNdlrSubView& sub)
{
    const strMode& m = kModes[s.mode];
    const int root = kKeySemi[s.key];
    sub.kind = ndlrSubTkm;
    SUBTEXT(0, "T-K-M");
    SUBTEXT(1, "%s", m.name);
    SUBTEXT(2, "%s", kKeyName[s.key]);
    SUBTEXT(3, "%u", unsigned(s.trackerClock ? s.trackerTempo : s.tempo));
    SUBTEXT(5, "%s", kQuantiseShort[quantiseMode()]);
    SUBTEXT(6, "%s", kClockShort[s.internalClock]);

    // every note of the key and mode, then the chord, then the key root
    size_t used = 0;
    for (int i = 0; i < m.n; ++i)
    {
        const int pc = (root + m.iv[i]) % 12;
        sub.marks[pc] = 1;
        used += snprintf(sub.text[4] + used, kNdlrSubTextLen - used, i ? " %s" : "%s", kNoteName[pc]);
        if (used >= kNdlrSubTextLen)
            break;
    }
    int16_t chord[4];
    const int size = buildChord(3, chord, 4);
    for (int i = 0; i < size; ++i)
        sub.marks[chord[i] % 12] = 2;
    sub.marks[root] = 3;

    if (editing(3, 2))
        sub.hiMask |= 1u << 1;
    if (editing(3, 1))
        sub.hiMask |= 1u << 2;
    if (editing(3, 0))
        sub.hiMask |= 1u << 3;
    if (editing(3, 3))
        sub.hiMask |= 1u << 5;
    if (editing(3, 4))
        sub.hiMask |= 1u << 6;
}

static void buildMotifSub(strNdlrSubView& sub)
{
    const int index = s.motif - 1;
    const auto& m = s.motifSettings[index];
    sub.kind = ndlrSubMotif;
    SUBTEXT(0, "MOTIF-%u", unsigned(s.motif));
    SUBTEXT(1, "SYN %d", index + 3);
    SUBTEXT(2, "%d", m.pattern + 1);
    SUBTEXT(3, "%s", kDivisionNames[m.division]);
    SUBTEXT(4, "OFS%u", unsigned(s.motifOffset));
    SUBTEXT(5, "%d", m.rhythm + 1);
    SUBTEXT(6, "%s", kAccentNames[m.accent]);
    SUBTEXT(7, "%d:%d", effectivePatternLength(index), noteBeats(index));
    SUBTEXT(8, "OCT %u", unsigned(m.octave));
    SUBTEXT(9, "%s", kVariationNames[m.variation]);
    SUBTEXT(10, "G%u", unsigned(m.gate));
    fillPatternBars(sub, index, 20, -1);
    fillRhythmBars(sub, index, -1);
    static constexpr uint8_t kSlotFor[4][3] = { { 8, 7, 0 }, { 9, 2, 0 }, { 3, 7, 10 }, { 6, 5, 4 } };
    if (s.menuMode == 0 && s.activeEnc >= 4)
        sub.hiMask |= 1u << kSlotFor[s.activeEnc - 4][s.encLabel[s.activeEnc]];
}

static void buildEditorSub(strNdlrSubView& sub)
{
    const int index = s.motif - 1;
    const auto& m = s.motifSettings[index];
    char value[8];
    if (kNdlrGeom[s.sel].kind == ndlrKindEnc)
        sub.selCell = s.sel - ndlrEnc1;

    if (s.editor == ndlrEnc1)
    {
        const SeqStep& st = seqStepRef();
        char roman[8], name[4];
        chordLabels(roman, sizeof(roman), name, sizeof(name), st.key, st.mode, st.degree);
        sub.kind = ndlrSubChordSeq;
        SUBTEXT(0, "CHORD SEQUENCER");
        SUBTEXT(2, "%u", unsigned(s.seqLoad));
        SUBTEXT(3, "%u.%u 1/4 Notes", unsigned(st.dur / 2), unsigned(st.dur % 2 * 5));
        SUBTEXT(4, "%s -", kKeyName[st.key]);
        SUBTEXT(5, "%s", kModes[st.mode].name);
        SUBTEXT(6, "%s %s", roman, name);
        SUBTEXT(7, "%s", kTypeName[st.type]);
        SUBTEXT(8, "%u", unsigned(s.seqSave));
        SUBTEXT(9, "%s", s.seqArmed ? "ARMED" : "");
        SUBTEXT(10, "%s", s.seqRunning ? "PLAY" : "");
        sub.hiMask |= 1u << (3 + s.seqAttr);
        sub.onMask |= 1u << 9 | 1u << 10;
        for (int i = 0; i < 18; ++i)
        {
            const bool playing = s.seqRunning && s.seqPlaySection == i / 6 && s.seqPlayStep == i % 6;
            sub.steps[i] = i == s.seqStep ? 2 : playing ? 3 : s.seq.sections[i / 6][i % 6].dur ? 1 : 0;
        }
        for (int i = 0; i < 8; ++i)
        {
            const SeqSlot& slot = s.seq.song[i];
            snprintf(sub.slotText[i], sizeof(sub.slotText[i]), "%cx%u", 'A' + slot.section, unsigned(slot.repeats));
            sub.slotState[i] = s.seqRunning && s.seqPlaySlot == i ? 2 : slot.repeats ? 1 : 0;
        }
        sub.field = s.seqField;
        return;
    }

    const bool rhythm = s.editor == ndlrEnc6;
    const int length = effectivePatternLength(index);
    sub.kind = rhythm ? ndlrSubRhythm : ndlrSubPattern;
    SUBTEXT(0, "%s", rhythm ? "RHYTHM EDITOR" : "PATTERN EDITOR");
    SUBTEXT(1, "Mot%u", unsigned(s.motif));
    SUBTEXT(3, "%s", kVariationShort[m.variation]);
    SUBTEXT(7, "%s", kDivisionNames[m.division]);
    SUBTEXT(11, "%d:%d", length, noteBeats(index));
    if (rhythm)
    {
        SUBTEXT(2, "%d", m.rhythm + 1);
        SUBTEXT(4, "%d", m.pattern + 1);
        SUBTEXT(5, "%u/%u", unsigned(s.reBeat), unsigned(m.rhythmLength));
        SUBTEXT(6, "%u", unsigned(s.reSave + 1));
        SUBTEXT(8, "%u", unsigned(m.rhythmLength));
        SUBTEXT(9, "%s", beatText(value, sizeof(value), m.rhy[s.reBeat - 1]));
        fillRhythmBars(sub, index, s.reBeat);
    }
    else
    {
        SUBTEXT(2, "%d", m.pattern + 1);
        SUBTEXT(4, "%d", m.rhythm + 1);
        if (s.peStep)
            SUBTEXT(5, "%u/%d", unsigned(s.peStep), length);
        else
            SUBTEXT(5, "TYPE");
        SUBTEXT(6, "%u", unsigned(s.peSave + 1));
        if (m.patternLength)
            SUBTEXT(8, "%u", unsigned(m.patternLength));
        else
            SUBTEXT(8, "Auto");
        if (s.peStep)
            SUBTEXT(9, "%u", unsigned(m.patt.notes[s.peStep - 1]));
        else
            SUBTEXT(9, "%s", kPatternTypeNames[m.patt.type]);
        SUBTEXT(10, "%s", kPatternTypeNames[m.patt.type]);
        if (s.peStep == 0)
            sub.hiMask |= 1u << 10;
        fillPatternBars(sub, index, 72, s.peStep);
    }
}

static void buildSub(strNdlrSubView& sub)
{
    memset(&sub, 0, sizeof(sub));
    sub.selCell = 0xff;
    // The other motif, a new chord or the main panel can shorten what the editors point into.
    const int index = s.motif - 1;
    if (s.reBeat > s.motifSettings[index].rhythmLength)
        s.reBeat = s.motifSettings[index].rhythmLength;
    if (s.peStep > effectivePatternLength(index))
        s.peStep = static_cast<uint8_t>(effectivePatternLength(index));
    if (s.menuMode == 2)
        buildEditorSub(sub);
    else if (s.activeEnc <= 1)
        buildPadSub(sub);
    else if (s.activeEnc == 2)
        buildDroneSub(sub);
    else if (s.activeEnc == 3)
        buildTkmSub(sub);
    else
        buildMotifSub(sub);
}

// What a dial holds in the open editor, for the green readout.
static void editorValue(char* text, size_t size, int e)
{
    const int index = s.motif - 1;
    const auto& m = s.motifSettings[index];
    char value[8];
    if (s.editor == ndlrEnc1)
    {
        const SeqStep& st = seqStepRef();
        const SeqSlot& slot = s.seq.song[s.seqField >> 1];
        static constexpr const char* kAttr[5] = { "Duration", "Key", "Mode", "Degree", "Type" };
        switch (e)
        {
            case 0: snprintf(text, size, "Load %u", unsigned(s.seqLoad)); break;
            case 1: snprintf(text, size, "%s", kAttr[s.seqAttr]); break;
            case 2: snprintf(text, size, "Step %c%u", 'A' + s.seqStep / 6, unsigned(s.seqStep % 6 + 1)); break;
            case 3: snprintf(text, size, "Slot %u %s", unsigned((s.seqField >> 1) + 1), s.seqField & 1 ? "repeats" : "section"); break;
            case 4: snprintf(text, size, "Save %u", unsigned(s.seqSave)); break;
            case 5:
                if (s.seqAttr == 0) snprintf(text, size, "%u.%u beats", unsigned(st.dur / 2), unsigned(st.dur % 2 * 5));
                else if (s.seqAttr == 1) snprintf(text, size, "%s", kKeyName[st.key]);
                else if (s.seqAttr == 2) snprintf(text, size, "%s", kModes[st.mode].name);
                else if (s.seqAttr == 3) snprintf(text, size, "%s", kRomanUpper[st.degree]);
                else snprintf(text, size, "%s", kTypeName[st.type]);
                break;
            case 6: snprintf(text, size, "%s", s.seqArmed ? "ARMED" : "Not armed"); break;
            default:
                if (s.seqField & 1) snprintf(text, size, "x%u", unsigned(slot.repeats));
                else snprintf(text, size, "Section %c", 'A' + slot.section);
                break;
        }
        return;
    }
    const bool rhythm = s.editor == ndlrEnc6;
    switch (e)
    {
        case 0: slotName(text, size, rhythm ? m.rhythm : m.pattern, rhythm); break;
        case 1: snprintf(text, size, "%s", kVariationNames[m.variation]); break;
        case 2: slotName(text, size, rhythm ? m.pattern : m.rhythm, !rhythm); break;
        case 3:
            if (rhythm) snprintf(text, size, "Beat %u", unsigned(s.reBeat));
            else if (s.peStep) snprintf(text, size, "Step %u", unsigned(s.peStep));
            else snprintf(text, size, "Note type");
            break;
        case 4: snprintf(text, size, "Slot %u", unsigned((rhythm ? s.reSave : s.peSave) + 1)); break;
        case 5: snprintf(text, size, "%s", kDivisionNames[m.division]); break;
        case 6:
            if (rhythm) snprintf(text, size, "%u beats", unsigned(m.rhythmLength));
            else if (m.patternLength) snprintf(text, size, "Length %u", unsigned(m.patternLength));
            else snprintf(text, size, "Auto (%d)", effectivePatternLength(index));
            break;
        default:
            if (rhythm) snprintf(text, size, "Value %s", beatText(value, sizeof(value), m.rhy[s.reBeat - 1]));
            else if (s.peStep) snprintf(text, size, "Value %u", unsigned(m.patt.notes[s.peStep - 1]));
            else snprintf(text, size, "%s", kPatternTypeNames[m.patt.type]);
            break;
    }
}

static void menuValue(char* text, size_t size, int enc);
static int16_t menuAngle(int enc);

static void rebuildView()
{
    strNdlrPanelView& v = s_view;
    v.selected = s.sel;
    v.shift = shiftOn();
    v.menuMode = s.menuMode;
    for (int i = 0; i < 4; i++)
    {
        v.partOn[i] = s.partOn[i];
        v.modRing[i] = s.modRing[i];
    }
    v.motif = s.motif;
    v.activeEnc = s.activeEnc;
    for (int i = 0; i < 8; i++)
    {
        v.encLabel[i] = s.encLabel[i];
        v.encAngle[i] = s.menuMode == 1 ? menuAngle(i) : encoderAngle(i);
    }
    v.chordDegree = s.degree;
    v.queuedChord = s.queuedChord;
    v.pendingPartToggles = s.pendingPartToggles;
    v.typePending = s.pendingType >= 0;
    v.recording = ndlrRecorder.isRecording();
    v.editor = s.editor;
    buildSub(v.sub);

    char roman[8], name[4];
    chordLabels(roman, sizeof(roman), name, sizeof(name));
    snprintf(v.keyMode, sizeof(v.keyMode), "%s %s", kKeyName[s.key], kModes[s.mode].name);
    memcpy(v.toast, s.toast, sizeof(v.toast));
    if (hasQueuedLaunch() && !(s.recordToast && v.toast[0]))
        snprintf(v.toast, sizeof(v.toast), "Queued for %s", s.quantise == 2 ? "next bar" : "next beat");
    else if (!v.toast[0] && s.capState != capOff)
        snprintf(v.toast, sizeof(v.toast), "%s", s.capState == capArmed ? "CAPTURE ARMED" : "CAPTURING");
    snprintf(v.chordText, sizeof(v.chordText), "%s %s", roman, name);
    snprintf(v.typeText, sizeof(v.typeText), "%s",
             s.pendingType >= 0 ? kTypeName[s.pendingType] : kTypeName[resolvedType()]);
    if (s.trackerClock)
        snprintf(v.bpmText, sizeof(v.bpmText), "%u BPM TRK", unsigned(s.trackerTempo));
    else
        snprintf(v.bpmText, sizeof(v.bpmText), "%u BPM", unsigned(s.tempo));

    // info column + description for the highlighted control
    v.infoCount = 0;
    v.descCount = 0;
    const strNdlrGeom& g = kNdlrGeom[s.sel];

    strInfo chordInfo{};
    const strInfo* inf = &kInfo[s.sel];
    char chordName[24], chordNormal[64], chordShift[64], chordDesc[160];
    if (g.kind == ndlrKindChord)
    {
        const int d = s.sel - ndlrChord1;
        snprintf(chordName, sizeof(chordName), "Chord %s", kRomanUpper[d]);
        snprintf(chordNormal, sizeof(chordNormal), "Play chord degree %s. All parts follow, in key.",
                 kRomanUpper[d]);
        snprintf(chordShift, sizeof(chordShift), "Set chord type %s. Applies on the next chord press.",
                 kTypeName[d]);
        snprintf(chordDesc, sizeof(chordDesc),
                 "Buttons I-VII are the scale degrees. With Shift they pick the type: I triad, II 7th, "
                 "III sus2, IV alt2, V alt1, VI sus4, VII 6th.");
        chordInfo.name = chordName;
        chordInfo.rowCount = 2;
        chordInfo.rows[0] = { imNormal, chordNormal };
        chordInfo.rows[1] = { imShift, chordShift };
        chordInfo.desc = chordDesc;
        inf = &chordInfo;
    }

    uint8_t active = imNormal;
    {
        const uint8_t want = s.menuMode == 2 ? imEditor : s.menuMode == 1 ? imMenu : shiftOn() ? imShift : imNormal;
        bool hasWant = false, hasView = false;
        for (int i = 0; i < inf->rowCount; i++)
        {
            if (inf->rows[i].mode == want)
                hasWant = true;
            if (inf->rows[i].mode == imView)
                hasView = true;
        }
        active = hasView ? uint8_t(imView) : hasWant ? want : uint8_t(imNormal);
    }

    // In an editor each dial has one job: say what it turns and what a press does.
    const bool editorDial = g.kind == ndlrKindEnc && s.menuMode == 2;
    const int dial = s.sel - ndlrEnc1;
    if (editorDial)
    {
        const char* const* names = s.editor == ndlrEnc6 ? kRhythmDialName
                                 : s.editor == ndlrEnc1 ? kChordSeqDialName
                                                        : kPatternDialName;
        snprintf(v.info[0], kNdlrInfoLineLen, "Dial %d - %s", dial + 1, names[dial]);
        v.infoKind[0] = ndlrLineTitle;
        v.infoCount = 1;
        addInfoLines(editorHelp().dial[dial], ndlrLineActive);
        if (v.infoCount < kNdlrInfoLines)
        {
            snprintf(v.info[v.infoCount], kNdlrInfoLineLen, "PRESS");
            v.infoKind[v.infoCount++] = ndlrLineHeader;
        }
        addInfoLines(kEditorPress[dial], ndlrLineText);
    }
    else
    {
        snprintf(v.info[0], kNdlrInfoLineLen, "%s", inf->name);
        v.infoKind[0] = ndlrLineTitle;
        v.infoCount = 1;
    }
    for (int i = 0; !editorDial && i < inf->rowCount; i++)
    {
        const bool act = inf->rows[i].mode == active;
        if (v.infoCount >= kNdlrInfoLines)
            break;
        snprintf(v.info[v.infoCount], kNdlrInfoLineLen, "%s", kModeHeader[inf->rows[i].mode]);
        v.infoKind[v.infoCount++] = ndlrLineHeader;
        addInfoLines(inf->rows[i].text, act ? ndlrLineActive : ndlrLineText);
    }

    const uint8_t enc = s.activeEnc;
    if (s.menuMode == 2)
        snprintf(v.detailText, sizeof(v.detailText), "%s",
                 s.editor == ndlrEnc6 ? "RHY EDITOR" : s.editor == ndlrEnc1 ? "CHORD SEQ" : "PATT EDITOR");
    else if (s.menuMode == 1)
        menuValue(v.detailText, sizeof(v.detailText), enc);
    else
        encoderValue(v.detailText, sizeof(v.detailText), enc);

    char now[40] = "";
    if (g.kind == ndlrKindPart)
        snprintf(now, sizeof(now), "Now: %s", s.partOn[s.sel - ndlrBtnPad] ? "PLAYING" : "paused");
    else if (editorDial)
        editorValue(now, sizeof(now), dial);
    else if (g.kind == ndlrKindEnc && s.menuMode == 0)
        encoderValue(now, sizeof(now), s.sel - ndlrEnc1);
    else if (g.kind == ndlrKindEnc && s.menuMode == 1)
    {
        char value[24];
        menuValue(value, sizeof(value), s.sel - ndlrEnc1);
        snprintf(now, sizeof(now), "Now: %s", value);
    }
    else if (g.kind == ndlrKindChord || g.kind == ndlrKindScreen)
        snprintf(now, sizeof(now), "Now: %s %s %s", roman, name, kTypeName[resolvedType()]);
    else if (s.sel == ndlrBtnMotifSel)
        snprintf(now, sizeof(now), "Now: Motif %u", (unsigned)s.motif);

    if (now[0] && v.infoCount < kNdlrInfoLines)
    {
        snprintf(v.info[v.infoCount], kNdlrInfoLineLen, "%s", now);
        v.infoKind[v.infoCount++] = ndlrLineNow;
    }

    const char* desc = editorDial ? editorHelp().desc : inf->desc ? inf->desc : "";
    v.descCount = (uint8_t)wrap(desc, kNdlrFontNormal, 772, &v.desc[0][0], kNdlrDescLineLen, kNdlrDescLines);
}

//==============================================================================================
// Refresh
//==============================================================================================
static void refreshPanel()
{
    rebuildView();
    display.refreshControl(P->panel);
    display.synchronizeRefresh();
    s.lastRefresh = nowMs();
    s.dirty = 0;
}

//==============================================================================================
// Navigation (dpad)
//==============================================================================================
enum enDir : uint8_t
{
    dirLeft,
    dirRight,
    dirUp,
    dirDown
};

struct strOverride
{
    uint8_t from;
    uint8_t dir;
    uint8_t to;
};
static const strOverride kOverrides[] = {
    { ndlrBtnShift, dirLeft, ndlrBtnDrone },   { ndlrBtnShift, dirRight, ndlrBtnMotif1 },
    { ndlrBtnMenu, dirLeft, ndlrChord5 },      { ndlrBtnMenu, dirRight, ndlrChord4 },
    { ndlrChord3, dirUp, ndlrChord2 },         { ndlrChord6, dirUp, ndlrChord7 },
    { ndlrBtnMotifSel, dirLeft, ndlrChord1 },  { ndlrBtnAll, dirRight, ndlrChord1 },
    { ndlrEnc8, dirDown, ndlrBtnRecord },      { ndlrBtnRecord, dirUp, ndlrEnc8 },
    { ndlrBtnRecord, dirLeft, ndlrEnc8 },
};

static uint8_t neighbour(uint8_t from, uint8_t dir)
{
    for (const strOverride& o : kOverrides)
    {
        if (o.from == from && o.dir == dir)
            return o.to;
    }
    const int dx = dir == dirLeft ? -1 : dir == dirRight ? 1 : 0;
    const int dy = dir == dirUp ? -1 : dir == dirDown ? 1 : 0;
    int best = -1;
    long bestScore = 0x7fffffff;
    for (int i = 0; i < ndlrControlCount; i++)
    {
        if (i == from)
            continue;
        const int vx = kNdlrGeom[i].x - kNdlrGeom[from].x;
        const int vy = kNdlrGeom[i].y - kNdlrGeom[from].y;
        const int along = vx * dx + vy * dy;
        if (along <= 8)
            continue;
        const int perp = (vx * dy < 0 ? -vx * dy : vx * dy) + (vy * dx < 0 ? -vy * dx : vy * dx);
        const long score = (long)along * 10 + (long)perp * 22;
        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    return best >= 0 ? (uint8_t)best : from;
}

static void move(uint8_t dir)
{
    s.sel = neighbour(s.sel, dir);
    if (kNdlrGeom[s.sel].kind == ndlrKindEnc)
    {
        s.activeEnc = s.sel - ndlrEnc1;
    }
    s.dirty = 1;
}

//==============================================================================================
// Press / turn
//==============================================================================================
static void toggleAll()
{
    advanceClock(nowMs());
    updateLaunch();
    const uint8_t playing = playingParts();
    s.resync = true; // ALL restarts both motifs together
    if (s.trackerClock)
    {
        if (!hasQueuedLaunch())
            setLaunchDeadline();
        const uint8_t requested = playing ^ s.pendingPartToggles;
        s.pendingPartToggles = playing ^ (requested ? 0 : kAllParts);
        s.dirty = 1;
    }
    else if (!playing)
    {
        s.barPulse = 0;
        s.tickPhase = 0;
        s.pendingPartToggles = kAllParts;
        applyLaunch(s.pulse);
        toast("All parts playing");
    }
    else if (!s.quantise)
    {
        s.pendingPartToggles = playing;
        applyLaunch(s.pulse);
        toast("All parts paused");
    }
    else
    {
        if (!hasQueuedLaunch())
            setLaunchDeadline();
        s.pendingPartToggles = s.pendingPartToggles == playing ? 0 : playing;
        if (!hasQueuedLaunch())
        {
            s.resync = false;
            toast("Queued stop cancelled");
        }
        s.dirty = 1;
    }
}

static void toggleAllMods()
{
    const bool on = !s.modRing[0];
    for (int i = 0; i < 4; i++)
        s.modRing[i] = on;
    toast("MOD rings: visual only");
}

// Four presses on the panel's SHIFT set the tempo, 40 to 300 BPM.
static void tapShift()
{
    const uint32_t now = nowMs();
    if (s.tapCount && now - s.taps[s.tapCount - 1] > 1600)
        s.tapCount = 0;
    if (s.tapCount == 4)
    {
        memmove(s.taps, s.taps + 1, 3 * sizeof(s.taps[0]));
        s.tapCount = 3;
    }
    s.taps[s.tapCount++] = now;
    if (s.tapCount < 4)
        return;
    const uint32_t span = s.taps[3] - s.taps[0];
    if (span == 0)
        return;
    if (s.trackerClock)
    {
        toast("Following Tracker tempo");
        return;
    }
    s.tempo = static_cast<uint16_t>(clamp<uint32_t>((3 * kMinuteMs + span / 2) / span, 40, 300));
    char text[24];
    snprintf(text, sizeof(text), "Tap tempo %u", unsigned(s.tempo));
    toast(text);
}

// Shift + press dial 1 in an editor: load the stored pattern, rhythm or sequence again.
static void editorLoad()
{
    const int index = s.motif - 1;
    char text[32];
    if (s.editor == ndlrEnc5)
    {
        loadPattern(index);
        snprintf(text, sizeof(text), "Pattern %d loaded", s.motifSettings[index].pattern + 1);
    }
    else if (s.editor == ndlrEnc6)
    {
        loadRhythm(index);
        snprintf(text, sizeof(text), "Rhythm %d loaded", s.motifSettings[index].rhythm + 1);
    }
    else
    {
        s.seq = s_sequences[s.seqLoad];
        s.seqIndex = -1;
        snprintf(text, sizeof(text), "Chord Seq %u loaded", unsigned(s.seqLoad));
    }
    toast(text);
}

// Shift + press dial 5 in an editor: save to the chosen user slot.
static void editorSave()
{
    const int index = s.motif - 1;
    auto& m = s.motifSettings[index];
    char text[32];
    if (s.editor == ndlrEnc5)
    {
        m.patt.autoLength = static_cast<uint8_t>(effectivePatternLength(index));
        s_userPatterns[s.peSave - kPresetCount] = m.patt;
        m.pattern = s.peSave;
        snprintf(text, sizeof(text), "Saved to pattern %u", unsigned(s.peSave + 1));
    }
    else if (s.editor == ndlrEnc6)
    {
        memcpy(s_userRhythms[s.reSave - kPresetCount], m.rhy, kRhythmBeats);
        m.rhythm = s.reSave;
        snprintf(text, sizeof(text), "Saved to rhythm %u", unsigned(s.reSave + 1));
    }
    else
    {
        s_sequences[s.seqSave] = s.seq;
        snprintf(text, sizeof(text), "Saved Chord Seq %u", unsigned(s.seqSave));
    }
    toast(text);
}

static void pressSelected()
{
    const uint8_t c = s.sel;
    const strNdlrGeom& g = kNdlrGeom[c];
    const bool sh = shiftOn();

    switch (g.kind)
    {
        case ndlrKindEnc:
        {
            const int e = c - ndlrEnc1;
            s.activeEnc = e;
            if (s.menuMode == 1)
            {
                if (e >= 4)
                    toggleCapture();
                else
                    toast("Turn to pick Synth or MIDI ch");
            }
            else if (s.menuMode == 2)
            {
                // In Editor mode a press changes editor; Shift + press on dials 1 and 5 loads and saves.
                if (sh && e == 0)
                    editorLoad();
                else if (sh && e == 4)
                    editorSave();
                else if (c == ndlrEnc1 || c == ndlrEnc5 || c == ndlrEnc6)
                    s.editor = c;
                else
                    toast(e == 3   ? "MIDI Controller: not built yet"
                          : e == 6 ? "Perlin: not in this build"
                                   : "No editor on this dial");
            }
            else
            {
                s.encLabel[e] = (uint8_t)((s.encLabel[e] + 1) % kNdlrEncLabelCount[e]);
            }
            break;
        }
        case ndlrKindChord:
        {
            const int d = c - ndlrChord1;
            // In the Chord Sequencer the chord buttons also write to the chosen step.
            const bool inSeq = s.menuMode == 2 && s.editor == ndlrEnc1;
            SeqStep& st = s.seq.sections[s.seqStep / 6][s.seqStep % 6];
            if (sh)
            {
                if (inSeq)
                {
                    st.type = static_cast<uint8_t>(d);
                    s.seqStep = (s.seqStep + 1) % 18;
                }
                else
                {
                    s.pendingType = (int8_t)d;
                    toast("Chord type set for next chord");
                }
            }
            else
            {
                if (inSeq)
                    st.degree = static_cast<uint8_t>(d);
                selectChord(d);
            }
            break;
        }
        case ndlrKindPart:
        {
            const int p = c - ndlrBtnPad;
            if (sh)
                s.modRing[p] = !s.modRing[p];
            else
                togglePart(p);
            break;
        }
        case ndlrKindShift:
            s.shiftLatched = !s.shiftLatched;
            tapShift();
            break;
        case ndlrKindRecord: P->toggleRecording(); break;
        case ndlrKindBtn:
        {
            if (c == ndlrBtnAll)
            {
                sh ? toggleAllMods() : toggleAll();
            }
            else if (c == ndlrBtnMotifSel)
            {
                if (sh)
                {
                    s.pendingPartToggles = 0;
                    s.queuedChord = 0;
                    memset(s.partOn, 0, sizeof(s.partOn));
                    allNotesOff();
                    P->reserveVoices(0, 0);
                    P->reserveVoices(1, 0);
                    toast("PANIC: all parts stopped");
                }
                else
                {
                    s.motif = s.motif == 1 ? 2 : 1;
                }
            }
            else if (c == ndlrBtnMenu)
            {
                if (sh)
                    s.menuMode = s.menuMode == 2 ? 0 : 2;
                else
                    s.menuMode = s.menuMode == 1 ? 0 : 1;
            }
            break;
        }
        default: break;
    }
    s.dirty = 1;
}

template<typename T>
static bool adjust(T& value, int delta, int low, int high)
{
    const int next = static_cast<int>(value) + delta;
    const T bounded = static_cast<T>(next < low ? low : next > high ? high : next);
    const bool changed = bounded != value;
    value = bounded;
    return changed;
}

template<typename T, size_t N>
static bool cycle(T& value, int direction, const char* const (&)[N])
{
    const T next = static_cast<T>((static_cast<int>(value) + direction + static_cast<int>(N)) % N);
    const bool changed = next != value;
    value = next;
    return changed;
}

template<typename T>
static bool cycleCount(T& value, int direction, int count)
{
    const T next = static_cast<T>((static_cast<int>(value) + direction + count) % count);
    const bool changed = next != value;
    value = next;
    return changed;
}

// Motif settings that the main panel and the editors both turn.
static void turnVariation(int index, int dir)
{
    if (cycle(s.motifSettings[index].variation, dir, kVariationNames))
        s.motifPlayback[index].patternStep = 0;
}

static void turnDivision(int index, int dir)
{
    auto& motif = s.motifSettings[index];
    if (cycle(motif.division, dir, kDivisionNames))
        s.motifPlayback[index].nextStep = s.pulse + kDivisionPulses[motif.division];
}

static void turnPatternLength(int index, int dir)
{
    auto& motif = s.motifSettings[index];
    if (motif.patternLength == 0)
        motif.patternLength = static_cast<uint8_t>(effectivePatternLength(index));
    if (adjust(motif.patternLength, dir, 0, kPatternNotes))
        s.motifPlayback[index].patternStep = 0;
}

static void turnRhythmLength(int index, int n)
{
    auto& motif = s.motifSettings[index];
    if (adjust(motif.rhythmLength, n, 4, kRhythmBeats))
        s.motifPlayback[index].rhythmStep %= motif.rhythmLength;
}

static void turnPattern(int index, int dir)
{
    if (adjust(s.motifSettings[index].pattern, dir, 0, kSlotCount - 1))
        loadPattern(index);
}

static void turnRhythm(int index, int dir)
{
    if (adjust(s.motifSettings[index].rhythm, dir, 0, kSlotCount - 1))
        loadRhythm(index);
}

static void editorTurn(int e, int n)
{
    const int dir = n > 0 ? 1 : -1;
    const int index = s.motif - 1;
    auto& m = s.motifSettings[index];

    if (s.editor == ndlrEnc5) // Pattern Editor
    {
        switch (e)
        {
            case 0: turnPattern(index, dir); break;
            case 1: turnVariation(index, dir); break;
            case 2: turnRhythm(index, dir); break;
            case 3: adjust(s.peStep, dir, 0, effectivePatternLength(index)); break;
            case 4: adjust(s.peSave, dir, kPresetCount, kSlotCount - 1); break;
            case 5: turnDivision(index, dir); break;
            case 6: turnPatternLength(index, dir); break;
            default:
                if (s.peStep == 0)
                {
                    // the note type; values above the new type's range are brought down to it
                    if (adjust(m.patt.type, dir, 0, ptCount - 1))
                    {
                        for (auto& note : m.patt.notes)
                            note = note > kPatternTypeMax[m.patt.type] ? kPatternTypeMax[m.patt.type] : note;
                    }
                }
                else
                    adjust(m.patt.notes[s.peStep - 1], n, 1, kPatternTypeMax[m.patt.type]);
                break;
        }
        const int length = effectivePatternLength(index);
        if (s.peStep > length)
            s.peStep = static_cast<uint8_t>(length);
    }
    else if (s.editor == ndlrEnc6) // Rhythm Editor
    {
        switch (e)
        {
            case 0: turnRhythm(index, dir); break;
            case 1: turnVariation(index, dir); break;
            case 2: turnPattern(index, dir); break;
            case 3: adjust(s.reBeat, dir, 1, m.rhythmLength); break;
            case 4: adjust(s.reSave, dir, kPresetCount, kSlotCount - 1); break;
            case 5: turnDivision(index, dir); break;
            case 6: turnRhythmLength(index, n); break;
            default:
            {
                // the Value dial runs Rest, Tie, then velocities 1 to 127
                int8_t& beat = m.rhy[s.reBeat - 1];
                const int position = clamp<int>((beat == kRest ? 0 : beat == kTie ? 1 : beat + 1) + n, 0, 128);
                beat = static_cast<int8_t>(position == 0 ? kRest : position == 1 ? kTie : position - 1);
                break;
            }
        }
        if (s.reBeat > m.rhythmLength)
            s.reBeat = m.rhythmLength;
    }
    else // Chord Sequencer
    {
        SeqStep& st = s.seq.sections[s.seqStep / 6][s.seqStep % 6];
        switch (e)
        {
            case 0: adjust(s.seqLoad, dir, 0, kSequenceCount - 1); break;
            case 1: adjust(s.seqAttr, dir, 0, 4); break;
            case 2: adjust(s.seqStep, dir, 0, 17); break;
            case 3: adjust(s.seqField, dir, 0, 15); break;
            case 4: adjust(s.seqSave, dir, 1, kSequenceCount - 1); break;
            case 5:
                if (s.seqAttr == 0) adjust(st.dur, n, 0, 63);
                else if (s.seqAttr == 1) cycleCount(st.key, dir, 12);
                else if (s.seqAttr == 2) cycleCount(st.mode, dir, kModeCount);
                else if (s.seqAttr == 3) adjust(st.degree, dir, 0, 6);
                else adjust(st.type, dir, 0, ctCount - 1);
                break;
            case 6:
                if (dir > 0)
                {
                    // arming again sends a playing sequence back to its start
                    int slot, section, step;
                    s.seqArmed = true;
                    s.seqArmPulse = s.pulse;
                    seqStop();
                    toast(seqPlaylist(-1, &slot, &section, &step) ? "Chord Seq armed" : "Armed, but the song is empty");
                }
                else
                {
                    s.seqArmed = false;
                    toast(s.seqRunning ? "Disarmed: plays until you stop" : "Chord Seq disarmed");
                }
                break;
            default:
            {
                SeqSlot& slot = s.seq.song[s.seqField >> 1];
                if (s.seqField & 1)
                    adjust(slot.repeats, dir, 0, 8);
                else
                    cycleCount(slot.section, dir, 3);
                break;
            }
        }
    }
}

//==============================================================================================
// Menu mode: outputs on dials 1-4, capture on dials 5-8
//==============================================================================================
static void menuValue(char* text, size_t size, int enc)
{
    switch (enc)
    {
        case 0:
        case 1:
        case 2:
        case 3:
            if (external(enc)) snprintf(text, size, "MIDI ch %u", unsigned(s.partOut[enc]));
            else snprintf(text, size, "Synth %d", enc + 1);
            break;
        case 4:
            if (s.capState == capArmed) snprintf(text, size, "Armed");
            else if (s.capState == capRunning) snprintf(text, size, "Capturing");
            else snprintf(text, size, "%s", kCaptureNames[s.capParts]);
            break;
        case 5:
            snprintf(text, size, "Tracks %u-%d", unsigned(s.capTrack + 1), captureLastTrack() + 1);
            break;
        case 6: snprintf(text, size, "Pad %u tracks", unsigned(s.padTracks)); break;
        default: snprintf(text, size, "Strum %s", s.strumKeep ? "kept" : "ignored"); break;
    }
}

static int16_t menuAngle(int enc)
{
    switch (enc)
    {
        case 0:
        case 1:
        case 2:
        case 3: return dialAngle(s.partOut[enc], 0, kMidiChannels);
        case 4: return optionAngle(s.capParts, kCaptureNames);
        case 5: return dialAngle(s.capTrack, 0, kAllTracks - 1);
        case 6: return dialAngle(s.padTracks, 1, kMaxPadTracks);
        default: return dialAngle(s.strumKeep, 0, 1);
    }
}

// Changing where a part plays ends its notes first, so none is left hanging on the old output.
static void setOutput(int part, int dir)
{
    uint8_t out = s.partOut[part];
    if (!adjust(out, dir, 0, kMidiChannels))
        return;
    if (part == 0)
        releasePad();
    else if (part == 1)
        releaseDrone();
    else
        releaseMotif(part - 2);
    s.partOut[part] = out;
    P->reserveVoices(part, 0);
    if (part == 0 && s.partOn[0])
        voicePad();
    else if (part == 1 && s.partOn[1])
        voiceDrone();
    if (out && mtConfig.midi.notesOutMode == notesOut_mode_None)
        toast("Turn on Config > MIDI notes out");
}

static void menuTurn(int e, int dir)
{
    if (e < 4)
    {
        setOutput(e, dir);
        return;
    }
    if (s.capState != capOff)
    {
        toast("Stop the capture first");
        return;
    }
    if (e == 4)
        cycle(s.capParts, dir, kCaptureNames);
    else if (e == 5)
        adjust(s.capTrack, dir, 0, kAllTracks - 1);
    else if (e == 6)
        adjust(s.padTracks, dir, 1, kMaxPadTracks);
    else
        s.strumKeep = !s.strumKeep;
}

static void turn(int n)
{
    if (kNdlrGeom[s.sel].kind != ndlrKindEnc)
    {
        toast("Select an encoder to turn it");
        return;
    }
    const uint32_t now = nowMs();
    advanceClock(now);
    updateLaunch();
    const int e = s.sel - ndlrEnc1;
    const int label = s.encLabel[e];
    const int dir = n > 0 ? 1 : -1;
    const int index = s.motif - 1;
    auto& motif = s.motifSettings[index];
    s.activeEnc = e;

    if (s.menuMode == 2)
    {
        editorTurn(e, n);
        s.dirty = 1;
        return;
    }
    if (s.menuMode == 1)
    {
        menuTurn(e, dir);
        s.dirty = 1;
        return;
    }

    switch (e)
    {
        case 0:
            if (label == 0 ? adjust(s.padOct, dir, 1, 6) : cycle(s.padStrum, dir, kStrumNames))
                voicePad();
            break;
        case 1:
            if (label == 0)
            {
                if (s.padRange == 0)
                    s.padRange = kShape[resolvedType()].n;
                if (adjust(s.padRange, dir, 0, kMaxPadNotes))
                    voicePad();
            }
            else if (cycle(s.padSpread, dir, kSpreadNames))
                voicePad();
            break;
        case 2:
        {
            const bool changed = label == 0 ? adjust(s.droneOct, dir, 1, 4)
                               : label == 1 ? cycle(s.droneNotesMode, dir, kDroneNotesNames)
                                            : cycle(s.droneTrigger, dir, kTriggerNames);
            if (changed)
                restartDrone();
            break;
        }
        case 3:
            if (label == 0)
            {
                if (s.trackerClock)
                    toast("Following Tracker tempo");
                else
                    adjust(s.tempo, n, 10, 300);
            }
            else if (label == 1)
            {
                s.key = (s.key + dir + 12) % 12;
                revoice();
            }
            else if (label == 2)
            {
                s.mode = (s.mode + dir + kModeCount) % kModeCount;
                revoice();
            }
            else if (label == 3)
            {
                const auto quantise = clamp<int>(quantiseMode() + dir, s.trackerClock ? 1 : 0, 2);
                if (quantise != quantiseMode())
                {
                    s.quantise = quantise;
                    if (hasQueuedLaunch())
                    {
                        if (quantiseLaunch())
                            setLaunchDeadline();
                        else
                            applyLaunch(s.pulse);
                    }
                }
            }
            else if (adjust(s.internalClock, dir, 0, 1))
            {
                advanceClock(now);
            }
            break;
        case 4:
            if (label == 0)
                adjust(motif.octave, dir, 2, 6);
            else
                turnPatternLength(index, dir);
            break;
        case 5:
            if (label == 0)
                turnVariation(index, dir);
            else
                turnPattern(index, dir);
            break;
        case 6:
            if (label == 0)
                turnDivision(index, dir);
            else if (label == 1)
                turnRhythmLength(index, n);
            else
                adjust(motif.gate, n, 5, 100);
            break;
        case 7:
            if (label == 0)
                cycle(motif.accent, dir, kAccentNames);
            else if (label == 1)
                turnRhythm(index, dir);
            else if (adjust(s.motifOffset, dir, 0, 3))
            {
                resetMotif(0);
                resetMotif(1);
            }
            break;
    }
    s.dirty = 1;
}

//==============================================================================================
// Button callbacks
//==============================================================================================
static void functUp() { move(dirUp); }
static void functDown() { move(dirDown); }
static void functLeft() { move(dirLeft); }
static void functRight() { move(dirRight); }
static void functMiddle(uint8_t state)
{
    if (state == buttonRelease)
    {
        s.middleHandled = false;
    }
    else if (state == buttonPress && !s.middleHandled)
    {
        s.middleHandled = true;
        pressSelected();
    }
}

static void functPlay()
{
    if (!mtMenuPopup.isPopupVisible())
    {
        shiftOn() ? toggleAllMods() : toggleAll();
    }
}

static void functEncoder(int16_t value)
{
    if (value != 0)
    {
        turn(value);
    }
}

static void functSwitchModule(uint8_t button)
{
    P->eventFunct(eventSwitchModule, P, &button, 0);
}

static void functMenu(uint8_t state)
{
    if (state == buttonPress)
    {
        mtMenuPopup.showModulePopup(P);
    }
    else if (state == buttonRelease)
    {
        mtMenuPopup.hideModulePopup(P);
    }
}

//==============================================================================================
// Module
//==============================================================================================
void cNdlrPanel::start(uint32_t options)
{
    (void)options;
    moduleRefresh = 1;

    songTimer.hide();
    titleBar->hide();

    ip->setButtonObj(interfaceButtonPerformance, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonParams, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonFile, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonConfig, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonMaster, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSamplePlay, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleEdit, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleRec, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleLoad, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSong, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonPattern, buttonPress, functSwitchModule);

    ip->setButtonObj(interfaceButtonMiniMenu, functMenu);

    ip->setButtonObj(interfaceButtonUp, buttonPress, functUp);
    ip->setButtonObj(interfaceButtonDown, buttonPress, functDown);
    ip->setButtonObj(interfaceButtonLeft, buttonPress, functLeft);
    ip->setButtonObj(interfaceButtonRight, buttonPress, functRight);
    ip->setButtonObj(interfaceButtonMiniMiddle, functMiddle);
    ip->setButtonObj(interfaceButtonPlay, buttonPress, functPlay);

    // The Mini's +/- buttons feed the standard virtual encoder.
    ip->setPotObj(interfacePot0, functEncoder);

    if (!sessionActive)
    {
        sessionActive = true;
        resetSession();
    }

    s.middleHandled = buttons->isButtonPressed(interfaceButtonMiniMiddle);
    s.previousShift = shiftOn();
    display.setControlData(panel, &s_view);
    display.setControlShow(panel);
    refreshPanel();
}

void cNdlrPanel::resetSession()
{
    const bool internalClock = s.internalClock;
    uint8_t partOut[4];
    memcpy(partOut, s.partOut, sizeof(partOut));
    const uint8_t capParts = s.capParts, capTrack = s.capTrack, padTracks = s.padTracks;
    const bool strumKeep = s.strumKeep;
    if (s.capState == capRunning)
        ndlrCapture::finish();
    memset(&s, 0, sizeof(s));
    s.internalClock = internalClock;
    memcpy(s.partOut, partOut, sizeof(partOut));
    s.capParts = capParts;
    // Tracks 9 on are the MIDI tracks, often free beside the sample tracks.
    s.capTrack = padTracks ? capTrack : 8;
    s.padTracks = padTracks ? padTracks : 2;
    s.strumKeep = padTracks ? strumKeep : true;
    s.capLastPos = UINT32_MAX;
    memset(&s_view, 0, sizeof(s_view));
    s.sel = ndlrEnc1;
    s.motif = 1;
    s.degree = 0;
    s.type = ctTriad;
    s.pendingType = -1;
    s.tempo = 90;
    s.padOct = 3;
    s.droneOct = 2;
    s.motifSettings[0] = { 4, 0, 0, 0, 5, 8, 100, 0, 0, {}, {} };
    s.motifSettings[1] = { 5, 0, 1, 0, 3, 8, 100, 0, 0, {}, {} };
    s.motifPlayback[0].note = s.motifPlayback[1].note = -1;
    s.motifPlayback[0].pos = s.motifPlayback[1].pos = -1;
    s.motifPlayback[0].beat = s.motifPlayback[1].beat = -1;
    initStore();
    for (int index = 0; index < 2; ++index)
    {
        loadPattern(index);
        loadRhythm(index);
    }
    s.editor = ndlrEnc5; // the Pattern Editor opens first
    s.peStep = 1;
    s.peSave = kPresetCount;
    s.reBeat = 1;
    s.reSave = kPresetCount;
    s.seq = s_sequences[1];
    s.seqLoad = 1;
    s.seqSave = 1;
    s.seqPlaySection = s.seqPlayStep = s.seqPlaySlot = -1;
    s.random = nowMs() | 1u;
    s.lastTickUpdate = nowMs();

    // Start with the project allocations. Each part can borrow spare voices on demand.
    for (int part = 0; part < 4; ++part)
        s.voices[part] = mtProject.values.synthPart[part].voiceCount;
    advanceClock(s.lastTickUpdate);
}

void cNdlrPanel::stop()
{
    if (const char* result = ndlrRecorder.stop())
        toast(result, true);
    moduleRefresh = 0;
    display.setControlHide(panel);
    panel = -1;
    songTimer.show();
}

void cNdlrPanel::endSession()
{
    if (!sessionActive)
        return;
    if (s.capState == capRunning)
        finishCapture(false);
    s.capState = capOff;
    sessionActive = false;
    s.pendingPartToggles = 0;
    s.queuedChord = 0;
    allNotesOff();
    for (int part = 0; part < 4; ++part)
        reserveVoices(part, 0);
}

void cNdlrPanel::update()
{
    if (const char* result = ndlrRecorder.update())
        toast(result, true);

    const uint32_t now = nowMs();

    const bool shifted = shiftOn();
    if (shifted != s.previousShift)
    {
        s.previousShift = shifted;
        s.dirty = 1;
    }

    if (s.toast[0] != 0 && static_cast<int32_t>(now - s.toastUntil) >= 0)
    {
        s.toast[0] = 0;
        s.dirty = 1;
    }

    updateJam();

    if (s.dirty && !mtMenuPopup.isPopupVisible() && (now - s.lastRefresh) >= 30 &&
        display.isControlsRefreshProcessIdle())
    {
        refreshPanel();
    }
}

bool cNdlrPanel::toggleBackgroundPlayback()
{
    if (!sessionActive)
        return false;
    toggleAll();
    return true;
}

void cNdlrPanel::synthChanging(int part)
{
    if (!sessionActive)
        return;
    if (part == 0)
        releasePad();
    else if (part == 1)
        releaseDrone();
    else
        releaseMotif(part - 2);
    s.synthChanges |= 1u << part;
}

void cNdlrPanel::updateJam()
{
    if (!sessionActive)
        return;

    const uint32_t now = nowMs();
    advanceClock(now);
    if (s.synthChanges & 1)
        voicePad();
    if (s.synthChanges & 2)
        voiceDrone();
    for (int part = 2; part < 4; ++part)
    {
        if (s.synthChanges & (1u << part))
            reserveVoices(part, 0);
    }
    s.synthChanges = 0;
    musicTick();
}

void cNdlrPanel::reserveVoices(int part, int wanted)
{
    const int configured = mtProject.values.synthPart[part].voiceCount;
    int voices = configured;
    if (configured > 0 && wanted > configured)
    {
        const int available = synth->getMaxVoiceCount(part);
        if (available > configured)
            voices = wanted < available ? wanted : available;
    }
    synth->setVoiceCount(part, voices);
    s.voices[part] = static_cast<uint8_t>(voices);
}

void cNdlrPanel::toggleRecording()
{
    if (ndlrRecorder.isRecording())
        toast(ndlrRecorder.stop(), true);
    else if (newFileManager.getStatus() != fmIdle)
        toast("SD card busy", true);
    else
        toast(ndlrRecorder.start(*platform->getFileSystem()), true);
}

uint32_t cNdlrPanel::nowMs() const
{
    return platform->getTimer()->millis();
}

void cNdlrPanel::hideControlsUnderMiniMenu()
{
    display.setControlHide(panel);
}

void cNdlrPanel::initDisplayControls()
{
    strControlProperties prop;
    prop.colors = nullptr;
    prop.text = nullptr;
    prop.data = nullptr;
    prop.style = controlStyleShow;
    prop.value = 0;
    prop.x = 0;
    prop.y = 0;
    prop.w = 800;
    prop.h = 480;
    panel = display.createControl<cNdlrPanelControl>(&prop);
}

#endif // TRACKER_MINI
