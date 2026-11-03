// Draws the controller layout pictures (controls_en.png, controls_ru.png): labels and arrows over
// the controllers' photo. Built and run by build_installer.bat.
// Usage: ControlsImage.exe <output folder> <controllers photo>
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;

static class ControlsImage
{
    const int W = 1800, H = 1060;
    static readonly Color Bg = Color.FromArgb(24, 27, 34), Text1 = Color.FromArgb(240, 242, 246), Text2 = Color.FromArgb(170, 178, 190);
    static readonly Color LeftC = Color.FromArgb(90, 170, 255), RightC = Color.FromArgb(255, 160, 70);
    // The part of the photo with both controllers (pixels of the 620x403 photo) and where it goes
    static readonly RectangleF Src = new RectangleF(170, 95, 270, 200);
    const float Scale = 3.3f, DstX = 455, DstY = 160;

    class L {
        public string Title, Desc; public PointF Target; public bool Down;   // Down: the leader goes level, then down onto it
        public L(string t, string d, PointF p, bool down = false) { Title = t; Desc = d; Target = p; Down = down; }
    }

    // a point of the photo -> the picture
    static PointF M(float x, float y) { return new PointF(DstX + (x - Src.X) * Scale, DstY + (y - Src.Y) * Scale); }

    static void Main(string[] args)
    {
        string dir = args[0];
        Directory.CreateDirectory(dir);
        using (var photo = Image.FromFile(args[1])) {
            Draw(Path.Combine(dir, "controls_en.png"), photo, false);
            Draw(Path.Combine(dir, "controls_ru.png"), photo, true);
        }
    }

    static void Draw(string file, Image photo, bool ru)
    {
        Func<string, string, string> T = (en, r) => ru ? r : en;
        using (var bmp = new Bitmap(W, H))
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.Clear(Bg);
            using (var ft = new Font("Segoe UI", 30, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var fs = new Font("Segoe UI", 19, GraphicsUnit.Pixel))
            using (var b1 = new SolidBrush(Text1)) using (var b2 = new SolidBrush(Text2)) {
                g.DrawString(T("Transformers: War for Cybertron VR — controls", "Transformers: War for Cybertron VR — управление"), ft, b1, 40, 26);
                g.DrawString(T("Shown on Meta Quest controllers; other Touch-style controllers (Rift, Index...) are the same. Default layout (GripAbilities=0)",
                               "На контроллерах Meta Quest; у других контроллеров типа Touch (Rift, Index...) так же. Раскладка по умолчанию (GripAbilities=0)"), fs, b2, 42, 70);
            }
            g.DrawImage(photo, new RectangleF(DstX, DstY, Src.Width * Scale, Src.Height * Scale), Src, GraphicsUnit.Pixel);

            var left = new[] {
                new L("Y", T("grenade", "граната"), M(252, 131), true),
                new L(T("Left stick", "Левый стик"), T("move; click: transform", "движение; нажатие: трансформация"), M(217, 125)),
                new L(T("Menu", "Меню"), T("pause; hold 1.5 s (or hold + A): VR settings", "пауза; удерживать 1.5 с (или + A): настройки VR"), M(208, 148)),
                new L("X", T("reload / interact", "перезарядка / действие"), M(233, 147)),
                new L(T("Left trigger", "Левый курок"), T("fine aim, the weapon's 2nd action (detonate); vehicle: boost", "точное прицеливание, 2-е действие оружия (подрыв); в машине — ускорение"), M(240, 200)),
                new L(T("Left grip", "Левый грип"), T("at the gun's front: two-handed hold; at the belt: grenade; aimed at an item: pull it; elsewhere: use",
                                                       "у ствола: двуручный хват; у пояса: граната; на предмет: притянуть; иначе: использовать"), M(212, 240)),
            };
            var right = new[] {
                new L("B", T("tap: main ability; hold: weapon wheel", "коротко: главная абилка; держать: колесо оружия"), M(361, 131), true),
                new L(T("Right stick", "Правый стик"), T("turn; up / down: previous / next weapon; click: second ability", "поворот; вверх / вниз: пред. / след. оружие; нажатие: вторая абилка"), M(396, 124)),
                new L("A", T("jump", "прыжок"), M(379, 147)),
                new L(T("Right trigger", "Правый курок"), T("fire (the gun shoots where the controller points)", "огонь (пушка стреляет, куда смотрит контроллер)"), M(370, 200)),
                new L(T("Right grip", "Правый грип"), T("empty hand: use, pick up, pull items; in VR settings: move the gun in the hand",
                                                        "пустой рукой: использовать, подобрать, притянуть; в настройках VR: сдвинуть пушку в руке"), M(397, 240)),
            };
            Labels(g, left, LeftC, false);
            Labels(g, right, RightC, true);

            using (var fh = new Font("Segoe UI", 22, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var fs = new Font("Segoe UI", 19, GraphicsUnit.Pixel))
            using (var b1 = new SolidBrush(Text1)) using (var b2 = new SolidBrush(Text2)) {
                float y = 905;
                g.DrawString(T("Gestures", "Жесты"), fh, b1, 40, y);
                g.DrawString(T("• a sharp punch forward with the left (or empty right) hand: melee     • a throw with the free left hand: grenade\n" +
                               "• a sniper rifle raised to the eye: scope     • left grip on a grenade at the belt, left trigger arms it, let go to throw",
                               "• резкий удар рукой вперёд (левой или пустой правой): ближний бой     • бросок свободной левой рукой: граната\n" +
                               "• снайперка, поднесённая к глазу: прицел     • левый грип у пояса — граната, левый курок — взвести, отпустить — бросок"),
                             fs, b2, new RectangleF(40, y + 34, W - 80, 80));
            }
            bmp.Save(file, ImageFormat.Png);
        }
    }

    static void Labels(Graphics g, L[] ls, Color col, bool right)
    {
        const float boxW = 380, top = 140, step = 128;
        float x = right ? W - 40 - boxW : 40;
        using (var ft = new Font("Segoe UI", 22, FontStyle.Bold, GraphicsUnit.Pixel))
        using (var fd = new Font("Segoe UI", 18, GraphicsUnit.Pixel))
        using (var bt = new SolidBrush(col)) using (var bd = new SolidBrush(Text1))
        using (var dark = new Pen(Color.FromArgb(160, 0, 0, 0), 7))
        using (var pen = new Pen(col, 3)) {
            pen.CustomEndCap = new AdjustableArrowCap(5, 6);
            dark.StartCap = dark.EndCap = LineCap.Round;
            var fmt = new StringFormat { Alignment = right ? StringAlignment.Near : StringAlignment.Far };
            for (int i = 0; i < ls.Length; i++) {
                float y = top + i * step;
                g.DrawString(ls[i].Title, ft, bt, new RectangleF(x, y, boxW, 30), fmt);
                g.DrawString(ls[i].Desc, fd, bd, new RectangleF(x, y + 30, boxW, 70), fmt);
                var from = new PointF(right ? x - 14 : x + boxW + 14, y + 16);
                var t = ls[i].Target;
                var bend = ls[i].Down ? new PointF(t.X, from.Y) : new PointF(from.X + (right ? -40 : 40), from.Y);
                // the arrow stops short of the button, so it stays visible
                float dx = t.X - bend.X, dy = t.Y - bend.Y, len = (float)Math.Sqrt(dx * dx + dy * dy);
                var end = new PointF(t.X - dx / len * 26, t.Y - dy / len * 26);
                // a dark outline under the line keeps it readable over the white handle
                g.DrawLines(dark, new[] { from, bend, end });
                using (var thin = new Pen(col, 3)) g.DrawLine(thin, from, bend);
                g.DrawLine(pen, bend, end);
                using (var b = new SolidBrush(col)) g.FillEllipse(b, from.X - 5, from.Y - 5, 10, 10);
            }
        }
    }
}
