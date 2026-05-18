$Project = "F:\GameDev\ADA\ADA.uproject"
$Engine  = "F:\GameDev\UE_AngelScript\Engine"

$BuildBat = Join-Path $Engine "Build\BatchFiles\Build.bat"
$Editor   = Join-Path $Engine "Binaries\Win64\UnrealEditor.exe"

$CloseTimeoutSeconds = 60
$ForceKillAfterTimeout = $true

function Get-ProjectEditorProcesses {
    $escapedProject = [regex]::Escape($Project)

    Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -eq "UnrealEditor.exe" -and
            $_.CommandLine -match $escapedProject
        } |
        ForEach-Object {
            Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
        }
}

function Close-ProjectEditor {
    Write-Host "`nClosing Unreal Editor for this project gracefully..." -ForegroundColor Cyan

    $procs = @(Get-ProjectEditorProcesses)

    if ($procs.Count -eq 0) {
        Write-Host "No matching Unreal Editor process found." -ForegroundColor DarkGray
        return
    }

    foreach ($proc in $procs) {
        Write-Host "Requesting close for UnrealEditor.exe PID $($proc.Id)..."

        if ($proc.MainWindowHandle -ne 0) {
            $null = $proc.CloseMainWindow()
        }
        else {
            Write-Host "PID $($proc.Id) has no main window handle." -ForegroundColor Yellow
        }
    }

    $deadline = (Get-Date).AddSeconds($CloseTimeoutSeconds)

    while ((Get-Date) -lt $deadline) {
        $stillRunning = @(Get-ProjectEditorProcesses)

        if ($stillRunning.Count -eq 0) {
            Write-Host "Unreal Editor closed cleanly." -ForegroundColor Green
            Start-Sleep -Seconds 2
            return
        }

        Write-Host "Waiting for Unreal Editor to exit cleanly... $($stillRunning.Count) process(es) still running."
        Start-Sleep -Seconds 3
    }

    $stillRunning = @(Get-ProjectEditorProcesses)

    if ($stillRunning.Count -eq 0) {
        Write-Host "Unreal Editor closed cleanly." -ForegroundColor Green
        Start-Sleep -Seconds 2
        return
    }

    Write-Host "Unreal Editor did not close within $CloseTimeoutSeconds seconds." -ForegroundColor Yellow

    if ($ForceKillAfterTimeout) {
        foreach ($proc in $stillRunning) {
            Write-Host "Force killing stuck UnrealEditor.exe PID $($proc.Id)..." -ForegroundColor Red
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }

        Start-Sleep -Seconds 2
    }
    else {
        Write-Host "Force kill disabled. Close Unreal manually, then press Enter." -ForegroundColor Yellow
        Read-Host
    }
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

    Start-Process `
        -FilePath $Editor `
        -ArgumentList "`"$Project`""
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
    Write-Host "Press Enter to close editor gracefully and build again, or Ctrl+C to exit." -ForegroundColor Yellow
    Read-Host
}