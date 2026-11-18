BUILD_DIR ?= build
CMAKE ?= cmake
CTEST ?= ctest
ARDUINO_CLI ?= arduino-cli
ARDUINO_FQBN ?= arduino:avr:uno
ARDUINO_PORT ?=

.PHONY: all configure build test firmware upload clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

firmware:
	$(ARDUINO_CLI) compile --fqbn $(ARDUINO_FQBN) firmware/ArduinoController

upload:
	@test -n "$(ARDUINO_PORT)" || { echo "Specify ARDUINO_PORT (example: make upload ARDUINO_PORT=/dev/ttyACM0)" >&2; exit 2; }
	$(ARDUINO_CLI) compile --upload --fqbn $(ARDUINO_FQBN) --port "$(ARDUINO_PORT)" firmware/ArduinoController

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean
