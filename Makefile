# Gated Pulse — build helpers (macOS / Linux; on Windows use the arduino-cli
# commands from the README directly, or run make under WSL/Git Bash).
#
#   make flash      compile + upload over USB
#   make ota        compile + upload over WiFi (needs OTA_PASS in secrets.h)
#   make build      compile only
#   make upload     upload the last build over USB
#   make monitor    open a serial monitor (resets the board on connect)
#   make state      ask the running firmware to print its state
#   make pioverify  re-assemble the .pio sources, diff vs the sketch's arrays
#   make apk        build the Android app -> android/GatedPulse.apk
#   make apk-install  build it and adb install to a USB-connected phone
#   make clean      remove build artifacts
#
# Override anything on the command line:
#   make flash PORT=/dev/ttyACM0
#   make ota   HOST=192.168.1.9
#   make flash FQBN=rp2040:rp2040:rpipico2

# flash=...: 2 MB sketch + 2 MB LittleFS. The FS is where an over-the-air
# image is staged before the bootloader copies it in; the firmware is ~0.5 MB.
# The EEPROM sector (saved settings) sits above the FS at the same address in
# every layout, so changing this keeps a board's saved settings.
FQBN ?= rp2040:rp2040:rpipico2w:flash=4194304_2097152
BAUD ?= 115200

# Auto-detect the board's serial port, so a fresh checkout usually needs no
# arguments. Overridable, and the error below explains what to do if the guess
# fails -- which it will on a machine with more than one USB serial device.
PORT ?= $(shell arduino-cli board list 2>/dev/null | \
          awk '/usbmodem|ttyACM/ {p=$$1} /rpipico2w|RP2350|Pico/ {print p; exit}')

# Where `make ota` sends the image. The IP is the reliable choice: the board's
# mDNS answer includes an IPv6 address it does not serve HTTP on.
HOST ?= gatedpulse.local

ARDUINO_CLI ?= arduino-cli
SKETCH      ?= GatedPulsePico
OUT         ?= build/$(SKETCH)

# OTA credentials come from the gitignored secrets.h, never from this file.
OTA_USER = $(or $(shell sed -n 's/^\#define OTA_USER "\([^"]*\)".*/\1/p' $(SKETCH)/secrets.h 2>/dev/null),gatedpulse)
OTA_PASS = $(shell sed -n 's/^\#define OTA_PASS "\([^"]*\)".*/\1/p' $(SKETCH)/secrets.h 2>/dev/null)

# pioasm ships inside the RP2040 core. The glob keeps this working across core
# updates rather than pinning a version that will rot.
PIOASM ?= $(firstword $(wildcard \
  $(HOME)/Library/Arduino15/packages/rp2040/tools/pqt-pioasm/*/pioasm \
  $(HOME)/.arduino15/packages/rp2040/tools/pqt-pioasm/*/pioasm))

.PHONY: help build upload flash ota monitor state pioverify apk apk-install clean check-port

help:
	@grep -E '^#   make ' $(MAKEFILE_LIST) | sed 's/^#   //'

check-port:
	@if [ -z "$(PORT)" ]; then \
	  echo ""; \
	  echo "  No board found. Is it plugged in?"; \
	  echo "  List what the system sees:   arduino-cli board list"; \
	  echo "  Then pass it explicitly:     make flash PORT=/dev/cu.usbmodemXXXX"; \
	  echo ""; exit 1; \
	fi

# build_info.h stamps the firmware with the git revision it was built from
# ("-dirty" if the sketch had uncommitted changes), so the board can say in
# /state and on its web page exactly which build it is running. Gitignored.
build:
	@printf '#define FW_BUILD "%s %s"\n' \
	  "$$(git rev-parse --short HEAD 2>/dev/null || echo local)$$(git diff --quiet HEAD -- $(SKETCH) 2>/dev/null || echo -dirty)" \
	  "$$(date +%Y-%m-%d)" > $(SKETCH)/build_info.h
	$(ARDUINO_CLI) compile --fqbn $(FQBN) --output-dir $(OUT) $(SKETCH)

upload: check-port
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $(PORT) --input-dir $(OUT) $(SKETCH)

flash: build upload

# Reflash over WiFi: no USB lead, the board can stay wired into the rig. The
# output stops for the few seconds the board takes to reboot. The first time,
# flash over USB: the OTA endpoint and the flash layout it needs arrive with v2.
# Close anything polling the board hard (an OBS browser source, say) first --
# it serves one client at a time and a starved upload gets cut off.
ota: build
	@test -n "$(OTA_PASS)" || { echo "set OTA_PASS in $(SKETCH)/secrets.h, then flash once over USB"; exit 1; }
	@echo "uploading $(OUT)/$(SKETCH).ino.bin to $(HOST)/update"
	@curl -4 -sS --fail-with-body -u "$(OTA_USER):$(OTA_PASS)" \
	  -F "image=@$(OUT)/$(SKETCH).ino.bin" http://$(HOST)/update
	@echo; echo "waiting for $(HOST) to reboot..."; \
	for i in $$(seq 1 20); do curl -4 -s -m 1 -o /dev/null http://$(HOST)/state || break; sleep 1; done; \
	for i in $$(seq 1 30); do sleep 2; \
	  b=$$(curl -4 -s -m 2 http://$(HOST)/state | sed -n 's/.*"build":"\([^"]*\)".*/\1/p'); \
	  [ -n "$$b" ] && { echo "up: build $$b"; exit 0; }; done; \
	echo "no answer after 60 s"; exit 1

monitor: check-port
	$(ARDUINO_CLI) monitor --port $(PORT) --config baudrate=$(BAUD)

state: check-port
	@printf 'P\n' > $(PORT); sleep 1; head -c 2000 < $(PORT)

# Not optional busywork: the sketch carries hand-maintained PIO encodings
# because arduino-cli does not run pioasm during a sketch build. Run this after
# touching any .pio file or any *_insns[] array -- a silent drift between them
# is a waveform bug you would chase on a scope instead of in a diff.
pioverify:
	@if [ -z "$(PIOASM)" ]; then \
	  echo "pioasm not found -- install the rp2040 core first (see README)"; exit 1; fi
	python3 tools/pioverify.py "$(PIOASM)" $(SKETCH)

# Android app -- bare SDK tools, no Gradle. Setup: android/README.md
apk:
	android/build.sh

apk-install:
	android/build.sh install

clean:
	rm -rf build $(SKETCH)/build
