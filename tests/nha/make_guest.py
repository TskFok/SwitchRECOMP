# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Assemble the original AArch64 fixture and wrap its .text bytes in an NRO0 header."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


def make_guest(output, compiler, source=Path(__file__).with_name("guest.S")):
    with tempfile.TemporaryDirectory() as temp:
        obj = Path(temp) / "guest.o"
        command = [compiler]
        if "clang" in Path(compiler).name:
            command += ["--target=aarch64-none-elf", "-c"]
        subprocess.run([*command, "-o", str(obj), str(source)], check=True)
        elf = obj.read_bytes()

    if elf[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", elf, 18)[0] != 183:
        raise ValueError("Expected a little-endian AArch64 ELF object")
    off = struct.unpack_from("<Q", elf, 40)[0]
    entsize, count, strindex = struct.unpack_from("<3H", elf, 58)
    sections = [struct.unpack_from("<II4QII2Q", elf, off + i * entsize) for i in range(count)]
    names = sections[strindex][4]
    text = next(header for header in sections if elf.startswith(b".text\0", names + header[0]))
    image = bytearray(elf[text[4]:text[4] + text[5]])
    size = (len(image) + 0xfff) & ~0xfff
    image.extend(bytes(size - len(image)))
    struct.pack_into("<4s9I", image, 0x10, b"NRO0", 0, size, 0, 0, size, size, 0, size, 0)
    image[0x40:0x60] = hashlib.sha256(source.read_bytes()).digest()
    output.write_bytes(image)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--assembler", default="aarch64-linux-gnu-as")
    args = parser.parse_args()
    make_guest(args.output, args.assembler)
