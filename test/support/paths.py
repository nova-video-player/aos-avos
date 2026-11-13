"""Single source of truth for where things live, so tests never count ``parents[N]``."""
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parents[1]      # <repo>/test
REPO_ROOT = TEST_DIR.parent                         # <repo>
SOURCE_DIR = REPO_ROOT / 'Source'
INCLUDE_DIR = REPO_ROOT / 'Include'
DEVICE_DIR = TEST_DIR / 'device'                    # adb harnesses + log analyzers
FIXTURES_DIR = TEST_DIR / 'fixtures'
