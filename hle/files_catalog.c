// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The File Manager's catalog: what an object is, what a folder holds,
// creating, deleting and renaming, and the volume. Both the FSRef calls and
// the classic parameter-block ones the game makes.

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "carbon.h"

static const char kVolumeName[] = "Macintosh HD";

// HFS+ 'H+'.
#define HFS_PLUS_SIGNATURE 0x482B

#define MACHINE_ITERATOR_MAGIC 0x49746572u

OSErr hle_oserr_from_errno(int err) {
  switch (err) {
    case 0:
      return noErr;
    case ENOENT:
      return fnfErr;
    case EEXIST:
      return dupFNErr;
    case EACCES:
    case EPERM:
      return permErr;
    case ENOSPC:
      return dskFulErr;
    case ENOTEMPTY:
      return fBsyErr;
    case ENOTDIR:
      return dirNFErr;
    case EROFS:
      return wPrErr;
    case ENAMETOOLONG:
      return bdNamErr;
    case EMFILE:
    case ENFILE:
      return tmfoErr;
    case EISDIR:
      return notAFileErr;
    default:
      return ioErr;
  }
}

static void parent_path(const char* path, char* out, size_t size) {
  snprintf(out, size, "%s", path);
  char* slash = strrchr(out, '/');
  if (!slash || slash == out) {
    snprintf(out, size, "/");
  } else {
    *slash = '\0';
  }
}

static UInt32 parent_id(const char* path) {
  if (!strcmp(path, "/")) {
    return fsRtParID;
  }
  char parent[PATH_MAX];
  parent_path(path, parent, sizeof(parent));
  return hle_node_id(parent);
}

static const char* base_name(const char* path) {
  if (!strcmp(path, "/")) {
    return kVolumeName;
  }
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// A folder's visible items, sorted without regard to case as HFS lists them.
typedef struct {
  char** names;
  size_t count;
} listing;

static int compare_names(const void* a, const void* b) {
  return strcasecmp(*(char* const*)a, *(char* const*)b);
}

static int list_directory(const char* dir, listing* out) {
  out->names = NULL;
  out->count = 0;
  DIR* d = opendir(dir);
  if (!d) {
    return 0;
  }
  size_t capacity = 0;
  struct dirent* entry;
  while ((entry = readdir(d))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        hle_appledouble_is_companion(entry->d_name)) {
      continue;
    }
    if (out->count == capacity) {
      capacity = capacity ? capacity * 2 : 32;
      out->names = realloc(out->names, sizeof(char*) * capacity);
    }
    out->names[out->count++] = strdup(entry->d_name);
  }
  closedir(d);
  qsort(out->names, out->count, sizeof(char*), compare_names);
  return 1;
}

static void free_listing(listing* l) {
  for (size_t i = 0; i < l->count; i++) {
    free(l->names[i]);
  }
  free(l->names);
}

static UInt32 count_items(const char* dir) {
  listing l;
  if (!list_directory(dir, &l)) {
    return 0;
  }
  UInt32 n = l.count;
  free_listing(&l);
  return n;
}

// Finder info is big-endian on disk and host-endian in the API. Each table
// lists the field widths in order.
static void swap_fields(const uint8_t* from, uint8_t* to,
                        const uint8_t* widths, size_t n) {
  for (size_t i = 0; i < n; i++) {
    for (uint8_t k = 0; k < widths[i]; k++) {
      to[k] = from[widths[i] - 1 - k];
    }
    from += widths[i];
    to += widths[i];
  }
}

static void finder_info_from_disk(const uint8_t* disk, int is_dir,
                                  UInt8* info, UInt8* extended) {
  static const uint8_t kFile[] = { 4, 4, 2, 2, 2, 2 };
  static const uint8_t kFileExtended[] = { 2, 2, 2, 2, 1, 1, 2, 4 };
  static const uint8_t kFolder[] = { 2, 2, 2, 2, 2, 2, 2, 2 };
  static const uint8_t kFolderExtended[] = { 2, 2, 4, 1, 1, 2, 4 };
  if (info) {
    if (is_dir) {
      swap_fields(disk, info, kFolder, sizeof(kFolder));
    } else {
      swap_fields(disk, info, kFile, sizeof(kFile));
    }
  }
  if (extended) {
    if (is_dir) {
      swap_fields(disk + 16, extended, kFolderExtended,
                  sizeof(kFolderExtended));
    } else {
      swap_fields(disk + 16, extended, kFileExtended, sizeof(kFileExtended));
    }
  }
}

static UInt8 user_access(const char* path, const struct stat* st) {
  UInt8 bits = 0;
  if (st->st_uid == getuid()) {
    bits |= 0x80;
  }
  if (access(path, W_OK) == 0) {
    bits |= 0x04;
  }
  if (access(path, R_OK) == 0) {
    bits |= 0x02;
  }
  if (access(path, X_OK) == 0) {
    bits |= 0x01;
  }
  return bits;
}

// Fills the fields |which| asks for and leaves the others alone.
static OSErr fill_catalog_info(const char* path, FSCatalogInfoBitmap which,
                               FSCatalogInfo* info) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return fnfErr;
  }
  int is_dir = S_ISDIR(st.st_mode);
  if (which & kFSCatInfoTextEncoding) {
    info->textEncodingHint = kCFStringEncodingMacRoman;
  }
  if (which & kFSCatInfoNodeFlags) {
    info->nodeFlags = (is_dir ? kFSNodeIsDirectoryMask : 0) |
                      (access(path, W_OK) != 0 && !is_dir ? kFSNodeLockedMask
                                                           : 0);
  }
  if (which & kFSCatInfoVolume) {
    info->volume = kHleVolumeRefNum;
  }
  if (which & kFSCatInfoParentDirID) {
    info->parentDirID = parent_id(path);
  }
  if (which & kFSCatInfoNodeID) {
    info->nodeID = hle_node_id(path);
  }
  if (which & kFSCatInfoCreateDate) {
    // Linux stat has no birth time; the older of the two it has.
    hle_utc_from_unix(st.st_mtime < st.st_ctime ? st.st_mtime : st.st_ctime,
                      &info->createDate);
  }
  if (which & kFSCatInfoContentMod) {
    hle_utc_from_unix(st.st_mtime, &info->contentModDate);
  }
  if (which & kFSCatInfoAttrMod) {
    hle_utc_from_unix(st.st_ctime, &info->attributeModDate);
  }
  if (which & kFSCatInfoAccessDate) {
    hle_utc_from_unix(st.st_atime, &info->accessDate);
  }
  if (which & kFSCatInfoBackupDate) {
    memset(&info->backupDate, 0, sizeof(info->backupDate));
  }
  if (which & kFSCatInfoPermissions) {
    FSPermissionInfo* p = (FSPermissionInfo*)info->permissions;
    p->userID = st.st_uid;
    p->groupID = st.st_gid;
    p->reserved1 = 0;
    p->userAccess = user_access(path, &st);
    p->mode = (UInt16)st.st_mode;
    p->fileSec = 0;
  }
  if (which & (kFSCatInfoFinderInfo | kFSCatInfoFinderXInfo)) {
    hle_appledouble ad;
    UInt8* fi = which & kFSCatInfoFinderInfo ? info->finderInfo : NULL;
    UInt8* xi = which & kFSCatInfoFinderXInfo ? info->extFinderInfo : NULL;
    if (hle_appledouble_read(path, &ad) && ad.has_finder_info) {
      finder_info_from_disk(ad.finder_info, is_dir, fi, xi);
    } else {
      if (fi) {
        memset(fi, 0, 16);
      }
      if (xi) {
        memset(xi, 0, 16);
      }
    }
  }
  if (which & kFSCatInfoValence) {
    info->valence = is_dir ? count_items(path) : 0;
  }
  if (which & kFSCatInfoDataSizes) {
    info->dataLogicalSize = is_dir ? 0 : (UInt64)st.st_size;
    info->dataPhysicalSize = is_dir ? 0 : (UInt64)st.st_blocks * 512;
  }
  if (which & kFSCatInfoRsrcSizes) {
    hle_appledouble ad;
    UInt64 length = !is_dir && hle_appledouble_read(path, &ad) &&
                            ad.has_resource_fork
                        ? ad.resource_length
                        : 0;
    info->rsrcLogicalSize = length;
    info->rsrcPhysicalSize = length;
  }
  if (which & kFSCatInfoSharingFlags) {
    info->sharingFlags = 0;
  }
  if (which & kFSCatInfoUserPrivs) {
    info->userPrivileges = 0;
  }
  return noErr;
}

static void fill_outputs(const char* path, HFSUniStr255* name, FSSpec* spec,
                         FSRef* parent) {
  if (name) {
    hle_name_to_unicode(base_name(path), name);
  }
  if (spec) {
    hle_path_to_spec(path, spec);
  }
  if (parent) {
    memset(parent, 0, sizeof(*parent));
    if (strcmp(path, "/")) {
      char dir[PATH_MAX];
      parent_path(path, dir, sizeof(dir));
      hle_fsref_make(dir, parent->hidden);
    }
  }
}

// The object a classic (vRefNum, dirID, name) triple names. fnfErr when it
// does not exist, with |out| still set.
static OSErr classic_path(SInt16 vref, int32_t dir_id, ConstStringPtr name,
                          char* out, size_t size) {
  FSSpec spec;
  OSErr err = FSMakeFSSpec(vref, dir_id, name, &spec);
  if (err != noErr && err != fnfErr) {
    return err;
  }
  OSErr path_err = hle_spec_to_path(&spec, out, size);
  return path_err ? path_err : err;
}

// ---------------------------------------------------------------------------
// Catalog information

int FSGetCatalogInfo(const FSRef* ref, FSCatalogInfoBitmap which,
                     FSCatalogInfo* info, HFSUniStr255* name, FSSpec* spec,
                     FSRef* parent) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    return err;
  }
  if (info) {
    err = fill_catalog_info(path, which, info);
    if (err) {
      return err;
    }
  }
  fill_outputs(path, name, spec, parent);
  return noErr;
}

static time_t unix_from_utc(const UTCDateTime* d) {
  uint64_t seconds = ((uint64_t)d->highSeconds << 32) | d->lowSeconds;
  return (time_t)((int64_t)seconds - HLE_MAC_EPOCH_OFFSET);
}

// Dates and permissions reach the file; Finder info has nowhere to go.
int FSSetCatalogInfo(const FSRef* ref, FSCatalogInfoBitmap which,
                     const FSCatalogInfo* info) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    return err;
  }
  if (which & (kFSCatInfoContentMod | kFSCatInfoAccessDate)) {
    struct timespec times[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
    if (which & kFSCatInfoAccessDate) {
      times[0].tv_sec = unix_from_utc(&info->accessDate);
      times[0].tv_nsec = 0;
    }
    if (which & kFSCatInfoContentMod) {
      times[1].tv_sec = unix_from_utc(&info->contentModDate);
      times[1].tv_nsec = 0;
    }
    utimensat(AT_FDCWD, path, times, 0);
  }
  if (which & kFSCatInfoPermissions) {
    const FSPermissionInfo* p = (const FSPermissionInfo*)info->permissions;
    chmod(path, p->mode & 07777);
  }
  if (which & (kFSCatInfoFinderInfo | kFSCatInfoFinderXInfo)) {
    cf_warn_once("storing Finder info");
  }
  return noErr;
}

// ---------------------------------------------------------------------------
// Iterators

typedef struct {
  uint32_t magic;
  char path[PATH_MAX];
  listing items;
  size_t next;
} iterator;

int FSOpenIterator(const FSRef* container, uint32_t flags,
                   iterator** out) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(container, path, sizeof(path));
  if (err) {
    return err;
  }
  if (flags & ~(uint32_t)kFSIterateSubtree) {
    return errFSBadIteratorFlags;
  }
  if (flags & kFSIterateSubtree) {
    cf_warn_once("FSOpenIterator with kFSIterateSubtree");
    return errFSIteratorNotSupported;
  }
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return errFSNotAFolder;
  }
  iterator* it = calloc(1, sizeof(*it));
  it->magic = MACHINE_ITERATOR_MAGIC;
  snprintf(it->path, sizeof(it->path), "%s", path);
  list_directory(path, &it->items);
  *out = it;
  return noErr;
}

int FSCloseIterator(iterator* it) {
  if (!it || it->magic != MACHINE_ITERATOR_MAGIC) {
    return errFSIteratorNotFound;
  }
  it->magic = 0;
  free_listing(&it->items);
  free(it);
  return noErr;
}

int FSGetCatalogInfoBulk(iterator* it, ItemCount maximum, ItemCount* actual,
                         Boolean* changed, FSCatalogInfoBitmap which,
                         FSCatalogInfo* infos, FSRef* refs, FSSpec* specs,
                         HFSUniStr255* names) {
  if (!it || it->magic != MACHINE_ITERATOR_MAGIC) {
    return errFSIteratorNotFound;
  }
  if (maximum == 0) {
    return errFSBadItemCount;
  }
  ItemCount n = 0;
  while (n < maximum && it->next < it->items.count) {
    const char* name = it->items.names[it->next++];
    char child[PATH_MAX];
    if (!hle_join(it->path, name, child, sizeof(child))) {
      continue;
    }
    if (infos && fill_catalog_info(child, which, &infos[n]) != noErr) {
      continue;  // gone since the folder was listed
    }
    if (refs) {
      hle_fsref_make(child, refs[n].hidden);
    }
    if (specs) {
      hle_path_to_spec(child, &specs[n]);
    }
    if (names) {
      hle_name_to_unicode(name, &names[n]);
    }
    n++;
  }
  if (actual) {
    *actual = n;
  }
  if (changed) {
    *changed = 0;
  }
  return it->next >= it->items.count ? errFSNoMoreItems : noErr;
}

// ---------------------------------------------------------------------------
// Creating, deleting, renaming

static OSErr create_in(const FSRef* parent, UniCharCount length,
                       const UniChar* name, int directory, char* path,
                       size_t size) {
  if (!name || length == 0) {
    return errFSMissingName;
  }
  char dir[PATH_MAX];
  OSErr err = hle_ref_to_path(parent, dir, sizeof(dir));
  if (err) {
    return err;
  }
  char* posix = hle_name_from_unicode(name, length);
  int ok = hle_join(dir, posix, path, size);
  free(posix);
  if (!ok) {
    return bdNamErr;
  }
  if (directory) {
    if (mkdir(path, 0755) != 0) {
      return hle_oserr_from_errno(errno);
    }
  } else {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) {
      return hle_oserr_from_errno(errno);
    }
    close(fd);
  }
  return noErr;
}

int FSCreateFileUnicode(const FSRef* parent, UniCharCount length,
                        const UniChar* name, FSCatalogInfoBitmap which,
                        const FSCatalogInfo* info, FSRef* new_ref,
                        FSSpec* new_spec) {
  char path[PATH_MAX];
  OSErr err = create_in(parent, length, name, 0, path, sizeof(path));
  if (err) {
    return err;
  }
  if (new_ref) {
    hle_fsref_make(path, new_ref->hidden);
  }
  if (new_spec) {
    hle_path_to_spec(path, new_spec);
  }
  return noErr;
}

int FSCreateDirectoryUnicode(const FSRef* parent, UniCharCount length,
                             const UniChar* name, FSCatalogInfoBitmap which,
                             const FSCatalogInfo* info, FSRef* new_ref,
                             FSSpec* new_spec, UInt32* new_dir_id) {
  char path[PATH_MAX];
  OSErr err = create_in(parent, length, name, 1, path, sizeof(path));
  if (err) {
    return err;
  }
  if (new_ref) {
    hle_fsref_make(path, new_ref->hidden);
  }
  if (new_spec) {
    hle_path_to_spec(path, new_spec);
  }
  if (new_dir_id) {
    *new_dir_id = hle_node_id(path);
  }
  return noErr;
}

static OSErr delete_path(const char* path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return fnfErr;
  }
  if ((S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) != 0) {
    return hle_oserr_from_errno(errno);
  }
  hle_appledouble ad;
  hle_appledouble_read(path, &ad);
  if (ad.path[0]) {
    unlink(ad.path);
  }
  return noErr;
}

int FSDeleteObject(const FSRef* ref) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  return err ? err : delete_path(path);
}

int FSRenameUnicode(const FSRef* ref, UniCharCount length, const UniChar* name,
                    TextEncoding hint, FSRef* new_ref) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    return err;
  }
  if (!name || length == 0) {
    return errFSMissingName;
  }
  char dir[PATH_MAX];
  parent_path(path, dir, sizeof(dir));
  char* posix = hle_name_from_unicode(name, length);
  char target[PATH_MAX];
  int n = snprintf(target, sizeof(target), "%s/%s", strcmp(dir, "/") ? dir : "",
                   posix);
  free(posix);
  if (n < 0 || (size_t)n >= sizeof(target)) {
    return bdNamErr;
  }
  char existing[PATH_MAX];
  // A case-only rename is allowed; anything else by that name is not.
  if (hle_path_resolve(target, existing, sizeof(existing)) &&
      strcasecmp(existing, path)) {
    return dupFNErr;
  }
  if (rename(path, target) != 0) {
    return hle_oserr_from_errno(errno);
  }
  hle_appledouble from;
  hle_appledouble to;
  hle_appledouble_read(path, &from);
  hle_appledouble_read(target, &to);
  if (from.path[0] && to.path[0]) {
    rename(from.path, to.path);
  }
  if (new_ref) {
    hle_fsref_make(target, new_ref->hidden);
  }
  return noErr;
}

int FSpCreate(const FSSpec* spec, OSType creator, OSType type,
              SInt16 script) {
  char path[PATH_MAX];
  OSErr err = hle_spec_to_path(spec, path, sizeof(path));
  if (err) {
    return err;
  }
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    return hle_oserr_from_errno(errno);
  }
  close(fd);
  return noErr;
}

int FSpDelete(const FSSpec* spec) {
  char path[PATH_MAX];
  OSErr err = hle_spec_to_path(spec, path, sizeof(path));
  return err ? err : delete_path(path);
}

// Type and creator have nowhere to live on Linux.
int FSpSetFInfo(const FSSpec* spec, const FInfo* info) {
  char path[PATH_MAX];
  OSErr err = hle_spec_to_path(spec, path, sizeof(path));
  if (err) {
    return err;
  }
  struct stat st;
  return stat(path, &st) == 0 ? noErr : fnfErr;
}

// ---------------------------------------------------------------------------
// The volume

// Volumes count from 1: the startup disk, then the user's disc if there is
// one. By reference number, everything but the disc is the startup disk.
static int is_disc(SInt16 vref, int index) {
  if (!hle_cd_volume_name()) {
    return 0;
  }
  return index > 0 ? index == 2 : vref == kHleCdVolumeRefNum;
}

static int volume_count(void) {
  return hle_cd_volume_name() ? 2 : 1;
}

int FSGetVolumeInfo(SInt16 volume, ItemCount index, SInt16* actual,
                    FSVolumeInfoBitmap which, FSVolumeInfo* info,
                    HFSUniStr255* name, FSRef* root) {
  if (volume == 0 && (index < 1 || index > (ItemCount)volume_count())) {
    return nsvErr;
  }
  int disc = is_disc(volume, volume == 0 ? (int)index : 0);
  const char* path = disc ? hle_cd_volume_path() : "/";
  if (actual) {
    *actual = disc ? kHleCdVolumeRefNum : kHleVolumeRefNum;
  }
  if (info) {
    struct statvfs sv;
    memset(info, 0, sizeof(*info));
    if (statvfs(path, &sv) == 0) {
      uint64_t blocks = (uint64_t)sv.f_blocks;
      uint64_t free_blocks = (uint64_t)sv.f_bavail;
      info->blockSize = sv.f_frsize;
      info->totalBytes = blocks * sv.f_frsize;
      info->freeBytes = free_blocks * sv.f_frsize;
      info->totalBlocks = blocks > 0xFFFFFFFFu ? 0xFFFFFFFFu : blocks;
      info->freeBlocks = free_blocks > 0xFFFFFFFFu ? 0xFFFFFFFFu : free_blocks;
    }
    time_t now = time(NULL);
    hle_utc_from_unix(now, &info->createDate);
    hle_utc_from_unix(now, &info->modifyDate);
    info->signature = HFS_PLUS_SIGNATURE;
    info->driveNumber = disc ? 9 : 1;
    info->nextCatalogID = 1000000;
  }
  if (name) {
    hle_name_to_unicode(disc ? hle_cd_volume_name() : kVolumeName, name);
  }
  if (root) {
    hle_fsref_make(path, root->hidden);
  }
  return noErr;
}

// Classic calls count the volume in 16-bit blocks. As the File Manager does
// for large volumes, sizes are reported as at most 2 GB.
int PBHGetVInfoSync(HParamBlockRec* pb) {
  HVolumeParam* v = &pb->volumeParam;
  if (v->ioVolIndex > volume_count()) {
    v->ioResult = nsvErr;
    return nsvErr;
  }
  int disc = is_disc(v->ioVRefNum, v->ioVolIndex);
  const char* path = disc ? hle_cd_volume_path() : "/";
  if (v->ioNamePtr) {
    hle_name_to_pascal(disc ? hle_cd_volume_name() : kVolumeName,
                       v->ioNamePtr, 28);
  }
  struct statvfs sv;
  uint64_t total = 0;
  uint64_t available = 0;
  if (statvfs(path, &sv) == 0) {
    total = (uint64_t)sv.f_blocks * sv.f_frsize;
    available = (uint64_t)sv.f_bavail * sv.f_frsize;
  }
  const uint64_t kCap = 0x7FFFFFFFu;
  const UInt32 kBlock = 0x8000;
  UInt32 now = hle_mac_local_seconds(time(NULL));
  v->ioVRefNum = disc ? kHleCdVolumeRefNum : kHleVolumeRefNum;
  v->ioVCrDate = now;
  v->ioVLsMod = now;
  // A disc is locked in hardware and software.
  v->ioVAtrb = disc ? 0x8080 : 0;
  v->ioVNmFls = 0;
  v->ioVBitMap = 0;
  v->ioAllocPtr = 0;
  v->ioVNmAlBlks = (total < kCap ? total : kCap) / kBlock;
  v->ioVAlBlkSiz = kBlock;
  v->ioVClpSiz = kBlock;
  v->ioAlBlSt = 0;
  v->ioVNxtCNID = 1000000;
  v->ioVFrBlk = (available < kCap ? available : kCap) / kBlock;
  v->ioVSigWord = HFS_PLUS_SIGNATURE;
  v->ioVDrvInfo = disc ? 9 : 1;
  v->ioVDRefNum = disc ? -34 : -33;
  v->ioVFSID = 0;
  v->ioVBkUp = 0;
  v->ioVSeqNum = 0;
  v->ioVWrCnt = 0;
  v->ioVFilCnt = 0;
  v->ioVDirCnt = 0;
  memset(v->ioVFndrInfo, 0, sizeof(v->ioVFndrInfo));
  v->ioResult = noErr;
  return noErr;
}

int PBHGetVolParmsSync(HParamBlockRec* pb) {
  HIOParam* io = &pb->ioParam;
  GetVolParmsInfoBuffer parms;
  memset(&parms, 0, sizeof(parms));
  parms.vMVersion = 4;
  parms.vMMaxNameLength = 255;
  size_t n = io->ioReqCount < (int32_t)sizeof(parms) ? (size_t)io->ioReqCount
                                                     : sizeof(parms);
  if (io->ioBuffer && n) {
    memcpy(io->ioBuffer, &parms, n);
  }
  io->ioActCount = n;
  io->ioResult = noErr;
  return noErr;
}

// Everything is the user's own, with full access.
int PBHGetDirAccessSync(HParamBlockRec* pb) {
  AccessParam* a = &pb->accessParam;
  char path[PATH_MAX];
  OSErr err = classic_path(a->ioVRefNum, a->ioDirID, a->ioNamePtr, path,
                           sizeof(path));
  if (!err) {
    a->ioACUser = 0;
    a->ioACOwnerID = 0;
    a->ioACGroupID = 0;
    a->ioACAccess = 0x07000007;
  }
  a->ioResult = err;
  return err;
}

int PBHCopyFileSync(HParamBlockRec* pb) {
  CopyParam* c = &pb->copyParam;
  char source[PATH_MAX];
  OSErr err = classic_path(c->ioVRefNum, c->ioDirID, c->ioNamePtr, source,
                           sizeof(source));
  char dir[PATH_MAX];
  if (!err) {
    err = classic_path(c->ioDstVRefNum, c->ioNewDirID, c->ioNewName, dir,
                       sizeof(dir));
  }
  char target[PATH_MAX];
  if (!err) {
    char* name = c->ioCopyName ? hle_name_from_pascal(c->ioCopyName)
                               : strdup(base_name(source));
    if (!hle_join(dir, name, target, sizeof(target))) {
      err = bdNamErr;
    }
    free(name);
  }
  if (!err) {
    int in = open(source, O_RDONLY | O_CLOEXEC);
    int out = in < 0 ? -1
                     : open(target, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
                            0644);
    if (in < 0 || out < 0) {
      err = hle_oserr_from_errno(errno);
    } else {
      char buf[65536];
      ssize_t n;
      while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
          err = hle_oserr_from_errno(errno);
          break;
        }
      }
      if (n < 0) {
        err = ioErr;
      }
    }
    if (in >= 0) {
      close(in);
    }
    if (out >= 0) {
      close(out);
    }
  }
  c->ioResult = err;
  return err;
}

// ---------------------------------------------------------------------------
// Classic catalog information

int PBGetCatInfoSync(CInfoPBRec* pb) {
  HFileInfo* f = &pb->hFileInfo;
  DirInfo* d = &pb->dirInfo;
  char path[PATH_MAX];
  OSErr err = noErr;
  UInt32 dir_id = f->ioDirID ? (UInt32)f->ioDirID : hle_default_dir_id();

  if (f->ioFDirIndex > 0) {
    // The index-th item of the folder.
    const char* dir = hle_node_path(dir_id);
    listing items;
    if (!dir || !list_directory(dir, &items)) {
      err = dirNFErr;
    } else {
      if ((size_t)f->ioFDirIndex > items.count ||
          !hle_join(dir, items.names[f->ioFDirIndex - 1], path,
                    sizeof(path))) {
        err = fnfErr;
      } else if (f->ioNamePtr) {
        hle_name_to_pascal(items.names[f->ioFDirIndex - 1], f->ioNamePtr, 64);
      }
      free_listing(&items);
    }
  } else if (f->ioFDirIndex < 0) {
    // The folder itself.
    const char* dir = hle_node_path(dir_id);
    if (!dir) {
      err = dirNFErr;
    } else {
      snprintf(path, sizeof(path), "%s", dir);
      if (f->ioNamePtr) {
        hle_name_to_pascal(base_name(dir), f->ioNamePtr, 64);
      }
    }
  } else {
    err = classic_path(f->ioVRefNum, f->ioDirID, f->ioNamePtr, path,
                       sizeof(path));
  }

  struct stat st;
  if (!err && stat(path, &st) != 0) {
    err = fnfErr;
  }
  if (err) {
    f->ioResult = err;
    return err;
  }

  f->ioVRefNum = kHleVolumeRefNum;
  f->ioFRefNum = 0;
  f->ioFVersNum = 0;
  f->ioACUser = 0;
  UInt32 local_mtime = hle_mac_local_seconds(st.st_mtime);
  if (S_ISDIR(st.st_mode)) {
    d->ioFlAttrib = 0x10;
    memset(&d->ioDrUsrWds, 0, sizeof(d->ioDrUsrWds));
    d->ioDrDirID = hle_node_id(path);
    UInt32 items = count_items(path);
    d->ioDrNmFls = items > 0xFFFF ? 0xFFFF : items;
    memset(d->filler3, 0, sizeof(d->filler3));
    d->ioDrCrDat = local_mtime;
    d->ioDrMdDat = local_mtime;
    d->ioDrBkDat = 0;
    memset(&d->ioDrFndrInfo, 0, sizeof(d->ioDrFndrInfo));
    d->ioDrParID = parent_id(path);
  } else {
    hle_appledouble ad;
    int have_ad = hle_appledouble_read(path, &ad);
    f->ioFlAttrib = access(path, W_OK) != 0 ? 0x01 : 0;
    if (have_ad && ad.has_finder_info) {
      finder_info_from_disk(ad.finder_info, 0, (UInt8*)&f->ioFlFndrInfo,
                            (UInt8*)&f->ioFlXFndrInfo);
    } else {
      memset(&f->ioFlFndrInfo, 0, sizeof(f->ioFlFndrInfo));
      memset(&f->ioFlXFndrInfo, 0, sizeof(f->ioFlXFndrInfo));
    }
    f->ioDirID = hle_node_id(path);
    f->ioFlStBlk = 0;
    f->ioFlLgLen = st.st_size > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)st.st_size;
    uint64_t physical = (uint64_t)st.st_blocks * 512;
    f->ioFlPyLen = physical > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)physical;
    f->ioFlRStBlk = 0;
    f->ioFlRLgLen = have_ad && ad.has_resource_fork ? ad.resource_length : 0;
    f->ioFlRPyLen = f->ioFlRLgLen;
    f->ioFlCrDat = local_mtime;
    f->ioFlMdDat = local_mtime;
    f->ioFlBkDat = 0;
    f->ioFlParID = parent_id(path);
    f->ioFlClpSiz = 0;
  }
  f->ioResult = noErr;
  return noErr;
}

int PBMakeFSRefSync(FSRefParam* pb) {
  char path[PATH_MAX];
  OSErr err = classic_path(pb->ioVRefNum, pb->ioDirID, pb->ioNamePtr, path,
                           sizeof(path));
  if (!err && !hle_fsref_make(path, pb->newRef->hidden)) {
    err = fnfErr;
  }
  pb->ioResult = err;
  return err;
}
