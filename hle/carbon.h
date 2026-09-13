// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Carbon types and structures as a 10.4 i386 executable sees them.
//
// Carbon's File Manager structures keep 68K alignment on Intel. The game
// writes CInfoPBRec.ioNamePtr at offset 18, not 20, and walks ioDrParID at
// 100, so everything here is packed to 2 bytes and those offsets are
// asserted. Fields the headers declare long are int32_t: long is 32 bits on
// both sides. Values are host-endian, as the File Manager returns them on
// Intel; only on-disk formats (resource forks, AppleDouble) are big-endian.

#ifndef HLE_CARBON_H_
#define HLE_CARBON_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "appledouble.h"
#include "cf.h"

typedef int8_t SInt8;
typedef int16_t SInt16;
typedef int64_t SInt64;
typedef uint64_t UInt64;
typedef int16_t OSErr;
typedef int32_t OSStatus;
typedef uint32_t OSType;
typedef OSType ResType;
typedef char* Ptr;
typedef Ptr* Handle;
typedef long Size;
typedef unsigned char Str255[256];
typedef unsigned char Str63[64];
typedef unsigned char* StringPtr;
typedef const unsigned char* ConstStringPtr;
typedef uint32_t TextEncoding;
typedef uint32_t ItemCount;
typedef uint32_t ByteCount;
typedef uint32_t UniCharCount;
typedef int16_t FSVolumeRefNum;
typedef uint32_t FSCatalogInfoBitmap;
typedef uint32_t FSVolumeInfoBitmap;

// MacErrors.h
enum {
  noErr = 0,
  dirFulErr = -33,
  dskFulErr = -34,
  nsvErr = -35,
  ioErr = -36,
  bdNamErr = -37,
  fnOpnErr = -38,
  eofErr = -39,
  posErr = -40,
  tmfoErr = -42,
  fnfErr = -43,
  wPrErr = -44,
  fLckdErr = -45,
  vLckdErr = -46,
  fBsyErr = -47,
  dupFNErr = -48,
  opWrErr = -49,
  paramErr = -50,
  rfNumErr = -51,
  permErr = -54,
  wrPermErr = -61,
  memFullErr = -108,
  nilHandleErr = -109,
  dirNFErr = -120,
  resNotFound = -192,
  resFNotFound = -193,
  mapReadErr = -199,
  buffersTooSmall = -210,
  notAFileErr = -1302,
  errFSBadFSRef = -1401,
  errFSBadForkName = -1402,
  errFSBadBuffer = -1403,
  errFSBadForkRef = -1404,
  errFSBadInfoBitmap = -1405,
  errFSMissingCatInfo = -1406,
  errFSNotAFolder = -1407,
  errFSForkNotFound = -1409,
  errFSNameTooLong = -1410,
  errFSMissingName = -1411,
  errFSBadPosMode = -1412,
  errFSNoMoreItems = -1417,
  errFSBadItemCount = -1418,
  errFSRefsDifferent = -1420,
  errFSForkExists = -1421,
  errFSBadIteratorFlags = -1422,
  errFSIteratorNotFound = -1423,
  errFSIteratorNotSupported = -1424,
  pathTooLongErr = -2110,
  afpAccessDenied = -5000,
};

enum {
  fsCurPerm = 0,
  fsRdPerm = 1,
  fsWrPerm = 2,
  fsRdWrPerm = 3,
  fsRdWrShPerm = 4,
};

enum {
  fsAtMark = 0,
  fsFromStart = 1,
  fsFromLEOF = 2,
  fsFromMark = 3,
};

enum {
  fsRtParID = 1,
  fsRtDirID = 2,
};

enum {
  kFSCatInfoTextEncoding = 0x00000001,
  kFSCatInfoNodeFlags = 0x00000002,
  kFSCatInfoVolume = 0x00000004,
  kFSCatInfoParentDirID = 0x00000008,
  kFSCatInfoNodeID = 0x00000010,
  kFSCatInfoCreateDate = 0x00000020,
  kFSCatInfoContentMod = 0x00000040,
  kFSCatInfoAttrMod = 0x00000080,
  kFSCatInfoAccessDate = 0x00000100,
  kFSCatInfoBackupDate = 0x00000200,
  kFSCatInfoPermissions = 0x00000400,
  kFSCatInfoFinderInfo = 0x00000800,
  kFSCatInfoFinderXInfo = 0x00001000,
  kFSCatInfoValence = 0x00002000,
  kFSCatInfoDataSizes = 0x00004000,
  kFSCatInfoRsrcSizes = 0x00008000,
  kFSCatInfoSharingFlags = 0x00010000,
  kFSCatInfoUserPrivs = 0x00020000,
};

enum {
  kFSNodeLockedMask = 0x0001,
  kFSNodeIsDirectoryMask = 0x0010,
};

enum {
  kFSIterateFlat = 0,
  kFSIterateSubtree = 1,
};

enum {
  kOnSystemDisk = -32768,
  kOnAppropriateDisk = -32767,
  kSystemDomain = -32766,
  kLocalDomain = -32765,
  kNetworkDomain = -32764,
  kUserDomain = -32763,
  kClassicDomain = -32762,
};

// The one volume everything is on: the system disk, rooted at /.
enum {
  kHleVolumeRefNum = -100,
};

#pragma pack(push, 2)

typedef struct {
  uint8_t hidden[80];
} FSRef;

typedef struct {
  SInt16 vRefNum;
  int32_t parID;
  Str63 name;
} FSSpec;

typedef struct {
  UInt16 length;
  UniChar unicode[255];
} HFSUniStr255;

typedef struct {
  UInt16 highSeconds;
  UInt32 lowSeconds;
  UInt16 fraction;
} UTCDateTime;

typedef struct {
  SInt16 v;
  SInt16 h;
} Point;

typedef struct {
  SInt16 top;
  SInt16 left;
  SInt16 bottom;
  SInt16 right;
} Rect;

typedef struct {
  OSType fdType;
  OSType fdCreator;
  UInt16 fdFlags;
  Point fdLocation;
  SInt16 fdFldr;
} FInfo;

typedef struct {
  SInt16 fdIconID;
  SInt16 fdReserved[3];
  SInt8 fdScript;
  SInt8 fdXFlags;
  SInt16 fdComment;
  int32_t fdPutAway;
} FXInfo;

typedef struct {
  Rect frRect;
  UInt16 frFlags;
  Point frLocation;
  SInt16 frView;
} DInfo;

typedef struct {
  Point frScroll;
  int32_t frOpenChain;
  SInt8 frScript;
  SInt8 frXFlags;
  SInt16 frComment;
  int32_t frPutAway;
} DXInfo;

typedef struct {
  UInt16 nodeFlags;
  FSVolumeRefNum volume;
  UInt32 parentDirID;
  UInt32 nodeID;
  UInt8 sharingFlags;
  UInt8 userPrivileges;
  UInt8 reserved1;
  UInt8 reserved2;
  UTCDateTime createDate;
  UTCDateTime contentModDate;
  UTCDateTime attributeModDate;
  UTCDateTime accessDate;
  UTCDateTime backupDate;
  UInt32 permissions[4];
  UInt8 finderInfo[16];
  UInt8 extFinderInfo[16];
  UInt64 dataLogicalSize;
  UInt64 dataPhysicalSize;
  UInt64 rsrcLogicalSize;
  UInt64 rsrcPhysicalSize;
  UInt32 valence;
  TextEncoding textEncodingHint;
} FSCatalogInfo;

typedef struct {
  UInt32 userID;
  UInt32 groupID;
  UInt8 reserved1;
  UInt8 userAccess;
  UInt16 mode;
  UInt32 fileSec;
} FSPermissionInfo;

typedef struct {
  UTCDateTime createDate;
  UTCDateTime modifyDate;
  UTCDateTime backupDate;
  UTCDateTime checkedDate;
  UInt32 fileCount;
  UInt32 folderCount;
  UInt64 totalBytes;
  UInt64 freeBytes;
  UInt32 blockSize;
  UInt32 totalBlocks;
  UInt32 freeBlocks;
  UInt32 nextAllocation;
  UInt32 rsrcClumpSize;
  UInt32 dataClumpSize;
  UInt32 nextCatalogID;
  UInt8 finderInfo[32];
  UInt16 flags;
  UInt16 filesystemID;
  UInt16 signature;
  UInt16 driveNumber;
  SInt16 driverRefNum;
} FSVolumeInfo;

// The header every parameter block starts with.
#define HLE_PARAM_BLOCK_HEADER \
  void* qLink;                 \
  SInt16 qType;                \
  SInt16 ioTrap;               \
  Ptr ioCmdAddr;               \
  void* ioCompletion;          \
  SInt16 ioResult;             \
  StringPtr ioNamePtr;         \
  SInt16 ioVRefNum

typedef struct {
  HLE_PARAM_BLOCK_HEADER;
  SInt16 ioRefNum;
  SInt8 ioVersNum;
  SInt8 ioPermssn;
  Ptr ioMisc;
  Ptr ioBuffer;
  int32_t ioReqCount;
  int32_t ioActCount;
  SInt16 ioPosMode;
  int32_t ioPosOffset;
} HIOParam;

typedef struct {
  HLE_PARAM_BLOCK_HEADER;
  SInt16 ioFRefNum;
  SInt8 ioFVersNum;
  SInt8 filler1;
  SInt16 ioFDirIndex;
  SInt8 ioFlAttrib;
  SInt8 ioACUser;
  FInfo ioFlFndrInfo;
  int32_t ioDirID;
  UInt16 ioFlStBlk;
  int32_t ioFlLgLen;
  int32_t ioFlPyLen;
  UInt16 ioFlRStBlk;
  int32_t ioFlRLgLen;
  int32_t ioFlRPyLen;
  UInt32 ioFlCrDat;
  UInt32 ioFlMdDat;
  UInt32 ioFlBkDat;
  FXInfo ioFlXFndrInfo;
  int32_t ioFlParID;
  int32_t ioFlClpSiz;
} HFileInfo;

typedef struct {
  HLE_PARAM_BLOCK_HEADER;
  SInt16 ioFRefNum;
  SInt8 ioFVersNum;
  SInt8 filler1;
  SInt16 ioFDirIndex;
  SInt8 ioFlAttrib;
  SInt8 ioACUser;
  DInfo ioDrUsrWds;
  int32_t ioDrDirID;
  UInt16 ioDrNmFls;
  SInt16 filler3[9];
  UInt32 ioDrCrDat;
  UInt32 ioDrMdDat;
  UInt32 ioDrBkDat;
  DXInfo ioDrFndrInfo;
  int32_t ioDrParID;
} DirInfo;

typedef union {
  HFileInfo hFileInfo;
  DirInfo dirInfo;
} CInfoPBRec;

typedef struct {
  HLE_PARAM_BLOCK_HEADER;
  int32_t filler2;
  SInt16 ioVolIndex;
  UInt32 ioVCrDate;
  UInt32 ioVLsMod;
  SInt16 ioVAtrb;
  UInt16 ioVNmFls;
  UInt16 ioVBitMap;
  UInt16 ioAllocPtr;
  UInt16 ioVNmAlBlks;
  UInt32 ioVAlBlkSiz;
  UInt32 ioVClpSiz;
  UInt16 ioAlBlSt;
  UInt32 ioVNxtCNID;
  UInt16 ioVFrBlk;
  UInt16 ioVSigWord;
  SInt16 ioVDrvInfo;
  SInt16 ioVDRefNum;
  SInt16 ioVFSID;
  UInt32 ioVBkUp;
  SInt16 ioVSeqNum;
  UInt32 ioVWrCnt;
  UInt32 ioVFilCnt;
  UInt32 ioVDirCnt;
  int32_t ioVFndrInfo[8];
} HVolumeParam;

typedef struct {
  HLE_PARAM_BLOCK_HEADER;
  SInt16 ioRefNum;
  SInt16 ioDenyModes;
  SInt16 filler4;
  SInt8 filler5;
  SInt8 ioACUser;
  int32_t filler6;
  int32_t ioACOwnerID;
  int32_t ioACGroupID;
  int32_t ioACAccess;
  int32_t ioDirID;
} AccessParam;

typedef struct {
  HLE_PARAM_BLOCK_HEADER;
  SInt16 ioDstVRefNum;
  SInt16 filler8;
  StringPtr ioNewName;
  StringPtr ioCopyName;
  int32_t ioNewDirID;
  int32_t filler14;
  int32_t filler15;
  int32_t ioDirID;
} CopyParam;

typedef union {
  HIOParam ioParam;
  HVolumeParam volumeParam;
  AccessParam accessParam;
  CopyParam copyParam;
} HParamBlockRec;

typedef struct {
  HLE_PARAM_BLOCK_HEADER;
  SInt16 reserved1;
  UInt8 reserved2;
  UInt8 reserved3;
  const FSRef* ref;
  FSCatalogInfoBitmap whichInfo;
  FSCatalogInfo* catInfo;
  UniCharCount nameLength;
  const UniChar* name;
  int32_t ioDirID;
  FSSpec* spec;
  FSRef* parentRef;
  FSRef* newRef;
  TextEncoding textEncodingHint;
  HFSUniStr255* outName;
} FSRefParam;

typedef struct {
  SInt16 vMVersion;
  int32_t vMAttrib;
  Handle vMLocalHand;
  int32_t vMServerAdr;
  int32_t vMVolumeGrade;
  SInt16 vMForeignPrivID;
  int32_t vMExtendedAttributes;
  void* vMDeviceID;
  UniCharCount vMMaxNameLength;
} GetVolParmsInfoBuffer;

typedef struct {
  int32_t initialize;
  SInt16 priv[6];
} CatPositionRec;

#pragma pack(pop)

_Static_assert(sizeof(FSSpec) == 70, "FSSpec");
_Static_assert(sizeof(UTCDateTime) == 8, "UTCDateTime");
_Static_assert(sizeof(HFSUniStr255) == 512, "HFSUniStr255");
_Static_assert(sizeof(FSCatalogInfo) == 144, "FSCatalogInfo");
_Static_assert(offsetof(FSCatalogInfo, finderInfo) == 72, "finderInfo");
_Static_assert(offsetof(FSCatalogInfo, dataLogicalSize) == 104,
               "dataLogicalSize");
// These hold only where pointers are 4 bytes, which is the only target.
#ifdef __i386__
_Static_assert(offsetof(HFileInfo, ioNamePtr) == 18, "ioNamePtr");
_Static_assert(offsetof(HFileInfo, ioFDirIndex) == 28, "ioFDirIndex");
_Static_assert(offsetof(DirInfo, ioDrDirID) == 48, "ioDrDirID");
_Static_assert(offsetof(DirInfo, ioDrParID) == 100, "ioDrParID");
_Static_assert(sizeof(HVolumeParam) == 122, "HVolumeParam");
_Static_assert(offsetof(HIOParam, ioBuffer) == 32, "ioBuffer");
_Static_assert(sizeof(FSRefParam) == 72, "FSRefParam");
_Static_assert(sizeof(GetVolParmsInfoBuffer) == 32, "GetVolParmsInfoBuffer");
#endif

// ---------------------------------------------------------------------------
// Internal API shared by the File, Resource and Memory Manager code.

// Seconds between 1904-01-01 and 1970-01-01.
#define HLE_MAC_EPOCH_OFFSET 2082844800u

void hle_utc_from_unix(time_t t, UTCDateTime* out);
// Classic dates: local-time seconds since 1904.
UInt32 hle_mac_local_seconds(time_t t);

// Names. Mac names are MacRoman (Pascal strings) or UTF-16 and may contain
// '/'; POSIX names are UTF-8 and may contain ':'. The two characters swap,
// as they do on Mac OS X. Returned strings are malloc'd.
char* hle_name_from_pascal(ConstStringPtr name);
char* hle_name_from_unicode(const UniChar* name, UniCharCount length);
void hle_name_to_pascal(const char* posix_name, StringPtr out, size_t size);
void hle_name_to_unicode(const char* posix_name, HFSUniStr255* out);

// Node IDs for directories and files, allocated per path. "/" is fsRtDirID
// and its parent fsRtParID. hle_node_path is NULL for an ID never handed out.
UInt32 hle_node_id(const char* path);
const char* hle_node_path(UInt32 id);

// The directory vRefNum 0 / dirID 0 refer to: the working directory until
// HSetVol names another.
UInt32 hle_default_dir_id(void);

// Stands in for / on the system volume, for folders outside the home.
const char* hle_mac_root(void);

// The existing object an FSRef names, into |out|. errFSBadFSRef or fnfErr.
OSErr hle_ref_to_path(const FSRef* ref, char* out, size_t size);
// The path an FSSpec names, which need not exist. dirNFErr when its parent
// directory does not exist, bdNamErr for names this code cannot parse.
OSErr hle_spec_to_path(const FSSpec* spec, char* out, size_t size);
// Fills |spec| for |path|, which need not exist; its parent must.
OSErr hle_path_to_spec(const char* path, FSSpec* spec);
// A path under a directory, with the component matched without regard to
// case when an object by that name exists.
int hle_join(const char* dir, const char* name, char* out, size_t size);

// The File Manager's error for an errno value.
OSErr hle_oserr_from_errno(int err);

// Public File Manager calls used between these files.
int FSMakeFSSpec(SInt16 vref, int32_t dir_id, ConstStringPtr name,
                 FSSpec* spec);

// The resource fork of |path|, read whole into a malloc'd buffer. eofErr
// for a file without one.
OSErr hle_resource_fork_load(const char* path, uint8_t** data,
                             uint32_t* length);

// Memory Manager.
void hle_set_mem_error(OSErr err);
Handle hle_handle_new(const void* data, Size size, int is_resource);
int hle_handle_is_resource(Handle handle);
void DisposeHandle(Handle handle);
Size GetHandleSize(Handle handle);

// Resource Manager: drops a resource handle from the loaded list without
// disposing of it.
void hle_resource_forget(Handle handle);

#endif  // HLE_CARBON_H_
