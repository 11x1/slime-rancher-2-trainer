#!/usr/bin/env python3
"""Verify the built proxy against the real system version.dll.

Checks: x64 machine type, DLL characteristic, identical export name set,
identical ordinals, forwarders preserved, and that the proxy does NOT import
version.dll itself.  Exit code 0 = pass.

Usage:  python proxy/verify_exports.py path\\to\\version.dll
"""
import os
import sys

import pefile

SYS = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "version.dll")


def exports(pe):
    return {s.name.decode(): (s.ordinal, s.forwarder.decode() if s.forwarder else None)
            for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    built = pefile.PE(sys.argv[1])
    real = pefile.PE(SYS)
    ok = True

    machine = built.FILE_HEADER.Machine
    is_dll = bool(built.FILE_HEADER.Characteristics & 0x2000)
    print(f"machine=0x{machine:x} ({'x64' if machine == 0x8664 else 'NOT x64'}), dll={is_dll}")
    ok &= machine == 0x8664 and is_dll

    be, re_ = exports(built), exports(real)
    missing = sorted(set(re_) - set(be))
    extra = sorted(set(be) - set(re_))
    print(f"system exports={len(re_)}  proxy exports={len(be)}")
    if missing:
        print("MISSING:", ", ".join(missing)); ok = False
    if extra:
        print("EXTRA:", ", ".join(extra)); ok = False
    for name in sorted(set(be) & set(re_)):
        (bo, bf), (ro, rf) = be[name], re_[name]
        status = "ok"
        if bo != ro:
            status = f"ORDINAL MISMATCH (proxy {bo} vs system {ro})"; ok = False
        if rf and bf != rf:
            status = f"FORWARDER MISMATCH (proxy {bf} vs system {rf})"; ok = False
        print(f"  @{bo:<3} {name:<28} {('-> ' + bf) if bf else '':<30} {status}")

    imports = [e.dll.decode().lower() for e in getattr(built, "DIRECTORY_ENTRY_IMPORT", [])]
    print("imports:", ", ".join(imports))
    if "version.dll" in imports:
        print("ERROR: proxy imports version.dll (would import itself)"); ok = False
    if any(i.startswith(("msvcp", "vcruntime", "ucrtbase", "api-ms-win-crt")) for i in imports):
        print("WARNING: dynamic CRT import found (expected static /MT)"); ok = False

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
