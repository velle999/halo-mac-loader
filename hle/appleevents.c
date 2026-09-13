// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The Apple Event Manager's descriptors. Nothing sends this process Apple
// Events (toolbox.c accepts handlers and never runs them), so lists are
// empty, parameters are missing and events sent go nowhere.

#include "carbon.h"

enum {
  errAEDescNotFound = -1701,
  kTypeNull = 'null',
};

#pragma pack(push, 2)
typedef struct {
  OSType descriptorType;
  Handle dataHandle;
} AEDesc;
#pragma pack(pop)

static void null_desc(AEDesc* desc) {
  if (desc) {
    desc->descriptorType = kTypeNull;
    desc->dataHandle = NULL;
  }
}

int AECreateDesc(OSType type, const void* data, Size size, AEDesc* result) {
  if (!result) {
    return paramErr;
  }
  result->descriptorType = type;
  result->dataHandle = size > 0 ? hle_handle_new(data, size, 0) : NULL;
  return noErr;
}

int AEDisposeDesc(AEDesc* desc) {
  if (desc && desc->dataHandle) {
    DisposeHandle(desc->dataHandle);
  }
  null_desc(desc);
  return noErr;
}

int AECountItems(const AEDesc* list, SInt32* count) {
  if (count) {
    *count = 0;
  }
  return noErr;
}

int AEGetNthPtr(const AEDesc* list, SInt32 index, OSType desired,
                OSType* keyword, OSType* type, void* data, Size max,
                Size* actual) {
  return errAEDescNotFound;
}

int AEGetParamDesc(const AEDesc* event, OSType keyword, OSType desired,
                   AEDesc* result) {
  null_desc(result);
  return errAEDescNotFound;
}

int AEGetAttributePtr(const AEDesc* event, OSType keyword, OSType desired,
                      OSType* type, void* data, Size max, Size* actual) {
  return errAEDescNotFound;
}

int AEPutParamPtr(AEDesc* event, OSType keyword, OSType type,
                  const void* data, Size size) {
  return event ? noErr : paramErr;
}

int AECreateAppleEvent(OSType event_class, OSType event_id,
                       const AEDesc* target, SInt16 return_id,
                       SInt32 transaction_id, AEDesc* result) {
  if (!result) {
    return paramErr;
  }
  result->descriptorType = 'aevt';
  result->dataHandle = NULL;
  return noErr;
}

int AESend(const AEDesc* event, AEDesc* reply, SInt32 mode, SInt16 priority,
           SInt32 timeout, void* idle, void* filter) {
  null_desc(reply);
  return noErr;
}
