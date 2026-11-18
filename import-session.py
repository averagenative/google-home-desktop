#!/usr/bin/env python3
"""Copy the Google session cookies from Firefox into the wrapper.

Google's sign-in blocks embedded browsers outright (BotGuard fingerprints
the JS runtime, so no user-agent spoof gets past it). Instead: sign in to
home.google.com in real Firefox, then run this script. It dumps your
*.google.com cookies to a private staging file that the app imports and
deletes on its next launch.
"""

import configparser
import json
import shutil
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

FIREFOX_DIR = Path.home() / ".mozilla" / "firefox"
STAGING = Path.home() / ".config" / "google-home-desktop" / "import-cookies.json"

SAMESITE = {0: "no_restriction", 1: "lax", 2: "strict"}  # 256 = unset


def to_seconds(expiry):
    # Newer Firefox builds store expiry in milliseconds; older ones used
    # seconds. Electron wants seconds.
    return expiry / 1000 if expiry > 10**11 else expiry


def find_profile():
    if len(sys.argv) > 1:
        return Path(sys.argv[1])
    ini = configparser.ConfigParser()
    ini.read(FIREFOX_DIR / "profiles.ini")
    candidates = [
        FIREFOX_DIR / s["Path"]
        for s in ini.values()
        if "Path" in s
    ]
    with_cookies = [p for p in candidates if (p / "cookies.sqlite").exists()]
    if not with_cookies:
        sys.exit("No Firefox profile with cookies.sqlite found")
    return max(with_cookies, key=lambda p: (p / "cookies.sqlite").stat().st_mtime)


def main():
    profile = find_profile()
    db = profile / "cookies.sqlite"
    print(f"Reading {db}")

    with tempfile.NamedTemporaryFile(suffix=".sqlite") as tmp:
        shutil.copyfile(db, tmp.name)
        con = sqlite3.connect(tmp.name)
        # originAttributes = '' keeps only first-party, non-container,
        # non-private cookies. Partitioned (CHIPS) copies set while
        # browsing other sites would otherwise overwrite the real ones.
        rows = con.execute(
            "SELECT host, path, name, value, isSecure, isHttpOnly, expiry, sameSite"
            " FROM moz_cookies WHERE host LIKE '%google.com'"
            " AND originAttributes = ''"
        ).fetchall()
        (last_used,) = con.execute(
            "SELECT MAX(lastAccessed) FROM moz_cookies"
            " WHERE host = '.google.com' AND name = '__Secure-1PSIDTS'"
            " AND originAttributes = ''"
        ).fetchone()
        con.close()

    if not rows:
        sys.exit("No google.com cookies in that profile — sign in with Firefox first")

    cookies = [
        {
            "host": host,
            "path": path,
            "name": name,
            "value": value,
            "secure": bool(secure),
            "httpOnly": bool(http_only),
            "expirationDate": to_seconds(expiry),
            "sameSite": SAMESITE.get(same_site, "unspecified"),
        }
        for host, path, name, value, secure, http_only, expiry, same_site in rows
    ]

    names = {c["name"] for c in cookies}
    if "SID" not in names and "__Secure-1PSID" not in names:
        sys.exit("Found google.com cookies but no session (SID) — sign in with Firefox first")

    if last_used:
        age_days = (time.time() - last_used / 1e6) / 86400
        if age_days > 1:
            print(
                f"Warning: Firefox last used this Google session {age_days:.0f} days ago."
                " If the app lands on a sign-in or cookie error page, open"
                " https://home.google.com in Firefox first, then import again."
            )

    STAGING.parent.mkdir(parents=True, exist_ok=True)
    STAGING.write_text(json.dumps(cookies))
    STAGING.chmod(0o600)
    print(f"Staged {len(cookies)} cookies -> {STAGING}")
    print("Launch Google Home now; the app imports and deletes this file.")


if __name__ == "__main__":
    main()
