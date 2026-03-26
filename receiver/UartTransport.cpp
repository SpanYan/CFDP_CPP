#include "UartTransport.h"
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

UartTransport::UartTransport(const std::string& device) {
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0)
        throw std::runtime_error("UartTransport: cannot open " + device);

    // Mode raw : pas d'écho, pas de conversion CR/LF, pas de flow control
    struct termios tty{};
    ::tcgetattr(fd_, &tty);
    ::cfmakeraw(&tty);
    ::tcsetattr(fd_, TCSANOW, &tty);
}

UartTransport::~UartTransport() {
    if (fd_ >= 0) ::close(fd_);
}

void UartTransport::send(const std::vector<uint8_t>& pdu) {
    uint32_t len = static_cast<uint32_t>(pdu.size());
    ::write(fd_, &len, sizeof(len));
    ::write(fd_, pdu.data(), pdu.size());
}

void UartTransport::read_exact(void* buf, size_t n) {
    auto* p   = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd_, p + got, n - got);
        if (r <= 0) throw std::runtime_error("UartTransport: read error");
        got += static_cast<size_t>(r);
    }
}

std::vector<uint8_t> UartTransport::recv() {
    uint32_t len = 0;
    read_exact(&len, sizeof(len));
    std::vector<uint8_t> pdu(len);
    read_exact(pdu.data(), len);
    return pdu;
}
