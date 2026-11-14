Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public static class TestWindows {
    public delegate bool Callback(IntPtr h, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Callback call, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, Callback call, IntPtr data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder result, int length);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder result, int length);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint message, IntPtr key, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint message, IntPtr key, string data);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    public static string Text(IntPtr h) { var b=new StringBuilder(512); GetWindowText(h,b,b.Capacity); return b.ToString(); }
    public static string Class(IntPtr h) { var b=new StringBuilder(128); GetClassName(h,b,b.Capacity); return b.ToString(); }
    public static IntPtr[] ForProcess(uint target) {
        var windows = new List<IntPtr>();
        EnumWindows((h,d) => { uint p; GetWindowThreadProcessId(h,out p); if (p==target && IsWindowVisible(h)) windows.Add(h); return true; }, IntPtr.Zero);
        return windows.ToArray();
    }
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr window, uint command);
    public static string PlaybackStatus(IntPtr parent) {
        uint process;
        GetWindowThreadProcessId(parent, out process);
        foreach (IntPtr window in ForProcess(process)) {
            if (GetWindow(window, 4) != parent) continue;
            string value = Text(window);
            if (value.EndsWith(" left")) return value;
        }
        return "";
    }
    public static void Press(IntPtr h, int key) { PostMessage(h,0x100,(IntPtr)key,(IntPtr)1); PostMessage(h,0x101,(IntPtr)key,(IntPtr)1); }
    public static void Click(IntPtr h, int id) { PostMessage(GetDlgItem(h,id),0xF5,IntPtr.Zero,IntPtr.Zero); }
}
"@
