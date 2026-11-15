#pragma once
struct cPatternEditor { int refresh=0; void refreshPatternFromOutsideModule(){++refresh;} };
extern cPatternEditor patternEditor;
