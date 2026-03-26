#pragma once
#include "Transport.h"
#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  UartTransport
//
//  Implémente ICfdpTransport sur un fd UART / PTY.
//  Protocole de framing (stream → paquets) :
//      [length : uint32_t LE][payload : length octets]
// ─────────────────────────────────────────────────────────────────────────────

class UartTransport : public ICfdpTransport {
public:
    explicit UartTransport(const std::string& device);
    ~UartTransport();

    // ICfdpTransport : sérialise [len][pdu] puis write()
    void send(const std::vector<uint8_t>& pdu) override;

    // Lecture bloquante d'un PDU complet
    std::vector<uint8_t> recv();

    int fd() const { return fd_; }

private:
    int  fd_;
    void read_exact(void* buf, size_t n);
};
