import getpass
import hashlib
import os
from Crypto.Cipher import AES
from Crypto.Util.Padding import pad

def encrypt_vault():
    pin = getpass.getpass("PIN: ")
    key = hashlib.sha256(pin.encode()).digest()
    iv = os.urandom(16)

    with open("vault.txt", "rb") as f:
        data = f.read()

    cipher = AES.new(key, AES.MODE_CBC, iv)
    enc_data = cipher.encrypt(pad(data, AES.block_size))

    with open("vault.enc", "wb") as f:
        f.write(iv)
        f.write(enc_data)

if __name__ == "__main__":
    encrypt_vault()