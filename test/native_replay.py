"""Compile small replays of production C; preserve compiler/assertion failures."""
import os
from pathlib import Path
import subprocess
import tempfile

from test_video_pcm_startup import block

ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / 'Source' / name).read_text()


def extract(text, marker):
    try:
        return block(text, marker)
    except (ValueError, IndexError) as error:
        raise AssertionError(f'Production replay marker no longer matches: {marker}') from error


def run_c(code):
    with tempfile.TemporaryDirectory(prefix='avos-replay-') as directory:
        path = Path(directory) / 'replay.c'
        binary = path.with_suffix('')
        path.write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-pthread', '-I' + str(ROOT / 'Include'),
                        '-I' + str(ROOT / 'common/Include'), str(path), '-o', str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
