param(
    [string]$SecretsPath = (Join-Path $PSScriptRoot '..\include\wifi_secrets.h')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-CppStringLiteral {
    param(
        [string]$Source,
        [string]$VariableName
    )

    $escapedName = [regex]::Escape($VariableName)
    $pattern = $escapedName + '\s*\[\s*\]\s*=\s*"((?:\\.|[^"\\])*)"\s*;'
    $match = [regex]::Match($Source, $pattern)
    if (-not $match.Success) {
        throw "$VariableName 선언을 찾을 수 없습니다: $SecretsPath"
    }

    return [regex]::Unescape($match.Groups[1].Value)
}

$resolvedPath = (Resolve-Path -LiteralPath $SecretsPath).Path
$source = Get-Content -Raw -LiteralPath $resolvedPath
$ssid = Read-CppStringLiteral -Source $source -VariableName 'kWifiSsid'
$password = Read-CppStringLiteral -Source $source -VariableName 'kWifiPassword'

$utf8 = [System.Text.Encoding]::UTF8
$material = "esp32-ac-controller:wifi:v1`n$ssid`n$password"
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $digest = $sha256.ComputeHash($utf8.GetBytes($material))
}
finally {
    $sha256.Dispose()
}

$fingerprint = -join ($digest[0..3] | ForEach-Object { $_.ToString('X2') })
Write-Output 'Wi-Fi credential diagnostics (local header):'
Write-Output "  SSID: $ssid"
Write-Output "  password bytes: $($utf8.GetByteCount($password))"
Write-Output "  fingerprint: $fingerprint"
Write-Output '  Password text is never printed.'
