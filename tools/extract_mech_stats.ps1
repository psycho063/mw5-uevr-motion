# Extracts per-variant and per-chassis mech stats from the MW5 mod editor content.
#   powershell -File extract_mech_stats.ps1 -Root <...\MW5Mercs\Content> -Out <folder>
# Writes mech_variants.csv and mech_chassis.csv. Speeds are converted from cm/s to km/h.
param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Out)
$root=$Root; $out=$Out
$rows=New-Object System.Collections.Generic.List[object]
$bad=0
foreach($f in Get-ChildItem -LiteralPath $root -Recurse -Filter '*_MDA.json'){
  try{ $j=Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json }catch{ $bad++; continue }
  $s=$j.mechDataStats; if(-not $s -or -not $s.moveStats){ $bad++; continue }
  $rel=$f.FullName.Substring($root.Length+1)
  $model=''; if($j.mechModel -match '/([^/]+)/Model/'){ $model=$Matches[1] }   # folder above Model; clan mechs sit under Mechs/CLANS
  $mdl=$null; $capsule=''
  $t=$s.torsoStats
  $jj=0; if($s.mechImportData.componentInformation){ foreach($c in $s.mechImportData.componentInformation.PSObject.Properties){ $jj+=[int]$c.Value.jumpJets } }
  $rows.Add([pscustomobject]@{
    Chassis=$model; Variant=($f.BaseName -replace "_MDA`$",""); Source=$rel.Split([char]92)[0];
    Tons=$s.baseStats.tons
    TopSpeedKmh=[math]::Round($s.moveStats.topSpeed*0.036,1)
    ReverseSpeedKmh=[math]::Round($s.moveStats.reverseTopSpeed*0.036,1)
    TurnSpeedDegS=[math]::Round($s.moveStats.turnSpeed,1)
    TorsoYawRateDegS=[math]::Round($t.torsoTwistRate.yaw,1); TorsoPitchRateDegS=[math]::Round($t.torsoTwistRate.pitch,1)
    TorsoYawLimitDeg=$t.torsoTwistBounds.high.yaw; TorsoYawUnbound=$t.torsoTwistBounds.bIsYawUnbound
    TorsoPitchUpDeg=$t.torsoTwistBounds.high.pitch; TorsoPitchDownDeg=$t.torsoTwistBounds.low.pitch
    EngineRating=$s.mechImportData.baseMechInformation.engineRating
    JumpJets=$jj
  })
}
"files with stats: $($rows.Count), skipped: $bad"
$rows | Sort-Object Chassis,Variant | Export-Csv -LiteralPath (Join-Path $out 'mech_variants.csv') -NoTypeInformation -Encoding UTF8
# capsule per chassis from the model file
$caps=@{}
foreach($f in Get-ChildItem -LiteralPath $root -Recurse -Filter '*_MDL.json'){ try{ $m=Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json; if($m.capsule -match '/([^/.]+)\.'){ $caps[($f.BaseName -replace '_MDL$','')]=$Matches[1] } }catch{} }
$ch=$rows | ?{$_.Chassis} | Group-Object Chassis | %{
  $g=$_.Group; $sp=$g|Measure-Object TopSpeedKmh -Minimum -Maximum; $tn=$g|Measure-Object Tons -Minimum -Maximum; $tr=$g|Measure-Object TurnSpeedDegS -Minimum -Maximum
  $band= if($sp.Maximum -le 60){'Slow'}elseif($sp.Maximum -le 75){'Standard'}elseif($sp.Maximum -le 100){'Fast'}else{'Very Fast'}
  [pscustomobject]@{Chassis=$_.Name; Variants=$g.Count; Tons=$tn.Maximum; TopSpeedMinKmh=$sp.Minimum; TopSpeedMaxKmh=$sp.Maximum; BandAtMaxSpeed=$band
    TurnSpeedMinDegS=$tr.Minimum; TurnSpeedMaxDegS=$tr.Maximum
    TorsoYawLimitDeg=($g|Measure-Object TorsoYawLimitDeg -Maximum).Maximum; TorsoPitchUpDeg=($g|Measure-Object TorsoPitchUpDeg -Maximum).Maximum; TorsoPitchDownDeg=($g|Measure-Object TorsoPitchDownDeg -Minimum).Minimum
    WithJumpJets=($g|?{$_.JumpJets -gt 0}).Count; Capsule=$caps[$_.Name]
    TurnSwayAtTopSpeedMs2=[math]::Round(($sp.Maximum/3.6)*($tr.Maximum*[math]::PI/180)) }
}
$ch | Sort-Object Tons,Chassis | Export-Csv -LiteralPath (Join-Path $out 'mech_chassis.csv') -NoTypeInformation -Encoding UTF8
"chassis: $($ch.Count)"
