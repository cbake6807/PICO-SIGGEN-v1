# Gated Pulse — build helpers (macOS / Linux; on Windows use the arduino-cli
# commands from the README directly, or run make under WSL/Git Bash).
#
#   make flash      compile + upload
#   make build      compile only
#   make upload     upload the last build
#   make monitor    open a serial monitor (resets the board on connect)
#   make state      ask the running firmware to print its state
#   make pioverify  re-assemble the .pio sources, diff vs the sketch's arrays
#   make clean      remove build artifacts
#
# Override anything on the command line:
#   make flash PORT=/dev/ttyACM0
#   make flash FQBN=rp2040:rp2040:rpipico2

FQBN ?= rp2040:rp2040:rpipico2w
BAUD ?= 115200

# Auto-detect the board's serial port, so a fresh checkout usually needs no
# arguments. Overridable, and the error below explains what to do if the guess
# fails -- which it will on a machine with more than one USB serial device.
PORT ?= $(shell arduino-cli board list 2>/dev/null | \
          awk '/rpipico2w|RP2350|Pico/ {print $$1; exit}')

ARDUINO_CLI ?= arduino-cli
SKETCH      ?= GatedPulsePico

# pioasm ships inside the RP2040 core. The glob keeps this working across core
# updates rather than pinning a version that will rot.
PIOASM ?= $(firstword $(wildcard \
  $(HOME)/Library/Arduino15/packages/rp2040/tools/pqt-pioasm/*/pioasm \
  $(HOME)/.arduino15/packages/rp2040/tools/pqt-pioasm/*/pioasm))

.PHONY: help build upload flash monitor state pioverify clean check-port

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

build:
	$(ARDUINO_CLI) compile --fqbn $(FQBN) $(SKETCH)

upload: check-port
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $(PORT) $(SKETCH)

flash: build upload

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

clean:
	rm -rf build $(SKETCH)/build
