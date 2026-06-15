#!/usr/bin/env python3
"""gen-bank-secret.py <out-path> — idempotentni generace master secretu.

Pokud soubor neexistuje, vytvori 32 nahodnych bajtu + vypise hlasite varovani.
Pokud existuje, NIKDY neprepise (idempotentni). Volaji Makefile i CMake; oba
buildy tak maji secret bez rucniho kroku, na vsech platformach (jen python3).
"""
import os
import secrets
import sys

SECRET_LEN = 32


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen-bank-secret.py <out-path>")
    out = sys.argv[1]
    if os.path.exists(out):
        return  # idempotentni — nikdy neprepisuj
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(secrets.token_bytes(SECRET_LEN))
    sys.stderr.write(
        "\n*** VYGENEROVAN NOVY bank secret: %s ***\n"
        "    ZAZALOHUJ HO. Drive vydane licensed banky s nim nepujdou otevrit;\n"
        "    produkcni secret je dlouhozijici aktivum (neztrat, nepregeneruj).\n\n"
        % out)


if __name__ == "__main__":
    main()
