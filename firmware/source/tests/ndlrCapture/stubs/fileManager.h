#pragma once
struct cFileManager { int undo=0, changed=0; void storePatternUndoRevision(){++undo;} void setActualPatternStructChanged(){++changed;} };
extern cFileManager newFileManager;
