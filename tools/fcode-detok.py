#!/usr/bin/env python3
# fcode-detok.py FILE [START [END]] [--offset16] [--apple]
# Detokenizes Open Firmware FCode (IEEE 1275): a PCI card's expansion ROM
# (START defaults to the FCode image after the PCIR structure) or the
# packages inside a Power Macintosh ROM (give START and END in the file).
# For reading only: the output is one token per line with its offset,
# operands resolved; tokens the standard does not name show as tok_NNN,
# or by the name a named-token gave them. --apple: the ROM's own packages
# (Open Firmware 1.0.5), where 0x00 is a two-byte token prefix like
# 0x01-0x0F and branch offsets are 8 bits.
#
# PowerMac emulator tools. Copyright 2026, the PowerMac emulator authors.
# MIT License.
import struct
import sys

NAMES = {
    0x00: 'end0', 0x10: 'b(lit)', 0x11: "b(')", 0x12: 'b(")',
    0x13: 'bbranch', 0x14: 'b?branch', 0x15: 'b(loop)', 0x16: 'b(+loop)',
    0x17: 'b(do)', 0x18: 'b(?do)', 0x19: 'i', 0x1a: 'j', 0x1b: 'b(leave)',
    0x1c: 'b(of)', 0x1d: 'execute', 0x1e: '+', 0x1f: '-', 0x20: '*',
    0x21: '/', 0x22: 'mod', 0x23: 'and', 0x24: 'or', 0x25: 'xor',
    0x26: 'invert', 0x27: 'lshift', 0x28: 'rshift', 0x29: '>>a',
    0x2a: '/mod', 0x2b: 'u/mod', 0x2c: 'negate', 0x2d: 'abs', 0x2e: 'min',
    0x2f: 'max', 0x30: '>r', 0x31: 'r>', 0x32: 'r@', 0x33: 'exit',
    0x34: '0=', 0x35: '0<>', 0x36: '0<', 0x37: '0<=', 0x38: '0>',
    0x39: '0>=', 0x3a: '<', 0x3b: '>', 0x3c: '=', 0x3d: '<>', 0x3e: 'u>',
    0x3f: 'u<=', 0x40: 'u<', 0x41: 'u>=', 0x42: '>=', 0x43: '<=',
    0x44: 'between', 0x45: 'within', 0x46: 'drop', 0x47: 'dup',
    0x48: 'over', 0x49: 'swap', 0x4a: 'rot', 0x4b: '-rot', 0x4c: 'tuck',
    0x4d: 'nip', 0x4e: 'pick', 0x4f: 'roll', 0x50: '?dup', 0x51: 'depth',
    0x52: '2drop', 0x53: '2dup', 0x54: '2over', 0x55: '2swap',
    0x56: '2rot', 0x57: '2/', 0x58: 'u2/', 0x59: '2*', 0x5a: '/c',
    0x5b: '/w', 0x5c: '/l', 0x5d: '/n', 0x5e: 'ca+', 0x5f: 'wa+',
    0x60: 'la+', 0x61: 'na+', 0x62: 'char+', 0x63: 'wa1+', 0x64: 'la1+',
    0x65: 'cell+', 0x66: 'chars', 0x67: '/w*', 0x68: '/l*', 0x69: 'cells',
    0x6a: 'on', 0x6b: 'off', 0x6c: '+!', 0x6d: '@', 0x6e: 'l@', 0x6f: 'w@',
    0x70: '<w@', 0x71: 'c@', 0x72: '!', 0x73: 'l!', 0x74: 'w!', 0x75: 'c!',
    0x76: '2@', 0x77: '2!', 0x78: 'move', 0x79: 'fill', 0x7a: 'comp',
    0x7b: 'noop', 0x7c: 'lwsplit', 0x7d: 'wljoin', 0x7e: 'lbsplit',
    0x7f: 'bljoin', 0x80: 'wbflip', 0x81: 'upc', 0x82: 'lcc', 0x83: 'pack',
    0x84: 'count', 0x85: 'body>', 0x86: '>body', 0x87: 'fcode-revision',
    0x88: 'span', 0x89: 'unloop', 0x8a: 'expect', 0x8b: 'alloc-mem',
    0x8c: 'free-mem', 0x8d: 'key?', 0x8e: 'key', 0x8f: 'emit', 0x90: 'type',
    0x91: '(cr', 0x92: 'cr', 0x93: '#out', 0x94: '#line', 0x95: 'hold',
    0x96: '<#', 0x97: 'u#>', 0x98: 'sign', 0x99: 'u#', 0x9a: 'u#s',
    0x9b: 'u.', 0x9c: 'u.r', 0x9d: '.', 0x9e: '.r', 0x9f: '.s',
    0xa0: 'base', 0xa1: 'convert', 0xa2: '$number', 0xa3: 'digit',
    0xa4: '-1', 0xa5: '0', 0xa6: '1', 0xa7: '2', 0xa8: '3', 0xa9: 'bl',
    0xaa: 'bs', 0xab: 'bell', 0xac: 'bounds', 0xad: 'here', 0xae: 'aligned',
    0xaf: 'wbsplit', 0xb0: 'bwjoin', 0xb1: 'b(<mark)', 0xb2: 'b(>resolve)',
    0xb3: 'set-token-table', 0xb4: 'set-table', 0xb5: 'new-token',
    0xb6: 'named-token', 0xb7: 'b(:)', 0xb8: 'b(value)',
    0xb9: 'b(variable)', 0xba: 'b(constant)', 0xbb: 'b(create)',
    0xbc: 'b(defer)', 0xbd: 'b(buffer:)', 0xbe: 'b(field)', 0xbf: 'b(code)',
    0xc0: 'instance', 0xc2: 'b(;)', 0xc3: 'b(to)', 0xc4: 'b(case)',
    0xc5: 'b(endcase)', 0xc6: 'b(endof)', 0xc7: '#', 0xc8: '#s', 0xc9: '#>',
    0xca: 'external-token', 0xcb: '$find', 0xcc: 'offset16',
    0xcd: 'evaluate', 0xd0: 'c,', 0xd1: 'w,', 0xd2: 'l,', 0xd3: ',',
    0xd4: 'um*', 0xd5: 'um/mod', 0xd8: 'd+', 0xd9: 'd-', 0xda: 'get-token',
    0xdb: 'set-token', 0xdc: 'state', 0xdd: 'compile,', 0xde: 'behavior',
    0xf0: 'start0', 0xf1: 'start1', 0xf2: 'start2', 0xf3: 'start4',
    0xfc: 'ferror', 0xfd: 'version1', 0xfe: '4-byte-id', 0xff: 'end1',
    0x102: 'my-address', 0x103: 'my-space', 0x105: 'free-virtual',
    0x110: 'property', 0x111: 'encode-int', 0x112: 'encode+',
    0x113: 'encode-phys', 0x114: 'encode-string', 0x115: 'encode-bytes',
    0x116: 'reg', 0x119: 'model', 0x11a: 'device-type',
    0x11b: 'parse-2int', 0x11c: 'is-install', 0x11d: 'is-remove',
    0x11e: 'is-selftest', 0x11f: 'new-device', 0x120: 'diagnostic-mode?',
    0x121: 'display-status', 0x122: 'memory-test-suite', 0x124: 'mask',
    0x125: 'get-msecs', 0x126: 'ms', 0x127: 'finish-device',
    0x128: 'decode-phys', 0x12b: 'map-low', 0x150: '#lines',
    0x151: '#columns', 0x152: 'line#', 0x153: 'column#', 0x154: 'inverse?',
    0x155: 'inverse-screen?', 0x157: 'draw-character',
    0x158: 'reset-screen', 0x159: 'toggle-cursor', 0x15a: 'erase-screen',
    0x15b: 'blink-screen', 0x15c: 'invert-screen',
    0x15d: 'insert-characters', 0x15e: 'delete-characters',
    0x15f: 'insert-lines', 0x160: 'delete-lines', 0x161: 'draw-logo',
    0x162: 'frame-buffer-adr', 0x163: 'screen-height', 0x164: 'screen-width',
    0x165: 'window-top', 0x166: 'window-left', 0x16a: 'default-font',
    0x16b: 'set-font', 0x16c: 'char-height', 0x16d: 'char-width',
    0x16e: '>font', 0x16f: 'fontbytes', 0x180: 'fb8-draw-character',
    0x181: 'fb8-reset-screen', 0x182: 'fb8-toggle-cursor',
    0x183: 'fb8-erase-screen', 0x184: 'fb8-blink-screen',
    0x185: 'fb8-invert-screen', 0x186: 'fb8-insert-characters',
    0x187: 'fb8-delete-characters', 0x188: 'fb8-insert-lines',
    0x189: 'fb8-delete-lines', 0x18a: 'fb8-draw-logo', 0x18b: 'fb8-install',
    0x1a4: 'mac-address', 0x201: 'device-name', 0x202: 'my-args',
    0x203: 'my-self', 0x204: 'find-package', 0x205: 'open-package',
    0x206: 'close-package', 0x207: 'find-method', 0x208: 'call-package',
    0x209: '$call-parent', 0x20a: 'my-parent', 0x20b: 'ihandle>phandle',
    0x20d: 'my-unit', 0x20e: '$call-method', 0x20f: '$open-package',
    0x210: 'processor-type', 0x211: 'firmware-version',
    0x212: 'fcode-version', 0x213: 'alarm', 0x214: '(is-user-word)',
    0x215: 'suspend-fcode', 0x216: 'abort', 0x217: 'catch', 0x218: 'throw',
    0x219: 'user-abort', 0x21a: 'get-my-property', 0x21b: 'decode-int',
    0x21c: 'decode-string', 0x21d: 'get-inherited-property',
    0x21e: 'delete-property', 0x21f: 'get-package-property',
    0x220: 'cpeek', 0x221: 'wpeek', 0x222: 'lpeek', 0x223: 'cpoke',
    0x224: 'wpoke', 0x225: 'lpoke', 0x226: 'lwflip', 0x227: 'lbflip',
    0x228: 'lbflips', 0x229: 'adr-mask', 0x230: 'rb@', 0x231: 'rb!',
    0x232: 'rw@', 0x233: 'rw!', 0x234: 'rl@', 0x235: 'rl!',
    0x236: 'wbflips', 0x237: 'lwflips', 0x238: 'probe',
    0x239: 'probe-virtual', 0x23b: 'child', 0x23c: 'peer',
    0x23d: 'next-property', 0x23e: 'byte-load', 0x23f: 'set-args',
    0x240: 'left-parse-string',
}

BRANCHES = {0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x1c, 0xc6}
DEFINERS = {0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf}


def pci_fcode_start(data):
    """The FCode image of a PCI expansion ROM (after its PCIR structure)."""
    if data[0:2] != b'\x55\xaa':
        return 0
    offset = 0
    while offset < len(data) and data[offset:offset + 2] == b'\x55\xaa':
        pcir = offset + struct.unpack_from('<H', data, offset + 0x18)[0]
        code_type = data[pcir + 0x14]
        if code_type == 1:
            # the FCode follows the PCIR data structure
            length = struct.unpack_from('<H', data, pcir + 0x0a)[0]
            return pcir + length
        image_blocks = struct.unpack_from('<H', data, pcir + 0x10)[0]
        if data[pcir + 0x15] & 0x80:
            break
        offset += image_blocks * 512
    return 0


def detokenize(data, start, end, offset16, apple):
    names = dict(NAMES)
    pos = start
    depth = 0

    def token():
        nonlocal pos
        code = data[pos]
        pos += 1
        if (0x01 if not apple else 0x00) <= code <= 0x0f:
            code = (code << 8) | data[pos]
            pos += 1
        return code

    def branch_offset():
        nonlocal pos
        if offset16:
            value = struct.unpack_from('>h', data, pos)[0]
            pos += 2
        else:
            value = struct.unpack_from('>b', data, pos)[0]
            pos += 1
        return value

    def name_of(code):
        return names.get(code, 'tok_%x' % code)

    while pos < end:
        at = pos
        code = token()
        name = name_of(code)
        text = name
        if code == 0x10:
            value = struct.unpack_from('>I', data, pos)[0]
            pos += 4
            text = '%x' % value
        elif code == 0x12:
            length = data[pos]
            string = data[pos + 1:pos + 1 + length]
            pos += 1 + length
            text = 'b(") "%s"' % string.decode('latin-1').replace(
                '\n', '\\n')
        elif code == 0x11 or code == 0xc3 or code == 0xdd:
            target = token()
            text = "%s %s" % (name, name_of(target))
        elif code in BRANCHES:
            value = branch_offset()
            text = '%s %+d (-> %x)' % (name, value, at + 1 + value
                + (0 if not offset16 else 0))
        elif code in (0xb5, 0xb6, 0xca):
            if code in (0xb6, 0xca):
                length = data[pos]
                label = data[pos + 1:pos + 1 + length].decode('latin-1')
                pos += 1 + length
            else:
                label = None
            new = token()
            if label:
                names[new] = label
            text = '%s %s = tok_%x' % (name, label or '', new)
        elif code == 0xcc:
            offset16 = True
        elif code in (0xf0, 0xf1, 0xf2, 0xf3, 0xfd):
            checksum, length = struct.unpack_from('>HI', data, pos + 1)
            text = '%s (format %d, checksum %04x, length %x)' % (name,
                data[pos], checksum, length)
            if code != 0xfd:
                offset16 = True
            pos += 7
        if code in DEFINERS:
            depth = 1 if code == 0xb7 else depth
        print('%06x: %s%s' % (at, '  ' * depth if code not in DEFINERS
            else '', text))
        if code == 0xc2:
            depth = 0
        if code in (0x00, 0xff) and start != 0 and not apple:
            break


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    offset16 = '--offset16' in sys.argv
    apple = '--apple' in sys.argv
    if not args:
        sys.exit('usage: fcode-detok.py FILE [START [END]] [--offset16] '
            '[--apple]')
    data = open(args[0], 'rb').read()
    if len(args) > 1:
        start = int(args[1], 16)
    else:
        start = pci_fcode_start(data)
    end = int(args[2], 16) if len(args) > 2 else len(data)
    detokenize(data, start, end, offset16, apple)


if __name__ == '__main__':
    main()
