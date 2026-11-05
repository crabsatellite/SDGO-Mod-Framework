"""Turn a hidden GameTest client's recorded clip into a video.

The quiet client writes <clip>_<pid>.raw (top-down BGRA frames) and <clip>_<pid>.idx (one QPC time per frame) when
SDGO_GAME_TEST_RECORD_DIR is set and <dir>\\record.on exists. This tool resamples the frames onto a constant frame rate
using the recorded times (a frame repeats while the game was slower, and is skipped when it was faster), so motion keeps
real speed, then encodes with ffmpeg.

    python tools/game_test_recording.py convert <dir>/<clip>_<pid>.idx --out clip.mp4 [--fps 60] [--crf 14] [--size 1920:1080]
    python tools/game_test_recording.py list <dir>
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys


def read_index(path: Path):
    lines = path.read_text(encoding='ascii').splitlines()
    head = re.match(r'sdgo-recording 1 width=(\d+) height=(\d+) format=bgra qpc_frequency=(\d+)$', lines[0] if lines else '')
    if not head:
        raise ValueError(f'{path} is not an SDGO recording index')
    width, height, frequency = (int(v) for v in head.groups())
    times = []
    for line in lines[1:]:
        if line.startswith('end '):
            break
        number, qpc = line.split()
        if int(number) != len(times):
            raise ValueError(f'{path}: frame numbers are not consecutive at {number}')
        times.append(int(qpc) / frequency)
    if not times:
        raise ValueError(f'{path} holds no frames')
    return width, height, [t - times[0] for t in times]


def schedule(times, fps):
    """Index of the recorded frame to show at each output tick (the latest frame not after the tick)."""
    picks, j = [], 0
    for tick in range(int(times[-1] * fps) + 1):
        t = tick / fps
        while j + 1 < len(times) and times[j + 1] <= t:
            j += 1
        picks.append(j)
    return picks


def convert(index: Path, out: Path, fps=60, crf=14, size=None):
    width, height, times = read_index(index)
    raw = index.with_suffix('.raw')
    frame_bytes = width * height * 4
    frames = raw.stat().st_size // frame_bytes
    if frames < len(times):
        times = times[:frames]
    picks = schedule(times, fps)
    encoder = subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'bgra', '-s', f'{width}x{height}', '-r', str(fps),
                                '-i', '-', '-vf', (f'scale={size}:flags=lanczos' if size else 'scale=trunc(iw/2)*2:trunc(ih/2)*2'),
                                '-c:v', 'libx264', '-preset', 'medium', '-crf', str(crf), '-pix_fmt', 'yuv420p', str(out)],
                               stdin=subprocess.PIPE)
    with raw.open('rb') as source:
        loaded, frame = -1, b''
        for j in picks:
            if j != loaded:
                source.seek(j * frame_bytes)
                frame, loaded = source.read(frame_bytes), j
            encoder.stdin.write(frame)
    encoder.stdin.close()
    if encoder.wait():
        raise RuntimeError('ffmpeg failed')
    gaps = [b - a for a, b in zip(times, times[1:])]
    return {'frames': len(times), 'seconds': round(times[-1], 2), 'width': width, 'height': height,
            'mean_fps': round(len(times) / max(times[-1], 1e-6), 1), 'max_gap_ms': round(max(gaps, default=0) * 1000)}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest='command', required=True)
    c = sub.add_parser('convert')
    c.add_argument('index', type=Path)
    c.add_argument('--out', type=Path, required=True)
    c.add_argument('--fps', type=int, default=60)
    c.add_argument('--crf', type=int, default=14)
    c.add_argument('--size', help='output size such as 1920:1080 (default: source size rounded down to even)')
    l = sub.add_parser('list')
    l.add_argument('folder', type=Path)
    args = p.parse_args(argv)
    if args.command == 'convert':
        print(convert(args.index, args.out, args.fps, args.crf, args.size))
    else:
        for index in sorted(args.folder.glob('*.idx')):
            width, height, times = read_index(index)
            print(f'{index.name}: {width}x{height}, {len(times)} frames, {times[-1]:.1f} s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
