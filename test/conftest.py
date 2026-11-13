"""Shared pytest configuration for the AVOS test suite.

Environment understood by the whole suite:

    FFMPEG_PREFIX   install prefix of FFmpeg >= 7.1 (the sources need sws_free_context and the
                    AV_CHAN_SIDE_SURROUND_* constants). Exported to pkg-config and the dynamic
                    loader for every test, so tests never need PKG_CONFIG_PATH / LD_LIBRARY_PATH.
    CC              C compiler used by tests that compile C (default: cc)
"""
import os
import shutil
from pathlib import Path

import pytest

def _prepend(var, path):
    current = os.environ.get(var, '')
    if path not in current.split(os.pathsep):
        os.environ[var] = path + (os.pathsep + current if current else '')


def pytest_configure(config):
    prefix = os.environ.get('FFMPEG_PREFIX')
    if prefix:
        _prepend('PKG_CONFIG_PATH', str(Path(prefix) / 'lib' / 'pkgconfig'))
        _prepend('LD_LIBRARY_PATH', str(Path(prefix) / 'lib'))
        _prepend('DYLD_LIBRARY_PATH', str(Path(prefix) / 'lib'))


def pytest_collection_modifyitems(config, items):
    """Tag every test with its type, derived from where the file lives."""
    for item in items:
        parts = Path(str(item.path)).parts
        for kind in ('unit', 'replay', 'integration', 'tooling'):
            if kind in parts[parts.index('test') + 1:]:
                item.add_marker(getattr(pytest.mark, kind))
                break


@pytest.fixture(scope='session')
def cc():
    """The C compiler the suite uses (honours $CC, which may contain arguments)."""
    return os.environ.get('CC', 'cc')


@pytest.fixture(scope='session')
def require_tool():
    """Skip (never silently pass) when an external tool a test needs is missing."""
    def _require(name):
        if shutil.which(name) is None:
            pytest.skip(f'{name} is not installed')
    return _require
