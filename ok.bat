@echo off
setlocal

REM Output folder next to this .bat file (current working dir for the script)
set OUTDIR=%~dp0TXT_OUTPUT
if "%OUTDIR:~-1%"=="\" set OUTDIR=%OUTDIR:~0,-1%

REM Create or clean output folder
if not exist "%OUTDIR%" (
    mkdir "%OUTDIR%"
) else (
    del /q "%OUTDIR%\*.*" >nul 2>&1
    for /d %%D in ("%OUTDIR%\*") do rmdir /s /q "%%D"
)

REM Save both a readable TXT and a CSV
set OUTTXT=%OUTDIR%\suspicious_process_check.txt
set OUTCSV=%OUTDIR%\suspicious_process_check.csv

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
"$ErrorActionPreference='SilentlyContinue';" ^
"$names = @('SpitCamSrv','UDClientService','NahimicService','MessagingPlugin','AppProvisioningPlugin','CrossDeviceResume','CodeSetup-stable-94e8ae2b');" ^
"$rows = Get-Process | Where-Object { $names -contains $_.Name } | ForEach-Object { " ^
"  $p = $_; $path = $p.Path; $sig = if ($path) { Get-AuthenticodeSignature -FilePath $path } else { $null }; " ^
"  [PSCustomObject]@{ " ^
"    Name = $p.Name; " ^
"    PID = $p.Id; " ^
"    Path = $path; " ^
"    Company = $p.Company; " ^
"    SignatureStatus = if ($sig) { $sig.Status } else { 'NO_PATH' }; " ^
"    Signer = if ($sig -and $sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' } " ^
"  } " ^
"};" ^
"$rows | Sort-Object SignatureStatus, Company, Name | Export-Csv '%OUTCSV%' -NoTypeInformation;" ^
"$rows | Sort-Object SignatureStatus, Company, Name | Format-Table -AutoSize | Out-String -Width 4096 | Set-Content '%OUTTXT%';"

echo.
echo Done.
echo Output written to:
echo %OUTTXT%
echo %OUTCSV%
echo.

pause
endlocal
