#include "SerialConnection.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace arduino {
namespace {

constexpr std::size_t kMaximumLineLength = 4096;

speed_t toTermiosSpeed(int baudRate) {
    switch (baudRate) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:
        throw std::invalid_argument("Unsupported baud rate: " +
                                    std::to_string(baudRate));
    }
}

void validateTimeout(std::chrono::milliseconds timeout) {
    // poll() accepts an int timeout, so reject durations that cannot be represented.
    if (timeout.count() <= 0 ||
        timeout.count() > static_cast<long long>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Serial timeout must be a positive int-sized duration");
    }
}

int remainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(remaining > 0 ? remaining : 1);
}

} // namespace

SerialConnection::SerialConnection(std::string port, int baudRate)
    : port_(std::move(port)), baudRate_(baudRate) {
    if (port_.empty()) {
        throw std::invalid_argument("Serial port path must not be empty");
    }
    toTermiosSpeed(baudRate_);
}

SerialConnection::~SerialConnection() {
    disconnect();
}

void SerialConnection::connect() {
    if (isConnected()) return;

    const speed_t speed = toTermiosSpeed(baudRate_);
    fileDescriptor_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fileDescriptor_ < 0) {
        throw std::runtime_error("Cannot open serial port " + port_ + ": " +
                                 std::string(std::strerror(errno)));
    }

    termios settings{};
    if (tcgetattr(fileDescriptor_, &settings) != 0) {
        const std::string error = std::strerror(errno);
        disconnect();
        throw std::runtime_error("Cannot read serial port settings: " + error);
    }

    // Disable terminal processing; the device protocol is raw newline-delimited bytes.
    cfmakeraw(&settings);
    if (cfsetispeed(&settings, speed) != 0 || cfsetospeed(&settings, speed) != 0) {
        const std::string error = std::strerror(errno);
        disconnect();
        throw std::runtime_error("Cannot set serial baud rate: " + error);
    }
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= ~CSTOPB;
    settings.c_cflag &= ~PARENB;
#ifdef CRTSCTS
    settings.c_cflag &= ~CRTSCTS;
#endif
    settings.c_cflag &= ~CSIZE;
    settings.c_cflag |= CS8;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;

    if (tcsetattr(fileDescriptor_, TCSANOW, &settings) != 0) {
        const std::string error = std::strerror(errno);
        disconnect();
        throw std::runtime_error("Cannot configure serial port: " + error);
    }
    if (tcflush(fileDescriptor_, TCIOFLUSH) != 0) {
        const std::string error = std::strerror(errno);
        disconnect();
        throw std::runtime_error("Cannot flush serial port: " + error);
    }
    pendingLine_.clear();
}

void SerialConnection::disconnect() noexcept {
    if (fileDescriptor_ >= 0) {
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
    pendingLine_.clear();
}

bool SerialConnection::isConnected() const noexcept {
    return fileDescriptor_ >= 0;
}

void SerialConnection::sendLine(const std::string& line,
                                std::chrono::milliseconds timeout) {
    if (!isConnected()) {
        throw std::runtime_error("Serial connection is not connected");
    }
    if (line.empty() || line.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("Serial message must be one non-empty line");
    }
    validateTimeout(timeout);

    const std::string message = line + '\n';
    // Keep partial writes under one deadline instead of restarting the timeout per retry.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t written = 0;
    while (written < message.size()) {
        const auto result = ::write(fileDescriptor_, message.data() + written,
                                    message.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            const std::string error = std::strerror(errno);
            disconnect();
            throw std::runtime_error("Serial write failed: " + error);
        }

        pollfd descriptor{fileDescriptor_, POLLOUT, 0};
        const int pollResult =
            ::poll(&descriptor, 1, remainingMilliseconds(deadline));
        if (pollResult == 0) {
            disconnect();
            throw std::runtime_error("Timed out writing to serial device");
        }
        if (pollResult < 0) {
            if (errno == EINTR) continue;
            const std::string error = std::strerror(errno);
            disconnect();
            throw std::runtime_error("Serial write poll failed: " + error);
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            disconnect();
            throw std::runtime_error("Serial device disconnected during write");
        }
    }
}

std::string SerialConnection::readLine(std::chrono::milliseconds timeout) {
    if (!isConnected()) {
        throw std::runtime_error("Serial connection is not connected");
    }
    validateTimeout(timeout);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        // A previous read can buffer more than one response line.
        const auto newline = pendingLine_.find('\n');
        if (newline != std::string::npos) {
            if (newline > kMaximumLineLength) {
                pendingLine_.erase(0, newline + 1);
                throw std::runtime_error("Serial response line exceeds 4096 bytes");
            }
            std::string result = pendingLine_.substr(0, newline);
            pendingLine_.erase(0, newline + 1);
            if (!result.empty() && result.back() == '\r') result.pop_back();
            return result;
        }
        if (pendingLine_.size() > kMaximumLineLength) {
            pendingLine_.clear();
            throw std::runtime_error("Serial response line exceeds 4096 bytes");
        }

        pollfd descriptor{fileDescriptor_, POLLIN, 0};
        const int pollResult =
            ::poll(&descriptor, 1, remainingMilliseconds(deadline));
        if (pollResult == 0) {
            throw std::runtime_error("Timed out waiting for serial response");
        }
        if (pollResult < 0) {
            if (errno == EINTR) continue;
            const std::string error = std::strerror(errno);
            disconnect();
            throw std::runtime_error("Serial read poll failed: " + error);
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            disconnect();
            throw std::runtime_error("Serial device descriptor is invalid");
        }
        if ((descriptor.revents & POLLERR) != 0) {
            disconnect();
            throw std::runtime_error("Serial device reported a read error");
        }

        char buffer[256];
        const auto bytesRead = ::read(fileDescriptor_, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            const std::string error = std::strerror(errno);
            disconnect();
            throw std::runtime_error("Serial read failed: " + error);
        }
        if (bytesRead == 0) {
            if ((descriptor.revents & POLLHUP) != 0) {
                disconnect();
                throw std::runtime_error("Serial device disconnected during read");
            }
            continue;
        }
        pendingLine_.append(buffer, static_cast<std::size_t>(bytesRead));
    }
}

} // namespace arduino
