#!/usr/bin/env python3
"""把 FIT 里 kernel 的 compression 从 lzo 改成 gzip，并用 gzip 压缩的内核重建镜像。

原理：U-Boot 的 image_decomp() 是 4 路跳转表，RSA 验签只挂在 LZO 分支（类型 4）。
类型 1（gzip）分支没有任何校验调用，所以换压缩方式即可绕过内核验签，
且完全不需要修改 U-Boot 组件本身。

警告：DATA_START (0x53f4)、DTB 长度 (0x4ef6) 与 STIF_LEN (259) 针对中国电信版
HG6371F（2026-05-13 固件基线）实测确认；其他运营商或批次固件请先核对自己的 bootfs 头部结构。
"""
import struct
import hashlib
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else 'backups/bootfs2.bin'
GZ = sys.argv[2] if len(sys.argv) > 2 else 'build/kernel.gz'
OUT = sys.argv[3] if len(sys.argv) > 3 else 'build/bootfs_gzip.bin'
DATA_START = 0x53f4          # 原镜像数据区起点（DTB+STIF 之后有 ~1KB 余量）
STIF_LEN = 259


def parse_fdt(d):
    magic, tot, off_s, off_str, off_rsv, ver, lastc, bootcpu, lsz, lst = struct.unpack('>10I', d[:40])
    assert magic == 0xD00DFEED
    o = off_s
    depth = 0
    path = []
    imgs = {}
    cur = None
    while True:
        t = struct.unpack('>I', d[o:o + 4])[0]
        o += 4
        if t == 1:
            n = d[o:d.index(b'\0', o)].decode()
            o += len(n) + 1
            o = (o + 3) & ~3
            path.append(n)
            depth += 1
            if depth == 3 and path[1] == 'images':
                cur = n
                imgs[cur] = {}
        elif t == 2:
            depth -= 1
            path.pop()
        elif t == 3:
            ln, no = struct.unpack('>II', d[o:o + 8])
            vo = o + 8
            v = bytes(d[vo:vo + ln])
            o = vo + ln
            o = (o + 3) & ~3
            pn = d[off_str + no:d.index(b'\0', off_str + no)].decode()
            if cur and depth == 3:
                if pn == 'data-position':
                    imgs[cur]['pos'] = struct.unpack('>I', v)[0]
                    imgs[cur]['pos_off'] = vo
                elif pn == 'data-size':
                    imgs[cur]['size'] = struct.unpack('>I', v)[0]
                    imgs[cur]['size_off'] = vo
            elif cur and depth == 4 and path[-1] == 'hash-1' and pn == 'value':
                imgs[cur]['hash_off'] = vo
            if pn == 'compression' and depth == 3 and path[-1] == 'kernel':
                imgs['__compression__'] = {'vo': vo, 'len_off': o - ln - 8, 'len': ln, 'val': v}
        elif t == 9:
            break
    return imgs, tot, off_str, lst


def grow_compression(d):
    """把 kernel 的 compression 值从 'lzo' 扩成 'gzip'（属性长度 4→5，4 字节对齐后 +4 字节）"""
    imgs, tot, off_str, lst = parse_fdt(d)
    c = imgs['__compression__']
    assert c['val'] == b'lzo\0', c['val']
    vo, len_off = c['vo'], c['len_off']
    insert_at = vo + ((c['len'] + 3) & ~3)      # 属性值 4 字节对齐后的末尾
    out = bytearray(d[:insert_at]) + bytearray(4) + bytearray(d[insert_at:])
    # 写新值：len=5, "gzip\0" + 3 字节填充
    struct.pack_into('>I', out, len_off, 5)
    out[vo:vo + 8] = b'gzip\0\0\0\0'
    # 头部：totalsize / off_strings / size_struct 各 +4
    struct.pack_into('>I', out, 4, tot + 4)
    struct.pack_into('>I', out, 12, off_str + 4)
    struct.pack_into('>I', out, 36, lst + 4)
    return bytes(out)


def build():
    src = bytearray(open(SRC, 'rb').read())
    dtb = grow_compression(src[:0x4ef6])
    print(f"DTB: {0x4ef6} -> {len(dtb)} 字节（+{len(dtb)-0x4ef6}）")

    imgs, *_ = parse_fdt(dtb)
    order = sorted([k for k in imgs if not k.startswith('__')],
                   key=lambda k: imgs[k]['pos'])
    print(f"镜像 {len(order)} 个，数据区起点 {DATA_START:#x}")

    out = bytearray(dtb)
    out += src[0x4ef6:0x4ef6 + STIF_LEN]        # STIF 块随 DTB 后移
    out += b'\0' * (DATA_START - len(out))      # 填充到原数据区起点
    assert len(out) == DATA_START, hex(len(out))

    gz = open(GZ, 'rb').read()
    cursor = DATA_START
    for name in order:
        m = imgs[name]
        if name == 'kernel':
            data = gz
        else:
            data = bytes(src[m['pos']:m['pos'] + m['size']])
        struct.pack_into('>I', out, m['pos_off'], cursor)
        struct.pack_into('>I', out, m['size_off'], len(data))
        if m.get('hash_off'):
            out[m['hash_off']:m['hash_off'] + 32] = hashlib.sha256(data).digest()
        out += data
        cursor += len(data)
        pad = (-cursor) % 4
        if pad:
            out += b'\0' * pad
            cursor += pad

    open(OUT, 'wb').write(out)
    print(f"输出 {OUT}: {len(out)} 字节  sha256 {hashlib.sha256(out).hexdigest()[:16]}")

    # 自检
    chk, *_ = parse_fdt(out[:len(dtb)])
    ok = all(hashlib.sha256(bytes(out[chk[n]['pos']:chk[n]['pos'] + chk[n]['size']])).digest()
             == bytes(out[chk[n]['hash_off']:chk[n]['hash_off'] + 32])
             for n in chk if not n.startswith('__') and chk[n].get('hash_off'))
    print("hash-1 全对:", ok)
    print("kernel compression:", chk['__compression__']['val'][:6])
    print("kernel data-size:", chk['kernel']['size'], " 期望:", len(gz))
    return 0


if __name__ == '__main__':
    sys.exit(build())
