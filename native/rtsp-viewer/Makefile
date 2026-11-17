# Build a receiver for iDrive 8's Intel MGU. Uses the same pinned userspace
# and ABI checks as RideTube's ytdec; no FFmpeg or third-party media runtime.
CXX ?= g++
PYTHON ?= python3
BUILD ?= build
IMAGE := rtsp-viewer-build
T := $(BUILD)/target
CXXFLAGS = -std=c++14 -O2 -g -Wall -Wextra -Werror
CPPFLAGS = -Isrc -Iabi/include
SRC = src/rtsp_viewer.cpp src/decoder_msdk.cpp src/nal_reader.cpp src/mgvf.cpp
STUBS = $(T)/stub/libmfxhw64.so $(T)/stub/libva.so.1 $(T)/stub/libva-drm.so.1

.PHONY: target target-in-docker clean
target:
	docker build --platform linux/amd64 -t $(IMAGE) docker
	docker run --rm --platform linux/amd64 -v "$(CURDIR):/w" -w /w $(IMAGE) make target-in-docker

target-in-docker: $(T)/rtsp-viewer

$(T)/stub/libmfxhw64.so: abi/libmfx.symbols
	mkdir -p $(@D)
	$(PYTHON) tools/ramses_abi.py stub-source $< > $(@D)/mfx_stub.c
	gcc -shared -Wl,-soname,libmfxhw64.so -o $@ $(@D)/mfx_stub.c

$(T)/stub/libva.so.1: abi/libva.symbols
	mkdir -p $(@D)
	$(PYTHON) tools/ramses_abi.py stub-source $< > $(@D)/va_stub.c
	gcc -shared -Wl,-soname,libva.so.1 -o $@ $(@D)/va_stub.c

$(T)/stub/libva-drm.so.1: abi/libva-drm.symbols
	mkdir -p $(@D)
	$(PYTHON) tools/ramses_abi.py stub-source $< > $(@D)/va_drm_stub.c
	gcc -shared -Wl,-soname,libva-drm.so.1 -o $@ $(@D)/va_drm_stub.c

$(T)/rtsp-viewer: $(SRC) $(wildcard src/*.h) $(wildcard abi/include/mfx/*.h) $(STUBS)
	mkdir -p $(@D)
	g++ $(CXXFLAGS) $(CPPFLAGS) -DNDEBUG -pthread -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2 -o $@ $(SRC) $(STUBS) -lresolv
	$(PYTHON) tools/abi.py check $@ abi/libvsomeip3.so.3.symbols
	$(PYTHON) tools/woc_abi.py check-imports $@ --prefix MFX va -- abi/libmfx.symbols abi/libva.symbols abi/libva-drm.symbols

clean:
	rm -rf $(BUILD)
