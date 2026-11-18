$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$env:PATH = 'D:/code/stm32cube/bundles/cmake/4.3.1+st.1/bin;D:/code/stm32cube/bundles/ninja/1.13.2+st.1/bin;D:/code/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin;' + $env:PATH
function Invoke-Check([string]$Program, [string[]]$CommandArgs) {
    & $Program @CommandArgs
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location -LiteralPath $projectRoot
try {
    foreach ($mode in @('Debug','Release')) {
        Invoke-Check cmake @('--preset',$mode,'-DLAB2_BUILD_BUSINESS_TESTS=ON','-DLAB2_ENABLE_INPUT_DEBUG=OFF','-DLAB2_ENABLE_SWEEP=OFF')
        Invoke-Check cmake @('--build','--preset',$mode)
        $testFiles = @("build/$mode/input_logic_tests.elf", "build/$mode/sweep_logic_tests.elf")
        foreach ($number in 1..6) { $testFiles += "build/$mode/sweep_fault_$number.elf" }
        Invoke-Check python (@('tests/business/run_arm_tests.py') + $testFiles)
    }
    Invoke-Check python @('tests/business/verify_input_dma.py')
    $diagArgs = @('-S','.','-B','build/Diagnostic','-G','Ninja',
        '-DCMAKE_BUILD_TYPE=Debug','-DLAB2_ENABLE_INPUT_DEBUG=ON')
    if (-not (Test-Path -LiteralPath 'build/Diagnostic/CMakeCache.txt')) {
        $diagArgs += '-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake'
    }
    Invoke-Check cmake $diagArgs
    Invoke-Check cmake @('--build','build/Diagnostic')
    Invoke-Check python @('tests/business/verify_debug.py')
    Invoke-Check python @('tests/business/prepare_wave_integration.py','8f300542703d8113984870726b86d1501683fb03')
    Invoke-Check python @('tests/business/verify_real_sweep.py')
    Invoke-Check python @('tests/business/verify_delivery.py')
    Invoke-Check python @('tests/business/run_regression.py')
    Invoke-Check git @('diff','--check')
} finally { Pop-Location }
