param(
	[Parameter(Mandatory = $true)]
	[string]$Executable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Start-LobbyProcess([string]$Mode, [int]$Port, [int]$PlayerId, [int]$PlayerCount, [string]$Path) {
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = (Resolve-Path -LiteralPath $Path).Path
	$startInfo.Arguments = "$Mode $Port $PlayerId $PlayerCount"
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	$process = [System.Diagnostics.Process]::new()
	$process.StartInfo = $startInfo
	[void]$process.Start()
	return $process
}

foreach ($guestCount in @(1, 2, 3)) {
	$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
	$listener.Start()
	$port = $listener.LocalEndpoint.Port
	$listener.Stop()
	$playerCount = $guestCount + 1
	$hostProcess = $null
	$guestProcesses = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
	try {
		$hostProcess = Start-LobbyProcess '--tcp-host' $port 301 $playerCount $Executable
		Start-Sleep -Milliseconds 100
		for ($index = 0; $index -lt $guestCount; $index++) {
			$guestId = 302 + $index
			$guestProcesses.Add((Start-LobbyProcess '--tcp-guest' $port $guestId $playerCount $Executable))
		}

		foreach ($guestProcess in $guestProcesses) {
			if (-not $guestProcess.WaitForExit(20000)) {
				throw "TCP lobby guest process $($guestProcess.Id) exceeded the 20 second deadline."
			}
		}
		if (-not $hostProcess.WaitForExit(20000)) {
			throw 'TCP lobby host exceeded the 20 second deadline.'
		}

		$hostOut = $hostProcess.StandardOutput.ReadToEnd()
		$hostErr = $hostProcess.StandardError.ReadToEnd()
		$hostStarted = $hostOut.Contains('host-match-started')
		$hostSynchronized = $hostOut.Contains("host-synced-commands=$guestCount")
		if ($hostProcess.ExitCode -ne 0 -or -not $hostStarted -or -not $hostSynchronized) {
			$guestDetails = @()
			foreach ($guestProcess in $guestProcesses) {
				$guestDetails += $guestProcess.StandardOutput.ReadToEnd()
				$guestDetails += $guestProcess.StandardError.ReadToEnd()
			}
			throw "TCP lobby host failed for $playerCount active players (exit=$($hostProcess.ExitCode)).`n$hostOut$hostErr`n$($guestDetails -join "`n")"
		}
		foreach ($guestProcess in $guestProcesses) {
			$guestOut = $guestProcess.StandardOutput.ReadToEnd()
			$guestErr = $guestProcess.StandardError.ReadToEnd()
			$guestStarted = $guestOut.Contains('guest-match-started')
			$guestSynchronized = $guestOut.Contains("guest-synced-commands=$guestCount")
			if ($guestProcess.ExitCode -ne 0 -or -not $guestStarted -or -not $guestSynchronized) {
				throw "TCP lobby guest failed for $playerCount active players (exit=$($guestProcess.ExitCode)).`n$guestOut$guestErr"
			}
		}
		Write-Output "TCP lobby passed with $playerCount separate player processes."
		Write-Output "TCP authoritative command replication passed for $guestCount guest intents."
	}
	finally {
		foreach ($process in @($guestProcesses) + @($hostProcess)) {
			if ($null -ne $process) {
				if (-not $process.HasExited) {
					$process.Kill()
				}
				$process.Dispose()
			}
		}
	}
}
