[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$OutputDirectory,
 [Parameter(Mandatory)][string]$GameExe,
 [ValidateRange(5,3600)][int]$Seconds=30,
 [ValidateRange(1,10)][int]$Repetitions=2,
 [int]$APM=150,
 [switch]$IncludeAblations,
 [switch]$IncludeChunkComparison,
 [switch]$SkipPresentMon,
 [switch]$PresentMonV2,
 [Alias('WprPresent')][switch]$WprGpu,
 [string]$PresentMon
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(![IO.Path]::IsPathRooted($OutputDirectory)){$OutputDirectory=Join-Path $PWD $OutputDirectory}
if(![IO.Path]::IsPathRooted($GameExe)){$GameExe=Join-Path $PWD $GameExe}
if(Test-Path -LiteralPath $OutputDirectory){throw 'Choose a new diagnostics directory.'}
$null=New-Item -ItemType Directory -Path $OutputDirectory
$cases=@(
 @{name='poll-unrestricted';mode='Unrestricted';flags=@()},
 @{name='poll-inter';mode='InterFrame';flags=@()},
 @{name='poll-intra';mode='IntraFrame';flags=@()},
 @{name='event-unrestricted';mode='Unrestricted';flags=@('-LegoAdmissionEventWake')},
 @{name='event-inter';mode='InterFrame';flags=@('-LegoAdmissionEventWake')},
 @{name='event-intra';mode='IntraFrame';flags=@('-LegoAdmissionEventWake')}
)
if($IncludeChunkComparison -and !$IncludeAblations){
 foreach($mode in @('Unrestricted','InterFrame','IntraFrame')){
  $cases+=@{name=($mode+'-chunks8');mode=$mode;flags=@('-LegoCheckpointNodes=8')}
 }
}
if($IncludeAblations){
 foreach($mode in @('Unrestricted','InterFrame','IntraFrame')){
  foreach($ablation in @(
   @{name='no-records';flag='-LegoProfileNoChunkRecords'},
   @{name='chunks8';flag='-LegoCheckpointNodes=8'}
  )){$cases+=@{name=($mode+'-'+$ablation.name);mode=$mode;flags=@($ablation.flag)}}
 }
 foreach($mode in @('InterFrame','IntraFrame')){$cases+=@{name=($mode+'-no-fit');mode=$mode;flags=@('-LegoProfileNoFit')}}
 $cases+=@{name='unrestricted-no-checkpoints';mode='Unrestricted';flags=@('-LegoCheckpointNodes=0')}
}
$completed=@()
try {
 for($rep=0;$rep -lt $Repetitions;$rep++){
  # Reverse the complete order on alternating repetitions to reduce drift bias.
  $order=@($cases)
  if($rep%2){[array]::Reverse($order)}
  foreach($case in $order){
   $label=('{0}-r{1:d2}' -f $case.name,($rep+1))
   @{success=$null;current=$label;completed=$completed}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $OutputDirectory 'status.json') -Encoding UTF8
   $options=@{OutputDirectory=(Join-Path $OutputDirectory $label);GameExe=$GameExe;Mode=$case.mode;Seconds=$Seconds;APM=$APM;FPS=60;
    ExtraGameArguments=(@('-LegoAdmissionProfile')+$case.flags);Purpose='controlled-diagnosis';SkipPresentMon=$SkipPresentMon;PresentMonV2=$PresentMonV2;WprGpu=$WprGpu}
   if($PresentMon){$options.PresentMon=$PresentMon}
   & (Join-Path $PSScriptRoot 'Run-LegoTrial.ps1') @options *> (Join-Path $OutputDirectory ($label+'.log'))
   $completed+=$label
  }
 }
 & node (Join-Path $PSScriptRoot 'Analyze-LegoProfiles.mjs') $OutputDirectory
 if($LASTEXITCODE){throw 'Profile analysis failed'}
 @{success=$true;completed=$completed}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $OutputDirectory 'status.json') -Encoding UTF8
} catch {
 @{success=$false;error=$_.Exception.Message;completed=$completed}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $OutputDirectory 'status.json') -Encoding UTF8
 throw
}
