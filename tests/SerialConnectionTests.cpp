#include "SerialConnection.hpp"
#include "GpioController.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void writeAll(int descriptor, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto result =
            ::write(descriptor, text.data() + offset, text.size() - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("Pseudo-terminal write failed");
        }
    }
}

void testLineRoundTripAndBufferedResponses() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    expect(master >= 0, "Could not create pseudo-terminal");
    try {
        expect(::grantpt(master) == 0, "Could not grant pseudo-terminal access");
        expect(::unlockpt(master) == 0, "Could not unlock pseudo-terminal");
        const char* slavePath = ::ptsname(master);
        expect(slavePath != nullptr, "Could not get pseudo-terminal path");

        arduino::SerialConnection connection(slavePath);
        connection.connect();
        expect(connection.isConnected(), "Connection should be open");
        connection.sendLine("PUMP ON");

        pollfd descriptor{master, POLLIN, 0};
        expect(::poll(&descriptor, 1, 1000) > 0,
               "Arduino emulator should receive the sent line");
        char command[32]{};
        const auto bytesRead = ::read(master, command, sizeof(command));
        expect(bytesRead > 0, "Could not read the sent command");
        expect(std::string(command, static_cast<std::size_t>(bytesRead)) == "PUMP ON\n",
               "Commands should be sent with a newline");

        writeAll(master, "OK PUMP ON\r\nREADY\n");
        expect(connection.readLine() == "OK PUMP ON",
               "The first response should be returned without CR");
        expect(connection.readLine() == "READY",
               "Buffered following response lines should be preserved");
        connection.disconnect();
        expect(!connection.isConnected(), "Connection should close");
    } catch (...) {
        ::close(master);
        throw;
    }
    ::close(master);
}

void testTimeoutAndInputValidation() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    expect(master >= 0, "Could not create pseudo-terminal");
    try {
        expect(::grantpt(master) == 0, "Could not grant pseudo-terminal access");
        expect(::unlockpt(master) == 0, "Could not unlock pseudo-terminal");
        const char* slavePath = ::ptsname(master);
        expect(slavePath != nullptr, "Could not get pseudo-terminal path");

        arduino::SerialConnection connection(slavePath);
        connection.connect();

        bool rejectedMultiline = false;
        try {
            connection.sendLine("ONE\nTWO");
        } catch (const std::invalid_argument&) {
            rejectedMultiline = true;
        }
        expect(rejectedMultiline, "Embedded line breaks must be rejected");

        bool timedOut = false;
        const auto started = std::chrono::steady_clock::now();
        try {
            connection.readLine(std::chrono::milliseconds(30));
        } catch (const std::runtime_error& error) {
            timedOut = std::string(error.what()).find("Timed out") != std::string::npos;
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(timedOut, "A missing response should report a timeout");
        expect(elapsed < std::chrono::seconds(1), "Read timeout should be bounded");
    } catch (...) {
        ::close(master);
        throw;
    }
    ::close(master);
}

void testRejectsOverlongResponse() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    expect(master >= 0, "Could not create pseudo-terminal");
    try {
        expect(::grantpt(master) == 0, "Could not grant pseudo-terminal access");
        expect(::unlockpt(master) == 0, "Could not unlock pseudo-terminal");
        const char* slavePath = ::ptsname(master);
        expect(slavePath != nullptr, "Could not get pseudo-terminal path");

        arduino::SerialConnection connection(slavePath);
        connection.connect();
        writeAll(master, std::string(4097, 'A') + "\n");

        bool rejected = false;
        try {
            connection.readLine();
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("exceeds 4096 bytes") !=
                       std::string::npos;
        }
        expect(rejected, "Overlong response lines should be rejected");
    } catch (...) {
        ::close(master);
        throw;
    }
    ::close(master);
}

void runGpioEmulator(int master,
                     std::atomic<bool>& stop,
                     std::mutex& commandsMutex,
                     std::vector<std::string>& commands,
                     bool rejectHeartbeat = false) {
    std::string line;
    while (!stop.load()) {
        pollfd descriptor{master, POLLIN, 0};
        const int pollResult = ::poll(&descriptor, 1, 20);
        if (pollResult <= 0) continue;
        char buffer[256];
        const auto count = ::read(master, buffer, sizeof(buffer));
        if (count <= 0) continue;
        for (ssize_t index = 0; index < count; ++index) {
            if (buffer[index] != '\n') {
                if (buffer[index] != '\r') line.push_back(buffer[index]);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(commandsMutex);
                commands.push_back(line);
            }
            std::string response;
            if (line == "HELLO") {
                response = "READY\n";
            } else if (line.rfind("WD ", 0) == 0) {
                response = "OK " + line + "\n";
            } else if (line.rfind("ALLOW ", 0) == 0) {
                std::istringstream input(line);
                std::string allow;
                int pin = 0;
                input >> allow >> pin;
                response = "OK ALLOW " + std::to_string(pin) + "\n";
            } else if (line == "STOP") {
                response = "OK STOP\n";
            } else if (line == "HB") {
                response = rejectHeartbeat ? "ERR WATCHDOG\n" : "OK HB\n";
            } else if (line.rfind("SET ", 0) == 0) {
                std::istringstream input(line);
                std::string set;
                int pin = 0;
                input >> set >> pin;
                response = "OK SET " + std::to_string(pin) + "\n";
            } else {
                response = "ERR UNKNOWN_COMMAND\n";
            }
            writeAll(master, response);
            line.clear();
        }
    }
}

void testGpioConfigurationAndCommands() {
    std::istringstream input(
        "PORT /dev/ttyACM0\n"
        "BAUD 115200\n"
        "WATCHDOG_MS 600\n"
        "OUTPUT indicator DIGITAL 8\n"
        "OUTPUT motor PWM 3\n"
        "OUTPUT servo SERVO 10\n");
    const auto config = arduino::GpioConfiguration::load(input, "test.conf");
    expect(config.outputs.size() == 3, "All configured outputs should be loaded");
    expect(config.watchdogTimeout == std::chrono::milliseconds(600),
           "Configured watchdog timeout should be preserved");

    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    expect(master >= 0, "Could not create pseudo-terminal");
    try {
        expect(::grantpt(master) == 0, "Could not grant pseudo-terminal access");
        expect(::unlockpt(master) == 0, "Could not unlock pseudo-terminal");
        const char* slavePath = ::ptsname(master);
        expect(slavePath != nullptr, "Could not get pseudo-terminal path");

        auto activeConfig = config;
        activeConfig.port = slavePath;
        std::atomic<bool> stop{false};
        std::mutex commandsMutex;
        std::vector<std::string> commands;
        std::thread emulator(runGpioEmulator, master, std::ref(stop),
                             std::ref(commandsMutex), std::ref(commands), false);

        std::exception_ptr testFailure;
        try {
            arduino::GpioController controller(activeConfig);
            controller.connect();
            bool rejectedValue = false;
            try {
                controller.setOutput("indicator", 2);
            } catch (const std::out_of_range&) {
                rejectedValue = true;
            }
            expect(rejectedValue, "Digital output values outside LOW/HIGH must be rejected");

            bool rejectedName = false;
            try {
                controller.setOutput("not_configured", 1);
            } catch (const std::invalid_argument&) {
                rejectedName = true;
            }
            expect(rejectedName, "Unconfigured output names must be rejected");

            controller.setOutput("indicator", 1);
            controller.setOutput("motor", 127);
            controller.setOutput("servo", 90);
            controller.stopAll();
            controller.checkHealth();
            controller.disconnect();
        } catch (...) {
            testFailure = std::current_exception();
        }
        stop.store(true);
        emulator.join();
        ::close(master);
        master = -1;
        if (testFailure) std::rethrow_exception(testFailure);

        std::lock_guard<std::mutex> lock(commandsMutex);
        expect(std::find(commands.begin(), commands.end(), "ALLOW 8 DIGITAL") !=
                   commands.end(),
               "Configured digital outputs should be allowlisted on the device");
        expect(std::find(commands.begin(), commands.end(), "SET 8 DIGITAL 1") !=
                   commands.end(),
               "Digital output command should be sent");
        expect(std::find(commands.begin(), commands.end(), "SET 3 PWM 127") !=
                   commands.end(),
               "PWM output command should be sent");
        expect(std::find(commands.begin(), commands.end(), "SET 10 SERVO 90") !=
                   commands.end(),
               "Servo output command should be sent");
        expect(std::find(commands.begin(), commands.end(), "STOP") != commands.end(),
               "Safe stop command should be sent");
    } catch (...) {
        if (master >= 0) ::close(master);
        throw;
    }
}

void testGpioConfigurationRejectsUnsupportedPins() {
    std::istringstream input(
        "PORT /dev/ttyACM0\n"
        "OUTPUT invalid PWM 8\n");
    bool rejected = false;
    try {
        (void)arduino::GpioConfiguration::load(input, "invalid.conf");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "Non-PWM Uno pins must not be configured for PWM");

    std::istringstream conflict(
        "PORT /dev/ttyACM0\n"
        "OUTPUT servo SERVO 8\n"
        "OUTPUT pwm PWM 9\n");
    rejected = false;
    try {
        (void)arduino::GpioConfiguration::load(conflict, "conflict.conf");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "Servo timer conflicts with PWM pins 9 and 10 must be rejected");
}

void testStopAllRemainsAvailableAfterHeartbeatFailure() {
    std::istringstream input(
        "PORT /dev/ttyACM0\n"
        "WATCHDOG_MS 600\n"
        "OUTPUT indicator DIGITAL 8\n");
    auto config = arduino::GpioConfiguration::load(input, "heartbeat.conf");

    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    expect(master >= 0, "Could not create pseudo-terminal");
    try {
        expect(::grantpt(master) == 0, "Could not grant pseudo-terminal access");
        expect(::unlockpt(master) == 0, "Could not unlock pseudo-terminal");
        const char* slavePath = ::ptsname(master);
        expect(slavePath != nullptr, "Could not get pseudo-terminal path");
        config.port = slavePath;

        std::atomic<bool> stop{false};
        std::mutex commandsMutex;
        std::vector<std::string> commands;
        std::thread emulator(runGpioEmulator, master, std::ref(stop),
                             std::ref(commandsMutex), std::ref(commands), true);
        std::exception_ptr testFailure;
        try {
            arduino::GpioController controller(config);
            controller.connect();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            bool heartbeatFailed = false;
            try {
                controller.checkHealth();
            } catch (const std::runtime_error& error) {
                heartbeatFailed =
                    std::string(error.what()).find("Arduino heartbeat failed") !=
                    std::string::npos;
            }
            expect(heartbeatFailed, "Heartbeat failures should be explicitly reported");
            controller.stopAll();
            controller.disconnect();
        } catch (...) {
            testFailure = std::current_exception();
        }
        stop.store(true);
        emulator.join();
        ::close(master);
        master = -1;
        if (testFailure) std::rethrow_exception(testFailure);

        std::lock_guard<std::mutex> lock(commandsMutex);
        expect(std::count(commands.begin(), commands.end(), "STOP") >= 2,
               "Safe stop should remain available after heartbeat failure");
    } catch (...) {
        if (master >= 0) ::close(master);
        throw;
    }
}

} // namespace

int main() {
    try {
        testLineRoundTripAndBufferedResponses();
        testTimeoutAndInputValidation();
        testRejectsOverlongResponse();
        testGpioConfigurationAndCommands();
        testGpioConfigurationRejectsUnsupportedPins();
        testStopAllRemainsAvailableAfterHeartbeatFailure();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "All ArduinoController tests passed\n";
    return EXIT_SUCCESS;
}
