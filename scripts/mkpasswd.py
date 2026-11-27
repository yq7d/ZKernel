#!/usr/bin/env python3
# prints a ZOS password hash: mkpasswd.py <password> [salt]
# same scheme as hash_password in user/lib/users.c
import hashlib, os, sys

def zhash(password, salt):
    h = hashlib.sha256((salt + password).encode()).digest()
    for _ in range(4999):
        h = hashlib.sha256(h + salt.encode() + password.encode()).digest()
    return '$5z$%s$%s' % (salt, h.hex())

if len(sys.argv) < 2:
    sys.exit('usage: mkpasswd.py <password> [salt]')
print(zhash(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.urandom(8).hex()))
