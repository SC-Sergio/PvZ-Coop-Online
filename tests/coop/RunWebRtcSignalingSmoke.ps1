param(
	[Parameter(Mandatory = $true)][string]$NodeExecutable,
	[Parameter(Mandatory = $true)][string]$ServerScript,
	[Parameter(Mandatory = $true)][string]$ClientExecutable
)

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$env:COOP_SIGNAL_HOST = '127.0.0.1'
$env:COOP_SIGNAL_PORT = [string]$port
$serverDirectory = Split-Path -Parent $ServerScript
$serverFileName = Split-Path -Leaf $ServerScript
$logPrefix = Join-Path $env:TEMP "coop-signal-$PID"
$stdoutPath = "$logPrefix.out"
$stderrPath = "$logPrefix.err"
$server = Start-Process -FilePath $NodeExecutable -WorkingDirectory $serverDirectory -ArgumentList $serverFileName `
	-PassThru -WindowStyle Hidden -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

try {
	$ready = $false
	for ($attempt = 0; $attempt -lt 100; ++$attempt) {
		if ($server.HasExited) {
			throw "Signaling service exited early with code $($server.ExitCode)."
		}
		try {
			$client = [System.Net.Sockets.TcpClient]::new()
			$connect = $client.ConnectAsync('127.0.0.1', $port)
			if ($connect.Wait(250) -and $client.Connected) {
				$ready = $true
			}
			$client.Dispose()
			if ($ready) { break }
		} catch {
			Start-Sleep -Milliseconds 50
		}
	}
	if (-not $ready) {
		$stdout = if (Test-Path $stdoutPath) { Get-Content $stdoutPath -Raw } else { '' }
		$stderr = if (Test-Path $stderrPath) { Get-Content $stderrPath -Raw } else { '' }
		throw "Signaling service did not become ready. stdout=$stdout stderr=$stderr"
	}

	& $ClientExecutable --signaling "ws://127.0.0.1:$port"
	if ($LASTEXITCODE -ne 0) {
		throw "WebRTC signaling client exited with code $LASTEXITCODE."
	}
}
finally {
	if (-not $server.HasExited) {
		Stop-Process -Id $server.Id -Force
	}
	Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
}
