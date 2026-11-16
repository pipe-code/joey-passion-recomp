# Host build (macOS/Linux) of the recompiled Joey_Pc.exe.
#   make lift            -> analyze game/Joey_Pc.exe and translate it to build/all/lifted_*.c
#   make -j10 host       -> compile the translated code + Win32 layer into build/all/joey  (separate invocation:
#                           the list of lifted_*.c files is read when make starts)
#   make run             -> play
#   make difftest        -> differential test of every function against the original x86 (Unicorn)
#   make test-lzss       -> needs work/assets/raw (python -I tools/kcej.py game/Data.dat unpack work/assets/raw)

.DEFAULT_GOAL := host
PY      := .venv/bin/python -I
CC      := clang
CFLAGS  := -O2 -Wall -Wno-unused-label -Wno-unused-but-set-variable -Wno-unused-variable \
           -Wno-parentheses-equality -Iruntime -Ibuild/all
OUT     := build/all

LIFTED_C := $(wildcard $(OUT)/lifted_*.c)
LIFTED_O := $(patsubst $(OUT)/%.c,$(OUT)/obj/%.o,$(LIFTED_C))
RUNTIME  := runtime/x86rt.c runtime/hle_crt.c

.PHONY: lift funcs test-lzss clean-obj

funcs: work/exe/funcs.json
work/exe/funcs.json: game/Joey_Pc.exe tools/lifter/funcs.py
	@mkdir -p $(dir $@)
	$(PY) tools/lifter/funcs.py $< $@

lift: work/exe/funcs.json
	$(PY) tools/lifter/lift.py game/Joey_Pc.exe work/exe/funcs.json $(OUT) --all

$(OUT)/obj/%.o: $(OUT)/%.c $(OUT)/lifted.h runtime/x86rt.h runtime/x86mmx.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/lzss_test: tests/lzss_spike.c $(RUNTIME) $(LIFTED_O)
	$(CC) $(CFLAGS) -o $@ $^

test-lzss: $(OUT)/lzss_test
	$< game/Data.dat work/assets/raw $(OUT)/image.bin

$(OUT)/libdifftest.dylib: tests/difftest_lib.c $(RUNTIME) $(LIFTED_O)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $^

difftest-lib: $(OUT)/libdifftest.dylib

# Instrumented build (per-instruction register trace) for debugging difftest mismatches
TOUT := build/trace
lift-trace: work/exe/funcs.json
	LIFT_TRACE=1 $(PY) tools/lifter/lift.py game/Joey_Pc.exe work/exe/funcs.json $(TOUT) --all
	$(MAKE) OUT=$(TOUT) $(TOUT)/libdifftest.dylib

# Differential test vs. Unicorn (original x86) over every function; trace failures to the first bad instruction
difftest: $(OUT)/libdifftest.dylib
	$(PY) tools/difftest.py work/difftest/all.json --jobs 9 --seeds 4
difftrace: lift-trace
	$(PY) tools/difftrace.py work/difftest/all.json work/difftest/trace.json

# Native host (macOS): Win32 emulation layer + recompiled game
HOST_SRC := $(wildcard host/*.c)
$(OUT)/imports.h: game/Joey_Pc.exe tools/gen_imports.py
	$(PY) tools/gen_imports.py $< $@
$(OUT)/joey: $(HOST_SRC) host/host.h $(OUT)/imports.h $(RUNTIME) $(LIFTED_O)
	$(CC) $(CFLAGS) -Ihost $(shell sdl2-config --cflags) -o $@ $(HOST_SRC) $(RUNTIME) $(LIFTED_O) $(shell sdl2-config --libs) -lSDL2_ttf
host: $(OUT)/joey
run: $(OUT)/joey
	$(OUT)/joey game
# macOS app bundle from your own files: make app APPFLAGS="--data build/Data_anime.dat --icon joey.ico"
app: $(OUT)/joey
	$(PY) tools/make_macapp.py $(APPFLAGS)

# ------------------------------------------------------------------------------------------------ PS Vita
#   make lift && make -j10 vita     -> build/vita/joey.vpk   (VitaSDK in $VITASDK, default ~/vitasdk)
VITASDK ?= $(HOME)/vitasdk
VBIN    := $(VITASDK)/bin
VCC     := $(VBIN)/arm-vita-eabi-gcc
VOUT    := build/vita
VTITLE  := JOEY00001
VPROF   ?= 1          # 1 = sampling profiler (PROFILE lines in log.txt); 0 for release builds
# -marm: lifted functions keep 8 guest registers + flags live; ARM mode has far fewer spills than Thumb.
# no tree vectorization: GCC "vectorizes" register spills to the CPU struct through the stack (slower).
VCFLAGS := -O2 -march=armv7-a -mtune=cortex-a9 -mfpu=neon -marm -fno-tree-vectorize -fno-tree-slp-vectorize \
           -Iruntime -I$(OUT) -Ihost \
           $(if $(filter 1,$(strip $(VPROF))),-DX86_PROF) \
           -I$(VITASDK)/arm-vita-eabi/include/SDL2 -Wno-unknown-pragmas
VLIFTED_O := $(patsubst $(OUT)/%.c,$(VOUT)/obj/%.o,$(LIFTED_C))
VHOST_O   := $(patsubst host/%.c,$(VOUT)/host/%.o,$(HOST_SRC)) $(VOUT)/rt/x86rt.o $(VOUT)/rt/hle_crt.o
VLIBS   := -Wl,--start-group -lSDL2_ttf -lharfbuzz -lfreetype -lpng16 -lbz2 -lz -lSDL2 \
           -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub -lSceAppMgr_stub -lSceAppUtil_stub -lSceAudio_stub \
           -lSceAudioIn_stub -lSceSysmodule_stub -lSceIofilemgr_stub -lSceCommonDialog_stub -lSceTouch_stub \
           -lSceHid_stub -lSceMotion_stub -lScePower_stub -lSceNet_stub -lSceNetCtl_stub -lSceProcessmgr_stub -lSceKernelDmacMgr_stub \
           -lpthread -lm -Wl,--end-group
LIVEAREA := sce_sys/icon0.png sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/startup.png \
            sce_sys/livearea/contents/template.xml

$(VOUT)/obj/%.o: $(OUT)/%.c $(OUT)/lifted.h runtime/x86rt.h runtime/x86mmx.h
	@mkdir -p $(dir $@)
	$(VCC) $(VCFLAGS) -w -c $< -o $@
$(VOUT)/host/%.o: host/%.c host/host.h runtime/x86rt.h $(OUT)/imports.h
	@mkdir -p $(dir $@)
	$(VCC) $(VCFLAGS) -Wall -Wno-unused-but-set-variable -c $< -o $@
$(VOUT)/rt/%.o: runtime/%.c runtime/x86rt.h runtime/x86mmx.h
	@mkdir -p $(dir $@)
	$(VCC) $(VCFLAGS) -Wall -c $< -o $@

$(VOUT)/joey.elf: $(VLIFTED_O) $(VHOST_O) tools/vita_link.sh
	tools/vita_link.sh $(VCC) $@ $(VOUT)/pad -- $(VLIFTED_O) $(VHOST_O) -- $(VLIBS)
$(VOUT)/eboot.bin: $(VOUT)/joey.elf
	$(VBIN)/vita-elf-create $< $(VOUT)/joey.velf
	$(VBIN)/vita-make-fself -c $(VOUT)/joey.velf $@
$(VOUT)/param.sfo:
	@mkdir -p $(dir $@)
	$(VBIN)/vita-mksfoex -s TITLE_ID=$(VTITLE) -d ATTRIBUTE2=12 "Joey the Passion" $@
# LiveArea art: build/livearea (made by tools/make_livearea.py from your own game files) if present, else vita/
LADIR := $(if $(wildcard build/livearea/sce_sys/icon0.png),build/livearea,vita)
$(VOUT)/joey.vpk: $(VOUT)/eboot.bin $(VOUT)/param.sfo $(addprefix $(LADIR)/,$(LIVEAREA)) assets/fonts/Arimo.ttf
	$(VBIN)/vita-pack-vpk -s $(VOUT)/param.sfo -b $(VOUT)/eboot.bin \
	  $(foreach f,$(LIVEAREA),-a $(LADIR)/$(f)=$(f)) -a assets/fonts/Arimo.ttf=fonts/Arimo.ttf $@
vita: $(VOUT)/joey.vpk
.PHONY: vita

# ------------------------------------------------------------------------------------------------ Windows
#   make -j10 win [WICON=icon.ico]   -> build/win/Joey.exe (x86-64, cross-compiled with mingw-w64)
#   SDL2 / SDL2_ttf mingw development packages unpacked in $(WLIBS) (see README)
WCC     := x86_64-w64-mingw32-gcc
WRC     := x86_64-w64-mingw32-windres
WLIBS   ?= work/winlibs
WSDL    := $(WLIBS)/SDL2-2.30.9/x86_64-w64-mingw32
WTTF    := $(WLIBS)/SDL2_ttf-2.22.0/x86_64-w64-mingw32
WOUT    := build/win
WICON   ?=
WCFLAGS := -O2 -Iruntime -Ibuild/all -Ihost -I$(WSDL)/include/SDL2 -I$(WTTF)/include/SDL2 -Wno-unknown-pragmas
WLIFTED_O := $(patsubst $(OUT)/%.c,$(WOUT)/obj/%.o,$(LIFTED_C))
WHOST_O   := $(patsubst host/%.c,$(WOUT)/host/%.o,$(HOST_SRC)) $(patsubst runtime/%.c,$(WOUT)/rt/%.o,$(RUNTIME))
WRES_O    := $(if $(WICON),$(WOUT)/joey_res.o)

$(WOUT)/obj/%.o: $(OUT)/%.c $(OUT)/lifted.h runtime/x86rt.h runtime/x86mmx.h
	@mkdir -p $(dir $@)
	$(WCC) $(WCFLAGS) -w -c $< -o $@
$(WOUT)/host/%.o: host/%.c host/host.h runtime/x86rt.h $(OUT)/imports.h
	@mkdir -p $(dir $@)
	$(WCC) $(WCFLAGS) -Wall -Wno-unused-but-set-variable -c $< -o $@
$(WOUT)/rt/%.o: runtime/%.c runtime/x86rt.h runtime/x86mmx.h
	@mkdir -p $(dir $@)
	$(WCC) $(WCFLAGS) -Wall -c $< -o $@
$(WOUT)/joey_res.o: $(WICON)
	@mkdir -p $(dir $@)
	printf '1 ICON "%s"\n' "$(abspath $(WICON))" > $(WOUT)/joey.rc
	$(WRC) $(WOUT)/joey.rc -O coff -o $@
$(WOUT)/Joey.exe: $(WLIFTED_O) $(WHOST_O) $(WRES_O)
	$(WCC) -o $@ $^ -L$(WSDL)/lib -L$(WTTF)/lib -lmingw32 -lSDL2main -lSDL2 -lSDL2_ttf \
	  -lws2_32 -liphlpapi -mwindows -static-libgcc
win: $(WOUT)/Joey.exe

# ------------------------------------------------------------------------------------------------ Android
#   tools/android_deps.sh                    -> SDL2 / SDL2_ttf for arm64-v8a in work/android/prefix (once)
#   make -j10 android [AICON=icon.ico]       -> build/android/joey.apk  (game files are imported on the device)
ANDROID_HOME ?= $(HOME)/Library/Android/sdk
ANDK    ?= $(ANDROID_HOME)/ndk/26.1.10909125
ACC     := $(ANDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android24-clang
APREFIX := work/android/prefix
AOUT    := build/android
AICON   ?=
ACFLAGS := -O2 -fPIC -Iruntime -Ibuild/all -Ihost -I$(APREFIX)/include/SDL2 -Wno-unknown-pragmas
ALIFTED_O := $(patsubst $(OUT)/%.c,$(AOUT)/obj/%.o,$(LIFTED_C))
AHOST_O   := $(patsubst host/%.c,$(AOUT)/host/%.o,$(HOST_SRC)) $(patsubst runtime/%.c,$(AOUT)/rt/%.o,$(RUNTIME))
AJNI    := $(AOUT)/jniLibs/arm64-v8a

$(AOUT)/obj/%.o: $(OUT)/%.c $(OUT)/lifted.h runtime/x86rt.h runtime/x86mmx.h
	@mkdir -p $(dir $@)
	$(ACC) $(ACFLAGS) -w -c $< -o $@
$(AOUT)/host/%.o: host/%.c host/host.h runtime/x86rt.h $(OUT)/imports.h
	@mkdir -p $(dir $@)
	$(ACC) $(ACFLAGS) -Wall -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-variable -Wno-parentheses-equality -c $< -o $@
$(AOUT)/rt/%.o: runtime/%.c runtime/x86rt.h runtime/x86mmx.h
	@mkdir -p $(dir $@)
	$(ACC) $(ACFLAGS) -Wall -Wno-unused-label -Wno-unused-variable -Wno-parentheses-equality -c $< -o $@
$(AJNI)/libmain.so: $(ALIFTED_O) $(AHOST_O)
	@mkdir -p $(dir $@)
	$(ACC) -shared -o $@ $^ -L$(APREFIX)/lib -lSDL2 -lSDL2_ttf -llog -landroid -lm -Wl,--build-id
	cp $(APREFIX)/lib/libSDL2.so $(APREFIX)/lib/libSDL2_ttf.so $(AJNI)/
$(AOUT)/joey.apk: $(AJNI)/libmain.so assets/fonts/Arimo.ttf $(wildcard android/app/src/main/java/*/*/*/*.java) android/app/src/main/AndroidManifest.xml
	$(PY) tools/make_android_res.py $(if $(AICON),--icon $(AICON))
	cd android && ANDROID_HOME=$(ANDROID_HOME) ./gradlew -q assembleDebug
	cp android/app/build/outputs/apk/debug/app-debug.apk $@
android: $(AOUT)/joey.apk
.PHONY: android
