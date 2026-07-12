param(
    [string]$EmberCli = "buildt/ember_cli.exe",
    [int]$TimeoutSeconds = 30,
    [switch]$IncludeNegative
)

# Differential runner for the self-hosted Ember compiler.
# Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1
#   powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_correctness_audit.ps1 -IncludeNegative
#
# PowerShell is intentional: unlike a POSIX shell on Windows, it preserves the
# full signed process exit value used as Ember's i64 result channel.

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$cli = (Resolve-Path (Join-Path $repo $EmberCli)).Path
$tests = $PSScriptRoot

function Invoke-Captured {
    param([string]$Exe, [string[]]$Arguments, [string]$InputText = "")
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    $psi.WorkingDirectory = $repo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    # Windows PowerShell 5.1 lacks ProcessStartInfo.ArgumentList. These paths
    # contain no quotes; quote every argument for spaces and invoke through the
    # classic Arguments string instead.
    $quoted = foreach ($arg in $Arguments) { '"' + $arg.Replace('"', '\"') + '"' }
    $psi.Arguments = $quoted -join ' ' 
    $p = [System.Diagnostics.Process]::new()
    $p.StartInfo = $psi
    [void]$p.Start()
    if ($InputText.Length -gt 0) { $p.StandardInput.WriteLine($InputText) }
    $p.StandardInput.Close()
    $stdoutTask = $p.StandardOutput.ReadToEndAsync()
    $stderrTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        $p.Kill($true)
        $p.WaitForExit()
        return [pscustomobject]@{ TimedOut=$true; ExitCode=$null; Stdout=$stdoutTask.Result; Stderr=$stderrTask.Result }
    }
    return [pscustomobject]@{ TimedOut=$false; ExitCode=$p.ExitCode; Stdout=$stdoutTask.Result; Stderr=$stderrTask.Result }
}

function Decode-EmberResult([int64]$Raw) {
    # ember_cli clamps to a 31-bit process result; negative i64 values arrive
    # through Windows as 0x7ffffxxx and are decoded back here for reporting.
    if ($Raw -gt 1073741824) { return $Raw - 2147483648 }
    return $Raw
}

function Expected-Value([string]$Path) {
    $m = Select-String -Path $Path -Pattern '^\s*//\s*expect:\s*(-?[0-9]+)\s*$' | Select-Object -First 1
    if ($null -eq $m) { throw "missing // expect: N in $Path" }
    return [int64]$m.Matches[0].Groups[1].Value
}

$positive = Get-ChildItem $tests -Filter '*.ember' | Where-Object { $_.Name -notlike 'reject_*' -and $_.Name -ne 'diagnose_file.ember' -and $_.Name -ne 'file_pipeline_runner.ember' } | Sort-Object Name
$pass = 0; $fail = 0; $hang = 0
Write-Host "Self-hosted differential correctness audit ($($positive.Count) positive programs)"
foreach ($test in $positive) {
    $relative = $test.FullName.Substring($repo.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/')
    $expected = Expected-Value $test.FullName
    $native = Invoke-Captured $cli @('run', $relative, '--fn', 'main')
    $self = Invoke-Captured $cli @('run', 'self_hosted/correctness_tests/file_pipeline_runner.ember', '--fn', 'run_file', '--ffi') $relative
    if ($native.TimedOut -or $self.TimedOut) {
        Write-Host "HANG $($test.Name) native=$($native.TimedOut) self=$($self.TimedOut)" -ForegroundColor Red
        $hang++; $fail++; continue
    }
    $nativeDecoded = Decode-EmberResult $native.ExitCode
    $selfDecoded = Decode-EmberResult $self.ExitCode
    $ok = ($nativeDecoded -eq $expected) -and ($selfDecoded -eq $expected) -and ($nativeDecoded -eq $selfDecoded)
    if ($ok) {
        Write-Host "PASS $($test.Name) => $expected"
        $pass++
    } else {
        Write-Host "FAIL $($test.Name): expected=$expected native=$nativeDecoded self=$selfDecoded" -ForegroundColor Red
        if ($native.Stdout) { Write-Host "  native stdout: $($native.Stdout.Trim())" }
        if ($native.Stderr) { Write-Host "  native stderr: $($native.Stderr.Trim())" }
        if ($self.Stdout) { Write-Host "  self stdout: $($self.Stdout.Trim())" }
        if ($self.Stderr) { Write-Host "  self stderr: $($self.Stderr.Trim())" }
        $fail++
    }
}

if ($IncludeNegative) {
    $negative = Get-ChildItem $tests -Filter 'reject_*.ember' | Sort-Object Name
    Write-Host "`nSelf-hosted rejection audit ($($negative.Count) programs)"
    foreach ($test in $negative) {
        $relative = $test.FullName.Substring($repo.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/')
        $tag = (Select-String -Path $test.FullName -Pattern '^\s*//\s*selfhost-reject:\s*(\S+)\s*$' | Select-Object -First 1).Matches[0].Groups[1].Value
        $self = Invoke-Captured $cli @('run', 'self_hosted/correctness_tests/file_pipeline_runner.ember', '--fn', 'run_file', '--ffi') $relative
        if ($self.TimedOut) { Write-Host "HANG $($test.Name)" -ForegroundColor Red; $hang++; $fail++; continue }
        $decoded = Decode-EmberResult $self.ExitCode
        $ok = if ($tag -eq 'negative') { $decoded -lt 0 } else { $decoded -eq [int]$tag }
        if ($ok) { Write-Host "PASS $($test.Name) => $decoded"; $pass++ }
        else { Write-Host "FAIL $($test.Name): wanted=$tag self=$decoded (raw=$($self.ExitCode))" -ForegroundColor Red; $fail++ }
    }
}

Write-Host "`nSUMMARY pass=$pass fail=$fail hangs=$hang"
if ($fail -ne 0) { exit 1 }
exit 0
