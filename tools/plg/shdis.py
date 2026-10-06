# SH-2 の小さな逆アセンブラ（調べもの用）。ROM は 0 番地から。
#   shdis.py <開始> <長さ>         その範囲を逆アセンブルする
#   shdis.py find <16 進のバイト列>   ROM の中の場所を探す
#   shdis.py lit <32bit の値>        その値を置いている定数（4 バイト境界）と、それを読む MOV.L を探す
import struct, sys
import os
ROM = open(os.environ.get('MU2000_ROM', 'mu2000_flash.bin'), 'rb').read()   # ROM の場所は環境変数 MU2000_ROM で

def w16(a): return struct.unpack('>H', ROM[a:a + 2])[0]
def w32(a): return struct.unpack('>I', ROM[a:a + 4])[0]
def s8(v): return v - 256 if v & 0x80 else v
def s12(v): return v - 4096 if v & 0x800 else v

def dis(pc):
    op = w16(pc)
    n, m, d4, d8 = (op >> 8) & 15, (op >> 4) & 15, op & 15, op & 255
    R = lambda x: 'r%d' % x
    t = op >> 12
    if t == 0:
        lo = op & 15
        if lo == 4: return 'mov.b %s,@(r0,%s)' % (R(m), R(n))
        if lo == 5: return 'mov.w %s,@(r0,%s)' % (R(m), R(n))
        if lo == 6: return 'mov.l %s,@(r0,%s)' % (R(m), R(n))
        if lo == 7: return 'mul.l %s,%s' % (R(m), R(n))
        if lo == 12: return 'mov.b @(r0,%s),%s' % (R(m), R(n))
        if lo == 13: return 'mov.w @(r0,%s),%s' % (R(m), R(n))
        if lo == 14: return 'mov.l @(r0,%s),%s' % (R(m), R(n))
        if lo == 15: return 'mac.l @%s+,@%s+' % (R(m), R(n))
        k = op & 0xff
        T = {0x02: 'stc sr,%s', 0x12: 'stc gbr,%s', 0x22: 'stc vbr,%s', 0x03: 'bsrf %s', 0x23: 'braf %s', 0x29: 'movt %s',
             0x0a: 'sts mach,%s', 0x1a: 'sts macl,%s', 0x2a: 'sts pr,%s'}
        if k in T: return T[k] % R(n)
        U = {0x0008: 'clrt', 0x0018: 'sett', 0x0028: 'clrmac', 0x0009: 'nop', 0x0019: 'div0u', 0x000b: 'rts', 0x001b: 'sleep', 0x002b: 'rte'}
        return U.get(op, '.word %04x' % op)
    if t == 1: return 'mov.l %s,@(%d,%s)' % (R(m), d4 * 4, R(n))
    if t == 2:
        T = {0: 'mov.b %s,@%s', 1: 'mov.w %s,@%s', 2: 'mov.l %s,@%s', 4: 'mov.b %s,@-%s', 5: 'mov.w %s,@-%s', 6: 'mov.l %s,@-%s',
             7: 'div0s %s,%s', 8: 'tst %s,%s', 9: 'and %s,%s', 10: 'xor %s,%s', 11: 'or %s,%s', 12: 'cmp/str %s,%s', 13: 'xtrct %s,%s',
             14: 'mulu %s,%s', 15: 'muls %s,%s'}
        return T[d4] % (R(m), R(n)) if d4 in T else '.word %04x' % op
    if t == 3:
        T = {0: 'cmp/eq', 2: 'cmp/hs', 3: 'cmp/ge', 4: 'div1', 5: 'dmulu.l', 6: 'cmp/hi', 7: 'cmp/gt', 8: 'sub', 10: 'subc', 11: 'subv',
             12: 'add', 13: 'dmuls.l', 14: 'addc', 15: 'addv'}
        return '%s %s,%s' % (T[d4], R(m), R(n)) if d4 in T else '.word %04x' % op
    if t == 4:
        k = op & 0xff
        T = {0x00: 'shll %s', 0x01: 'shlr %s', 0x02: 'sts.l mach,@-%s', 0x03: 'stc.l sr,@-%s', 0x04: 'rotl %s', 0x05: 'rotr %s',
             0x06: 'lds.l @%s+,mach', 0x07: 'ldc.l @%s+,sr', 0x08: 'shll2 %s', 0x09: 'shlr2 %s', 0x0a: 'lds %s,mach', 0x0b: 'jsr @%s',
             0x0e: 'ldc %s,sr', 0x10: 'dt %s', 0x11: 'cmp/pz %s', 0x12: 'sts.l macl,@-%s', 0x13: 'stc.l gbr,@-%s', 0x15: 'cmp/pl %s',
             0x16: 'lds.l @%s+,macl', 0x17: 'ldc.l @%s+,gbr', 0x18: 'shll8 %s', 0x19: 'shlr8 %s', 0x1a: 'lds %s,macl', 0x1b: 'tas.b @%s',
             0x1e: 'ldc %s,gbr', 0x20: 'shal %s', 0x21: 'shar %s', 0x22: 'sts.l pr,@-%s', 0x23: 'stc.l vbr,@-%s', 0x24: 'rotcl %s',
             0x25: 'rotcr %s', 0x26: 'lds.l @%s+,pr', 0x27: 'ldc.l @%s+,vbr', 0x28: 'shll16 %s', 0x29: 'shlr16 %s', 0x2a: 'lds %s,pr',
             0x2b: 'jmp @%s', 0x2e: 'ldc %s,vbr'}
        if k in T: return T[k] % R(n)
        if d4 == 15: return 'mac.w @%s+,@%s+' % (R(m), R(n))
        return '.word %04x' % op
    if t == 5: return 'mov.l @(%d,%s),%s' % (d4 * 4, R(m), R(n))
    if t == 6:
        T = {0: 'mov.b @%s,%s', 1: 'mov.w @%s,%s', 2: 'mov.l @%s,%s', 3: 'mov %s,%s', 4: 'mov.b @%s+,%s', 5: 'mov.w @%s+,%s',
             6: 'mov.l @%s+,%s', 7: 'not %s,%s', 8: 'swap.b %s,%s', 9: 'swap.w %s,%s', 10: 'negc %s,%s', 11: 'neg %s,%s',
             12: 'extu.b %s,%s', 13: 'extu.w %s,%s', 14: 'exts.b %s,%s', 15: 'exts.w %s,%s'}
        return T[d4] % (R(m), R(n))
    if t == 7: return 'add #%d,%s' % (s8(d8), R(n))
    if t == 8:
        k = (op >> 8) & 15
        if k == 0: return 'mov.b r0,@(%d,%s)' % (d4, R(m))
        if k == 1: return 'mov.w r0,@(%d,%s)' % (d4 * 2, R(m))
        if k == 4: return 'mov.b @(%d,%s),r0' % (d4, R(m))
        if k == 5: return 'mov.w @(%d,%s),r0' % (d4 * 2, R(m))
        if k == 8: return 'cmp/eq #%d,r0   ; 0x%02x' % (s8(d8), d8)
        B = {9: 'bt', 11: 'bf', 13: 'bt/s', 15: 'bf/s'}
        if k in B: return '%s 0x%06x' % (B[k], pc + 4 + s8(d8) * 2)
        return '.word %04x' % op
    if t == 9:
        a = pc + 4 + d8 * 2
        return 'mov.w @(0x%06x),%s   ; =0x%04x' % (a, R(n), w16(a))
    if t == 10: return 'bra 0x%06x' % (pc + 4 + s12(op & 0xfff) * 2)
    if t == 11: return 'bsr 0x%06x' % (pc + 4 + s12(op & 0xfff) * 2)
    if t == 12:
        k = (op >> 8) & 15
        T = {0: 'mov.b r0,@(%d,gbr)' % d8, 1: 'mov.w r0,@(%d,gbr)' % (d8 * 2), 2: 'mov.l r0,@(%d,gbr)' % (d8 * 4), 3: 'trapa #%d' % d8,
             4: 'mov.b @(%d,gbr),r0' % d8, 5: 'mov.w @(%d,gbr),r0' % (d8 * 2), 6: 'mov.l @(%d,gbr),r0' % (d8 * 4),
             8: 'tst #0x%02x,r0' % d8, 9: 'and #0x%02x,r0' % d8, 10: 'xor #0x%02x,r0' % d8, 11: 'or #0x%02x,r0' % d8,
             12: 'tst.b #0x%02x,@(r0,gbr)' % d8, 13: 'and.b #0x%02x,@(r0,gbr)' % d8, 14: 'xor.b #0x%02x,@(r0,gbr)' % d8,
             15: 'or.b #0x%02x,@(r0,gbr)' % d8}
        if k == 7:
            a = (pc & ~3) + 4 + d8 * 4
            return 'mova @(0x%06x),r0' % a
        return T[k]
    if t == 13:
        a = (pc & ~3) + 4 + d8 * 4
        return 'mov.l @(0x%06x),%s   ; =0x%08x' % (a, R(n), w32(a))
    if t == 14: return 'mov #%d,%s   ; 0x%02x' % (s8(d8), R(n), d8)
    return '.word %04x' % op

def main():
    a = sys.argv[1:]
    if a[0] == 'find':
        pat = bytes.fromhex(''.join(a[1:]))
        i = ROM.find(pat)
        n = 0
        while i >= 0 and n < 40:
            print('%06x' % i)
            n += 1
            i = ROM.find(pat, i + 1)
        return
    if a[0] == 'lit':
        v = int(a[1], 16)
        pat = struct.pack('>I', v)
        for i in range(0, len(ROM) - 4, 4):
            if ROM[i:i + 4] == pat:
                # MOV.L @(disp,PC) で読める範囲（前 1020 バイト）から探す
                users = []
                for pc in range(max(0, i - 1024), i, 2):
                    op = w16(pc)
                    if (op >> 12) == 13 and (pc & ~3) + 4 + (op & 255) * 4 == i:
                        users.append(pc)
                if users:
                    print('literal at %06x used by: %s' % (i, ' '.join('%06x' % u for u in users[:12])))
        return
    start, n = int(a[0], 16), int(a[1], 16)
    for pc in range(start, start + n, 2):
        print('%06x  %04x  %s' % (pc, w16(pc), dis(pc)))

main()
