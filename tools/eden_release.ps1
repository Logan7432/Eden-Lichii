# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds eden.exe as the next build-N, zips it like Lichii's Windows release and publishes it as a
# GitHub release on Logan7432/Eden-Lichii, where installed copies look for updates.
# Double-click tools\eden_release.cmd to run it.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
try {
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
} catch {
}

$Owner = 'Logan7432'
$Repo = 'Eden-Lichii'
$Branch = 'eden-lichii'
$Remote = 'github'
$RemoteBranch = 'main'
$Platform = 'windows-x86_64'
$AssetName = "eden-$Platform.zip"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'build'
$BinDir = Join-Path $BuildDir 'bin\Release'
$OutDir = Join-Path $BuildDir 'eden-release'
$ApiBase = "https://api.github.com/repos/$Owner/$Repo"
$UploadBase = "https://uploads.github.com/repos/$Owner/$Repo"
$ScriptStart = Get-Date

$script:Headers = $null

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Write-Info([string]$Text) {
    Write-Host "    $Text"
}

function Write-Warn([string]$Text) {
    Write-Host "    WARNING: $Text" -ForegroundColor Yellow
}

function Confirm-Continue([string]$Question) {
    $answer = Read-Host "    $Question (y/N)"
    if ($answer -notmatch '^(y|yes)$') {
        throw 'Stopped by you.'
    }
}

function Invoke-Git {
    $output = & git -C $RepoRoot @args
    if ($LASTEXITCODE -ne 0) {
        throw "git $($args -join ' ') failed with exit code $LASTEXITCODE"
    }
    return $output
}

function Get-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    $fallback = 'C:\Program Files\CMake\bin\cmake.exe'
    if (Test-Path $fallback) {
        return $fallback
    }
    throw 'cmake.exe was not found. Install CMake or add it to PATH.'
}

function ConvertTo-CmdArgument([string]$Value) {
    if ($Value -eq '' -or $Value -match '[\s"&|<>^]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

# Build tools start at Idle priority and their children inherit it; this also catches any build
# process that started at a higher priority anyway.
function Set-BuildProcessesIdle {
    $names = 'cmake', 'MSBuild', 'cl', 'link', 'lib', 'Tracker', 'mspdbsrv', 'moc', 'uic', 'rcc', 'rc', 'mt'
    foreach ($process in @(Get-Process -Name $names -ErrorAction SilentlyContinue)) {
        try {
            if ($process.StartTime -ge $ScriptStart -and
                $process.PriorityClass -ne [Diagnostics.ProcessPriorityClass]::Idle) {
                $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::Idle
            }
        } catch {
        }
    }
}

function Invoke-Idle([string]$Exe, [string[]]$Arguments) {
    $argumentLine = ($Arguments | ForEach-Object { ConvertTo-CmdArgument $_ }) -join ' '
    $commandLine = "/d /v:on /c start `"`" /low /b /wait `"$Exe`" $argumentLine & exit !errorlevel!"
    $process = Start-Process -FilePath $env:ComSpec -ArgumentList $commandLine -NoNewWindow -PassThru
    $null = $process.Handle
    while (-not $process.WaitForExit(2000)) {
        Set-BuildProcessesIdle
    }
    $process.WaitForExit()
    return $process.ExitCode
}

function Get-GitHubToken {
    $lines = @('protocol=https', 'host=github.com', '') |
        & git -C $RepoRoot -c credential.helper=manager credential fill
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not read your GitHub login from Git Credential Manager (git credential fill failed).'
    }
    foreach ($line in @($lines)) {
        if ($line -like 'password=*') {
            return $line.Substring('password='.Length)
        }
    }
    throw 'Git Credential Manager returned no GitHub token. Sign in to GitHub once (for example with git push) and try again.'
}

function Invoke-GitHub {
    param(
        [string]$Method,
        [string]$Uri,
        $Body = $null,
        [string]$InFile = '',
        [string]$ContentType = ''
    )
    $parameters = @{
        Method = $Method
        Uri = $Uri
        Headers = $script:Headers
        UserAgent = 'eden-release-script'
        UseBasicParsing = $true
        TimeoutSec = 3600
    }
    if ($null -ne $Body) {
        $json = ConvertTo-Json -InputObject $Body -Depth 5
        $parameters.Body = [Text.Encoding]::UTF8.GetBytes($json)
        $parameters.ContentType = 'application/json; charset=utf-8'
    }
    if ($InFile -ne '') {
        $parameters.InFile = $InFile
        $parameters.ContentType = $ContentType
    }
    $response = Invoke-RestMethod @parameters
    return , $response
}

function Get-GitHubList([string]$Uri) {
    $all = @()
    $page = 1
    while ($true) {
        $separator = '?'
        if ($Uri.Contains('?')) {
            $separator = '&'
        }
        $response = Invoke-GitHub -Method Get -Uri "$Uri${separator}per_page=100&page=$page"
        $batch = @($response | Where-Object { $null -ne $_ })
        $all += $batch
        if ($batch.Count -lt 100) {
            break
        }
        $page++
        if ($page -gt 50) {
            break
        }
    }
    return , $all
}

function Get-BuildNumber([string]$Tag) {
    if ($Tag -match '^build-([0-9]+)$') {
        return [int64]$Matches[1]
    }
    return $null
}

function Resolve-TagCommit([object[]]$Refs, [string]$Tag) {
    foreach ($ref in $Refs) {
        if ($ref.ref -eq "refs/tags/$Tag") {
            if ($ref.object.type -eq 'commit') {
                return $ref.object.sha
            }
            if ($ref.object.type -eq 'tag') {
                $annotated = Invoke-GitHub -Method Get -Uri "$ApiBase/git/tags/$($ref.object.sha)"
                return $annotated.object.sha
            }
        }
    }
    return $null
}

$exitCode = 0
$releaseId = $null
$published = $false
$savedCl = $env:_CL_
try {
    Write-Host "Eden-Lichii release for $Owner/$Repo" -ForegroundColor Green
    Write-Info "Source: $RepoRoot"
    Write-Info "Build:  $BuildDir"

    Write-Step 'Checking the source tree'
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
        throw 'git.exe was not found on PATH.'
    }
    $cmake = Get-CMake
    $currentBranch = "$(Invoke-Git rev-parse --abbrev-ref HEAD | Select-Object -First 1)".Trim()
    if ($currentBranch -ne $Branch) {
        throw "The source is on branch '$currentBranch'. Switch to '$Branch' first."
    }
    $headSha = "$(Invoke-Git rev-parse HEAD | Select-Object -First 1)".Trim()
    $headShort = $headSha.Substring(0, 10)
    $headSubject = "$(Invoke-Git log -1 --format=%s HEAD | Select-Object -First 1)"
    Write-Info "HEAD $headShort $headSubject"
    $dirty = @(Invoke-Git status --porcelain --untracked-files=no)
    if ($dirty.Count -gt 0) {
        Write-Warn 'There are uncommitted changes. They would be built into the release but are not in the pushed commit:'
        $dirty | Select-Object -First 15 | ForEach-Object { Write-Info "  $_" }
        Confirm-Continue 'Build and publish anyway?'
    }

    Write-Step 'Checking that Eden is not running from the build folder'
    $binFull = [IO.Path]::GetFullPath($BinDir).TrimEnd('\')
    $running = @(Get-Process -Name 'eden', 'eden-cli' -ErrorAction SilentlyContinue | Where-Object {
            $path = $null
            try {
                $path = $_.Path
            } catch {
            }
            $path -and ((Split-Path -Parent $path).TrimEnd('\') -ieq $binFull)
        })
    if ($running.Count -gt 0) {
        throw "eden.exe is running from $binFull. Close it and run this again."
    }
    Write-Info 'Not running.'

    Write-Step 'Reading your GitHub login from Git Credential Manager'
    $token = Get-GitHubToken
    $script:Headers = @{
        Authorization = "Bearer $token"
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    Remove-Variable token
    $repoInfo = Invoke-GitHub -Method Get -Uri $ApiBase
    Write-Info "Signed in; repository $($repoInfo.full_name) found."
    if ($repoInfo.full_name -cne "$Owner/$Repo") {
        throw "GitHub spells the repository '$($repoInfo.full_name)', but the updater looks for download links under '$Owner/$Repo' and compares them case-sensitively. Make the names match in this script and in CMakeModules/GenerateSCMRev.cmake."
    }
    if ($repoInfo.private) {
        Write-Warn 'The repository is private. Installed copies check for updates without a login, so they will not see this release until the repository is public (GitHub > Settings > General > Danger Zone > Change visibility).'
    }

    Write-Step 'Finding the next build number'
    $releases = Get-GitHubList "$ApiBase/releases"
    $tagRefs = @()
    try {
        $tagRefs = Get-GitHubList "$ApiBase/git/matching-refs/tags/build-"
    } catch {
        Write-Warn "Could not list the repository's tags: $($_.Exception.Message)"
    }
    $highest = [int64]0
    $previousTag = $null
    foreach ($release in $releases) {
        $number = Get-BuildNumber $release.tag_name
        if ($null -eq $number -or $release.draft) {
            continue
        }
        if ($number -gt $highest) {
            $highest = $number
            $previousTag = $release.tag_name
        }
    }
    foreach ($ref in $tagRefs) {
        $number = Get-BuildNumber ($ref.ref -replace '^refs/tags/', '')
        if ($null -ne $number -and $number -gt $highest) {
            $highest = $number
        }
    }
    $buildNumber = $highest + 1
    $tag = "build-$buildNumber"
    if ($previousTag) {
        Write-Info "Latest release is $previousTag; this one is $tag."
    } else {
        Write-Info "No build-N releases yet; this one is $tag."
    }
    foreach ($release in $releases) {
        if ($release.draft -and $release.tag_name -eq $tag) {
            Write-Info "Removing the leftover draft release $tag from an earlier failed run."
            Invoke-GitHub -Method Delete -Uri "$ApiBase/releases/$($release.id)" | Out-Null
        }
    }

    $previousSha = $null
    if ($previousTag) {
        $previousSha = Resolve-TagCommit $tagRefs $previousTag
        if ($previousSha) {
            $ErrorActionPreference = 'Continue'
            & git -C $RepoRoot cat-file -e "$previousSha^{commit}" 2>&1 | Out-Null
            $found = $LASTEXITCODE -eq 0
            $ErrorActionPreference = 'Stop'
            if (-not $found) {
                Write-Warn "The commit of $previousTag ($previousSha) is not in this clone; the notes list recent commits instead."
                $previousSha = $null
            }
        }
        if ($previousSha -eq $headSha -and $dirty.Count -eq 0) {
            Write-Warn "HEAD is the same commit as $previousTag, so the new release would hold the same code."
            Confirm-Continue "Publish $tag anyway?"
        }
    }

    Write-Step "Pushing $Branch to $Remote/$RemoteBranch"
    & git -C $RepoRoot push $Remote "${Branch}:$RemoteBranch"
    if ($LASTEXITCODE -ne 0) {
        throw "git push $Remote ${Branch}:$RemoteBranch failed. Pull or fix the branch, then run this again."
    }

    Write-Step "Configuring $BuildDir as $tag ($Platform)"
    $configureArgs = @(
        '-S', $RepoRoot,
        '-B', $BuildDir,
        '-DENABLE_UPDATE_CHECKER=ON',
        "-DLICHII_BUILD_TAG=$tag",
        "-DLICHII_BUILD_PLATFORM=$Platform"
    )
    $code = Invoke-Idle $cmake $configureArgs
    if ($code -ne 0) {
        throw "CMake configure failed with exit code $code."
    }

    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    $buildCli = [bool](Select-String -Path $cache -Pattern '^YUZU_CMD:BOOL=(ON|TRUE|1)$' -Quiet)
    $targets = @('yuzu')
    if ($buildCli) {
        $targets += 'yuzu-cmd'
    }
    $targets += 'lichii-updater'

    $env:_CL_ = '/MP4'
    foreach ($target in $targets) {
        Write-Step "Building $target (Release, 2 projects at a time, 4 compilers each, Idle priority)"
        $started = Get-Date
        $buildArgs = @('--build', $BuildDir, '--config', 'Release', '--target', $target, '--',
            '/m:2', '/p:CL_MPCount=4', '/nodeReuse:false')
        $code = Invoke-Idle $cmake $buildArgs
        if ($code -ne 0) {
            throw "Building $target failed with exit code $code."
        }
        Write-Info ("{0} built in {1:hh\:mm\:ss}." -f $target, ((Get-Date) - $started))
    }
    $env:_CL_ = $savedCl

    Write-Step "Packaging $AssetName"
    $stage = Join-Path $OutDir 'stage'
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    $required = @('eden.exe', 'lichii-updater.exe', 'nx2_switches.txt')
    if ($buildCli) {
        $required += 'eden-cli.exe'
    }
    foreach ($name in $required) {
        $source = Join-Path $BinDir $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "$source was not produced."
        }
        Copy-Item -LiteralPath $source -Destination $stage
    }
    $source = Join-Path $BinDir 'qt.conf'
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination $stage
    }
    Get-ChildItem -LiteralPath $BinDir -Filter '*.dll' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $stage
    }
    $qtFolders = 'plugins', 'platforms', 'styles', 'imageformats', 'iconengines', 'tls', 'generic',
        'networkinformation', 'multimedia', 'sqldrivers', 'translations'
    foreach ($name in $qtFolders) {
        $source = Join-Path $BinDir $name
        if (Test-Path -LiteralPath $source -PathType Container) {
            Copy-Item -LiteralPath $source -Destination $stage -Recurse
        }
    }

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $zipPath = Join-Path $OutDir $AssetName
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath,
        [IO.Compression.CompressionLevel]::Optimal, $false)
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($entry in $zip.Entries) {
            Write-Info ("{0,12:N0}  {1}" -f $entry.Length, $entry.FullName)
        }
    } finally {
        $zip.Dispose()
    }
    $zipSize = (Get-Item -LiteralPath $zipPath).Length
    $sha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$zipPath.sha256" -Value "$sha256 *$AssetName" -Encoding ASCII
    Write-Info ("{0}: {1:N1} MB, SHA-256 {2}" -f $AssetName, ($zipSize / 1MB), $sha256)

    Write-Step 'Writing the release notes'
    if ($previousSha) {
        $subjects = @(Invoke-Git log --first-parent --format=%s "$previousSha..$headSha")
        $heading = "Changes since ${previousTag}:"
    } else {
        $subjects = @(Invoke-Git log --first-parent --format=%s -n 15 $headSha)
        $heading = 'Latest changes:'
    }
    $lines = @("Eden-Lichii $tag, built from $headShort.", '', $heading)
    $limit = 60
    $shown = @($subjects | Select-Object -First $limit)
    if ($shown.Count -eq 0) {
        $lines += '- No new commits.'
    }
    foreach ($subject in $shown) {
        $lines += "- $subject"
    }
    if ($subjects.Count -gt $limit) {
        $lines += "- ...and $($subjects.Count - $limit) more."
    }
    $lines += ''
    $lines += "Windows: extract $AssetName and run eden.exe. Installed copies offer this update when they start."
    $notes = $lines -join "`n"
    $lines | ForEach-Object { Write-Info $_ }

    Write-Step "Creating the release $tag on $Owner/$Repo"
    $release = Invoke-GitHub -Method Post -Uri "$ApiBase/releases" -Body @{
        tag_name = $tag
        target_commitish = $headSha
        name = "Eden-Lichii $tag ($($headSha.Substring(0, 7)))"
        body = $notes
        draft = $true
        prerelease = $false
    }
    $releaseId = $release.id

    Write-Step "Uploading $AssetName"
    $asset = Invoke-GitHub -Method Post -Uri "$UploadBase/releases/$releaseId/assets?name=$AssetName" `
        -InFile $zipPath -ContentType 'application/zip'
    if ([int64]$asset.size -ne $zipSize) {
        throw "GitHub stored $($asset.size) bytes, expected $zipSize."
    }
    $digestProperty = $asset.PSObject.Properties['digest']
    if ($digestProperty -and $digestProperty.Value) {
        if ($digestProperty.Value -ne "sha256:$sha256") {
            throw "GitHub reports $($digestProperty.Value) for the upload, expected sha256:$sha256."
        }
        Write-Info 'GitHub has the same SHA-256; the updater verifies downloads against it.'
    } else {
        Write-Warn 'GitHub returned no SHA-256 digest for the asset; the updater will only check its size.'
    }

    Write-Step 'Publishing the release'
    $release = Invoke-GitHub -Method Patch -Uri "$ApiBase/releases/$releaseId" -Body @{
        draft = $false
        make_latest = 'true'
    }
    $published = $true
    $expectedUrl = "https://github.com/$Owner/$Repo/releases/download/$tag/$AssetName"
    $publishedUrls = @($release.assets | ForEach-Object { $_.browser_download_url })
    if ($publishedUrls -cnotcontains $expectedUrl) {
        Write-Warn "The published asset link is not $expectedUrl, so installed copies will not find this release."
    }

    Write-Host ''
    Write-Host 'Done.' -ForegroundColor Green
    Write-Info "Tag:      $tag ($headShort)"
    Write-Info ("Zip:      {0} ({1:N1} MB)" -f $zipPath, ($zipSize / 1MB))
    Write-Info "SHA-256:  $sha256"
    Write-Info "Release:  $($release.html_url)"
    Write-Info "Your build\bin\Release\eden.exe is $tag and will offer the next release."
} catch {
    $exitCode = 1
    Write-Host ''
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ErrorDetails -and $_.ErrorDetails.Message) {
        Write-Host "GitHub said: $($_.ErrorDetails.Message)" -ForegroundColor Red
    }
    if ($releaseId -and -not $published) {
        try {
            Invoke-GitHub -Method Delete -Uri "$ApiBase/releases/$releaseId" | Out-Null
            Write-Info 'The unfinished draft release was removed.'
        } catch {
            Write-Warn "Could not remove the draft release $releaseId; delete it on GitHub before the next run."
        }
    }
} finally {
    $env:_CL_ = $savedCl
    $script:Headers = $null
}
exit $exitCode
