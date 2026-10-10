#!/usr/bin/env python3
"""Summarize a sampling profile of the desktop build (debug command "sample").

    PERF_BUILDID_DIR=<dir> NFSMW_SAMPLE_OUT=<samples.txt> ./build/nfsmw ...   # then "cmd sample"
    tools/profile.py <samples.txt> <dir>/perf-<pid>.map <nfsmw.log> <game lib dir> [--top N]

Where the main thread was: translated game code (by guest function), the
loader's own C++ code, or libraries (Mesa, libc...).
"""
import bisect
import collections
import os
import re
import subprocess
import sys


def load_perf_map(path):
    starts, ends, names = [], [], []
    rows = []
    with open(path) as f:
        for line in f:
            p = line.split(None, 2)
            if len(p) < 3:
                continue
            rows.append((int(p[0], 16), int(p[1], 16), p[2].strip()))
    rows.sort()
    for s, n, name in rows:
        starts.append(s)
        ends.append(s + n)
        names.append(name)
    return starts, ends, names


def load_samples(path):
    samples, maps, in_maps = [], [], False
    with open(path) as f:
        for line in f:
            if line.startswith('# maps'):
                in_maps = True
                continue
            if line.startswith('#'):
                continue
            if in_maps:
                m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \S+\s*(.*)', line)
                if m:
                    maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5)))
            else:
                samples.append(int(line, 16))
    return samples, maps


def module_bases(log):
    bases = {}
    for line in open(log, errors='replace'):
        m = re.search(r'loaded (\S+) at 0x([0-9A-Fa-f]+)', line)
        if m:
            bases[m.group(1)] = int(m.group(2), 16)
    return bases


def symbols(lib_path, base):
    """Function starts of an ARM .so: exported names where there are some,
    'sub_<offset>' for the rest (from the .ARM.exidx unwind table)."""
    out = subprocess.run(['nm', '-D', '--defined-only', lib_path], capture_output=True, text=True).stdout
    named = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tTwW':
            named[int(p[0], 16) & ~1] = p[2]
    starts = set(named)
    data = open(lib_path, 'rb').read()
    hdr = subprocess.run(['readelf', '-SW', lib_path], capture_output=True, text=True).stdout
    for line in hdr.splitlines():
        m = re.search(r'\.ARM\.exidx\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
        if m:
            addr, off, size = (int(x, 16) for x in m.groups())
            for i in range(0, size, 8):
                w = int.from_bytes(data[off + i:off + i + 4], 'little')
                rel = w & 0x7fffffff
                if rel & 0x40000000:
                    rel -= 0x80000000
                starts.add(((addr + i + rel) & 0xffffffff) & ~1)
    syms = sorted((a + base, named.get(a, 'sub_%X' % a)) for a in starts)
    return syms


def main():
    args = sys.argv[1:]
    top = 40
    if '--top' in args:
        i = args.index('--top')
        top = int(args[i + 1])
        del args[i:i + 2]
    samples_path, map_path, log_path, lib_dir = args
    starts, ends, names = load_perf_map(map_path)
    samples, maps = load_samples(samples_path)
    bases = module_bases(log_path)
    mods = sorted((b, n) for n, b in bases.items())
    syms = {n: symbols(os.path.join(lib_dir, n), b) for n, b in bases.items()}
    nfsmw_maps = [m for m in maps if m[3].endswith('/nfsmw')]

    cat = collections.Counter()
    guest_funcs = collections.Counter()
    host_addrs = collections.Counter()
    for a in samples:
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < ends[i]:
            name = names[i]
            m = re.match(r'a32_[at]([0-9A-F]{8})', name)
            if not m:
                cat['jit: ' + re.sub(r'_\d+$', '', name)] += 1
                continue
            pc = int(m.group(1), 16)
            j = bisect.bisect_right(mods, (pc, '\xff')) - 1
            mod = mods[j][1] if j >= 0 else '?'
            cat['game code: ' + mod] += 1
            ss = syms.get(mod, [])
            k = bisect.bisect_right(ss, (pc, '\xff')) - 1
            fn = ss[k][1] if k >= 0 else '?'
            off = pc - ss[k][0] if k >= 0 else 0
            guest_funcs[(mod, fn if off < 0x4000 else '%s+0x%X' % (fn, off & ~0xFFF))] += 1
            continue
        lib = '?'
        for lo, hi, foff, path in maps:
            if lo <= a < hi:
                lib = os.path.basename(path) or '[anon]'
                if path.endswith('/nfsmw'):
                    host_addrs[a - nfsmw_maps[0][0]] += 1
                break
        cat['host: ' + lib] += 1

    n = len(samples)
    print('%d samples' % n)
    for k, v in cat.most_common():
        print('  %5.1f%%  %s' % (100.0 * v / n, k))
    print('\nhot game functions:')
    for (mod, fn), v in guest_funcs.most_common(top):
        print('  %5.1f%%  %s  %s' % (100.0 * v / n, mod, fn))
    if host_addrs and nfsmw_maps:
        exe = nfsmw_maps[0][3]
        addrs = list(host_addrs)
        out = subprocess.run(['addr2line', '-f', '-C', '-e', exe] + ['%x' % a for a in addrs],
                             capture_output=True, text=True).stdout.splitlines()
        by_fn = collections.Counter()
        for idx, a in enumerate(addrs):
            fn = out[2 * idx] if 2 * idx < len(out) else '?'
            by_fn[fn[:110]] += host_addrs[a]
        print('\nhot loader functions:')
        for fn, v in by_fn.most_common(top):
            print('  %5.1f%%  %s' % (100.0 * v / n, fn))


if __name__ == '__main__':
    main()
