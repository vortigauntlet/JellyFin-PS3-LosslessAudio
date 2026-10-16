// ffwrap -- stands in for Jellyfin's ffmpeg.exe and caps how fast ffmpeg pulls
// an INTERNET source (debrid / Gelato streams).
//
// Why: with the server PC wired to the router, a transcode reads its remote
// input as fast as the internet allows (~110 Mbps here, ffmpeg at 4x).  That
// saturates the router, and while it is saturated the router stops passing
// LAN traffic between the PC and the PS3 -- pings to the console were lost or
// took 0.3-1.3 s, and the PS3 stream fell to ~0.5 Mbps (measured 2026-10-13).
// Every PS3 stream that ran clean that night was one the server served with no
// ffmpeg pulling.
//
// What it does: for an http(s) input it inserts
//     -readrate 1.75 -readrate_initial_burst 4
// before that -i, so ffmpeg reads at 1.75x the media's own rate after a 4 s
// head start.  ffwrap.ini next to this exe can set another rate
// ("readrate=1.5"; 1.1 .. 4) and head start ("burst=6"; 1 .. 60 s).
//
// Why 1.75 (2026-10-14; it was 1.25): the server can only stay that far ahead
// of playback, so at 1.25 a 21 Mbps episode arrived at ~26 Mbps and the PS3
// could never build a buffer -- every bitrate peak or network blip was a
// visible hitch.  The PS3 takes ~30 Mbps, so 1.75 lets it fill its buffer on
// typical episodes, while a 40 Mbps remux still pulls only ~70 Mbps, clear of
// the ~110 Mbps that choked the router.
//
// Why a 4 s head start, not 20 (2026-10-16): the burst is read at full internet
// speed, and 20 s of a 24 Mbps remux is ~60 MB, ~5 s of ~110 Mbps.  Pings to the
// console while a stream started: 150 ms average, up to 890 ms, with losses, for
// ~20 s -- the PS3 saw its stream collapse a few seconds in, then the app's
// reopen made it worse.  With 4 s: no losses, worst 340 ms once, ~10 ms after.
// (2 s and a 1.5 rate measured the same way; the PS3 still gets >= 44 Mbps from
// the start.)
//
// Everything else is passed through untouched: the command line is forwarded
// verbatim (no re-quoting), the std handles are inherited, the
// exit code is returned, and ffmpeg runs in a job object so it dies with this
// process when Jellyfin kills it.
//
// The real ffmpeg: FFWRAP_REAL if set, else "ffmpeg=" in ffwrap.ini (the
// installer writes the path Jellyfin was using), else the one next to Jellyfin,
// C:\Program Files\Jellyfin\Server\ffmpeg.exe.  ffwrap.log next to this exe
// records each rewrite.

using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

static class FfWrap
{
    const double DefaultRate = 1.75;
    const int    DefaultBurst = 4;

    // One "key=value" from ffwrap.ini next to this exe, or null.
    static string IniValue(string key)
    {
        try
        {
            string ini = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ffwrap.ini");
            if (!File.Exists(ini)) return null;
            foreach (string line in File.ReadAllLines(ini))
            {
                string l = line.Trim();
                if (l.StartsWith(key + "=", StringComparison.OrdinalIgnoreCase))
                    return l.Substring(key.Length + 1).Trim();
            }
        }
        catch { }
        return null;
    }

    static string Inject()
    {
        double rate = DefaultRate, v;
        int burst = DefaultBurst, b;
        string s = IniValue("readrate");
        if (s != null && double.TryParse(s, System.Globalization.NumberStyles.Float,
                                         System.Globalization.CultureInfo.InvariantCulture, out v)
            && v >= 1.1 && v <= 4.0)
            rate = v;
        string sb = IniValue("burst");
        if (sb != null && int.TryParse(sb, out b) && b >= 1 && b <= 60) burst = b;
        return "-readrate " + rate.ToString("0.0#", System.Globalization.CultureInfo.InvariantCulture)
             + " -readrate_initial_burst " + burst + " ";
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObject(IntPtr a, string name);
    [DllImport("kernel32.dll")]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtLimit info, int len);
    [DllImport("kernel32.dll")]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);

    [StructLayout(LayoutKind.Sequential)]
    struct BasicLimit { public long a, b; public uint flags; public UIntPtr c, d; public uint e; public UIntPtr f; public uint g, h; }
    [StructLayout(LayoutKind.Sequential)]
    struct IoCounters { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)]
    struct ExtLimit { public BasicLimit basic; public IoCounters io; public UIntPtr p, j, pp, jp; }

    static string ArgsOf(string cmd)
    {
        // Strip argv[0] exactly as Windows wrote it, keep the rest byte for byte.
        int i = 0;
        if (cmd.Length > 0 && cmd[0] == '"') { i = cmd.IndexOf('"', 1); i = i < 0 ? cmd.Length : i + 1; }
        else { while (i < cmd.Length && cmd[i] != ' ' && cmd[i] != '\t') i++; }
        return cmd.Substring(i).TrimStart();
    }

    static string Rewrite(string args)
    {
        if (args.Contains("-readrate") || args.Contains(" -re ")) return args;
        foreach (var pat in new[] { "-i \"http", "-i http" })
        {
            int k = args.IndexOf(pat, StringComparison.OrdinalIgnoreCase);
            if (k >= 0 && (k == 0 || args[k - 1] == ' ')) return args.Substring(0, k) + Inject() + args.Substring(k);
        }
        return args;
    }

    static int Main()
    {
        string real = Environment.GetEnvironmentVariable("FFWRAP_REAL");
        if (string.IsNullOrEmpty(real)) real = IniValue("ffmpeg");
        if (string.IsNullOrEmpty(real)) real = @"C:\Program Files\Jellyfin\Server\ffmpeg.exe";
        string args = ArgsOf(Environment.CommandLine);
        string outArgs = Rewrite(args);
        if (!ReferenceEquals(outArgs, args))
        {
            try
            {
                string log = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ffwrap.log");
                File.AppendAllText(log, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " injected: " + Inject() + "\r\n");
            }
            catch { }
        }

        var psi = new ProcessStartInfo(real, outArgs) { UseShellExecute = false };
        Process p;
        try { p = Process.Start(psi); }
        catch (Exception e) { Console.Error.WriteLine("ffwrap: cannot start " + real + ": " + e.Message); return 1; }

        try
        {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            var info = new ExtLimit();
            info.basic.flags = 0x2000;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(ExtLimit)));
            AssignProcessToJobObject(job, p.Handle);
            // The handle is deliberately never closed: it closes when this process
            // ends, and then the job kills ffmpeg with it.
        }
        catch { }

        p.WaitForExit();
        return p.ExitCode;
    }
}
