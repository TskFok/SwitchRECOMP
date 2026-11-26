# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
$ErrorActionPreference = 'Stop'
python (Join-Path $PSScriptRoot 'run.py') @args
exit $LASTEXITCODE
