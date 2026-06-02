param(
  [Parameter(Mandatory = $true)]
  [string]$Tag,

  [Parameter(Mandatory = $true)]
  [string]$Repo,

  [Parameter(Mandatory = $true)]
  [string]$Token,

  [string]$ArtifactsDir = "artifacts",
  [string]$ApiBaseUrl = "https://api.github.com"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($ArtifactsDir)) {
  $ArtifactsDir = Join-Path $root $ArtifactsDir
}
$apiBase = $ApiBaseUrl.TrimEnd('/')

$binaryZip = Join-Path $ArtifactsDir ("WinAudio-" + $Tag + "-windows-x64.zip")
$symbolsZip = Join-Path $ArtifactsDir ("WinAudio-" + $Tag + "-windows-x64-symbols.zip")

foreach ($path in @($binaryZip, $symbolsZip)) {
  if (-not (Test-Path -LiteralPath $path)) {
    throw "Required release upload asset not found: $path"
  }
}

$headers = @{
  Authorization = "Bearer $Token"
  Accept = "application/vnd.github+json"
  "X-GitHub-Api-Version" = "2022-11-28"
  "User-Agent" = "winaudio-release-workflow"
}

$releaseBody = @{
  tag_name = $Tag
  name = $Tag
  generate_release_notes = $true
} | ConvertTo-Json

$release = Invoke-RestMethod `
  -Method Post `
  -Uri ($apiBase + "/repos/" + $Repo + "/releases") `
  -Headers $headers `
  -Body $releaseBody `
  -ContentType "application/json"

if (-not $release.upload_url) {
  throw "GitHub release creation did not return an upload_url."
}

$uploadBase = $release.upload_url -replace '\{\?name,label\}$', ''

function Upload-ReleaseAsset {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath
  )

  $name = [System.IO.Path]::GetFileName($FilePath)
  $uri = $uploadBase + "?name=" + [System.Uri]::EscapeDataString($name)
  $assetHeaders = @{
    Authorization = "Bearer $Token"
    Accept = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent" = "winaudio-release-workflow"
  }

  Invoke-RestMethod `
    -Method Post `
    -Uri $uri `
    -Headers $assetHeaders `
    -InFile $FilePath `
    -ContentType "application/zip" | Out-Null
}

Upload-ReleaseAsset -FilePath $binaryZip
Upload-ReleaseAsset -FilePath $symbolsZip

Write-Host ("RELEASE_URL=" + $release.html_url)
