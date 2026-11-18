# Google Home Desktop (unofficial)

Electron wrapper around the Google Home web app (https://home.google.com),
packaged as a Linux AppImage. No official Linux client exists and no
community wrapper existed as of June 2026, so this is a thin shell:
persistent login session, Chrome user agent (Google blocks logins from
user agents it identifies as embedded browsers), external links open in
the system browser, window size/position remembered.

## Install from a release

Download `Google-Home-<version>-x86_64.AppImage` from the [Releases page](https://github.com/averagenative/google-home-desktop/releases), make it executable (`chmod +x`), and run it. Sign-in needs Firefox and `python3`; see [Sign-in](#sign-in).

## Build

```bash
npm install
npm run build        # -> dist/Google Home-<version>.AppImage
```

## Install (AppImage + GNOME launcher entry)

```bash
./install-desktop.sh
```

Copies the AppImage to `~/Applications/google-home-x86_64.AppImage` and
registers the icon + `.desktop` entry so Super-key search finds
"Google Home".

## Run during development

```bash
npm start
```

## Sign-in

Google's BotGuard rejects sign-in from any embedded browser ("This
browser or app may not be secure") no matter the user agent, so login
happens out-of-band:

1. Sign in to https://home.google.com in regular Firefox.
2. Launch the app. If it has no Google session it runs the bundled
   `import-session.py` itself, copying your *.google.com cookies from
   Firefox; if Google signs it out later it offers the same import in
   a dialog. (You can still run `./import-session.py` by hand — the
   app ingests the staged file on next launch and deletes it.)

The import only succeeds if Firefox's session is fresh — if you land
on an account-chooser/sign-in page, redo the Firefox sign-in first,
then accept the import dialog again.

A session left unused for weeks can also land on
`accounts.google.com/CookieMismatch` ("We've detected a problem with
your cookie settings"). The app treats that as signed out and offers
the import, too. Each import replaces all of the app's Google cookies
rather than merging, because leftover cookies from the old session are
what trigger the mismatch.

## Notes

- The session persists in `~/.config/google-home-desktop/`.
- Runs natively on Wayland (`ozone-platform-hint=auto`).
- Icon source is `build/icon.svg`; regenerate the PNG with
  `rsvg-convert -w 512 -h 512 build/icon.svg -o build/icon.png`.
