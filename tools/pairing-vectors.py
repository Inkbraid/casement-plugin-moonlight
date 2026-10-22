#!/usr/bin/env python3
# Derives tests/unit/pairing_vectors.h, the expected messages of a GameStream
# pairing with fixed inputs, independently of the plugin's C++ code: it
# follows moonlight-qt's NvPairingManager::pair()
# (app/backend/nvpairingmanager.cpp) step by step, using the Python
# cryptography package for AES, hashes and RSA signatures.
#
#   python3 tools/pairing-vectors.py > tests/unit/pairing_vectors.h
#
# The two identities below are throwaway test certificates; nothing trusts
# them.

import hashlib

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

CLIENT_CERT = """-----BEGIN CERTIFICATE-----
MIIDJzCCAg+gAwIBAgIUIGsz0rkBWpxPWATlzjwFA3vrRCEwDQYJKoZIhvcNAQEL
BQAwIzEhMB8GA1UEAwwYTlZJRElBIEdhbWVTdHJlYW0gQ2xpZW50MB4XDTI2MTAy
MTE5MTAyNVoXDTQ2MTAxNjE5MTAyNVowIzEhMB8GA1UEAwwYTlZJRElBIEdhbWVT
dHJlYW0gQ2xpZW50MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA8HyP
hGS6C5s9HPm2Cr7O6ivu3eRMFnAVr5F38rvVBrnIbOHsLRwtazeCeKRPtddbYNjI
lB+rQLXzyyMrmHgF/wSzjlqtPMMvMs2WN7AwfjwCU2t204r5vp+KIvijWhxiFFvY
K37XA0qy3RXuANx8JB1t4x44pZ0p1gy8XhmNP3JtgXEU4jUcdY6g1ZpGyamKCR/c
FiqxuZPjp2h32Bc1aXlnnCVr74gO/nbe9AX3tcLfo91IYB2JATSAbB7/FUsvF4jF
rTIvg8ho8cZE02noenuwAkVUD3cDGgwkoZef9oVNYMEGXpC0Dfgb3S0JgRZ3eKrP
Dvp723IftH2nIUsJ3QIDAQABo1MwUTAdBgNVHQ4EFgQUueUFU4i5vMpuRNkHSix+
5Z8TqKswHwYDVR0jBBgwFoAUueUFU4i5vMpuRNkHSix+5Z8TqKswDwYDVR0TAQH/
BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAhZSglVre3DZ/bqwP7kiS//Xuf4ND
lqFE1P4RXVehX0PNaZXbfubYAq7jRqn9tToVcJ1h1dDSTjsIGl+TkODcaXuJ1QYL
MyMy1I9bNLywvo+tA3N+6i2ooJBfxGvys++/jbyk7DlwIJ+W3ni01IMYciHmFjPl
3R/53rwtU6eIpbgLNspRq6UizKcyF9jDewFnoslG/GuTAMjjHy3ThCw8Mnb229f8
IjWBvcThPQ4MH/uu9CgWVH+wInc5InZOFlcgEPF3DshhZd8i4UXfpNRlAhPOoJx/
hD/wYj5Nfi2h8JyVHAuZYIEE9XBvAhpo0d1LWXzi3HJbzH9LRmXxb1GaIQ==
-----END CERTIFICATE-----
"""
CLIENT_KEY = """-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDwfI+EZLoLmz0c
+bYKvs7qK+7d5EwWcBWvkXfyu9UGuchs4ewtHC1rN4J4pE+111tg2MiUH6tAtfPL
IyuYeAX/BLOOWq08wy8yzZY3sDB+PAJTa3bTivm+n4oi+KNaHGIUW9grftcDSrLd
Fe4A3HwkHW3jHjilnSnWDLxeGY0/cm2BcRTiNRx1jqDVmkbJqYoJH9wWKrG5k+On
aHfYFzVpeWecJWvviA7+dt70Bfe1wt+j3UhgHYkBNIBsHv8VSy8XiMWtMi+DyGjx
xkTTaeh6e7ACRVQPdwMaDCShl5/2hU1gwQZekLQN+BvdLQmBFnd4qs8O+nvbch+0
fachSwndAgMBAAECggEAFjMNZYWN1gnxyYW0nAfSnnPJRFLxgiW9HryhM7dKmikW
uxRc4qol+C6C3QOf9xOG9Cj21ROaUaoqM9+Og+/ab9jdgV/V+5Sx+/DV89SQGWsN
NOmn6a270bC1BfbpT2Vs2V/EE/nMu9lOqxF967buv7+2t98/j+wy4zdAQTh/HTLT
YIsZuADerAPw0QC+1DU8mKMUx3lZGvm/L3yRFm92DI/S+r3+d/XzM6ffnIwyMWwG
cdL9Jt62MY4NjqsY5J0tbDicX8B4ZMF7P9X/EaQorrnrBXKYGGjv7Zck1IM2nhaf
xL7Xa/X1w0Hl3kBmyxEc3kjXvb9B6ESv70QFFdHHGQKBgQD7BNwlsaGKeCkaJAGZ
r/sR7C027nF7Ta8Dn4FFEWwtY4GhZTjwYfN4+P6iaov6WuEdeEMCxjXIrSh9PDCx
6TGJQ+E4g+qM7hiPrTM5HY3stxjmqfLX4AciEFoqKxjx550OJ2QRl8w7CIjDvWmb
vON+hc9g2AxYXlS8uhpAeDPZ9QKBgQD1QjKPZ07lF6Ngt7rExym2L2PX69SmqTdE
isveqaiWmEe1Aqd/u9BZp0WCelB/2hhXRsL4WV5vD2PClTJVA04FyDpR2dPyuKUh
y1b+OhuSBVoZ7jiWBNhEtikqB6AoIxpQB2pMukSl/mSkwyIeen/iWIXC16VLyGlo
sVT8+813SQKBgQCOMxSbbeM7VBLEsj0N2Tl7z7VVdwJmfX11qfhLLjGADOy0h2kY
FAUfNL7bN0yQyMbaMPSVL7/sLNvRBjX3s0jSn9eIcoJ+z6quScnIw0tRG/BJS2E8
lba0v1q0pLF+6Fx6VipEJSAXGSCQ8k5PYVPmqtrt27YyoU2TlEGm3/FPuQKBgFPQ
7LB/cMHwIyCBia2IFuQxK+D1snXGbKHbATM2LexRXGgq4Q1RJTIObT3bCTm4Ugs5
npX7DL/NtZ0hGdNdzsNBG1UvqTRRD/YNpMw7FYMAmzszY1k5jKNm0YiQlJZC8NIa
xqQ0IBPObZy1JyrdWGkFMalVwyKz+foSaGYQs7YpAoGAYO+Y4oKbC614/rJfLGrO
7H53srkv02kvFYCYCjofSjmSBY+NoSP2ymbmbwgUOhBaBOt30zFEObrTO20/feNK
jatmwZiwgMoPifPdvqs0G8KNfwO2Mj1XhsaZCYn6PCQPia7bwOHYdrVFSJ6Gv3u1
lWtx29oPsPMW8KLY3nEON0Q=
-----END PRIVATE KEY-----
"""
HOST_CERT = """-----BEGIN CERTIFICATE-----
MIIDJzCCAg+gAwIBAgIUQz8VJmc3W3Eon8KKAeTpT5EJE2AwDQYJKoZIhvcNAQEL
BQAwIzEhMB8GA1UEAwwYU3Vuc2hpbmUgR2FtZXN0cmVhbSBIb3N0MB4XDTI2MTAy
MTE5MTAyNVoXDTQ2MTAxNjE5MTAyNVowIzEhMB8GA1UEAwwYU3Vuc2hpbmUgR2Ft
ZXN0cmVhbSBIb3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA5J4H
Hz0i13mb8iQjJm9hPkVsOAussfz8Lm8yZuM2D7UhXBaEPAfSkjhWEFWVdNujPm9S
bxRe5zett8wZyhnIT+W10E7LdCM8jLdP/pFHWG8yrAHIkNX27cwJ+BqLmSZBLlVD
K6s4jyx3OvdKP9QgVnrm1ESnDynIqnZZkfDD3AkDZJOV5ELBXz09Vkk7blto709V
+nGHG8RzkZs88ZHWtwx2qSqN3VM/0QFQFQB/Z+wabSeM4huhsrr7YioVkWlaEq6q
BifFJzFLJlOOqRtxWQhuMZGXWNcCiFfVhOHfjwXBO9w5lMP39cTNKGhPsZJuZrv4
h2YceRtLfuqPtDDWAwIDAQABo1MwUTAdBgNVHQ4EFgQU47k0KfECwk39bAA/6vng
dLjawqAwHwYDVR0jBBgwFoAU47k0KfECwk39bAA/6vngdLjawqAwDwYDVR0TAQH/
BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAiXoq0e+XqjgjFYhKMKYjWMmVVwWh
bA9CQlf2I7HTDdCSBAd+A2QuoO1d4rL0zDCSOwcs3kjlHPl2S8PPcBz+L2pljppt
g+5z5ncI3qd1vT8qFnncHS9R0EtH1ydwKvBUl76kQFQ9Z0Spa8V2kp9E0tBQImCt
VDTAMS4ROj+vydLMu4tm7boMORjDdyjc10/Ruo1WZP8+eIpAxX1oZpQ9zokwrFC9
3JOl9Siwq5y+aOD2EsjjwYhchr5EfwmwHLENitD4L1a2fFG5MIoA1yT2QaXfLI8Y
5VVUsL6pdfNNXzZITlB2lFDBBrV5LqWFe4qLJ3nSSv3zcfa3ArwcKAWg8w==
-----END CERTIFICATE-----
"""
HOST_KEY = """-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDkngcfPSLXeZvy
JCMmb2E+RWw4C6yx/PwubzJm4zYPtSFcFoQ8B9KSOFYQVZV026M+b1JvFF7nN623
zBnKGchP5bXQTst0IzyMt0/+kUdYbzKsAciQ1fbtzAn4GouZJkEuVUMrqziPLHc6
90o/1CBWeubURKcPKciqdlmR8MPcCQNkk5XkQsFfPT1WSTtuW2jvT1X6cYcbxHOR
mzzxkda3DHapKo3dUz/RAVAVAH9n7BptJ4ziG6GyuvtiKhWRaVoSrqoGJ8UnMUsm
U46pG3FZCG4xkZdY1wKIV9WE4d+PBcE73DmUw/f1xM0oaE+xkm5mu/iHZhx5G0t+
6o+0MNYDAgMBAAECggEAaIhRXVlI6xlx0r/vSo22+xAnMWE1L5UrgdfaRe0yNPBv
NnodMI2rHKyhvYpEclBTTiL8oEnCMxrkVZv/Vsd/Lfksrvi4UnwiHX1cGFw5MRh3
JOsT2BXdeFHJZSZOGlLBjvvxYLBDhkOgqNeVnaXSu3gqv3g8otRKKpc+0VozVBH1
nIb/ZrHdXuxN5FSy9WEDSTj42jrHrDb7K+8Uu7Rk/So8RdDlBXAZD4fCe3hRTu8q
Og4BxqLXY2UtVPBEXptayjZZpEQnAwChPSrakOhq/kfgTDpYcKGylsBr+7+G75/2
yMTYBKXBmGnLVvLtw6avoifBYiRwPI5GB3aBMV0mcQKBgQD6RBGGZtsdsbUdMkch
LekMIV3RmFhpldRcAIz66mdGYnUYRmrB8zx88BSCp6jJWOQZoZEsNY6H04Xwg+Uw
OYIVah6TY5WQeEhkMxFR/hgTCV1arFlA17EMu60QBqI6S1pE2uDMNXS5pNBjHhDR
99k94GEb3PLCdql3xdbJa58OfwKBgQDp2vrTP0TrhJRNCY3SMwRxQSKfKfCArpjW
xMGAdCGfMvScqfQs1zMEbH5o1MZNacVor1v6oXliMe7UagCKYxAb5RbDAsAJZPtI
3zutoDTVU5czQ+3JLXWwsyHE9kg75utQ1l3I6BFskREWMWGMdzzgsRNCJACMHUhN
kNFoI/w+fQKBgQCYIg7BM/9ZErD1eZ3RppX3gMUGZ2S/cowwDUmmMh3rpGhoBE5b
bmBD1rGxJvgsxbOLfTLBvCDToprWNWd6JEtWdzb1A+FkfCoNdxm9TQgXcmbkesaa
60M+aUvv5QhzBWFvX9qtaIV3YKAo0JsK+G5/9ckyZeLQoc+s9n/1201KQwKBgQCi
g7NbqtRSd8g34TPVp+6mZgWRZqvkQXGljwaKJipXYCnt1Y4Cu2qQ4HTYlUVZ9u9h
WLwq6JxaSSkYJLJLeZxwRjwTOY4K+UpH0hq/Bl6xAW5KCiLtc9IeAd6t6hk4x7kw
bEiwndW+BPJC4f5Qh2yfGW+wvyRCOvdMMB/Wp2tcYQKBgAceMKBalW+1T4i5EU1F
CYm2stvwbuKPSIfVHfh1Im4IvpYg2cBdVrsRQ0FatMiDZn15FDw1wTMuY/1LrJp9
/rhXaE0NdzoGWXYyYhrNsa0LiAccXTKiySDdbZb9VrqO7IXW3RCouADgF+rREnKG
yziaNeb1EgSQJ4OodNdc4/P9
-----END PRIVATE KEY-----
"""

PIN = "4821"
SALT = bytes(range(0x00, 0x10))
CLIENT_CHALLENGE = bytes(range(0x10, 0x20))
CLIENT_SECRET = bytes(range(0x20, 0x30))
HOST_CHALLENGE = bytes(range(0x30, 0x40))
HOST_SECRET = bytes(range(0x40, 0x50))


def aes_ecb(key, data):
    """AES-128-ECB without padding, as NvPairingManager::encrypt()."""
    assert len(data) % 16 == 0
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return e.update(data) + e.finalize()


def signature_of(cert_pem):
    """getSignatureFromPemCert(): the certificate's signatureValue bytes."""
    return x509.load_pem_x509_certificate(cert_pem.encode()).signature


def sign(key_pem, data):
    """signMessage(): RSA PKCS#1 v1.5 with SHA-256, whatever the pairing hash."""
    key = serialization.load_pem_private_key(key_pem.encode(), password=None)
    return key.sign(data, padding.PKCS1v15(), hashes.SHA256())


def vectors(generation):
    # Generation 7 and later hash with SHA-256, earlier hosts with SHA-1.
    h = hashlib.sha256 if generation >= 7 else hashlib.sha1
    aes_key = h(SALT + PIN.encode()).digest()[:16]

    # 1. getservercert: the salt and our certificate's PEM text, in hex.
    getservercert = ("devicename=roth&updateState=1&phrase=getservercert&salt=" + SALT.hex() +
                     "&clientcert=" + CLIENT_CERT.encode().hex())
    plaincert = HOST_CERT.encode().hex()

    # 2. clientchallenge: our challenge, encrypted.
    clientchallenge = ("devicename=roth&updateState=1&clientchallenge=" +
                       aes_ecb(aes_key, CLIENT_CHALLENGE).hex())
    # The host's answer: hash(challenge + its signature + its secret), then
    # its own challenge, padded to whole blocks and encrypted.
    host_hash = h(CLIENT_CHALLENGE + signature_of(HOST_CERT) + HOST_SECRET).digest()
    plain = host_hash + HOST_CHALLENGE
    plain += bytes(-len(plain) % 16)
    challengeresponse = aes_ecb(aes_key, plain).hex()

    # 3. serverchallengeresp: hash(host challenge + our signature + our
    # secret), padded to 32 bytes, encrypted.
    client_hash = h(HOST_CHALLENGE + signature_of(CLIENT_CERT) + CLIENT_SECRET).digest()
    client_hash = client_hash.ljust(32, bytes(1))
    serverchallengeresp = ("devicename=roth&updateState=1&serverchallengeresp=" +
                           aes_ecb(aes_key, client_hash).hex())
    # The host's secret, signed with its key.
    pairingsecret = (HOST_SECRET + sign(HOST_KEY, HOST_SECRET)).hex()

    # 4. clientpairingsecret: our secret, signed with our key.
    clientpairingsecret = ("devicename=roth&updateState=1&clientpairingsecret=" +
                           (CLIENT_SECRET + sign(CLIENT_KEY, CLIENT_SECRET)).hex())

    # 5. pairchallenge over HTTPS.
    pairchallenge = "devicename=roth&updateState=1&phrase=pairchallenge"

    return [("aes_key", aes_key.hex()), ("getservercert", getservercert), ("plaincert", plaincert),
            ("clientchallenge", clientchallenge), ("challengeresponse", challengeresponse),
            ("serverchallengeresp", serverchallengeresp), ("pairingsecret", pairingsecret),
            ("clientpairingsecret", clientpairingsecret), ("pairchallenge", pairchallenge)]


def cpp_string(name, value):
    return 'inline constexpr std::string_view ' + name + ' =\n    R"(' + value + ')";\n'


def main():
    out = ["// Written by tools/pairing-vectors.py: run it again instead of editing.\n\n",
           "#pragma once\n\n#include <string_view>\n\nnamespace vectors {\n\n"]
    for name, value in [("client_cert", CLIENT_CERT), ("client_key", CLIENT_KEY),
                        ("host_cert", HOST_CERT), ("host_key", HOST_KEY), ("pin", PIN)]:
        out.append(cpp_string(name, value))
    for name, data in [("salt", SALT), ("client_challenge", CLIENT_CHALLENGE),
                       ("client_secret", CLIENT_SECRET), ("host_challenge", HOST_CHALLENGE),
                       ("host_secret", HOST_SECRET)]:
        out.append(cpp_string(name, data.hex()))
    for generation, space in [(7, "sha256"), (6, "sha1")]:
        out.append("\nnamespace " + space + " {\n\n")
        for name, value in vectors(generation):
            out.append(cpp_string(name, value))
        out.append("\n}  // namespace " + space + "\n")
    out.append("\n}  // namespace vectors\n")
    print("".join(out), end="")


if __name__ == "__main__":
    main()
