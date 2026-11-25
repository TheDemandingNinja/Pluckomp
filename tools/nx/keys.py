"""prod.keys / title.keys parsing. Values never leave this module in text form: errors name the key, not its value."""

from pathlib import Path

from . import FormatError


class Keys:
    def __init__(self, values: dict[str, bytes]):
        self._values = values

    @classmethod
    def load(cls, path: Path) -> "Keys":
        values = {}
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            line = line.split("#", 1)[0].split(";", 1)[0].strip()
            if not line:
                continue
            name, sep, value = line.partition("=")
            if not sep:
                raise FormatError(f"{path.name} line {number}: expected 'name = hex'")
            try:
                values[name.strip().lower()] = bytes.fromhex(value.strip())
            except ValueError:
                raise FormatError(f"{path.name} line {number}: the value of {name.strip()} is not hex") from None
        return cls(values)

    def get(self, name: str, size: int = 16) -> bytes:
        value = self._values.get(name)
        if value is None:
            raise FormatError(f"the keys file has no {name}; dump the keys again from a console on a newer firmware")
        if len(value) != size:
            raise FormatError(f"{name} in the keys file is {len(value)} bytes, expected {size}")
        return value
