# Unified Stream Chat (C++)

A lightweight C++ app that merges **YouTube**, **Twitch** and **Kick** live chat into one feed. You show it in OBS as a **Custom Browser Dock**. It also translates messages with Google Translate and reads them aloud with Google text-to-speech.

```
┌──────────────────────── OBS ────────────────────────┐
│  Custom Browser Dock  →  http://localhost:8787/      │
└───────────────▲─────────────────────────────────────┘
                │ HTTP + Server-Sent Events (127.0.0.1 only)
┌───────────────┴──────── unified_stream_chat (C++) ──────────────────────┐
│ TwitchClient  (IRC over WebSocket, OAuth)  ─┐                           │
│ YouTubeClient (Data API v3 polling, OAuth) ─┼─► MessagePipeline ─► EventHub ─► dock
│ KickClient    (Pusher WebSocket, username) ─┘   │ Google Translate            │
│                                                 └ TTS segments  /api/tts ◄── dock plays MP3
└─────────────────────────────────────────────────────────────────────────┘
```

## Features

- **One feed, three platforms.** Every message shows the platform logo and the platform's color code (a colored stripe and the logo in that color) next to the username. You can change the colors. Twitch and Kick emotes show as images, and role badges show too (host, mod, VIP, sub, member).
- **Logins:**
  - **Twitch:** OAuth2 login (authorization-code flow, scope `chat:read`). Tokens refresh automatically.
  - **YouTube:** Google OAuth2 login (scope `youtube.readonly`). The app finds your active live stream on its own, or you can enter a video ID or URL.
  - **Kick:** just type your username. No login needed.
- **Google Translate.** English messages stay as written, and the translated Arabic line appears right below them. Arabic is the default target and you can change it. There's an optional setting to translate every other foreign language too.
- **Google TTS** with separate toggles for:
  - reading the username (with a customizable phrase, e.g. `{user} says`)
  - reading the message text
  - **reading mode:** the original text as written, or the translated text
  - **skipping long messages** over a maximum character count you set
  - skipping `!commands` and links, choosing which platforms get read aloud, and an ignored-users list (bots)
  - volume, speed, queue limit, and a mute/skip button in the dock
- **Settings sidebar.** The ⚙ button opens a sidebar with tabs: **Connections · Chat · Translation · Voice (TTS) · Advanced**. Changes save right away to `settings.json`.
- **Overlay mode.** `http://localhost:8787/overlay` gives a transparent Browser Source for putting the chat on stream. It's silent unless you add `?tts=1`.
- **One small binary.** The web UI is built into the executable. Idle CPU use is near zero.

## Building

Requirements: CMake ≥ 3.20, a C++17 compiler, and OpenSSL 3. Everything else (cpp-httplib, IXWebSocket, nlohmann/json) is downloaded by CMake.

```bash
# Linux:   sudo apt install build-essential cmake libssl-dev
# macOS:   brew install cmake openssl@3   (then add -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3))
# Windows: install Visual Studio 2022 + CMake + OpenSSL (e.g. `choco install openssl`)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release     # unit tests
```

Every push to GitHub runs `.github/workflows/build.yml`, which builds downloadable Windows, macOS and Linux binaries. On Windows the OpenSSL DLLs are included.

## Running and adding it to OBS

1. Start the app with `unified_stream_chat` (on Windows, `UnifiedStreamChat.exe`). It prints the dock URL.
2. In OBS, go to **Docks → Custom Browser Docks…**. Enter the name `Chat` and the URL `http://localhost:8787/`.
3. Click ⚙ in the dock and connect your platforms (see below).
4. Optional: to show the chat on stream, add a **Browser Source** with the URL `http://localhost:8787/overlay`.

Command-line options: `--port <n>`, `--config <dir>`, `--web <dir>` (serves the UI from disk while you work on it), `--open`.

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

For every live message (the backlog loaded when the app connects is never read aloud), the app:

1. Skips the message if TTS is off, the platform is excluded, the user is on the ignored list, or the message is a `!command`.
2. Removes links (if that option is on). Skips the message **entirely** if it is longer than *Skip messages longer than* characters.
3. Picks the text to read: the **original** in its detected language, or the **translated** text in the target language.
4. Builds the speech from the username phrase and/or the message text, based on your toggles.

The dock plays the speech in order through `/api/tts`, and the backend fetches the MP3 from Google. TTS audio plays in the OBS dock, so OBS doesn't capture it into your stream by default. To put it on stream, use the overlay with `?tts=1` as a Browser Source and mute TTS in the dock.

## Project layout

```
CMakeLists.txt            build, FetchContent deps, embeds web/ into the binary
cmake/EmbedAssets.cmake   web/* -> generated C++ byte arrays
src/main.cpp              CLI entry point
src/core/                 App (wiring), Settings, TokenStore, MessagePipeline, EventHub (SSE), Util
src/net/                  HTTPS client helpers, OAuth2 authorization-code flow
src/platforms/            TwitchClient, YouTubeClient, KickClient
src/services/             Translator (Google Translate), TtsService (Google TTS)
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
| POST | `/api/test`, `/api/clear`, `/api/reconnect?platform=` | tools |

The server listens only on `127.0.0.1`. It rejects requests whose `Host` isn't localhost, and every POST needs the `X-USC` header. Together these block other websites and DNS-rebinding attacks from controlling the app.
