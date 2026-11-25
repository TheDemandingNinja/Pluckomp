"""Readers for the Switch container formats the extractor needs: NSP, XCI, NCA, PFS0/HFS0, RomFS, tickets and CNMT.

Layouts follow the Switchbrew wiki (NCA, XCI, Ticket, CNMT, Filesystem services pages). Standard library plus the
`cryptography` package for AES.
"""


class FormatError(Exception):
    """The input is not the format the reader expected, or uses a feature it does not support."""
