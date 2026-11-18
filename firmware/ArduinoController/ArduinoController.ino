#include <Servo.h>

namespace {

// Keep this allowlist in sync with the host's arduino-controller.conf.
constexpr uint8_t kMaximumPin = 19;
constexpr unsigned long kMinimumWatchdogMs = 500;
constexpr unsigned long kMaximumWatchdogMs = 10000;
constexpr uint8_t kDigital = 1;
constexpr uint8_t kPwm = 2;
constexpr uint8_t kServo = 4;

struct PinCapability {
    uint8_t pin;
    uint8_t mode;
};

const PinCapability kFirmwareAllowlist[] = {
    {8, kDigital},
    {3, kPwm},
    {10, kServo},
};

Servo servoOutputs[kMaximumPin + 1];
bool servoAttached[kMaximumPin + 1] = {};
uint8_t allowedModes[kMaximumPin + 1] = {};
char inputLine[80];
uint8_t inputLength = 0;
bool discardingLine = false;
unsigned long watchdogMs = 2000;
unsigned long lastHeartbeat = 0;
bool watchdogTripped = true;

bool isDigitalPin(int pin) {
    return (pin >= 2 && pin <= 13) || (pin >= 14 && pin <= 19);
}

bool isPwmPin(int pin) {
    return pin == 3 || pin == 5 || pin == 6 || pin == 9 || pin == 10 || pin == 11;
}

bool isFirmwareAllowed(uint8_t pin, uint8_t mode) {
    for (const PinCapability& capability : kFirmwareAllowlist) {
        if (capability.pin == pin && capability.mode == mode) return true;
    }
    return false;
}

uint8_t modeFlag(const char* mode) {
    if (mode == nullptr) return 0;
    if (strcmp(mode, "DIGITAL") == 0) return kDigital;
    if (strcmp(mode, "PWM") == 0) return kPwm;
    if (strcmp(mode, "SERVO") == 0) return kServo;
    return 0;
}

void allOutputsLow() {
    for (uint8_t pin = 2; pin <= kMaximumPin; ++pin) {
        if (allowedModes[pin] == 0) continue;
        if (servoAttached[pin]) {
            servoOutputs[pin].detach();
            servoAttached[pin] = false;
        }
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
}

void reject(const char* reason) {
    Serial.print(F("ERR "));
    Serial.println(reason);
}

bool parseLong(const char* text, long& value) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    value = strtol(text, &end, 10);
    return end != text && *end == '\0';
}

void processLine(char* line) {
    char* save = nullptr;
    char* command = strtok_r(line, " \t", &save);
    if (command == nullptr) return;

    if (strcmp(command, "HELLO") == 0 && strtok_r(nullptr, " \t", &save) == nullptr) {
        // A new host session starts disarmed, with every previously allowed output LOW.
        allOutputsLow();
        memset(allowedModes, 0, sizeof(allowedModes));
        watchdogTripped = false;
        lastHeartbeat = millis();
        Serial.println(F("READY"));
        return;
    }

    if (strcmp(command, "WD") == 0) {
        if (watchdogTripped) {
            reject("WATCHDOG");
            return;
        }
        char* valueText = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        long value = 0;
        if (extra != nullptr || !parseLong(valueText, value) ||
            value < static_cast<long>(kMinimumWatchdogMs) ||
            value > static_cast<long>(kMaximumWatchdogMs)) {
            reject("WATCHDOG_RANGE");
            return;
        }
        watchdogMs = static_cast<unsigned long>(value);
        watchdogTripped = false;
        lastHeartbeat = millis();
        Serial.print(F("OK WD "));
        Serial.println(watchdogMs);
        return;
    }

    if (strcmp(command, "ALLOW") == 0) {
        char* pinText = strtok_r(nullptr, " \t", &save);
        char* modeText = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        long pin = 0;
        const uint8_t flag = modeFlag(modeText);
        // Host configuration may select only capabilities explicitly compiled here.
        if (extra != nullptr || !parseLong(pinText, pin) || pin < 0 ||
            pin > kMaximumPin || !isDigitalPin(static_cast<int>(pin)) || flag == 0 ||
            (flag == kPwm && !isPwmPin(static_cast<int>(pin))) ||
            !isFirmwareAllowed(static_cast<uint8_t>(pin), flag)) {
            reject("PIN_MODE");
            return;
        }
        if (flag == kPwm) {
            // Servo uses Timer 1 on Uno, which conflicts with PWM on D9/D10.
            for (uint8_t candidate = 2; candidate <= kMaximumPin; ++candidate) {
                if (allowedModes[candidate] == kServo &&
                    (pin == 9 || pin == 10)) {
                    reject("SERVO_PWM_TIMER_CONFLICT");
                    return;
                }
            }
        }
        if (flag == kServo && allowedModes[9] == kPwm) {
            reject("SERVO_PWM_TIMER_CONFLICT");
            return;
        }
        if (flag == kServo && allowedModes[10] == kPwm) {
            reject("SERVO_PWM_TIMER_CONFLICT");
            return;
        }
        if (allowedModes[pin] != 0 && allowedModes[pin] != flag) {
            reject("PIN_ALREADY_CONFIGURED");
            return;
        }
        allowedModes[pin] = flag;
        pinMode(static_cast<uint8_t>(pin), OUTPUT);
        digitalWrite(static_cast<uint8_t>(pin), LOW);
        Serial.print(F("OK ALLOW "));
        Serial.println(pin);
        return;
    }

    if (strcmp(command, "STOP") == 0 &&
        strtok_r(nullptr, " \t", &save) == nullptr) {
        allOutputsLow();
        Serial.println(F("OK STOP"));
        return;
    }

    if (strcmp(command, "HB") == 0 && strtok_r(nullptr, " \t", &save) == nullptr) {
        if (watchdogTripped) {
            reject("WATCHDOG");
            return;
        }
        lastHeartbeat = millis();
        Serial.println(F("OK HB"));
        return;
    }

    if (strcmp(command, "SET") == 0) {
        char* pinText = strtok_r(nullptr, " \t", &save);
        char* modeText = strtok_r(nullptr, " \t", &save);
        char* valueText = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        long pin = 0;
        long value = 0;
        const uint8_t flag = modeFlag(modeText);
        if (watchdogTripped) {
            reject("WATCHDOG");
            return;
        }
        if (extra != nullptr || !parseLong(pinText, pin) || pin < 0 ||
            pin > kMaximumPin || !parseLong(valueText, value) || flag == 0 ||
            allowedModes[pin] != flag) {
            reject("OUTPUT_NOT_ALLOWED");
            return;
        }
        if (flag == kDigital) {
            if (value != LOW && value != HIGH) {
                reject("DIGITAL_RANGE");
                return;
            }
            digitalWrite(static_cast<uint8_t>(pin), static_cast<uint8_t>(value));
        } else if (flag == kPwm) {
            if (!isPwmPin(static_cast<int>(pin)) || value < 0 || value > 255) {
                reject("PWM_RANGE");
                return;
            }
            analogWrite(static_cast<uint8_t>(pin), static_cast<int>(value));
        } else {
            if (value < 0 || value > 180) {
                reject("SERVO_RANGE");
                return;
            }
            if (!servoAttached[pin]) {
                servoOutputs[pin].attach(static_cast<uint8_t>(pin));
                servoAttached[pin] = true;
            }
            servoOutputs[pin].write(static_cast<int>(value));
        }
        Serial.print(F("OK SET "));
        Serial.println(pin);
        return;
    }

    reject("UNKNOWN_COMMAND");
}

void checkWatchdog() {
    if (!watchdogTripped && millis() - lastHeartbeat > watchdogMs) {
        // The board, not the host process, forces outputs safe after lost heartbeats.
        allOutputsLow();
        watchdogTripped = true;
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    for (const PinCapability& capability : kFirmwareAllowlist) {
        pinMode(capability.pin, OUTPUT);
        digitalWrite(capability.pin, LOW);
    }
    lastHeartbeat = millis();
}

void loop() {
    checkWatchdog();
    while (Serial.available() > 0) {
        const char character = static_cast<char>(Serial.read());
        if (character == '\r') continue;
        if (discardingLine) {
            if (character == '\n') discardingLine = false;
            continue;
        }
        if (character == '\n') {
            inputLine[inputLength] = '\0';
            processLine(inputLine);
            inputLength = 0;
        } else if (static_cast<size_t>(inputLength) + 1U < sizeof(inputLine)) {
            inputLine[inputLength++] = character;
        } else {
            inputLength = 0;
            discardingLine = true;
            reject("LINE_TOO_LONG");
        }
    }
}
