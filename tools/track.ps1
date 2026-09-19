# track.ps1 - find orange arrow pixels in a region and report their centroid per frame.
param(
  [Parameter(Mandatory=$true)][string]$Dir,
  [int]$X0=0,[int]$Y0=0,[int]$X1=1919,[int]$Y1=1079,
  [int]$From=1,[int]$To=999,
  [switch]$Probe, [int]$PX=0, [int]$PY=0
)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
public class TR {
  public static byte[] Load(string p, out int w,out int h,out int st){
    using(Bitmap bm=(Bitmap)Bitmap.FromFile(p)){
      w=bm.Width;h=bm.Height;
      BitmapData d=bm.LockBits(new Rectangle(0,0,w,h),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
      st=d.Stride;byte[] b=new byte[st*h];Marshal.Copy(d.Scan0,b,0,b.Length);bm.UnlockBits(d);return b;}}
  // BGRA order
  public static double[] Cent(byte[] b,int st,int x0,int y0,int x1,int y1){
    long n=0;double sx=0,sy=0;
    for(int y=y0;y<=y1;y++){int row=y*st;
      for(int x=x0;x<=x1;x++){int i=row+x*4;
        int bl=b[i],g=b[i+1],r=b[i+2];
        if(r>165 && r-g>45 && g-bl>25){n++;sx+=x;sy+=y;}}}
    if(n==0) return new double[]{0,-1,-1};
    return new double[]{n,sx/n,sy/n};}
  public static string Px(byte[] b,int st,int x,int y){int i=y*st+x*4;
    return string.Format("R{0} G{1} B{2}",b[i+2],b[i+1],b[i]);}
}
"@ -ReferencedAssemblies System.Drawing
$files = Get-ChildItem $Dir -Filter 'f_*.png' | Sort-Object Name
$w=0;$h=0;$st=0
foreach($f in $files){
  $num = [int]($f.BaseName -replace '\D','')
  if($num -lt $From -or $num -gt $To){continue}
  $b = [TR]::Load($f.FullName,[ref]$w,[ref]$h,[ref]$st)
  if($Probe){ "{0} ({1},{2}) {3}" -f $f.Name,$PX,$PY,[TR]::Px($b,$st,$PX,$PY); continue }
  $c = [TR]::Cent($b,$st,$X0,$Y0,$X1,$Y1)
  "{0} n={1} cx={2:F1} cy={3:F1}" -f $f.Name,$c[0],$c[1],$c[2]
}
