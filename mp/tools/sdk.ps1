# Rebuild the patched SDK and copy the runtime DLLs next to bioshock.exe.
#   .\tools\sdk.ps1
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$log  = Join-Path $Root ("logs\sdk_build_{0}.log" -f (Get-Date -Format yyyyMMdd_HHmmss))
cmd /c "cmake --build `"$Root\.deps\rexglue-sdk\out\build\win-amd64`" --config Release --target install 2>&1" | Out-File -Encoding utf8 $log
$errs = Select-String $log -Pattern 'error:' | ForEach-Object Line
if ($LASTEXITCODE -ne 0 -or $errs) { $errs; throw "SDK build failed. See $log" }
Select-String $log -Pattern 'Building CXX' | ForEach-Object Line
$bin = Join-Path $Root ".deps\rexglue-sdk\out\install\win-amd64\bin"
$dst = Join-Path $Root "app\out\build\win-amd64-relwithdebinfo"
Copy-Item "$bin\rexruntime.dll", "$bin\rexgpu-xenos.dll" $dst -Force
Write-Host ("Copied DLLs at {0}" -f (Get-Item "$dst\rexgpu-xenos.dll").LastWriteTime) -ForegroundColor Green
