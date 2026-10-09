#!/usr/bin/env python3
# license:BSD-3-Clause
"""Domino（takabosoft）の音源定義ファイルに、架空の FC ボードの音色を入れる。

Domino のトラックの音色欄から FC ボードのプログラムを選べるようにするための道具。
選ぶと、バンクセレクト（MSB 90〜95・LSB 0）とプログラムチェンジが入る。

  python tools/domino/fcdef.py out.xml
      FC ボードだけの定義ファイルを作る（音色の並びと、基本のコントロールチェンジ）。

  python tools/domino/fcdef.py --into S-MU2000.xml out.xml
      いま使っている定義ファイル（dominogen で作ったものなど）の写しに、FC ボードの並びを足す。
      XG の音色と FC ボードの音色を、同じ定義の中で選べる。元のファイルは書き換えない。

入るもの（Domino の音色を選ぶ画面の「マップ」）:

  FC BOARD PLG-1 (MSB 90) 〜 PLG-6 (MSB 95)   1 パートの FC ボード。差込口ごとにバンクが違う
  FC BOARD 16 parts (port E)                   16 パートの FC ボード。バンクは送らない（プログラムチェンジだけ）

音色の名前は、初期の 16 個（doc/vboard.md）。自分で作った音色の組（.smufc）を使っているボードは、
その名前で 128 個並べられる:

  --set 1=<ファイル>      PLG-1 のボードの音色の組
  --set 16=<ファイル>     16 パートのボードの音色の組
  --ini <gui.ini>         設定ファイルの board_fc=・board_fc2=…・board_fc16= から読む
                          （省くと %LOCALAPPDATA%/S-MU2000/gui.ini を探す。--no-ini で読まない）

音色の名前は全部ここ（S-MU2000）で付けたものか、使う人が付けたものなので、出来たファイルは配って差し支えない。
ただし --into で足した先のファイルに ROM から読んだ音色名が入っているなら、そちらの決まりに従うこと。
"""
import argparse
import os
import sys

DEFAULT_NAMES = [
    "Square50", "Square25", "Square12", "Triangle", "Noise", "MetalNz", "DutySwp", "OctArp",
    "Sq50 Dcy", "Sq25 Dcy", "Sq12 Dcy", "Tri Dcy", "NoiseDcy", "MetalDcy", "SweepDcy", "ArpDcy",
]
SLOTS = 6                       # PLG-1〜6（4 から先は増設の差込口）
BANK_MSB = 90                   # mu2000::VBOARD_BANK_MSB。差込口ごとに +1
INI_KEYS = {1: "board_fc", 2: "board_fc2", 3: "board_fc3", 4: "board_fc4", 5: "board_fc5", 6: "board_fc6", 16: "board_fc16"}


def read_set(path):
    """.smufc（src/vboard.h の write_fc_bank）から、組の名前と 128 個の音色名を読む。"""
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 28 + 128 * 32 or d[:8] != b"SMUFCBNK" or d[8:12] != b"\x01\x00\x00\x00":
        raise ValueError("%s: FC ボードの音色の組ではない" % path)
    title = d[12:28].split(b"\0")[0].decode("ascii", "replace").strip()
    names = []
    for i in range(128):
        raw = d[28 + i * 32:28 + i * 32 + 8]
        names.append("".join(chr(c) if 0x20 <= c < 0x7f else "?" for c in raw).rstrip())
    return title, names


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def one_map(title, names, msb):
    """マップ 1 つ。msb が None ならバンクを書かない（プログラムチェンジだけ送る）。"""
    out = ['\t\t<Map Name="%s">' % esc(title)]
    for i, name in enumerate(names):
        label = "%03d %s" % (i + 1, name if name else "(empty)")
        bank = '' if msb is None else ' MSB="%d" LSB="0"' % msb
        out.append('\t\t\t<PC Name="%s" PC="%d">' % (esc(label), i + 1))
        out.append('\t\t\t\t<Bank Name="%s"%s />' % (esc(label), bank))
        out.append('\t\t\t</PC>')
    out.append('\t\t</Map>')
    return out


def maps(sets):
    out = []
    for slot in range(1, SLOTS + 1):
        title, names = sets.get(slot, (None, DEFAULT_NAMES))
        head = "FC BOARD PLG-%d (MSB %d)" % (slot, BANK_MSB + slot - 1)
        out += one_map(head + (" - " + title if title else ""), names, BANK_MSB + slot - 1)
    title, names = sets.get(16, (None, DEFAULT_NAMES))
    out += one_map("FC BOARD 16 parts (port E)" + (" - " + title if title else ""), names, None)
    return "\r\n".join(out) + "\r\n"


# FC ボードだけの定義に入れる、基本のコントロールチェンジ。ID は Domino に同梱の GM の定義と同じ
# （ID で曲に覚えられるので、ほかの定義に替えても名前が出るように）
CONTROLS = """\t<ControlChangeMacroList>
\t\t<Folder Name="FC board">
\t\t\t<CCM ID="130" Name="PitchBend" Color="#0fa806" Sync="Last">
\t\t\t\t<Value Min="-8192" Max="8191" Offset="8192" />
\t\t\t\t<Data>@PB #VH #VL</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="1" Name="Modulation (vibrato)" Color="#1c1cfb" Sync="Last">
\t\t\t\t<Value />
\t\t\t\t<Data>@CC 1 #VL</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="7" Name="Part Level" Color="#CA0000" Sync="Last">
\t\t\t\t<Value Default="100" />
\t\t\t\t<Data>@CC 7 #VL</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="11" Name="Expression" Color="#CA0000" Sync="Last">
\t\t\t\t<Value />
\t\t\t\t<Data>@CC 11 #VL</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="10" Name="Part Panpot" Color="#008080" Sync="Last">
\t\t\t\t<Value Min="-64" Max="63" Offset="64" />
\t\t\t\t<Data>@CC 10 #VL</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="91" Name="Reverb Send" Sync="Last">
\t\t\t\t<Value Default="40" />
\t\t\t\t<Data>@CC 91 #VL</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="93" Name="Chorus Send" Sync="Last">
\t\t\t\t<Value />
\t\t\t\t<Data>@CC 93 #VL</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="120" Name="AllSoundOff">
\t\t\t\t<Data>@CC 120 0</Data>
\t\t\t</CCM>
\t\t\t<CCM ID="123" Name="AllNoteOff">
\t\t\t\t<Data>@CC 123 0</Data>
\t\t\t</CCM>
\t\t</Folder>
\t</ControlChangeMacroList>
""".replace("\n", "\r\n")


def standalone(sets):
    return ('<?xml version="1.0" encoding="Shift_JIS"?>\r\n\r\n'
            '<ModuleData Name="S-MU2000 FC board" Folder="YAMAHA" Priority="1" FileCreator="S-MU2000 tools/domino/fcdef.py" '
            'FileVersion="1.00" WebSite="https://github.com/tarboh/S-MU2000">\r\n'
            '\t<InstrumentList>\r\n' + maps(sets) + '\t</InstrumentList>\r\n' + CONTROLS + '</ModuleData>\r\n')


def merged(path, sets, name):
    with open(path, "rb") as f:
        text = f.read().decode("cp932")
    end = text.find("</InstrumentList>")
    if end < 0:
        raise ValueError("%s: <InstrumentList> が無い" % path)
    if "FC BOARD PLG-1" in text:
        raise ValueError("%s: もう FC ボードの並びが入っている（元の定義ファイルを渡すこと）" % path)
    line = text.rfind("\n", 0, end) + 1          # </InstrumentList> の行の頭に入れる
    text = text[:line] + maps(sets) + text[line:]
    # 元の定義と並べて選べるよう、定義の名前を変える
    at = text.find("<ModuleData ")
    q0 = text.find('Name="', at) + 6
    q1 = text.find('"', q0)
    if at < 0 or q0 < 6 or q1 < 0:
        raise ValueError("%s: <ModuleData Name=...> が無い" % path)
    return text[:q0] + esc(name if name else text[q0:q1] + " + FC board") + text[q1:]


def ini_sets(path):
    found = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            kv = dict(l.rstrip("\r\n").split("=", 1) for l in f if "=" in l)
    except OSError:
        return found
    for slot, key in INI_KEYS.items():
        p = kv.get(key, "")
        if p and os.path.isfile(p):
            found[slot] = p
    return found


def main():
    ap = argparse.ArgumentParser(description="Domino instrument definition for the S-MU2000 FC boards")
    ap.add_argument("out")
    ap.add_argument("--into", metavar="XML", help="copy this definition and add the FC board maps to it")
    ap.add_argument("--name", help="module name shown in Domino (default: the original name + ' + FC board')")
    ap.add_argument("--set", action="append", default=[], metavar="N=FILE", help="voice set (.smufc) of slot N (1-6) or of the 16-part board (16)")
    ap.add_argument("--ini", help="gui.ini to read board_fc*= from")
    ap.add_argument("--no-ini", action="store_true", help="do not look for gui.ini")
    a = ap.parse_args()

    paths = {}
    if not a.no_ini:
        ini = a.ini or os.path.join(os.environ.get("LOCALAPPDATA", ""), "S-MU2000", "gui.ini")
        paths.update(ini_sets(ini))
    for s in a.set:
        n, _, p = s.partition("=")
        if not n.isdigit() or int(n) not in INI_KEYS or not p:
            ap.error("--set は N=ファイル（N は 1〜6 か 16）: %s" % s)
        paths[int(n)] = p
    sets = {}
    for slot, p in sorted(paths.items()):
        sets[slot] = read_set(p)
        print("%s: %s (%s)" % ("16 parts" if slot == 16 else "PLG-%d" % slot, sets[slot][0], p))

    if os.path.abspath(a.out) == os.path.abspath(a.into or ""):
        ap.error("出力は元のファイルと別の名前にすること")
    text = merged(a.into, sets, a.name) if a.into else standalone(sets)
    with open(a.out, "wb") as f:
        f.write(text.encode("cp932", "replace"))
    print("wrote %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
