param([Parameter(Mandatory=$true)][string]$Dir, [int]$Thresh = 24)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class FD {
  public static byte[] W; public static byte[] H;
  public static byte[] Load(string p, out int w, out int h, out int st) {
    using (Bitmap bm = (Bitmap)Bitmap.FromFile(p)) {
      w = bm.Width; h = bm.Height;
      BitmapData d = bm.LockBits(new Rectangle(0,0,w,h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      st = d.Stride;
      byte[] buf = new byte[st*h];
      Marshal.Copy(d.Scan0, buf, 0, buf.Length);
      bm.UnlockBits(d);
      return buf;
    }
  }
  // returns: count, minx, miny, maxx, maxy
  public static int[] Diff(byte[] a, byte[] b, int w, int h, int st, int thresh) {
    int n=0, minx=int.MaxValue, miny=int.MaxValue, maxx=-1, maxy=-1;
    for (int y=0; y<h; y++) {
      int row = y*st;
      for (int x=0; x<w; x++) {
        int i = row + x*4;
        int d0 = a[i]-b[i]; if (d0<0) d0=-d0;
        int d1 = a[i+1]-b[i+1]; if (d1<0) d1=-d1;
        int d2 = a[i+2]-b[i+2]; if (d2<0) d2=-d2;
        if (d0+d1+d2 > thresh) {
          n++;
          if (x<minx) minx=x; if (x>maxx) maxx=x;
          if (y<miny) miny=y; if (y>maxy) maxy=y;
        }
      }
    }
    return new int[]{n,minx,miny,maxx,maxy};
  }
}
"@ -ReferencedAssemblies System.Drawing
$files = Get-ChildItem $Dir -Filter 'f_*.png' | Sort-Object Name
$w=0;$h=0;$st=0
$prev = $null
foreach ($f in $files) {
  $cur = [FD]::Load($f.FullName, [ref]$w, [ref]$h, [ref]$st)
  if ($prev -ne $null) {
    $r = [FD]::Diff($prev, $cur, $w, $h, $st, $Thresh)
    "{0} {1} {2},{3}-{4},{5}" -f $f.Name, $r[0], $r[1], $r[2], $r[3], $r[4]
  }
  $prev = $cur
}
"SIZE {0}x{1}" -f $w,$h
