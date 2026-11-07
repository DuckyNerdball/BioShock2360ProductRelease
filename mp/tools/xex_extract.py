# xex_extract.py - extract the decrypted basefile (PE image) from a retail XEX2.
# Replaces xorloser's xextool -b for XEXs using basic (or no) compression.
# LZX ("normal") compression is NOT supported - use xextool for those.
#   python tools\xex_extract.py game_bs2\Bioshock2_Xenon.xex dumps\Bioshock2_Xenon_base.exe
# Needs: pip install cryptography   (or pycryptodome)
import sys, struct

RETAIL_KEY = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')
DEVKIT_KEY = bytes(16)

def aes_ecb_dec(key, data):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        d = Cipher(algorithms.AES(key), modes.ECB()).decryptor(); return d.update(data) + d.finalize()
    except ImportError:
        from Crypto.Cipher import AES; return AES.new(key, AES.MODE_ECB).decrypt(data)

def aes_cbc_dec(key, data):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        d = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor(); return d.update(data) + d.finalize()
    except ImportError:
        from Crypto.Cipher import AES; return AES.new(key, AES.MODE_CBC, bytes(16)).decrypt(data)

def main(src, dst):
    x = open(src, 'rb').read()
    magic, flags, pe_off, _, sec_off, n = struct.unpack_from('>4sIIIII', x, 0)
    if magic != b'XEX2': sys.exit('not XEX2: %r' % magic)
    hdrs = dict(struct.unpack_from('>II', x, 24 + 8 * i) for i in range(n))
    if 0x3FF not in hdrs: sys.exit('no file format info header')
    ff = hdrs[0x3FF]
    info_size, enc, comp = struct.unpack_from('>IHH', x, ff)
    image_size, = struct.unpack_from('>I', x, sec_off + 0x004)
    file_key = x[sec_off + 0x150: sec_off + 0x160]
    payload = x[pe_off:]
    print('enc=%d comp=%d image_size=0x%X' % (enc, comp, image_size), file=sys.stderr)
    if comp not in (0, 1):
        sys.exit('compression type %d (LZX) not supported - use xextool -b' % comp)

    def build(key):
        data = payload
        if enc == 1:
            session = aes_ecb_dec(key, file_key)
            data = aes_cbc_dec(session, data[:len(data) & ~15])
        if comp == 0:
            return data[:image_size]
        out = bytearray(); p = 0
        for i in range((info_size - 8) // 8):
            dsz, zsz = struct.unpack_from('>II', x, ff + 8 + 8 * i)
            out += data[p:p + dsz]; out += bytes(zsz); p += dsz
        return bytes(out[:image_size])

    for name, key in (('retail', RETAIL_KEY), ('devkit', DEVKIT_KEY)):
        img = build(key)
        if img[:2] == b'MZ':
            open(dst, 'wb').write(img)
            print('wrote %s (%d bytes, %s key)' % (dst, len(img), name), file=sys.stderr)
            return
    sys.exit('decryption produced no MZ header with retail or devkit key')

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__ or 'usage: xex_extract.py in.xex out.exe')
    main(sys.argv[1], sys.argv[2])
