#pragma once

#include <chrono>
#include <string>

namespace arduino {

/**
 * Line-oriented USB serial connection for an Arduino running user-defined
 * firmware. This class only handles transport framing/timeouts; the caller
 * defines the command and acknowledgement protocol.
 */
class SerialConnection {
public:
    explicit SerialConnection(std::string port, int baudRate = 115200);
    ~SerialConnection();

    SerialConnection(const SerialConnection&) = delete;
    SerialConnection& operator=(const SerialConnection&) = delete;

    void connect();
    void disconnect() noexcept;
    bool isConnected() const noexcept;

    /**
     * Send one line, appending '\n'. A single deadline covers the complete
     * write, including retries while the device is not ready to accept data.
     */
    void sendLine(const std::string& line,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /**
     * Read one newline-terminated response line without its line ending.
     * Partial reads are retained for the next read; overlong lines are rejected.
     */
    std::string readLine(
        std::chrono::milliseconds timeout = std::chrono::seconds(5));

private:
    std::string port_;
    int baudRate_;
    int fileDescriptor_{-1};
    std::string pendingLine_;
};

} // namespace arduino
