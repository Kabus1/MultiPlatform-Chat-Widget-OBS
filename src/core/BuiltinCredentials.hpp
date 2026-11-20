#pragma once

// OAuth client shipped with the app, so users never have to create their own
// Google Cloud project: they just click "Login with YouTube" once.
//
// The Client ID is public by design (it is sent to every browser in the
// login URL). For a Google "Desktop app" client the client secret is not
// confidential either (Google: "the client secret is obviously not treated
// as a secret" for installed apps), but it is kept out of git and injected at
// build time:
//   cmake -DUSC_GOOGLE_CLIENT_SECRET=...           (local build)
//   GitHub Actions secret USC_GOOGLE_CLIENT_SECRET (CI release builds)
// Settings > YouTube > "Use my own Google credentials" still overrides both.

#ifndef USC_GOOGLE_CLIENT_ID
#define USC_GOOGLE_CLIENT_ID "233537098772-9ehsehkjvebipcvomcjeujlhrfeocbe9.apps.googleusercontent.com"
#endif

#ifndef USC_GOOGLE_CLIENT_SECRET
#define USC_GOOGLE_CLIENT_SECRET ""
#endif

namespace usc::builtin {
inline constexpr const char* kGoogleClientId = USC_GOOGLE_CLIENT_ID;
inline constexpr const char* kGoogleClientSecret = USC_GOOGLE_CLIENT_SECRET;
} // namespace usc::builtin
