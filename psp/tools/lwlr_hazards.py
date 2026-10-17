#!/usr/bin/env python3
"""Lists the places in a PSP program where an lwl/lwr result is used by the very
next instruction. A PSP 3001 gives that instruction the loaded word before it's
merged, so a jump through it goes to the wrong place (see docs/psp.md). PPSSPP
doesn't, so these only fail on a PSP.

usage: psp/tools/lwlr_hazards.py ELF     (needs pspdev; PSPDEV defaults to ~/pspdev)"""
import re, subprocess, sys, os
dis = subprocess.run([os.path.join(os.environ.get('PSPDEV', os.path.expanduser('~/pspdev')), 'bin', 'psp-objdump'), '-d', '--no-show-raw-insn', sys.argv[1]],
                     capture_output=True, text=True).stdout.split('\n')
fn = None; ins = []
for l in dis:
    m = re.match(r'^[0-9a-f]+ <(.*)>:$', l)
    if m: fn = m.group(1); continue
    m = re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$', l)
    if m: ins.append((fn, m.group(1), m.group(2), m.group(3)))
def regs_read(op, args):
    # every register operand except the destination of a write
    regs = re.findall(r'\b(zero|at|v[01]|a[0-3]|t[0-9]|s[0-8]|k[01]|gp|sp|fp|ra)\b', args)
    if op in ('sw','sh','sb','swl','swr','swc1','jr','jalr','beq','bne','beql','bnel','beqz','bnez','beqzl','bnezl',
              'bltz','bgez','blez','bgtz','bltzl','bgezl','blezl','bgtzl','mtc1','mthi','mtlo','mult','multu','div','divu','madd','maddu','msub','msubu'):
        return set(regs)
    return set(regs[1:])  # first operand is the destination
hits = {}
for i in range(len(ins) - 1):
    f, addr, op, args = ins[i]
    if op not in ('lwl', 'lwr'): continue
    dst = args.split(',')[0].strip()
    nf, naddr, nop_, nargs = ins[i + 1]
    if nop_ in ('lwl', 'lwr') and nargs.split(',')[0].strip() == dst: continue  # its other half
    if dst in regs_read(nop_, nargs):
        hits.setdefault(f, []).append(f"{addr}: {op} {args} ; {nop_} {nargs}")
total = sum(len(v) for v in hits.values())
print(f"{total} uses right after lwl/lwr in {len(hits)} functions")
for f, v in sorted(hits.items(), key=lambda x: -len(x[1]))[:40]:
    print(f"  {f}: {len(v)}   e.g. {v[0]}")
