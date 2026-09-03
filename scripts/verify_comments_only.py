#!/usr/bin/env python3
# 验证 main/ 下的 C/C++ 文件在剥离注释后是否保持一致。
# 无参数时比较 HEAD 与当前工作区；两个参数时比较指定 Git 版本。
import subprocess
import sys
from pathlib import Path


def strip_c_comments(text):
    """极简 C 词法分析：按状态机剥离 // 与 /* */ 注释，保留字符串/字符字面量。"""
    out = []
    i = 0
    n = len(text)
    state = "normal"  # normal | line_cmt | block_cmt | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "normal":
            if c == "/" and nxt == "/":
                state = "line_cmt"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block_cmt"
                i += 2
                continue
            if c == '"':
                state = "string"
                out.append(c)
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(c)
                i += 1
                continue
            out.append(c)
            i += 1
        elif state == "line_cmt":
            if c == "\n":
                state = "normal"
                out.append(c)
            i += 1
        elif state == "block_cmt":
            if c == "*" and nxt == "/":
                state = "normal"
                i += 2
                continue
            i += 1
        elif state == "string":
            out.append(c)
            if c == "\\":
                out.append(nxt)
                i += 2
                continue
            if c == '"':
                state = "normal"
            i += 1
        elif state == "char":
            out.append(c)
            if c == "\\":
                out.append(nxt)
                i += 2
                continue
            if c == "'":
                state = "normal"
            i += 1
    return "".join(out)


def norm(text):
    # 归一化换行，避免 CRLF/LF 差异造成误报
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    # 忽略纯空白行（注释块之间插入/移除分隔空行不算代码改动），并去掉行尾空白
    lines = [ln.rstrip() for ln in text.split("\n") if ln.strip() != ""]
    return "\n".join(lines)


def git_text(ref, path):
    result = subprocess.run(
        ["git", "show", f"{ref}:{path}"], text=True,
        encoding="utf-8", errors="replace", capture_output=True)
    return result.stdout if result.returncode == 0 else ""


def changed_files(base, target):
    # 将 rename 按删除+新增处理，避免只拿到新路径后误读基线。
    command = ["git", "diff", "--no-renames", "--name-only",
               "--diff-filter=ACDMRTUXB", base]
    if target is not None:
        command.append(target)
    command.extend(["--", "main/"])
    files = subprocess.check_output(
        command, text=True, encoding="utf-8", errors="replace").splitlines()
    if target is None:
        untracked = subprocess.check_output(
            ["git", "ls-files", "--others", "--exclude-standard", "--", "main/"],
            text=True, encoding="utf-8", errors="replace").splitlines()
        files.extend(untracked)
    return sorted({path for path in files if path.endswith((".c", ".h"))})


def main():
    if len(sys.argv) == 1:
        base, target = "HEAD", None
    elif len(sys.argv) == 3:
        base, target = sys.argv[1], sys.argv[2]
    else:
        print("usage: verify_comments_only.py [<base> <target>]", file=sys.stderr)
        return 2

    files = changed_files(base, target)
    problems = []
    for path in files:
        before = git_text(base, path)
        if target is None:
            after = (Path(path).read_text(encoding="utf-8", errors="replace")
                     if Path(path).exists() else "")
        else:
            after = git_text(target, path)
        if norm(strip_c_comments(before)) != norm(strip_c_comments(after)):
            problems.append(path)
    if problems:
        print("NON-COMMENT DIFFERENCES FOUND (code changed):")
        for p in problems:
            print("  " + p)
        return 1
    target_name = target if target is not None else "working tree"
    print(f"OK: {len(files)} C/H files are comments-only ({base} -> {target_name}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
