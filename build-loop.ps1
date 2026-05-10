$Project = "F:\GameDev\ADA\ADA.uproject"
$Engine  = "F:\GameDev\UE_AngelScript\Engine"

$BuildBat = Join-Path $Engine "Build\BatchFiles\Build.bat"
$Editor   = Join-Path $Engine "Binaries\Win64\UnrealEditor.exe"

function Close-ProjectEditor {
    Write-Host "`nClosing Unreal Editor for this project..." -ForegroundColor Cyan

    Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -eq "UnrealEditor.exe" -and
            $_.CommandLine -like "*$Project*"
        } |
        ForEach-Object {
            Write-Host "Killing UnrealEditor.exe PID $($_.ProcessId)"
            Stop-Process -Id $_.ProcessId -Force
        }

    Start-Sleep -Seconds 2
}

function Build-Project {
    Write-Host "`nBuilding ADAEditor..." -ForegroundColor Cyan

    $Args = @(
        "ADAEditor",
        "Win64",
        "Development",
        "-Project=`"$Project`"",
        "-WaitMutex"
    )

    $Proc = Start-Process `
        -FilePath $BuildBat `
        -ArgumentList $Args `
        -Wait `
        -PassThru `
        -NoNewWindow

    return $Proc.ExitCode
}

function Start-ProjectEditor {
    Write-Host "`nBuild succeeded. Starting project..." -ForegroundColor Green
    Start-Process $Editor -ArgumentList "`"$Project`""
}

while ($true) {
    while ($true) {
        Close-ProjectEditor

        $ExitCode = Build-Project

        if ($ExitCode -eq 0) {
            Start-ProjectEditor
            break
        }

        Write-Host "`nBuild failed with exit code $ExitCode." -ForegroundColor Red
        Write-Host "Fix code, then press Enter to build again." -ForegroundColor Yellow
        Read-Host
    }

    Write-Host "`nEditor started." -ForegroundColor Green
    Write-Host "Press Enter to close editor and build again, or Ctrl+C to exit." -ForegroundColor Yellow
    Read-Host
}