# Copyright 2026 Courville Software
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Single source of truth for where things live, so tests never count ``parents[N]``."""
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parents[1]      # <repo>/test
REPO_ROOT = TEST_DIR.parent                         # <repo>
SOURCE_DIR = REPO_ROOT / 'Source'
INCLUDE_DIR = REPO_ROOT / 'Include'
DEVICE_DIR = TEST_DIR / 'device'                    # adb harnesses + log analyzers
FIXTURES_DIR = TEST_DIR / 'fixtures'
