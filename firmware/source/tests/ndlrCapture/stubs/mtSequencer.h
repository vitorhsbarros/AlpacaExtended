#pragma once
#include <stdint.h>
static constexpr int kAllTracks = 16;
class Sequencer {
public:
  static constexpr int kStepFxSlots = 2;
  enum enStep { STEP_NOTE_EMPTY=-1, STEP_NOTE_OFF_FADE=-2, STEP_NOTE_OFF_CUT=-3, STEP_NOTE_OFF_DEFAULT=-4, STEP_NOTE_MAX=127, STEP_VELO_DEFAULT=-1 };
  struct strFxConsts { enum enFxType { FX_TYPE_NONE, FX_TYPE_OFF, FX_TYPE_MIDI_CHORD = 13 }; };
  struct strPattern { struct strTrack { uint8_t length; struct strStep { int8_t note; uint8_t instrument; struct strFx { uint8_t type; uint8_t value; } fx[kStepFxSlots]; int8_t volume; int8_t micromove; } step[128]; } track[kAllTracks]; };
  strPattern pattern;
  strPattern* getActualPattern() { return &pattern; }
};
extern Sequencer sequencer;
