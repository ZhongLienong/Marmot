# Puts this folder's Marmot on the user's PATH and its prelude on MARMOT_PATH, as
# `dev.py install` does: each is appended unless it is there already, and the
# variable keeps its registry type. Run it again after moving the folder.
$ErrorActionPreference = 'Stop'
$environmentKey = 'HKCU:\Environment'

function Add-UserListEntry([string] $Name, [string] $Entry)
{
    $key = Get-Item $environmentKey
    $kind = if ($key.GetValueNames() -contains $Name) { $key.GetValueKind($Name) } else { 'ExpandString' }
    $entries = @(([string] $key.GetValue($Name, '', 'DoNotExpandEnvironmentNames')) -split ';' | Where-Object { $_.Trim() })
    $normalized = $Entry.TrimEnd('\')
    if (-not ($entries | Where-Object { $_.Trim().Trim('"').TrimEnd('\') -ieq $normalized }))
    {
        Set-ItemProperty -Path $environmentKey -Name $Name -Value (($entries + $Entry) -join ';') -Type $kind
    }
}

Add-UserListEntry 'PATH' (Join-Path $PSScriptRoot 'bin')
Add-UserListEntry 'MARMOT_PATH' (Join-Path $PSScriptRoot 'MarmotPrelude')

# Tells running programs, Explorer among them, that the environment changed.
Add-Type -Namespace MarmotSetup -Name Native -MemberDefinition '[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern System.IntPtr SendMessageTimeout(System.IntPtr window, uint message, System.UIntPtr wParam, string lParam, uint flags, uint timeout, out System.UIntPtr result);'
$result = [System.UIntPtr]::Zero
[void] [MarmotSetup.Native]::SendMessageTimeout([System.IntPtr] 0xffff, 0x1A, [System.UIntPtr]::Zero, 'Environment', 2, 5000, [ref] $result)

Write-Host 'Marmot is on PATH and MARMOT_PATH; open a new terminal to use it.'
