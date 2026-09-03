#!/usr/bin/env python3
# Write (and read back) the Finder .DS_Store of the Cably Desktop DMG template
# without Finder: window geometry, icon-view prefs with the background image and
# the icon positions. No third-party modules; the on-disk layout mirrors what
# the ds_store/mac_alias libraries (and therefore dmgbuild) produce, which
# Finder accepts:
#
#   .DS_Store = "Buddy" allocator file: 32-byte header, a 32-byte DSDB
#   superblock at offset 32, the allocator's root block (block ids -> offsets,
#   TOC, 32 free lists) at 2048, and one B-tree leaf node (>= 4096 bytes)
#   holding the records sorted by (lower-cased file name, record code).
#   Records: "." bwsp (window: binary plist), icvl 'icnv', icvp (icon view:
#   binary plist whose backgroundImageAlias is a classic Alias-v2 record of
#   the background file on THIS volume), vSrn 1; one Iloc (x, y) per item.
#
# Usage:
#   make-ds-store.py write --root <mounted volume root> --volume-name NAME \
#       [--background .background.png] [--window X,Y,W,H] [--icon-size 96] \
#       [--text-size 12] --item "Name=x,y" ... [--out <root>/.DS_Store]
#   make-ds-store.py dump <.DS_Store>        # print every record (tests)
#
# Copyright (C) 2026 Cably <dev@cably.dev>
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details. You should have received a copy of the GNU General Public
# License along with this program; see LICENSE.GPLv3 in this repository.
import argparse
import os
import plistlib
import struct
import sys

MAC_EPOCH_OFFSET = 2082844800  # 1904-01-01 -> 1970-01-01, seconds
HEADER_UNKNOWN = bytes.fromhex("0000100c000000870000200b00000000")
PAGE = 4096

# Alias-v2 tag numbers (Alias Manager / mac_alias)
TAG_CARBON_FOLDER_NAME = 0
TAG_CARBON_PATH = 2
TAG_UNICODE_FILENAME = 14
TAG_UNICODE_VOLUME_NAME = 15
TAG_HIGH_RES_VOLUME_CREATION_DATE = 16
TAG_HIGH_RES_CREATION_DATE = 17
TAG_POSIX_PATH = 18
TAG_POSIX_PATH_TO_MOUNTPOINT = 19


def mac_seconds(unix_ts):
    """Classic Mac date: seconds since 1904-01-01 (UTC, as mac_alias/dmgbuild write it:
    KiCad's template carries exactly st_birthtime + 2082844800 in the high-res tags)."""
    return int(unix_ts + MAC_EPOCH_OFFSET)


def pstr(s, width):
    b = s.encode("utf-8")[: width - 1]
    return struct.pack(">B", len(b)) + b.ljust(width - 1, b"\0")


def tag(n, payload):
    return struct.pack(">hh", n, len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")


def alias_v2(volume_name, vol_birth, mount_path, filename, file_cnid, file_birth, folder_cnid):
    """Alias record for <mount>/<filename>, the file sitting at the volume root."""
    voldate = mac_seconds(vol_birth)
    crdate = mac_seconds(file_birth)
    body = struct.pack(">h", 0)                                   # kind: file
    body += pstr(volume_name, 28) + struct.pack(">I2sh", voldate, b"H+", 0)  # date, HFS+, fixed disk
    body += struct.pack(">I", folder_cnid) + pstr(filename, 64)
    body += struct.pack(">II4s4shhI2s10s", file_cnid, crdate, b"\0" * 4, b"\0" * 4, -1, -1, 0, b"\0\0", b"\0" * 10)
    # extras, in the order Finder/mac_alias write them
    body += tag(TAG_CARBON_FOLDER_NAME, volume_name.encode("utf-8"))
    body += struct.pack(">hhQhhQ", TAG_HIGH_RES_VOLUME_CREATION_DATE, 8, voldate << 16,
                        TAG_HIGH_RES_CREATION_DATE, 8, crdate << 16)
    body += tag(TAG_CARBON_PATH, f"{volume_name}:{filename}".encode("utf-8"))
    u = filename.encode("utf-16-be")
    body += struct.pack(">hhh", TAG_UNICODE_FILENAME, len(u) + 2, len(u) // 2) + u
    u = volume_name.encode("utf-16-be")
    body += struct.pack(">hhh", TAG_UNICODE_VOLUME_NAME, len(u) + 2, len(u) // 2) + u
    body += tag(TAG_POSIX_PATH, ("/" + filename).encode("utf-8"))
    body += tag(TAG_POSIX_PATH_TO_MOUNTPOINT, mount_path.encode("utf-8"))
    body += struct.pack(">hh", -1, 0)
    rec = struct.pack(">4shh", b"\0" * 4, 0, 2) + body
    return rec[:4] + struct.pack(">h", len(rec)) + rec[6:]


def encode_record(name, code, typ, value):
    u = name.encode("utf-16-be")
    out = struct.pack(">I", len(u) // 2) + u + code + typ
    if typ == b"blob":
        out += struct.pack(">I", len(value)) + value
    elif typ in (b"long", b"shor"):
        out += struct.pack(">I", value)
    elif typ == b"bool":
        out += struct.pack(">?", value)
    elif typ == b"type":
        out += value
    else:
        raise ValueError(typ)
    return out


def iloc(x, y):
    return struct.pack(">II", x, y) + b"\xff" * 6 + b"\0\0"


def build(records):
    """records: list of (name, code, type, value) -> bytes of a whole .DS_Store."""
    records = sorted(records, key=lambda r: (r[0].lower(), r[1]))
    node = struct.pack(">II", 0, len(records)) + b"".join(encode_record(*r) for r in records)
    width = max(12, (len(node) - 1).bit_length())      # node block: 4096, 8192, ...
    node_off = 1 << width
    # free lists of a fresh file (header 0..32, DSDB 32..64, root 2048..4096), minus the node block
    free = [[] for _ in range(32)]
    for w in range(6, 11):
        free[w].append(1 << w)
    for w in range(12, 31):
        if w != width:
            free[w].append(1 << w)
    offsets = [2048 | 11, 32 | 5, node_off | width]
    root = struct.pack(">II", len(offsets), 0)
    root += b"".join(struct.pack(">I", o) for o in offsets) + b"\0" * (4 * (256 - len(offsets)))
    root += struct.pack(">I", 1) + struct.pack(">B4sI", 4, b"DSDB", 1)
    for lst in free:
        root += struct.pack(">I", len(lst)) + b"".join(struct.pack(">I", o) for o in lst)
    assert len(root) <= 2048
    out = bytearray(4 + node_off + (1 << width))
    out[0:36] = struct.pack(">I4sIII16s", 1, b"Bud1", 2048, 2048, 2048, HEADER_UNKNOWN)
    out[36:56] = struct.pack(">IIIII", 2, 0, len(records), 1, PAGE)   # DSDB at 32(+4)
    out[2052:2052 + len(root)] = root
    out[node_off + 4:node_off + 4 + len(node)] = node
    return bytes(out)


def write(args):
    root = os.path.abspath(args.root)
    bg = os.path.join(root, args.background)
    st_root, st_bg = os.stat(root), os.stat(bg)
    x, y, w, h = (int(v) for v in args.window.split(","))
    bwsp = {"ContainerShowSidebar": False, "PreviewPaneVisibility": False, "ShowPathbar": False,
            "ShowSidebar": False, "ShowStatusBar": False, "ShowTabView": False, "ShowToolbar": False,
            "SidebarWidth": 180, "WindowBounds": "{{%d, %d}, {%d, %d}}" % (x, y, w, h)}
    icvp = {"arrangeBy": "none", "backgroundColorBlue": 1.0, "backgroundColorGreen": 1.0,
            "backgroundColorRed": 1.0, "backgroundType": 2,
            "backgroundImageAlias": alias_v2(args.volume_name, st_root.st_birthtime, args.mount_path,
                                             args.background, st_bg.st_ino, st_bg.st_birthtime, st_root.st_ino),
            "gridOffsetX": 0.0, "gridOffsetY": 0.0, "gridSpacing": 100.0, "iconSize": float(args.icon_size),
            "labelOnBottom": True, "scrollPositionX": 0.0, "scrollPositionY": 0.0,
            "showIconPreview": False, "showItemInfo": False, "textSize": float(args.text_size),
            "viewOptionsVersion": 1}
    records = [(".", b"bwsp", b"blob", plistlib.dumps(bwsp, fmt=plistlib.FMT_BINARY)),
               (".", b"icvl", b"type", b"icnv"),
               (".", b"icvp", b"blob", plistlib.dumps(icvp, fmt=plistlib.FMT_BINARY)),
               (".", b"vSrn", b"long", 1)]
    for item in args.item:
        name, pos = item.rsplit("=", 1)
        ix, iy = (int(v) for v in pos.split(","))
        records.append((name, b"Iloc", b"blob", iloc(ix, iy)))
    data = build(records)
    out = args.out or os.path.join(root, ".DS_Store")
    with open(out, "wb") as f:
        f.write(data)
    print(f"make-ds-store: wrote {out} ({len(data)} bytes, {len(records)} records, "
          f"background alias -> {args.volume_name}:{args.background} cnid {st_bg.st_ino})")


# --- reader (dump) ---------------------------------------------------------------

def parse_alias(b):
    appinfo, length, version = struct.unpack(">4shh", b[:8])
    out = {"version": version, "length": length}
    if version != 2:
        return out
    (kind, volname, voldate, fstype, disktype, folder_cnid, fname, cnid, crdate, creator, ftype,
     lfrom, lto, attrs, fsid, _res) = struct.unpack(">h28pI2shI64pII4s4shhI2s10s", b[8:8 + 142])
    out.update(kind=kind, volume=volname.decode("utf-8", "replace"), voldate=voldate, fstype=fstype,
               folder_cnid=folder_cnid, filename=fname.decode("utf-8", "replace"), cnid=cnid, crdate=crdate)
    p, tags = 8 + 142, {}
    while p + 4 <= len(b):
        t, n = struct.unpack(">hh", b[p:p + 4])
        if t == -1:
            break
        v = b[p + 4:p + 4 + n]
        p += 4 + n + (n & 1)
        if t in (TAG_UNICODE_FILENAME, TAG_UNICODE_VOLUME_NAME):
            v = v[2:].decode("utf-16-be", "replace")
        elif t in (TAG_HIGH_RES_VOLUME_CREATION_DATE, TAG_HIGH_RES_CREATION_DATE):
            v = struct.unpack(">Q", v)[0] >> 16
        else:
            v = v.decode("utf-8", "replace")
        tags[t] = v
    out["tags"] = tags
    return out


def read_records(data):
    magic1, magic2, off, size, off2 = struct.unpack(">I4sIII", data[:20])
    assert magic1 == 1 and magic2 == b"Bud1" and off == off2, "not a .DS_Store"
    root = data[off + 4:off + 4 + size]
    count, _unk = struct.unpack(">II", root[:8])
    offsets = list(struct.unpack(">%dI" % count, root[8:8 + 4 * count]))
    p = 8 + 4 * ((count + 255) & ~255)
    toccount = struct.unpack(">I", root[p:p + 4])[0]
    p += 4
    toc = {}
    for _ in range(toccount):
        n = root[p]
        toc[root[p + 1:p + 1 + n]] = struct.unpack(">I", root[p + 1 + n:p + 5 + n])[0]
        p += 5 + n
    free = []
    for _ in range(32):
        n = struct.unpack(">I", root[p:p + 4])[0]
        free.append(list(struct.unpack(">%dI" % n, root[p + 4:p + 4 + 4 * n])))
        p += 4 + 4 * n

    def block(i):
        a = offsets[i]
        o, w = a & ~0x1F, a & 0x1F
        return data[o + 4:o + 4 + (1 << w)]

    sb = block(toc[b"DSDB"])
    rootnode, levels, nrec, nnodes, pagesize = struct.unpack(">IIIII", sb[:20])
    recs = []

    def walk(bid):
        nb = block(bid)
        nxt, cnt = struct.unpack(">II", nb[:8])
        q = 8
        for _ in range(cnt):
            if nxt:
                child = struct.unpack(">I", nb[q:q + 4])[0]
                q += 4
                walk(child)
            n = struct.unpack(">I", nb[q:q + 4])[0]
            name = nb[q + 4:q + 4 + 2 * n].decode("utf-16-be")
            q += 4 + 2 * n
            code, typ = nb[q:q + 4], nb[q + 4:q + 8]
            q += 8
            if typ == b"blob":
                n = struct.unpack(">I", nb[q:q + 4])[0]
                val = nb[q + 4:q + 4 + n]
                q += 4 + n
            elif typ in (b"long", b"shor"):
                val = struct.unpack(">I", nb[q:q + 4])[0]
                q += 4
            elif typ == b"bool":
                val = bool(nb[q])
                q += 1
            elif typ == b"type":
                val = nb[q:q + 4]
                q += 4
            elif typ == b"ustr":
                n = struct.unpack(">I", nb[q:q + 4])[0]
                val = nb[q + 4:q + 4 + 2 * n].decode("utf-16-be")
                q += 4 + 2 * n
            elif typ in (b"comp", b"dutc"):
                val = struct.unpack(">Q", nb[q:q + 8])[0]
                q += 8
            else:
                raise ValueError(f"unknown record type {typ!r}")
            recs.append((name, code, typ, val))
        if nxt:
            walk(nxt)

    walk(rootnode)
    return {"records": recs, "levels": levels, "count": nrec, "nodes": nnodes, "free": free, "offsets": offsets}


def dump(args):
    data = open(args.file, "rb").read()
    info = read_records(data)
    print(f"{args.file}: {len(data)} bytes, {info['count']} records, {info['nodes']} node(s), levels {info['levels']}")
    for name, code, typ, val in info["records"]:
        if code == b"Iloc":
            x, y = struct.unpack(">II", val[:8])
            print(f"  {name!r} Iloc x={x} y={y}")
        elif typ == b"blob" and val.startswith(b"bplist"):
            pl = plistlib.loads(val)
            keys = []
            for k, v in sorted(pl.items()):
                if k == "backgroundImageAlias":
                    a = parse_alias(v)
                    v = f"<alias v{a.get('version')} {a.get('volume')!r}:{a.get('filename')!r} cnid {a.get('cnid')} tags {a.get('tags')}>"
                keys.append(f"{k}={v}")
            print(f"  {name!r} {code.decode()} {{{', '.join(keys)}}}")
        else:
            print(f"  {name!r} {code.decode()} {typ.decode()} {val!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write")
    w.add_argument("--root", required=True, help="mounted template volume root")
    w.add_argument("--volume-name", required=True)
    w.add_argument("--mount-path", default=None, help="where Finder will mount it (default /Volumes/<volume name>)")
    w.add_argument("--background", default=".background.png", help="file at the volume root")
    w.add_argument("--window", default="100,100,660,400", help="X,Y,W,H of the Finder window")
    w.add_argument("--icon-size", type=int, default=96)
    w.add_argument("--text-size", type=int, default=12)
    w.add_argument("--item", action="append", default=[], help='"Name=x,y" icon centre; repeatable')
    w.add_argument("--out", default=None)
    w.set_defaults(func=write)
    d = sub.add_parser("dump")
    d.add_argument("file")
    d.set_defaults(func=dump)
    args = ap.parse_args()
    if args.cmd == "write" and args.mount_path is None:
        args.mount_path = "/Volumes/" + args.volume_name
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
