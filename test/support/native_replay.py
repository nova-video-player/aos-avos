"""Compile small replays of production C; preserve compiler/assertion failures."""
import os
from pathlib import Path
import subprocess
import tempfile

from paths import REPO_ROOT

ROOT = REPO_ROOT


def block(source, marker):
    """Return ``source`` from ``marker`` through the matching closing brace."""
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def slew_helpers(source):
    """The bounded renderer-offset arithmetic section of codec_sfdec2.c."""
    start = source.index('// Bounded renderer-offset arithmetic.')
    end = source.index('// Qualify the retained anchor,', start)
    return source[start:end]


def typedef(text, name):
    """The ``typedef struct { ... } name;`` declaration for ``name``."""
    end = text.index('} ' + name + ';') + len('} ' + name + ';')
    return text[text.rfind('typedef struct {', 0, end):end]


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
