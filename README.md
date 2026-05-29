<h1 align="center" id="title">📻 FH6 Universal Radio</h1>

<p align="center">
  <a href="https://discord.gg/NyZUcATqWZ"><img src="https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=flat-square&logo=discord&logoColor=white" /></a>
</p>

<p align="center"><img src="assets/banner.png" alt="FH6 Universal Radio" /></p>

An open-source radio mod for **Forza Horizon 6**. Adds a new in-game radio station fed from local music, **YouTube Music**, and experimental **Apple Music** helper playback, controlled from a browser dashboard and helper player.

<p align="center">
  <img src="assets/ingame.png" alt="In-game radio station" width="49%" />
  <img src="assets/webui.png" alt="Web dashboard" width="49%" />
</p>

## Features

- **Local files**: point it at any folder. MP3 / FLAC / WAV / OGG play out of the box; M4A / AAC / OPUS / WMA / etc. play if `ffmpeg` is installed (same binary as YouTube Music below).
- **YouTube Music**: paste any video, playlist, or YT Music URL from the dashboard.
- **In-game radio integration**: audio is routed through FH6's radio bus, fades with menus and reacts to in-game volume like every other station.
- **Live dashboard** at `http://localhost:8420`: switch source, transport controls, volume, settings.
- **Race start action**: on race begin, advance to next track, restart the current one, or leave it alone.
- **Quick station skip**: tune the radio knob away and back within 1s to skip the current track.
- **Loudness normalization**: For consistent volume across tracks.
- **5-band equalizer**: 60 Hz / 250 Hz / 1 kHz / 4 kHz / 12 kHz peaking biquads, ±6 dB per band, applied producer-side at 48 kHz before audio hits the game.
- **Apple Music helper: experimental MusicKit/WebView2 player that captures Apple Music playback and streams 48 kHz stereo PCM into the in-game FMOD radio path through a named pipe.**

## Install

1. Download the latest `fh6-universal-radio.zip` from [Nexus Mods](https://www.nexusmods.com/forzahorizon6/mods/215).
2. Close FH6.
3. Extract the ZIP into your Forza Horizon 6 install folder (next to `forzahorizon6.exe`). Overwrite when prompted.
4. Launch the game. In **Audio settings**, set **Radio DJ = Off** and **Streamer Mode = On**.
5. Cycle through radio stations until you land on the new one.
6. Open <http://localhost:8420> in any browser on the same machine or LAN.

### YouTube Music

YouTube playback requires three external tools on disk:

- [`yt-dlp`](https://github.com/yt-dlp/yt-dlp/releases) and [`ffmpeg`](https://www.gyan.dev/ffmpeg/builds/) either on your `PATH`, or pointed at explicitly in the dashboard under **Settings > YouTube Music**.
- [`deno`](https://deno.com/) on `PATH`. Install with `winget install DenoLand.Deno` (or `irm https://deno.land/install.ps1 | iex`).

Private/age-restricted content also needs a Netscape `cookies.txt` exported from your browser.

### Apple Music Helper

Apple Music support is experimental and uses a separate helper application instead of directly decoding Apple Music streams.

Apple Music / MusicKit does not expose a raw audio stream that the mod can decode directly. To route Apple Music into FH6's FMOD radio path, this branch uses a helper process that hosts MusicKit inside WebView2, captures the helper process audio, and streams raw PCM into the mod through a named pipe.

```text
Apple Music Helper WebView2 player
    -> Windows process-loopback capture
    -> named pipe PCM stream
    -> version.dll AppleMusicSource
    -> FH6 / FMOD radio bus
```

#### Requirements

Apple Music helper playback requires:

- An active Apple Music subscription.
- Apple Developer Program access.
- A MusicKit developer token.
- Microsoft Edge WebView2 Runtime.
- Microsoft WebView2 SDK package for source builds.
- Windows 10 build 20348 or newer, or Windows 11, for process-specific loopback capture.
- A virtual audio cable if you do not want to hear helper playback directly.

Do not commit your `.p8` private key, generated developer token / JWT, `Keys.txt`, or local `config.toml`.

#### Apple Music Configuration

Add or verify the following section in your runtime config:

```toml
[apple_music]
enabled = true
developer_token = ""
storefront = "ca"
playback_mode = "helper"
```

The developer token can also be pasted into the Apple Music Helper UI. The helper stores the token locally in WebView2 local storage so it does not need to be entered every time.

#### WebView2 SDK Setup

The repository does not commit the WebView2 NuGet package. Install it locally before building:

```powershell
Invoke-WebRequest `
  -Uri "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" `
  -OutFile ".\nuget.exe"

.\nuget.exe install Microsoft.Web.WebView2 -OutputDirectory .\external
```

If CMake cannot find WebView2, configure `WEBVIEW2_ROOT` manually:

```powershell
cmake -S . -B build -DWEBVIEW2_ROOT="C:\path\to\Microsoft.Web.WebView2.x.y.z"
```

Example folder layout:

```text
external/
└── Microsoft.Web.WebView2.1.0.xxxxx.xx/
    └── build/
        └── native/
            ├── include/
            └── x64/
```

#### Running the Apple Music Helper

Until the helper has an embedded static HTTP server, start a local HTTP server for the helper UI:

```powershell
cd tools\apple_helper\web
py -m http.server 8421 --bind 127.0.0.1
```

Then start the helper:

```powershell
.\build\Release\fh6_apple_helper.exe
```

In the helper:

1. Paste your MusicKit developer token.
2. Click **Configure**.
3. Click **Authorize**.
4. Search Apple Music or load your library playlists.
5. Play a song.
6. Click **Start FH6 Stream**.
7. In the dashboard, switch the active source to **Apple Music** if it is not already active.

#### Avoiding Duplicate Helper Audio

The helper must render Apple Music audio so Windows can capture it. If the helper outputs to your normal headphones or speakers, you will hear both the helper playback and the in-game radio playback.

Recommended setup:

1. Install a virtual audio cable.
2. Open **Windows Settings > System > Sound > Volume mixer**.
3. Route `fh6_apple_helper.exe` / `msedgewebview2.exe` output to the virtual cable input.
4. Keep FH6 output on your normal headphones or speakers.
5. Do not monitor the virtual cable output.

With this setup, the helper renders Apple Music to the virtual device, the helper captures that process audio, and FH6 plays the captured PCM through the in-game radio.

#### Recommended External Capture Setting

`external_capture` was used during early testing and is not required for the Apple Music helper path.

Recommended config:

```toml
[external_capture]
enabled = false
device = "default"
gain = 1.0
```

#### Troubleshooting Apple Music

| Symptom | Fix |
|---|---|
| Helper can search but only plays previews | Click **Authorize** and confirm the Apple ID has an active Apple Music subscription. |
| Authorization fails | Make sure the helper page is loaded from `http://127.0.0.1:8421/`, not `file://`. |
| Start FH6 Stream says the mod must start first | Start FH6 first and make sure `[apple_music] enabled = true` in the runtime config. |
| Audio plays in helper but not in-game | Switch the active source to `apple_music`, click **Start FH6 Stream**, then check `fh6-radio\bridge.log` for `PCM helper connected`. |
| Audio plays both in helper and in-game | Route the helper / WebView2 output to a virtual audio cable and leave FH6 on your normal output device. |
| Build cannot find WebView2 | Install `Microsoft.Web.WebView2` into `external/` or set `WEBVIEW2_ROOT`. |
| Helper opens but MusicKit does not authorize | Confirm the local HTTP server is running on `127.0.0.1:8421`. |

## Uninstall

- Delete `version.dll` from the game directory.
- Delete the `fh6-radio` folder.
- Verify game files through Steam / Xbox / Microsoft Store to restore the patched assets.

## Build from source

Requires **Visual Studio 2022+** with the *Desktop development with C++* workload (CMake is bundled) and the **Forza Horizon 6** radio-station media overlay from any existing radio mod ZIP. The overlay is mod-agnostic and the assets are modified copies of game files, so we don't ship them.

```powershell
.\scripts\get-deps.ps1                                                  # one-time: header-only deps
.\scripts\build.ps1                                                     # compile + stage dist\
.\scripts\fetch-media.ps1 -Source "C:\path\to\radio-mod.zip"            # radio-station overlay
.\scripts\install.ps1 -GameDir "C:\XboxGames\Forza Horizon 6\Content"   # copy into game
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Dashboard says **bridge offline** | Media overlay not installed. Re-run `install.ps1` with `dist\media\` present. |
| New radio station doesn't show in-game | **Audio > Streamer Mode** is off. Turn it on, restart the game. |
| Game crashes on launch | Antivirus quarantined `version.dll`. Add an exclusion for the game folder. |
| Local files don't play | No `music_dir` set, or the folder only has unsupported formats. Set one from the dashboard. |
| `[local] failed to open ... .m4a` (or `.opus`, `.aac`, ...) | The built-in decoder handles MP3/FLAC/WAV/OGG only; other formats are routed through `ffmpeg`. Install it (`winget install Gyan.FFmpeg`) and either put it on `PATH` or set the path under **Settings > YouTube Music > ffmpeg_path**. |
| YouTube Music produces no audio | Check `%TEMP%\fh6-stderr.log` (helper-process stderr lands there). Usually missing yt-dlp/ffmpeg, expired cookies, or geo/format restrictions. |

## Why this exists

[Big John](https://www.nexusmods.com/forzahorizon6/mods/95) released a great **Spotify** radio mod for FH6 that I drew a lot of inspiration from. The catch: it requires Spotify Premium, and the author chose to keep it closed-source. I built FH6 Universal Radio because I believe the project can go much further once the community is allowed to contribute: adding sources (TIDAL, internet radio, etc.), polishing the UI, fixing edge cases, supporting more game builds. So this one is **fully open and GPLv3-licensed** to make that possible.

## Support the Project

FH6 Universal Radio is a community-driven project, and your support helps it grow! 🚀

- ❤️ **Donate** via [GitHub Sponsors](https://github.com/sponsors/g0ldyy) or [Ko-fi](https://ko-fi.com/g0ldyy) to support development
- ⭐ **Star the repository** here on GitHub
- 🐛 **Contribute** by reporting issues, suggesting features, or submitting PRs

## License

Released under the [GNU General Public License v3.0](LICENSE). You're free to use, modify, and redistribute the code; forks and derivatives must remain GPLv3 and credit the original project.

## Disclaimer

Unofficial fan-made mod. Not affiliated with, endorsed by, or connected to Turn 10 Studios, Playground Games, Xbox Game Studios, Microsoft, Google, or YouTube. All trademarks belong to their respective owners. Use at your own risk.
