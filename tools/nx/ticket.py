"""Tickets (.tik): carry the title key that unlocks NCAs with a rights id."""

import struct

from . import FormatError

# Signature type -> size of signature plus padding (Switchbrew, Ticket).
SIGNATURE_SPAN = {
    0x10000: 0x200 + 0x3C, 0x10001: 0x100 + 0x3C, 0x10002: 0x3C + 0x40,
    0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40,
}


def read_ticket(data: bytes) -> tuple[bytes, bytes]:
    """Return (rights id, encrypted title key) of a common ticket."""
    sig_type = struct.unpack_from("<I", data, 0)[0]
    span = SIGNATURE_SPAN.get(sig_type)
    if span is None:
        raise FormatError(f"ticket has unknown signature type {sig_type:#x}")
    body = 4 + span
    if len(data) < body + 0x180:
        raise FormatError("ticket is truncated")
    key_type = data[body + 0x141]
    if key_type != 0:
        raise FormatError("personalized ticket: the title key is bound to the console; "
                          "dump the game again with a common ticket")
    return data[body + 0x160:body + 0x170], data[body + 0x40:body + 0x50]
