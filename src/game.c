// flip: browse a folder, open a .cbz or a folder of images and turn its pages like paper.
//
// Page curl: the page being turned is a grid mesh. Everything past a fold line is wrapped around a cylinder of
// radius R lying on the page, and what goes past the top of the cylinder lies flat again, face down. Triangles
// that end up mirrored show the back of the page (a washed-out copy of it, like ink showing through paper).
// Everything is computed in "logical" page coordinates (spine at x=0, pages turn towards -x); RTL just
// mirrors x on the way to the screen.
#include "flip.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE 6
#define GX 40           // curl mesh cells across
#define GY 80           // and down
#define BAR 150         // library header height
#define ROW 128         // library row height
#define PAPER (Color){ 250, 248, 242, 255 }
#define BG (Color){ 24, 22, 26, 255 }
#define ACCENT (Color){ 230, 180, 120, 255 }
#define STORAGE "/sdcard"

enum { LIBRARY, READER };

typedef struct {
    bool started;
    int screen;
    Font font;

    // library
    char root[PATHLEN];  // the user's folder ("" = not picked yet)
    int navH;            // nav bar height: the window extends under it
    bool picking;
    char dir[PATHLEN];
    Entry entries[MAX_ENTRIES];
    int nEntries;
    bool noAccess;
    float accessRetry;
    float scroll, scrollVel;

    // touch (shared by both screens)
    bool down, moved;
    Vector2 pressPos, lastPos;
    double pressTime;
    float pressScroll, vx;

    // reader
    Chapter ch;
    char title[256];
    int page;
    bool rtl, overlay;
    RenderTexture2D sheet[CACHE];  // pages drawn onto paper, screen-sized
    int sheetPage[CACHE];          // page + 1, 0 = empty
    int sheetUse[CACHE], useClock;
    int sheetW, sheetH;
    RenderTexture2D back;          // the back of the page being turned
    int backPage;                  // page + 1
    rlRenderBatch curlBatch;       // big enough for the whole curl mesh (see drawcurl)

    // the page turn
    int turn;                      // 0 none, 1 forward (page -> page+1), -1 backward
    bool dragging, finishing;      // finishing: animate to the turned-over end, else back to flat
    // the fold line, in logical coordinates: it crosses height pivotY at x = X and leans by tilt (dx per dy;
    // > 0 = the bottom of the page is further over). X = page width: flat, X = turnedX(): turned over.
    float X, tilt, pivotY;
    float startX, grabTilt;
    float fromX, toX, fromTilt, animT, animDur, animLift;  // release/tap animation; animLift: extra tilt mid-way
} Game;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static void text(Game *g, const char *s, float x, float y, float size, Color c)
{
    DrawTextEx(g->font, s, (Vector2){ x, y }, size, 0, c);
}

// ---------------------------------------------------------------------------------------------- library screen
//
// The library is a folder the user picks once (saved in the app's data dir, root.txt). Picking mode browses
// /sdcard; in normal mode Up stops at the chosen folder. A folder with images in it, or a .cbz, opens as a chapter.

static const char *top(Game *g) { return g->picking ? STORAGE : g->root; }

static void opendirectory(Game *g, const char *path)
{
    snprintf(g->dir, sizeof g->dir, "%s", path);
    g->nEntries = ListDir(g->dir, g->entries, MAX_ENTRIES);
    if (g->nEntries < 0) g->nEntries = 0;
    g->scroll = g->scrollVel = 0;
}

static void clearsheets(Game *g)
{
    for (int i = 0; i < CACHE; i++) g->sheetPage[i] = 0;
    g->backPage = 0;
}

static bool openchapter(Game *g, const char *path, const char *name)
{
    if (!OpenChapter(path, &g->ch)) return false;
    snprintf(g->title, sizeof g->title, "%s", name);
    g->page = 0;
    g->turn = 0;
    g->overlay = false;
    clearsheets(g);
    g->screen = READER;
    return true;
}

static void loadroot(Game *g)
{
    g->root[0] = 0;
    FILE *f = fopen(TextFormat("%s/root.txt", DataPath()), "r");
    if (!f) return;
    if (fgets(g->root, sizeof g->root, f)) g->root[strcspn(g->root, "\n")] = 0;
    fclose(f);
}

static void saveroot(Game *g)
{
    FILE *f = fopen(TextFormat("%s/root.txt", DataPath()), "w");
    if (f) { fprintf(f, "%s\n", g->root); fclose(f); }
}

static void startpicking(Game *g)
{
    g->picking = true;
    opendirectory(g, g->root[0] ? g->root : STORAGE);
}

static void goup(Game *g)
{
    if (strcmp(g->dir, top(g)) == 0) {
        if (g->picking && g->root[0]) { g->picking = false; opendirectory(g, g->root); }  // cancel picking
        return;
    }
    char *slash = strrchr(g->dir, '/');
    if (!slash) return;
    char parent[PATHLEN];
    snprintf(parent, sizeof parent, "%.*s", (int)(slash - g->dir), g->dir);
    opendirectory(g, parent);
}

static void library(Game *g, float dt)
{
    int w = GetScreenWidth(), h = GetScreenHeight();
    Vector2 m = GetMousePosition();
    float listBottom = g->picking ? h - g->navH - BAR : h;
    float maxScroll = fmaxf(0, g->nEntries * ROW - (listBottom - BAR));

    // "All files access" (MANAGE_EXTERNAL_STORAGE): without it only media files are visible, never .cbz
    g->accessRetry -= dt;
    if (g->accessRetry <= 0) {
        g->accessRetry = 1;
        bool had = !g->noAccess;
        g->noAccess = !HasAllFilesAccess();
        if (!had && !g->noAccess) opendirectory(g, g->dir);  // just granted: list again
    }
    if (g->noAccess) {
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && m.y > h * 0.5f && m.y < h * 0.5f + 150) OpenAllFilesSettings();
        BeginDrawing();
        ClearBackground(BG);
        text(g, "flip reads .cbz files and images", 60, h * 0.5f - 220, 46, RAYWHITE);
        text(g, "from your storage.", 60, h * 0.5f - 160, 46, RAYWHITE);
        text(g, "Allow \"All files access\" for it:", 60, h * 0.5f - 90, 46, GRAY);
        DrawRectangleRounded((Rectangle){ 60, h * 0.5f, w - 120, 150 }, 0.3f, 8, ACCENT);
        text(g, "Open settings", w * 0.5f - 150, h * 0.5f + 48, 54, BG);
        EndDrawing();
        return;
    }
    if (IsKeyPressed(KEY_BACK)) goup(g);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g->down = true, g->moved = false;
        g->pressPos = g->lastPos = m;
        g->pressScroll = g->scroll;
        g->scrollVel = 0;
    }
    if (g->down && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (fabsf(m.y - g->pressPos.y) > 20) g->moved = true;
        if (g->moved) {
            g->scroll = g->pressScroll - (m.y - g->pressPos.y);
            if (dt > 0) g->scrollVel = g->scrollVel * 0.5f + 0.5f * (g->lastPos.y - m.y) / dt;
        }
        g->lastPos = m;
    }
    bool atTop = strcmp(g->dir, top(g)) == 0;
    if (g->down && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        g->down = false;
        if (!g->moved) {
            if (m.y < BAR) {
                if (m.x < 260 && !atTop) goup(g);
                else if (m.x > w - 260 && !g->picking) startpicking(g);
            } else if (g->picking && m.y > listBottom) {  // "Use this folder"
                snprintf(g->root, sizeof g->root, "%s", g->dir);
                saveroot(g);
                g->picking = false;
                opendirectory(g, g->root);
            } else {
                int i = (int)((m.y - BAR + g->scroll) / ROW);
                if (i >= 0 && i < g->nEntries) {
                    Entry *e = &g->entries[i];
                    char path[PATHLEN];
                    snprintf(path, sizeof path, "%s/%s", g->dir, e->name);
                    if (g->picking) { if (e->dir) opendirectory(g, path); }
                    else if (!e->dir || DirHasImages(path)) openchapter(g, path, e->name);
                    else opendirectory(g, path);
                }
            }
        }
    }
    if (!g->down) {
        g->scroll += g->scrollVel * dt;
        g->scrollVel *= expf(-dt * 3);
    }
    g->scroll = clampf(g->scroll, 0, maxScroll);

    BeginDrawing();
    ClearBackground(BG);
    BeginScissorMode(0, BAR, w, listBottom - BAR);
    for (int i = (int)(g->scroll / ROW); i < g->nEntries; i++) {
        float y = BAR + i * ROW - g->scroll;
        if (y > listBottom) break;
        Entry *e = &g->entries[i];
        DrawRectangle(0, y + ROW - 2, w, 2, (Color){ 50, 46, 54, 255 });
        if (e->dir) DrawTriangle((Vector2){ 40, y + 46 }, (Vector2){ 40, y + 82 }, (Vector2){ 66, y + 64 }, ACCENT);
        else DrawCircle(52, y + 64, 9, GRAY);
        // ellipsize long names
        char buf[256];
        snprintf(buf, sizeof buf, "%s", e->name);
        int len = strlen(buf);
        while (len > 4 && MeasureTextEx(g->font, buf, 46, 0).x > w - 130) {
            buf[--len] = 0;
            memcpy(buf + len - 3, "...", 3);
        }
        text(g, buf, 96, y + 38, 46, g->picking && !e->dir ? GRAY : RAYWHITE);
    }
    EndScissorMode();
    if (g->nEntries == 0) text(g, g->picking ? "no folders here" : "nothing here", 60, BAR + 60, 46, GRAY);

    DrawRectangle(0, 0, w, BAR, (Color){ 36, 33, 40, 255 });
    if (!atTop) text(g, "‹ Up", 36, 50, 54, ACCENT);
    const char *name = strrchr(g->dir, '/');
    char hdr[128];
    snprintf(hdr, sizeof hdr, "%.26s", g->picking && atTop ? "Pick a folder" : name ? name + 1 : g->dir);
    text(g, hdr, atTop ? 36 : 230, 52, 50, RAYWHITE);
    if (!g->picking) text(g, "Folder", w - 200, 52, 50, ACCENT);
    if (g->picking) {
        DrawRectangle(0, listBottom, w, h - listBottom, (Color){ 36, 33, 40, 255 });
        DrawRectangleRounded((Rectangle){ 40, listBottom + 20, w - 80, BAR - 40 }, 0.4f, 8, ACCENT);
        text(g, "Use this folder", w * 0.5f - 170, listBottom + 46, 54, BG);
    }
    EndDrawing();
}

// ---------------------------------------------------------------------------------------------- page sheets

// a page drawn onto a screen-sized sheet of paper (decoded on demand, LRU cache)
static Texture2D *sheet(Game *g, int page, bool load)
{
    int w = GetScreenWidth(), h = GetScreenHeight();
    if (w != g->sheetW || h != g->sheetH) {  // rotated: everything is the wrong size
        for (int i = 0; i < CACHE; i++) if (g->sheet[i].id) { UnloadRenderTexture(g->sheet[i]); g->sheet[i].id = 0; }
        if (g->back.id) { UnloadRenderTexture(g->back); g->back.id = 0; }
        clearsheets(g);
        g->sheetW = w, g->sheetH = h;
    }
    for (int i = 0; i < CACHE; i++)
        if (g->sheetPage[i] == page + 1) { g->sheetUse[i] = ++g->useClock; return &g->sheet[i].texture; }
    if (!load) return NULL;

    int slot = 0;
    for (int i = 1; i < CACHE; i++) if (g->sheetUse[i] < g->sheetUse[slot]) slot = i;
    if (!g->sheet[slot].id) {
        g->sheet[slot] = LoadRenderTexture(w, h);
        SetTextureFilter(g->sheet[slot].texture, TEXTURE_FILTER_BILINEAR);
    }
    float margin = w * 0.025f;
    Image img = LoadPage(&g->ch, page, w - 2 * margin, h - 2 * margin);
    Texture2D t = { 0 };
    if (img.data) {
        t = LoadTextureFromImage(img);
        SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
        UnloadImage(img);
    }
    BeginTextureMode(g->sheet[slot]);
    ClearBackground(PAPER);
    rlColorMask(true, true, true, false);  // blending would lower the sheet's alpha and the page would turn see-through
    if (t.id) {
        float s = fminf((w - 2 * margin) / t.width, (h - 2 * margin) / t.height);
        Rectangle dst = { (w - t.width * s) / 2, (h - t.height * s) / 2, t.width * s, t.height * s };
        DrawTexturePro(t, (Rectangle){ 0, 0, t.width, t.height }, dst, (Vector2){ 0 }, 0, WHITE);
    } else {
        text(g, "can't load this page", margin * 2, h / 2, 46, GRAY);
    }
    // the gutter: paper darkens towards the spine (left edge, right edge for RTL)
    float gw = w * 0.06f;
    if (g->rtl) DrawRectangleGradientH(w - gw, 0, gw, h, BLANK, Fade(BLACK, 0.16f));
    else DrawRectangleGradientH(0, 0, gw, h, Fade(BLACK, 0.16f), BLANK);
    rlDrawRenderBatchActive();
    rlColorMask(true, true, true, true);
    EndTextureMode();
    if (t.id) UnloadTexture(t);

    g->sheetPage[slot] = page + 1;
    g->sheetUse[slot] = ++g->useClock;
    return &g->sheet[slot].texture;
}

// the back of a page: blank paper with the print faintly showing through (the mesh mirrors it)
static Texture2D *backside(Game *g, int page)
{
    Texture2D *front = sheet(g, page, true);
    if (g->backPage == page + 1) return &g->back.texture;
    if (!g->back.id) {
        g->back = LoadRenderTexture(g->sheetW, g->sheetH);
        SetTextureFilter(g->back.texture, TEXTURE_FILTER_BILINEAR);
    }
    BeginTextureMode(g->back);
    ClearBackground((Color){ 238, 235, 228, 255 });
    rlColorMask(true, true, true, false);
    DrawTextureRec(*front, (Rectangle){ 0, 0, front->width, -front->height }, (Vector2){ 0 }, Fade(WHITE, 0.13f));
    rlDrawRenderBatchActive();
    rlColorMask(true, true, true, true);
    EndTextureMode();
    g->backPage = page + 1;
    return &g->back.texture;
}

static void drawflat(Texture2D *t)
{
    DrawTextureRec(*t, (Rectangle){ 0, 0, t->width, -t->height }, (Vector2){ 0 }, WHITE);
}

// ---------------------------------------------------------------------------------------------- the curl

typedef struct { Vector2 p; float z, shade; } CurlVert;
typedef struct { int a, b, c; float z; } Tri;

static int trizcmp(const void *x, const void *y)
{
    float d = ((const Tri *)x)->z - ((const Tri *)y)->z;
    return d < 0 ? -1 : d > 0;
}

// The curl must span a couple of mesh cells: a triangle with one corner flat and another already folded over
// is stretched across the fold and shows as a sawtooth.
static float radius(float w)
{
    float cell = fmaxf(w / GX, GetScreenHeight() / (float)GY);
    return fmaxf(w * 0.09f, 1.6f * cell);
}

// fold position where the turned page (curl included) is completely past the spine, off screen
static float turnedX(void) { return -1.3f * radius(GetScreenWidth()); }

// The page is bound at the spine (x = 0): the fold line may never cross it, or the paper would tear. So close to
// the spine the fold straightens up instead (no stalling: X itself always follows the finger).
static float spinetilt(float X, float tilt, float pivotY, float h)
{
    if (X <= 0) return 0;
    return clampf(tilt, -X / pivotY, X / (h - pivotY));
}

static Vector2 toscreen(Game *g, Vector2 p)
{
    return (Vector2){ g->rtl ? GetScreenWidth() - p.x : p.x, p.y };
}

static void drawtris(Game *g, Texture2D *t, CurlVert *v, Tri *tris, int n)
{
    // Textured triangles go in as quads with the last corner repeated, like raylib's own textured shapes: rlgl
    // can't start a textured RL_TRIANGLES draw (rlBegin resets the texture, rlSetTexture leaves the mode at quads).
    {
        rlSetTexture(t->id);
        rlBegin(RL_QUADS);
        for (int i = 0; i < n; i++) {
            int idx[4] = { tris[i].a, tris[i].b, tris[i].c, tris[i].c };
            for (int k = 0; k < 4; k++) {
                int j = idx[k];
                float u = (float)(j % (GX + 1)) / GX, vv = (float)(j / (GX + 1)) / GY;
                unsigned char s = (unsigned char)(255 * v[j].shade);
                Vector2 sp = toscreen(g, v[j].p);
                rlColor4ub(s, s, s, 255);
                rlTexCoord2f(g->rtl ? 1 - u : u, 1 - vv);  // render textures are stored upside down
                rlVertex3f(sp.x, sp.y, -0.5f);  // mid depth range (rlVertex2f can sit exactly on the far plane)
            }
        }
        rlEnd();
        rlSetTexture(0);
    }
}

static void drawcurl(Game *g, int page)
{
    float w = GetScreenWidth(), h = GetScreenHeight();
    Texture2D *back = backside(g, page);
    Texture2D *front = sheet(g, page, true);
    float R = radius(w);
    float tilt = spinetilt(g->X, g->tilt, g->pivotY, h);
    Vector2 n = Vector2Normalize((Vector2){ 1, tilt });  // towards the free edge
    Vector2 L0 = { g->X, g->pivotY };
    // nothing past the fold yet: flat (rtl sheets are already drawn the right way round)
    if (Vector2DotProduct(Vector2Subtract((Vector2){ w, 0 }, L0), n) <= 0 &&
        Vector2DotProduct(Vector2Subtract((Vector2){ w, h }, L0), n) <= 0) { drawflat(front); return; }

    // raylib culls back faces, and folded-over paper is mirrored (and RTL mirrors everything): the GPU would drop
    // half the triangles. Culling is applied when the batch is drawn, so flush around the change.
    // The mesh (~15k vertices) also doesn't fit raylib's GLES2 batch (8k): it gets its own, so nothing flushes
    // halfway through a page.
    rlDrawRenderBatchActive();
    if (!g->curlBatch.vertexBuffer) g->curlBatch = rlLoadRenderBatch(1, 8192);
    rlSetRenderBatchActive(&g->curlBatch);
    rlDisableBackfaceCulling();

    // the shadow the curl throws on the page underneath
    {
        Vector2 t = { -n.y, n.x };
        float strength = clampf((w - g->X) / (w * 0.3f), 0, 1) * clampf((g->X - turnedX()) / (w * 0.2f), 0, 1) * 0.45f;
        Vector2 a = Vector2Add(L0, Vector2Scale(n, R * 0.7f)), b = Vector2Add(a, Vector2Scale(n, w * 0.16f));
        Vector2 q[4] = { Vector2Add(a, Vector2Scale(t, 4 * h)), Vector2Subtract(a, Vector2Scale(t, 4 * h)),
                         Vector2Subtract(b, Vector2Scale(t, 4 * h)), Vector2Add(b, Vector2Scale(t, 4 * h)) };
        unsigned char al[4] = { 255 * strength, 255 * strength, 0, 0 };
        rlBegin(RL_QUADS);
        int order[4] = { 0, 1, 2, 3 };
        for (int k = 0; k < 4; k++) {
            Vector2 s = toscreen(g, q[order[k]]);
            rlColor4ub(0, 0, 0, al[order[k]]);
            rlVertex3f(s.x, s.y, -0.5f);
        }
        rlEnd();
    }

    static CurlVert v[(GX + 1) * (GY + 1)];
    static Tri fronts[GX * GY * 2], backs[GX * GY * 2];
    for (int y = 0; y <= GY; y++)
        for (int x = 0; x <= GX; x++) {
            Vector2 p = { w * x / GX, h * y / GY };
            float dd = Vector2DotProduct(Vector2Subtract(p, L0), n);
            CurlVert *cv = &v[y * (GX + 1) + x];
            if (dd <= 0) {
                // the flat paper darkens a little where it starts to lift
                // (a soft band just before the fold, back to full brightness at the fold to match the cylinder)
                float k = sinf(PI * clampf(-dd / (1.5f * R), 0, 1));
                *cv = (CurlVert){ p, 0, 1 - 0.12f * k };
            } else if (dd < PI * R) {
                float a = dd / R;
                cv->p = Vector2Add(p, Vector2Scale(n, R * sinf(a) - dd));
                cv->z = R * (1 - cosf(a));
                cv->shade = 0.62f + 0.38f * fabsf(cosf(a));  // darkest where the paper is edge-on
            } else {
                cv->p = Vector2Subtract(p, Vector2Scale(n, dd - PI * R + dd));
                cv->z = 2 * R;
                cv->shade = 1;
            }
        }
    int nf = 0, nb = 0;
    for (int y = 0; y < GY; y++)
        for (int x = 0; x < GX; x++) {
            int i0 = y * (GX + 1) + x, i1 = i0 + 1, i2 = i0 + GX + 1, i3 = i2 + 1;
            int tri[2][3] = { { i0, i2, i1 }, { i1, i2, i3 } };
            for (int k = 0; k < 2; k++) {
                Vector2 a = v[tri[k][0]].p, b = v[tri[k][1]].p, c = v[tri[k][2]].p;
                float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);  // < 0 when not mirrored
                float z = (v[tri[k][0]].z + v[tri[k][1]].z + v[tri[k][2]].z) / 3;
                Tri t = { tri[k][0], tri[k][1], tri[k][2], z };
                if (area <= 0) fronts[nf++] = t; else backs[nb++] = t;
            }
        }
    // front-facing parts are all lower than the face-down ones (z < R vs z > R), so two sorted passes are enough
    qsort(fronts, nf, sizeof *fronts, trizcmp);
    qsort(backs, nb, sizeof *backs, trizcmp);
    drawtris(g, front, v, fronts, nf);
    drawtris(g, back, v, backs, nb);
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    rlSetRenderBatchActive(NULL);
}

// ---------------------------------------------------------------------------------------------- reader screen

static void startturn(Game *g, int dir, float grabY)
{
    float w = GetScreenWidth(), h = GetScreenHeight();
    if (dir > 0 && g->page >= g->ch.n - 1) return;
    if (dir < 0 && g->page <= 0) return;
    g->turn = dir;
    g->pivotY = clampf(grabY, h * 0.05f, h * 0.95f);
    g->grabTilt = (g->pivotY / h - 0.5f) * 0.6f;  // grab low: the bottom leads, grab high: the top leads
    g->X = g->startX = dir > 0 ? w : turnedX();
    g->tilt = g->grabTilt;
}

static void endturn(Game *g)
{
    if (g->finishing == (g->turn > 0)) g->page += g->turn;  // forward: finished; backward: back to flat = done
    g->turn = 0;
}

#define TURN_SECONDS 0.55f  // a full turn from flat; shorter distances take proportionally less (but >= 0.3s)

static void animate(Game *g, float lift)
{
    float w = GetScreenWidth();
    g->fromX = g->X;
    g->toX = g->finishing ? turnedX() : w;
    g->fromTilt = spinetilt(g->X, g->tilt, g->pivotY, GetScreenHeight());
    g->animT = 0;
    g->animDur = fmaxf(0.25f, TURN_SECONDS * fabsf(g->toX - g->fromX) / (w - turnedX()));
    g->animLift = lift;
}

static void reader(Game *g, float dt)
{
    float w = GetScreenWidth(), h = GetScreenHeight();
    Vector2 m = GetMousePosition();
    Vector2 lm = { g->rtl ? w - m.x : m.x, m.y };  // logical: pages always turn towards -x

    if (IsKeyPressed(KEY_BACK)) {
        if (g->overlay) g->overlay = false;
        else { g->screen = LIBRARY; return; }
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g->down = true, g->moved = false;
        g->pressPos = g->lastPos = lm;
        g->pressTime = GetTime();
        g->vx = 0;
        if (g->turn) {  // a page still in the air: catch it and keep dragging from where it is
            g->dragging = true, g->moved = true;
            g->startX = g->X;
            g->grabTilt = g->tilt;
        }
    }
    if (g->down && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (dt > 0) g->vx = g->vx * 0.6f + 0.4f * (lm.x - g->lastPos.x) / dt;
        float dx = lm.x - g->pressPos.x;
        if (!g->moved && fabsf(dx) > 14) {  // only sideways movement picks the direction
            g->moved = true;
            g->overlay = false;
            startturn(g, dx < 0 ? 1 : -1, g->pressPos.y);
            g->dragging = g->turn != 0;
        }
        // hold still for half a second: the menu bar
        if (!g->moved && !g->overlay && Vector2Distance(lm, g->pressPos) < 20 && GetTime() - g->pressTime > 0.5) {
            g->overlay = true;
            g->moved = true;  // so letting go isn't also a tap
        }
        if (g->dragging) {
            // the fold follows the finger 1:1 across; moving up/down leans it (pulling up from low = bottom leads)
            g->X = clampf(g->startX + (lm.x - g->pressPos.x), turnedX(), w);
            g->tilt = clampf(g->grabTilt - 1.5f * (lm.y - g->pressPos.y) / h, -0.6f, 0.6f);
        }
        g->lastPos = lm;
    }
    if (g->down && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        g->down = false;
        if (g->dragging) {
            g->dragging = false;
            // a flick decides; otherwise where the edge is (past the middle = turned)
            g->finishing = g->vx < -300 || (g->vx < 300 && g->X < w * 0.5f);
            animate(g, 0);
        } else if (!g->moved && GetTime() - g->pressTime < 0.4) {
            bool handled = false;
            if (g->overlay && m.y < BAR) {
                if (m.x < 300) { g->screen = LIBRARY; return; }
                if (m.x > w - 300) { g->rtl = !g->rtl; clearsheets(g); }
                handled = true;
            }
            if (!handled && g->overlay) g->overlay = false;  // a tap with the menu open just closes it
            else if (!handled) {
                // left 35%: back, the rest: forward (both sides swap for RTL, lm is logical)
                if (lm.x > w * 0.35f) { startturn(g, 1, h * 0.6f); g->finishing = true; animate(g, 0.35f); }
                else { startturn(g, -1, h * 0.6f); g->finishing = false; animate(g, 0.35f); }
            }
        }
    }
    if (g->turn && !g->dragging) {
        // ease out (cubic): quick off the finger, settling gently; taps also arc the corner up and back down
        g->animT += dt;
        float t = fminf(1, g->animT / g->animDur), e = 1 - powf(1 - t, 3);
        g->X = Lerp(g->fromX, g->toX, e);
        g->tilt = Lerp(g->fromTilt, 0, e) + g->animLift * sinf(PI * e);
        if (t >= 1) endturn(g);
    }

    // decode ahead while nothing moves (one page per frame)
    if (!g->turn) {
        int want[4] = { g->page, g->page + 1, g->page - 1, g->page + 2 };
        for (int i = 0; i < 4; i++)
            if (want[i] >= 0 && want[i] < g->ch.n && !sheet(g, want[i], false)) { sheet(g, want[i], true); break; }
    }
    // the turn needs both pages ready before drawing (loading inside BeginDrawing would switch framebuffers)
    int under = g->turn > 0 ? g->page + 1 : g->page, turning = g->turn > 0 ? g->page : g->page - 1;
    sheet(g, under, true);
    if (g->turn) { sheet(g, turning, true); backside(g, turning); }

    BeginDrawing();
    ClearBackground(BG);
    drawflat(sheet(g, under, true));
    if (g->turn) drawcurl(g, turning);
    if (g->overlay) {
        DrawRectangle(0, 0, w, BAR, Fade(BG, 0.92f));
        text(g, "‹ Library", 36, 50, 50, ACCENT);
        text(g, g->rtl ? "RTL" : "LTR", w - 140, 50, 50, ACCENT);
        float by = h - g->navH - 160;
        DrawRectangle(0, by, w, 160 + g->navH, Fade(BG, 0.92f));
        char info[320];
        snprintf(info, sizeof info, "%.40s", g->title);
        text(g, info, 36, by + 30, 44, RAYWHITE);
        snprintf(info, sizeof info, "%d / %d", g->page + 1, g->ch.n);
        text(g, info, 36, by + 86, 40, GRAY);
    }
    EndDrawing();
}

// ---------------------------------------------------------------------------------------------- host contract

static void init(void *state)
{
    Game *g = state;
    SetTargetFPS(60);
    // the arrows and ellipsis in the UI need more than ASCII
    int cps[300], n = 0;
    for (int c = 32; c < 127; c++) cps[n++] = c;
    for (int c = 160; c < 256; c++) cps[n++] = c;     // Latin-1
    for (int c = 0x2010; c < 0x2028; c++) cps[n++] = c;  // dashes, curly quotes
    cps[n++] = 0x2039;  // ‹
    g->font = GetFontDefault();
    FILE *f = fopen("/system/fonts/SourceSansPro-Regular.ttf", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *data = malloc(len);
        if (fread(data, 1, len, f) == (size_t)len) {
            g->font = LoadFontFromMemory(".ttf", data, len, 56, cps, n);
            SetTextureFilter(g->font.texture, TEXTURE_FILTER_BILINEAR);
        }
        free(data);
        fclose(f);
    }
    g->navH = NavBarHeight();
    loadroot(g);
    if (g->root[0]) opendirectory(g, g->root);
    else startpicking(g);
}

// testing without a finger: /sdcard/Download/flip_debug.txt = "<chapter path>\n<page> <X> <tilt> <pivotY>" opens that
// chapter with a page frozen mid-turn (delete the file to get the normal app back)
static void debugcurl(Game *g)
{
    FILE *f = fopen("/sdcard/Download/flip_debug.txt", "r");
    if (!f) return;
    char path[PATHLEN];
    int page;
    float x, tilt, py;
    if (fgets(path, sizeof path, f) && fscanf(f, "%d %f %f %f", &page, &x, &tilt, &py) == 4) {
        path[strcspn(path, "\n")] = 0;
        if (g->screen != READER || strcmp(g->ch.path, path) != 0) openchapter(g, path, "debug");
        g->page = page;
        g->turn = 1;
        g->dragging = true;
        g->X = x, g->tilt = tilt, g->pivotY = py;
    }
    fclose(f);
}

static void frame(void *state)
{
    Game *g = state;
    float dt = fminf(GetFrameTime(), 1 / 20.0f);
    debugcurl(g);
    if (g->screen == READER) reader(g, dt);
    else library(g, dt);
}

static void reload(void *state, bool assetsChanged) {}

static void unload(void *state)
{
    Game *g = state;
    for (int i = 0; i < CACHE; i++) if (g->sheet[i].id) UnloadRenderTexture(g->sheet[i]);
    if (g->back.id) UnloadRenderTexture(g->back);
    if (g->curlBatch.vertexBuffer) rlUnloadRenderBatch(g->curlBatch);
    UnloadFont(g->font);
}

GAME_API(sizeof(Game), init, frame, reload, unload);
