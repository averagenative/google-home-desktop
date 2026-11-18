const { app, BrowserWindow, shell, session, Menu, dialog } = require('electron');
const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');

// The bare home.google.com lands on a marketing page whose "Go to
// Google Home web" button just points at /login — so start there.
// With a live session it goes straight into the app.
const HOME_URL = 'https://home.google.com/login';

// Run natively on Wayland when available, X11 otherwise.
app.commandLine.appendSwitch('ozone-platform-hint', 'auto');

// Electron 36 defaults to GTK4 on GNOME Wayland, but GTK3 modules still
// get loaded into the process and GTK aborts with "GTK 2/3 symbols
// detected". Force GTK3 everywhere. (electron/electron#46538)
app.commandLine.appendSwitch('gtk-version', '3');

const logFile = path.join(app.getPath('userData'), 'main.log');

function log(...parts) {
  const line = `[${new Date().toISOString()}] ${parts.join(' ')}`;
  console.error(line);
  try {
    fs.appendFileSync(logFile, line + '\n');
  } catch {}
}

process.on('uncaughtException', (err) => log('uncaughtException:', err.stack || err));
process.on('unhandledRejection', (err) => log('unhandledRejection:', err));

const stateFile = () => path.join(app.getPath('userData'), 'window-state.json');

function loadWindowState() {
  try {
    return JSON.parse(fs.readFileSync(stateFile(), 'utf8'));
  } catch {
    return { width: 1200, height: 800 };
  }
}

function saveWindowState(win) {
  if (win.isMinimized() || win.isFullScreen()) return;
  const [width, height] = win.getSize();
  const [x, y] = win.getPosition();
  try {
    fs.writeFileSync(stateFile(), JSON.stringify({ width, height, x, y }));
  } catch {}
}

// Google rejects sign-in from anything it identifies as an embedded
// browser. Stripping tokens from the default UA is fragile (the app
// name sneaks in as "GoogleHome/x.y"), so build a stock Chrome UA from
// the Chromium version Electron actually ships — the engine really is
// that Chrome, so feature probes match.
// Sign-in happens out-of-band (import-session.py), so no Firefox
// disguise: claiming Gecko makes Google serve Gecko-targeted assets,
// and its Google Symbols woff2 build fails Chromium's OTS sanitizer —
// every icon then renders as its ligature name in serif. A clean
// Chrome UA (sans Electron/app tokens, which trip Google's
// embedded-browser checks) gets Chromium-compatible assets.
const CHROME_UA = `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${process.versions.chrome} Safari/537.36`;
app.userAgentFallback = CHROME_UA;

// Google's sign-in cannot be completed in-app at all — BotGuard
// fingerprints the JS runtime and rejects Electron regardless of UA.
// import-session.py stages cookies from a real Firefox login here; we
// ingest and delete the file before the first page load.
async function importStagedCookies() {
  const staging = path.join(app.getPath('userData'), 'import-cookies.json');
  let cookies;
  try {
    cookies = JSON.parse(fs.readFileSync(staging, 'utf8'));
  } catch {
    return;
  }
  // Replace the old session wholesale. Mixing fresh cookies with
  // leftovers from a stale session leaves conflicting copies, and
  // Google answers that with accounts.google.com/CookieMismatch.
  const jar = session.defaultSession.cookies;
  let cleared = 0;
  for (const c of await jar.get({})) {
    const host = c.domain.replace(/^\./, '');
    if (host !== 'google.com' && !host.endsWith('.google.com')) continue;
    try {
      await jar.remove(`https://${host}${c.path}`, c.name);
      cleared++;
    } catch {}
  }
  let ok = 0;
  for (const c of cookies) {
    // Firefox writes domain cookies with a leading dot and host-only
    // cookies without one. Passing `domain` for a host-only cookie
    // widens it (accounts.google.com -> .accounts.google.com), and
    // Google then sees two different LSIDs. Host-only cookies,
    // including every __Host- cookie, get no domain attribute.
    const hostOnly = !c.host.startsWith('.');
    try {
      await jar.set({
        url: `https://${c.host.replace(/^\./, '')}${c.path}`,
        ...(hostOnly ? {} : { domain: c.host }),
        path: c.path,
        name: c.name,
        value: c.value,
        secure: c.secure,
        httpOnly: c.httpOnly,
        expirationDate: c.expirationDate,
        sameSite: c.sameSite,
      });
      ok++;
    } catch (err) {
      log('cookie import failed:', c.name, String(err));
    }
  }
  try {
    fs.unlinkSync(staging);
  } catch {}
  log(`cleared ${cleared} old cookies; imported ${ok}/${cookies.length} from staging file`);
}

// The same script that ships in the repo is bundled into the AppImage
// (extraResources), so the app can run the Firefox import itself.
function importScriptPath() {
  return app.isPackaged
    ? path.join(process.resourcesPath, 'import-session.py')
    : path.join(__dirname, '..', 'import-session.py');
}

function runImportScript() {
  return new Promise((resolve) => {
    execFile('python3', [importScriptPath()], (err, stdout, stderr) => {
      if (err) {
        log('import-session failed:', (stderr || String(err)).trim());
        resolve(false);
      } else {
        log('import-session:', stdout.trim().split('\n').pop());
        resolve(true);
      }
    });
  });
}

async function haveSession() {
  const cookies = await session.defaultSession.cookies.get({
    name: '__Secure-1PSID',
  });
  return cookies.length > 0;
}

function stripClientHints() {
  session.defaultSession.webRequest.onBeforeSendHeaders((details, callback) => {
    for (const header of Object.keys(details.requestHeaders)) {
      if (header.toLowerCase().startsWith('sec-ch-ua')) {
        delete details.requestHeaders[header];
      }
    }
    callback({ requestHeaders: details.requestHeaders });
  });
}

function isAllowedOrigin(url) {
  try {
    const { hostname } = new URL(url);
    return (
      hostname === 'home.google.com' ||
      hostname === 'accounts.google.com' ||
      hostname === 'accounts.youtube.com' ||
      hostname.endsWith('.googleusercontent.com') ||
      hostname === 'myaccount.google.com' ||
      hostname === 'gds.google.com'
    );
  } catch {
    return false;
  }
}

function createWindow() {
  const state = loadWindowState();
  const win = new BrowserWindow({
    width: state.width,
    height: state.height,
    x: state.x,
    y: state.y,
    icon: path.join(__dirname, '..', 'build', 'icon.png'),
    autoHideMenuBar: true,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });

  // Keep auth and app navigation in-window; open everything else
  // (help pages, store links) in the system browser.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (isAllowedOrigin(url)) return { action: 'allow' };
    shell.openExternal(url);
    return { action: 'deny' };
  });

  win.webContents.on('will-navigate', (event, url) => {
    if (!isAllowedOrigin(url)) {
      event.preventDefault();
      shell.openExternal(url);
    }
  });

  win.on('close', () => saveWindowState(win));

  // In-app sign-in is impossible (BotGuard), so landing on the Google
  // sign-in or account-chooser page means the session is gone — offer a
  // fresh import. A stale or conflicting session can also land on
  // /CookieMismatch ("We've detected a problem with your cookie
  // settings"), which a re-import fixes too.
  const SIGNED_OUT = /signin|accountchooser|cookiemismatch/i;
  let offeredImport = false;
  win.webContents.on('did-navigate', async (e, url) => {
    if (offeredImport) return;
    let u;
    try {
      u = new URL(url);
    } catch {
      return;
    }
    if (u.hostname !== 'accounts.google.com' || !SIGNED_OUT.test(u.pathname))
      return;
    offeredImport = true;
    const { response } = await dialog.showMessageBox(win, {
      type: 'question',
      message: 'Google signed you out',
      detail:
        'Sign-in cannot happen inside this app. Sign in to ' +
        'home.google.com in Firefox first, then import that session.',
      buttons: ['Import from Firefox', 'Cancel'],
      defaultId: 0,
      cancelId: 1,
    });
    if (response === 0) {
      if (await runImportScript()) {
        await importStagedCookies();
        win.loadURL(HOME_URL);
      }
      offeredImport = false;
    }
  });

  win.once('ready-to-show', () => log('window ready-to-show'));
  win.webContents.on('did-start-loading', () => log('did-start-loading'));
  win.webContents.on('did-finish-load', () =>
    log('did-finish-load:', win.webContents.getURL()));
  win.webContents.on('did-fail-load', (e, code, desc, url) =>
    log('did-fail-load:', code, desc, url));
  win.webContents.on('render-process-gone', (e, details) =>
    log('render-process-gone:', JSON.stringify(details)));
  win.webContents.on('console-message', (event) => {
    if (event.level === 'error' || event.level === 'warning')
      log('page-console:', event.level, event.message);
  });

  log('creating window', JSON.stringify(state), 'ua:', CHROME_UA);
  win.loadURL(HOME_URL);
  return win;
}

app.on('child-process-gone', (e, details) =>
  log('child-process-gone:', JSON.stringify(details)));

app.whenReady().then(async () => {
  log(
    `app ready: electron=${process.versions.electron}`,
    `wayland=${!!process.env.WAYLAND_DISPLAY}`,
    `session=${process.env.XDG_SESSION_TYPE || '?'}`,
    `log=${logFile}`
  );
  Menu.setApplicationMenu(null);
  stripClientHints();
  await importStagedCookies();
  if (!(await haveSession())) {
    log('no Google session — importing from Firefox');
    if (await runImportScript()) await importStagedCookies();
  }
  createWindow();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  app.quit();
});
