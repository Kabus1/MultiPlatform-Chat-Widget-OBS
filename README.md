# Unified Stream Chat (C++)

A lightweight C++ app that merges **YouTube**, **Twitch** and **Kick** live chat into one feed. It works two ways at the same time (**hybrid mode**):

- **Desktop app:** a native window (WebView2 on Windows) with the full chat UI and settings.
- **Web / OBS:** a built-in local web server, so the same chat can be added to OBS as a **Custom Browser Dock** or **Browser Source**, or opened in any browser.

It also translates messages with Google Translate and reads them aloud with Google text-to-speech, on the speaker you choose.

```
                      ┌──────────── UnifiedStreamChat.exe ─────────────┐
 Native window ◄──────┤ WebView2 window  (same UI, http://localhost:8787/?app=1)
 OBS dock / source ◄──┤ local HTTP + SSE server  (127.0.0.1:8787)       │
                      │                                                 │
 Twitch  (IRC/WS) ────┤                 ┌─► Bot filter (drop bots)      │
 YouTube (Data API) ──┼─► clients ──────┤─► Google Translate            │
 Kick    (Pusher WS) ─┤                 └─► TTS ─► AudioPlayer ─► chosen speaker
                      └─────────────────────────────────────────────────┘
```

## Features

- **Hybrid mode.** Double-click `UnifiedStreamChat.exe` and it opens its own window. The OBS/browser links keep working while it runs. Use `--no-window` to run only the server. Closing the window quits the app.
- **One feed, three platforms.** Every message shows the platform logo and color code (a colored stripe and the logo in that color) next to the username. You can change the colors. Twitch and Kick emotes and role badges are shown.
- **Logins:**
  - **Twitch:** OAuth2 login.
  - **YouTube:** Google OAuth2 login. The app finds your active live stream on its own.
  - **Kick:** just type your username.
- **Google Translate.** English messages stay as written, with the translated Arabic line right below them. The target language can be changed.
  - **Arabic is never translated.** Arabic in any dialect or casual spelling ("هاي", "شلونك", Iraqi/Gulf "شگد", "چا") is shown and read exactly as written, with the Arabic voice.
    - The app recognizes Arabic from the letters themselves and never sends it to Google. Google's detection often labels short dialect words as Persian or Urdu, which used to cause unwanted "translations".
    - Real Persian/Urdu spelling (ی ک پ ژ ے) is still translated.
  - **English slang and abbreviations** (idk, brb, gg, wp, ngl, tbh, ty, lol, W/L, …, about 150 built in, plus your own `abbr = meaning` lines) are expanded before translating. For example, "idk bro, gg wp" becomes "I don't know, brother, good game, well played".
    - The Arabic line under the message is then a real translation, not the abbreviation echoed back or a failed detection.
    - The voice uses the same text: the expanded English in *original* mode, or the same Arabic line in *translated* mode.
    - Short slang is translated as English even when Google's detection guesses another language.
  - If Google's free endpoint rate-limits (HTTP 429), translation pauses for 60 s instead of retrying non-stop. A Google Cloud API key avoids the limit.
- **Google TTS** with separate toggles for:
  - reading the username
  - reading the message text
  - reading the original text or the translation
  - skipping messages longer than a character limit you set
  - choosing which platforms are read aloud
  - volume, speed and queue size
- **Clean speech.** TTS never reads:
  - **emotes:** Twitch and Kick native emotes; **7TV, BTTV and FFZ** emotes (global and your channel's, which are also shown as images in chat); Kick `[emote:…]` tokens; YouTube `:shortcodes:`; a built-in list of common emote words; emote-code style words such as `catJAM` or `xqcL`; and your own word list
  - **emoji, symbols and ASCII/braille art**, including in usernames
  - **messages with links** (`https://`, `www.`, and bare domains such as `discord.gg/…`). These are skipped entirely.
  - **bots** (Nightbot, BotRix, …), even when the bot filter's chat display is off
  - `!commands`

  A message that is only emotes or emoji is not read at all, not even the username.
- **Audio output device.** Settings → Voice → Audio output lists every output device on your system (speakers, headsets, virtual cables). Pick one, and optionally the **left or right channel only**.
  - The choice is saved in `settings.json` as `audio.deviceId` (the system's stable device ID, e.g. the WASAPI endpoint ID) plus `audio.deviceName`.
  - Each time the app starts, it restores and opens that device right away.
  - It matches by ID first, then by name, and updates the file if either one changed.
  - If the device is unplugged, the system default is used until it comes back.
- **Bot filter.** Messages from chat bots (Nightbot, BotRix, StreamElements, Streamlabs, Moobot, Fossabot and more) are dropped completely: never shown, translated or read aloud. Detection uses:
  - a built-in list you can edit
  - your own extra list
  - platform bot badges
  - an optional rule for names ending in "bot"

  An allow-list protects real users the rules would otherwise catch, and viewers' `!commands` can optionally be hidden too.
- **Links.** Settings → Links (or the 🔗 button) shows ready-to-copy URLs for the OBS dock, the overlay, and the overlay with voice, each with **Copy** and **Open** buttons.
- **Settings sidebar** with tabs: Connections · Chat · Translation · Voice (TTS) · Links · Advanced. Every change is saved immediately.

## Building

### Windows: Visual Studio 2022 (no setup)
1. Install **Visual Studio 2022** with the **"Desktop development with C++"** workload. That workload includes CMake, Ninja and Git.
2. **File → Open → Folder…** and pick this repository.
3. Choose the **Windows x64 Release** configuration and pick `UnifiedStreamChat.exe` as the startup item.
4. **Build → Build All**, then run it.

No OpenSSL, no vcpkg and no paths to configure. The WebView2 SDK headers are downloaded from NuGet automatically, and the webview library's built-in loader means no `WebView2Loader.dll` needs to ship. On Windows, HTTPS and WebSockets use **WinHTTP**, which is built into Windows: TLS comes from SChannel, certificates from the Windows certificate store, and the proxy from your system settings. The dependencies are header-only (nlohmann/json, cpp-httplib) and CMake downloads them automatically. The C++ runtime is linked statically, so `UnifiedStreamChat.exe` is a single file that runs on any Windows 10 or 11 PC. It only uses DLLs that ship with Windows.

From a *Developer Command Prompt* the same build is:
```bat
cmake --preset x64-release
cmake --build --preset x64-release
```

**Optional: vcpkg.** The root `vcpkg.json` is a manifest with a pinned `builtin-baseline`. If you prefer vcpkg packages, use the **Windows x64 Release (vcpkg manifest)** preset, which uses `%VCPKG_ROOT%` (VS 2022's Developer environment sets it). CMake then uses vcpkg's `nlohmann-json` and falls back to FetchContent for anything that's missing. On Windows, vcpkg only installs that one header-only package.

### Linux / macOS
Requirements: CMake ≥ 3.21, a C++17 compiler and OpenSSL 3. IXWebSocket, miniaudio and webview are downloaded automatically. On Linux the desktop window needs WebKitGTK (`libwebkit2gtk-4.1-dev`); without it the build is server/OBS-only.

```bash
# Linux:   sudo apt install build-essential cmake git libssl-dev libwebkit2gtk-4.1-dev
# macOS:   brew install cmake openssl@3   (add -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) if needed)
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

Every push to GitHub runs `.github/workflows/build.yml`, which builds downloadable Windows, macOS and Linux binaries.

## Running

1. Start `UnifiedStreamChat.exe`. The chat window opens.
   - The window uses the **Microsoft Edge WebView2 Runtime**, which Windows 10 and 11 already include.
   - If the runtime is missing, the app opens the chat in your default browser instead and keeps running.
2. Click ⚙ and connect your platforms (see below).
3. **OBS (optional).** Open ⚙ → **Links**, copy a link, and add it in OBS:
   - **Docks → Custom Browser Docks…**: `http://localhost:8787/`
   - **Browser Source** (transparent chat on stream): `http://localhost:8787/overlay`
   - **Browser Source with voice** (OBS captures the voice): `http://localhost:8787/overlay?tts=1`. In this case, set *Voice → Audio output* to the page option or mute the app, so the voice isn't played twice.

Where the voice plays:
- **App (default):** the program plays TTS itself on the device you chose. It works whether the window is open or you only use OBS, and it never plays twice. To get the voice into your stream, choose a device OBS captures (e.g. a virtual audio cable), or use the overlay-with-voice link.
- **Page:** the dock or window page plays the audio instead.

Command-line options:
- `--no-window`: run only the server
- `--port <n>`
- `--config <dir>`
- `--web <dir>`: serve the UI from disk while you work on it
- `--open`

On Windows the app has no console window. Logs go to `app.log` in the settings folder, or to the terminal if you start it from one. If you start the app while it's already running, it opens another window onto the running copy.

Settings and tokens are stored in:
- Windows: `%APPDATA%\UnifiedStreamChat\`
- macOS: `~/Library/Application Support/UnifiedStreamChat/`
- Linux: `~/.config/unified-stream-chat/`

`tokens.json` holds your OAuth tokens. Keep it private.

## Creating API credentials

Login uses your own developer app, so no third-party server is involved. Each one takes about 2 minutes.

### Twitch
1. Open <https://dev.twitch.tv/console/apps> and click **Register Your Application**.
2. **OAuth Redirect URL:** `http://localhost:8787/auth/twitch/callback`. The **Copy** button in the dock gives you the exact URL.
3. Category: *Chat Bot*. Client type: *Confidential*.
4. Copy the **Client ID**, create a **Client Secret**, and paste both into *Settings → Connections → Twitch*.
5. Click **Login with Twitch**. Your normal browser opens. After you approve, the dock shows *connected*.

### YouTube
1. In <https://console.cloud.google.com/>, create a project and enable **YouTube Data API v3**.
2. Set up the **OAuth consent screen** (External; add yourself as a test user).
3. Go to **Credentials → Create credentials → OAuth client ID → Desktop app**. Google accepts loopback redirects such as `http://localhost:8787/auth/youtube/callback`. If you pick the *Web application* type instead, add that exact URL as an authorized redirect URI.
4. Paste the Client ID and Secret into *Settings → Connections → YouTube* and click **Login with YouTube**.

Quota note: each chat poll costs API quota units, and the default quota is 10,000 units per day. The app respects YouTube's `pollingIntervalMillis` and never polls faster than the **Minimum poll interval** setting (6 s by default). If you stream for long sessions, raise the interval or ask Google for more quota.

### Kick
Enter your channel name and turn it on. If Kick's Cloudflare protection blocks the channel lookup, the status shows an error. In that case, open `https://kick.com/api/v2/channels/<name>` in your browser, copy `chatroom.id`, and paste it into *Kick → Troubleshooting → Chatroom ID*.

### Google Translate / TTS
Nothing is needed by default: the app uses Google's public Translate and Translate-TTS endpoints. For higher reliability and better voices (WaveNet/Neural2), create a Google Cloud API key with **Cloud Translation API** and **Cloud Text-to-Speech API** enabled, then paste it into *Translation → Google Cloud*. You can also set a voice name such as `ar-XA-Wavenet-B`.

## How TTS decides what to say

For every live message from a real user (the backlog loaded when the app connects is never read aloud), the app:

1. Skips the message if TTS is off, the platform is excluded, the user is on the ignored list, or the message is a `!command`.
2. Skips the message entirely if it contains a link. Otherwise, removes emotes, emoji and symbols (see *Clean speech*). If nothing readable is left, the message is skipped.
3. Skips the message **entirely** if the remaining text is longer than *Skip messages longer than* characters.
4. Picks the text to read: the **original** in its detected language, or the **translated** text in the target language.
5. Builds the speech from the username phrase and/or the message text, based on your toggles.

The backend fetches the MP3 from Google. By default it plays the speech itself (miniaudio) on the chosen output device and channel. With *Audio output → page*, or with the `?tts=1` link, the page plays it through `/api/tts` instead.

Bot messages never reach these steps: the bot filter removes them before translation.

## Project layout

```
CMakeLists.txt            build; deps from vcpkg if present, else FetchContent; embeds web/
CMakePresets.json         Visual Studio / command-line configurations
vcpkg.json                optional vcpkg manifest (pinned baseline)
cmake/EmbedAssets.cmake   web/* -> generated C++ byte arrays
src/main.cpp              CLI entry point
src/core/                 App (wiring), Settings, TokenStore, MessagePipeline, BotFilter,
                          TtsTextFilter (emote/emoji/link cleaning), EmoteRegistry (7TV/BTTV/FFZ),
                          EventHub (SSE), Util
src/net/                  HTTPS client + WebSocket client (WinHTTP on Windows,
                          cpp-httplib/IXWebSocket + OpenSSL elsewhere), OAuth2 flow
src/platforms/            TwitchClient, YouTubeClient, KickClient
src/services/             Translator (Google Translate), TtsService (Google TTS),
                          AudioPlayer (miniaudio: output devices + native playback)
src/desktop/              DesktopWindow (webview / WebView2 native window)
src/server/               WebServer: static UI, REST API, SSE, OAuth callbacks
web/                      dock UI (index.html, app.css, app.js) - no frameworks
tests/unit_tests.cpp      parser + TTS/translation decision tests (no network)
```

## Local API

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/settings` | current settings |
| POST | `/api/settings` | JSON merge-patch (needs header `X-USC: 1`) |
| GET | `/api/status` | per-platform connection state |
| GET | `/api/events` | SSE stream: `hello`, `chat`, `status`, `settings`, `clear` |
| GET | `/api/tts?text=&lang=` | MP3 audio |
| POST | `/api/auth/{twitch,youtube}/login` / `logout` | OAuth |
| GET | `/api/audio/devices` | output devices + saved choice |
| POST | `/api/audio/test`, `/api/audio/skip`, `/api/audio/clear` | native voice controls |
| GET | `/api/links` | dock / overlay URLs |
| POST | `/api/open?target=dock\|overlay\|overlayVoice` | open one of those links in the browser |
| POST | `/api/test`, `/api/clear`, `/api/reconnect?platform=` | tools |

The server listens only on `127.0.0.1`. It rejects requests whose `Host` isn't localhost, and every POST needs the `X-USC` header. Together these block other websites and DNS-rebinding attacks from controlling the app.
