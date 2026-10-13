# flip: notes

Working notes for whoever picks this up next (Claude or the user). flip is a page-curl demo: open a .cbz or a
folder of images and turn the pages like paper under your finger. Built 2026-10-13 in one session with the
raylib-android kit (`../raylib-android`, read its NOTES.md too). It began as a "can this even be done" test.

## Build, install, run
```sh
cd ~/dev/c/flip
../raylib-android/kit.sh build . install   # release APK, installed through Shizuku (rish)
~/rish/rish -c 'am start -S -n local.flip/android.app.NativeActivity'
```
- `./dev.sh` (hot reload) works too, but the dev APK replaces the release one (same package).
- game.conf: `LIBS="jnigraphics"` (AImageDecoder), `PERMS="MANAGE_EXTERNAL_STORAGE"`. Both are kit options added for
  flip.
- Granting "All files access" without the settings screen: `rish -c 'appops set local.flip MANAGE_EXTERNAL_STORAGE allow'`.

## Files
- `src/book.c`: folder listing (natural sort, skips `*_tmp` dirs, which download apps leave behind for unfinished downloads), .cbz reading (own zip central-directory
  parser; stored entries copied, deflate through raylib's `DecompressData`, which is raw deflate), page decoding with
  Android's `AImageDecoder` (webp/jpg/png/gif/heic/avif, decoded straight to the screen size), and three JNI calls:
  open the "All files access" settings page, `Environment.isExternalStorageManager()`, nav bar height.
  Uses plain stdio on purpose: raylib's `LoadFileData` goes through `android_fopen`, which prefixes absolute paths
  with the app's data dir.
- `src/game.c`: library screen (folder picker), reader, page sheets, the curl, input, and the debug hook.
- The library folder is saved in `<internalDataPath>/root.txt`. No saved folder: the picker opens at /sdcard.

## Page sheets
Each page is drawn once onto a screen-sized RenderTexture of paper colour (image letterboxed, a light gutter
gradient on the spine side). LRU cache of 6; the reader decodes one page per frame ahead while nothing moves
(page, +1, -1, +2). The back of the page being turned is a second RenderTexture: paper with the front at 13% alpha.
The curl mesh then only needs one texture per side, with uv 0..1 (v flipped: render textures are upside down).

## The curl (drawcurl)
The page being turned is a grid mesh (40x80 cells). Everything is in logical coordinates: spine at x = 0, pages
turn towards -x; RTL only mirrors x on the way to the screen (and the uv u).
- The fold is a line: it crosses height `pivotY` at `x = X`, leaning by `tilt` (dx per dy, > 0 = the bottom of the
  page is further over). `n` = unit normal towards the free edge, `dd` = signed distance of a point past the fold.
- `dd <= 0`: flat, unchanged (shaded with a soft darker band just before the fold, none at the fold itself so it
  meets the cylinder without a seam).
- `0 < dd < πR`: wrapped round a cylinder of radius R lying on the page: `p + n(R sin a - dd)`, `a = dd / R`,
  height `R(1 - cos a)`. Shade `0.62 + 0.38|cos a|` (darkest edge-on).
- Shading is per pixel (`CURL_FS` in game.c): the fragment shader recomputes dd from the texture coordinate (the
  spot on the flat page, exact everywhere) and applies the band / cylinder shade. Per-vertex colours showed the mesh
  triangles as faint facets (the shade isn't linear across the bent mesh).
- `dd >= πR`: past the top of the cylinder, lying flat face down: `p - n(2dd - πR)`.
- Triangles whose screen winding flipped show the back of the paper. Front-facing parts are always lower
  (z < R) than face-down ones (z > R), so: sort each set by z, draw fronts, then backs. No depth buffer.
- Shadow on the page underneath: a contact shadow from `L0 + 0.85R n` to `L0 + 1.7R n` (just past the roll's outer
  edge), clipped to the part of the fold line inside the page (where the roll actually is), strength 0.4 scaled by
  how far the page reaches past the fold (`lift / πR`). Earlier versions drew it along the whole line and 16% of the
  width out: a shadow with no paper casting it (at the first touch, and after the roll left the page).
  The user still finds it not quite right (2026-10-13): a real shadow would follow the lifted paper's silhouette
  (e.g. render the lifted part darkened and offset, or soften the ends of the band).
- R = 9% of the width, but never below 1.6 mesh cells (see dead ends).
- The spine: the fold may never cross x = 0 (that would tear the page off the binding). `spinetilt()` clamps the
  lean so both spine corners stay on the flat side; at X <= 0 the fold is vertical. X itself is never clamped by
  this, so the paper never stops following the finger.
- `turnedX() = -1.3R`: X where even the cylinder is off screen. Flat = X >= width with no part past the fold.

## Input model (reader)
- Drag: direction is picked by the first 14 px of *sideways* movement (left = forward). The fold then follows the
  finger 1:1: `X = startX + dx`. Lean = grab height (`(pivotY/h - 0.5) * 0.6`: low grab = bottom leads) minus
  1.5 * dy/h (pulling up from low leans more), clamped to ±0.6, then the spine clamp.
- Release: a flick (|vx| > 300 px/s) decides; otherwise X past the middle turns the page.
- Animation: cubic ease-out from X to the end (`TURN_SECONDS` 0.55 for a full turn, shorter for less, min 0.25 s),
  the lean eases to 0. Taps add an arc to the lean (`animLift` 0.35 * sin) so the page goes over corner-first.
- Touching a page that's still animating catches it: dragging continues from its current X and lean.
- Taps: left 35% back, the rest forward. Long press (0.5 s, still): menu bar (Library, LTR/RTL, page n/N). Back key:
  close the menu, then back to the library.
- The window is edge-to-edge (targetSdk 35 forces it): bottom UI sits above `NavBarHeight()`.

## Testing without a finger (and without taking over the user's phone for long)
- Debug hook: `/sdcard/Download/flip_debug.txt` = `<chapter path>\n<page> <X> <tilt> <pivotY>` makes the app open
  that chapter with the page frozen mid-turn (re-read every frame, so editing the file moves the fold live).
  Then `rish -c 'screencap -p /sdcard/Download/x.png'` and Read the PNG. Delete the file afterwards.
- The user is often using the phone: start the app, screenshot, and switch back to Termux in one rish command
  (`am start -n com.termux/.app.TermuxActivity`). Don't inject `input` gestures while they're using it.
- Motion: `screenrecord --time-limit 4` + `input swipe`, then ffmpeg `fps=10,tile=10x3` into one image.

## Dead ends and bugs found the hard way (don't redo these)
1. **Textured RL_TRIANGLES in rlgl**: `rlBegin(RL_TRIANGLES)` after a quads draw resets the texture to white (the
   page went blank while held). Calling `rlSetTexture` after `rlBegin` starts a new draw whose mode stays RL_QUADS,
   so triangles get indexed as quads: a checkerboard of missing triangles. Fix: quads with the last vertex
   repeated (what raylib's own textured shapes do).
2. **Back-face culling** is on by default in raylib; folded paper is mirrored. Flush + `rlDisableBackfaceCulling()`.
3. **GLES2 batch is 8192 vertices**; the mesh is ~26k. Own batch: `rlLoadRenderBatch(1, 8192)` (quads) +
   `rlSetRenderBatchActive`. Vertices at z = -0.5 explicitly (after a flush `rlVertex2f` sits on the far plane).
4. **See-through flap**: blending into a RenderTexture lowers its alpha. `rlColorMask(1,1,1,0)` while drawing onto
   sheets, flush before restoring.
5. **Corner-under-finger model** (fold = perpendicular bisector of corner and finger, pushed back when it crossed
   the spine): physically "right" but the paper stalled near the spine while the finger kept going. The user called
   it stuck and finicky. Replaced by fold-follows-finger + lean clamp.
6. **Tightening R near the spine** (to make the page hinge): R went down to 2 px while cells are ~30 px, so
   triangles straddled the whole fold and drew a sawtooth. R has a floor of 1.6 cells now.
7. **Double-speed drag** (the corner moving 2x the finger): the page never felt attached to the finger.
8. **Ignoring touches during the animation**: every quick second swipe did nothing (the user's "fails 1 in 5").

## Not done
- The shadow (see above): better than before, not fully convincing yet.
- Remember the last page per chapter; continue into the next chapter at the end.
- Webtoon strips: long pages are scaled to fit one screen (tiny). They'd need splitting into screen-sized slices.
- Two-page spreads in landscape.
- Nav bar height comes from the `navigation_bar_height` resource, not real window insets (fine for 3-button and
  gesture nav; not exact on every device).
