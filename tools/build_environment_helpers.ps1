function Get-BuildEnvironmentSummary {
  $cmdOutput = cmd /c set path 2>$null
  $pathEntries = @()
  foreach ($line in $cmdOutput) {
    if ($line -cmatch '^(PATH|Path)=(.*)$') {
      $pathEntries += [pscustomobject]@{
        Key = $Matches[1]
        Value = $Matches[2]
      }
    }
  }

  $source = "cmd"
  if (@($pathEntries).Count -eq 0) {
    $source = "process-fallback"
    $processVariables = [System.Environment]::GetEnvironmentVariables("Process")
    foreach ($key in $processVariables.Keys) {
      if ([string]::Equals([string]$key, "PATH", [System.StringComparison]::OrdinalIgnoreCase)) {
        $pathEntries += [pscustomobject]@{
          Key = [string]$key
          Value = [string]$processVariables[$key]
        }
      }
    }
  }

  $canonicalEntry = $pathEntries |
    Where-Object { $_.Key -ceq "PATH" } |
    Select-Object -First 1
  if ($null -eq $canonicalEntry) {
    $canonicalEntry = $pathEntries |
      Where-Object { $_.Key -ceq "Path" } |
      Select-Object -First 1
  }
  if ($null -eq $canonicalEntry) {
    $canonicalEntry = $pathEntries | Select-Object -First 1
  }

  $canonicalValue = if ($null -ne $canonicalEntry) { [string]$canonicalEntry.Value } else { "" }
  $canonicalSegments = @()
  if (-not [string]::IsNullOrEmpty($canonicalValue)) {
    $canonicalSegments = $canonicalValue -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  }

  [pscustomobject]@{
    Source = $source
    PathEntries = @($pathEntries)
    HasCollision = @($pathEntries).Count -gt 1
    CanonicalKey = if ($null -ne $canonicalEntry) { [string]$canonicalEntry.Key } else { "" }
    CanonicalValue = $canonicalValue
    CanonicalSegmentCount = $canonicalSegments.Count
  }
}

function Write-BuildEnvironmentSummary {
  param(
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Summary,

    [string]$Prefix = ""
  )

  if ([string]::IsNullOrEmpty($Prefix)) {
    Write-Host "Build environment"
    Write-Host "Source: $($Summary.Source)"
    Write-Host "PATH-like entries: $($Summary.PathEntries.Count)"
    Write-Host "Collision detected: $($Summary.HasCollision)"
    Write-Host "Canonical key: $($Summary.CanonicalKey)"
    Write-Host "Canonical segment count: $($Summary.CanonicalSegmentCount)"
    foreach ($entry in $Summary.PathEntries) {
      $segments = @()
      if (-not [string]::IsNullOrEmpty($entry.Value)) {
        $segments = $entry.Value -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
      }
      Write-Host "PATH_ENTRY[$($entry.Key)] segments=$($segments.Count)"
    }
    Write-Host "Build wrapper recommended: $($Summary.HasCollision)"
    return
  }

  Write-Host "$Prefix path entries=$($Summary.PathEntries.Count) collision=$($Summary.HasCollision) canonical=$($Summary.CanonicalKey) source=$($Summary.Source)"
}
