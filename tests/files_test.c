// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks the File, Resource and Memory Managers in hle/ against libmac.so,
// in a temporary directory.
//
//   make tests/files_test
//   tests/files_test [/path/to/Halo.app]
//
// With a bundle path it also opens the game's EULA.rsrc resource fork.

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hle/carbon.h"

int FSFindFolder(SInt16, OSType, unsigned int, FSRef*);
int FSRefMakePath(const FSRef*, UInt8*, UInt32);
int FSPathMakeRef(const UInt8*, FSRef*, Boolean*);
int FSGetCatalogInfo(const FSRef*, FSCatalogInfoBitmap, FSCatalogInfo*,
                     HFSUniStr255*, FSSpec*, FSRef*);
int FSCreateFileUnicode(const FSRef*, UniCharCount, const UniChar*,
                        FSCatalogInfoBitmap, const FSCatalogInfo*, FSRef*,
                        FSSpec*);
int FSCreateDirectoryUnicode(const FSRef*, UniCharCount, const UniChar*,
                             FSCatalogInfoBitmap, const FSCatalogInfo*, FSRef*,
                             FSSpec*, UInt32*);
int FSOpenFork(const FSRef*, UniCharCount, const UniChar*, SInt8, SInt16*);
int FSWriteFork(SInt16, UInt16, SInt64, ByteCount, const void*, ByteCount*);
int FSReadFork(SInt16, UInt16, SInt64, ByteCount, void*, ByteCount*);
int FSGetForkSize(SInt16, SInt64*);
int FSSetForkSize(SInt16, UInt16, SInt64);
int FSCloseFork(SInt16);
int FSpCreate(const FSSpec*, OSType, OSType, SInt16);
int FSpOpenDF(const FSSpec*, SInt8, SInt16*);
int FSWrite(SInt16, int32_t*, const void*);
int SetEOF(SInt16, int32_t);
int GetEOF(SInt16, int32_t*);
int SetFPos(SInt16, SInt16, int32_t);
int FSClose(SInt16);
int PBGetCatInfoSync(CInfoPBRec*);
int FSOpenIterator(const FSRef*, uint32_t, void**);
int FSGetCatalogInfoBulk(void*, ItemCount, ItemCount*, Boolean*,
                         FSCatalogInfoBitmap, FSCatalogInfo*, FSRef*, FSSpec*,
                         HFSUniStr255*);
int FSCloseIterator(void*);
int FSRenameUnicode(const FSRef*, UniCharCount, const UniChar*, TextEncoding,
                    FSRef*);
int FSDeleteObject(const FSRef*);
int HSetVol(ConstStringPtr, SInt16, int32_t);
Ptr NewPtr(Size);
Size GetPtrSize(Ptr);
void SetPtrSize(Ptr, Size);
void DisposePtr(Ptr);
int MemError(void);
short FSOpenResFile(const FSRef*, SInt8);
short CurResFile(void);
Handle GetResource(ResType, SInt16);
void ReleaseResource(Handle);
void GetIndString(unsigned char*, SInt16, SInt16);
void CloseResFile(SInt16);
void c2pstrcpy(unsigned char*, const char*);

static int failures;

static void check(int ok, const char* what) {
  printf("%s - %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

static UniCharCount unicode(const char* ascii, UniChar* out) {
  UniCharCount n = strlen(ascii);
  for (UniCharCount i = 0; i < n; i++) {
    out[i] = (unsigned char)ascii[i];
  }
  return n;
}

static void test_memory(void) {
  Ptr p = NewPtr(100);
  check(p && GetPtrSize(p) == 100 && ((uintptr_t)p & 15) == 0,
        "NewPtr: size and 16-byte alignment");
  SetPtrSize(p, 50);
  check(MemError() == noErr && GetPtrSize(p) == 50, "SetPtrSize shrinks");
  SetPtrSize(p, 1 << 20);
  check(MemError() == memFullErr && GetPtrSize(p) == 50,
        "SetPtrSize cannot move a pointer");
  DisposePtr(p);
}

static void test_folders(void) {
  UInt8 path[1024];
  FSRef folder;
  check(FSFindFolder(kUserDomain, 'pref', 1, &folder) == noErr,
        "FSFindFolder creates Preferences");
  FSRefMakePath(&folder, path, sizeof(path));
  check(strstr((char*)path, "/Library/Preferences") != NULL,
        "Preferences is under the home");
  check(FSFindFolder(kOnAppropriateDisk, 'fram', 1, &folder) == noErr,
        "FSFindFolder: the system Frameworks folder");
  FSRefMakePath(&folder, path, sizeof(path));
  check(strstr((char*)path, "/System/Library/Frameworks") != NULL,
        "Frameworks is under the root");
  check(FSFindFolder(kUserDomain, 'docs', 0, &folder) == fnfErr,
        "FSFindFolder without create: fnfErr");
}

static void test_catalog_and_forks(const char* dir) {
  UniChar name[64];
  UniCharCount len;
  FSRef root;
  Boolean is_dir = 0;
  check(FSPathMakeRef((const UInt8*)dir, &root, &is_dir) == noErr && is_dir,
        "FSPathMakeRef on a folder");

  len = unicode("Maps", name);
  FSRef maps;
  UInt32 maps_id = 0;
  check(FSCreateDirectoryUnicode(&root, len, name, 0, NULL, &maps, NULL,
                                 &maps_id) == noErr && maps_id,
        "create a folder");
  check(FSCreateDirectoryUnicode(&root, len, name, 0, NULL, NULL, NULL,
                                 NULL) == dupFNErr,
        "create it again: dupFNErr");

  len = unicode("b30.map", name);
  FSRef map;
  check(FSCreateFileUnicode(&maps, len, name, 0, NULL, &map, NULL) == noErr,
        "create a file");
  SInt16 fork;
  ByteCount n = 0;
  check(FSOpenFork(&map, 0, NULL, fsRdWrPerm, &fork) == noErr,
        "open the data fork for writing");
  check(FSWriteFork(fork, fsFromStart, 0, 10, "0123456789", &n) == noErr &&
            n == 10,
        "write the fork");
  char buf[32] = { 0 };
  check(FSReadFork(fork, fsFromStart, 4, 16, buf, &n) == eofErr && n == 6 &&
            !memcmp(buf, "456789", 6),
        "a short read: eofErr, with the bytes there were");
  SInt64 size = 0;
  FSSetForkSize(fork, fsFromStart, 4);
  FSGetForkSize(fork, &size);
  check(size == 4, "set the fork size");
  FSCloseFork(fork);

  FSCatalogInfo info;
  HFSUniStr255 uname;
  FSSpec spec;
  memset(&info, 0xAA, sizeof(info));
  check(FSGetCatalogInfo(&map,
                         kFSCatInfoNodeFlags | kFSCatInfoParentDirID |
                             kFSCatInfoDataSizes,
                         &info, &uname, &spec, NULL) == noErr,
        "FSGetCatalogInfo");
  check(info.nodeFlags == 0 && info.parentDirID == maps_id &&
            info.dataLogicalSize == 4,
        "catalog info fields");
  check(info.nodeID == 0xAAAAAAAA, "fields not asked for are left alone");
  check(uname.length == 7 && uname.unicode[0] == 'b', "the catalog name");
  check(spec.parID == (int32_t)maps_id && spec.name[0] == 7 &&
            !memcmp(spec.name + 1, "b30.map", 7),
        "the FSSpec FSGetCatalogInfo fills");

  // The screenshot saver: a default directory, then numbered names until
  // FSpCreate stops answering dupFNErr.
  check(HSetVol(NULL, 0, maps_id) == noErr, "HSetVol");
  unsigned char pname[256];
  c2pstrcpy(pname, "Screenshot 1");
  FSSpec shot;
  check(FSMakeFSSpec(0, 0, pname, &shot) == fnfErr &&
            shot.parID == (int32_t)maps_id,
        "FSMakeFSSpec of a new name: fnfErr, spec filled");
  check(FSpCreate(&shot, 0, 'PICT', -1) == noErr, "FSpCreate");
  check(FSpCreate(&shot, 0, 'PICT', -1) == dupFNErr,
        "FSpCreate again: dupFNErr");
  SInt16 ref;
  int32_t count = 5;
  int32_t eof = 0;
  check(FSpOpenDF(&shot, fsRdWrPerm, &ref) == noErr, "FSpOpenDF");
  SetEOF(ref, 512);
  SetFPos(ref, fsFromStart, 512);
  check(FSWrite(ref, &count, "hello") == noErr && count == 5, "FSWrite");
  GetEOF(ref, &eof);
  check(eof == 517, "SetEOF, SetFPos, FSWrite");
  FSClose(ref);

  c2pstrcpy(pname, "::Maps:b30.map");
  check(FSMakeFSSpec(0, maps_id, pname, &spec) == noErr &&
            spec.parID == (int32_t)maps_id,
        "a partial pathname that climbs with ::");

  // How the game builds a path: ioDrParID from folder to folder.
  CInfoPBRec pb;
  unsigned char dirname[256];
  int32_t id = maps_id;
  int levels = 0;
  while (id != fsRtDirID && levels < 64) {
    memset(&pb, 0, sizeof(pb));
    pb.dirInfo.ioNamePtr = dirname;
    pb.dirInfo.ioFDirIndex = -1;
    pb.dirInfo.ioDrDirID = id;
    if (PBGetCatInfoSync(&pb) != noErr) {
      break;
    }
    id = pb.dirInfo.ioDrParID;
    levels++;
  }
  check(id == fsRtDirID && levels > 1, "the ioDrParID chain reaches the root");

  memset(&pb, 0, sizeof(pb));
  pb.hFileInfo.ioNamePtr = dirname;
  pb.hFileInfo.ioFDirIndex = 2;
  pb.hFileInfo.ioDirID = maps_id;
  check(PBGetCatInfoSync(&pb) == noErr && dirname[0] == 12 &&
            !memcmp(dirname + 1, "Screenshot 1", 12) &&
            pb.hFileInfo.ioFlLgLen == 517,
        "PBGetCatInfoSync by index, in HFS order");

  char companion[1024];
  FSRefMakePath(&maps, (UInt8*)companion, sizeof(companion) - 16);
  strcat(companion, "/._b30.map");
  close(open(companion, O_CREAT | O_WRONLY, 0644));
  void* it;
  ItemCount actual = 0;
  HFSUniStr255 names[8];
  check(FSOpenIterator(&maps, kFSIterateFlat, &it) == noErr, "FSOpenIterator");
  check(FSGetCatalogInfoBulk(it, 8, &actual, NULL, 0, NULL, NULL, NULL,
                             names) == errFSNoMoreItems &&
            actual == 2,
        "iteration hides AppleDouble companions");
  FSCloseIterator(it);

  len = unicode("b31.map", name);
  FSRef renamed;
  check(FSRenameUnicode(&map, len, name, 0, &renamed) == noErr, "rename");
  check(access(companion, F_OK) != 0, "the companion moves with the file");
  check(FSDeleteObject(&renamed) == noErr, "delete");
}

static void put16(uint8_t* p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

// A resource fork holding 'STR#' 128 = { "first", "second" }, in an
// AppleDouble companion beside an empty data fork.
static void write_resource_file(const char* dir, char* path, size_t size) {
  uint8_t fork[325] = { 0 };
  put32(fork, 256);
  put32(fork + 4, 275);
  put32(fork + 8, 19);
  put32(fork + 12, 50);
  uint8_t* res = fork + 256;
  put32(res, 15);
  put16(res + 4, 2);
  memcpy(res + 6, "\x05" "first" "\x06" "second", 13);
  uint8_t* map = fork + 275;
  put16(map + 24, 28);
  put16(map + 26, 50);
  uint8_t* types = map + 28;
  put16(types, 0);
  memcpy(types + 2, "STR#", 4);
  put16(types + 6, 0);
  put16(types + 8, 10);
  uint8_t* refs = types + 10;
  put16(refs, 128);
  put16(refs + 2, 0xFFFF);

  uint8_t header[38] = { 0 };
  put32(header, 0x00051607);
  put32(header + 4, 0x00020000);
  put16(header + 24, 1);
  put32(header + 26, 2);
  put32(header + 30, sizeof(header));
  put32(header + 34, sizeof(fork));

  char companion[1024];
  if (snprintf(path, size, "%s/Strings.rsrc", dir) >= (int)size ||
      snprintf(companion, sizeof(companion), "%s/._Strings.rsrc", dir) >=
          (int)sizeof(companion)) {
    fprintf(stderr, "path too long: %s\n", dir);
    exit(1);
  }
  close(open(path, O_CREAT | O_WRONLY, 0644));
  FILE* f = fopen(companion, "wb");
  fwrite(header, 1, sizeof(header), f);
  fwrite(fork, 1, sizeof(fork), f);
  fclose(f);
}

static void test_resources(const char* dir) {
  char path[1024];
  write_resource_file(dir, path, sizeof(path));
  FSRef ref;
  FSPathMakeRef((const UInt8*)path, &ref, NULL);
  short refnum = FSOpenResFile(&ref, fsRdPerm);
  check(refnum > 0 && CurResFile() == refnum,
        "FSOpenResFile opens an AppleDouble resource fork and makes it current");
  unsigned char s[256];
  GetIndString(s, 128, 2);
  check(s[0] == 6 && !memcmp(s + 1, "second", 6), "GetIndString");
  GetIndString(s, 128, 3);
  check(s[0] == 0, "GetIndString past the end");
  Handle a = GetResource('STR#', 128);
  Handle b = GetResource('STR#', 128);
  check(a && a == b && GetHandleSize(a) == 15,
        "GetResource keeps one handle while loaded");
  ReleaseResource(a);
  check(GetResource('STR#', 129) == NULL, "a missing resource: NULL");
  CloseResFile(refnum);
}

static void test_bundle(const char* app) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/Contents/Resources/EULA.rsrc", app);
  FSRef ref;
  check(FSPathMakeRef((const UInt8*)path, &ref, NULL) == noErr,
        "the game's EULA.rsrc");
  short refnum = FSOpenResFile(&ref, fsRdPerm);
  check(refnum > 0, "its resource fork opens");
  if (refnum > 0) {
    CloseResFile(refnum);
  }
}

int main(int argc, char** argv) {
  char home[] = "/tmp/hle-files-test-XXXXXX";
  if (!mkdtemp(home)) {
    perror("mkdtemp");
    return 1;
  }
  setenv("HALO_MAC_HOME", home, 1);
  char work[1024];
  snprintf(work, sizeof(work), "%s/work", home);
  mkdir(work, 0755);

  test_memory();
  test_folders();
  test_catalog_and_forks(work);
  test_resources(work);
  if (argc > 1) {
    test_bundle(argv[1]);
  }

  char cmd[1100];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
  if (system(cmd) != 0) {
    fprintf(stderr, "could not remove %s\n", home);
  }
  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
