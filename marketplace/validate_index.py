#!/usr/bin/env python3
"""Validates marketplace/index.json (schemaVersion 1) for HB Studio.

Usage:
    python3 marketplace/validate_index.py [index.json] [--no-network] [--strict-network]

Checks (the same rules as the C++ parser in src/marketplace/catalog.cpp, which is what the app runs):
  * structure, types, lengths, charsets; ids lowercase/unique; https-only URLs without userinfo/IP literals;
  * category/type/permissions consistency (native code must be declared; themes never run code);
  * sha256 is 64 lowercase hex, size is 1..64 MiB (templates: <= 1 MiB);
  * for every download URL: if the file is reachable, its SHA-256 and size MUST match the index; and the
    package content must agree with the entry (manifest id/type/version inside the .hbplugin zip, or
    "format": "hb-templates" for templates). Unreachable URLs are only a warning (--strict-network: error),
    because a PR adds its entry before the files exist at their final URL;
  * if the URL points at this repository's marketplace/packages/<file> and that file exists locally, it is
    checked locally (no network needed).

Exit code 0 = valid, 1 = errors. Only the Python standard library is used.
"""
import hashlib
import io
import json
import os
import re
import sys
import urllib.error
import urllib.request
import zipfile

MAX_PACKAGE = 64 * 1024 * 1024
MAX_TEMPLATE = 1024 * 1024
CATEGORIES = {"theme", "video-source", "audio-effect", "graphic", "template"}
TYPES = {"theme", "source", "effect", "template"}
PERMISSIONS = {"native-code", "network", "filesystem", "hardware-capture"}
RESERVED = {"con", "prn", "aux", "nul"} | {f"com{i}" for i in range(1, 10)} | {f"lpt{i}" for i in range(1, 10)}
SEMVER = re.compile(r"^\d{1,6}\.\d{1,6}\.\d{1,6}(-[0-9A-Za-z.-]{1,32})?(\+[0-9A-Za-z.-]{1,32})?$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")

errors, warnings = [], []


def err(where, msg):
    errors.append(f"{where}: {msg}")


def warn(where, msg):
    warnings.append(f"{where}: {msg}")


def has_control(s):
    return any(ord(c) < 0x20 or ord(c) == 0x7F for c in s)


def safe_https_url(u):
    if not isinstance(u, str) or len(u) > 2048 or not u.lower().startswith("https://") or len(u) <= 8:
        return False
    if any(ord(c) <= 0x20 or ord(c) >= 0x7F or c in '\\"<>`' for c in u):
        return False
    authority = re.split(r"[/?#]", u[8:], maxsplit=1)[0]
    if not authority or "@" in authority or "[" in authority:
        return False
    host, _, port = authority.partition(":")
    if ":" in authority:
        if not port.isdigit() or len(port) > 5 or not (1 <= int(port) <= 65535):
            return False
    if not host or "." not in host or len(host) > 253 or re.fullmatch(r"[0-9.]+", host):
        return False
    if not all(re.fullmatch(r"[A-Za-z0-9-]{1,63}", lab) for lab in host.split(".")):
        return False
    return not host.lower().endswith((".local", ".localhost", ".internal", ".lan"))


def text(obj, key, where, maxlen, required=False):
    v = obj.get(key)
    if v is None:
        if required:
            err(where, f"missing field '{key}'")
        return None
    if not isinstance(v, str) or len(v) > maxlen or (required and not v) or has_control(v):
        err(where, f"invalid value in '{key}' (string up to {maxlen} chars, no control characters)")
        return None
    return v


def valid_id(i):
    if not isinstance(i, str) or not (3 <= len(i) <= 64):
        return False
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]*[a-z0-9]", i) or ".." in i:
        return False
    return i.split(".")[0] not in RESERVED


def check_entry(e, n, ids):
    where = f"plugins[{n}]"
    if not isinstance(e, dict):
        err(where, "entry is not an object")
        return None
    pid = e.get("id")
    if not valid_id(pid):
        err(where, "invalid id (3-64 chars, lowercase letters/digits/./-/_, reverse-domain recommended)")
        return None
    where = f"plugins[{n}] ({pid})"
    if pid in ids:
        err(where, "duplicate id")
    ids.add(pid)
    text(e, "name", where, 80, True)
    text(e, "author", where, 80, True)
    text(e, "license", where, 64, True)
    desc = e.get("description")
    if not isinstance(desc, dict) or not any(isinstance(desc.get(k), str) and desc.get(k) for k in ("pt-BR", "en")):
        err(where, "description must be an object like {\"pt-BR\": \"...\", \"en\": \"...\"} with at least one text")
    else:
        for k in ("pt-BR", "en"):
            if k in desc:
                text(desc, k, where, 600)
    cat, typ = e.get("category"), e.get("type")
    if cat not in CATEGORIES:
        err(where, f"category must be one of {sorted(CATEGORIES)}")
    if typ not in TYPES:
        err(where, f"type must be one of {sorted(TYPES)}")
    if cat in CATEGORIES and typ in TYPES:
        if (cat == "theme" and typ != "theme") or ((cat == "template") != (typ == "template")) or \
           (cat == "video-source" and typ != "source") or (cat == "audio-effect" and typ != "effect"):
            err(where, f"category '{cat}' is incompatible with type '{typ}'")
    tags = e.get("tags", [])
    if not isinstance(tags, list) or len(tags) > 8 or not all(isinstance(t, str) and 0 < len(t) <= 24 and not has_control(t) for t in tags):
        err(where, "tags must be up to 8 strings of up to 24 chars")
    for k in ("homepage", "repository"):
        if k in e and not (isinstance(e[k], str) and (e[k] == "" or safe_https_url(e[k]))):
            err(where, f"'{k}' must be an https URL")
    if "verified" in e and not isinstance(e["verified"], bool):
        err(where, "'verified' must be true/false")
    shots = e.get("screenshots", [])
    if not isinstance(shots, list) or len(shots) > 6 or not all(safe_https_url(s) for s in shots):
        err(where, "screenshots must be up to 6 https URLs")
    perms = e.get("permissions")
    if not isinstance(perms, list) or len(perms) > 8 or not all(p in PERMISSIONS for p in perms):
        err(where, f"permissions is required: a list with values from {sorted(PERMISSIONS)}")
    elif typ in TYPES and (("native-code" in perms) != (typ in ("source", "effect"))):
        err(where, "permissions must contain 'native-code' exactly when type is 'source' or 'effect'")
    versions = e.get("versions")
    if not isinstance(versions, list) or not (1 <= len(versions) <= 50):
        err(where, "versions must have 1 to 50 items")
        return None
    seen = set()
    for j, v in enumerate(versions):
        check_version(v, f"{where} versions[{j}]", typ, pid, seen)
    return e


def check_version(v, where, typ, pid, seen):
    if not isinstance(v, dict):
        err(where, "version is not an object")
        return
    ver = v.get("version")
    if not isinstance(ver, str) or not SEMVER.match(ver):
        err(where, "version must be semver MAJOR.MINOR.PATCH")
        return
    if ver in seen:
        err(where, f"duplicate version {ver}")
    seen.add(ver)
    mn = v.get("minAppVersion", "0.0.0")
    if not isinstance(mn, str) or not SEMVER.match(mn):
        err(where, "minAppVersion must be semver")
    plats = v.get("platforms")
    if not isinstance(plats, list) or not (1 <= len(plats) <= 8) or not all(isinstance(p, str) and re.fullmatch(r"[a-z0-9-]{3,24}", p) for p in plats):
        err(where, "platforms must be 1-8 ids like 'windows-x64'")
    urls = v.get("downloadUrls")
    if not isinstance(urls, list) or not (1 <= len(urls) <= 4) or not all(safe_https_url(u) for u in urls):
        err(where, "downloadUrls must be 1-4 https URLs (no userinfo, no IP addresses/localhost)")
        urls = []
    sha, size = v.get("sha256"), v.get("size")
    if not isinstance(sha, str) or not HEX64.match(sha):
        err(where, "sha256 must be 64 lowercase hex characters")
        sha = None
    cap = MAX_TEMPLATE if typ == "template" else MAX_PACKAGE
    if not isinstance(size, int) or isinstance(size, bool) or not (1 <= size <= cap):
        err(where, f"size must be an integer between 1 and {cap} bytes")
        size = None
    for k, mx in (("releasedAt", 32), ("notes", 300)):
        if k in v:
            text(v, k, where, mx)
    if sha and size:
        for u in urls:
            verify_download(u, sha, size, typ, pid, ver, where)


REPO_PKG = re.compile(r"/marketplace/packages/([^/?#]+)$")
HERE = os.path.dirname(os.path.abspath(__file__))
OPTIONS = {"network": True, "strict": False}


def fetch(u):
    req = urllib.request.Request(u, headers={"User-Agent": "hbstudio-marketplace-validator"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read(MAX_PACKAGE + 1)
    return data


def verify_download(u, sha, size, typ, pid, ver, where):
    data = None
    m = REPO_PKG.search(u.split("?")[0])
    if m:
        local = os.path.join(HERE, "packages", m.group(1))
        if os.path.isfile(local):
            with open(local, "rb") as f:
                data = f.read()
    if data is None and OPTIONS["network"]:
        try:
            data = fetch(u)
        except (urllib.error.URLError, OSError, ValueError) as ex:
            (err if OPTIONS["strict"] else warn)(where, f"could not download {u} ({ex}); hash not verified")
            return
    if data is None:
        return
    if len(data) != size:
        err(where, f"{u}: size is {len(data)} bytes but the index says {size}")
    digest = hashlib.sha256(data).hexdigest()
    if digest != sha:
        err(where, f"{u}: SHA-256 is {digest} but the index says {sha}")
        return
    check_content(data, typ, pid, ver, f"{where} {u}")


def check_content(data, typ, pid, ver, where):
    if typ == "template":
        try:
            doc = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            err(where, "template file is not valid JSON")
            return
        if not isinstance(doc, dict) or doc.get("format") != "hb-templates" or doc.get("version") != 1:
            err(where, "template file must have \"format\": \"hb-templates\" and \"version\": 1")
        return
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            manifest = json.loads(z.read("manifest.json").decode("utf-8"))
    except (zipfile.BadZipFile, KeyError, UnicodeDecodeError, ValueError):
        err(where, "package is not a valid .hbplugin (zip with manifest.json)")
        return
    if manifest.get("id") != pid:
        err(where, f"manifest id '{manifest.get('id')}' differs from the index id '{pid}'")
    if manifest.get("type") != typ:
        err(where, f"manifest type '{manifest.get('type')}' differs from the index type '{typ}'")
    if manifest.get("version") != ver:
        err(where, f"manifest version '{manifest.get('version')}' differs from the index version '{ver}'")


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    OPTIONS["network"] = "--no-network" not in argv
    OPTIONS["strict"] = "--strict-network" in argv
    path = args[0] if args else os.path.join(HERE, "index.json")
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as ex:
        print(f"ERROR: cannot read {path}: {ex}")
        return 1
    if len(raw) > 2 * 1024 * 1024:
        print("ERROR: index.json is larger than 2 MiB")
        return 1
    try:
        doc = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as ex:
        print(f"ERROR: invalid JSON: {ex}")
        return 1
    if not isinstance(doc, dict):
        print("ERROR: the document must be a JSON object")
        return 1
    if doc.get("schemaVersion") != 1 or isinstance(doc.get("schemaVersion"), bool):
        err("root", "schemaVersion must be 1")
    plugins = doc.get("plugins")
    if not isinstance(plugins, list) or len(plugins) > 500:
        err("root", "plugins must be a list with at most 500 entries")
        plugins = []
    ids = set()
    for n, e in enumerate(plugins):
        check_entry(e, n, ids)
    for w in warnings:
        print("WARNING:", w)
    for e in errors:
        print("ERROR:", e)
    if errors:
        print(f"\n{len(errors)} error(s). The app drops invalid entries, so fix them before merging.")
        return 1
    print(f"OK: {len(plugins)} plugin(s), {len(warnings)} warning(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
