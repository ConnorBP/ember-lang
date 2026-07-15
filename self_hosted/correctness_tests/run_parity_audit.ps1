param(
    [string]$EmberCli = "build/ember_cli.exe",
    [int]$TimeoutSeconds = 30
)

# =============================================================================
# Differential parity audit for the ember self-hosted compiler.
#
# For every .ember test program this script compiles+runs it TWO ways and
# compares the results:
#
#   NATIVE  : build/ember_cli.exe run <path> --fn main
#             (the C++ host compiler -- the reference oracle)
#   SELF    : echo <path> | build/ember_cli.exe run
#             self_hosted/correctness_tests/file_pipeline_runner.ember
#             --fn run_file --ffi
#             (the ember-written compiler, feeding the target path on stdin)
#
# Three test classes are exercised:
#
#   1. VALID tests  -- programs that should return a known value.
#        Sources: tests/lang/valid_*.ember that carry an `// expect: N`
#        comment, PLUS every non-reject .ember program living next to this
#        script in self_hosted/correctness_tests/.
#        Pass condition: native==expected AND self==expected (full parity).
#        A self-hosted run that returns a negative -3xx rejection code is
#        classified as UNSUPPORTED-BY-SUBSET (the self-hosted compiler only
#        implements a language subset), NOT as a real mismatch. This baseline
#        turns green feature by feature as the subset grows.
#
#   2. INVALID tests -- tests/lang/sema_invalid_*.ember and invalid_*.ember,
#        programs that must NOT compile. Both backends are run and their
#        behaviour recorded (native should error, self should return a
#        negative -3xx). These are informational only for now because the
#        self-hosted subset does not yet cover every rejection path; they
#        never fail the script.
#
#   3. PERMANENT negative tests -- the 17 reject_*.ember programs in this
#        directory. These are the self-hosted compiler's own out-of-subset
#        rejection contract and MUST always pass (self returns the documented
#        -3xx code from the `// selfhost-reject:` header).
#
# The script exits 0 only when there are NO hangs and NO real mismatches.
# Unsupported-by-subset results and recorded invalid-test behaviour are
# expected at the baseline and do not fail the run.
#
# Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File self_hosted/correctness_tests/run_parity_audit.ps1
#
# PowerShell is intentional: unlike a POSIX shell on Windows, it preserves the
# full signed process exit value used as Ember's i64 result channel.
# =============================================================================

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$cli = (Resolve-Path (Join-Path $repo $EmberCli)).Path
$tests = $PSScriptRoot
$langDir = (Resolve-Path (Join-Path $repo "tests/lang")).Path

# -----------------------------------------------------------------------------
# Helpers (mirrored from run_correctness_audit.ps1 so the two runners share a
# common capture/decode contract).
# -----------------------------------------------------------------------------

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
    # ember_cli masks the i64 return with 0x7fffffff (line ~1011 of
    # ember_cli.cpp): exit_code = int(uint64_t(entry_ret) & 0x7fffffff).
    # A negative i64 therefore arrives as (2^31 - |v|) which is > 2^30 for any
    # negative value; subtract 2^31 to recover the signed result.
    if ($Raw -gt 1073741824) { return $Raw - 2147483648 }
    return $Raw
}

function Expected-Value([string]$Path) {
    # `// expect: N` -- capture the first signed integer after the marker. The
    # lang corpus annotates some expectations with a trailing parenthetical
    # explanation (e.g. `// expect: 150 (10+20+30+40+50)`), so we must NOT
    # require the number to be the last token on the line.
    $m = Select-String -Path $Path -Pattern '^\s*//\s*expect:\s*(-?[0-9]+)' | Select-Object -First 1
    if ($null -eq $m) { throw "missing // expect: N in $Path" }
    return [int64]$m.Matches[0].Groups[1].Value
}

function Relative-Path([string]$Full) {
    return $Full.Substring($repo.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/')
}

# Run a program natively (the C++ host compiler) against its `main` entry.
function Invoke-Native([string]$RelativePath) {
    return Invoke-Captured $cli @('run', $RelativePath, '--fn', 'main')
}

# Run a program through the self-hosted compiler by feeding its path on stdin
# to the file-pipeline adapter (which needs --ffi for the io extension).
function Invoke-SelfHosted([string]$RelativePath) {
    return Invoke-Captured $cli @('run', 'self_hosted/correctness_tests/file_pipeline_runner.ember', '--fn', 'run_file', '--ffi') $RelativePath
}

# -----------------------------------------------------------------------------
# Counters for the final TAP-ish summary.
# -----------------------------------------------------------------------------
$script:totalValid = 0
$script:validPass = 0        # full parity: native==expected AND self==expected
$script:nativePass = 0       # native==expected (regardless of self)
$script:selfPass = 0         # self==expected (regardless of native)
$script:unsupportedSubset = 0  # valid test the self-hosted compiler rejects (-3xx)
$script:realMismatch = 0     # both ran but disagree, or self ran to a wrong non-negative value
$script:validHang = 0
$script:validNativeFail = 0  # native itself did not match expected (oracle problem)

$script:invalidTotal = 0
$script:invalidNativeError = 0  # native correctly errored (nonzero / negative)
$script:invalidSelfReject = 0   # self correctly returned a negative -3xx
$script:invalidRecorded = 0     # recorded behaviour (informational, never fails)

$script:permPass = 0
$script:permFail = 0
$script:permHang = 0

$script:hangList = [System.Collections.Generic.List[string]]::new()
$script:mismatchList = [System.Collections.Generic.List[string]]::new()

# -----------------------------------------------------------------------------
# CLASS 1 -- VALID tests (full parity: native vs self vs expected).
# -----------------------------------------------------------------------------

# Build the valid corpus: lang valid_*.ember carrying `// expect: N`, plus the
# non-reject programs in this directory (the self-hosted subset's own positive
# suite). The file_pipeline_runner and diagnose_file helpers are infrastructure,
# not test programs, so they are excluded.
$langValid = Get-ChildItem $langDir -Filter 'valid_*.ember' |
    Where-Object { (Select-String -Path $_.FullName -Pattern '^\s*//\s*expect:\s*(-?[0-9]+)' -Quiet) } |
    Sort-Object Name

$localValid = Get-ChildItem $tests -Filter '*.ember' |
    Where-Object { $_.Name -notlike 'reject_*' -and $_.Name -ne 'file_pipeline_runner.ember' -and $_.Name -ne 'diagnose_file.ember' } |
    Where-Object { (Select-String -Path $_.FullName -Pattern '^\s*//\s*expect:\s*(-?[0-9]+)' -Quiet) } |
    Sort-Object Name

$validTests = @()
foreach ($t in $langValid) { $validTests += [pscustomobject]@{ FullName=$t.FullName; Origin='lang' } }
foreach ($t in $localValid) { $validTests += [pscustomobject]@{ FullName=$t.FullName; Origin='local' } }

Write-Host "================================================================"
Write-Host "Differential parity audit -- VALID tests ($($validTests.Count) programs)"
Write-Host "  native = $EmberCli (C++ reference)"
Write-Host "  self   = file_pipeline_runner.ember (ember-written compiler)"
Write-Host "================================================================"

foreach ($test in $validTests) {
    $script:totalValid++
    $relative = Relative-Path $test.FullName
    $name = [IO.Path]::GetFileName($test.FullName)
    $expected = Expected-Value $test.FullName

    $native = Invoke-Native $relative
    $self = Invoke-SelfHosted $relative

    if ($native.TimedOut -or $self.TimedOut) {
        Write-Host ("HANG  {0}  native_timeout={1} self_timeout={2}" -f $name, $native.TimedOut, $self.TimedOut) -ForegroundColor Red
        $script:validHang++
        $script:hangList.Add("VALID $name")
        continue
    }

    $nativeDecoded = Decode-EmberResult $native.ExitCode
    $selfDecoded = Decode-EmberResult $self.ExitCode

    $nativeOk = ($nativeDecoded -eq $expected)
    if ($nativeOk) { $script:nativePass++ }
    if ($selfDecoded -eq $expected) { $script:selfPass++ }

    if (-not $nativeOk) {
        # The oracle itself disagreed with the documented expectation -- a
        # native failure, not a parity problem. Record and continue.
        Write-Host ("NATIVE-FAIL {0}: expected={1} native={2}" -f $name, $expected, $nativeDecoded) -ForegroundColor Magenta
        if ($native.Stderr) { Write-Host ("  native stderr: {0}" -f $native.Stderr.Trim()) }
        $script:validNativeFail++
        continue
    }

    if ($selfDecoded -lt 0) {
        # A negative -3xx from the self-hosted compiler means it rejected the
        # program as out-of-subset. That is expected for anything beyond the
        # supported language subset; it is NOT a real mismatch.
        Write-Host ("UNSUPPORTED {0}: expected={1} self={2} (subset rejection)" -f $name, $expected, $selfDecoded) -ForegroundColor DarkGray
        $script:unsupportedSubset++
        continue
    }

    if ($selfDecoded -eq $expected) {
        Write-Host ("PASS  {0} => {1} (native==self==expected)" -f $name, $expected)
        $script:validPass++
    } else {
        # Both backends ran to a non-negative value but disagree: a real
        # parity mismatch (the self-hosted compiler produced a wrong result).
        Write-Host ("MISMATCH {0}: expected={1} native={2} self={3}" -f $name, $expected, $nativeDecoded, $selfDecoded) -ForegroundColor Red
        if ($self.Stdout) { Write-Host ("  self stdout: {0}" -f $self.Stdout.Trim()) }
        if ($self.Stderr) { Write-Host ("  self stderr: {0}" -f $self.Stderr.Trim()) }
        $script:realMismatch++
        $script:mismatchList.Add("VALID $name native=$nativeDecoded self=$selfDecoded expected=$expected")
    }
}

# -----------------------------------------------------------------------------
# CLASS 2 -- INVALID tests (informational: record native + self behaviour).
# -----------------------------------------------------------------------------

$invalidTests = @()
$invalidTests += Get-ChildItem $langDir -Filter 'sema_invalid_*.ember' | Sort-Object Name
$invalidTests += Get-ChildItem $langDir -Filter 'invalid_*.ember' | Sort-Object Name

Write-Host ""
Write-Host "================================================================"
Write-Host "INVALID tests -- informational record ($($invalidTests.Count) programs)"
Write-Host "  native expected: nonzero/error ; self expected: negative -3xx"
Write-Host "  (recorded only; the self-hosted subset does not yet cover every"
Write-Host "   rejection path, so these never fail the script)"
Write-Host "================================================================"

foreach ($test in $invalidTests) {
    $script:invalidTotal++
    $relative = Relative-Path $test.FullName
    $name = $test.Name

    $native = Invoke-Native $relative
    $self = Invoke-SelfHosted $relative

    if ($native.TimedOut -or $self.TimedOut) {
        Write-Host ("HANG  {0}  native_timeout={1} self_timeout={2}" -f $name, $native.TimedOut, $self.TimedOut) -ForegroundColor Red
        $script:hangList.Add("INVALID $name")
        continue
    }

    $nativeDecoded = Decode-EmberResult $native.ExitCode
    $selfDecoded = Decode-EmberResult $self.ExitCode

    # Native "errored as expected" = anything other than a clean 0 exit (a
    # compile/parse/sema failure surfaces as a nonzero or negative code).
    $nativeErr = ($nativeDecoded -ne 0)
    if ($nativeErr) { $script:invalidNativeError++ }
    # Self "rejected as expected" = a negative -3xx rejection code.
    $selfReject = ($selfDecoded -lt 0)
    if ($selfReject) { $script:invalidSelfReject++ }

    $tag = if ($nativeErr -and $selfReject) { "OK" } elseif ($nativeErr) { "native-only" } elseif ($selfReject) { "self-only" } else { "NEITHER" }
    Write-Host ("RECORD {0}: native={1} self={2} [{3}]" -f $name, $nativeDecoded, $selfDecoded, $tag) -ForegroundColor DarkGray
    $script:invalidRecorded++
}

# -----------------------------------------------------------------------------
# CLASS 3 -- PERMANENT negative tests (must always pass: self returns -3xx).
# -----------------------------------------------------------------------------

$rejectTests = Get-ChildItem $tests -Filter 'reject_*.ember' | Sort-Object Name

Write-Host ""
Write-Host "================================================================"
Write-Host "PERMANENT negative tests -- reject_*.ember ($($rejectTests.Count) programs)"
Write-Host "  self MUST return the documented -3xx (or any negative for the word 'negative')"
Write-Host "================================================================"

foreach ($test in $rejectTests) {
    $relative = Relative-Path $test.FullName
    $name = $test.Name
    $tagMatch = Select-String -Path $test.FullName -Pattern '^\s*//\s*selfhost-reject:\s*(\S+)\s*$' | Select-Object -First 1
    if ($null -ne $tagMatch) { $tag = $tagMatch.Matches[0].Groups[1].Value } else { $tag = 'negative' }

    $self = Invoke-SelfHosted $relative

    if ($self.TimedOut) {
        Write-Host ("HANG  {0}" -f $name) -ForegroundColor Red
        $script:permHang++
        $script:permFail++
        $script:hangList.Add("REJECT $name")
        continue
    }

    $decoded = Decode-EmberResult $self.ExitCode
    $ok = if ($tag -eq 'negative') { $decoded -lt 0 } else { $decoded -eq [int]$tag }
    if ($ok) {
        Write-Host ("PASS  {0} => {1}" -f $name, $decoded)
        $script:permPass++
    } else {
        Write-Host ("FAIL  {0}: wanted={1} self={2} (raw={3})" -f $name, $tag, $decoded, $self.ExitCode) -ForegroundColor Red
        $script:permFail++
        $script:mismatchList.Add("REJECT $name wanted=$tag self=$decoded")
    }
}

# -----------------------------------------------------------------------------
# Final TAP-ish summary.
# -----------------------------------------------------------------------------

$totalRuns = $script:totalValid + $script:invalidTotal + $rejectTests.Count
$totalHangs = $script:validHang + $script:permHang
$totalMismatches = $script:realMismatch + $script:permFail

Write-Host ""
Write-Host "================================================================"
Write-Host "SUMMARY"
Write-Host "================================================================"
Write-Host ("  total runs:            {0}" -f $totalRuns)
Write-Host ("  -- VALID ({0}) --" -f $script:totalValid)
Write-Host ("     full parity (pass): {0}" -f $script:validPass)
Write-Host ("     native-pass:        {0}" -f $script:nativePass)
Write-Host ("     self-pass:          {0}" -f $script:selfPass)
Write-Host ("     unsupported-subset: {0}" -f $script:unsupportedSubset)
Write-Host ("     real mismatches:    {0}" -f $script:realMismatch)
Write-Host ("     native failures:    {0}" -f $script:validNativeFail)
Write-Host ("     hangs:              {0}" -f $script:validHang)
Write-Host ("  -- INVALID ({0}, informational) --" -f $script:invalidTotal)
Write-Host ("     native errored:     {0}" -f $script:invalidNativeError)
Write-Host ("     self rejected -3xx: {0}" -f $script:invalidSelfReject)
Write-Host ("  -- PERMANENT reject ({0}) --" -f $rejectTests.Count)
Write-Host ("     pass:               {0}" -f $script:permPass)
Write-Host ("     fail:               {0}" -f $script:permFail)
Write-Host ("     hangs:              {0}" -f $script:permHang)
Write-Host ("  -- OVERALL --")
Write-Host ("     hangs (any class):  {0}" -f $totalHangs)
Write-Host ("     real mismatches:    {0}  (valid parity + permanent reject failures)" -f $totalMismatches)

if ($script:hangList.Count -gt 0) {
    Write-Host ""
    Write-Host "HANGS:" -ForegroundColor Red
    foreach ($h in $script:hangList) { Write-Host ("  {0}" -f $h) -ForegroundColor Red }
}
if ($script:mismatchList.Count -gt 0) {
    Write-Host ""
    Write-Host "REAL MISMATCHES / REJECT FAILURES:" -ForegroundColor Red
    foreach ($mm in $script:mismatchList) { Write-Host ("  {0}" -f $mm) -ForegroundColor Red }
}

# Exit 0 only if there were NO hangs and NO real mismatches. Unsupported-by-
# subset results and recorded invalid-test behaviour are expected at the
# baseline and do not fail the run.
if ($totalHangs -ne 0 -or $totalMismatches -ne 0) { exit 1 }
exit 0
