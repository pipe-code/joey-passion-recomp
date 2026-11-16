# Joey the Passion: technical notes

## Files (`game/`)
- `Joey_Pc.exe` — PE32 x86, MSVC, compiled 2004-05-21, no DRM. "Version 1.00.000".
- `Data.dat` — KCEJYUGI archive (3592 entries). See `tools/kcej.py`, `tools/lzss.py`.
- `Voice.dat` — KCEJYUGI with 125 WAVs (Spanish voices).
- `system_allcards.dat` — save with all the cards (from the "All Cards" installer; goes in as `system.dat`).

## KCEJYUGI format
- `"KCEJYUGI"` + u32 count, then `count` entries of 0x10C bytes:
  name[0x100] (each byte with its nibbles swapped), u32 offset, u32 real size, u32 stored size.
- If stored != real → Okumura LZSS (N=4096, F=18, threshold 2, window filled with **0x00**, r=N-F).
- `python -I tools/kcej.py game/Data.dat unpack work/assets/raw` decompresses everything (205 compressed, sizes verified).

## Assets (all decoded)
- `.bmp` regular 24bpp BMP; native resolution 800×600.
- `.yga` → `tools/yga.py`: "yga\0", w, h, fmt=1, w*h*4, payload; LZSS → BGRA top-down, straight alpha. 537/537 OK.
- Card database → `tools/carddb.py` → `work/assets/cards.json` (1115 slots) and `dialog.json` (4315 texts × 6 languages).
  CARD_Prop: bits 0-8 DEF/10, 9-17 ATK/10, 18-19 subtype, 20-24 type (21 trap, 22 magic), 25-28 level, 29-31 attribute.
- `.txt` per-screen asset lists: `file<TAB>XXXYYY` (position in 800×600), `?` = language letter.
- `.ydc` decks: u32 version, float32 (AI rating?), 3 lists of u16 count + u16 card_id (main, fusion/side).
- `.sue` catalogs from one of the dev's image viewers, not used by the game.

## Executable (tools/x86scan.py, tools/x86detail.py → work/exe/)
- .text 1.94 MB; recursive descent covers 96.4% → 8,792 functions, 594,842 instructions, 193 distinct mnemonics.
- 559 jump tables resolved (4,810 targets); 39 unresolved indirect jumps (mostly CRT memcpy and COM thunks).
- 5,410 indirect calls (C++ vtables / COM).
- x87: 192 instructions in 59 functions. MMX: 35 functions clustered in 0x49B000–0x4A5000 (blitters). SSE: none.
- 2,105 `fs:` accesses → CRT/C++ SEH frames (linked list at fs:[0]).
- Threads: a single CreateThread site (0x5c5caa, the CRT's `_beginthreadex` wrapper).
- 203 imported Win32 functions (KERNEL32, USER32, GDI32, WINMM, MSACM32, ole32, OLEAUT32, ADVAPI32, comdlg32).
- COM via CoCreateInstance:
  - DirectDraw (IDirectDraw) — software framebuffer presentation, 800×600.
  - DirectInput (IDirectInputA), also a dynamic DirectInputCreateA from dinput.dll.
  - DirectSound (IDirectSound).
  - DirectPlay8 Peer + Address — LAN duel (can be stubbed).
  - DirectShow AMMultiMediaStream + DirectDraw media stream — video (probably "test.avi"/unused).
- OleLoadPicture → loads JPEG/BMP images (replace with stb_image).

## x86→C translator — status 2026-11-12 (step 1 complete)
- Function discovery: `tools/lifter/funcs.py` → `work/exe/funcs.json` (8,648 functions, 96.1% of .text,
  554 switch tables). Seeds by confidence: direct entry + calls > immediates in code > pointers in data;
  interior/overlapping/garbage seeds are rejected. Two-level switches (byte table) resolved.
- Translator: `tools/lifter/lift.py` → `build/all/lifted_*.c`. Supports the whole binary: ALU/flags, string ops with DF,
  x87 (decoded by opcode; double precision = MSVC's default precision), MMX (portable C in
  `runtime/x86mmx.h`), rcl/rcr, bt* on memory, lock, pushfd/popfd, cpuid (Pentium II + MMX).
- Runtime: `runtime/x86rt.{h,c}`; flags are stored in ctx only on tail calls / fallthroughs between functions.
- HLE (`tools/lifter/hle.txt`, `runtime/hle_crt.c`): the CRT's memcpy/memmove (asm with tables) → native.
- Known pending item: the CRT's SEH helpers (0x5C323A, 0x5C326E, 0x5C3275) do `jmp eax` to return
  addresses; they are only used when unwinding exceptions. If they show up at runtime → implement native SEH/C++ EH.

### Verification
- `make difftest`: each function runs in Unicorn (original x86) and in the translated version from the same
  coherent random state; 8 registers + writable memory + live stack are compared.
  Result: 3,572 identical, 0 discrepancies, 5,076 not yet testable (they call Win32).
- `make difftrace`: instrumented build (per-instruction register trace) that points at the first diverging
  instruction. This is how these were found: function entry not emitted first, wrongly rejected EH seeds,
  bt on memory, IF in pushfd.
- Coverage is still low on x87 (11/42 functions) and MMX (12/34): the blitters need testing with real surfaces.
- `make test-lzss`: the game's LZSS over the 205 compressed entries of Data.dat (1.67× vs native).

## Win32 host (step 2) — status 2026-11-12
- `make host && make run` → `build/all/joey game`. Loads the sections of the user's `Joey_Pc.exe` at runtime
  (no Konami code is embedded), patches the IAT with thunks (`THUNK_BASE`), creates the PEB/TEB and boots the CRT.
- 512 MB guest space (mmap): image at 0x400000, main stack 0xB00000–0xC00000, heap from 0x1000000.
- `host/galloc.c`: heap with boundary tags inside guest memory (HeapAlloc/Global/Local/VirtualAlloc).
- `host/kernel32.c`: ~110 functions (files with `C:\JOEY\...` paths → `game/`, Spanish locale 0x0C0A,
  cp1252, TLS, events, resources via the PE directory). Deliberately not implemented: CreateThread/ExitThread/
  ResumeThread, RaiseException, RtlUnwind (execution stops with a message if they are called).
- `host/user32.c`: windows/classes on top of SDL2, message queue with SDL events → WM_*, subclassing, wsprintf.
- `host/gdi32.c`: object model (DCs, DIB sections, fonts); SetDIBitsToDevice/TextOutW don't draw yet.
- `host/winmm.c`: timers; joystick "disconnected" (hook point for the Vita's buttons).
- `host/ole32.c`: CoCreateInstance only records the CLSID.
- Unimplemented functions → stub that prints name/args and exits (`imports: N not implemented`).

### Current startup
Full CRT → RegisterClassExA(YUGIOHAPPLICATION) → CreateWindowExA(806×625, client 800×600) → SetWindowPos →
ShowWindow → CoInitialize → CoCreateInstance(DirectDraw, IID_IDirectDraw) → CoCreateInstance(DirectInput) →
LoadLibrary(dinput)+GetProcAddress(DirectInputCreateA) → without checking errors it calls a null method (0x4A6758).
**Step 3**: implement IDirectDraw (+ QueryInterface to newer versions), IDirectInputA (keyboard/mouse), then
IDirectSound, real GDI (SetDIBitsToDevice, TextOutW with a font), threads, MSACM, registry.

## Step 3 (in progress) — status 2026-11-12: the game runs and shows its demo
- `make run` → 800×600 window: Konami logo → demo duel between AIs ("DEMOSTRACIÓN", i.e. "DEMONSTRATION"),
  ~40 fps with vsync, ~13% of one Mac core, ~120 MB RSS.
- The game only uses DirectX 1–3 interfaces: IDirectDraw v1 (fullscreen 800×600×16, RGB565, system-memory
  surfaces + GetDC for text), IDirectInputA + IDirectInputDevice2A (keyboard only; mouse via WM_*),
  IDirectSound v1 (not yet implemented: the game shows "Sound Error!" and continues muted), IPicture (OleLoadPicture).
- `host/com.c` (COM objects: [vtbl][handle], unimplemented methods → stub with name), `host/ddraw.c`
  (full DirectDraw: Blt/BltFast with color key and stretching, Lock, Flip, GetDC), `host/dinput.c` (SDL keyboard
  → DIK), `host/gdi32.c` (DCs on top of DIB/surface/window, TextOutW and measurements with SDL2_ttf; fonts in
  `game/fonts/regular.ttf`/`jp.ttf` or env `JOEY_FONT`/`JOEY_FONT_JP`), `host/advapi32.c` (registry in
  `game/registry.txt`: CommonDir, flcrc), `host/video.c` (SDL texture; `kill -USR1` dumps the screen to
  `$JOEY_DUMP` or screen.bmp).
- The game looks first for loose files (`data/J/...`) and then `Data.dat` → mods with loose files are possible.
- Function discovery: vtable seeds (level 3) can also land on jump targets
  (constructor → method via tail jmp). `funcs.py` reports vtable entries that were not accepted (25 remain:
  ASCII text, .rsrc data and CRT __except blocks).
- Sound (`host/dsound.c`): IDirectSound v1 on top of a software mixer in the SDL callback (44.1 kHz
  stereo; resampling, volume in dB/100, pan, looping, real cursors for streaming). All WAVs are 16-bit PCM
  → MSACM is not needed (it is never called). `JOEY_AUDIO_DUMP=file.raw` records the mix (s16le stereo).
  Verified: the recorded mix matches `m_menu.wav` byte for byte.
- Music: the Spanish repack's `Voice.dat` doesn't include it; the original English one ("Voz Original/Voice.dat", i.e. "Original Voice") does.
  `tools/extract_music.py` extracts it as loose files to `game/Voice/Joey/Spanish/Stream/` (19 tracks).
- Threads (`host/threads.c`): SDL threads + GIL (one guest thread at a time); released in Sleep, waits, busy critical
  sections, GetMessage and vsync. CPU/stack/TEB/TLS per thread; ExitThread with longjmp. The game creates one thread
  for music streaming.
- Performance on Mac: ~15% of one core, ~140 MB RSS with audio.
- Pending: test mouse/keyboard interaction, OleLoadPicture, DirectPlay (LAN, can be skipped).

## Recompilation plan
1. x86→C lifter (Python): one C function per x86 function, registers as local variables/state,
   lazy flags, guest memory = `membase + addr` (32-bit arena on the Vita).
2. Win32 imports / COM vtables → host functions (thunks with magic addresses in the IAT).
3. Native hot paths: MMX blitters, CRT memcpy/memset, LZSS → C/NEON.
4. Vita backend: vita2d or SDL2 (800×600 texture scaled to 725×544), SceCtrl/touch → mouse, SceAudio.
5. First compile and test on macOS/Linux (same lifter, SDL2 backend) before moving to the Vita.

## Controls (Vita / gamepad)
Left stick/d-pad → cursor · Cross = left click · Circle = right click · START = Esc ·
Right stick → mouse wheel (side lists and long descriptions) · on-screen keyboard automatic in text fields
(host/textinput.c) · Triangle = reopen the keyboard.

## Cross-game functions (host/hle_game.c)
- `0x5bedd0` ("do you have an exclusive Yugi/Kaiba card") → always 1: enables "All cards" in the list and
  "Complete Power of Chaos card game" in Options → FIX DECK. Does not modify the save.
- `0x5bea40` (game mask, 4 = Joey) → 7 only for the reward draw (0x445640) and its count (0x47da30):
  rewards come from the whole PoC collection (771 cards). CARD_Pack.bin: bit0 Yugi, bit1 Kaiba, bit2 Joey.
- system.dat: codec and checksums documented by the analysis (9-bit pow mod 299, sum header, flcrc in the registry).

## Art addon (tools/apply_card_addon.py)
`python -I tools/apply_card_addon.py game/Data.dat <addon> build/Data_anime.dat` rewrites card\*.bmp and
mini\*.bmp in place (uncompressed, same size; trims the trailing padding if the original is 2 bytes shorter).

## Custom LiveArea (tools/make_livearea.py)
`python -I tools/make_livearea.py game/Joey_Pc.exe <background.jpg>` → build/livearea/sce_sys (icon: `--icon file.ico` or the exe's MAIN ×4
nearest, 840×500 background, 280×158 cover with title; 8-bit paletted PNG). If it exists, `make vita` packages it instead
of the generic art in vita/sce_sys (which is what goes in the repo: it carries no Konami content).

## LAN duel: DirectPlay 8 (phase 1, mapping — host/dplay.c logs every call)
Menu: Duel Mode → "LAN duel between 2 players" → this machine's/other's data → name (preloaded with
GetUserNameA) → Host / Guest. Objects: CLSID_DirectPlay8Peer + CLSID_DirectPlay8Address, TCP/IP SP
(EBFE7BA0-…), port 2573 ("port" component, DWORD), application GUID D9723E6C-5AB9-4094-B977-D3526270D881.
- Common: `Initialize(ctx 0, handler 0x443760, DPNINITIALIZE_DISABLEPARAMVAL)`, `GetSPCaps`, `SetPeerInfo(name, SYNC)`.
- Host: `Host(appdesc{max 2, session = name}, 1 device address, flags OKTOQUERYFORADDRESSING)`.
- Guest: `EnumHosts(appdesc{app GUID}, host addr without hostname = broadcast, port 2573, infinite count/timeout,
  async)`; on response: `Address::Duplicate` of the sender/device and `Connect` (0x4014b3); `SendTo` at 0x402220.
- Messages the game uses (0x443aa0, and 0x401c00 for the connection ones):
  CREATE_PLAYER (GetPeerInfo → name; LOCAL/HOST flags → own/host dpnid), DESTROY_PLAYER, HOST_MIGRATE,
  RECEIVE (type 2 packet: [u8 2][4 bytes][u16 len][data] → game queue), TERMINATE_SESSION,
  ASYNC_OP_COMPLETE (end of EnumHosts), CONNECT_COMPLETE (hResult), ENUM_HOSTS_RESPONSE (0x401770: list of
  hosts de-duplicated by instance GUID; ignores full sessions).
- **Phase 2 (host/dplay.c)**: custom DirectPlay over sockets, only between copies of the port. Discovery: UDP to
  2573 (broadcast, per-interface directed broadcast and 127.0.0.1), reply with instance GUID, players and name.
  Session: one TCP connection, frames `[u32 len][u8 type][data]` (HELLO name, WELCOME name+ids, DATA = game packet).
  Non-blocking sockets polled from PeekMessage/GetMessage; the game's handler is called on the main thread.
  Details that matter: `GetSPCaps` must return the real retry interval (1500 ms): the game removes from the list
  hosts it doesn't hear from within 3× that interval. Game packets: `[u8 2][u32 sender dpnid][u16 len][data]`,
  `data` starts with a u16 command (0xF004 deck, 0xF005, 0xFC64, …).
- Registry: the game stores PlayerName without a NUL (cb = strlen); RegQueryValueExA now returns it terminated, as
  Windows does (before, garbage came out: "Playeram"). `JOEY_USERNAME` changes the default name (GetUserNameA).
- Tested Mac↔Mac (two instances): host list, connection, rock-paper-scissors, duel start, summoning and
  turn change in sync.

## Text fields → Vita keyboard (host/textinput.c)
All fields (LAN name, export deck, replay title) go through 0x44e900 (thiscall, this = field), which
only runs while one has focus: hle_game.c wraps it. Field: std::string at +0x18 (data +0x1c, length
+0x20, max 20), cursor +0x28; insert 0x44e6f0(char, pos<0 = end), delete 0x44e7d0(pos). Rejected
characters (table 0x44ec64, it is for file names): space " * / : < > ? \ | → space is typed as `_`.
Backspace/arrows in the field go through DirectInput, not messages. `JOEY_TYPE="text"` fills the first field (tests).

## Windows (mingw-w64, `make win`)
Portability changes: `host.h` (mkdir → _mkdir, O_BINARY), `kernel32.c` (binary open, localtime_s/gmtime_s),
`main.c` (VirtualAlloc; SDL_main; `game` folder next to Joey.exe and log in game\log.txt; no windows.h because
winbase.h defines its own GPTR), `dplay.c` (Winsock: WSAStartup, ioctlsocket FIONBIO, WSAGetLastError, directed
broadcast with GetAdaptersInfo), `video.c` (no SIGUSR1), `gdi32.c` (fonts: fonts\Arimo.ttf next to the exe, system Arial and MS
Gothic). Depends on SDL2.dll, SDL2_ttf.dll and the UCRT (Windows 10+). Packaging: tools/make_winpkg.py.

## Android (NDK, `make android`)
- libmain.so = lifted + host compiled with the NDK's clang (arm64, API 24); SDL2/SDL2_ttf (FreeType/HarfBuzz
  vendored) with tools/android_deps.sh. android/ is only the Gradle packaging: SDL's Java code is taken from the
  SDL2 tree in work/android, and jniLibs/assets/res from build/android.
- SDLActivity calls SDL_main on a Java thread with a small stack: main() is relaunched on a 64 MB SDL_Thread.
  On finish, exit() is called: the next launch is a new process (all guest state lives in globals).
- SDL_WINDOW_FULLSCREEN window (immersive mode) + layoutInDisplayCutoutMode SHORT_EDGES: the 4:3 image stays centered.
- Font: relative "fonts/Arimo.ttf" = APK asset (SDL_RWFromFile). Japanese: /system/fonts/NotoSansCJK-Regular.ttc.
- Touches (user32.c, touch_button/touch_pump): the game updates hover and polls the button once per frame, so
  a tap (jump + down + up in the same frame) was lost. For events with which == SDL_TOUCH_MOUSEID: WM_MOUSEMOVE
  immediately, WM_LBUTTONDOWN 60 ms later and WM_LBUTTONUP at least 100 ms after the down.
- Back: SDL_HINT_ANDROID_TRAP_BACK_BUTTON=1 and SDLK_AC_BACK -> VK_ESCAPE / DIK_ESCAPE (otherwise Android closes the app).
- Pause/resume (Home and return) tested in the emulator: SDL restores the EGL context; the renderer is not multithreaded.
- Phase 2 (touchui.c): on Android there is no SDL_RenderSetLogicalSize; touchui_render draws the game in `game` and the
  buttons in the bands (min. 0.55 in, the game shrinks if needed). SDL_HINT_TOUCH_MOUSE_EVENTS=0: fingers go
  to touchui_event; mouse events with which == SDL_TOUCH_MOUSEID are ignored and a real mouse is mapped with
  touchui_to_game. SetCursorPos doesn't warp on Android (the warp came back as movement in window pixels and sent
  the cursor to the edge): it moves mouse_x/y directly.
- host_small_screen(): always on Vita; on Android if the game image is < 3 in tall (SDL_GetDisplayDPI). gdi32 adds
  1 px to fonts <= 12 px.
- Keyboard: textinput.c calls JoeyActivity.showTextInput (AlertDialog + EditText, max 20) via JNI from the game
  thread (SDL_AndroidGetJNIEnv / GetObjectClass(activity)); the reply arrives at nativeTextResult on the UI thread and
  host_text_poll applies it the same way as the Vita's IME. dismissTextInput if the game closes the field.
- Right click and Esc from buttons are held ~100 ms (pad_r / pad_start) so the game's polling sees them.
- Phase 3 (LAN): same dplay.c (POSIX; getifaddrs exists since API 24). JoeyActivity takes a WifiManager.MulticastLock
  while the game runs: many Wi-Fi drivers drop broadcasts without it (the host wouldn't hear the search).
  Test in the emulator (its own NAT, broadcast doesn't get out): `adb emu redir add udp:2573:2573` + `adb forward tcp:2573
  tcp:2573`; Android host, Mac guest with JOEY_AUTOCLICK (the Mac queries 127.0.0.1): list, connection,
  rock-paper-scissors, turn and duel in sync; when the Mac closes, Android shows "conexión perdida" ("connection lost").

## Thread leak on the Vita (freeze after long sessions)
- SDL2 on the Vita ignores SDL_DetachThread (it doesn't call sceKernelDeleteThread): each guest thread (the game creates
  one loading thread per screen) leaked its 2 MB stack. After ~36 threads memory hit 0, sceKernelCreateThread failed
  and the game kept waiting for that thread. Now threads.c doesn't detach them: reap_threads() does SDL_WaitThread on
  the already finished threads before creating another.
