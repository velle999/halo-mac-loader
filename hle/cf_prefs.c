// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// CFPreferences: one XML property list per application ID, in
// Library/Preferences under the stand-in home folder, as ~/Library/Preferences
// on a Mac. Changes stay in memory until the game synchronizes.

#define _GNU_SOURCE

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf.h"

struct prefs_domain {
  char* app_id;
  char* path;
  CFMutableDictionaryRef values;
  int dirty;
  struct prefs_domain* next;
};

static pthread_mutex_t prefs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct prefs_domain* domains;

// kCFPreferencesCurrentApplication means the main bundle's identifier.
static char* app_id_utf8(CFStringRef app_id) {
  if (!app_id || app_id == kCFPreferencesCurrentApplication ||
      cf_string_is_ascii(app_id, "kCFPreferencesCurrentApplication")) {
    CFStringRef identifier = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (identifier && cf_is(identifier, CF_TYPE_STRING)) {
      return cf_string_utf8(identifier);
    }
    return strdup("halo-mac-loader");
  }
  return cf_string_utf8(app_id);
}

// Called with prefs_lock held.
static struct prefs_domain* domain_for(CFStringRef app_id) {
  char* id = app_id_utf8(app_id);
  for (struct prefs_domain* d = domains; d; d = d->next) {
    if (!strcmp(d->app_id, id)) {
      free(id);
      return d;
    }
  }
  struct prefs_domain* d = calloc(1, sizeof(*d));
  d->app_id = id;
  size_t size = strlen(hle_mac_home()) + strlen(id) + 64;
  d->path = malloc(size);
  snprintf(d->path, size, "%s/Library/Preferences/%s.plist", hle_mac_home(),
           id);
  CFPropertyListRef stored = cf_plist_read_file(d->path);
  if (stored && cf_is(stored, CF_TYPE_DICTIONARY)) {
    d->values = (CFMutableDictionaryRef)stored;
  } else {
    if (stored) {
      CFRelease(stored);
    }
    d->values = cf_dict_create();
  }
  d->next = domains;
  domains = d;
  return d;
}

CFPropertyListRef CFPreferencesCopyAppValue(CFStringRef key,
                                            CFStringRef app_id) {
  pthread_mutex_lock(&prefs_lock);
  struct prefs_domain* d = domain_for(app_id);
  CFTypeRef value = cf_dict_get(d->values, key);
  if (value) {
    CFRetain(value);
  }
  pthread_mutex_unlock(&prefs_lock);
  if (cf_trace_enabled) {
    char* k = cf_string_utf8(key);
    cf_trace("CFPreferencesCopyAppValue(%s) = %s", k, value ? "set" : "unset");
    free(k);
  }
  return value;
}

void CFPreferencesSetAppValue(CFStringRef key, CFPropertyListRef value,
                              CFStringRef app_id) {
  pthread_mutex_lock(&prefs_lock);
  struct prefs_domain* d = domain_for(app_id);
  if (value) {
    cf_dict_set(d->values, key, value);
  } else {
    cf_dict_remove(d->values, key);
  }
  d->dirty = 1;
  pthread_mutex_unlock(&prefs_lock);
}

int CFPreferencesAppSynchronize(CFStringRef app_id) {
  pthread_mutex_lock(&prefs_lock);
  struct prefs_domain* d = domain_for(app_id);
  int ok = 1;
  if (d->dirty) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/Library/Preferences", hle_mac_home());
    ok = hle_mkdirs(dir) && cf_plist_write_file(d->values, d->path);
    if (ok) {
      d->dirty = 0;
    }
  }
  pthread_mutex_unlock(&prefs_lock);
  return ok;
}

// The user and host variants, which the game does not import, share the
// per-application store.
CFPropertyListRef CFPreferencesCopyValue(CFStringRef key, CFStringRef app_id,
                                         CFStringRef user, CFStringRef host) {
  return CFPreferencesCopyAppValue(key, app_id);
}

void CFPreferencesSetValue(CFStringRef key, CFPropertyListRef value,
                           CFStringRef app_id, CFStringRef user,
                           CFStringRef host) {
  CFPreferencesSetAppValue(key, value, app_id);
}

int CFPreferencesSynchronize(CFStringRef app_id, CFStringRef user,
                             CFStringRef host) {
  return CFPreferencesAppSynchronize(app_id);
}
