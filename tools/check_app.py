#!/usr/bin/env python3
"""kit check: what an app has to pass before it is submitted, here and in its repository's CI.

  check_app.py <kit> <app>

The app's manifest (the main rules of Yaykit's contract; the review checks it against the contract itself), its version
and project name, the kit untouched at a version, everything committed, the release build there and of this version,
without development commands and at most 3.5 MB, the host tests, and nothing in the repository that looks like a key,
a token or a round screen's id. Standard library only: it runs before ESP-IDF is installed too.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

SLOT_MAX = 3670016
SCENES = {"party", "learning", "relax", "language", "focus", "tools"}
AUDIENCES = {"kids", "adults", "all"}
LANGS = {"zh", "en", "ja", "es-419"}
KEYS = {
    "slug", "kind", "version", "name", "icon", "iconLarge", "boards", "layout", "minYay", "apiLevel", "minFirmware",
    "capabilities", "launch", "aliases", "scenes", "audience", "records", "settings", "settingOptions", "settingLabels",
    "settingRules", "settingsScope", "ephemeral", "pricing",
}
REQUIRED = {"slug", "kind", "version", "name", "boards", "layout", "capabilities", "aliases", "scenes", "audience", "pricing"}
FAKE_DEVICES = {"yk_00000000beef", "yk_0a1b2c3d4e5f", "yk_000000000000", "yk_0123456789ab"}
SECRETS = [
    (re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"), "私钥"),
    (re.compile(r"\b(gh[opusr])_[A-Za-z0-9]{20,}|\bgithub_pat_[A-Za-z0-9_]{20,}"), "GitHub 令牌"),
    (re.compile(r"\bsk-[A-Za-z0-9_-]{20,}"), "API 密钥"),
    (re.compile(r"\bAKIA[0-9A-Z]{16}\b"), "AWS 密钥"),
    (re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.eyJ[A-Za-z0-9_-]{8,}\."), "令牌（JWT）"),
    (re.compile(r"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])"), "圆屏的 USB 序列号"),
]

problems, notes = [], []


def bad(text):
    problems.append(text)


def version_tuple(v):
    return tuple(int(x) for x in re.match(r"^(\d+)\.(\d+)\.(\d+)", v).groups())


def git(cwd, *args):
    return subprocess.run(["git", "-c", "safe.directory=*", "-C", cwd, *args], capture_output=True, text=True)


def text_of(spec, field, maxlen):
    value = spec.get(field)
    if not isinstance(value, dict) or set(value) - LANGS or not {"zh", "en"} <= set(value):
        bad(f"app.json 的 {field} 要有 zh 和 en（可以再加 ja、es-419），不能有别的语言")
        return
    for lang, s in value.items():
        if not isinstance(s, str) or not s.strip() or len(s) > maxlen:
            bad(f"app.json 的 {field}.{lang} 要是 1–{maxlen} 个字符")


def phrases_of(where, value, items, maxlen):
    if not isinstance(value, dict) or set(value) - LANGS or not {"zh", "en"} <= set(value):
        bad(f"app.json 的 {where} 要有 zh 和 en 两个列表")
        return
    for lang, lst in value.items():
        if not isinstance(lst, list) or len(lst) > items or any(not isinstance(s, str) or not s or len(s) > maxlen for s in lst):
            bad(f"app.json 的 {where}.{lang} 最多 {items} 句，每句 1–{maxlen} 个字符")


def check_manifest(spec, kit):
    for key in sorted(set(spec) - KEYS):
        bad(f"app.json 里有不认识的字段 {key}")
    for key in sorted(REQUIRED - set(spec)):
        bad(f"app.json 少了 {key}")
    slug = spec.get("slug", "")
    if not re.match(r"^[a-z][a-z0-9-]{1,40}$", str(slug)) or slug in {"yay", "launcher", "recovery"}:
        bad("app.json 的 slug 只用小写字母、数字、连字符，字母开头，2–41 个字符，不能是 yay、launcher、recovery")
    if spec.get("kind") != "extension":
        bad('app.json 的 kind 要写 "extension"')
    if not re.match(r"^\d{1,4}\.\d{1,4}\.\d{1,4}$", str(spec.get("version", ""))):
        bad("app.json 的 version 要是 主.次.修，比如 0.1.0")
    text_of(spec, "name", 24)
    if "tikpal-r1" not in (spec.get("boards") or []):
        bad('app.json 的 boards 要有 "tikpal-r1"')
    if spec.get("layout") != "yaykit-v1":
        bad('app.json 的 layout 要写 "yaykit-v1"')
    min_yay = spec.get("minYay")
    if not min_yay or not re.match(r"^\d+\.\d+\.\d+$", str(min_yay)):
        bad(f'app.json 要写 minYay（至少 "{kit["yay"]}"）')
    elif version_tuple(min_yay) < version_tuple(kit["yay"]):
        bad(f'app.json 的 minYay 是 {min_yay}，比开发包对应的 Yay {kit["yay"]} 还旧')
    if spec.get("apiLevel") != kit["apiLevel"]:
        bad(f'app.json 的 apiLevel 要写开发包的 {kit["apiLevel"]}')
    if not isinstance(spec.get("capabilities"), dict):
        bad("app.json 的 capabilities 要是对象；不要云能力就写 {}")
    launch = spec.get("launch")
    if launch is not None:
        if not isinstance(launch, dict) or set(launch) - {"summary", "examples", "params"}:
            bad("app.json 的 launch 只有 summary、examples、params")
        else:
            text_of(launch, "summary", 300)
            phrases_of("launch.examples", launch.get("examples"), 12, 80)
            for name, p in (launch.get("params") or {}).items():
                if not isinstance(p, dict) or p.get("type") not in {"string", "number", "boolean"} or not p.get("description"):
                    bad(f"app.json 的 launch.params.{name} 要有 type（string、number、boolean）和 description")
    phrases_of("aliases", spec.get("aliases"), 8, 24)
    scenes = spec.get("scenes")
    if not isinstance(scenes, list) or not 1 <= len(scenes) <= 3 or set(scenes) - SCENES:
        bad("app.json 的 scenes 是 1–3 个：" + "、".join(sorted(SCENES)))
    if spec.get("audience") not in AUDIENCES:
        bad("app.json 的 audience 是 kids、adults 或 all")
    if spec.get("pricing") != {"kind": "free"}:
        bad('app.json 的 pricing 现在只能是 {"kind": "free"}')


def main():
    kit_dir, app = sys.argv[1], sys.argv[2]
    kit = json.load(open(os.path.join(kit_dir, "devkit.json")))
    try:
        spec = json.load(open(os.path.join(app, "app.json"), encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"✗ app.json 读不了：{e}")
        return 1
    check_manifest(spec, kit)
    slug, version = spec.get("slug", ""), spec.get("version", "")

    cmake = open(os.path.join(app, "CMakeLists.txt"), encoding="utf-8").read()
    project = re.search(r"project\((\w+)\)", cmake)
    if not project or project.group(1) != "yaykit_" + slug.replace("-", "_"):
        bad(f"CMakeLists.txt 的 project() 要是 yaykit_{slug.replace('-', '_')}")
    ver = re.search(r'set\(PROJECT_VER "([^"]+)"\)', cmake)
    if not ver or ver.group(1) != version:
        bad(f"CMakeLists.txt 的 PROJECT_VER 要和 app.json 的 version（{version}）一样")

    devkit = os.path.join(app, "devkit")
    if os.path.isdir(os.path.join(devkit, "firmware")):
        if git(devkit, "status", "--porcelain").stdout.strip():
            bad("devkit/ 里有改动：开发包只读，git -C devkit checkout . 还原")
        tag = git(devkit, "describe", "--tags", "--exact-match").stdout.strip()
        if tag != "v" + kit["version"]:
            bad(f"devkit/ 不是开发包的正式版本（被切到了别的提交）：./kit update {kit['version']} 换回 {kit['version']} 版")
    dirty = git(app, "status", "--porcelain").stdout.strip()
    if dirty:
        bad("有没提交的改动（审核按提交号重编，没提交的不算）：\n    " + dirty.replace("\n", "\n    "))
    head = git(app, "rev-parse", "HEAD").stdout.strip()

    build = os.path.join(app, "build-release")
    desc_path = os.path.join(build, "project_description.json")
    if not os.path.exists(desc_path):
        bad("还没编提交用的那份：./kit build --release")
    else:
        desc = json.load(open(desc_path))
        if desc.get("project_version") != version:
            bad(f"build-release 是 {desc.get('project_version')} 编的，不是 {version}：重新 ./kit build --release")
        config = open(os.path.join(build, "sdkconfig"), encoding="utf-8").read()
        if "CONFIG_YAY_DEV=y" in config:
            bad("提交用的那份带着开发命令（CONFIG_YAY_DEV=y）：用 ./kit build --release 编")
        if "CONFIG_APP_REPRODUCIBLE_BUILD=y" not in config:
            bad("没开 CONFIG_APP_REPRODUCIBLE_BUILD：别从 sdkconfig.defaults 里删它")
        image = os.path.join(build, desc["app_bin"])
        data = open(image, "rb").read()
        if len(data) > SLOT_MAX:
            bad(f"镜像 {len(data)} 字节，超过 {SLOT_MAX}（3.5 MB）")
        notes.append(f"镜像 {desc['app_bin']}：{len(data)} 字节，SHA-256 {hashlib.sha256(data).hexdigest()}")

    tests = os.path.join(app, "test", "run.sh")
    if os.path.exists(tests):
        run = subprocess.run(["bash", tests], capture_output=True, text=True)
        if run.returncode:
            bad("主机测试没过：\n    " + (run.stdout + run.stderr).strip().replace("\n", "\n    "))
        else:
            notes.append("主机测试：" + (run.stdout.strip().splitlines() or ["通过"])[-1])
    else:
        notes.append("没有 test/run.sh（还没有主机测试）")

    tracked = git(app, "ls-files", "-z").stdout.split("\0")
    for path in filter(None, tracked):
        full = os.path.join(app, path)
        if path.startswith("devkit") or not os.path.isfile(full):
            continue
        raw = open(full, "rb").read()
        if b"\0" in raw[:8192]:
            continue
        for n, line in enumerate(raw.decode("utf-8", "replace").splitlines(), 1):
            for pattern, what in SECRETS:
                if pattern.search(line):
                    bad(f"{path}:{n} 像是{what}：不要放进仓库")
            for m in re.finditer(r"\byk_[0-9a-f]{12}\b", line):
                if m.group(0) not in FAKE_DEVICES:
                    bad(f"{path}:{n} 有圆屏的设备编号 {m.group(0)}：抹掉（换成 yk_0a1b2c3d4e5f 这样的假号）")

    for n in notes:
        print("· " + n)
    if head:
        print(f"· 提交号 {head}，开发包 {kit['version']}")
    if problems:
        for p in problems:
            print("✗ " + p)
        print(f"有 {len(problems)} 处要改，改完再 ./kit check。")
        return 1
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print("✓ 都过了：提交单里填上面的提交号和 SHA-256（这是应用仓库 CI 编的，跟审核重编的环境一样）。")
    else:
        print("✓ 都过了。提交单里的 SHA-256 填应用仓库 CI（GitHub 上）里 ./kit check 打的那一串：Mac 上编的和")
        print("  Linux 上编的差 60 几个字节（ELF 哈希和两处校验，乐鑫两个平台的工具链不一样），代码一样。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
