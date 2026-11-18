$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pathSeparator = [IO.Path]::PathSeparator
# Prefer tools already in PATH; add this workstation's optional installed bundles
# only when present. A clone on another machine does not need these drive paths.
$optionalToolDirs = @('D:/code/stm32cube/bundles/cmake/4.3.1+st.1/bin',
    'D:/code/stm32cube/bundles/ninja/1.13.2+st.1/bin',
    'D:/code/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin','D:/code/Python')
$toolDirs = @($optionalToolDirs | Where-Object { Test-Path -LiteralPath $_ -PathType Container })
if ($toolDirs.Count) { $env:PATH = ($toolDirs -join $pathSeparator) + $pathSeparator + $env:PATH }
$env:PYTHONPATH = @((Join-Path $projectRoot 'build/test_python'),
    (Join-Path $projectRoot 'build/snake_checks/python'),$env:PYTHONPATH) -join $pathSeparator
function Invoke-Check([string]$Program, [string[]]$CommandArgs) {
    & $Program @CommandArgs
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location -LiteralPath $projectRoot
try {
    & python -c "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('unicorn') and importlib.util.find_spec('elftools') else 1)"
    if ($LASTEXITCODE -ne 0) {
        Invoke-Check python @('-m','pip','install','--target','build/test_python','unicorn==2.1.4','pyelftools==0.32')
    }
    foreach ($mode in @('Debug','Release')) {
        Invoke-Check cmake @('--preset',$mode,'-DLAB2_BUILD_BUSINESS_TESTS=ON',
            '-DLAB2_ENABLE_INPUT_DEBUG=OFF','-DWAVE_ENABLE_DIAGNOSTICS=OFF')
        Invoke-Check cmake @('--build','--preset',$mode)
        $testFiles = @("build/$mode/input_logic_tests.elf", "build/$mode/sweep_logic_tests.elf")
        foreach ($number in 1..6) { $testFiles += "build/$mode/sweep_fault_$number.elf" }
        Invoke-Check python (@('tests/business/run_arm_tests.py') + $testFiles)
        Invoke-Check python @('tests/verify_wave.py','--stage','W5','--elf',"build/$mode/lab2_f407.elf")
    }
    Invoke-Check cmake @('-S','.','-B','build/Diagnostic','-G','Ninja',
        '-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake','-DCMAKE_BUILD_TYPE=Debug',
        '-DLAB2_ENABLE_INPUT_DEBUG=ON','-DWAVE_ENABLE_DIAGNOSTICS=ON')
    Invoke-Check cmake @('--build','build/Diagnostic')
    Invoke-Check python @('tests/business/verify_debug.py')
    Invoke-Check python @('tests/verify_wave.py','--stage','W5','--elf','build/Diagnostic/lab2_f407.elf','--diagnostics')
    Invoke-Check python @('tests/business/verify_input_dma.py')
    Invoke-Check python @('tests/business/verify_real_sweep.py')
    Invoke-Check python @('tests/verify_integrated_ui.py')
    Invoke-Check python @('tests/verify_slider.py')
    Invoke-Check python @('tests/verify_snake.py')
    Invoke-Check python @('BSP/Screen/tools/verify_assets.py')
    Invoke-Check python @('tests/business/verify_delivery.py')
    Invoke-Check git @('diff','--check')
    Write-Output 'PASS: integrated firmware software checks. Physical hardware remains unverified.'
} finally { Pop-Location }
