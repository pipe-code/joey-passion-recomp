# joey-passion-recomp

A native port of **Yu-Gi-Oh! Power of Chaos: Joey the Passion** (Konami, PC 2004) to **macOS, Windows, Android
and PS Vita**, made by **static recompilation**. The original Win32 x86 executable (`Joey_Pc.exe`) is translated
to C, compiled natively for each platform and run on a small reimplementation of the parts of Windows and
DirectX the game uses.

It is fully playable: graphics, text, mouse/touch/gamepad, music, voices, LAN duels, and Konami's original rules
and duel AI, unchanged. On the PS Vita it runs at 25–30 fps (444 MHz CPU, rendering on a second core).

> **You need your own copy of the game.** This repository contains no Konami files or code: no executable, no
> `.dat` archives, no music, and none of the C generated from the executable. That code is Konami's code in
> translated form, so it is generated locally in `build/`. For the same reason, no prebuilt binaries are
> published: every build contains the translated game code, so build it from your own copy.
>
> This is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Konami.
> Yu-Gi-Oh! and Power of Chaos are trademarks of their respective owners.

## How it works

```
Joey_Pc.exe ──► tools/lifter/funcs.py ──► functions (8,651, 96 % of .text)
           └──► tools/lifter/lift.py  ──► build/all/lifted_*.c  (one C function per x86 function)
                                               │
runtime/  (virtual CPU, guest memory, dispatch) ┤
host/     (KERNEL32, USER32, GDI32, DirectDraw, ├──► native binary
           DirectInput, DirectSound, threads…)  ┘
```

- **x86→C translator**: covers the whole binary (integer ALU and flags, x87, MMX, string operations…). Registers
  and flags become C locals, and game memory is a flat 32-bit space.
- **Verification**: `make difftest` runs each function in Unicorn (original x86) and in its translated form
  from the same state, then compares registers and memory. 3,572 functions match with 0 mismatches. The rest call
  into Windows and are validated by playing.
- **Host layer**: implements the ~190 Windows functions the game imports and the DirectX 1–3 interfaces it uses
  (IDirectDraw, IDirectInputA, IDirectSound, DirectPlay 8) on top of SDL2 / SDL2_ttf.

Technical details, file formats and the development log are in [`NOTES.md`](NOTES.md).

## Getting the game files

Extract the game from your installer or CD (the installer is usually Inno Setup):

```sh
innoextract -e -d /tmp/joey "path/to/installer.exe"
mkdir -p game && mv /tmp/joey/app/* game/          # Joey_Pc.exe, Data.dat, Voice.dat, Voice/, ...
# If your Voice.dat has no music (stream\m_*.wav), extract it from one that does:
.venv/bin/python -I tools/extract_music.py "path/to/Voice.dat-with-music" game/Voice/Joey/Spanish/Stream
```

`game/`, `build/` and `work/` are git-ignored. Never commit anything from them.

## Building and playing on macOS

Requirements: Xcode Command Line Tools, Homebrew and Python 3.

```sh
brew install sdl2 sdl2_ttf innoextract
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
make lift          # analyze and translate the exe (~40 s); required once for every platform
make -j10 host     # compile (~15 s on 10 cores)
make run
```

Useful options:
- `build/all/joey -v game`: verbose API call log.
- `kill -USR1 <pid>`: dumps the screen to `$JOEY_DUMP` (BMP).
- `JOEY_AUDIO_DUMP=f.raw`: records the audio mix.
- `JOEY_LCID`: changes the reported language.
- `JOEY_FONT` / `JOEY_FONT_JP`, or `game/fonts/{regular,jp}.ttf`: change the fonts.

### macOS app (`Joey.app`)

```sh
make app APPFLAGS="--data build/Data_anime.dat --icon path/to/icon.ico"   # -> build/Joey.app
```

This builds a self-contained app (SDL libraries bundled, ad-hoc signed) with your game files inside. Options:
- `--data`: use another `Data.dat`, e.g. the card art addon below.
- `--icon`: use another icon. Default: the executable's icon.
- `--save`: use another starting save. Default: the one in `game/`.

The game runs from `~/Library/Application Support/Joey the Passion/`. The save lives there (`registry.txt`,
`deck.ydc`, `Yu-Gi-Oh! Power of Chaos Common/`), as does `log.txt`. The starting save is only copied if none
exists. The app contains Konami data, so it is for your own use only.

## Building for Windows (`Joey.exe`)

Cross-compiled from macOS with mingw-w64 (x86-64, Windows 10/11):

```sh
brew install mingw-w64
mkdir -p work/winlibs && cd work/winlibs
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz
curl -LO https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-devel-2.22.0-mingw.tar.gz
tar xzf SDL2-devel-2.30.9-mingw.tar.gz && tar xzf SDL2_ttf-devel-2.22.0-mingw.tar.gz && cd ../..
make -j10 win WICON=path/to/icon.ico                                  # -> build/win/Joey.exe
.venv/bin/python -I tools/make_winpkg.py --data build/Data_anime.dat   # -> build/Joey the Passion/ (+ .zip)
```

The folder holds:
- `Joey.exe`
- the SDL DLLs
- the font
- `game/`, with your game files and the starting save

Put it somewhere writable, such as Documents rather than Program Files. The game saves to `game\` and writes
`game\log.txt`. The `.exe` is unsigned, so SmartScreen warns on first launch ("More info" → "Run anyway"). The
firewall asks for local network access, which LAN duels need. The Windows build is new and lightly tested.

## Building for Android (`joey.apk`)

ARM64, Android 7 or later. Requirements:
- Android SDK with NDK 26.1, CMake 3.22.1 and `platforms;android-34`. Set `ANDROID_HOME`; the default is
  `~/Library/Android/sdk`.
- JDK 17.

```sh
tools/android_deps.sh                         # once: SDL2 + SDL2_ttf for arm64 in work/android
make -j10 android AICON=path/to/icon.ico      # -> build/android/joey.apk
adb install -r build/android/joey.apk
```

The APK contains no game files. On first launch it asks you to pick the game folder and copies it to
`Android/data/io.github.joeyport/files/game/`. That folder must contain:
- `Joey_Pc.exe`, `Data.dat`, `Voice.dat` and `Voice/`
- optionally, to keep your save: `Region.dat`, `registry.txt`, `deck.ydc` and `Yu-Gi-Oh! Power of Chaos Common/`

You can also copy it there over USB or with `adb push`. The log is written to that folder (`log.txt`).

## Building and installing on PS Vita

Requirements: [VitaSDK](https://vitasdk.org) in `~/vitasdk` (or set `VITASDK=...`), and a Vita with
HENkaku/Enso and VitaShell.

```sh
make -j10 vita         # -> build/vita/joey.vpk (and build/vita/eboot.bin)
```

1. Install `build/vita/joey.vpk` with VitaShell.
2. Copy the game files to `ux0:data/joey/`:
   - `Joey_Pc.exe`, `Data.dat`, `Voice.dat` and the `Voice/` folder.
   - `Region.dat`, which selects the language. Without it the game starts in English.
   - Your save, if you have one: `registry.txt`, `deck.ydc` and the `Yu-Gi-Oh! Power of Chaos Common/` folder.
3. The log is written to `ux0:data/joey/log.txt`. Every few seconds it records fps, memory and a function profile.

To test changes quickly, replace `ux0:app/JOEY00001/eboot.bin` over FTP instead of reinstalling.

## Controls

On macOS and Windows you play with mouse and keyboard as on the original PC game. A connected gamepad moves the
cursor and clicks.

| PS Vita / gamepad | Action |
|---|---|
| Left stick or D-pad | move the cursor |
| Touch screen | move the cursor / click |
| Cross | left click |
| Circle | right click |
| Right stick | scroll (mouse wheel): side lists and long descriptions. Tilt further to scroll faster |
| (automatic) | the on-screen keyboard opens with the current text whenever a text field appears (LAN name, deck export, replay title). Confirming replaces the field's text |
| Triangle | reopen the keyboard |
| START | Esc (menu / cancel) |

| Android | Action |
|---|---|
| Tap | click. The cursor moves to the point first so the game registers the hover |
| Hold and slide | move the cursor without clicking, e.g. to read card descriptions. Releasing after a slide does not click |
| **Menu** button (left bar) / Android Back | Esc (menu / cancel) |
| **Keyboard** button | reopen the keyboard. It lights up while a text field is active; text fields open an Android text dialog on their own |
| **Right click** button | right click at the cursor |
| Scroll bar (right bar) | tap ▲/▼ = one wheel notch, hold = repeat, slide = continuous scroll |
| Bluetooth gamepad | same as on the Vita |

The buttons sit in the bars beside the 4:3 game image. On near-4:3 screens (tablets) the game shrinks slightly to
make room. On small screens, where the game image is under 3 inches tall (phones), small text is drawn 1 px larger,
as on the Vita. Button labels follow the device language (English or Spanish).

## Cross-platform LAN duels

"Duel mode → LAN duel" works between any copies of this port on the same Wi-Fi network: macOS, Windows, PS Vita
and Android. It does not work with the original Windows game, because the network protocol is the port's own.
1. One player picks Host and the other picks Guest.
2. The host shows up in the guest's list on its own.

It uses UDP and TCP on port 2573. The Windows and macOS firewalls ask for permission the first time. Tested:
macOS↔macOS, macOS↔Vita and Android (host)↔macOS.

## Changes from the original game

- **Cross-game Power of Chaos features** (`host/hle_game.c`). The "All cards" list and the "Complete Power of
  Chaos card game" option (Options → Set deck) normally require having played Yugi or Kaiba. Here they are always
  available, and post-duel reward cards come from the whole trilogy's collection (771 cards), not only Joey's. No
  cards are granted and the save is not modified.
- **PS Vita and Android phones**: small text (card descriptions) is drawn 1 px larger so it is readable on a small
  screen.

## Extra tools

**Card art addon** (for example the "AIO anime style cards" pack): writes a copy of `Data.dat` with the
addon's `card/*.bmp` and `mini/*.bmp` images. The original is not modified.

```sh
.venv/bin/python -I tools/apply_card_addon.py game/Data.dat path/to/addon build/Data_anime.dat
```

Then use `build/Data_anime.dat` as `Data.dat`. On the Vita, keep the original under another name so you can
switch back.

**Custom PS Vita LiveArea** (bubble, background and startup image): builds the art from your own files into
`build/livearea/`. If it exists, `make vita` packages it instead of the repo's generic art.

```sh
.venv/bin/python -I tools/make_livearea.py game/Joey_Pc.exe background.jpg [--icon icon.ico]
make vita
```

Without `--icon`, it uses the executable's 32×32 icon, upscaled as pixel art. The LiveArea only changes when you
reinstall the VPK.

## Layout

| Path | Contents |
|---|---|
| `tools/lifter/` | function discovery, the x86→C translator, and the list of functions replaced by native code (`hle.txt`) |
| `tools/` | `Data.dat`/`Voice.dat` extractors (KCEJYUGI + LZSS), `.yga` sprites, card database, differential tests, packaging |
| `runtime/` | guest CPU and memory, call dispatch, MMX, native CRT implementations |
| `host/` | Windows and DirectX on SDL2. `hle_game.c` holds game logic changes, `touchui.c` the Android touch UI, `prof.c` a profiler |
| `android/` | Gradle project: game-folder import screen and SDL activity |
| `vita/` | generic LiveArea art (no Konami content) |
| `tests/` | translated LZSS test and the differential-test library |

## Known limitations

- The window can't be resized and there is no native fullscreen or Retina rendering on macOS.
- On the Vita, MMX blitters are not yet converted to NEON, so some animations briefly drop frames.

## License

The source code in this repository is released under the [MIT License](LICENSE). The license does not cover the
game itself or any code generated from its executable. The bundled Arimo font is under the SIL Open Font License
(`assets/fonts/OFL-Arimo.txt`).
