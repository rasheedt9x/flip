// flip: a page-curl demo: images from a .cbz or a folder, turned like paper. Shared types between game.c (library screen,
// reader, page curl) and book.c (folders, CBZ, image decoding, the storage permission).
#ifndef FLIP_H
#define FLIP_H

#include "game.h"

#define PATHLEN 512
#define MAX_ENTRIES 2048
#define MAX_PAGES 1024

typedef struct { char name[256]; bool dir; } Entry;

// one page of a chapter: a file in a folder, or an entry in a .cbz (zip)
typedef struct {
    char name[200];
    unsigned off, csize, usize;  // zip only: local header offset, compressed/uncompressed size
    int method;                  // zip only: 0 stored, 8 deflate
} Page;

typedef struct {
    char path[PATHLEN];
    bool zip;
    int n;
    Page pages[MAX_PAGES];
} Chapter;

// book.c
int ListDir(const char *path, Entry *out, int max);       // dirs + .cbz, natural order; -1 if unreadable
bool OpenChapter(const char *path, Chapter *c);           // a .cbz or a folder of images
Image LoadPage(const Chapter *c, int i, int maxW, int maxH);  // decoded and scaled to fit, RGBA8
bool DirHasImages(const char *path);
void OpenAllFilesSettings(void);                          // Android's "All files access" screen for this app
bool HasAllFilesAccess(void);                             // Environment.isExternalStorageManager()
int NavBarHeight(void);                                   // pixels at the bottom covered by the nav bar
const char *DataPath(void);                               // the app's private files dir

#endif
