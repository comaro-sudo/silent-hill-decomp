/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fs_ogc.c - filesystem setup for GameCube and Wii (libfat), and Wii U (wut's
 * SD devoptab, already mounted at fs:/vol/external01).
 *
 * Implements the Sh360Fs_* interface (xbox360_port/include/fs_xbox360.h) that
 * the shared 360 code (cd_xbox360.c, mcard_xbox.c) calls. fatInitDefault()
 * mounts every FAT volume libfat knows for the console; the job here is to pick
 * the one holding the user's disc image and make sure the save folder exists.
 *
 * Volume names (libfat-ogc): Wii "sd:" (front SD slot) and "usb:"; GameCube
 * "carda:" / "cardb:" (SD Gecko in a memory-card slot) and "sd2:" (SD2SP2 in
 * the serial port 2 slot).
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(SH_WIIU_PORT)
#include <fat.h>
#endif

#include "sh_log.h"
#include "fs_xbox360.h"

static char s_dataRoot[SH360_PATH_MAX] = "";
static char s_binName[SH360_PATH_MAX]  = "";
static int  s_fatOk;

const char* Sh360Fs_DataRoot(void) { return s_dataRoot; }
const char* Sh360Fs_BinName(void)  { return s_binName;  }

#if defined(SH_WIIU_PORT)
/* wut mounts the SD card as fs:/vol/external01 before main */
static const char* const kRoots[] = {
    "fs:/vol/external01/silenthill/", "fs:/vol/external01/wiiu/apps/silenthill/",
    "fs:/vol/external01/",
};
#elif defined(SH_GC_PORT)
static const char* const kRoots[] = {
    "sd2:/silenthill/", "carda:/silenthill/", "cardb:/silenthill/",
    "sd2:/", "carda:/", "cardb:/",
};
#else
static const char* const kRoots[] = {
    "sd:/silenthill/", "usb:/silenthill/", "sd:/apps/silenthill/", "sd:/", "usb:/",
};
#endif
#define N_ROOTS ((int)(sizeof(kRoots) / sizeof(kRoots[0])))

static int EndsWithNoCase(const char* s, const char* suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return 0;
    s += ls - lx;
    while (*s) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*suffix))
            return 0;
        s++; suffix++;
    }
    return 1;
}

/* The disc image is named by whoever ripped it, so scan rather than guess. */
static int DirHasBin(const char* dir, char* binOut, int binOutSize)
{
    DIR*           d = opendir(dir);
    struct dirent* e;
    int            found = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (EndsWithNoCase(e->d_name, ".bin")) {
            snprintf(binOut, binOutSize, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* A writable root for logs even with no disc image: the first that opens. */
const char* Sh360Fs_WritableRoot(void)
{
    static char s_root[SH360_PATH_MAX];
    int i;
    if (s_dataRoot[0])
        return s_dataRoot;
    if (s_root[0])
        return s_root;
    for (i = 0; i < N_ROOTS; i++) {
        DIR* d = opendir(kRoots[i]);
        if (d) {
            closedir(d);
            snprintf(s_root, sizeof(s_root), "%s", kRoots[i]);
            return s_root;
        }
    }
    return "";
}

int Sh360Fs_Init(void)
{
    int i;

    if (s_dataRoot[0])
        return s_binName[0] != '\0';

#if defined(SH_WIIU_PORT)
    s_fatOk = 1;
#else
    s_fatOk = fatInitDefault() ? 1 : 0;
#endif
    if (!s_fatOk)
        return 0;

    for (i = 0; i < N_ROOTS; i++) {
        if (DirHasBin(kRoots[i], s_binName, sizeof(s_binName))) {
            snprintf(s_dataRoot, sizeof(s_dataRoot), "%s", kRoots[i]);
            return 1;
        }
    }
    snprintf(s_dataRoot, sizeof(s_dataRoot), "%s", Sh360Fs_WritableRoot());
    return 0;
}

static int EnsureDir(const char* path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return 1;
    return mkdir(path, 0777) == 0;
}

int XboxFs_EnsureDir(const char* path) { return EnsureDir(path); }

/* Used by the shared mcard_xbox.c, hence the Xbox name. */
int XboxFs_ResolveSaveDir(char* out, int outSize)
{
    char dir[SH360_PATH_MAX];

    Sh360Fs_Init();
    snprintf(dir, sizeof(dir), "%ssave", s_dataRoot);
    if (!EnsureDir(dir)) {
        SH_DBG("[MCRD] could not create '%s'", dir);
        return 0;
    }
    snprintf(out, outSize, "%s", dir);
    return 1;
}

/* fflush alone leaves the FAT directory entry (and so the file SIZE) stale
 * until close; a console session ends with the power button. */
void Sh360Fs_Sync(FILE* f)
{
    if (!f)
        return;
    fflush(f);
    fsync(fileno(f));
}
