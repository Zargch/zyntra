# Genera assets\zyntra-icon.png (512 px) y assets\zyntra.ico (16..256 px) a partir de assets\zyntra-logo.png.
# Los tamanos pequenos (<= 48 px) usan solo la "Z" para que se lea en la barra de tareas.
$root = Split-Path -Parent $PSScriptRoot
$cs = @'
using System; using System.IO; using System.Windows; using System.Windows.Media; using System.Windows.Media.Imaging;
public static class ZyntraIco {
  static BitmapSource Load(string p) { var d = BitmapDecoder.Create(new Uri(p), BitmapCreateOptions.None, BitmapCacheOption.OnLoad); return new FormatConvertedBitmap(d.Frames[0], PixelFormats.Pbgra32, null, 0); }
  static byte[] Render(BitmapSource b, int px, Int32Rect src, bool mark) {
    var dv = new DrawingVisual();
    using (var dc = dv.RenderOpen()) {
      dc.PushClip(new RectangleGeometry(new Rect(0, 0, px, px), px * 0.22, px * 0.22));
      dc.DrawRectangle(Brushes.Black, null, new Rect(0, 0, px, px));
      double pad = mark ? px * 0.06 : 0;
      dc.DrawImage(new CroppedBitmap(b, src), new Rect(pad, pad, px - 2 * pad, px - 2 * pad));
      dc.Pop();
      if (mark) {
        var pen = new Pen(new SolidColorBrush(Color.FromRgb(255, 110, 20)), Math.Max(1, px / 40.0));
        dc.DrawRoundedRectangle(null, pen, new Rect(pen.Thickness / 2, pen.Thickness / 2, px - pen.Thickness, px - pen.Thickness), px * 0.21, px * 0.21);
      }
    }
    RenderOptions.SetBitmapScalingMode(dv, BitmapScalingMode.HighQuality);
    var rt = new RenderTargetBitmap(px, px, 96, 96, PixelFormats.Pbgra32); rt.Render(dv);
    var enc = new PngBitmapEncoder(); enc.Frames.Add(BitmapFrame.Create(rt));
    var ms = new MemoryStream(); enc.Save(ms); return ms.ToArray();
  }
  public static void Make(string logo, string png, string ico) {
    var b = Load(logo);
    var full = new Int32Rect(30, 30, 962, 962);   // logo completo sin el margen negro exterior
    var mark = new Int32Rect(167, 30, 690, 690);  // solo la Z
    File.WriteAllBytes(png, Render(b, 512, full, false));
    int[] sizes = { 16, 24, 32, 48, 64, 128, 256 };
    var imgs = new byte[sizes.Length][];
    for (int i = 0; i < sizes.Length; i++) imgs[i] = Render(b, sizes[i], sizes[i] <= 48 ? mark : full, sizes[i] <= 48);
    using (var f = new BinaryWriter(File.Create(ico))) {
      f.Write((short)0); f.Write((short)1); f.Write((short)sizes.Length);
      int off = 6 + 16 * sizes.Length;
      for (int i = 0; i < sizes.Length; i++) {
        int s = sizes[i];
        f.Write((byte)(s >= 256 ? 0 : s)); f.Write((byte)(s >= 256 ? 0 : s)); f.Write((byte)0); f.Write((byte)0);
        f.Write((short)1); f.Write((short)32); f.Write(imgs[i].Length); f.Write(off); off += imgs[i].Length;
      }
      foreach (var im in imgs) f.Write(im);
    }
    File.WriteAllBytes(png.Replace(".png", "-mark.png"), Render(b, 256, mark, true));
  }
}
'@
Add-Type -TypeDefinition $cs -ReferencedAssemblies PresentationCore, WindowsBase, System.Xaml
[ZyntraIco]::Make("$root\assets\zyntra-logo.png", "$root\assets\zyntra-icon.png", "$root\assets\zyntra.ico")
