"""Compile the production Yaz0 loops with authored fixtures; no game assets.

This isolates decoder behavior, not DVD refill, ARAM DMA, or game loading.
The native executable also runs these fixtures with --yaz0-self-test.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def extract(source, signature):
    start = source.index(signature)
    at = source.index('{', start)
    end, depth = at + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    game = root / 'games/pikmin2'
    kernel = game / 'pikmin2-decomp/src/JSystem/JKernel'
    memory = extract((kernel / 'JKRDecomp.cpp').read_text('utf-8'),
                     'void JKRDecomp::decodeSZS(')
    source = (kernel / 'JKRDvdRipper.cpp').read_text('utf-8')
    # Match the definition, not the forward declaration.
    stream = extract(source, 'static int decompSZS_subroutine(u8* src, u8* dest)')
    seam = extract(source, 'int pc_p2_yaz0_dvd_decoder_test(')
    prefix = r'''
#include "JSystem/JKernel/JKRDecomp.h"
#include <cstdlib>
static u8 *refBuf, *refEnd, *refCurrent, *srcLimit;
static u32 transLeft, fileOffset, readCount, maxDest, *tsPtr;
// Complete fixture input is resident; a refill would violate this test boundary.
static u8* nextSrcData(u8*) { std::abort(); }
'''
    suffix = '\nint pc_p2_yaz0_self_test();\nint main() { return pc_p2_yaz0_self_test(); }\n'
    flags = ['-w', '-std=c++17', '-fpermissive', '-DPIKI_PC_PORT', '-DVERSION_GPVE01',
             '-include', str(game / 'pikmin2-decomp-adapter/pikmin2_pc_types.h')]
    flags += ['-I' + str(game / path) for path in
              ('pikmin2-decomp/include', 'pikmin2-decomp-adapter', 'pc_port')]
    env = dict(os.environ)
    compiler = Path(args.compiler)
    if compiler.is_file():
        env['PATH'] = str(compiler.resolve().parent) + os.pathsep + env.get('PATH', '')
    # Restore just the original raw-length interpretation as a negative control.
    old_memory = memory.replace('JKRDecompExpandSize(src_buffer)',
                                '*(u32*)(src_buffer + 4)')
    old_stream = stream.replace('JKRDecompExpandSize(src)', '*(u32*)(src + 4)')
    if old_memory == memory or old_stream == stream:
        raise SystemExit('Expected production expanded-size accessor not found')
    results = []
    with tempfile.TemporaryDirectory(prefix='pikmin2-yaz0-') as directory:
        out = Path(directory)
        for name, body in [('fixed', memory + '\n' + stream),
                           ('raw-length-negative-control', old_memory + '\n' + old_stream)]:
            cpp, exe = out / (name + '.cpp'), out / (name + '.exe')
            cpp.write_text(prefix + body + '\n' + seam + suffix, encoding='utf-8')
            subprocess.run([args.compiler, *flags, str(cpp),
                            str(game / 'pc_port/pc_p2_yaz0_test.cpp'), '-o', str(exe)],
                           check=True, env=env)
            result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
            print(name, 'exit', result.returncode, result.stdout.strip())
            results.append(result.returncode)
    if results != [0, 2]:
        raise SystemExit('Expected fixed pass and original raw-length failure on little-endian host')


if __name__ == '__main__':
    main()
