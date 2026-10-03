#Requires -Version 5.1
# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
  Outside-GUI semantic oracle for the native object-drag trial. Parses the three
  native-saved SVG snapshots (baseline, endpoint, returned) and validates that
  exactly the expected target translated by the expected screen distance and that
  the document/viewport/every unrelated object was preserved.

.DESCRIPTION
  Pure XML parsing with the Windows PowerShell 5.1 System.Xml stack; no external
  dependency, no GUI, no input, no app source. It never derives the zoom from the
  measured displacement: the baseline namedview inkscape:zoom is the independent
  logical-px-per-document-unit scale at DPI 96.

  Two independent scales are used, and neither is inferred from the observed
  translation. namedview inkscape:zoom is the native logical-px-per-document-unit
  scale. PhysicalPixelsPerLogicalPixel is the caller-supplied
  physical-screen-px-per-GTK-logical-px factor for the target display (default 1,
  i.e. the previous behaviour). Physical travel is
  docDelta * baselineZoom * PhysicalPixelsPerLogicalPixel. ExpectedScreenDelta and
  ToleranceScreenPx are expressed in PHYSICAL screen px. The factor is supplied
  from a separate runtime calibration; it is never derived from screen DPI, a
  widget integer scale store, a screenshot, or the measured drag distance.

  Narrow known-fixture contract (see ROOT-REVIEW / ROOT-DESIGN). This is NOT a
  generic SVG engine; a document outside the contract is rejected, never
  approximately accepted:
  - Exactly three renderable elements, all <rect>, all direct children of the root
    <svg>: background, tinyControl and the dragged dragTarget. No groups, text,
    gradients or other renderable nodes. Renderable-entry cardinality is asserted
    so a nested PowerShell collection can never masquerade as one node.
  - The root <svg> and the two static rects must have no transform at all. The
    target may change only by a native rect x/y translation or an SVG
    translate()/linear-identity matrix translation; scale, rotate, skew and every
    other transform are rejected.
  - All three snapshots must share the same root units, 1 CSS px per viewBox unit,
    and a stable namedview viewport.
  - Every numeric value must be present and finite: NaN and +/-Infinity are
    rejected even though [double]::TryParse accepts them.
  - namedview inkscape:zoom / inkscape:cx / inkscape:cy must be real Inkscape
    namespace attributes (a namespace-confused local attribute is rejected), the
    zoom must be positive, and document rotation must be zero.
  - Every unrelated rect (and every attribute of it, including x/y) must be
    byte-semantic equal between the canonical baseline save and the other saves,
    with only a tight 1e-6 numeric epsilon. There is no 1.5-screen-px allowance
    outside the target translation.
  - Every target attribute except the permitted x/y/transform translation carrier
    must be preserved with the same tight numeric epsilon; a changed fill, stroke,
    opacity, rx, style, etc. is invalid. A missing returned width/height fails.
  - Endpoint target translation must be ExpectedScreenDelta PHYSICAL px in +x and
    0 PHYSICAL px in y (within ToleranceScreenPx physical px). The returned
    translation must be back within ToleranceScreenPx physical px of the baseline.
    The 1.5 physical-px tolerance applies only to target translation position and
    to the namedview viewport shift (converted to document units as
    ToleranceScreenPx / (zoom * factor)); it never relaxes the tight 1e-6
    geometry/style preservation of unrelated or target attributes.

.PARAMETER BaselineSvg
  Native Ctrl+S save taken before selection; the canonical reference.

.PARAMETER EndpointSvg
  Native Ctrl+S save taken at the drag endpoint (outside the measured window).

.PARAMETER ReturnedSvg
  Native Ctrl+S save taken after the separate return leg.

.PARAMETER TargetId
  Id of the single dragged object. Default dragTarget.

.PARAMETER ExpectedScreenDelta
  Expected endpoint screen-px travel along +x. Default 120.

.PARAMETER ToleranceScreenPx
  Physical screen-px tolerance for endpoint/return translation and for the
  namedview viewport shift. Default 1.5.

.PARAMETER PhysicalPixelsPerLogicalPixel
  Physical screen px per GTK logical px for the target display. Supplied
  independently by the caller from a separate runtime calibration; default 1.0
  preserves the previous behaviour. Must be finite and in (0, 1000]. It is never
  inferred from DPI, widget scale, a screenshot, or the measured translation.

.PARAMETER OutputJson
  Optional path; the full oracle result object is always written there when given.

.NOTES
  Exit 0 = valid, 2 = invalid/failed. Diagnostic only; no PASS issue.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$BaselineSvg,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$EndpointSvg,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$ReturnedSvg,
    [ValidateNotNullOrEmpty()][string]$TargetId = 'dragTarget',
    [ValidateRange(0.0, 100000.0)][double]$ExpectedScreenDelta = 120.0,
    [ValidateRange(0.0, 1000.0)][double]$ToleranceScreenPx = 1.5,
    [double]$PhysicalPixelsPerLogicalPixel = 1.0,
    [string]$OutputJson
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$OracleSchema    = 'vacards-app-drag-oracle/2'
$InkscapeNs      = 'http://www.inkscape.org/namespaces/inkscape'
$NumericEps      = 0.000001
$MaxPhysicalPixelsPerLogicalPixel = 1000.0
$PhysicalScaleSource = 'caller-supplied independent runtime calibration; not derived from DPI, widget scale, screenshot, or observed translation'
$KnownRectIds    = @('background', 'dragTarget', 'tinyControl')
$SkipLocalNames  = @('namedview', 'title', 'desc', 'metadata', 'script', 'style', 'defs')
$PermittedTargetAttrs = @('x', 'y', 'transform')
$NumericAttrs    = @('x', 'y', 'width', 'height', 'cx', 'cy', 'r', 'rx', 'ry',
                     'x1', 'y1', 'x2', 'y2', 'offset', 'dx', 'dy', 'fx', 'fy',
                     'font-size', 'stroke-width', 'fill-opacity', 'stroke-opacity',
                     'opacity', 'stop-opacity', 'stroke-miterlimit', 'stroke-dashoffset',
                     'letter-spacing', 'word-spacing')
$Invariant       = [System.Globalization.CultureInfo]::InvariantCulture

$errors   = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Add-OracleError([string]$Message) { $errors.Add($Message) | Out-Null }
function Add-OracleWarning([string]$Message) { $warnings.Add($Message) | Out-Null }

# ---------------------------------------------------------------------------
# Independent display-scale parameter. This value must be supplied by the caller
# from a separate runtime calibration (or left at 1.0 for the default identity).
# It is never derived from screen DPI, a widget/GDI integer scale store, a
# screenshot, or the observed object translation. It maps native logical px
# (namedview zoom space) to physical screen px. Validated here, in the body, so
# an out-of-range value still produces an 'invalid' oracle result with a clear
# error and exit 2 instead of a raw parameter-binding crash.
# ---------------------------------------------------------------------------
$Factor = $PhysicalPixelsPerLogicalPixel
$FactorRaw = '<unprintable>'
try { $FactorRaw = $Factor.ToString($Invariant) } catch { }
$FactorForJson = $null
if ([double]::IsNaN($Factor) -or [double]::IsInfinity($Factor)) {
    Add-OracleError ('PhysicalPixelsPerLogicalPixel must be finite; got ' + $FactorRaw)
} elseif ($Factor -le 0.0) {
    Add-OracleError ('PhysicalPixelsPerLogicalPixel must be > 0; got ' + $FactorRaw)
} elseif ($Factor -gt $MaxPhysicalPixelsPerLogicalPixel) {
    Add-OracleError ('PhysicalPixelsPerLogicalPixel must be <= ' + $MaxPhysicalPixelsPerLogicalPixel + '; got ' + $FactorRaw)
} else {
    $FactorForJson = $Factor
}

function Get-FileSha256([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}

function ConvertTo-Double([string]$Text) {
    # Rejects absent, non-numeric, NaN and +/-Infinity. [double]::TryParse alone
    # accepts the special NaN/Infinity tokens, which must never gate as geometry.
    if ($null -eq $Text) { return $null }
    $trimmed = $Text.Trim()
    if ($trimmed -eq '') { return $null }
    $value = 0.0
    if (-not [double]::TryParse($trimmed, [System.Globalization.NumberStyles]::Float,
            $Invariant, [ref]$value)) { return $null }
    if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) { return $null }
    return $value
}

function Get-AttrAny($Node, [string]$LocalName) {
    if ($null -eq $Node) { return $null }
    foreach ($attr in $Node.Attributes) {
        if ($attr.LocalName -eq $LocalName) { return $attr.Value }
    }
    return $null
}

function Get-NsAttr($Node, [string]$NamespaceUri, [string]$LocalName) {
    # Namespace-aware lookup: a same-local-name attribute in the wrong namespace
    # (namespace confusion) is not accepted.
    if ($null -eq $Node) { return $null }
    foreach ($attr in $Node.Attributes) {
        if ($attr.NamespaceURI -eq $NamespaceUri -and $attr.LocalName -eq $LocalName) { return $attr.Value }
    }
    return $null
}

function Convert-LengthToPx([string]$Text) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $value = $null
    if ([regex]::IsMatch($Text.Trim(), '^[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?(px)?$')) {
        $value = ConvertTo-Double ($Text -replace 'px$', '')
        return $value
    }
    $m = [regex]::Match($Text.Trim(), '^([+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?)\s*([a-zA-Z]+)$')
    if (-not $m.Success) { return $null }
    $number = ConvertTo-Double $m.Groups[1].Value
    if ($null -eq $number) { return $null }
    $unit = $m.Groups[5].Value.ToLowerInvariant()
    switch ($unit) {
        'px' { return $number }
        'in' { return $number * 96.0 }
        'pt' { return $number * (96.0 / 72.0) }
        'pc' { return $number * 16.0 }
        'mm' { return $number * (96.0 / 25.4) }
        'cm' { return $number * (96.0 / 2.54) }
        'q'  { return $number * (96.0 / 25.4) / 4.0 }
        default { return $null }
    }
}

function Read-SvgDocument([string]$Path, [string]$Label) {
    $result = [ordered]@{ available = $false; doc = $null; sha256 = $null; error = $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $result.error = $Label + ' SVG is missing: ' + $Path
        return $result
    }
    try { $result.sha256 = Get-FileSha256 $Path } catch { $result.error = $Label + ' SVG hash unavailable'; return $result }
    try {
        $doc = New-Object System.Xml.XmlDocument
        $doc.XmlResolver = $null
        $doc.Load($Path)
        if ($null -eq $doc.DocumentElement -or $doc.DocumentElement.LocalName -ne 'svg') {
            $result.error = $Label + ' SVG root element is not <svg>'
            return $result
        }
        $result.doc = $doc
        $result.available = $true
    } catch {
        $result.error = $Label + ' SVG is not well-formed XML: ' + $_.Exception.Message
    }
    return $result
}

function Get-RootUnits($Doc, [string]$Label) {
    $result = [ordered]@{
        available = $false; width = $null; height = $null
        viewBoxX = $null; viewBoxY = $null; viewBoxWidth = $null; viewBoxHeight = $null
        viewBox = $null; cssPxPerUnit = $null; error = $null
    }
    $root = $Doc.DocumentElement
    $result.width = Convert-LengthToPx (Get-AttrAny $root 'width')
    $result.height = Convert-LengthToPx (Get-AttrAny $root 'height')
    if ($null -eq $result.width -or $null -eq $result.height) {
        $result.error = $Label + ' root width/height is missing or not a finite supported CSS length'
        return $result
    }
    $viewBoxText = Get-AttrAny $root 'viewBox'
    if ([string]::IsNullOrWhiteSpace($viewBoxText)) {
        $result.error = $Label + ' root viewBox is missing'
        return $result
    }
    $parts = @($viewBoxText -split '[,\s]+' | Where-Object { $_ -ne '' })
    if ($parts.Count -ne 4) {
        $result.error = $Label + ' root viewBox is not four numbers'
        return $result
    }
    $vx = ConvertTo-Double $parts[0]; $vy = ConvertTo-Double $parts[1]
    $vw = ConvertTo-Double $parts[2]; $vh = ConvertTo-Double $parts[3]
    if ($null -eq $vx -or $null -eq $vy -or $null -eq $vw -or $null -eq $vh -or $vw -le 0 -or $vh -le 0) {
        $result.error = $Label + ' root viewBox is invalid or non-finite'
        return $result
    }
    $result.viewBox = @($vx, $vy, $vw, $vh)
    $result.viewBoxX = $vx; $result.viewBoxY = $vy
    $result.viewBoxWidth = $vw; $result.viewBoxHeight = $vh
    if ([Math]::Abs($result.width - $vw) -gt 0.01 -or [Math]::Abs($result.height - $vh) -gt 0.01) {
        $result.error = ($Label + ' root is not 1 CSS px per viewBox unit (width=' + $result.width +
                         ' viewBoxW=' + $vw + ', height=' + $result.height + ' viewBoxH=' + $vh + ')')
        return $result
    }
    $result.cssPxPerUnit = 1.0
    $result.available = $true
    return $result
}

function Get-NamedViewNode($Node) {
    foreach ($child in $Node.ChildNodes) {
        if ($child.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
        if ($child.LocalName -eq 'namedview') { return $child }
    }
    return $null
}

function Get-NamedViewState($NamedViewNode, [string]$Label) {
    $state = [ordered]@{
        present = ($null -ne $NamedViewNode); zoom = $null; cx = $null; cy = $null
        rotation = 0.0; rotationRaw = $null; error = $null
    }
    if ($null -eq $NamedViewNode) {
        $state.error = $Label + ' namedview is missing'
        return $state
    }
    $state.zoom = ConvertTo-Double (Get-NsAttr $NamedViewNode $InkscapeNs 'zoom')
    if ($null -eq $state.zoom -or $state.zoom -le 0) {
        $state.error = $Label + ' namedview inkscape:zoom is missing, non-finite or not positive'
        return $state
    }
    $state.cx = ConvertTo-Double (Get-NsAttr $NamedViewNode $InkscapeNs 'cx')
    $state.cy = ConvertTo-Double (Get-NsAttr $NamedViewNode $InkscapeNs 'cy')
    if ($null -eq $state.cx -or $null -eq $state.cy) {
        $state.error = $Label + ' namedview inkscape:cx/cy is missing, namespace-confused or non-finite'
        return $state
    }
    $rotText = Get-NsAttr $NamedViewNode $InkscapeNs 'document-rotation'
    if ([string]::IsNullOrWhiteSpace($rotText)) { $rotText = Get-NsAttr $NamedViewNode $InkscapeNs 'rotation' }
    $state.rotationRaw = $rotText
    if (-not [string]::IsNullOrWhiteSpace($rotText)) {
        $rot = ConvertTo-Double $rotText
        if ($null -eq $rot) {
            $state.error = $Label + ' namedview document rotation is not finite numeric'
            return $state
        }
        $state.rotation = $rot
    }
    if ([Math]::Abs($state.rotation) -gt 0.000001) {
        $state.error = $Label + ' namedview document rotation is not zero (' + $state.rotationRaw + ')'
        return $state
    }
    return $state
}

function Get-DirectRenderableElements($Root) {
    # One entry per renderable direct child of the root <svg>. Returns a flat
    # XmlElement array; never a nested ArrayList. Callers must @()-wrap and assert
    # the count so a nested collection cannot pass as a single node.
    $list = New-Object 'System.Collections.Generic.List[System.Xml.XmlElement]'
    foreach ($child in $Root.ChildNodes) {
        if ($child.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
        if ($SkipLocalNames -contains $child.LocalName) { continue }
        $list.Add($child) | Out-Null
    }
    return $list.ToArray()
}

function Test-KnownRectTree($Doc, [string]$Label) {
    # Enforce the narrow known fixture: exactly three direct root <rect> children
    # with the known ids, no root transform, no transform on the static rects.
    $root = $Doc.DocumentElement
    $rootTransform = Get-AttrAny $root 'transform'
    if (-not [string]::IsNullOrWhiteSpace($rootTransform)) {
        Add-OracleError ($Label + ' root <svg> has a transform (' + $rootTransform + '); root/ancestor transforms are rejected')
    }
    $direct = @(Get-DirectRenderableElements $root)
    if ($direct.Count -ne 3) {
        Add-OracleError ($Label + ' renderable element count is ' + $direct.Count + ', expected exactly 3 direct root rects (narrow known fixture)')
        return $null
    }
    $seen = @{}
    foreach ($element in $direct) {
        if ($element -isnot [System.Xml.XmlElement]) {
            Add-OracleError ($Label + ' renderable entry is not a single element (nested array cardinality); one entry per rect is required')
            return $null
        }
        if ($element.LocalName -ne 'rect') {
            Add-OracleError ($Label + ' renderable element <' + $element.LocalName + '> is not a rect')
        }
        $id = Get-AttrAny $element 'id'
        if ([string]::IsNullOrWhiteSpace($id)) {
            Add-OracleError ($Label + ' renderable rect has no id')
        } elseif ($seen.ContainsKey($id)) {
            Add-OracleError ($Label + ' duplicate renderable id ' + $id)
        } else {
            $seen[$id] = $element
        }
        $transform = Get-AttrAny $element 'transform'
        if ($id -ne $TargetId -and -not [string]::IsNullOrWhiteSpace($transform)) {
            Add-OracleError ($Label + ' static rect ' + $id + ' has a transform; unrelated nodes must stay untransformed')
        }
    }
    foreach ($known in $KnownRectIds) {
        if (-not $seen.ContainsKey($known)) { Add-OracleError ($Label + ' is missing the known rect id ' + $known) }
    }
    foreach ($id in @($seen.Keys)) {
        if (-not ($KnownRectIds -contains $id)) {
            Add-OracleError ($Label + ' has an unexpected renderable id ' + $id + '; the narrow known fixture has no generic SVG content')
        }
    }
    return $direct
}

function Normalize-StyleValue([string]$Value) {
    if ($null -eq $Value) { return '' }
    return ($Value -replace '\s+', '').TrimEnd(';')
}

function Test-ValueEqual([string]$Key, [string]$BaseValue, [string]$OtherValue) {
    if ($Key -eq 'style') {
        return ((Normalize-StyleValue $BaseValue) -ceq (Normalize-StyleValue $OtherValue))
    }
    if ($NumericAttrs -contains $Key) {
        $b = ConvertTo-Double $BaseValue
        $o = ConvertTo-Double $OtherValue
        if ($null -ne $b -and $null -ne $o) { return ([Math]::Abs($b - $o) -le $NumericEps) }
    }
    return ($BaseValue -ceq $OtherValue)
}

function Get-AttrMap($Node) {
    $map = [ordered]@{}
    foreach ($attr in $Node.Attributes) { $map[$attr.Name] = $attr.Value }
    return $map
}

function Compare-FullElement($BaseElement, $OtherElement, [string]$Label) {
    # Every attribute of an unrelated node, x/y included, tight 1e-6 numeric.
    $b = Get-AttrMap $BaseElement
    $o = Get-AttrMap $OtherElement
    if ($b.Keys.Count -ne $o.Keys.Count) {
        Add-OracleError ($Label + ' attribute set differs from baseline (' + $o.Keys.Count + ' vs ' + $b.Keys.Count + ')')
        return
    }
    foreach ($key in $b.Keys) {
        if (-not $o.Contains($key)) { Add-OracleError ($Label + ' is missing attribute ' + $key); continue }
        if (-not (Test-ValueEqual $key $b[$key] $o[$key])) {
            Add-OracleError ($Label + ' attribute ' + $key + ' differs (' + $o[$key] + ' vs ' + $b[$key] + ')')
        }
    }
}

function Compare-TargetAttributes($BaseElement, $OtherElement, [string]$Label) {
    # Preserve every target attribute except the permitted x/y/transform
    # translation carriers. Missing returned width/height is an error, not skipped.
    $b = Get-AttrMap $BaseElement
    $o = Get-AttrMap $OtherElement
    if (-not $b.Contains('width') -or -not $b.Contains('height')) {
        Add-OracleError 'baseline target has no width/height'
    } else {
        $bw = ConvertTo-Double $b['width']; $bh = ConvertTo-Double $b['height']
        if ($null -eq $bw -or $null -eq $bh -or $bw -le 0 -or $bh -le 0) {
            Add-OracleError 'baseline target width/height is not positive finite'
        }
    }
    foreach ($needed in @('width', 'height')) {
        if (-not $o.Contains($needed)) { Add-OracleError ($Label + ' target is missing required ' + $needed) }
    }
    foreach ($key in $b.Keys) {
        if ($PermittedTargetAttrs -contains $key) { continue }
        if (-not $o.Contains($key)) { Add-OracleError ($Label + ' target is missing attribute ' + $key); continue }
        if (-not (Test-ValueEqual $key $b[$key] $o[$key])) {
            Add-OracleError ($Label + ' target attribute ' + $key + ' differs (' + $o[$key] + ' vs ' + $b[$key] + ')')
        }
    }
    foreach ($key in $o.Keys) {
        if ($PermittedTargetAttrs -contains $key) { continue }
        if (-not $b.Contains($key)) { Add-OracleError ($Label + ' target has an unexpected new attribute ' + $key) }
    }
}

function Get-TransformTranslation([string]$TransformText, [string]$Where) {
    $tx = 0.0; $ty = 0.0
    if ([string]::IsNullOrWhiteSpace($TransformText)) { return @($tx, $ty) }
    $matches = [regex]::Matches($TransformText, '([a-zA-Z]+)\s*\(([^)]*)\)')
    if ($matches.Count -eq 0) { throw ("unsupported transform on " + $Where + ": '" + $TransformText + "'") }
    $stripped = $TransformText
    foreach ($m in $matches) { $stripped = $stripped.Replace($m.Value, '') }
    if ($stripped.Trim(' ', "`t", "`r", "`n", ',') -ne '') {
        throw ("unsupported transform text on " + $Where + ": '" + $TransformText + "'")
    }
    foreach ($m in $matches) {
        $fn = $m.Groups[1].Value.ToLowerInvariant()
        $parts = @($m.Groups[2].Value -split '[,\s]+' | Where-Object { $_ -ne '' })
        switch ($fn) {
            'translate' {
                if ($parts.Count -lt 1 -or $parts.Count -gt 2) { throw ("translate arity on " + $Where) }
                $v0 = ConvertTo-Double $parts[0]
                if ($null -eq $v0) { throw ("translate value on " + $Where) }
                $tx += $v0
                if ($parts.Count -eq 2) {
                    $v1 = ConvertTo-Double $parts[1]
                    if ($null -eq $v1) { throw ("translate value on " + $Where) }
                    $ty += $v1
                }
            }
            'matrix' {
                if ($parts.Count -ne 6) { throw ("matrix arity on " + $Where) }
                $vals = @()
                foreach ($p in $parts) {
                    $v = ConvertTo-Double $p
                    if ($null -eq $v) { throw ("matrix value on " + $Where) }
                    $vals += $v
                }
                if ([Math]::Abs($vals[0] - 1.0) -gt 1e-9 -or [Math]::Abs($vals[3] - 1.0) -gt 1e-9 -or
                    [Math]::Abs($vals[1]) -gt 1e-9 -or [Math]::Abs($vals[2]) -gt 1e-9) {
                    throw ("non-identity linear matrix on " + $Where + " (scale/rotation/skew rejected)")
                }
                $tx += $vals[4]; $ty += $vals[5]
            }
            default { throw ("unsupported transform function '" + $fn + "' on " + $Where) }
        }
    }
    return @($tx, $ty)
}

function Get-ElementPosition($Node, [string]$Where) {
    $x = Get-AttrAny $Node 'x'; $y = Get-AttrAny $Node 'y'
    if ($null -eq $x -or $null -eq $y) { throw ("target " + $Where + " has no native x/y geometry") }
    $xv = ConvertTo-Double $x; $yv = ConvertTo-Double $y
    if ($null -eq $xv -or $null -eq $yv) { throw ("target " + $Where + " x/y is not finite numeric") }
    $t = Get-TransformTranslation (Get-AttrAny $Node 'transform') $Where
    return @(($xv + $t[0]), ($yv + $t[1]))
}

# ---------------------------------------------------------------------------
# Load and parse.
# ---------------------------------------------------------------------------
$baselineRead = Read-SvgDocument $BaselineSvg 'baseline'
$endpointRead = Read-SvgDocument $EndpointSvg 'endpoint'
$returnedRead = Read-SvgDocument $ReturnedSvg 'returned'
foreach ($read in @($baselineRead, $endpointRead, $returnedRead)) {
    if (-not $read.available) { Add-OracleError $read.error }
}

$result = [ordered]@{
    schema   = $OracleSchema
    status   = 'invalid'
    valid    = $false
    targetId = $TargetId
    expectedScreenDelta = $ExpectedScreenDelta
    toleranceScreenPx   = $ToleranceScreenPx
    physicalPixelsPerLogicalPixel = $FactorForJson
    physicalPixelsPerLogicalPixelRaw = $FactorRaw
    physicalScaleSource = $PhysicalScaleSource
    inputs   = [ordered]@{
        baseline = [ordered]@{ path = $BaselineSvg; sha256 = $baselineRead.sha256 }
        endpoint = [ordered]@{ path = $EndpointSvg; sha256 = $endpointRead.sha256 }
        returned = [ordered]@{ path = $ReturnedSvg; sha256 = $returnedRead.sha256 }
    }
    baselineRootUnits = $null
    renderableEntryCount = 0
    renderableIds     = @()
    namedview = [ordered]@{}
    target = [ordered]@{}
    checks = [ordered]@{}
    errors = @()
    warnings = @()
}

$baselineUnits = $null; $bState = $null; $eState = $null; $rState = $null
$bRects = $null; $eRects = $null; $rRects = $null
$targetChecks = [ordered]@{}

if ($errors.Count -eq 0) {
    $baselineUnits = Get-RootUnits $baselineRead.doc 'baseline'
    $endpointUnits = Get-RootUnits $endpointRead.doc 'endpoint'
    $returnedUnits = Get-RootUnits $returnedRead.doc 'returned'
    foreach ($u in @($baselineUnits, $endpointUnits, $returnedUnits)) {
        if (-not $u.available) { Add-OracleError $u.error }
    }
    $result.baselineRootUnits = [ordered]@{
        width = $baselineUnits.width; height = $baselineUnits.height
        viewBox = $baselineUnits.viewBox; cssPxPerUnit = $baselineUnits.cssPxPerUnit
    }
    if ($baselineUnits.available) {
        foreach ($pair in @(@('endpoint', $endpointUnits), @('returned', $returnedUnits))) {
            $label = $pair[0]; $u = $pair[1]
            if ($u.available -and (
                    [Math]::Abs($u.width - $baselineUnits.width) -gt 0.01 -or
                    [Math]::Abs($u.height - $baselineUnits.height) -gt 0.01 -or
                    [Math]::Abs($u.viewBoxX - $baselineUnits.viewBoxX) -gt 0.01 -or
                    [Math]::Abs($u.viewBoxY - $baselineUnits.viewBoxY) -gt 0.01 -or
                    [Math]::Abs($u.viewBoxWidth - $baselineUnits.viewBoxWidth) -gt 0.01 -or
                    [Math]::Abs($u.viewBoxHeight - $baselineUnits.viewBoxHeight) -gt 0.01)) {
                Add-OracleError ($label + ' root units/viewBox differ from baseline (document resized or panned)')
            }
        }
    }
}

if ($errors.Count -eq 0) {
    $bState = Get-NamedViewState (Get-NamedViewNode $baselineRead.doc.DocumentElement) 'baseline'
    $eState = Get-NamedViewState (Get-NamedViewNode $endpointRead.doc.DocumentElement) 'endpoint'
    $rState = Get-NamedViewState (Get-NamedViewNode $returnedRead.doc.DocumentElement) 'returned'
    foreach ($s in @($bState, $eState, $rState)) { if ($s.error) { Add-OracleError $s.error } }
    $result.namedview = [ordered]@{
        baselineZoom = $bState.zoom; endpointZoom = $eState.zoom; returnedZoom = $rState.zoom
        baselineCx = $bState.cx; baselineCy = $bState.cy
        endpointCx = $eState.cx; endpointCy = $eState.cy
        returnedCx = $rState.cx; returnedCy = $rState.cy
        rotation = $bState.rotation
    }
}

if ($errors.Count -eq 0) {
    if ([Math]::Abs($eState.zoom - $bState.zoom) -gt ([Math]::Max(1e-6, [Math]::Abs($bState.zoom) * 1e-6)) -or
        [Math]::Abs($rState.zoom - $bState.zoom) -gt ([Math]::Max(1e-6, [Math]::Abs($bState.zoom) * 1e-6))) {
        Add-OracleError ('namedview zoom is not stable between saves (baseline=' + $bState.zoom +
                         ' endpoint=' + $eState.zoom + ' returned=' + $rState.zoom + '); viewport shift rejected')
    }
    # namedview cx/cy is in document units. A doc-unit shift becomes
    # (docShift * zoom) logical px and (docShift * zoom * factor) physical px, so
    # the physical tolerance converts to document units via /(zoom * factor).
    $cxTol = $ToleranceScreenPx / ($bState.zoom * $Factor)
    foreach ($pair in @(@('endpoint', $eState), @('returned', $rState))) {
        $label = $pair[0]; $s = $pair[1]
        if ([Math]::Abs($s.cx - $bState.cx) -gt $cxTol) {
            Add-OracleError ($label + ' namedview inkscape:cx moved (' + $s.cx + ' vs ' + $bState.cx + '); viewport shift rejected')
        }
        if ([Math]::Abs($s.cy - $bState.cy) -gt $cxTol) {
            Add-OracleError ($label + ' namedview inkscape:cy moved (' + $s.cy + ' vs ' + $bState.cy + '); viewport shift rejected')
        }
    }
}

if ($errors.Count -eq 0) {
    $bRects = Test-KnownRectTree $baselineRead.doc 'baseline'
    $eRects = Test-KnownRectTree $endpointRead.doc 'endpoint'
    $rRects = Test-KnownRectTree $returnedRead.doc 'returned'
    if ($null -ne $bRects) {
        $result.renderableEntryCount = $bRects.Count
        $result.renderableIds = @($bRects | ForEach-Object { Get-AttrAny $_ 'id' })
    }
    if ($errors.Count -eq 0 -and $null -ne $bRects -and $null -ne $eRects -and $null -ne $rRects) {
        $bSeq = @($bRects | ForEach-Object { Get-AttrAny $_ 'id' })
        foreach ($pair in @(@('endpoint', $eRects), @('returned', $rRects))) {
            $label = $pair[0]; $rects = $pair[1]
            $seq = @($rects | ForEach-Object { Get-AttrAny $_ 'id' })
            if (($seq -join '|') -ne ($bSeq -join '|')) {
                Add-OracleError ($label + ' renderable rect order/ids differ from baseline (' + ($seq -join ',') + ' vs ' + ($bSeq -join ',') + ')')
            }
        }
        $bById = @{}; foreach ($element in $bRects) { $bById[(Get-AttrAny $element 'id')] = $element }
        $eById = @{}; foreach ($element in $eRects) { $eById[(Get-AttrAny $element 'id')] = $element }
        $rById = @{}; foreach ($element in $rRects) { $rById[(Get-AttrAny $element 'id')] = $element }

        foreach ($id in $KnownRectIds) {
            if ($id -eq $TargetId) { continue }
            if ($errors.Count -gt 0) { break }
            Compare-FullElement $bById[$id] $eById[$id] ('endpoint static rect ' + $id)
            Compare-FullElement $bById[$id] $rById[$id] ('returned static rect ' + $id)
        }

        $bTarget = $bById[$TargetId]; $eTarget = $eById[$TargetId]; $rTarget = $rById[$TargetId]
        if ($errors.Count -eq 0) {
            Compare-TargetAttributes $bTarget $eTarget 'endpoint'
            Compare-TargetAttributes $bTarget $rTarget 'returned'
        }
        $targetChecks.baselineWidth = ConvertTo-Double (Get-AttrAny $bTarget 'width')
        $targetChecks.baselineHeight = ConvertTo-Double (Get-AttrAny $bTarget 'height')
        $targetChecks.endpointWidth = ConvertTo-Double (Get-AttrAny $eTarget 'width')
        $targetChecks.endpointHeight = ConvertTo-Double (Get-AttrAny $eTarget 'height')
        $targetChecks.returnedWidth = ConvertTo-Double (Get-AttrAny $rTarget 'width')
        $targetChecks.returnedHeight = ConvertTo-Double (Get-AttrAny $rTarget 'height')

        if ($errors.Count -eq 0) {
            $bPos = $null; $ePos = $null; $rPos = $null
            try {
                $bPos = Get-ElementPosition $bTarget 'baseline'
                $ePos = Get-ElementPosition $eTarget 'endpoint'
                $rPos = Get-ElementPosition $rTarget 'returned'
            } catch {
                Add-OracleError ('target transform rejected: ' + $_.Exception.Message)
            }
            if ($null -ne $bPos -and $null -ne $ePos -and $null -ne $rPos) {
                $eDoc = @(($ePos[0] - $bPos[0]), ($ePos[1] - $bPos[1]))
                $rDoc = @(($rPos[0] - $bPos[0]), ($rPos[1] - $bPos[1]))
                $zoom = $bState.zoom
                # Logical px = doc delta * native zoom. Physical px applies the
                # caller-supplied independent calibration factor; with the default
                # factor of 1 this is exactly the previous behaviour.
                $eLogical = @(($eDoc[0] * $zoom), ($eDoc[1] * $zoom))
                $rLogical = @(($rDoc[0] * $zoom), ($rDoc[1] * $zoom))
                $eScreen = @(($eLogical[0] * $Factor), ($eLogical[1] * $Factor))
                $rScreen = @(($rLogical[0] * $Factor), ($rLogical[1] * $Factor))
                $targetChecks.physicalPixelsPerLogicalPixel = $Factor
                $targetChecks.baselinePosition = @($bPos[0], $bPos[1])
                $targetChecks.endpointPosition = @($ePos[0], $ePos[1])
                $targetChecks.returnedPosition = @($rPos[0], $rPos[1])
                $targetChecks.endpointDeltaDoc = $eDoc
                $targetChecks.endpointDeltaLogical = $eLogical
                $targetChecks.endpointDeltaScreen = $eScreen
                $targetChecks.returnedDeltaDoc = $rDoc
                $targetChecks.returnedDeltaLogical = $rLogical
                $targetChecks.returnedDeltaScreen = $rScreen

                if ([Math]::Abs($eScreen[0]) -le $ToleranceScreenPx -and [Math]::Abs($eScreen[1]) -le $ToleranceScreenPx) {
                    Add-OracleError 'endpoint target is unchanged; a pan/no-op is not an object drag'
                }
                if ([Math]::Abs($eScreen[0] - $ExpectedScreenDelta) -gt $ToleranceScreenPx) {
                    Add-OracleError ('endpoint target x travel is ' + [Math]::Round($eScreen[0], 4) + ' physical screen px, expected ' +
                                     $ExpectedScreenDelta + ' +/- ' + $ToleranceScreenPx)
                }
                if ([Math]::Abs($eScreen[1]) -gt $ToleranceScreenPx) {
                    Add-OracleError ('endpoint target y travel is ' + [Math]::Round($eScreen[1], 4) + ' physical screen px, expected 0 +/- ' + $ToleranceScreenPx)
                }
                if ([Math]::Abs($rScreen[0]) -gt $ToleranceScreenPx -or [Math]::Abs($rScreen[1]) -gt $ToleranceScreenPx) {
                    Add-OracleError ('returned target is ' + [Math]::Round($rScreen[0], 4) + ',' + [Math]::Round($rScreen[1], 4) +
                                     ' physical screen px from baseline, tolerance ' + $ToleranceScreenPx)
                }
            }
        }
    }
}

$result.target = $targetChecks
$result.checks = [ordered]@{
    rootUnitsPxPerUnitOne = ($null -ne $result.baselineRootUnits -and $result.baselineRootUnits.cssPxPerUnit -eq 1.0)
    namedviewZoomPositive = ($null -ne $bState -and $null -ne $bState.zoom -and $bState.zoom -gt 0)
    namedviewRotationZero = ($null -ne $bState -and [Math]::Abs($bState.rotation) -le 0.000001)
    physicalScaleValid    = ($null -ne $FactorForJson -and $Factor -gt 0.0 -and $Factor -le $MaxPhysicalPixelsPerLogicalPixel)
    threeDirectRootRects  = ($result.renderableEntryCount -eq 3)
    targetTranslatedOnly  = ($errors.Count -eq 0)
    allChecksPassed       = ($errors.Count -eq 0)
}

$result.errors = @($errors)
$result.warnings = @($warnings)
$result.valid = ($errors.Count -eq 0)
$result.status = if ($result.valid) { 'valid' } else { 'invalid' }

if (-not [string]::IsNullOrWhiteSpace($OutputJson)) {
    try {
        [IO.File]::WriteAllText($OutputJson, ($result | ConvertTo-Json -Depth 12), [System.Text.UTF8Encoding]::new($false))
    } catch {
        Write-Warning ('Could not write oracle JSON: ' + $_.Exception.Message)
    }
}

$result | ConvertTo-Json -Depth 12 | Write-Output
if ($result.valid) { exit 0 } else { exit 2 }
