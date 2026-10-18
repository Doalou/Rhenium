#!/usr/bin/env python3
# Rhénium : corrige les relais d'appels virtuels (vcall thunks) d'un exe ARM32
# produit par clang-cl.
#
# En ABI Microsoft, un pointeur vers une fonction membre virtuelle désigne un
# relais qui lit la vtable puis saute à l'entrée. Clang l'émet comme une
# fonction variadique à appel terminal forcé ; le backend ARM32 n'y voit que r0
# comme argument et prend r1 comme registre de travail :
#
#   push.w {r11, lr} ; mov r11, sp ; ldr r1, [r0, #a] ; ldr r1, [r1, #b]
#   pop.w {r11, lr} ; bx r1
#
# alors que r1-r3 portent encore les arguments de l'appel. Ce script réécrit
# chaque relais, à taille égale, avec r12 (registre de travail inter-appels de
# l'ABI) :
#
#   ldr.w r12, [r0, #a] ; ldr.w r12, [r12, #b] ; bx r12 ; nop...
#
# La réécriture ne change rien quand r1 n'est pas un argument : elle est sûre
# pour toute fonction qui a exactement cette forme.
#
# Usage : fix_vcall_thunks.py rhenium_shell.exe [--dry-run]

import struct
import sys

PUSH_R11_LR = bytes.fromhex('2de90048')  # push.w {r11, lr}
MOV_R11_SP = bytes.fromhex('eb46')       # mov r11, sp
POP_R11_LR = bytes.fromhex('bde80048')   # pop.w {r11, lr}
BX_R1 = bytes.fromhex('0847')            # bx r1
NOP = bytes.fromhex('00bf')              # nop (T1)


def text_sections(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    assert machine == 0x1c4, 'exe ARM Thumb-2 attendu'
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    table = pe + 24 + optional_size
    for i in range(count):
        entry = table + 40 * i
        characteristics = struct.unpack_from('<I', data, entry + 36)[0]
        if characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            raw_size, raw_offset = struct.unpack_from('<II', data, entry + 16)
            yield raw_offset, raw_offset + raw_size


def decode_ldr(data, pos, rt, rn):
    """LDR rt, [rn, #imm] en T1 (2 octets) ou T3 (4 octets) : (imm, taille)."""
    half = struct.unpack_from('<H', data, pos)[0]
    if half >> 11 == 0b01101 and half & 7 == rt and (half >> 3) & 7 == rn:
        return ((half >> 6) & 0x1f) * 4, 2
    first, second = struct.unpack_from('<HH', data, pos)
    if first == 0xf8d0 | rn and second >> 12 == rt:
        return second & 0xfff, 4
    return None


def ldr_w(rt, rn, imm):
    return struct.pack('<HH', 0xf8d0 | rn, (rt << 12) | imm)


def fix(data, dry_run):
    patched = 0
    for start, end in text_sections(data):
        pos = data.find(PUSH_R11_LR, start, end)
        while pos != -1:
            cur = pos + 4
            ok = data[cur:cur + 2] == MOV_R11_SP
            cur += 2
            first = decode_ldr(data, cur, 1, 0) if ok else None
            if first:
                cur += first[1]
                second = decode_ldr(data, cur, 1, 1)
            if first and second:
                cur += second[1]
                if (data[cur:cur + 4] == POP_R11_LR and
                        data[cur + 4:cur + 6] == BX_R1 and pos % 2 == 0):
                    size = cur + 6 - pos
                    code = (ldr_w(12, 0, first[0]) + ldr_w(12, 12, second[0]) +
                            bytes.fromhex('6047'))  # bx r12
                    code += NOP * ((size - len(code)) // 2)
                    assert len(code) == size
                    if not dry_run:
                        data[pos:pos + size] = code
                    patched += 1
            pos = data.find(PUSH_R11_LR, pos + 2, end)
    return patched


def main():
    path = sys.argv[1]
    dry_run = '--dry-run' in sys.argv[2:]
    with open(path, 'rb') as f:
        data = bytearray(f.read())
    patched = fix(data, dry_run)
    if not dry_run:
        with open(path, 'wb') as f:
            f.write(data)
    print(f'{path} : {patched} relais vcall {"à corriger" if dry_run else "corrigés"}')


if __name__ == '__main__':
    main()
