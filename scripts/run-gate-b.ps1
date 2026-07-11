[CmdletBinding(DefaultParameterSetName = 'Normal')]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [Parameter(ParameterSetName = 'Normal')]
    [ValidateRange(1, 100)]
    [int]$Iterations = 1,

    [Parameter(Mandatory, ParameterSetName = 'Fault')]
    [ValidateSet('UnhookFailure')]
    [string]$Fault
)

$ErrorActionPreference = 'Stop'
$configurationName = $Configuration.ToLowerInvariant()
$binaryDirectory = Join-Path $PSScriptRoot "..\out\build\windows-x64-$configurationName\bin"
$testExecutable = Join-Path $binaryDirectory 'WinExInfoTests.exe'
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "WinExInfoTests.exe was not found: $testExecutable"
}

$stdoutPath = Join-Path $env:TEMP "winexinfo-gate-b-$PID.stdout"
$stderrPath = Join-Path $env:TEMP "winexinfo-gate-b-$PID.stderr"
Remove-Item -LiteralPath $stdoutPath, $stderrPath -ErrorAction SilentlyContinue
$target = $null

try {
    $target = Start-Process `
        -FilePath $testExecutable `
        -ArgumentList '--hook-target' `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -PassThru

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    $readyLines = @()
    do {
        Start-Sleep -Milliseconds 25
        if (Test-Path -LiteralPath $stdoutPath) {
            $readyLines = @(Get-Content -LiteralPath $stdoutPath)
        }
    } while ($readyLines.Count -eq 0 -and -not $target.HasExited -and
             [DateTime]::UtcNow -lt $deadline)

    if ($readyLines.Count -ne 1) {
        throw "Target did not emit exactly one READY record within 5000ms."
    }
    $readyMatch = [regex]::Match(
        $readyLines[0],
        '^READY protocol=1 pid=(\d+) tid=(\d+) hwnd=(0x[0-9A-F]{16})$')
    if (-not $readyMatch.Success -or [int]$readyMatch.Groups[1].Value -ne $target.Id) {
        throw "Target READY record is invalid."
    }

    if ($PSCmdlet.ParameterSetName -eq 'Fault') {
        $controllerArguments = @(
            '--hook-controller', [string]$target.Id,
            '--fault', 'unhook-failure')
    }
    else {
        $controllerArguments = @(
            '--hook-controller', [string]$target.Id,
            '--iterations', [string]$Iterations)
    }
    $controllerLines = @(& $testExecutable @controllerArguments 2>&1)
    $controllerExit = $LASTEXITCODE
    if ($controllerExit -ne 0) {
        throw "Gate B controller failed with exit code $controllerExit`: $($controllerLines -join ' ')"
    }

    $acceptedLines = @($controllerLines | Where-Object {
        $_ -match '^TARGET_ACCEPTED protocol=1 '
    })
    if ($acceptedLines.Count -ne 1) {
        throw "Controller did not emit exactly one TARGET_ACCEPTED record."
    }
    $acceptedMatch = [regex]::Match(
        [string]$acceptedLines[0],
        '^TARGET_ACCEPTED protocol=1 pid=(\d+) tid=(\d+) hwnd=(0x[0-9A-F]{16})$')
    if (-not $acceptedMatch.Success -or
        $acceptedMatch.Groups[1].Value -cne $readyMatch.Groups[1].Value -or
        $acceptedMatch.Groups[2].Value -cne $readyMatch.Groups[2].Value -or
        $acceptedMatch.Groups[3].Value -cne $readyMatch.Groups[3].Value) {
        throw "READY and TARGET_ACCEPTED identities differ."
    }

    if ($PSCmdlet.ParameterSetName -eq 'Fault') {
        $expected = 'GATE_B_FAULT_PASS fault=unhook_failure module_retained=true event_signaled=false target_exit=0'
    }
    else {
        $expected = "GATE_B_PASS iterations=$Iterations target_handles_delta=0 target_threads_delta=0 controller_handles_delta=0 controller_threads_delta=0 target_exit=0"
        $resourceLines = @($controllerLines | Where-Object {
            [string]$_ -match '^RESOURCE_COUNTS target_handles_start=\d+ target_handles_end=\d+ target_threads_start=\d+ target_threads_end=\d+ controller_handles_start=\d+ controller_handles_end=\d+ controller_threads_start=\d+ controller_threads_end=\d+$'
        })
        if ($resourceLines.Count -ne 1) {
            throw "Controller resource-count record is missing or not exact."
        }
    }
    if (@($controllerLines | Where-Object { [string]$_ -ceq $expected }).Count -ne 1) {
        throw "Controller success record is missing or not exact."
    }
    if ($PSCmdlet.ParameterSetName -eq 'Normal') {
        Write-Output ([string]$resourceLines[0])
    }
    Write-Output $expected
}
finally {
    if ($null -ne $target -and -not $target.HasExited) {
        Stop-Process -Id $target.Id -Force
        $target.WaitForExit()
    }
    Remove-Item -LiteralPath $stdoutPath, $stderrPath -ErrorAction SilentlyContinue
}
