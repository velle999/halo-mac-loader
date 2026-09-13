// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Faults the game takes on its own, and how it is carried past them.

#ifndef HLE_GAME_FAULTS_H_
#define HLE_GAME_FAULTS_H_

#include <signal.h>

// Called from a SA_SIGINFO signal handler with what it was given. Returns 1
// when the fault is one the game is known to take and |context| has been
// changed to go on past it, so the handler should return; 0 when the fault
// should be reported. Always 0 when HLE_RECOVER is 0.
int hle_recover_fault(int signum, siginfo_t* info, void* context);

#endif  // HLE_GAME_FAULTS_H_
