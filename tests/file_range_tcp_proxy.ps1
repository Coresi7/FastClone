# Simple TCP proxy for the file-range lane-kill test (T-largefile-block-multinic, V-03).
# Listens on 127.0.0.1:$ListenPort and forwards to 127.0.0.1:$TargetPort. The
# server->client direction is throttled to $DownlinkBytesPerSec so the proxied lane stays
# mid-transfer long enough for the test to kill this process (killing the process breaks
# exactly ONE client lane, which exercises block-level reroute idempotency).
#
# The forwarding core is C# (Add-Type): PowerShell scriptblocks cannot run on raw .NET
# threads (no runspace), so a pure-PS proxy silently resets connections.
#
# Usage: powershell -ExecutionPolicy Bypass -File file_range_tcp_proxy.ps1 -ListenPort 28201 -TargetPort 28200 -DownlinkBytesPerSec 12582912

param(
    [int]$ListenPort,
    [int]$TargetPort,
    [long]$DownlinkBytesPerSec = 0
)

$ErrorActionPreference = "Stop"

$proxyCs = @'
using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;

public static class TcpProxy {
    private static void Copy(NetworkStream src, NetworkStream dst, long limitBytesPerSec) {
        try {
            byte[] buf = new byte[65536];
            Stopwatch sw = Stopwatch.StartNew();
            long total = 0;
            int n;
            while ((n = src.Read(buf, 0, buf.Length)) > 0) {
                dst.Write(buf, 0, n);
                dst.Flush();
                if (limitBytesPerSec > 0) {
                    total += n;
                    double expectedMs = (total * 1000.0) / limitBytesPerSec;
                    double delay = expectedMs - sw.Elapsed.TotalMilliseconds;
                    if (delay > 0) {
                        Thread.Sleep((int)delay);
                    }
                }
            }
        } catch {
            // Peer reset / stream closed: fall through and close the other direction.
        }
        try { dst.Close(); } catch {}
        try { src.Close(); } catch {}
    }

    public static void Run(int listenPort, int targetPort, long downlinkBytesPerSec) {
        TcpListener listener = new TcpListener(IPAddress.Parse("127.0.0.1"), listenPort);
        listener.Start();
        Console.Out.WriteLine("proxy listening 127.0.0.1:" + listenPort + " -> 127.0.0.1:" +
                              targetPort + " (downlink=" + downlinkBytesPerSec + " B/s)");
        Console.Out.WriteLine("PROXY_READY");
        Console.Out.Flush();
        while (true) {
            TcpClient client = listener.AcceptTcpClient();
            try {
                TcpClient target = new TcpClient();
                target.NoDelay = true;
                target.Connect("127.0.0.1", targetPort);
                NetworkStream cs = client.GetStream();
                NetworkStream ts = target.GetStream();
                // client -> server: unthrottled control traffic
                Thread tUp = new Thread(() => Copy(cs, ts, 0));
                // server -> client: throttled (bulk download direction)
                Thread tDown = new Thread(() => Copy(ts, cs, downlinkBytesPerSec));
                tUp.Start();
                tDown.Start();
                tUp.Join();
                tDown.Join();
                try { client.Close(); } catch {}
                try { target.Close(); } catch {}
            } catch (Exception ex) {
                Console.Out.WriteLine("proxy connection error: " + ex.Message);
                Console.Out.Flush();
                try { client.Close(); } catch {}
            }
        }
    }
}
'@

Add-Type -TypeDefinition $proxyCs -ReferencedAssemblies @("System", "System.Core")
[TcpProxy]::Run($ListenPort, $TargetPort, $DownlinkBytesPerSec)
