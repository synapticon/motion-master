"""Read FoE error packets from a pcap file of EtherCAT frames.

The server does not show what a device sends in an FoE error packet beyond its error code, because
SOEM keeps only the code. A test that needs the text of the packet records the frames with a
capture and reads them here.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

_MAGIC = {0xA1B2C3D4: "<", 0xA1B23C4D: "<", 0xD4C3B2A1: ">", 0x4D3CB2A1: ">"}
_LINKTYPE_ETHERNET = 1
_ETHERTYPE_ETHERCAT = 0x88A4
# The EtherCAT frame type that carries datagrams (ETG.1000.4).
_FRAME_TYPE_DATAGRAMS = 1
# The datagram command that reads one slave at its configured address. The master reads the mailbox
# of a slave with it.
_FPRD = 4
_MAILBOX_HEADER_BYTES = 6
_MAILBOX_TYPE_FOE = 4
_FOE_OPCODE_ERROR = 5
# Opcode, a reserved byte, and the error code.
_FOE_ERROR_HEADER_BYTES = 6


@dataclass(frozen=True)
class FoeError:
    """One FoE error packet that a slave sent, as it came back to the master."""

    station_address: int
    code: int
    text: bytes


def frames(pcap: bytes) -> list[bytes]:
    """Return the Ethernet frames of a pcap file, in the order they were recorded."""
    if len(pcap) < 24:
        raise ValueError("the file is shorter than a pcap header")
    (magic,) = struct.unpack_from("<I", pcap, 0)
    order = _MAGIC.get(magic)
    if order is None:
        raise ValueError(f"not a pcap file: magic 0x{magic:08X}")
    (linktype,) = struct.unpack_from(f"{order}I", pcap, 20)
    if linktype != _LINKTYPE_ETHERNET:
        raise ValueError(f"the link type is {linktype}, not Ethernet")
    result = []
    offset = 24
    while offset + 16 <= len(pcap):
        (captured,) = struct.unpack_from(f"{order}I", pcap, offset + 8)
        offset += 16
        result.append(pcap[offset : offset + captured])
        offset += captured
    return result


def foe_errors(pcap: bytes) -> list[FoeError]:
    """Return every FoE error packet that a slave sent in the frames of a pcap file.

    Only frames on their way back to the master count, because only those carry what a slave
    wrote. Every slave sets the locally administered bit of the source MAC address in a frame it
    processed, which is how such a frame is told apart from the one the master sent.
    """
    result = []
    for frame in frames(pcap):
        if len(frame) < 16 or not frame[6] & 0x02:
            continue
        (ethertype,) = struct.unpack_from(">H", frame, 12)
        if ethertype != _ETHERTYPE_ETHERCAT:
            continue
        (header,) = struct.unpack_from("<H", frame, 14)
        if header >> 12 != _FRAME_TYPE_DATAGRAMS:
            continue
        result.extend(_foe_errors_in_datagrams(frame, 16, 16 + (header & 0x7FF)))
    return result


def _foe_errors_in_datagrams(frame: bytes, offset: int, end: int) -> list[FoeError]:
    result = []
    while offset + 12 <= min(end, len(frame)):
        command, _, station_address, _, length_and_flags, _ = struct.unpack_from(
            "<BBHHHH", frame, offset
        )
        length = length_and_flags & 0x7FF
        data = frame[offset + 10 : offset + 10 + length]
        (wkc,) = struct.unpack_from("<H", frame, offset + 10 + length)
        if command == _FPRD and wkc > 0:
            error = _foe_error(station_address, data)
            if error is not None:
                result.append(error)
        if not length_and_flags & 0x8000:
            break
        offset += 12 + length
    return result


def _foe_error(station_address: int, mailbox: bytes) -> FoeError | None:
    if len(mailbox) < _MAILBOX_HEADER_BYTES + _FOE_ERROR_HEADER_BYTES:
        return None
    (length,) = struct.unpack_from("<H", mailbox, 0)
    if mailbox[5] & 0x0F != _MAILBOX_TYPE_FOE or mailbox[6] != _FOE_OPCODE_ERROR:
        return None
    if length < _FOE_ERROR_HEADER_BYTES or _MAILBOX_HEADER_BYTES + length > len(mailbox):
        return None
    (code,) = struct.unpack_from("<I", mailbox, 8)
    text = mailbox[_MAILBOX_HEADER_BYTES + _FOE_ERROR_HEADER_BYTES : _MAILBOX_HEADER_BYTES + length]
    return FoeError(station_address, code, text)
