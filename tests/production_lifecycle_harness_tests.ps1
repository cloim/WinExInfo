$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot '..\scripts\run-production-lifecycle.ps1'
if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf)) {
    throw "Lifecycle harness is missing: $scriptPath"
}
$output = @(& $scriptPath -Configuration Debug -HarnessSelfTest)
if ($LASTEXITCODE -ne 0) {
    throw "Lifecycle harness self-test exited $LASTEXITCODE"
}
$pass = @($output | Where-Object { $_ -ceq 'HARNESS_SELF_TEST_PASS cases=8' })
if ($pass.Count -ne 1) {
    throw "Lifecycle harness self-test did not emit its exact PASS line: $($output -join '|')"
}
$source = Get-Content -Raw -LiteralPath $scriptPath
foreach ($forbidden in @('Stop-Process','TerminateProcess','taskkill.exe','taskkill ')) {
    if ($source.Contains($forbidden,[StringComparison]::OrdinalIgnoreCase)) {
        throw "Lifecycle harness contains forbidden process termination: $forbidden"
    }
}
foreach ($required in @('SignalExactHost','Assert-IsolatedControlledProcess','Assert-CurrentControlledWindow','Assert-ExactCleanupDiagnostic')) {
    if (-not $source.Contains($required,[StringComparison]::Ordinal)) {
        throw "Lifecycle harness is missing safety primitive: $required"
    }
}
