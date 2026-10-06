#!/bin/sh
# Cross-compile Headroom.exe (x64, Windows 7+) from Linux/macOS with clang + lld-link.
# No Windows SDK or C runtime needed: the import libraries are generated below.
#   ./build.sh            -> ../Headroom.exe
# lld-link comes with LLVM ("lld" package) or with Rust (rust-lld).
set -eu
cd "$(dirname "$0")"
CLANG=${CLANG:-clang}
LINK=${LINK:-$(command -v lld-link || true)}
if [ -z "$LINK" ]; then
	RUST_LLD=$(ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1 || true)
	[ -n "$RUST_LLD" ] || { echo "need lld-link (install lld) or rust-lld (install Rust)"; exit 1; }
	LINK="$RUST_LLD -flavor link"
fi
OUT=${1:-../Headroom.exe}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

implib() { # implib <dll> <functions...>: import library from a .def list
	dll=$1; shift
	{ echo "LIBRARY $dll"; echo "EXPORTS"; for f in "$@"; do echo "  $f"; done; } > "$TMP/$dll.def"
	$LINK /lib /machine:x64 /def:"$TMP/$dll.def" /out:"$TMP/${dll%.dll}.lib" > /dev/null
}
implib kernel32.dll GetModuleHandleW GetModuleFileNameW GetCommandLineW ExitProcess CreateMutexW \
	GetLastError SetErrorMode GetLogicalDrives GetLogicalDriveStringsW GetDriveTypeW GetDiskFreeSpaceExW \
	GetVolumeInformationW GetCurrentProcess SetPriorityClass GetProcAddress Beep QueueUserWorkItem \
	GetTickCount64 CreateFileW ReadFile WriteFile CloseHandle MoveFileExW CreateDirectoryW \
	GetFileAttributesW GetFileAttributesExW GetEnvironmentVariableW GetProcessHeap HeapAlloc HeapFree CreateProcessW
implib user32.dll RegisterClassExW CreateWindowExW DefWindowProcW ShowWindow GetMessageW TranslateMessage \
	DispatchMessageW PostMessageW PostQuitMessage SetTimer KillTimer SetWindowPos GetWindowRect GetCursorPos \
	SetCapture ReleaseCapture UpdateLayeredWindow CreatePopupMenu AppendMenuW TrackPopupMenu DestroyMenu \
	SetForegroundWindow GetForegroundWindow GetDesktopWindow GetShellWindow FindWindowW FindWindowExW \
	GetClassNameW GetWindowLongW IsWindowVisible MonitorFromRect MonitorFromWindow MonitorFromPoint \
	GetMonitorInfoW SetProcessDPIAware SetWinEventHook RegisterWindowMessageW LoadCursorW DrawTextW GetDC \
	ReleaseDC MessageBoxW IsWindow
implib gdi32.dll CreateCompatibleDC CreateDIBSection SelectObject DeleteObject CreateFontW SetTextColor \
	SetBkMode GetTextExtentPoint32W GdiFlush GetDeviceCaps
implib advapi32.dll RegOpenKeyExW RegCreateKeyExW RegSetValueExW RegQueryValueExW RegDeleteValueW RegCloseKey

# -fno-builtin/-ffreestanding: no C runtime.  Frames over 4 KB would need __chkstk
# (not provided), so the link fails loudly instead of risking a stack overflow.
$CLANG --target=x86_64-pc-windows-msvc -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
	-Wall -Wextra -Wframe-larger-than=4000 -c headroom.c -o "$TMP/headroom.obj"
$LINK /nologo /subsystem:windows,6.01 /entry:WinMainCRTStartup /nodefaultlib /opt:ref /opt:icf \
	/dynamicbase /nxcompat /highentropyva /release /out:"$OUT" "$TMP/headroom.obj" \
	"$TMP/kernel32.lib" "$TMP/user32.lib" "$TMP/gdi32.lib" "$TMP/advapi32.lib"
ls -l "$OUT"
