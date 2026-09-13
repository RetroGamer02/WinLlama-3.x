# WinLlama 3.x

WinLlama is a Win16 client for an Ollama server.  It is intended to run on
plain Windows 3.0 with a Winsock 1.1-compatible TCP/IP stack, and to remain
usable on Windows 9x and 32-bit Windows XP.

## Build

Install OpenWatcom C/C++ and initialize its environment.  On a DOS/Windows
OpenWatcom installation, run `owsetenv.bat` in the prompt that will build the
program.

The Linux OpenWatcom package used by this workspace has a setup script that
refers to a missing `lh` directory.  Set the actual header and Win16 library
paths explicitly before building:

```sh
export WATCOM=/usr/bin/watcom
export PATH="$WATCOM/binl:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/win${INCLUDE:+:$INCLUDE}"
export LIB="$WATCOM/lib286:$WATCOM/lib286/win${LIB:+:$LIB}"
```

Then run:

```text
wmake
```

That creates `WINLLAMA.EXE`, a 16-bit Windows NE executable.  The build must
also have access to a 16-bit Winsock 1.1 import library, commonly supplied by
the Winsock SDK or the target TCP/IP stack.  The default build links against
`winsock.lib`.  A complete OpenWatcom installation is required: for this
large-memory-model build, `$WATCOM/lib286/win/clibl.lib` must exist alongside
`windows.lib` and `winsock.lib`.  Some minimal Linux package builds omit the
16-bit C runtime libraries and cannot produce a Win16 executable.

## Run

1. Start Ollama on a modern host reachable over your LAN.
2. Ensure that host accepts connections on port 11434.
3. Start WinLlama, enter the host's IPv4 address and port, then choose **Test
   Connection**.

For Windows 3.0, install and configure a compatible Winsock provider first,
such as Trumpet Winsock.  Use plain HTTP only on a trusted local network; this
generation of client is not designed to implement modern HTTPS/TLS.
