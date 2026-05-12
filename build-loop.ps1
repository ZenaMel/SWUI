function Close-ProjectEditor {
    Write-Host "`nClosing Unreal Editor for this project..." -ForegroundColor Cyan

    $procs = Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -eq "UnrealEditor.exe" -and
            $_.CommandLine -like "*$Project*"
        }

    foreach ($p in $procs) {
        Write-Host "Requesting close UnrealEditor.exe PID $($p.ProcessId)"
        $proc = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue

        if ($proc -and $proc.MainWindowHandle -ne 0) {
            $null = $proc.CloseMainWindow()
        }
    }

    Start-Sleep -Seconds 10

    foreach ($p in $procs) {
        $proc = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
        if ($proc) {
            Write-Host "Force killing stuck UnrealEditor.exe PID $($p.ProcessId)"
            Stop-Process -Id $p.ProcessId -Force
        }
    }

    Start-Sleep -Seconds 2
}