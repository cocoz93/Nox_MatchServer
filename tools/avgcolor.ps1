# avgcolor.ps1 - average RGB of a rectangle in a PNG.
#
# Why: telling one ability effect from another by eye costs a picture each time.
# A mean colour over the character box turns "did it turn red / go translucent"
# into a number, so five abilities can be compared in one line.
#
#   .\tools\avgcolor.ps1 -In <png> -X 930 -Y 475 -W 55 -H 70
param(
  [Parameter(Mandatory=$true)][string]$In,
  [int]$X=0, [int]$Y=0, [int]$W=50, [int]$H=50
)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
public class AC {
  public static double[] Mean(string p,int x0,int y0,int w,int h){
    using(Bitmap bm=(Bitmap)Bitmap.FromFile(p)){
      BitmapData d=bm.LockBits(new Rectangle(0,0,bm.Width,bm.Height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
      int st=d.Stride; byte[] b=new byte[st*bm.Height];
      Marshal.Copy(d.Scan0,b,0,b.Length); bm.UnlockBits(d);
      double r=0,g=0,bl=0; long n=0, red=0, skin=0;
      for(int y=y0;y<y0+h && y<bm.Height;y++){int row=y*st;
        for(int x=x0;x<x0+w && x<bm.Width;x++){int i=row+x*4;
          int B=b[i],G=b[i+1],R=b[i+2];
          bl+=B; g+=G; r+=R; n++;
          // 붉은 옷 — 빨강이 뚜렷이 앞서고 어둡지 않은 것
          if(R>90 && R-G>35 && R-B>35) red++;
          // 살색 — 빨강>초록>파랑 이면서 셋 다 밝은 것
          if(R>120 && G>80 && B>55 && R>G && G>B && R-B<110) skin++;}}
      if(n==0) return new double[]{0,0,0,0,0,0};
      return new double[]{r/n,g/n,bl/n,n,red,skin};}}
}
"@ -ReferencedAssemblies System.Drawing
$m = [AC]::Mean((Resolve-Path $In), $X, $Y, $W, $H)
"{0} R{1:F1} G{2:F1} B{3:F1} · 붉은 {4} · 살색 {5} (픽셀 {6})" -f (Split-Path $In -Leaf), $m[0], $m[1], $m[2], $m[4], $m[5], $m[3]
