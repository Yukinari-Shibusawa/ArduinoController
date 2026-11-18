#include "GpioController.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace arduino {
namespace {

std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> tokens;
    std::string token;
    while (input >> token) tokens.push_back(token);
    return tokens;
}

long long parseInteger(const std::string& value,
                       const std::string& field,
                       const std::string& source,
                       std::size_t line) {
    std::size_t parsed = 0;
    long long result = 0;
    try {
        result = std::stoll(value, &parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(source + ":" + std::to_string(line) +
                                 ": invalid " + field + ": " + value);
    }
    if (parsed != value.size()) {
        throw std::runtime_error(source + ":" + std::to_string(line) +
                                 ": invalid " + field + ": " + value);
    }
    return result;
}

OutputMode parseMode(const std::string& value,
                     const std::string& source,
                     std::size_t line) {
    if (value == "DIGITAL") return OutputMode::Digital;
    if (value == "PWM") return OutputMode::Pwm;
    if (value == "SERVO") return OutputMode::Servo;
    throw std::runtime_error(source + ":" + std::to_string(line) +
                             ": output mode must be DIGITAL, PWM, or SERVO");
}

bool isUnoDigitalPin(int pin) {
    return (pin >= 2 && pin <= 13) || (pin >= 14 && pin <= 19);
}

bool isUnoPwmPin(int pin) {
    return pin == 3 || pin == 5 || pin == 6 || pin == 9 || pin == 10 || pin == 11;
}

std::string modeName(OutputMode mode) {
    switch (mode) {
    case OutputMode::Digital: return "DIGITAL";
    case OutputMode::Pwm: return "PWM";
    case OutputMode::Servo: return "SERVO";
    }
    throw std::invalid_argument("Invalid GPIO output mode");
}

} // namespace

GpioConfiguration GpioConfiguration::load(std::istream& input,
                                          const std::string& source) {
    GpioConfiguration configuration;
    bool portSet = false;
    bool baudSet = false;
    bool watchdogSet = false;
    std::string lineText;
    std::size_t lineNumber = 0;
    while (std::getline(input, lineText)) {
        ++lineNumber;
        const auto comment = lineText.find_first_of("#;");
        if (comment != std::string::npos) lineText.erase(comment);
        const auto tokens = tokenize(lineText);
        if (tokens.empty()) continue;

        if (tokens[0] == "PORT" && tokens.size() == 2 && !portSet) {
            configuration.port = tokens[1];
            portSet = true;
        } else if (tokens[0] == "BAUD" && tokens.size() == 2 && !baudSet) {
            const auto baud = parseInteger(tokens[1], "baud rate", source, lineNumber);
            if (baud > std::numeric_limits<int>::max() ||
                baud < std::numeric_limits<int>::min()) {
                throw std::runtime_error(source + ":" + std::to_string(lineNumber) +
                                         ": baud rate is out of range");
            }
            configuration.baudRate = static_cast<int>(baud);
            baudSet = true;
        } else if (tokens[0] == "WATCHDOG_MS" && tokens.size() == 2 && !watchdogSet) {
            const auto timeout =
                parseInteger(tokens[1], "watchdog timeout", source, lineNumber);
            if (timeout <= 0 ||
                timeout > std::numeric_limits<std::chrono::milliseconds::rep>::max()) {
                throw std::runtime_error(source + ":" + std::to_string(lineNumber) +
                                         ": watchdog timeout must be positive");
            }
            configuration.watchdogTimeout =
                std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout));
            watchdogSet = true;
        } else if (tokens[0] == "OUTPUT" && tokens.size() == 4) {
            const auto pin = parseInteger(tokens[3], "pin number", source, lineNumber);
            if (pin < 0 || pin > std::numeric_limits<std::uint8_t>::max()) {
                throw std::runtime_error(source + ":" + std::to_string(lineNumber) +
                                         ": pin number is out of range");
            }
            OutputPin output{tokens[1], parseMode(tokens[2], source, lineNumber),
                             static_cast<std::uint8_t>(pin)};
            if (!configuration.outputs.emplace(output.name, output).second) {
                throw std::runtime_error(source + ":" + std::to_string(lineNumber) +
                                         ": duplicate output name: " + output.name);
            }
        } else {
            throw std::runtime_error(source + ":" + std::to_string(lineNumber) +
                                     ": invalid or duplicate configuration entry");
        }
    }
    if (input.bad()) {
        throw std::runtime_error("Error reading Arduino configuration from " + source);
    }
    configuration.validate();
    return configuration;
}

void GpioConfiguration::validate() const {
    if (port.empty()) throw std::invalid_argument("Arduino port must not be empty");
    SerialConnection validationConnection(port, baudRate);
    (void)validationConnection;
    if (baudRate != 115200) {
        throw std::invalid_argument(
            "The Arduino Uno firmware currently requires a 115200 baud rate");
    }
    if (watchdogTimeout < std::chrono::milliseconds(500) ||
        watchdogTimeout > std::chrono::milliseconds(10000)) {
        throw std::invalid_argument("Watchdog timeout must be between 500 and 10000 ms");
    }
    if (outputs.empty()) {
        throw std::invalid_argument("At least one Arduino output must be configured");
    }

    std::map<std::uint8_t, OutputMode> pins;
    std::size_t servoCount = 0;
    for (const auto& entry : outputs) {
        const OutputPin& output = entry.second;
        if (entry.first.empty() || output.name != entry.first) {
            throw std::invalid_argument("Arduino output names must be non-empty and consistent");
        }
        switch (output.mode) {
        case OutputMode::Digital:
        case OutputMode::Pwm:
        case OutputMode::Servo:
            break;
        default:
            throw std::invalid_argument("Invalid Arduino output mode");
        }
        if (!isUnoDigitalPin(output.pin)) {
            throw std::invalid_argument("Arduino Uno output pin must be 2-13 or 14-19");
        }
        if (output.mode == OutputMode::Pwm && !isUnoPwmPin(output.pin)) {
            throw std::invalid_argument("Configured PWM pin is not PWM-capable on Arduino Uno");
        }
        const auto existing = pins.find(output.pin);
        if (existing != pins.end()) {
            throw std::invalid_argument("An Arduino pin cannot be configured more than once");
        }
        pins.emplace(output.pin, output.mode);
        if (output.mode == OutputMode::Servo) ++servoCount;
    }
    if (servoCount > 12) {
        throw std::invalid_argument("Arduino Uno supports at most 12 configured servo outputs");
    }
    if (servoCount > 0 &&
        (pins.find(9) != pins.end() || pins.find(10) != pins.end())) {
        const bool pwmOnTimer1 =
            (pins.find(9) != pins.end() && pins.at(9) == OutputMode::Pwm) ||
            (pins.find(10) != pins.end() && pins.at(10) == OutputMode::Pwm);
        if (pwmOnTimer1) {
            throw std::invalid_argument(
                "Servo outputs conflict with PWM on Arduino Uno pins 9 and 10");
        }
    }
}

GpioController::GpioController(GpioConfiguration configuration)
    : configuration_(std::move(configuration)),
      connection_(std::make_unique<SerialConnection>(configuration_.port,
                                                      configuration_.baudRate)) {
    configuration_.validate();
}

GpioController::~GpioController() {
    disconnect();
}

void GpioController::connect() {
    if (connection_->isConnected()) return;
    connection_->connect();
    try {
        std::lock_guard<std::mutex> lock(ioMutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool ready = false;
        // Opening USB serial can reset the board; retry HELLO until setup() has printed READY.
        while (std::chrono::steady_clock::now() < deadline) {
            connection_->sendLine("HELLO", std::chrono::seconds(1));
            try {
                if (connection_->readLine(std::chrono::milliseconds(300)) == "READY") {
                    ready = true;
                    break;
                }
            } catch (const std::runtime_error& error) {
                if (std::string(error.what()).find(
                        "Timed out waiting for serial response") == std::string::npos) {
                    throw;
                }
            }
        }
        if (!ready) {
            throw std::runtime_error(
                "Arduino firmware did not become ready within 8 seconds");
        }
        connection_->sendLine("WD " +
                              std::to_string(configuration_.watchdogTimeout.count()));
        if (connection_->readLine() !=
            "OK WD " + std::to_string(configuration_.watchdogTimeout.count())) {
            throw std::runtime_error("Arduino firmware rejected the watchdog timeout");
        }
        for (const auto& entry : configuration_.outputs) {
            const OutputPin& output = entry.second;
            const std::string command =
                "ALLOW " + std::to_string(output.pin) + " " + modeName(output.mode);
            connection_->sendLine(command);
            if (connection_->readLine() != "OK ALLOW " + std::to_string(output.pin)) {
                throw std::runtime_error("Arduino firmware rejected configured output " +
                                         output.name);
            }
        }
        // Never assume reset defaults: ask the firmware to acknowledge a known LOW state.
        connection_->sendLine("STOP");
        if (connection_->readLine() != "OK STOP") {
            throw std::runtime_error("Arduino firmware did not enter the safe output state");
        }
    } catch (...) {
        connection_->disconnect();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(failureMutex_);
        heartbeatFailure_ = nullptr;
    }
    stopping_.store(false);
    heartbeatThread_ = std::thread(&GpioController::heartbeatLoop, this);
}

void GpioController::disconnect() noexcept {
    stopping_.store(true);
    heartbeatCondition_.notify_all();
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
    connection_->disconnect();
}

bool GpioController::isConnected() const noexcept {
    return connection_->isConnected();
}

void GpioController::setOutput(const std::string& name, int value) {
    const OutputPin& output = outputFor(name);
    if (output.mode == OutputMode::Digital && value != 0 && value != 1) {
        throw std::out_of_range("Digital output must be 0 or 1");
    }
    if (output.mode == OutputMode::Pwm && (value < 0 || value > 255)) {
        throw std::out_of_range("PWM output must be between 0 and 255");
    }
    if (output.mode == OutputMode::Servo && (value < 0 || value > 180)) {
        throw std::out_of_range("Servo angle must be between 0 and 180 degrees");
    }

    checkHealth();
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (!connection_->isConnected()) {
        throw std::runtime_error("Arduino GPIO controller is not connected");
    }
    const std::string command = "SET " + std::to_string(output.pin) + " " +
                                modeName(output.mode) + " " + std::to_string(value);
    connection_->sendLine(command);
    const std::string response = connection_->readLine();
    if (response != "OK SET " + std::to_string(output.pin)) {
        throw std::runtime_error("Arduino firmware rejected output command: " + response);
    }
}

void GpioController::stopAll() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (!connection_->isConnected()) {
        throw std::runtime_error("Arduino GPIO controller is not connected");
    }
    connection_->sendLine("STOP");
    const std::string response = connection_->readLine();
    if (response != "OK STOP") {
        throw std::runtime_error("Arduino firmware did not confirm safe stop: " + response);
    }
}

void GpioController::checkHealth() const {
    std::lock_guard<std::mutex> lock(failureMutex_);
    if (heartbeatFailure_) {
        try {
            std::rethrow_exception(heartbeatFailure_);
        } catch (const std::exception& error) {
            throw std::runtime_error("Arduino heartbeat failed: " +
                                     std::string(error.what()));
        }
    }
}

void GpioController::sendAndExpect(const std::string& command,
                                   const std::string& expected,
                                   std::chrono::milliseconds timeout) {
    connection_->sendLine(command, timeout);
    const std::string response = connection_->readLine(timeout);
    if (response != expected) {
        throw std::runtime_error("Unexpected Arduino response to " + command + ": " +
                                 response);
    }
}

void GpioController::heartbeatLoop() noexcept {
    // Send several heartbeats per watchdog window so a delayed host thread has margin.
    const auto interval = configuration_.watchdogTimeout / 3;
    while (!stopping_.load()) {
        {
            std::unique_lock<std::mutex> waitLock(heartbeatWaitMutex_);
            heartbeatCondition_.wait_for(waitLock, interval,
                                         [this] { return stopping_.load(); });
        }
        if (stopping_.load()) break;
        try {
            std::lock_guard<std::mutex> lock(ioMutex_);
            if (!connection_->isConnected()) return;
            sendAndExpect("HB", "OK HB", configuration_.watchdogTimeout / 2);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(failureMutex_);
                heartbeatFailure_ = std::current_exception();
            }
            return;
        }
    }
}

const OutputPin& GpioController::outputFor(const std::string& name) const {
    const auto output = configuration_.outputs.find(name);
    if (output == configuration_.outputs.end()) {
        throw std::invalid_argument("Unconfigured Arduino output: " + name);
    }
    return output->second;
}

} // namespace arduino
