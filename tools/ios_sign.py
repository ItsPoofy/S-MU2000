#!/usr/bin/env python3
# license:BSD-3-Clause
#
# iOS signing, answered: one team id in, the rest found.
#
# The problem this solves is that a bundle id belongs to a team. The ids in
# packaging/*.plist are the upstream author's, so nobody else can sign them: a
# device install fails on the application-identifier mismatch, and the error
# ("provisioning profile ... doesn't include signing certificate ...") says
# nothing about which id went wrong. So a third-party build needs its own ids,
# and the two rules that make them work are Apple's, not ours:
#
#   - the AUv3's id must *prefix-extend* the app's, so one base and a suffix;
#   - both must be registered to the team, which means the developer portal (or
#     Xcode) has to know them before a profile exists.
#
# Given a team, this prints what everything else needs, and the Makefile reads it
# with $(shell): the derived ids, the signing identity whose organizational unit
# is that team, and the provisioning profiles that match. Nothing here edits the
# tree - the ids land in the *copied* Info.plists inside the build directory - so
# a build never leaves the checkout changed.
#
#   ios_sign.py team        the team id, or "" if none is set
#   ios_sign.py app-id      the app's bundle id
#   ios_sign.py appex-id    the AUv3's bundle id (prefix-extends the app's)
#   ios_sign.py identity    a signing identity for the team, or "" if none
#   ios_sign.py profile APP|APPEX   path of the matching profile, or ""
#   ios_sign.py report      all of the above, for `make ios-sign-info`
#
# The team comes from, in order: --team, the environment, and a .ios-team-id file
# in the checkout root (git-ignored, so it is written once and then forgotten).
#
# Only the device build needs any of this. The simulator signs ad-hoc and needs
# no profile, no identity and no team, which is why every query degrades to ""
# instead of complaining.

import argparse
import os
import plistlib
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The ids as shipped. Without a team they are what the plists say, so a build
# that has its own profile for them keeps working untouched.
APP_PLIST = os.path.join(ROOT, "packaging", "ios-app-Info.plist")
APPEX_PLIST = os.path.join(ROOT, "packaging", "auv3-ios-appex-Info.plist")

# A team id is 10 uppercase alphanumerics - letters A-Z and digits, not hex
# (7H4K2P9XTZ and ABCDE12345 are the two shapes you see in the wild, and a check
# that demanded hex would refuse real ones). Checked because a typo here becomes
# a profile search that silently finds nothing.
TEAM_RE = re.compile(r"^[A-Z0-9]{10}$")

# Two locations: Xcode 27 keeps managed profiles under UserData, older Xcode (and
# manual downloads from the portal) under MobileDevice. Both are searched.
PROFILE_DIRS = [
    "~/Library/Developer/Xcode/UserData/Provisioning Profiles",
    "~/Library/MobileDevice/Provisioning Profiles",
]


def plist_id(path):
    with open(path, "rb") as f:
        return plistlib.load(f).get("CFBundleIdentifier", "")


def team_from_file():
    path = os.path.join(ROOT, ".ios-team-id")
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def team_id(explicit, complain=False):
    """The team, from the flag, the environment or the checkout's own file.

    A malformed team is dropped rather than searched for - it is a typo, and
    searching with it would find nothing and say nothing about why. The complaint
    is opt-in because the Makefile asks for four values per invocation and a
    warning per answer would print it four times.
    """
    team = (explicit or os.environ.get("SMU2000_IOS_TEAM_ID") or team_from_file()).strip()
    if team and not TEAM_RE.match(team):
        if complain:
            print(f"ios: '{team}' is not a team id (10 uppercase letters and "
                  f"digits, as in ABCDE12345)", file=sys.stderr)
        return ""
    return team


def app_id(team):
    """The app's id: upstream's when there is no team, ours derived when there is.

    The team id goes in the middle rather than at the end so the AUv3 can extend
    it, which is the rule Apple's extension-target check enforces.
    """
    base = plist_id(APP_PLIST)
    if not team:
        return base
    return re.sub(r"\.app$", f".{team}", base)


def appex_id(team):
    base = plist_id(APPEX_PLIST)
    if not team:
        return base
    return re.sub(r"\.app\.auv3$", f".{team}.auv3", base)


def identities():
    """(team, name, sha1) for every code-signing identity in the keychain.

    The hash is carried because a team can hold two certificates with the same
    common name - revoking and reissuing leaves both - and codesign -s matches
    on the name, so which one wins is not something the person can see.
    """
    try:
        out = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    found = []
    for line in out.splitlines():
        # '  1) SHA1HEX "Apple Development: A Developer (7H4K2P9XTZ)"' - the
        # leading hash is the certificate, the team is the parenthesised one, and
        # both are [A-Z0-9], not hex.
        m = re.search(r'\) ([0-9A-F]{40}) "((.+)\(([A-Z0-9]{10})\))"', line)
        if m:
            # The whole string is what codesign --sign wants; the team is only
            # there to match on.
            found.append((m.group(4), m.group(2), m.group(1)))
    return found


def identity(team):
    """A signing identity for this team.

    Matching on the identity's own team rather than on a name, because the name
    has the developer's name in it and two of them can be spelled differently
    while the team is the same 10 characters.
    """
    if not team:
        return ""
    for ident_team, name, _sha1 in identities():
        if ident_team == team:
            return name
    return ""


def profiles(bundle_id, team=""):
    """Every profile *of this team* whose application-identifier covers this
    bundle id, newest first.

    Both halves matter. The identifier is TEAM.something, so a wildcard profile
    (TEAM.*) is what free provisioning hands out and covers any id - but only for
    its own team: matching the "*" alone finds another team's profile, which then
    fails at install with an error that names neither the team nor the id. Matching
    is on the profile's own entitlements, not its file name, so a renamed or
    re-downloaded profile is still found.
    """
    if not bundle_id or not team:
        # Without a team there is nothing a profile could be for: the ids belong
        # to whoever registered them, and embedding their profile next to an
        # ad-hoc signature is worse than embedding none - it turns an install that
        # could work into one that cannot. IOS_PROFILE still overrides.
        return []
    found = []
    for directory in PROFILE_DIRS:
        base = os.path.expanduser(directory)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if not name.endswith((".mobileprovision", ".provisionprofile")):
                continue
            path = os.path.join(base, name)
            try:
                # The profile is CMS-signed; security unwraps it to a plist.
                raw = subprocess.run(["security", "cms", "-D", "-i", path],
                                     capture_output=True, timeout=30).stdout
                info = plistlib.loads(raw)
            except (OSError, ValueError, subprocess.SubprocessError):
                continue
            ent = info.get("Entitlements", {})
            allowed = ent.get("application-identifier", "")
            prof_team, _, prof_id = allowed.partition(".")
            if team and prof_team != team:
                continue          # another team's profile: never ours to embed
            if prof_id not in ("*", bundle_id):
                continue
            found.append((info.get("CreationDate", ""), path, allowed,
                          info.get("Name", os.path.basename(path)),
                          info.get("ExpirationDate", "")))
    # Newest first: a re-downloaded profile should win over the one it replaced.
    found.sort(key=lambda f: f[0], reverse=True)
    return found


def profile(team, which):
    bid = app_id(team) if which == "APP" else appex_id(team)
    found = profiles(bid, team) if team else []
    return found[0][1] if found else ""


def report(team):
    the_app = app_id(team)
    the_appex = appex_id(team)
    sign = identity(team)
    print("iOS signing")
    print(f"  team         {team or '(not set - the simulator build needs none)'}")
    print(f"  app id       {the_app}")
    print(f"  appex id     {the_appex}")
    if not team:
        print("\nSet one with:  make ios-app TEAM_ID=ABCDE12345")
        print("or write it once into .ios-team-id (git-ignored), or export")
        print("SMU2000_IOS_TEAM_ID. Then register both ids (Xcode, or the")
        print("developer portal) so a profile exists for them.")
        return 0
    mine = [n for tm, n, _s in identities() if tm == team]
    print(f"  identity     {sign or '(none in the keychain for this team)'}"
          + (f"  [{len(mine)} certificates for this team; codesign takes the first]"
             if len(mine) > 1 else ""))
    if not sign:
        # A team with no identity is the most common reason for a device build to
        # fail, and the fix is in the keychain rather than in the Makefile - so
        # say which teams do have one, which is usually the answer.
        here = identities()
        if here:
            print()
            print("  teams with a signing identity on this machine:")
            for t, n, _s in here:
                print(f"    {t}  {n}")
        else:
            print("  (no code-signing identity at all - Xcode > Settings > Accounts)")
    for which, bid in (("app", the_app), ("appex", the_appex)):
        found = profiles(bid, team)
        if found:
            # The name is what Xcode called it ("iOS Team Provisioning Profile:
            # com.foo.bar"), which is recognisable; the file is a UUID.
            print(f"  {which} profile  {found[0][3]}")
            print(f"                 {found[0][2]}  expires {str(found[0][4])[:10]}"
                  if len(found[0]) > 4 else
                  f"                 {found[0][2]}")
        else:
            print(f"  {which} profile  (none for {bid} - Xcode, or the portal)")
    missing = []
    if not sign:
        missing.append("a signing identity for the team (Xcode > Settings > Accounts)")
    if not profiles(the_app, team):
        missing.append(f"a profile for {the_app}")
    if not profiles(the_appex, team):
        missing.append(f"a profile for {the_appex} (the extension target)")
    if missing:
        print("\nStill needed:")
        for what in missing:
            print(f"  - {what}")
        return 1
    print("\nReady:  make ios-app TEAM_ID=" + team)
    return 0


def main():
    ap = argparse.ArgumentParser(description="iOS signing: one team id in, the rest found")
    ap.add_argument("query", choices=["team", "app-id", "appex-id", "identity",
                                      "profile", "report"])
    ap.add_argument("--team", default="", help="the team id (10 hex characters)")
    ap.add_argument("--which", default="APP", choices=["APP", "APPEX"])
    args = ap.parse_args()

    # Only the two queries a person reads complain; the rest answer silently.
    team = team_id(args.team, complain=args.query in ("team", "report"))
    if args.query == "team":
        print(team)
    elif args.query == "app-id":
        print(app_id(team))
    elif args.query == "appex-id":
        print(appex_id(team))
    elif args.query == "identity":
        print(identity(team))
    elif args.query == "profile":
        print(profile(team, args.which))
    else:
        return report(team)
    return 0


if __name__ == "__main__":
    sys.exit(main())