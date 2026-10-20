# Compare export tables: proxy wsock32.dll must expose exactly the same (ordinal, name) pairs as the real one.
# Game.exe and WOLAPI.dll import WSOCK32 by ordinal, so a wrong ordinal is a wrong function.
param(
    [string]$Proxy = (Join-Path $PSScriptRoot 'wsock32.dll'),
    [string]$Real = 'C:\Windows\SysWOW64\wsock32.dll'
)

function Rva2Off($b, $rva) {
    $pe = [BitConverter]::ToInt32($b, 0x3C); $n = [BitConverter]::ToUInt16($b, $pe + 6)
    $opt = [BitConverter]::ToUInt16($b, $pe + 20); $s = $pe + 24 + $opt
    for ($i = 0; $i -lt $n; $i++) {
        $o = $s + $i * 40
        $vs = [BitConverter]::ToUInt32($b, $o + 8); $va = [BitConverter]::ToUInt32($b, $o + 12)
        $rs = [BitConverter]::ToUInt32($b, $o + 16); $ro = [BitConverter]::ToUInt32($b, $o + 20)
        if ($rva -ge $va -and $rva -lt $va + [Math]::Max($vs, $rs)) { return $rva - $va + $ro }
    }
    return -1
}
function CStr($b, $off) { $e = $off; while ($b[$e] -ne 0) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $off, $e - $off) }

function Exports($path) {
    $b = [IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ([BitConverter]::ToUInt16($b, $pe + 4) -ne 0x14C) { throw "$path is not x86" }
    $e = Rva2Off $b ([BitConverter]::ToUInt32($b, $pe + 24 + 96))
    $base = [BitConverter]::ToUInt32($b, $e + 16)
    $nFn = [BitConverter]::ToUInt32($b, $e + 20); $nNames = [BitConverter]::ToUInt32($b, $e + 24)
    $fnOff = Rva2Off $b ([BitConverter]::ToUInt32($b, $e + 28))
    $namesOff = Rva2Off $b ([BitConverter]::ToUInt32($b, $e + 32))
    $ordOff = Rva2Off $b ([BitConverter]::ToUInt32($b, $e + 36))
    $names = @{}
    for ($i = 0; $i -lt $nNames; $i++) {
        $names[[int][BitConverter]::ToUInt16($b, $ordOff + 2 * $i)] = CStr $b (Rva2Off $b ([BitConverter]::ToUInt32($b, $namesOff + 4 * $i)))
    }
    $rows = @()
    for ($i = 0; $i -lt $nFn; $i++) {
        if ([BitConverter]::ToUInt32($b, $fnOff + 4 * $i) -eq 0) { continue }
        $nm = if ($names.ContainsKey($i)) { $names[$i] } else { '(NONAME)' }
        $rows += '{0} {1}' -f ($i + $base), $nm
    }
    return $rows
}

$p = Exports $Proxy
$r = Exports $Real
$diff = Compare-Object $r $p
"real exports=$($r.Count)  proxy exports=$($p.Count)  diff=$(@($diff).Count)"
if ($diff) { $diff | ForEach-Object { '{0} {1}' -f $_.SideIndicator, $_.InputObject }; exit 1 }
'EXPORTS MATCH'
exit 0
