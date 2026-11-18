# Google Home Desktop (unofficial)

A small C wrapper around the Google Home web app (https://home.google.com) for Linux. It embeds [gsurf](https://git.podbielniak.com/zachpodbielniak/gsurf), a GObject web browser library, on WebKitGTK 6.0 and GTK4. You get one window with a persistent login. Google's own sign-in and account pages stay in the window, and every other link opens in your system browser. The window size and page zoom are remembered between runs.

Versions up to 0.1.1 were an Electron app. Since 0.2.0 the app is plain C: libgsurf is linked statically, and the only runtime dependency is the system WebKitGTK.

## Limitations

- **Camera live view probably does not work.** Nest camera streams use WebRTC, and Fedora builds WebKitGTK without it (`RTCPeerConnection` is undefined). Device controls, automations, and camera event history do not need it. The app enables WebRTC, so a WebKitGTK built with it would get live view.
- **No passkeys.** Fedora's WebKitGTK has no Web Authentication API (`navigator.credentials` is undefined), so Google offers only password sign-in here. To sign in with a passkey, sign in to https://home.google.com in Firefox, then choose **Import from Firefox**.
- Wayland does not let apps place their own windows, so the window position is not restored. The size, maximized state, and zoom are.

## Install a release

The release builds are for x86_64 and are built on Ubuntu 22.04, so they also run on newer systems. They need glibc 2.34 or later, GTK 4.6 or later, and WebKitGTK 6.0. They're tested on Ubuntu 22.04, Ubuntu 24.04, and Fedora 44.

Download the files from the [Releases page](https://github.com/averagenative/google-home-desktop/releases), with `SHA256SUMS`. To check a download, run `sha256sum -c --ignore-missing SHA256SUMS` in the download folder.

### Ubuntu and Debian

Install the `.deb` with apt, which also installs WebKitGTK and the other dependencies:

```bash
sudo apt install ./google-home_<version>_amd64.deb
```

On Ubuntu 23.10 and later, the package adds an AppArmor profile, `/etc/apparmor.d/google-home`, so WebKit's sandbox can create the user namespaces it needs. To uninstall, run `sudo apt remove google-home`.

### Other distributions

Install WebKitGTK 6.0 with your package manager (on Fedora, `sudo dnf install webkitgtk6.0`), then unpack the binary tarball into `/usr/local`:

```bash
sudo tar -xzf google-home-<version>-x86_64-linux.tar.gz -C /
```

To uninstall, delete the files the archive lists:

```bash
tar -tzf google-home-<version>-x86_64-linux.tar.gz | sed 's|^|/|' | sudo xargs rm -f
```

Each release also has `google-home-desktop-<version>.tar.gz`, the complete source with gsurf included. It builds with `make`, no git needed.

## Build

Install the build dependencies (Fedora):

```bash
sudo dnf install gcc make git pkgconf-pkg-config gtk4-devel webkitgtk6.0-devel \
    libsoup3-devel json-glib-devel libyaml-devel sqlite-devel readline-devel
```

Then build:

```bash
git clone https://github.com/averagenative/google-home-desktop
cd google-home-desktop
make deps      # gsurf submodule and its dependencies, over HTTPS
make           # -> build/google-home
make run       # try it from the tree
```

`make` stops with the `dnf` command above if a development package is missing. gsurf's own submodules point at SSH URLs; `make deps` fetches them over HTTPS, so you don't need SSH keys for those hosts.

## Install from source

```bash
make install                         # ~/.local/bin/google-home and a launcher
sudo make install PREFIX=/usr/local  # or for every user
```

This installs the binary, the `google-home-desktop.desktop` launcher, and the icon, so GNOME's search (Super key) finds "Google Home". `make uninstall` (with the same `PREFIX`) removes them.

### Moving from the Electron build

The Electron AppImage and its launcher are separate files. Remove them so search doesn't show two "Google Home" entries:

```bash
rm ~/Applications/google-home-x86_64.AppImage \
   ~/.local/share/applications/io.github.averagenative.googlehome.desktop
rm -r ~/.config/google-home-desktop   # the Electron session and logs
```

The Electron session cannot be carried over. On its first launch, the C app imports your Firefox session if there is one (see [Sign-in](#sign-in)), or you sign in again.

## Sign-in

The app has no Google session on first launch. It does the following:

1. If Firefox is signed in to Google, the app copies that session (the `*.google.com` cookies of the most recently used Firefox profile) and loads Google Home.
2. Otherwise it opens Google's sign-in page and offers two choices: **Sign In Here**, or **Import from Firefox** after you sign in to https://home.google.com in Firefox.

Signing in on that page works: Google accepts sign-in from WebKitGTK (tested in November 2026). With Electron it was impossible, because Google's BotGuard rejects Chromium-based embedded browsers. The Firefox import stays as a fallback, in case Google starts answering "This browser or app may not be secure" here too.

The import works only while Firefox's session is fresh. If the app lands on a sign-in, account chooser, or `CookieMismatch` page ("We've detected a problem with your cookie settings"), open https://home.google.com in Firefox, then import again. Each import replaces all of the app's Google cookies, because leftovers from an old session are what cause the mismatch.

To check what an import would find, without changing anything:

```bash
google-home --check-firefox
```

To force a fresh import at launch, run `google-home --import-firefox`. Add `--firefox-profile=DIR` to choose the profile.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+R, F5 | Reload |
| Ctrl+Shift+R, Shift+F5 | Reload, bypassing the cache |
| Alt+Left, Alt+Right, mouse thumb buttons | Back, forward |
| Ctrl+Plus, Ctrl+Minus, Ctrl+0 | Zoom in, out, reset |
| F11 | Full screen |
| Ctrl+Q | Quit |
| F12, Ctrl+Shift+I | Web Inspector (only with `--inspector`) |

## Configuration

The app reads an optional gsurf configuration file from `~/.config/google-home-desktop/config.yaml`. It uses gsurf's format; `vendor/gsurf/data/default-config.yaml` lists every key.

On Google's sites, WebKit's site-specific quirks choose the user agent themselves, and they override `user_agent`. If Google turns that user agent away, turn the quirks off as well:

```yaml
user_agent: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15"
webkit:
  site_quirks: false
```

## Scripting with gsurfctl

Start the app with `--control` and it opens a gsurf control socket named `google-home`. Build gsurf's client with `make gsurfctl`, then:

```bash
gsurfctl -i google-home uri
gsurfctl -i google-home eval 'document.title'
gsurfctl -i google-home screenshot home.png
```

Any program running as your user can use the socket, and the page it drives is signed in to your Google account. That's why the socket is off by default.

## Files

| Path | Contents |
|---|---|
| `~/.local/share/google-home-desktop/` | Web data: cookies (your Google session), local storage |
| `~/.cache/google-home-desktop/` | Web cache |
| `~/.local/state/google-home-desktop/main.log` | Log: startup, page loads (without query strings), import results |
| `~/.local/state/google-home-desktop/window.ini` | Window size, maximized state, zoom |
| `~/.config/google-home-desktop/config.yaml` | Optional configuration |

## Development notes

- `src/main.c` is the window and link policy, `src/firefox-import.c` the session import, `src/window-state.c` and `src/log.c` the rest.
- gsurf is pinned as a submodule in `vendor/gsurf`. To update it, check out a newer tag there, then run `make deps` and `make`.
- To build the release files, commit, then run `make dist` for the source tarball and `make portable` for the binary tarball and the `.deb`. `make portable` needs podman. It builds on Ubuntu 22.04 in a container, the oldest base the app supports (GTK 4.6, GLib 2.72, glibc 2.35), so the binaries also run on newer systems. The container applies `packaging/ubuntu-22.04/gsurf-gtk4.6-glib2.72.patch` to its own copy of gsurf; `vendor/gsurf` itself stays unpatched.
- The icon source is `data/google-home-desktop.svg`; regenerate the PNG with `rsvg-convert -w 512 -h 512 data/google-home-desktop.svg -o data/google-home-desktop.png`.

## License

AGPL-3.0-or-later, the same as gsurf, which this program links. See [LICENSE](LICENSE). Releases up to 0.1.1 (the Electron app) were MIT licensed.
