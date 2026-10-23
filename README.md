# Clipski

A fast, minimal, single-file C++17 desktop video trimmer for Windows built with **SDL2 (OpenGL)**, **libmpv**, and **Dear ImGui**. Designed for effortless game clipping directly from the Windows right-click menu.

---

## Features

- **Right-Click Context Menu Integration:** Right-click any video file (`.mp4`, `.mkv`, `.mov`, `.webm`, `.avi`) in Windows Explorer and select **Trim with Clipski** (or drag and drop files directly into the window).
- **Hardware-Accelerated Playback:** Powered by `libmpv` (`hwdec=auto`) rendering directly to an OpenGL 3.2 Core context with zero idle CPU usage (`SDL_WaitEvent` + `mpv_render_context_set_update_callback`).
- **Live Clip Loop Preview (`L`):** Toggle **Loop Clip** to lock playback and the timeline scrubber strictly between your `Start` and `End` trim points using `libmpv`'s hardware A-B loop.
- **One-Click Discord & Lossless Exports:**
  - **Save Discord 1080p:** Exports `<name>_compressed_1080.mp4` (`60fps`, `libx264`, `-crf 23`, `-preset slower`).
  - **Save Discord 720p:** Exports `<name>_compressed_720.mp4` (`60fps`, `libx264`, `-crf 32`, `-preset slower`).
  - **Save Raw Copy:** Exports `<name>_trimmed.mp4` (`-c copy`, instant lossless stream copy with zero re-encoding).
  - Runs `ffmpeg.exe` asynchronously in the background with no console popups or UI freezing.

---

## Hotkeys

| Key | Action |
| :--- | :--- |
| **`Space`** | Play / Pause |
| **`S`** (or **`I`** / **`[`**) | Set **Trim Start** to current timestamp |
| **`E`** (or **`O`** / **`]`**) | Set **Trim End** to current timestamp |
| **`L`** (or **`C`**) | Toggle **Loop Clip** mode (play only inside your trimmed range) |
| **`Left` / `Right` Arrow** | Seek backward / forward **1 second** |
| **`Shift + Left / Right`** | Seek backward / forward **5 seconds** |
| **`,` / `.`** (`<` / `>`) | Step backward / forward **1 exact frame** |

---

## Building on Windows

### Prerequisites
- **Visual Studio 2022** (with C++ Desktop workload) and **CMake 3.20+**
- **FFmpeg** (`ffmpeg.exe` available in your system `PATH`)
- **libmpv Windows Dev SDK** extracted into `external/mpv` (`include/mpv/*.h`, `libmpv-2.dll`, and MSVC import library `mpv.lib`).
  - *Note: `SDL2` and `Dear ImGui` are automatically downloaded and configured by CMake `FetchContent`.*

### Compile
```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
The compiled binary and required runtime DLLs (`SDL2.dll`, `libmpv-2.dll`) are output to `build\Release\clipski.exe`.

---

## Registering the Windows Right-Click Menu

Run the following in PowerShell (no Administrator privileges required) to add **Trim with Clipski** to your video file right-click context menu:

```powershell
$exePath  = "$PSScriptRoot\build\Release\clipski.exe"
$menuIcon = "$PSScriptRoot\clipski_menu.ico"
$exts     = @(".mp4", ".mkv", ".mov", ".webm", ".avi")

foreach ($ext in $exts) {
    $regKey = "HKCU:\Software\Classes\SystemFileAssociations\$ext\Shell\Trim with Clipski"
    $cmdKey = "$regKey\command"
    New-Item -Path $cmdKey -Force | Out-Null
    Set-ItemProperty -Path $regKey -Name "(Default)" -Value "Trim with Clipski"
    Set-ItemProperty -Path $regKey -Name "Icon" -Value $menuIcon
    Set-ItemProperty -Path $cmdKey -Name "(Default)" -Value "`"$exePath`" `"%1`""
}
```

---

## Customizing FFmpeg Presets

Open [`main.cpp`](main.cpp) and edit the command templates at the top of the file:
```cpp
static const std::string FFMPEG_ARGS_DISCORD_1080P =
    "-y -ss {start} -to {end} -i \"{input}\" -vf \"scale=-2:1080,fps=60\" -crf 23 -preset slower -c:v libx264 -c:a aac -b:a 128k \"{output}\"";

static const std::string FFMPEG_ARGS_DISCORD_720P =
    "-y -ss {start} -to {end} -i \"{input}\" -vf \"scale=-2:720,fps=60\" -crf 32 -preset slower -c:v libx264 -c:a aac -b:a 128k \"{output}\"";

static const std::string FFMPEG_ARGS_LOSSLESS_COPY =
    "-y -ss {start} -to {end} -i \"{input}\" -c copy \"{output}\"";
```
Then run `cmake --build build --config Release` to update `clipski.exe`.
