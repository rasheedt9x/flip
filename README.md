# flip

A page-curl demo for Android: drag a page and it bends like real paper, with the print faintly showing through
the back. Point it at a folder of `.cbz` files or images and flip through them.

- Opens `.cbz` files and folders of images (webp, jpg, png, gif, heic, avif).
- Pick a folder once; flip remembers it.
- Left-to-right or right-to-left page order.
- Written in C with [raylib](https://www.raylib.com). No Java, no Gradle: built entirely on the phone in Termux with
  [raylib-android](https://github.com/rasheedt9x/raylib-android).

## Install
Download the APK from Releases and open it. On first start flip asks for "All files access" (needed to see .cbz
files), then for a folder.

## Use
| | |
|---|---|
| Drag a page sideways | turn it; let go past the middle (or flick) to finish |
| Tap the right side / left side | next / previous page |
| Hold still for half a second | menu: back to the folder list, LTR/RTL, page number |
| Back | close the menu, back to the folder list, up a folder |
| "Folder" (top right) | pick a different folder |

## Build (Termux, aarch64)
```sh
git clone https://github.com/rasheedt9x/raylib-android
git clone https://github.com/rasheedt9x/flip
cd flip && ../raylib-android/kit.sh build .    # -> flip.apk
```
See [NOTES.md](NOTES.md) for how the page curl works.

## Limitations
Very tall images (webtoon strips) are shrunk to fit one screen. It doesn't remember your place yet.
