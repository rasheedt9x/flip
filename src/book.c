// Files: folder listing, .cbz (zip) reading, image decoding with Android's AImageDecoder (webp/jpg/png/...), and
// the one JNI call that opens the "All files access" settings page.
// Plain stdio here on purpose: raylib's own LoadFileData goes through android_fopen, which prefixes absolute paths
// with the app's data dir.
#include "flip.h"
#include <android/imagedecoder.h>
#include <android_native_app_glue.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

struct android_app *GetAndroidApp(void);  // raylib (rcore_android.c)

// "Chapter 2" < "Chapter 10"
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            int la = 0, lb = 0;
            while (isdigit((unsigned char)a[la])) la++;
            while (isdigit((unsigned char)b[lb])) lb++;
            if (la != lb) return la - lb;
            int c = strncmp(a, b, la);
            if (c) return c;
            a += la, b += lb;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++, b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int entrycmp(const void *x, const void *y) { return natcmp(((const Entry *)x)->name, ((const Entry *)y)->name); }
static int pagecmp(const void *x, const void *y) { return natcmp(((const Page *)x)->name, ((const Page *)y)->name); }

static bool endswith(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcasecmp(s + n - m, suf) == 0;
}

static bool isimage(const char *name)
{
    static const char *ext[] = { ".jpg", ".jpeg", ".png", ".webp", ".gif", ".avif", ".heic", ".bmp" };
    const char *base = strrchr(name, '/');
    if ((base ? base[1] : name[0]) == '.') return false;  // __MACOSX junk, hidden files
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) if (endswith(name, ext[i])) return true;
    return false;
}

int ListDir(const char *path, Entry *out, int max)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    char full[PATHLEN];
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        if (endswith(e->d_name, "_tmp")) continue;  // unfinished downloads (left by download apps)
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        bool dir = S_ISDIR(st.st_mode);
        if (!dir && !endswith(e->d_name, ".cbz") && !endswith(e->d_name, ".zip")) continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
        out[n++].dir = dir;
    }
    closedir(d);
    qsort(out, n, sizeof *out, entrycmp);
    return n;
}

bool DirHasImages(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) found = isimage(e->d_name);
    closedir(d);
    return found;
}

static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static bool openzip(const char *path, Chapter *c)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;  // EOCD (22 bytes) + max comment
    unsigned char *buf = malloc(tail);
    fseek(f, size - tail, SEEK_SET);
    bool ok = fread(buf, 1, tail, f) == (size_t)tail;
    long eocd = -1;
    for (long i = tail - 22; ok && i >= 0; i--) if (u32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(buf); fclose(f); return false; }
    unsigned count = u16(buf + eocd + 10), cdSize = u32(buf + eocd + 12), cdOff = u32(buf + eocd + 16);
    free(buf);

    unsigned char *cd = malloc(cdSize);
    fseek(f, cdOff, SEEK_SET);
    ok = fread(cd, 1, cdSize, f) == cdSize;
    fclose(f);
    unsigned p = 0;
    for (unsigned i = 0; ok && i < count && p + 46 <= cdSize && c->n < MAX_PAGES; i++) {
        if (u32(cd + p) != 0x02014b50) break;
        unsigned nlen = u16(cd + p + 28), elen = u16(cd + p + 30), clen = u16(cd + p + 32);
        if (p + 46 + nlen > cdSize) break;
        char name[sizeof c->pages[0].name];
        snprintf(name, sizeof name, "%.*s", (int)nlen, cd + p + 46);
        if (isimage(name)) {
            Page *pg = &c->pages[c->n++];
            memcpy(pg->name, name, sizeof name);
            pg->method = u16(cd + p + 10);
            pg->csize = u32(cd + p + 20);
            pg->usize = u32(cd + p + 24);
            pg->off = u32(cd + p + 42);
        }
        p += 46 + nlen + elen + clen;
    }
    free(cd);
    return c->n > 0;
}

bool OpenChapter(const char *path, Chapter *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s", path);
    struct stat st;
    if (stat(path, &st)) return false;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return false;
        struct dirent *e;
        while ((e = readdir(d)) && c->n < MAX_PAGES)
            if (isimage(e->d_name)) snprintf(c->pages[c->n++].name, sizeof c->pages[0].name, "%s", e->d_name);
        closedir(d);
    } else {
        c->zip = true;
        if (!openzip(path, c)) return false;
    }
    qsort(c->pages, c->n, sizeof *c->pages, pagecmp);
    return c->n > 0;
}

// the page's file bytes (malloc'd)
static unsigned char *readpage(const Chapter *c, int i, size_t *len)
{
    const Page *pg = &c->pages[i];
    char full[PATHLEN + 256];
    if (!c->zip) snprintf(full, sizeof full, "%s/%s", c->path, pg->name);
    FILE *f = fopen(c->zip ? c->path : full, "rb");
    if (!f) return NULL;
    unsigned char *data = NULL;
    if (!c->zip) {
        fseek(f, 0, SEEK_END);
        *len = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = malloc(*len);
        if (fread(data, 1, *len, f) != *len) { free(data); data = NULL; }
        fclose(f);
        return data;
    }
    unsigned char lh[30];
    fseek(f, pg->off, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || u32(lh) != 0x04034b50) { fclose(f); return NULL; }
    fseek(f, pg->off + 30 + u16(lh + 26) + u16(lh + 28), SEEK_SET);
    unsigned char *comp = malloc(pg->csize ? pg->csize : 1);
    bool ok = fread(comp, 1, pg->csize, f) == pg->csize;
    fclose(f);
    if (!ok) { free(comp); return NULL; }
    if (pg->method == 0) { *len = pg->csize; return comp; }
    if (pg->method == 8) {  // raylib's DecompressData is raw deflate (sinfl), which is what zip stores
        int n = 0;
        unsigned char *out = DecompressData(comp, pg->csize, &n);
        free(comp);
        *len = n;
        if (!out || n <= 0) { MemFree(out); return NULL; }
        data = malloc(n);  // DecompressData's buffer belongs to raylib's allocator
        memcpy(data, out, n);
        MemFree(out);
        return data;
    }
    TraceLog(LOG_WARNING, "FLIP: %s: zip method %d not supported", pg->name, pg->method);
    free(comp);
    return NULL;
}

Image LoadPage(const Chapter *c, int i, int maxW, int maxH)
{
    Image img = { 0 };
    size_t len = 0;
    unsigned char *data = readpage(c, i, &len);
    if (!data) return img;
    AImageDecoder *d;
    if (AImageDecoder_createFromBuffer(data, len, &d) != ANDROID_IMAGE_DECODER_SUCCESS) {
        TraceLog(LOG_WARNING, "FLIP: can't decode %s", c->pages[i].name);
        free(data);
        return img;
    }
    AImageDecoder_setAndroidBitmapFormat(d, ANDROID_BITMAP_FORMAT_RGBA_8888);
    AImageDecoder_setUnpremultipliedRequired(d, true);
    const AImageDecoderHeaderInfo *h = AImageDecoder_getHeaderInfo(d);
    int w = AImageDecoderHeaderInfo_getWidth(h), ht = AImageDecoderHeaderInfo_getHeight(h);
    float s = fminf(1, fminf((float)maxW / w, (float)maxH / ht));
    if (s < 1) {
        int tw = (int)(w * s), th = (int)(ht * s);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        if (AImageDecoder_setTargetSize(d, tw, th) == ANDROID_IMAGE_DECODER_SUCCESS) w = tw, ht = th;
    }
    size_t stride = AImageDecoder_getMinimumStride(d);
    unsigned char *px = malloc(stride * ht);
    int r = AImageDecoder_decodeImage(d, px, stride, stride * ht);
    AImageDecoder_delete(d);
    free(data);
    if (r != ANDROID_IMAGE_DECODER_SUCCESS && r != ANDROID_IMAGE_DECODER_INCOMPLETE) {
        TraceLog(LOG_WARNING, "FLIP: decode failed (%d): %s", r, c->pages[i].name);
        free(px);
        return img;
    }
    if (stride != (size_t)w * 4)
        for (int y = 1; y < ht; y++) memmove(px + (size_t)y * w * 4, px + y * stride, (size_t)w * 4);
    return (Image){ px, w, ht, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
}

// new Intent("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION", Uri.parse("package:<pkg>")) -> startActivity
void OpenAllFilesSettings(void)
{
    ANativeActivity *act = GetAndroidApp()->activity;
    JNIEnv *env;
    (*act->vm)->AttachCurrentThread(act->vm, &env, NULL);
    jobject activity = act->clazz;
    jclass actCls = (*env)->GetObjectClass(env, activity);
    jstring pkg = (*env)->CallObjectMethod(env, activity,
        (*env)->GetMethodID(env, actCls, "getPackageName", "()Ljava/lang/String;"));
    const char *p = (*env)->GetStringUTFChars(env, pkg, NULL);
    char uriStr[256];
    snprintf(uriStr, sizeof uriStr, "package:%s", p);
    (*env)->ReleaseStringUTFChars(env, pkg, p);

    jclass uriCls = (*env)->FindClass(env, "android/net/Uri");
    jobject uri = (*env)->CallStaticObjectMethod(env, uriCls,
        (*env)->GetStaticMethodID(env, uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
        (*env)->NewStringUTF(env, uriStr));
    jclass intentCls = (*env)->FindClass(env, "android/content/Intent");
    jobject intent = (*env)->NewObject(env, intentCls,
        (*env)->GetMethodID(env, intentCls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
        (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
    (*env)->CallVoidMethod(env, activity,
        (*env)->GetMethodID(env, actCls, "startActivity", "(Landroid/content/Intent;)V"), intent);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*act->vm)->DetachCurrentThread(act->vm);
}

bool HasAllFilesAccess(void)
{
    ANativeActivity *act = GetAndroidApp()->activity;
    JNIEnv *env;
    (*act->vm)->AttachCurrentThread(act->vm, &env, NULL);
    jclass envCls = (*env)->FindClass(env, "android/os/Environment");
    jboolean ok = (*env)->CallStaticBooleanMethod(env, envCls,
        (*env)->GetStaticMethodID(env, envCls, "isExternalStorageManager", "()Z"));
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ok = false; }
    (*env)->DeleteLocalRef(env, envCls);
    (*act->vm)->DetachCurrentThread(act->vm);
    return ok;
}

const char *DataPath(void) { return GetAndroidApp()->activity->internalDataPath; }

// height of the navigation bar the app draws under (edge-to-edge is forced from targetSdk 35)
int NavBarHeight(void)
{
    ANativeActivity *act = GetAndroidApp()->activity;
    JNIEnv *env;
    (*act->vm)->AttachCurrentThread(act->vm, &env, NULL);
    jclass actCls = (*env)->GetObjectClass(env, act->clazz);
    jobject res = (*env)->CallObjectMethod(env, act->clazz,
        (*env)->GetMethodID(env, actCls, "getResources", "()Landroid/content/res/Resources;"));
    jclass resCls = (*env)->GetObjectClass(env, res);
    jint id = (*env)->CallIntMethod(env, res,
        (*env)->GetMethodID(env, resCls, "getIdentifier", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I"),
        (*env)->NewStringUTF(env, "navigation_bar_height"), (*env)->NewStringUTF(env, "dimen"),
        (*env)->NewStringUTF(env, "android"));
    jint px = id ? (*env)->CallIntMethod(env, res, (*env)->GetMethodID(env, resCls, "getDimensionPixelSize", "(I)I"), id) : 0;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); px = 0; }
    (*act->vm)->DetachCurrentThread(act->vm);
    return px;
}
