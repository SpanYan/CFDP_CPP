#pragma once
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  ICfdpTransport  —  interface d'envoi PDU
//
//  À implémenter côté application pour brancher le protocole sur le
//  transport réel (UART, socket, canal simulé, etc.).
//
//  Usage :
//    class MyUartTransport : public ICfdpTransport {
//    public:
//        void send(const std::vector<uint8_t>& pdu) override { uart_write(pdu); }
//    };
//    CfdpSender sender(chunk_sz, pkt_sz, my_uart_transport);
// ─────────────────────────────────────────────────────────────────────────────

class ICfdpTransport {
public:
    virtual ~ICfdpTransport() = default;
    virtual void send(const std::vector<uint8_t>& pdu) = 0;
};
