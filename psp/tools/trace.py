#!/usr/bin/env python3
"""Shows what the game was doing when it froze, from the trace.bin it writes with
trace = 1 in settings.ini, using the unstripped pokeemerald32.elf of the same build.

usage: psp/tools/trace.py trace.bin pokeemerald32.elf   (needs pspdev; PSPDEV defaults to ~/pspdev)"""
import struct, subprocess, sys, os
data = open(sys.argv[1], 'rb').read()
elf = sys.argv[2]
words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
assert words[0] == 0x45435254, 'not a trace'
steps = ['?', 'game (MainLoop)', 'drawing', 'DMAs and VBlank', 'sound engine', 'GU and display', 'menu', 'writing the save']
d = words[2:]
names = {}
addrs = [a for a in d[2:5] + d[5:21] + d[21:22] if a]
if addrs:
    out = subprocess.run([os.path.join(os.environ.get('PSPDEV', os.path.expanduser('~/pspdev')), 'bin', 'psp-addr2line'), '-f', '-s', '-e', elf] + ['%x' % a for a in addrs],
                         capture_output=True, text=True).stdout.split('\n')
    for i, a in enumerate(addrs):
        names[a] = f"{out[2*i]} ({out[2*i+1]})"
def n(a): return '-' if a == 0 else f"{a:08x} {names.get(a, '')}"
print(f"record {words[1]}, frame {d[1]}, step: {steps[d[0]] if d[0] < len(steps) else d[0]}")
print("running  ", n(d[21]))
print("callback1", n(d[2])); print("callback2", n(d[3])); print("vblank   ", n(d[4]))
print("tasks:"); [print("  ", n(a)) for a in d[5:21] if a]
