public static int main (string[] args) {
    // Ignore SIGPIPE so broken PipeWire/GStreamer IPC sockets never force-quit the app
    Posix.signal (Posix.Signal.PIPE, Posix.SIG_IGN);

    Gst.init (ref args);
    Adw.init ();

    var app = new RecGo.Application ();
    return app.run (args);
}
