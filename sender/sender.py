#!/usr/bin/env python3
"""CFDP Sender — Python"""

import os
import sys
import struct
import zlib
import select
import termios
import tty
import random
import subprocess
import time
import atexit
# ─── PDU types ────────────────────────────────────────────────────────────────
PDU_METADATA     = 0x01
PDU_DATA         = 0x02
PDU_EOF          = 0x03
PDU_NAK          = 0x04
PDU_ACK          = 0x05
PDU_RESUME_QUERY = 0x06
PDU_RESUME_INFO  = 0x07

# ─── Paramètres (doivent correspondre au receiver C++) ────────────────────────
CHUNK_SIZE   = 1024 * 1024  # 1 MB
PACKET_SIZE  = 2048           # 4 KB
TIMEOUT_S    = 0.5
LOSS_RATE    = 0.02          # 2% perte
CORRUPT_RATE = 0.01           # 1% corruption

# ─── Formats structs (packed little-endian, matches __attribute__((packed))) ──
# MetadataPdu : transaction_id(4) file_size(8) chunk_size(4) packet_size(4)
#               total_chunks(4) checksum(4) filename(256)  → 284 octets
FMT_METADATA  = '<IQIIII256s'

# DataPdu header : transaction_id(4) chunk_index(4) packet_index(4)
#                  total_packets(4) data_len(4) crc8(1)    → 21 octets
FMT_DATA_HDR  = '<IIIIIB'

# EofPdu : transaction_id(4) checksum(4)                   → 8 octets
FMT_EOF       = '<II'

# AckPdu : transaction_id(4) chunk_index(4)                → 8 octets
# chunk_index == 0xFFFFFFFF → ACK final (EOF + CRC32 OK)
FMT_ACK       = '<II'
ACK_FINAL     = 0xFFFFFFFF

# NakPdu : transaction_id(4) count(4) [chunk(4) first(4) last(4)]×count
FMT_NAK_HDR   = '<II'
FMT_NAK_RANGE = '<III'

# ResumeQueryPdu : transaction_id(4)                           → 4 octets
# ResumeInfoPdu  : transaction_id(4) first_missing_chunk(4)   → 8 octets
FMT_RESUME_QUERY = '<I'
FMT_RESUME_INFO  = '<II'

# ─── CRC ──────────────────────────────────────────────────────────────────────

def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc

def crc32_file(path: str) -> int:
    crc = 0
    with open(path, 'rb') as f:
        while buf := f.read(65536):
            crc = zlib.crc32(buf, crc)
    return crc & 0xFFFFFFFF

# ─── Transport ────────────────────────────────────────────────────────────────

class Transport:
    def __init__(self, device: str,
                 loss_rate: float = 0.0,
                 corrupt_rate: float = 0.0):
        self.fd           = os.open(device, os.O_RDWR | os.O_NOCTTY)
        self.loss_rate    = loss_rate
        self.corrupt_rate = corrupt_rate
        tty.setraw(self.fd, termios.TCSANOW)

    def send(self, pdu: bytes):
        if random.random() < self.loss_rate:
            return  # paquet silencieusement perdu
        if random.random() < self.corrupt_rate:
            pdu = bytearray(pdu)
            pdu[random.randrange(len(pdu))] ^= 0xFF
            pdu = bytes(pdu)
        frame = struct.pack('<I', len(pdu)) + pdu
        os.write(self.fd, frame)

    def recv(self, timeout_s: float) -> bytes | None:
        r, _, _ = select.select([self.fd], [], [], timeout_s)
        if not r:
            return None
        n = struct.unpack('<I', self._read_exact(4))[0]
        return self._read_exact(n)

    def _read_exact(self, n: int) -> bytes:
        buf = b''
        while len(buf) < n:
            chunk = os.read(self.fd, n - len(buf))
            if not chunk:
                raise RuntimeError('Transport: connexion fermée')
            buf += chunk
        return buf

    def close(self):
        os.close(self.fd)

# ─── Sender ───────────────────────────────────────────────────────────────────

class CfdpSender:
    def __init__(self, device: str,
                 chunk_size: int = CHUNK_SIZE,
                 packet_size: int = PACKET_SIZE,
                 loss_rate: float = LOSS_RATE,
                 corrupt_rate: float = CORRUPT_RATE):
        self.t           = Transport(device, loss_rate, corrupt_rate)
        self.chunk_size  = chunk_size
        self.packet_size = packet_size
        self.tid         = 1

    def send(self, filepath: str, dest_filename: str):
        file_size    = os.path.getsize(filepath)
        total_chunks = (file_size + self.chunk_size - 1) // self.chunk_size
        checksum     = crc32_file(filepath)
        ack_sz       = 1 + struct.calcsize(FMT_ACK)

        print(f'[sender] {filepath} → {dest_filename} '
              f'({file_size} bytes, {total_chunks} chunks)')

        self._send_metadata(dest_filename, file_size, total_chunks, checksum)
        first_ci = self._query_resume(total_chunks, checksum)

        for ci in range(first_ci, total_chunks):
            self._send_one_chunk(filepath, file_size, ci, total_chunks)
            self._send_eof(checksum)

            # Attendre ACK(ci) ou NAK — ne pas passer au chunk suivant avant
            while True:
                pdu = self.t.recv(TIMEOUT_S)
                if pdu is None:
                    print(f'[sender] timeout chunk {ci + 1}/{total_chunks} → retransmission')
                    self._send_metadata(dest_filename, file_size, total_chunks, checksum)
                    self._send_eof(checksum)
                    continue

                batch = [pdu]
                while True:
                    p = self.t.recv(0)
                    if p is None:
                        break
                    batch.append(p)

                # ACK final : transfert entièrement validé (CRC32 OK)
                if any(p[0] == PDU_ACK and len(p) >= ack_sz
                       and struct.unpack_from(FMT_ACK, p, 1)[1] == ACK_FINAL
                       for p in batch):
                    print('[sender] ACK final → transfert terminé')
                    self.t.close()
                    return

                # ACK(ci) : ce chunk est validé → passer au suivant
                if any(p[0] == PDU_ACK and len(p) >= ack_sz
                       and struct.unpack_from(FMT_ACK, p, 1)[1] == ci
                       for p in batch):
                    print(f'[sender] chunk {ci + 1}/{total_chunks} ✓')
                    break

                # NAK : retransmettre les paquets manquants du chunk courant
                latest_nak = next((p for p in reversed(batch) if p[0] == PDU_NAK), None)
                if latest_nak is not None:
                    self._on_nak(latest_nak[1:], filepath, file_size)
                    self._send_eof(checksum)

        # Tous les chunks ACK'd. Attendre ACK final (CRC32 du fichier complet).
        while True:
            pdu = self.t.recv(TIMEOUT_S)
            if pdu is None:
                self._send_eof(checksum)
                continue
            batch = [pdu]
            while True:
                p = self.t.recv(0)
                if p is None:
                    break
                batch.append(p)
            if any(p[0] == PDU_ACK and len(p) >= ack_sz
                   and struct.unpack_from(FMT_ACK, p, 1)[1] == ACK_FINAL
                   for p in batch):
                print('[sender] ACK final → transfert terminé')
                break

        self.t.close()

    # ── ResumeQuery ───────────────────────────────────────────────────────────

    def _query_resume(self, total_chunks: int, checksum: int) -> int:
        """Envoie ResumeQuery au receiver, attend ResumeInfo.
        Retourne le premier chunk manquant (0 = départ de zéro, total_chunks = tout reçu)."""
        query    = bytes([PDU_RESUME_QUERY]) + struct.pack(FMT_RESUME_QUERY, self.tid)
        info_sz  = 1 + struct.calcsize(FMT_RESUME_INFO)

        for attempt in range(3):
            self.t.send(query)
            pdu = self.t.recv(TIMEOUT_S)
            if pdu is None:
                print(f'[sender] ResumeQuery timeout (tentative {attempt + 1}/3)')
                continue
            if len(pdu) < info_sz or pdu[0] != PDU_RESUME_INFO:
                continue
            tid, first_missing = struct.unpack_from(FMT_RESUME_INFO, pdu, 1)
            if tid != self.tid:
                continue
            if first_missing == 0xFFFFFFFF:
                # Receiver a tout reçu, son ACK final a dû se perdre → renvoyer EOF
                print('[sender] ResumeInfo: tout reçu → renvoi EOF')
                self._send_eof(checksum)
                return total_chunks   # skip la boucle chunks, tombe dans l'attente ACK final
            print(f'[sender] ResumeInfo: reprise depuis chunk {first_missing}/{total_chunks}')
            return first_missing

        # Pas de réponse → nouveau transfert depuis le début
        print('[sender] ResumeInfo: pas de réponse → départ de zéro')
        return 0

    # ── PDU builders ─────────────────────────────────────────────────────────

    def _send_metadata(self, dest_filename, file_size, total_chunks, checksum):
        fname = dest_filename.encode('ascii')[:255].ljust(256, b'\x00')
        payload = struct.pack(FMT_METADATA,
                              self.tid, file_size,
                              self.chunk_size, self.packet_size,
                              total_chunks, checksum, fname)
        self.t.send(bytes([PDU_METADATA]) + payload)

    def _send_one_chunk(self, filepath, file_size, ci, total_chunks):
        chunk_off  = ci * self.chunk_size
        chunk_sz   = min(file_size - chunk_off, self.chunk_size)
        total_pkts = (chunk_sz + self.packet_size - 1) // self.packet_size
        with open(filepath, 'rb') as f:
            for pi in range(total_pkts):
                self._send_packet(f, ci, pi, total_pkts, chunk_off, chunk_sz)

    def _send_packet(self, f, chunk_index, packet_index, total_pkts,
                     chunk_off, chunk_sz):
        pkt_off  = chunk_off + packet_index * self.packet_size
        data_len = min(chunk_sz - packet_index * self.packet_size, self.packet_size)

        f.seek(pkt_off)
        data = f.read(data_len)

        header = struct.pack(FMT_DATA_HDR,
                             self.tid, chunk_index, packet_index,
                             total_pkts, data_len, crc8(data))
        # Pad data à PACKET_SIZE pour correspondre à sizeof(DataPdu) côté C++
        padded = data + b'\x00' * (self.packet_size - data_len)
        self.t.send(bytes([PDU_DATA]) + header + padded)

    def _send_eof(self, checksum):
        self.t.send(bytes([PDU_EOF]) + struct.pack(FMT_EOF, self.tid, checksum))

    # ── NAK handler ──────────────────────────────────────────────────────────

    def _on_nak(self, payload, filepath, file_size):
        _tid, count = struct.unpack_from(FMT_NAK_HDR, payload, 0)
        print(f'[sender] NAK missing={count}')

        with open(filepath, 'rb') as f:
            for i in range(count):
                ci, first, last = struct.unpack_from(FMT_NAK_RANGE, payload, 8 + i * 12)
                chunk_off  = ci * self.chunk_size
                chunk_sz   = min(file_size - chunk_off, self.chunk_size)
                total_pkts = (chunk_sz + self.packet_size - 1) // self.packet_size
                for pi in range(first, last + 1):
                    self._send_packet(f, ci, pi, total_pkts, chunk_off, chunk_sz)

# ─── Setup loopback (mode sans device) ───────────────────────────────────────

_ROOT          = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_RECEIVER_PROC = os.path.join(_ROOT, 'build', 'receiver_proc')
_TTY_GND       = '/tmp/tty_ground'
_TTY_SAT       = '/tmp/tty_sat'
_STATE_DIR     = '/tmp/cfdp_state'
_RECV_DIR      = '/tmp/cfdp_recv'

def setup_loopback() -> str:
    """Lance socat + receiver_proc sur une paire PTY virtuelle.
    Retourne le device côté sender (/tmp/tty_ground)."""

    if not os.path.exists(_RECEIVER_PROC):
        raise RuntimeError(f'receiver_proc introuvable : {_RECEIVER_PROC}\n'
                           f'(lancer cmake --build build/ depuis {_ROOT})')

    os.makedirs(_STATE_DIR, exist_ok=True)
    os.makedirs(_RECV_DIR,  exist_ok=True)
    for p in [_TTY_GND, _TTY_SAT]:
        try: os.unlink(p)
        except FileNotFoundError: pass

    socat = subprocess.Popen(
        ['socat', '-d', '-d',
         f'pty,raw,echo=0,link={_TTY_GND}',
         f'pty,raw,echo=0,link={_TTY_SAT}'],
        stderr=subprocess.DEVNULL,
    )

    for _ in range(100):
        if os.path.exists(_TTY_GND) and os.path.exists(_TTY_SAT):
            break
        time.sleep(0.05)
    else:
        socat.kill()
        raise RuntimeError('socat : PTYs non créés après 5 s')

    print(f'[setup] PTY : {_TTY_GND} ↔ {_TTY_SAT}')

    receiver = subprocess.Popen([_RECEIVER_PROC, _TTY_SAT, _STATE_DIR, _RECV_DIR])
    time.sleep(0.1)  # laisse le receiver ouvrir le port

    def _cleanup():
        receiver.terminate()
        socat.kill()
        receiver.wait()
        socat.wait()

    atexit.register(_cleanup)
    return _TTY_GND

# ─── Entrée ───────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    # Mode auto  : sender.py <filepath> <dest_filename>
    # Mode manuel: sender.py <device>   <filepath> <dest_filename>
    if len(sys.argv) == 3:
        device   = setup_loopback()
        filepath = sys.argv[1]
        dest     = sys.argv[2]
    elif len(sys.argv) == 4:
        device   = sys.argv[1]
        filepath = sys.argv[2]
        dest     = sys.argv[3]
    else:
        print(f'Usage: {sys.argv[0]} [<device>] <filepath> <dest_filename>')
        sys.exit(1)

    try:
        CfdpSender(device).send(filepath, dest)
    except Exception as e:
        print(f'[sender] erreur fatale : {e}', file=sys.stderr)
        sys.exit(1)
