param(
	[Parameter(Mandatory = $true)]
	[string]$Executable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()

function Start-LobbyProcess([string]$Mode, [int]$Port, [string]$Path) {
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = (Resolve-Path -LiteralPath $Path).Path
	$startInfo.Arguments = "$Mode $Port"
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	$process = [System.Diagnostics.Process]::new()
	$process.StartInfo = $startInfo
	[void]$process.Start()
	return $process
}

$hostProcess = $null
$guestProcess = $null
try {
	$hostProcess = Start-LobbyProcess '--tcp-host' $port $Executable
	Start-Sleep -Milliseconds 100
	$guestProcess = Start-LobbyProcess '--tcp-guest' $port $Executable

	if (-not $guestProcess.WaitForExit(20000)) {
		throw 'TCP lobby guest exceeded the 20 second deadline.'
	}
	if (-not $hostProcess.WaitForExit(20000)) {
		throw 'TCP lobby host exceeded the 20 second deadline.'
	}
	$guestOut = $guestProcess.StandardOutput.ReadToEnd()
	$guestErr = $guestProcess.StandardError.ReadToEnd()
	$hostOut = $hostProcess.StandardOutput.ReadToEnd()
	$hostErr = $hostProcess.StandardError.ReadToEnd()
	if ($guestProcess.ExitCode -ne 0 -or $hostProcess.ExitCode -ne 0) {
		throw "TCP lobby process test failed (host=$($hostProcess.ExitCode), guest=$($guestProcess.ExitCode)).`n$hostOut$hostErr$guestOut$guestErr"
	}
	if (-not $hostOut.Contains('host-match-started') -or -not $guestOut.Contains('guest-match-started')) {
		throw "TCP lobby processes exited without completing the started-session checks.`n$hostOut$hostErr$guestOut$guestErr"
	}
	Write-Output "TCP lobby host/guest processes completed JOIN, READY, and START.`n$hostOut$guestOut"
}
finally {
	foreach ($process in @($guestProcess, $hostProcess)) {
		if ($null -ne $process) {
			if (-not $process.HasExited) {
				$process.Kill()
			}
			$process.Dispose()
		}
	}
}
