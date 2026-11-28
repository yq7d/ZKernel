#!/usr/bin/env python3
# builds the read only apps image that rides along in the iso: alpine packages
# and their dependencies unpacked into one tree, then made into ext2.
#
#   mkapps.py out.img cache_dir pkg...
#
# downloads go to cache_dir and are reused. runs mke2fs under fakeroot so
# everything belongs to root
import gzip
import io
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

MIRROR = "http://dl-cdn.alpinelinux.org/alpine/v3.24"
REPOS = ["main", "community"]
ARCH = "x86_64"
# hardware gl: llvm and the gallium drivers are 225 MB that only libGL/libEGL
# want, and those are only ever dlopened. without them programs fall back to
# software rendering
SKIP_PKGS = {"llvm22-libs"}
SKIP_FILES = ("usr/lib/libgallium",)


def fetch(url, path, max_age=None):
    if os.path.exists(path) and (max_age is None or time.time() - os.path.getmtime(path) < max_age):
        return
    tmp = path + ".part"
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
            os.replace(tmp, path)
            return
        except Exception as e:
            if attempt == 2:
                sys.exit(f"mkapps: {url}: {e}")
            time.sleep(2)


def load_index(cache):
    pkgs, providers = {}, {}
    for repo in REPOS:
        path = os.path.join(cache, f"APKINDEX-{repo}.tar.gz")
        fetch(f"{MIRROR}/{repo}/{ARCH}/APKINDEX.tar.gz", path, max_age=86400)
        with tarfile.open(path) as t:
            text = t.extractfile("APKINDEX").read().decode()
        for block in text.split("\n\n"):
            p = {"repo": repo, "deps": [], "provides": []}
            for line in block.splitlines():
                if len(line) < 2 or line[1] != ":":
                    continue
                k, v = line[0], line[2:]
                if k == "P":
                    p["name"] = v
                elif k == "V":
                    p["version"] = v
                elif k == "S":
                    p["size"] = int(v)
                elif k == "D":
                    p["deps"] = v.split()
                elif k == "p":
                    p["provides"] = v.split()
            if "name" not in p or p["name"] in pkgs:
                continue
            pkgs[p["name"]] = p
            for prov in [p["name"]] + p["provides"]:
                providers.setdefault(strip_ver(prov), []).append(p["name"])
    return pkgs, providers


def strip_ver(dep):
    for op in (">=", "<=", "~", "=", ">", "<"):
        i = dep.find(op)
        if i > 0:
            return dep[:i]
    return dep


def resolve(want, pkgs, providers):
    order, seen = [], set()
    todo = list(want)
    while todo:
        dep = todo.pop(0)
        if dep.startswith("!"):
            continue
        name = strip_ver(dep)
        cands = providers.get(name)
        if not cands:
            print(f"mkapps: nothing provides {name}, skipped", file=sys.stderr)
            continue
        pick = name if name in cands else cands[0]
        if pick in seen or pick in SKIP_PKGS:
            continue
        seen.add(pick)
        order.append(pick)
        todo.extend(pkgs[pick]["deps"])
    return order


def extract(apk, root):
    with open(apk, "rb") as f:
        data = gzip.decompress(f.read())    # all the members back to back
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:", ignore_zeros=True) as t:
        for m in t:
            if m.name.startswith(".") or m.name.startswith(SKIP_FILES):
                continue    # .PKGINFO, .SIGN, scripts
            try:
                t.extract(m, root, set_attrs=False, filter="fully_trusted")
            except (OSError, tarfile.TarError) as e:
                if not os.path.lexists(os.path.join(root, m.name)):
                    print(f"mkapps: {m.name}: {e}", file=sys.stderr)
                continue
            if m.isfile() or m.isdir():
                os.chmod(os.path.join(root, m.name), m.mode & 0o7777)


def main():
    out, cache, want = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(cache, exist_ok=True)
    pkgs, providers = load_index(cache)
    names = resolve(want, pkgs, providers)
    print(f"mkapps: {len(names)} packages")

    root = out + ".root"
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    for n in names:
        p = pkgs[n]
        file = f"{n}-{p['version']}.apk"
        path = os.path.join(cache, file)
        if not (os.path.exists(path) and os.path.getsize(path) == p.get("size", -1)):
            print(f"mkapps: getting {file}")
            fetch(f"{MIRROR}/{p['repo']}/{ARCH}/{file}", path)
        extract(path, root)

    # what install scripts and triggers would have done
    applets = os.path.join(root, "etc/busybox-paths.d/busybox")
    if os.path.exists(applets):
        for line in open(applets).read().split():
            link = os.path.join(root, line.lstrip("/"))
            if not os.path.lexists(link):
                os.makedirs(os.path.dirname(link), exist_ok=True)
                os.symlink("/bin/busybox", link)
    schemas = os.path.join(root, "usr/share/glib-2.0/schemas")
    if os.path.isdir(schemas) and shutil.which("glib-compile-schemas"):
        subprocess.run(["glib-compile-schemas", schemas], check=False)
    for d in ("tmp", "var/tmp", "var/cache/fontconfig", "var/lib/xkb"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    # there's no font cache (it would take running alpine's fc-cache), so
    # fontconfig opens every font on each start. keep it off the hundreds of
    # bitmap fonts only the x server uses, leaving it the few ttf ones
    if os.path.isdir(os.path.join(root, "etc/fonts/conf.d")):
        with open(os.path.join(root, "etc/fonts/conf.d/05-zos-bitmaps.conf"), "w") as f:
            f.write('<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">\n'
                    "<fontconfig>\n  <selectfont>\n    <rejectfont>\n"
                    + "".join(f"      <glob>/usr/share/fonts/{d}/*</glob>\n"
                              for d in ("misc", "75dpi", "100dpi", "cyrillic", "encodings"))
                    + "    </rejectfont>\n  </selectfont>\n</fontconfig>\n")

    used = int(subprocess.check_output(["du", "-sk", root]).split()[0])
    files = sum(len(f) + len(d) for _, d, f in os.walk(root))
    size_k = used * 106 // 100 + 8192
    if os.path.exists(out):
        os.remove(out)
    script = (f"chown -R 0:0 '{root}' && mke2fs -q -F -t ext2 -O ^dir_index,^resize_inode "
              f"-b 4096 -N {files + 4096} -L ZOSAPPS -d '{root}' '{out}' {size_k}k")
    subprocess.run(["fakeroot", "sh", "-c", script], check=True)
    shutil.rmtree(root, ignore_errors=True)
    print(f"mkapps: {out}, {size_k // 1024} MiB")


if __name__ == "__main__":
    main()
