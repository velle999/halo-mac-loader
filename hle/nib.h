// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Interface Builder's Carbon NIBs. A name.nib bundle keeps its windows and
// menus in objects.xib: XML objects with a class, typed properties named by
// a name attribute, and references to other objects by id. Nothing is
// drawn from them; the window code builds a model the game can query.

#ifndef HLE_NIB_H_
#define HLE_NIB_H_

typedef struct xml_node {
  char* tag;
  char** attributes;  // name, value, name, value...
  int attribute_count;
  char* text;
  struct xml_node** children;
  int child_count;
} xml_node;

typedef struct hle_nib hle_nib;

// Opens name.nib from the main bundle's resources. NULL when there is none.
hle_nib* hle_nib_open(const char* name);
void hle_nib_close(hle_nib* nib);

// The object the NIB's name table gives |name| to, such as "MenuBar".
xml_node* hle_nib_named(hle_nib* nib, const char* name);

const char* xml_attribute(const xml_node* node, const char* name);
// The child element whose name attribute is |name|.
xml_node* xml_named_child(const xml_node* node, const char* name);

// A property's text: <string name="title">, <int name="controlID"> and so on.
const char* hle_nib_property(const xml_node* object, const char* name);
// A property holding an object, following a reference to its definition.
xml_node* hle_nib_object(hle_nib* nib, const xml_node* object,
                         const char* name);
// Follows a <reference idRef="..."/> to the object it names.
xml_node* hle_nib_resolve(hle_nib* nib, xml_node* node);

#endif  // HLE_NIB_H_
