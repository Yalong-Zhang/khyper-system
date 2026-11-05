param([int]$Port = 0, [switch]$NoBrowser)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONPYCACHEPREFIX = Join-Path $PSScriptRoot '.runtime\python'
if ($Port -ne 0) { $env:KHYPER_PORT = "$Port" }
$python = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) { & (Join-Path $PSScriptRoot 'setup.ps1') }
elseif ((-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot '.runtime\build\Release\khyper_worker.exe'))) -or (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot '.venv\Lib\site-packages\sitecustomize.py')))) { & (Join-Path $PSScriptRoot 'setup.ps1') -SkipInstall }
if ($NoBrowser) { & $python (Join-Path $PSScriptRoot 'backend\serve.py') --no-browser }
else { & $python (Join-Path $PSScriptRoot 'backend\serve.py') }
exit $LASTEXITCODE
