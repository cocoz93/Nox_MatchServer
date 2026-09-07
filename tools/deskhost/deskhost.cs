// 별도 Windows 데스크톱에 게임을 띄우고, 그 데스크톱 안에서만 입력을 넣고 화면을 찍는 호스트.
// 데스크톱마다 입력 큐 · 포그라운드 창 · 커서가 따로라서 사용자 화면을 건드리지 않는다.
//
// 쓰는 법 : deskhost.exe <데스크톱이름> <제어폴더>
// 명령    : <제어폴더>\cmd.txt 에 한 줄씩 적으면 읽고 지운다. 결과는 log.txt 에 쌓인다.
// 빌드    : csc /target:exe /out:deskhost.exe /r:System.Drawing.dll deskhost.cs

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

class DeskHost
{
    const uint GENERIC_ALL = 0x10000000;

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateDesktop(string name, IntPtr dev, IntPtr devmode, int flags, uint access, IntPtr sa);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr OpenDesktop(string name, int flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool SetThreadDesktop(IntPtr hDesk);
    [DllImport("user32.dll", SetLastError = true)] static extern bool CloseDesktop(IntPtr hDesktop);
    [DllImport("user32.dll")] static extern IntPtr GetThreadDesktop(int tid);
    [DllImport("kernel32.dll")] static extern int GetCurrentThreadId();

    delegate bool EnumProc(IntPtr hWnd, IntPtr lp);
    [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr hDesk, EnumProc fn, IntPtr lp);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetWindowDC(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr hdc);
    [DllImport("user32.dll")] static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(int idAttach, int idAttachTo, bool fAttach);
    [DllImport("user32.dll")] static extern IntPtr SetActiveWindow(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetActiveWindow();
    [DllImport("user32.dll")] static extern IntPtr GetFocus();
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] static extern short GetKeyState(int vk);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] static extern bool SwitchDesktop(IntPtr hDesk);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr OpenInputDesktop(int flags, bool inherit, uint access);
    [DllImport("user32.dll")] static extern bool ClipCursor(IntPtr r);
    [DllImport("user32.dll")] static extern bool GetClipCursor(out RECT r);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, uint rop);

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO
    {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit,
        uint flags, IntPtr env, string curDir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    static string deskName, ctrlDir, logPath;
    static IntPtr hDesk = IntPtr.Zero;
    static IntPtr target = IntPtr.Zero;
    static IntPtr origDesk = IntPtr.Zero;      // 입력 데스크톱을 뺏었을 때 돌아갈 곳
    static DateTime deadline = DateTime.MinValue;

    static void Log(string s)
    {
        string line = DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s + Environment.NewLine;
        for (int i = 0; i < 8; i++)
        {
            try { File.AppendAllText(logPath, line, new UTF8Encoding(false)); return; }
            catch { Thread.Sleep(40); }
        }
    }

    static int Main(string[] a)
    {
        if (a.Length < 2) return 2;
        deskName = a[0]; ctrlDir = a[1];
        Directory.CreateDirectory(ctrlDir);
        logPath = Path.Combine(ctrlDir, "log.txt");
        Thread t = new Thread(Worker, 8 * 1024 * 1024);
        t.IsBackground = false;
        t.Start();
        t.Join();
        return 0;
    }

    static void Worker()
    {
        // ★ 이 스레드가 창을 하나라도 만들기 전에 데스크톱을 갈아타야 한다.
        hDesk = CreateDesktop(deskName, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
        if (hDesk == IntPtr.Zero)
        {
            int e = Marshal.GetLastWin32Error();
            hDesk = OpenDesktop(deskName, 0, true, GENERIC_ALL);
            Log("CreateDesktop 실패 err=" + e + " → OpenDesktop=" + hDesk);
        }
        else Log("CreateDesktop ok  " + deskName + "  h=" + hDesk);
        if (hDesk == IntPtr.Zero) { Log("ABORT: 데스크톱 없음"); return; }

        bool ok = SetThreadDesktop(hDesk);
        Log("SetThreadDesktop=" + ok + " err=" + Marshal.GetLastWin32Error() + "  tid=" + GetCurrentThreadId());
        Log("READY  ctrl=" + ctrlDir);

        string cmdPath = Path.Combine(ctrlDir, "cmd.txt");
        while (true)
        {
            try
            {
                if (File.Exists(cmdPath))
                {
                    string body = null;
                    try { body = File.ReadAllText(cmdPath); File.Delete(cmdPath); }
                    catch { Thread.Sleep(60); continue; }
                    foreach (string raw in body.Replace("\r", "").Split('\n'))
                    {
                        string line = raw.Trim();
                        if (line.Length == 0 || line.StartsWith("#")) continue;
                        Log("> " + line);
                        try { if (Exec(line)) { Log("BYE"); return; } }
                        catch (Exception ex) { Log("  !! " + ex.Message); }
                    }
                }
            }
            catch (Exception ex) { Log("loop !! " + ex.Message); }
            Thread.Sleep(120);
        }
    }

    static bool Exec(string line)
    {
        string[] p = line.Split(new char[] { ' ' }, 2);
        string c = p[0].ToLowerInvariant();
        string rest = p.Length > 1 ? p[1].Trim() : "";
        switch (c)
        {
            case "quit": return true;
            case "sleep": Thread.Sleep(int.Parse(rest)); return false;

            case "spawn":
                {
                    // spawn <exe경로>|<작업폴더>
                    string[] q = rest.Split('|');
                    string exe = q[0].Trim();
                    string wd = q.Length > 1 ? q[1].Trim() : Path.GetDirectoryName(exe);
                    STARTUPINFO si = new STARTUPINFO();
                    si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
                    si.lpDesktop = deskName;            // ★ 여기가 핵심
                    PROCESS_INFORMATION pi;
                    bool r = CreateProcess(exe, new StringBuilder("\"" + exe + "\""), IntPtr.Zero, IntPtr.Zero,
                        false, 0, IntPtr.Zero, wd, ref si, out pi);
                    Log("  spawn=" + r + " err=" + Marshal.GetLastWin32Error() + " pid=" + pi.dwProcessId);
                    return false;
                }

            case "list":
                {
                    List<string> lines = new List<string>();
                    EnumDesktopWindows(hDesk, delegate(IntPtr h, IntPtr lp)
                    {
                        int pid; GetWindowThreadProcessId(h, out pid);
                        StringBuilder sb = new StringBuilder(256);
                        GetWindowTextW(h, sb, 256);
                        RECT r; GetWindowRect(h, out r);
                        lines.Add(string.Format("    hwnd={0} pid={1} vis={2} rect={3},{4},{5},{6} \"{7}\"",
                            h, pid, IsWindowVisible(h), r.Left, r.Top, r.Right, r.Bottom, sb.ToString()));
                        return true;
                    }, IntPtr.Zero);
                    Log("  창 " + lines.Count + "개");
                    foreach (string s in lines) Log(s);
                    return false;
                }

            case "target":
                {
                    int want = int.Parse(rest);
                    IntPtr found = IntPtr.Zero; int best = 0;
                    EnumDesktopWindows(hDesk, delegate(IntPtr h, IntPtr lp)
                    {
                        int pid; GetWindowThreadProcessId(h, out pid);
                        if (pid != want) return true;
                        RECT r; GetWindowRect(h, out r);
                        int area = (r.Right - r.Left) * (r.Bottom - r.Top);
                        if (area > best) { best = area; found = h; }
                        return true;
                    }, IntPtr.Zero);
                    target = found;
                    Log("  target=" + target + " area=" + best);
                    return false;
                }

            case "hwnd": target = new IntPtr(long.Parse(rest)); Log("  target=" + target); return false;

            case "shot":     return Shot(rest, 0);
            case "shotfull": return Shot(rest, 2);
            case "shotdc":   return ShotDC(rest);
            case "shotdesk": return ShotDesk(rest);

            case "move":
                { string[] q = rest.Split(' '); bool ok = SetCursorPos(int.Parse(q[0]), int.Parse(q[1])); Log("  SetCursorPos=" + ok + " err=" + Marshal.GetLastWin32Error()); return Cursor(); }
            case "rel":
                { string[] q = rest.Split(' '); mouse_event(0x0001, int.Parse(q[0]), int.Parse(q[1]), 0, IntPtr.Zero); return Cursor(); }
            case "cursor": return Cursor();

            case "click": mouse_event(0x0002, 0, 0, 0, IntPtr.Zero); Thread.Sleep(80); mouse_event(0x0004, 0, 0, 0, IntPtr.Zero); return false;
            case "rclick": mouse_event(0x0008, 0, 0, 0, IntPtr.Zero); Thread.Sleep(80); mouse_event(0x0010, 0, 0, 0, IntPtr.Zero); return false;
            case "ldown": mouse_event(0x0002, 0, 0, 0, IntPtr.Zero); return false;
            case "lup": mouse_event(0x0004, 0, 0, 0, IntPtr.Zero); return false;
            case "rdown": mouse_event(0x0008, 0, 0, 0, IntPtr.Zero); return false;
            case "rup": mouse_event(0x0010, 0, 0, 0, IntPtr.Zero); return false;

            case "key":
                { byte vk = (byte)Convert.ToInt32(rest, 16); keybd_event(vk, 0, 0, IntPtr.Zero); Thread.Sleep(60); keybd_event(vk, 0, 2, IntPtr.Zero); return false; }

            case "fg":
                {
                    IntPtr f = GetForegroundWindow(); int pid; GetWindowThreadProcessId(f, out pid);
                    StringBuilder sb = new StringBuilder(256); GetWindowTextW(f, sb, 256);
                    Log("  foreground hwnd=" + f + " pid=" + pid + " \"" + sb + "\"");
                    return false;
                }

            case "front":
                { ShowWindow(target, 9); BringWindowToTop(target); bool r = SetForegroundWindow(target); Log("  front=" + r); return false; }

            case "movewin":
                { string[] q = rest.Split(' '); SetWindowPos(target, IntPtr.Zero, int.Parse(q[0]), int.Parse(q[1]), 0, 0, 0x0001 | 0x0004); return false; }

            case "focus":
                {
                    // 숨은 데스크톱에는 포그라운드 창이 없다. 게임 스레드에 입력큐를 붙여 강제로 준다.
                    int pid; int gtid = GetWindowThreadProcessId(target, out pid);
                    int mytid = GetCurrentThreadId();
                    bool at = AttachThreadInput(mytid, gtid, true);
                    ShowWindow(target, 9);
                    BringWindowToTop(target);
                    bool sf = SetForegroundWindow(target);
                    IntPtr sa = SetActiveWindow(target);
                    IntPtr sfo = SetFocus(target);
                    Log("  attach=" + at + " setFg=" + sf + " setActive=" + sa + " setFocus=" + sfo);
                    Log("  → fg=" + GetForegroundWindow() + " active=" + GetActiveWindow() + " focus=" + GetFocus());
                    AttachThreadInput(mytid, gtid, false);
                    return false;
                }
            case "async":
                {
                    int vk = Convert.ToInt32(rest, 16);
                    Log("  GetAsyncKeyState(0x" + rest + ")=" + GetAsyncKeyState(vk) + " GetKeyState=" + GetKeyState(vk));
                    return false;
                }
            case "clip":
                { RECT r; bool ok = GetClipCursor(out r); Log("  GetClipCursor=" + ok + " " + r.Left + "," + r.Top + "," + r.Right + "," + r.Bottom); return false; }
            case "unclip":
                { bool ok = ClipCursor(IntPtr.Zero); Log("  ClipCursor(null)=" + ok); return false; }
            case "post":
                {
                    // post <msgHex> <wpHex> <lpHex>  — 창 메시지 경로가 살아 있는지 가르는 탐침
                    string[] q = rest.Split(' ');
                    bool ok = PostMessage(target, (uint)Convert.ToInt32(q[0], 16), new IntPtr(Convert.ToInt64(q[1], 16)), new IntPtr(Convert.ToInt64(q[2], 16)));
                    Log("  PostMessage=" + ok);
                    return false;
                }
            case "switchto":
                { bool ok = SwitchDesktop(hDesk); Log("  SwitchDesktop(" + deskName + ")=" + ok + " err=" + Marshal.GetLastWin32Error()); return false; }
            case "probe":
                {
                    // 숨은 데스크톱을 잠깐 "입력 데스크톱"으로 올려 입력이 먹는지 본다.
                    // ★ 무슨 일이 나도 되돌아온다 — finally 로 한 번, 감시 스레드로 또 한 번.
                    const uint DESKTOP_SWITCHDESKTOP = 0x0100;
                    IntPtr orig = OpenInputDesktop(0, false, DESKTOP_SWITCHDESKTOP);
                    Log("  원래 입력 데스크톱 h=" + orig + " err=" + Marshal.GetLastWin32Error());
                    if (orig == IntPtr.Zero) { Log("  ABORT: 돌아올 데를 못 잡아서 안 바꾼다"); return false; }
                    IntPtr back = orig;
                    Thread wd = new Thread(delegate() {
                        Thread.Sleep(20000);
                        SwitchDesktop(back);   // 20초 뒤엔 무조건 복귀
                    });
                    wd.IsBackground = true; wd.Start();
                    string dir = rest.Length > 0 ? rest : ctrlDir;
                    try
                    {
                        bool sw = SwitchDesktop(hDesk);
                        Log("  SwitchDesktop(숨은쪽)=" + sw + " err=" + Marshal.GetLastWin32Error());
                        Thread.Sleep(700);

                        int pid; int gtid = GetWindowThreadProcessId(target, out pid);
                        int mytid = GetCurrentThreadId();
                        AttachThreadInput(mytid, gtid, true);
                        ShowWindow(target, 9); BringWindowToTop(target);
                        bool sf = SetForegroundWindow(target);
                        AttachThreadInput(mytid, gtid, false);
                        Log("  setFg=" + sf + " fg=" + GetForegroundWindow() + " (target=" + target + ")");
                        Thread.Sleep(400);

                        RECT r; GetWindowRect(target, out r);
                        bool ok = SetCursorPos(r.Left + 200, r.Top + 200);
                        POINT cp; GetCursorPos(out cp);
                        Log("  SetCursorPos=" + ok + " → cursor=" + cp.X + "," + cp.Y + " (창 원점 " + r.Left + "," + r.Top + ")");

                        // 게임 커서를 좌상단으로 몰았다가 Multiplay 로 (기존 실측 계수 2.058/2.054)
                        for (int i = 0; i < 5; i++) { mouse_event(0x0001, -3000, -3000, 0, IntPtr.Zero); Thread.Sleep(80); }
                        GetCursorPos(out cp); Log("  home 뒤 cursor=" + cp.X + "," + cp.Y);
                        mouse_event(0x0001, 272, 163, 0, IntPtr.Zero);
                        Thread.Sleep(400);
                        GetCursorPos(out cp); Log("  rel(272,163) 뒤 cursor=" + cp.X + "," + cp.Y);
                        Log("  GetAsyncKeyState 시험 — ldown 중 0x01 = " + Test01());
                        Shot(Path.Combine(dir, "p_cursor.png"), 2);

                        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero); Thread.Sleep(90);
                        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
                        Thread.Sleep(1500);
                        Shot(Path.Combine(dir, "p_click.png"), 2);
                    }
                    catch (Exception ex) { Log("  probe !! " + ex.Message); }
                    finally
                    {
                        bool b = SwitchDesktop(orig);
                        Log("  복귀 SwitchDesktop=" + b + " err=" + Marshal.GetLastWin32Error());
                    }
                    return false;
                }
            case "switchin":
                {
                    // 숨은 데스크톱을 입력 데스크톱으로 올린다. <초> 뒤에는 감시 스레드가 무조건 되돌린다.
                    if (origDesk != IntPtr.Zero) { deadline = DateTime.Now.AddSeconds(double.Parse(rest)); Log("  이미 올라가 있음 → 시한만 연장"); return false; }
                    const uint DESKTOP_SWITCHDESKTOP = 0x0100;
                    IntPtr o = OpenInputDesktop(0, false, DESKTOP_SWITCHDESKTOP);
                    if (o == IntPtr.Zero) { Log("  ABORT: 돌아올 데를 못 잡아서 안 바꾼다 err=" + Marshal.GetLastWin32Error()); return false; }
                    double sec = rest.Length > 0 ? double.Parse(rest) : 30;
                    if (sec > 180) sec = 180;
                    origDesk = o; deadline = DateTime.Now.AddSeconds(sec);
                    bool sw = SwitchDesktop(hDesk);
                    Log("  SwitchDesktop(숨은쪽)=" + sw + "  시한 " + sec + "초");
                    Thread wd = new Thread(delegate() {
                        while (DateTime.Now < deadline) Thread.Sleep(200);
                        IntPtr back = origDesk;
                        if (back != IntPtr.Zero) { bool b = SwitchDesktop(back); CloseDesktop(back); origDesk = IntPtr.Zero; Log("  [감시] 복귀=" + b); }
                    });
                    wd.IsBackground = true; wd.Start();
                    return false;
                }
            case "switchout":
                { deadline = DateTime.MinValue; Thread.Sleep(400); Log("  switchout 요청 — 감시 스레드가 되돌림"); return false; }
            case "grab":
                {
                    // 창 안 빈 곳을 실제로 눌러 게임이 마우스를 다시 잡게 한다.
                    // 이걸 빼면 이후 입력이 전부 무시된다 (nav.ps1 에서 얻은 것).
                    RECT wr; GetWindowRect(target, out wr);
                    SetCursorPos(wr.Left + 100, wr.Bottom - 55);
                    Thread.Sleep(250);
                    mouse_event(0x0002, 0, 0, 0, IntPtr.Zero); Thread.Sleep(90);
                    mouse_event(0x0004, 0, 0, 0, IntPtr.Zero); Thread.Sleep(800);
                    Log("  grab 클릭 (" + (wr.Left + 100) + "," + (wr.Bottom - 55) + ")");
                    return false;
                }
            case "goto":
                {
                    // goto <게임 클라 x> <y>  — 1280x960 기준. 게임 커서는 OS 커서의 약 2.058 배로 간다.
                    string[] q = rest.Split(' ');
                    double gx = double.Parse(q[0]), gy = double.Parse(q[1]);
                    POINT org = new POINT(); ClientToScreen(target, ref org);
                    RECT cr; GetClientRect(target, out cr);
                    int cw = cr.Right - cr.Left, ch = cr.Bottom - cr.Top;
                    int sx = (int)Math.Round(org.X + (gx / 2.058) * cw / 1280.0);
                    int sy = (int)Math.Round(org.Y + (gy / 2.054) * ch / 960.0);
                    for (int i = 0; i < 5; i++) { mouse_event(0x0001, -3000, -3000, 0, IntPtr.Zero); Thread.Sleep(90); }
                    Thread.Sleep(250);
                    POINT cur; int k;
                    for (k = 0; k < 25; k++)
                    {
                        GetCursorPos(out cur);
                        int ex = sx - cur.X, ey = sy - cur.Y;
                        if (Math.Abs(ex) <= 2 && Math.Abs(ey) <= 2) break;
                        mouse_event(0x0001, (int)Math.Round(ex / 1.5), (int)Math.Round(ey / 1.5), 0, IntPtr.Zero);
                        Thread.Sleep(50);
                    }
                    GetCursorPos(out cur);
                    Log("  goto 게임(" + gx + "," + gy + ") → OS 목표 (" + sx + "," + sy + ") 실제 (" + cur.X + "," + cur.Y + ") 되짚기 " + k + "회");
                    return false;
                }
            default: Log("  ?? 모르는 명령"); return false;
        }
    }

    // 버튼을 누른 채 상태표를 읽는다 — 장치 입력이 이 데스크톱까지 왔는지 가르는 지문.
    static string Test01()
    {
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
        Thread.Sleep(120);
        short s = GetAsyncKeyState(0x01);
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
        return s.ToString();
    }
    static bool Cursor()
    {
        POINT p; GetCursorPos(out p);
        Log("  cursor=" + p.X + "," + p.Y);
        return false;
    }

    static bool Shot(string path, uint flags)
    {
        if (target == IntPtr.Zero) { Log("  target 없음"); return false; }
        RECT r; GetWindowRect(target, out r);
        int w = r.Right - r.Left, h = r.Bottom - r.Top;
        if (w <= 0 || h <= 0) { Log("  bad rect " + w + "x" + h); return false; }
        using (Bitmap b = new Bitmap(w, h))
        {
            using (Graphics g = Graphics.FromImage(b))
            {
                IntPtr hdc = g.GetHdc();
                bool ok = PrintWindow(target, hdc, flags);
                g.ReleaseHdc(hdc);
                Log("  PrintWindow(flags=" + flags + ")=" + ok + " " + w + "x" + h);
            }
            b.Save(path, ImageFormat.Png);
        }
        Log("  saved " + path + " (" + Stats(path) + ")");
        return false;
    }

    static bool ShotDC(string path)
    {
        if (target == IntPtr.Zero) { Log("  target 없음"); return false; }
        RECT r; GetWindowRect(target, out r);
        int w = r.Right - r.Left, h = r.Bottom - r.Top;
        IntPtr src = GetWindowDC(target);
        using (Bitmap b = new Bitmap(w, h))
        {
            using (Graphics g = Graphics.FromImage(b))
            {
                IntPtr hdc = g.GetHdc();
                bool ok = BitBlt(hdc, 0, 0, w, h, src, 0, 0, 0x00CC0020);
                g.ReleaseHdc(hdc);
                Log("  BitBlt(windowDC)=" + ok);
            }
            b.Save(path, ImageFormat.Png);
        }
        ReleaseDC(target, src);
        Log("  saved " + path + " (" + Stats(path) + ")");
        return false;
    }

    static bool ShotDesk(string path)
    {
        IntPtr dw = GetDesktopWindow();
        RECT r; GetWindowRect(dw, out r);
        int w = r.Right - r.Left, h = r.Bottom - r.Top;
        IntPtr src = GetDC(dw);
        using (Bitmap b = new Bitmap(w, h))
        {
            using (Graphics g = Graphics.FromImage(b))
            {
                IntPtr hdc = g.GetHdc();
                bool ok = BitBlt(hdc, 0, 0, w, h, src, r.Left, r.Top, 0x00CC0020);
                g.ReleaseHdc(hdc);
                Log("  desktopWindow=" + dw + " " + w + "x" + h + " BitBlt=" + ok);
            }
            b.Save(path, ImageFormat.Png);
        }
        ReleaseDC(dw, src);
        Log("  saved " + path + " (" + Stats(path) + ")");
        return false;
    }

    // 찍은 그림이 "그냥 까만 사각형"인지 가른다 — 다른 색 개수와 평균 밝기.
    static string Stats(string path)
    {
        try
        {
            using (Bitmap b = new Bitmap(path))
            {
                Dictionary<int, int> hist = new Dictionary<int, int>();
                long sum = 0; int n = 0;
                for (int y = 0; y < b.Height; y += 7)
                    for (int x = 0; x < b.Width; x += 7)
                    {
                        Color c = b.GetPixel(x, y);
                        int key = c.ToArgb();
                        if (!hist.ContainsKey(key)) hist[key] = 0;
                        hist[key]++;
                        sum += (c.R + c.G + c.B) / 3; n++;
                    }
                return "색 " + hist.Count + "종, 평균밝기 " + (n > 0 ? (sum / n) : 0);
            }
        }
        catch (Exception e) { return "stat 실패 " + e.Message; }
    }
}