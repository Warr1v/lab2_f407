$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
New-Item -ItemType Directory -Path 'build/wave_checks' -Force | Out-Null

function Invoke-WaveCheck {
    param([string]$Program, [string[]]$Arguments, [string]$Log)
    $wavePreviousErrorPreference = $ErrorActionPreference
    try {
        # CMake writes ordinary status messages to stderr, including on success.
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message }
            else { "$_" }
        } | Tee-Object -FilePath (Join-Path 'build/wave_checks' $Log)
        $waveCheckExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $wavePreviousErrorPreference }
    if ($waveCheckExit -ne 0) { throw "$Program failed with exit code $waveCheckExit" }
}

Invoke-WaveCheck 'python' @('-m','pip','install','--target','build/wave_checks/python','unicorn==2.1.4','pyelftools==0.32','--upgrade') 'dependencies.log'
Invoke-WaveCheck 'cmake' @('--preset','Debug') 'configure_Debug.log'
Invoke-WaveCheck 'cmake' @('--build','--preset','Debug') 'build_Debug.log'
Invoke-WaveCheck 'cmake' @('--preset','Release') 'configure_Release.log'
Invoke-WaveCheck 'cmake' @('--build','--preset','Release') 'build_Release.log'
$waveDiagnosticArguments = @('-S','.','-B','build/WaveDiagnostics','-G','Ninja','-DCMAKE_BUILD_TYPE=Debug','-DWAVE_ENABLE_DIAGNOSTICS=ON')
if (!(Test-Path -LiteralPath 'build/WaveDiagnostics/CMakeCache.txt')) {
    $waveDiagnosticArguments += '-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake'
}
Invoke-WaveCheck 'cmake' $waveDiagnosticArguments 'configure_diagnostics.log'
Invoke-WaveCheck 'cmake' @('--build','build/WaveDiagnostics') 'build_diagnostics.log'
Invoke-WaveCheck 'python' @('tests/verify_wave.py','--stage','W5') 'test_Debug.log'
Invoke-WaveCheck 'python' @('tests/verify_wave.py','--stage','W5','--elf','build/Release/lab2_f407.elf') 'test_Release.log'
Invoke-WaveCheck 'python' @('tests/verify_wave.py','--stage','W5','--elf','build/WaveDiagnostics/lab2_f407.elf','--diagnostics') 'test_diagnostics.log'
Invoke-WaveCheck 'python' @('tests/verify_wave_assets.py') 'assets.log'
$env:PYTHONPATH = (Join-Path (Get-Location) 'build/wave_checks/python')
Invoke-WaveCheck 'python' @('tests/verify_snake.py') 'snake_regression.log'
