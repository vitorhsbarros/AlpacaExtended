#ifdef TRACKER_MINI

#include "NdlrCapture.h"

#include "fileManager.h"
#include "interruptsControl.h"
#include "mtChordDefs.h"
#include "mtSequencer.h"
#include "patternEditor/patternEditor.h"

namespace ndlrCapture
{
namespace
{
using Step = Sequencer::strPattern::strTrack::strStep;

struct State
{
    bool active = false;
    uint16_t steps = 0;
    PartLayout layout[kParts]{};
    int16_t openStep[kAllTracks]{}; // the step of the note a track is holding, or -1
    int16_t offStep[kAllTracks]{};  // the step of the last note-off written on a track, or -1
    Result result{};
};
State st;

int popcount(uint32_t value)
{
    int bits = 0;
    for (; value; value &= value - 1)
        ++bits;
    return bits;
}

// The intervals above the lowest note, as a Chord FX bitmask.
uint32_t intervalMask(const int16_t* notes, int count)
{
    uint32_t mask = 0;
    for (int i = 0; i < count; ++i)
    {
        const int interval = notes[i] - notes[0];
        if (interval >= 0 && interval < CHORD_FOR_MAX)
            mask |= 1u << interval;
    }
    return mask;
}

// The Chord FX shape with the most notes that all appear in `mask`, or zero.
uint8_t bestShape(uint32_t mask, bool exact)
{
    uint8_t best = 0;
    int bestNotes = 0;
    for (uint8_t shape = MIN_CHORD + 1; shape <= MAX_CHORD; ++shape)
    {
        const uint32_t def = chordDef[shape];
        if (exact ? def != mask : (def & mask) != def)
            continue;
        const int notes = popcount(def);
        if (notes > bestNotes)
        {
            best = shape;
            bestNotes = notes;
        }
    }
    return best;
}

Step* stepAt(int track, int32_t pulse)
{
    if (pulse < 0 || pulse >= int32_t(st.steps) * kPulsesPerStep)
        return nullptr;
    return &sequencer.getActualPattern()->track[track].step[pulse / kPulsesPerStep];
}

void clearStep(Step& step)
{
    step.note = Sequencer::STEP_NOTE_EMPTY;
    step.instrument = 0;
    for (auto& fx : step.fx)
        fx = { Sequencer::strFxConsts::FX_TYPE_NONE, 0 };
    step.volume = Sequencer::STEP_VELO_DEFAULT;
    step.micromove = -1;
}

void write(int part, int slot, int32_t pulse, int16_t note, uint8_t velocity, uint8_t chordFx)
{
    const PartLayout& layout = st.layout[part];
    if (layout.firstTrack < 0 || note < 0 || note > Sequencer::STEP_NOTE_MAX)
        return;
    if (slot >= layout.tracks)
    {
        ++st.result.dropped;
        return;
    }
    const int track = layout.firstTrack + slot;
    Step* step = stepAt(track, pulse);
    if (step == nullptr)
        return;

    // The step's micromove is a percentage of the step, so a pulse is 100 / 12 of it.
    const int offset = pulse % kPulsesPerStep;
    const int16_t index = static_cast<int16_t>(pulse / kPulsesPerStep);
    DISABLE_AUDIO_INTERRUPTS();
    // A note re-voiced on the step it started on takes back the note-off written after it.
    const int16_t off = st.offStep[track];
    if (off > index && sequencer.getActualPattern()->track[track].step[off].note == Sequencer::STEP_NOTE_OFF_DEFAULT)
        sequencer.getActualPattern()->track[track].step[off].note = Sequencer::STEP_NOTE_EMPTY;
    st.offStep[track] = -1;
    clearStep(*step);
    step->note = static_cast<int8_t>(note);
    step->instrument = layout.instrument;
    // Step volume is 0-100; MIDI and synth steps send it as velocity 0-127.
    step->volume = static_cast<int8_t>((velocity * 100 + 63) / 127);
    step->micromove = static_cast<int8_t>(offset ? (offset * 100 + kPulsesPerStep / 2) / kPulsesPerStep - 1 : -1);
    if (chordFx)
        step->fx[0] = { Sequencer::strFxConsts::FX_TYPE_MIDI_CHORD, chordFx };
    ENABLE_AUDIO_INTERRUPTS();

    st.openStep[track] = index;
    ++st.result.notes;
    if (index + 1 > st.result.lastStep)
        st.result.lastStep = static_cast<uint16_t>(index + 1);
}
} // namespace

uint8_t chordFxFor(const int16_t* notes, int count)
{
    if (count < 2 || notes[count - 1] - notes[0] >= CHORD_FOR_MAX)
        return 0;
    return bestShape(intervalMask(notes, count), true);
}

void begin(const PartLayout (&layout)[kParts], uint16_t steps, bool setLength)
{
    newFileManager.storePatternUndoRevision();
    if (setLength && steps > 0 && steps <= Sequencer::MAXSTEP + 1)
    {
        DISABLE_AUDIO_INTERRUPTS();
        for (auto& track : sequencer.getActualPattern()->track)
            track.length = static_cast<uint8_t>(steps - 1);
        ENABLE_AUDIO_INTERRUPTS();
    }
    st = State{};
    st.active = true;
    st.steps = steps;
    for (int track = 0; track < kAllTracks; ++track)
        st.openStep[track] = st.offStep[track] = -1;

    auto* pattern = sequencer.getActualPattern();
    for (int part = 0; part < kParts; ++part)
    {
        st.layout[part] = layout[part];
        if (layout[part].firstTrack < 0)
            continue;
        for (int i = 0; i < layout[part].tracks; ++i)
        {
            auto& track = pattern->track[layout[part].firstTrack + i];
            DISABLE_AUDIO_INTERRUPTS();
            for (int s = 0; s < steps; ++s)
                clearStep(track.step[s]);
            ENABLE_AUDIO_INTERRUPTS();
        }
    }
    newFileManager.setActualPatternStructChanged();
}

bool active()
{
    return st.active;
}

void chord(int part, int32_t pulse, const int16_t* notes, int count, uint8_t velocity)
{
    if (!st.active || count <= 0)
        return;

    // Track by track, the lowest note left and the biggest Chord FX shape built on it from the
    // notes left. A chord that is one shape takes one track; a spread voicing such as a low root
    // under a close chord takes two.
    bool used[16] = {};
    const int total = count < 16 ? count : 16;
    int left = total;
    for (int slot = 0; left > 0; ++slot)
    {
        if (slot >= st.layout[part].tracks)
        {
            st.result.dropped += static_cast<uint16_t>(left);
            return;
        }
        int base = 0;
        while (used[base])
            ++base;
        uint32_t mask = 0;
        for (int i = base; i < total; ++i)
        {
            const int interval = notes[i] - notes[base];
            if (!used[i] && interval >= 0 && interval < CHORD_FOR_MAX)
                mask |= 1u << interval;
        }
        const uint8_t fx = bestShape(mask, false);
        uint32_t wanted = fx ? chordDef[fx] : 1u;
        for (int i = base; i < total; ++i)
        {
            const int interval = notes[i] - notes[base];
            if (used[i] || interval < 0 || interval >= CHORD_FOR_MAX || !(wanted & (1u << interval)))
                continue;
            wanted &= ~(1u << interval); // a doubled pitch keeps its own track
            used[i] = true;
            --left;
        }
        write(part, slot, pulse, notes[base], velocity, fx);
    }
}

void note(int part, int slot, int32_t pulse, int16_t value, uint8_t velocity)
{
    if (st.active)
        write(part, slot, pulse, value, velocity, 0);
}

void release(int part, int32_t pulse)
{
    const PartLayout& layout = st.layout[part];
    if (!st.active || layout.firstTrack < 0 || pulse < 0)
        return;
    for (int i = 0; i < layout.tracks; ++i)
    {
        const int track = layout.firstTrack + i;
        if (st.openStep[track] < 0)
            continue;
        // A note-off on the step the note starts on would cut it, so it goes on the next step at
        // the earliest. A note written later on the same step replaces the note-off.
        int step = pulse / kPulsesPerStep;
        if (step <= st.openStep[track])
            step = st.openStep[track] + 1;
        st.openStep[track] = -1;
        if (step >= st.steps)
            continue;
        Step& target = sequencer.getActualPattern()->track[track].step[step];
        if (target.note != Sequencer::STEP_NOTE_EMPTY)
            continue;
        DISABLE_AUDIO_INTERRUPTS();
        target.note = Sequencer::STEP_NOTE_OFF_DEFAULT;
        ENABLE_AUDIO_INTERRUPTS();
        st.offStep[track] = static_cast<int16_t>(step);
    }
}

Result finish()
{
    if (!st.active)
        return {};
    st.active = false;
    st.result.steps = st.steps;
    newFileManager.setActualPatternStructChanged();
    patternEditor.refreshPatternFromOutsideModule();
    return st.result;
}
} // namespace ndlrCapture

#endif
