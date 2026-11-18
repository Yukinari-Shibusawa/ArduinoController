#pragma once

#include "SerialConnection.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace arduino {

enum class OutputMode {
    Digital,
    Pwm,
    Servo
};

struct OutputPin {
    std::string name;
    OutputMode mode;
    std::uint8_t pin;
};

struct GpioConfiguration {
    /** Serial device connected to the Arduino, e.g. /dev/ttyACM0. */
    std::string port;
    int baudRate{115200};
    /** Firmware forces all allowed outputs LOW when this heartbeat expires. */
    std::chrono::milliseconds watchdogTimeout{2000};
    /** Logical names used by applications map to physical pins and electrical modes. */
    std::map<std::string, OutputPin> outputs;

    /** Parse the line-oriented PORT/BAUD/WATCHDOG_MS/OUTPUT config format. */
    static GpioConfiguration load(std::istream& input, const std::string& source);
    /** Reject unsupported pins, duplicate pins, and incompatible Uno timers before connecting. */
    void validate() const;
};

/**
 * Controls configured Arduino GPIO outputs over the ArduinoController line
 * protocol. The firmware must be loaded from firmware/ArduinoController.ino.
 */
class GpioController {
public:
    explicit GpioController(GpioConfiguration configuration);
    ~GpioController();

    GpioController(const GpioController&) = delete;
    GpioController& operator=(const GpioController&) = delete;

    void connect();
    /** Stop the heartbeat worker and close serial; firmware watchdog remains the fallback. */
    void disconnect() noexcept;
    bool isConnected() const noexcept;

    /**
     * Set a configured output: DIGITAL accepts 0/1, PWM accepts 0..255,
     * and SERVO accepts 0..180 degrees. Firmware must acknowledge the command.
     */
    void setOutput(const std::string& name, int value);
    /** Drive all outputs LOW and verify the firmware acknowledgement. */
    void stopAll();
    /** Rethrow a heartbeat/communication failure detected by the worker. */
    void checkHealth() const;

private:
    void sendAndExpect(const std::string& command,
                       const std::string& expected,
                       std::chrono::milliseconds timeout);
    void heartbeatLoop() noexcept;
    const OutputPin& outputFor(const std::string& name) const;

    GpioConfiguration configuration_;
    std::unique_ptr<SerialConnection> connection_;
    mutable std::mutex ioMutex_;
    mutable std::mutex failureMutex_;
    std::mutex heartbeatWaitMutex_;
    std::condition_variable heartbeatCondition_;
    std::exception_ptr heartbeatFailure_;
    std::atomic<bool> stopping_{false};
    std::thread heartbeatThread_;
};

} // namespace arduino
