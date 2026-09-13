// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// QuickTime, absent. Movie files fail to open, which the game takes as
// having no intro to play; everything that could follow a movie that did
// open is harmless.

#include "carbon.h"

enum {
  couldNotResolveDataRef = -2000,
  noMovieFound = -2048,
};

int EnterMovies(void) {
  return noErr;
}

void ExitMovies(void) {
}

int OpenMovieFile(const FSSpec* spec, SInt16* refnum, SInt8 permission) {
  char path[1024];
  if (spec && hle_spec_to_path(spec, path, sizeof(path)) == noErr) {
    cf_trace("OpenMovieFile(%s): movies are not supported", path);
  }
  if (refnum) {
    *refnum = 0;
  }
  return noMovieFound;
}

int CloseMovieFile(SInt16 refnum) {
  return noErr;
}

int NewMovieFromFile(void** movie, SInt16 refnum, SInt16* resource_id,
                     StringPtr name, SInt16 flags, Boolean* data_ref_changed) {
  if (movie) {
    *movie = NULL;
  }
  return noMovieFound;
}

void DisposeMovie(void* movie) {
}

void GetMovieBox(void* movie, Rect* box) {
  if (box) {
    box->top = box->left = box->bottom = box->right = 0;
  }
}

void SetMovieBox(void* movie, const Rect* box) {
}

void SetMovieGWorld(void* movie, void* port, void* device) {
}

void SetMovieActive(void* movie, Boolean active) {
}

void SetMoviePlayHints(void* movie, UInt32 flags, UInt32 mask) {
}

void SetMovieVolume(void* movie, SInt16 volume) {
}

int32_t GetMovieDuration(void* movie) {
  return 0;
}

int32_t GetMovieTime(void* movie, void* time) {
  return 0;
}

void GoToBeginningOfMovie(void* movie) {
}

void GoToEndOfMovie(void* movie) {
}

int PrerollMovie(void* movie, int32_t time, int32_t rate) {
  return noErr;
}

void StartMovie(void* movie) {
}

void StopMovie(void* movie) {
}

void MoviesTask(void* movie, int32_t max_milliseconds) {
}

Boolean IsMovieDone(void* movie) {
  return 1;
}

void* GetMovieTimeBase(void* movie) {
  return NULL;
}

UInt32 GetTimeBaseFlags(void* time_base) {
  return 0;
}

void SetTimeBaseFlags(void* time_base, UInt32 flags) {
}

int32_t GetGraphicsImporterForDataRef(Handle data_ref, OSType type,
                                      void** importer) {
  if (importer) {
    *importer = NULL;
  }
  return couldNotResolveDataRef;
}
