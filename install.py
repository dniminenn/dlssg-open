#!/usr/bin/env python3
"""dlssg-open installer.

  ./install.py --appid 2215430            install for one Steam game (finds everything from Steam's config)
  ./install.py --appid 2215430 --undo     put the original Proton files back
  ./install.py --list                     show installed games with a DLSS-G runtime and its version
  ./install.py --proton DIR [--prefix DIR]  manual paths instead of --appid

Needs nvcuda.dll and nvapi64.dll next to this script (release tarball) or built in
nvcuda-lite/ and build/ (source tree). Only Python 3 is required to run this.
"""
import argparse, os, re, shutil, struct, sys

HOME = os.path.expanduser("~")
STEAM_ROOTS = [
    HOME + "/.steam/steam", HOME + "/.local/share/Steam", HOME + "/.steam/debian-installation",
    HOME + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
]
ENV_LINE = "DXVK_NVAPI_GPU_ARCH=AD100 NVCUDA_LITE_CC=8.9 DXVK_NVAPI_CUBIN_RETARGET_SM=86 %command%"

def die(msg):
    print("error: " + msg); sys.exit(1)

def steam_root():
    for r in STEAM_ROOTS:
        if os.path.isfile(r + "/config/config.vdf"):
            return os.path.realpath(r)
    die("Steam not found. Use --proton and --prefix.")

def libraries(root):
    libs = [root]
    try:
        for m in re.finditer(r'"path"\s+"([^"]+)"', open(root + "/steamapps/libraryfolders.vdf").read()):
            p = m.group(1).replace("\\\\", "\\")
            if os.path.isdir(p) and p not in libs:
                libs.append(p)
    except OSError:
        pass
    return libs

def compat_tool(root, appid):
    """Name of the Proton tool mapped to appid, or the default ("0"), or None."""
    try:
        s = open(root + "/config/config.vdf").read()
    except OSError:
        return None
    m = re.search(r'"CompatToolMapping"\s*\{(.*?)\n\t\t\t\t\}', s, re.S)
    if not m:
        return None
    tools = dict(re.findall(r'"(\d+)"\s*\{\s*"name"\s*"([^"]*)"', m.group(1)))
    return tools.get(str(appid)) or tools.get("0")

def proton_dir(libs, tool):
    names = {"proton_experimental": "Proton - Experimental", "proton_hotfix": "Proton Hotfix"}
    cand = []
    if tool in names:
        cand.append(names[tool])
    elif tool and tool.startswith("proton_"):
        cand.append("Proton " + tool[7:].replace("_", "."))
    cand += ["Proton - Experimental", "Proton Hotfix"]
    for lib in libs:
        for n in cand:
            d = lib + "/steamapps/common/" + n
            if os.path.isdir(d + "/files/lib/wine"):
                return d
    for lib in libs:   # anything Proton-like
        base = lib + "/steamapps/common"
        if os.path.isdir(base):
            for n in sorted(os.listdir(base)):
                if n.lower().startswith("proton") and os.path.isdir(base + "/" + n + "/files/lib/wine"):
                    return base + "/" + n
    return None

def game_info(libs, appid):
    for lib in libs:
        acf = "%s/steamapps/appmanifest_%d.acf" % (lib, appid)
        if os.path.isfile(acf):
            s = open(acf).read()
            name = re.search(r'"name"\s+"([^"]*)"', s); inst = re.search(r'"installdir"\s+"([^"]*)"', s)
            return lib, name.group(1) if name else str(appid), lib + "/steamapps/common/" + (inst.group(1) if inst else "")
    return None, None, None

def pe_version(path):
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    m = re.search(rb'F\x00i\x00l\x00e\x00V\x00e\x00r\x00s\x00i\x00o\x00n\x00\x00\x00(?:\x00\x00)?((?:[0-9.,]\x00)+)', d)
    return m.group(1).decode("utf-16le").replace(",", ".") if m else None

def find_runtime(gamedir):
    hits = []
    for base, dirs, files in os.walk(gamedir):
        if "nvngx_dlssg.dll" in files:
            hits.append(base + "/nvngx_dlssg.dll")
        if base.count(os.sep) - gamedir.count(os.sep) > 3:
            dirs[:] = []
    return hits

def payload(here):
    for nv, na in [(here + "/nvcuda.dll", here + "/nvapi64.dll"), (here + "/nvcuda-lite/nvcuda.dll", here + "/build/nvapi64.dll")]:
        if os.path.isfile(nv) and os.path.isfile(na):
            return nv, na
    die("nvcuda.dll / nvapi64.dll not found. Use the release tarball, or build: make -C nvcuda-lite && ./build-dxvk-nvapi.sh")

def replace(dst, src, backup):
    if not os.path.exists(backup) and os.path.lexists(dst):
        os.rename(dst, backup)
    elif os.path.lexists(dst):
        os.remove(dst)
    shutil.copyfile(src, dst); os.chmod(dst, 0o755)
    print("  installed " + dst)

def restore(dst, backup):
    if os.path.exists(backup):
        if os.path.lexists(dst):
            os.remove(dst)
        os.rename(backup, dst); print("  restored " + dst)

def install(proton, prefix, undo, nv, na):
    w = proton + "/files/lib/wine"
    if not os.path.isdir(w):
        die("not a Proton directory: " + proton)
    pairs = [(w + "/x86_64-windows/nvcuda.dll", w + "/x86_64-windows/nvcuda.dll.proton-stub", nv),
             (w + "/nvapi/x86_64-windows/nvapi64.dll", w + "/nvapi/x86_64-windows/nvapi64.dll.proton-orig", na)]
    if prefix:
        s = prefix + "/drive_c/windows/system32"
        if not os.path.isdir(s):
            die("not a Wine prefix: " + prefix)
        pairs.append((s + "/nvapi64.dll", s + "/nvapi64.dll.proton-orig", na))
    print(("Restoring " if undo else "Installing into ") + proton)
    for dst, backup, src in pairs:
        restore(dst, backup) if undo else replace(dst, src, backup)

def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--appid", type=int); ap.add_argument("--proton"); ap.add_argument("--prefix")
    ap.add_argument("--undo", action="store_true"); ap.add_argument("--list", action="store_true")
    ap.add_argument("-h", "--help", action="store_true")
    a = ap.parse_args()
    if a.help or not (a.appid or a.proton or a.list):
        print(__doc__); return
    here = os.path.dirname(os.path.realpath(__file__))
    if a.list:
        root = steam_root(); libs = libraries(root)
        for lib in libs:
            for f in sorted(os.listdir(lib + "/steamapps")) if os.path.isdir(lib + "/steamapps") else []:
                m = re.match(r"appmanifest_(\d+)\.acf", f)
                if not m: continue
                appid = int(m.group(1)); _, name, gamedir = game_info([lib], appid)
                if not gamedir or not os.path.isdir(gamedir): continue
                for rt in find_runtime(gamedir):
                    v = pe_version(rt) or "?"
                    print("%-10d %-45s DLSS-G %s %s" % (appid, name[:45], v, "" if v.startswith("310") else "(needs 310.x)"))
        return
    nv, na = payload(here)
    if a.appid:
        root = steam_root(); libs = libraries(root)
        lib, name, gamedir = game_info(libs, a.appid)
        if not lib:
            die("app %d is not installed in any Steam library" % a.appid)
        prefix = lib + "/steamapps/compatdata/%d/pfx" % a.appid
        if not os.path.isdir(prefix + "/drive_c"):
            die("no Proton prefix for %s yet. Launch the game once with Proton, then rerun." % name)
        tool = compat_tool(root, a.appid)
        proton = a.proton or proton_dir(libs, tool)
        if not proton:
            die("could not find the Proton build for %s (tool: %s). Pass --proton." % (name, tool))
        print("Game:   %s (%d)\nProton: %s\nPrefix: %s" % (name, a.appid, proton, prefix))
        for rt in find_runtime(gamedir):
            v = pe_version(rt) or "unknown"
            print("DLSS-G runtime: %s (%s)" % (v, rt))
            if not v.startswith("310"):
                print("  WARNING: only 310.x runtimes work. Replace this file with a 310.x nvngx_dlssg.dll from another game.")
        install(proton, prefix, a.undo, nv, na)
    else:
        install(a.proton, a.prefix, a.undo, nv, na)
    if a.undo:
        print("Done. Remove the launch options from the game's properties.")
    else:
        print("\nDone. In Steam, right-click the game > Properties > Launch Options, paste:\n\n  " + ENV_LINE +
              "\n\nThen enable DLSS and Frame Generation in the game's graphics settings."
              "\nProton updates overwrite these files. Rerun this installer if frame generation stops appearing.")

if __name__ == "__main__":
    main()
