# Unified Stream Chat (C++)

A lightweight C++ app that merges **YouTube**, **Twitch**, **Kick** and **TikTok LIVE** chat into one feed. It works two ways at the same time (**hybrid mode**):

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
 TikTok  (webcast WS)─┤
                      └─────────────────────────────────────────────────┘
```

## Features

- **Hybrid mode.** Double-click `UnifiedStreamChat.exe` and it opens its own window. The OBS/browser links keep working while it runs. Use `--no-window` to run only the server. Closing the window quits the app.
- **One feed, four platforms.** Every message shows the platform logo and color code (a colored stripe and the logo in that color) next to the username. You can change the colors. Twitch and Kick emotes and role badges are shown.
- **Logins:**
  - **Twitch:** OAuth2 login.
  - **YouTube:** Google OAuth2 login. The app finds your active live stream on its own.
  - **Kick:** just type your username. The chatroom is looked up once and remembered.
  - **TikTok LIVE:** type the @username. This needs a signing-server API key (see below).
- **Google Translate.** English messages stay as written, with the translated Arabic line right below them. The target language can be changed.
  - **Arabic is never translated.** Arabic in any dialect or casual spelling ("هاي", "شلونك", Iraqi/Gulf "شگد", "چا") is shown and read exactly as written, with the Arabic voice.
    - The app recognizes Arabic from the letters themselves and never sends it to Google. Google's detection often labels short dialect words as Persian or Urdu, which used to cause unwanted "translations".
    - Real Persian/Urdu spelling (ی ک پ ژ ے) is still translated.
  - **English slang and abbreviations** (idk, brb, gg, wp, ngl, tbh, ty, lol, W/L, …, about 150 built in, plus your own `abbr = meaning` lines) are expanded before translating. For example, "idk bro, gg wp" becomes "I don't know, brother, good game, well played".
    - The Arabic line under the message is then a real translation, not the abbreviation echoed back or a failed detection.
    - The voice uses the same text: the expanded English in *original* mode, or the same Arabic line in *translated* mode.
    - Short slang is translated as English even when Google's detection guesses another language.
  - If Google's free endpoint rate-limits (HTTP 429), translation pauses for 60 s instead of retrying non-stop. A Google Cloud API key avoids the limit.
- **Video capture relay (Windows).** One capture device (Elgato etc.) is shown in a window that OBS *Game Capture* and TikTok Studio can both capture, so the device is opened only once. It has its own page (🎥 button, or `/capture`). See [Video capture relay](#video-capture-relay-windows).
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
**Normally nothing to set up.** The app has its own Google OAuth client (Client ID `233537098772-9ehsehkjvebipcvomcjeujlhrfeocbe9.apps.googleusercontent.com`):

1. Click **Login with YouTube** once. If you stream on a Brand Account, pick that channel when Google asks.
2. The login (access token, refresh token, expiry and account) is saved in `tokens.json` and renewed automatically. It survives restarts and new streams until you click **Logout**, which also revokes it at Google.

How the login stays valid:
- **Bound to its client.** Each login remembers which OAuth client created it and is always renewed with that same client, so changing credentials in Settings never breaks it.
- **Only a dead token ends it.** A login is only dropped when Google says the refresh token itself is dead (`invalid_grant`).
- **Other failures keep it.** Network errors, Google outages, rate limits or a rejected client secret keep the login, show the reason on the YouTube card, and retry.
- **Secure flow.** The login uses PKCE with `access_type=offline` and `prompt=consent select_account`.

**For whoever builds/distributes the app.**
- **The client secret is not in git.** Inject it at build time with `cmake -DUSC_GOOGLE_CLIENT_SECRET=...`, or add a repository secret `USC_GOOGLE_CLIENT_SECRET` (GitHub → Settings → Secrets and variables → Actions) so the CI release builds include it. Google does not treat the secret of a *Desktop app* client as confidential.
- **Builds without it.** Users can paste it once under *YouTube → Google client*, and it is saved.
- **In the Google Cloud project for that client:**
  1. Enable **YouTube Data API v3**.
  2. Set the OAuth consent screen / Audience to **In production** (*Publish app*). While it is in **Testing**, Google expires every refresh token after **7 days** and only listed test users can log in. That is the usual cause of "I have to log in again every week". The app recognises this case (`invalid_grant`) and says so.
  3. Because `youtube.readonly` is a sensitive scope, an unverified app shows a "Google hasn't verified this app" notice (*Advanced → Go to …*) and allows up to 100 users. Submit the app for verification to remove both.
- **Your own client.** *Use my own Google credentials* (under *Google client*) switches to your own client. It must be a "Desktop app" OAuth client, and the redirect URL is shown there.

**Stream detection.** In order:
1. The video in *Stream → Video ID* if set. An ended video falls through to the next steps instead of blocking.
2. Your **live** broadcast.
3. A broadcast in preview, about to start, or the persistent "Stream now" one. Its chat opens before you go live and continues when you do. While on such a chat, the app re-checks every 60 s in case a different broadcast goes live.

**API quota.** Every check costs YouTube Data API quota (10,000 units/day by default), so the app spends it only where it matters:
- **Offline.** One `liveBroadcasts.list` call per check (the upcoming-broadcast query runs only on every third check). Checks back off: 1, 2, 4, 8 min, up to *When not live, check at most every* (10 min by default). That is about 150–300 calls a day at most, instead of about 11,500 with the old fixed 15 s checks.
- **On demand.** **Reconnect**, or OBS starting to stream (see *OBS link*), switches to quick checks (every 15 s) for 3 minutes, then backs off again.
- **Chat open, not live yet.** Read every 30 s instead of every 6 s, with one live check every 2 minutes.
- **Live.** Polling follows YouTube's own `pollingIntervalMillis` (at least *Minimum poll interval while live*).
- **Out of quota.** The app pauses for 30 minutes instead of retrying.
- **Failed login renewals.** These back off from 30 s to 10 min, and no login link is opened or shown unless you click *Login* / *Switch account*.

The status always names the channel and the reason. When a stream ends, the next one is searched for straight away.

Quota note: each chat poll costs API quota units, and the default quota is 10,000 units per day. The app respects YouTube's `pollingIntervalMillis` and never polls faster than *Minimum poll interval* (6 s by default). Raise it for long streams, or ask Google for more quota.

### Kick
Enter your channel name and turn it on. The line under the Kick card always says what is happening, and why if something fails.

How it connects:
1. **Chatroom ID.** It comes from, in order:
   - a manual override
   - the ID remembered for this username (`kick.resolvedChatroomId` in `settings.json`)
   - a lookup on `kick.com/api/v2/channels/<name>`, then `/chatroom`, then `/api/v1/channels/<name>`

   A successful lookup is saved, so it only has to succeed once.
2. **Pusher WebSocket.** It connects to `wss://ws-<cluster>.pusher.com/app/<key>` with `Origin: https://kick.com` and subscribes to `chatrooms.<id>.v2` and `chatroom_<id>`.
   - **The key rotates.** Kick retired `32cbd69e4b950bf97679` in October 2026. The current key (`34bf7a0ff419a2a775b9`, `us2`) is now fetched from Kick each time the app connects, via `POST web.kick.com/api/v1/realtime/connection` with `accepted_providers: [pusher]`.
   - If Pusher still answers 4001 ("app key not in this cluster"), the app fetches the key again and retries automatically.
   - Old keys in existing settings files are replaced on startup.
3. **Connected.** The status turns *connected* only when Kick confirms the subscription. If that doesn't happen within 15 s, the app shows an error and reconnects.

Pusher error codes 4000–4099 (wrong key, app disabled, origin rejected) are shown as errors and retried after 60 s, instead of looping on "connecting".

**If Cloudflare blocks the lookup** ("Kick blocked the automatic lookup"), click **Find chatroom ID**:
1. Your own browser opens the channel's data page (browsers get past Cloudflare).
2. Select all and copy (Ctrl+A, Ctrl+C), then paste it into the box. Just the chatroom number works too.
3. Click **Use this**.

This is needed once per channel. **Re-detect** forgets the saved ID. If Kick ever changes its Pusher key, update it under *Kick → Troubleshooting*.

### TikTok LIVE
Type the @username and turn it on. **No API keys, no TikTok developer account, no signing server.** The app joins the LIVE the same way a viewer's browser does:

1. **Live check.** One light request (`tiktok.com/api-live/user/room`) gives the room ID. The user is live when `liveRoom.status` or `user.status` is `2`.
   - While offline, the status is *waiting*. Checks back off: 1, 2, 4 min, up to 5 min.
   - **Reconnect**, or starting your stream in OBS (see *OBS link*), checks immediately.
2. **Viewer session.** An anonymous `ttwid` cookie from `https://www.tiktok.com/`. TikTok sets it only on some requests, so it is retried up to 8 times.
3. **WebSocket.** `wss://webcast-ws.tiktok.com/webcast/im/ws_proxy/ws_reuse_supplement/?room_id=…&identity=audience&…`, with no X-Bogus, msToken or signature, and `Cookie: ttwid`.
4. **Frames.** Heartbeat and `im_enter_room`, then `WebcastPushFrame` protobufs with gzip payloads (decoded with a built-in protobuf reader and miniz), acknowledged when TikTok asks. `WebcastChatMessage` events become chat messages.

**What the status means:**
- `connecting`: checking live status, getting the viewer session, or joining the room.
- `connected`: TikTok has actually answered after the app entered the room ("Listening to @user's LIVE (room …) – N chat messages so far"). An open socket alone doesn't count.
- `waiting`: the user is not live, or the LIVE ended.
- `error`: account not found, rate-limited, `DEVICE_BLOCKED` (a fresh viewer session is started automatically), or TikTok did not accept the connection within 30 s three times.
- No data at all for 60 s triggers a reconnect.

**Optional fallback.** A signing server (e.g. Euler Stream) is only used if you enter its API key under *Advanced*.

The connection flow follows the open-source PirateTok connectors. TikTok can change it without notice; all endpoints and field numbers are in `src/platforms/TikTokClient.cpp`.

### OBS link (event-driven checks)
The app connects to OBS's own WebSocket server (OBS 28+, *Tools → WebSocket Server Settings*; default `127.0.0.1:4455`, password from *Show Connect Info*).
- **It's local only.** It costs no API quota and contacts no streaming platform.
- **It reacts to "stream started".** When OBS starts streaming (`StreamStateChanged`), YouTube and TikTok check right away and keep checking every 15 s for 3 minutes until the stream shows as live. Nothing needs to poll often while you are offline.
- **It stays quiet when OBS is closed.** It retries every 30 s at most and only reports when its status changes.

### Video capture relay (Windows)
A capture card can be opened by only one program at a time. When OBS has a *Video Capture Device* source and TikTok Studio wants the same card, one of them fails. The relay opens the card **once** and shows it in a normal window, **USC Capture Output**. Any number of programs can then capture that window.

**Setup:**
1. Remove the card's *Video Capture Device* source from OBS (and close other apps using it).
2. Open the 🎥 page and pick the device. Its modes are read once and listed; only these are ever used.
3. Pick the output resolution (4K / 1440p / 1080p / 720p / match source) and FPS cap (144 / 120 / 100 / 60 / match source), then press **Start**.
4. In OBS add **Game Capture** → Mode *Capture specific window* → Window `[UnifiedStreamChat.exe]: USC Capture Output`. In TikTok Studio, add a window capture of the same window.
5. Keep the window open. It may sit behind other windows or on another monitor, but **not minimized**: Windows stops drawing minimized windows, so every capture would go black.

**How it works:**

```
capture.html ──HTTP/SSE──> WebServer ──> CaptureRelay (worker thread + command queue)
                                           ├─ MfVideoBackend  Media Foundation source reader (async)
                                           │     device mode exactly as reported; NV12/YUY2 pass through
                                           └─ D3DOutputWindow  top-level window + D3D11 flip swap chain
                                                 GPU video processor: YUV→RGB, scaling, letterbox
                                                 Present(DO_NOT_WAIT); preview JPEG only on request
```

**Pitfalls it avoids:**
- **Device contention.** The device is opened once. Listing devices never opens them. Probing opens a device briefly and is never done on the device that is streaming. Errors name the cause: "in use by another app", "blocked by camera privacy", "disconnected".
- **Unsupported modes.** Only modes the device reports are offered. *Auto* picks one that fits the output, in this order: fits the resolution, reaches the FPS, most detail, least wasted FPS, cheapest format. A saved mode the device no longer offers falls back to auto, with a note.
- **Colour and CPU cost.** NV12 and YUY2 go straight to the GPU, where the video processor does BT.709/601 limited→full range conversion and scaling. Only MJPG/RGB modes use Windows' decoder, and the UI marks them "CPU decode".
- **FPS.** The cap drops frames with a drift-free pacer (120 → 60 shows every other frame). A cap at or above the source rate does nothing. The page shows the measured capture FPS next to the output FPS.
- **Black screen in Game Capture.**
  - The window is a plain top-level window (no tool-window or layered style) drawn by a D3D11 flip-model swap chain, the path Game Capture hooks.
  - The swap chain stays at the output resolution whatever size the window is, so a small window still captures in full resolution.
  - The window gets black content immediately, before the first frame arrives.
  - The page warns when the window is minimized.
- **No idle cost.** When the relay is off, its thread sleeps until a command arrives. The preview (640×360 at about 5 fps) is made only while the page is visible and asks for it.
- **No UI blocking.** Every API call only queues work. If the device loses its signal, the status turns *No signal*; if it is unplugged, the relay retries with back-off (5 → 30 s) and finds it again by name on another USB port. Closing the output window stops the relay and saves that choice.

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
src/capture/              video capture relay: CaptureTypes (mode choice, pacing - portable, tested),
                          CaptureRelay (worker), MfVideoBackend (Media Foundation),
                          D3DOutputWindow (D3D11 output window), CaptureStubs (non-Windows)
src/desktop/              DesktopWindow (webview / WebView2 native window)
src/server/               WebServer: static UI, REST API, SSE, OAuth callbacks
web/                      dock UI (index.html, app.css, app.js) + capture page (capture.*) - no frameworks
tests/unit_tests.cpp      parser + TTS/translation decision tests (no network)
```

## Local API

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/settings` | current settings |
| POST | `/api/settings` | JSON merge-patch (needs header `X-USC: 1`) |
| GET | `/api/status` | per-platform connection state |
| GET | `/api/events` | SSE stream: `hello`, `chat`, `status`, `settings`, `clear`, `speak`, `capture` |
| GET | `/api/tts?text=&lang=` | MP3 audio |
| POST | `/api/auth/{twitch,youtube}/login` / `logout` | OAuth |
| GET | `/api/audio/devices` | output devices + saved choice |
| POST | `/api/audio/test`, `/api/audio/skip`, `/api/audio/clear` | native voice controls |
| GET | `/api/links` | dock / overlay URLs |
| POST | `/api/open?target=dock\|overlay\|overlayVoice` | open one of those links in the browser |
| POST | `/api/test`, `/api/clear`, `/api/reconnect?platform=` | tools |
| GET | `/api/capture/status` | relay state, chosen mode, measured FPS, window state (SSE event `capture`) |
| GET | `/api/capture/devices` · POST `/api/capture/devices/refresh` | capture devices |
| GET | `/api/capture/modes?device=` | the device's own modes (`probing: true` while being read) |
| GET | `/api/capture/preview.jpg` | small preview frame (204 when there is none) |
| POST | `/api/capture/start`, `/api/capture/stop` | turn the relay on / off (saved) |

The server listens only on `127.0.0.1`. It rejects requests whose `Host` isn't localhost, and every POST needs the `X-USC` header. Together these block other websites and DNS-rebinding attacks from controlling the app.
