# Build the library and the tests with MSVC (cl) and run them.
#
#   powershell tests/ci/msvc.ps1 -Arch x64 [-LargeArgs "5 512 --expect-all"]
#
# This is what the Windows jobs of .github/workflows/ci.yml do, in a Visual
# Studio command prompt for the architecture (the compiler must be on PATH).
# It calls cl directly, as the NMAKE file in win/ does not know the tests.

param(
  [string]$Arch = "x64",
  [string]$LargeArgs = "5 512 --expect-all"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$work = Join-Path $root "ci-work-$Arch"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
Set-Location $work

function Run {
  param([string]$What)
  Write-Host "== $What"
  & $args[0] @($args | Select-Object -Skip 1)
  if ($LASTEXITCODE -ne 0) { throw "FAILED: $What (exit code $LASTEXITCODE)" }
}

$flags = @("/nologo", "/O2", "/EHsc", "/MD", "/W0", "/D_CRT_SECURE_NO_WARNINGS",
  "/I$root\include", "/I$root\src", "/I$root\win")

$lib = "column", "custom", "derived", "fileio", "field", "format", "handler",
  "persist", "remap", "std", "store", "string", "table", "univ", "view", "viewx"
Run "compile library" cl @flags /c @($lib | ForEach-Object { "$root\src\$_.cpp" })
Run "make library" lib /nologo /OUT:mk4.lib @($lib | ForEach-Object { "$_.obj" })

# the programs which are one file
foreach ($t in "twide", "tfuzz", "tcorrupt", "tlarge") {
  Run "build $t" cl @flags "$root\tests\$t.cpp" /Fe:$t.exe /link mk4.lib
}

# the regression suite, which is a number of files
$reg = "regress", "tbasic1", "tbasic2", "tcusto1", "tcusto2", "tdiffer",
  "textend", "tformat", "tlimits", "tmapped", "tnotify", "tresize", "tstore1",
  "tstore2", "tstore3", "tstore4", "tstore5"
Run "build regress" cl @flags @($reg | ForEach-Object { "$root\tests\$_.cpp" }) /Fe:regress.exe /link mk4.lib

New-Item -ItemType Directory tests | Out-Null
Copy-Item "$root\unix\reversed" .
Run "regress" .\regress.exe
# what it wrote must be what is expected (line ends do not count)
git diff --no-index --ignore-cr-at-eol --exit-code "$root\tests\ok" tests
if ($LASTEXITCODE -ne 0) { throw "FAILED: regress output differs from tests/ok" }

Run "twide" .\twide.exe
Run "tfuzz" .\tfuzz.exe
Run "tcorrupt" .\tcorrupt.exe

if ($LargeArgs -ne "") {
  Run "tlarge $LargeArgs" .\tlarge.exe tlarge.mk @($LargeArgs -split " ")
  Remove-Item -Force tlarge.mk, tlarge_ints.mk -ErrorAction SilentlyContinue
}
Write-Host "== all passed ($Arch)"
