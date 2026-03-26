#include "CfdpReceiver.h"
#include "UartTransport.h"
#include <iostream>
#include <cstdlib>
#include <poll.h>

// ─────────────────────────────────────────────────────────────────────────────
//  receiver_proc <device> <state_dir> <recv_dir>
//
//  Exemple :
//    ./receiver_proc /tmp/tty_sat /tmp/cfdp_state /tmp/cfdp_recv
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: receiver_proc <device> <state_dir> <recv_dir>\n";
        return 1;
    }
    const std::string device    = argv[1];
    const std::string state_dir = argv[2];
    const std::string recv_dir  = argv[3];

    try {
        UartTransport uart(device);
        CfdpReceiver  receiver(state_dir, recv_dir, uart);

        receiver.on_boot(); // reprend si journal existant (reboot recovery)

        std::cout << "[receiver] en attente de PDUs sur " << device << "\n";

        bool was_active = false;
        while (true) 
        {
            auto pdu = uart.recv(); 
            receiver.on_pdu_received(pdu);

            if (receiver.transfer_active()) 
            {
                was_active = true;
            } 
            else if (was_active) 
            {
                // Transfert terminé : on reste actif pour renvoyer l'ACK si le sender
                // retransmet (son ACK a pu être perdu). On sort dès qu'il n'envoie plus rien.
                std::cout << "[receiver] transfert terminé, attente confirmation sender\n";
                struct pollfd rpfd{ uart.fd(), POLLIN, 0 };
                while (::poll(&rpfd, 1, 1500) > 0) {
                    auto rpdu = uart.recv();
                    receiver.on_pdu_received(rpdu);
                }
                break;
            }
        }
    } catch (const std::exception& e) 
    {
        std::cerr << "[receiver] erreur fatale : " << e.what() << "\n";
        return 1;
    }

    return 0;
}
