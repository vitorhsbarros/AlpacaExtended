#include "NdlrCapture.h"
#include "mtSequencer.h"
#include "fileManager.h"
#include "patternEditor/patternEditor.h"
#include <cstdio>
#include <cstring>
#include <cassert>
Sequencer sequencer; cFileManager newFileManager; cPatternEditor patternEditor;
using namespace ndlrCapture;
static auto& T(int t){ return sequencer.getActualPattern()->track[t]; }
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++fails;} }while(0)
int main(){
  // chord matching
  int16_t maj[]={60,64,67}, m7[]={57,60,64,67}, sus2b2[]={64,65,71}, wide[]={48,60,64,67,72};
  CHECK(chordFxFor(maj,3)==5);
  CHECK(chordFxFor(m7,4)==20);
  CHECK(chordFxFor(sus2b2,3)==0);
  CHECK(chordFxFor(wide,5)==0);
  memset(&sequencer.pattern, 0x55, sizeof(sequencer.pattern));
  PartLayout layout[4] = { {8,2,68}, {10,2,69}, {12,1,70}, {13,1,71} };
  begin(layout, 32, false);
  CHECK(newFileManager.undo==1);
  CHECK(T(8).step[0].note==-1 && T(13).step[31].note==-1 && T(14).step[0].note==0x55); // only own tracks cleared
  // pad chord at step 0 -> one track with chord FX
  chord(0, 0, maj, 3, 80);
  CHECK(T(8).step[0].note==60 && T(8).step[0].fx[0].type==13 && T(8).step[0].fx[0].value==5 && T(8).step[0].instrument==68);
  CHECK(T(8).step[0].volume==63 && T(8).step[0].micromove==-1);
  // chord change at pulse 48 (step 4)
  release(0, 48); chord(0, 48, m7, 4, 80);
  CHECK(T(8).step[4].note==57 && T(8).step[4].fx[0].value==20);
  CHECK(T(9).step[4].note==-1);
  // wide 5-note chord at step 8 with 2 tracks: biggest shape on track 8, rest on 9, one dropped
  release(0, 96); chord(0, 96, wide, 5, 80);
  // low root 48 alone, then 60-64-67 as a major chord, 72 left out
  CHECK(T(8).step[8].note==48 && T(8).step[8].fx[0].type==0);
  CHECK(T(9).step[8].note==60 && T(9).step[8].fx[0].value==5);
  // re-voice in same step: off must not cut
  release(0, 100); chord(0, 100, maj, 3, 80);
  CHECK(T(8).step[8].note==60 && T(8).step[8].micromove==((4*100+6)/12-1));
  CHECK(T(8).step[9].note==-1); // no stray note-off
  CHECK(T(9).step[9].note==-4); // track 9 had a note, gets released
  // motif with gate: note at step 2, off at pulse 30 (step 2) -> step 3
  note(2, 0, 24, 72, 100); release(2, 30);
  CHECK(T(12).step[2].note==72 && T(12).step[3].note==-4);
  // next note at pulse 36 (step 3) overwrites off
  note(2, 0, 36, 74, 100);
  CHECK(T(12).step[3].note==74);
  // strum: slots
  note(0, 0, 192, 60, 80); note(0, 1, 198, 64, 80); note(0, 2, 204, 67, 80);
  CHECK(T(8).step[16].note==60 && T(9).step[16].note==64 && T(9).step[16].micromove==49);
  // out of range ignored
  note(3, 0, 32*12, 60, 80); CHECK(T(13).step[0].note==-1);
  auto r = finish();
  printf("notes %u dropped %u\n", r.notes, r.dropped);
  CHECK(r.dropped==2);
  CHECK(r.lastStep==17 && r.steps==32); // one from wide chord + slot2 of strum
  CHECK(patternEditor.refresh==1);
  // fitting the length sets every track
  begin(layout, 48, true);
  CHECK(T(0).length==47 && T(15).length==47);
  finish();
  printf(fails? "FAILED %d\n":"ALL OK\n", fails); return fails;
}
