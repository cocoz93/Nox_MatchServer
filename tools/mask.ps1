# mask.ps1 - paint pixels that differ between two frames bright red over a dimmed base.
param(
  [Parameter(Mandatory=$true)][string]$A,
  [Parameter(Mandatory=$true)][string]$B,
  [Parameter(Mandatory=$true)][string]$Out,
  [int]$Thresh=24
)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
public class MK {
  public static void Run(string pa,string pb,string po,int th){
    Bitmap ba=(Bitmap)Bitmap.FromFile(pa); Bitmap bb=(Bitmap)Bitmap.FromFile(pb);
    int w=ba.Width,h=ba.Height;
    Rectangle r=new Rectangle(0,0,w,h);
    BitmapData da=ba.LockBits(r,ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
    BitmapData db=bb.LockBits(r,ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
    int st=da.Stride; byte[] A=new byte[st*h]; byte[] B=new byte[st*h];
    Marshal.Copy(da.Scan0,A,0,A.Length); Marshal.Copy(db.Scan0,B,0,B.Length);
    ba.UnlockBits(da); bb.UnlockBits(db);
    Bitmap o=new Bitmap(w,h,PixelFormat.Format32bppArgb);
    BitmapData dd=o.LockBits(r,ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);
    byte[] O=new byte[dd.Stride*h];
    for(int y=0;y<h;y++){int row=y*st;
      for(int x=0;x<w;x++){int i=row+x*4;
        int d0=A[i]-B[i]; if(d0<0)d0=-d0;
        int d1=A[i+1]-B[i+1]; if(d1<0)d1=-d1;
        int d2=A[i+2]-B[i+2]; if(d2<0)d2=-d2;
        if(d0+d1+d2>th){O[i]=0;O[i+1]=0;O[i+2]=255;}
        else {O[i]=(byte)(B[i]/4);O[i+1]=(byte)(B[i+1]/4);O[i+2]=(byte)(B[i+2]/4);}
        O[i+3]=255;}}
    Marshal.Copy(O,0,dd.Scan0,O.Length); o.UnlockBits(dd);
    o.Save(po,ImageFormat.Png);
    o.Dispose(); ba.Dispose(); bb.Dispose();
  }
}
"@ -ReferencedAssemblies System.Drawing
[MK]::Run((Resolve-Path $A),(Resolve-Path $B),$Out,$Thresh)
Write-Output ("OK " + $Out)
