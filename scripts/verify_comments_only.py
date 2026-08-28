#!/usr/bin/env python3
# 验证工具：对 main/ 下被修改的跟踪文件，剥离 C 注释后与 HEAD 版本对比，
# 若剥离后内容完全一致，则证明"只改了注释"。
# 用法: python verify_comments_only.py [git-arg ...]
# 说明: 这是一个一次性验证脚本，不属于固件代码，不参与 CMake 构建。
import subprocess
import sys


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


def main():
    # 本次会话开始前工作区已有代码改动的文件（在研移植/集成代码），无法以 HEAD 为基线严格归因，
    # 单独列出；其余文件以 HEAD 为基线做剥离对比。
    PRE_EXISTING = {
        "main/PCF85063/PCF85063.c", "main/PCF85063/PCF85063.h",
        "main/app/main.c",
        "main/context/julia_context.c", "main/context/julia_context.h",
        "main/hardware/tca9554.c", "main/hardware/tca9554.h",
        "main/ui/avatar_parts/avatar_eyes.c", "main/ui/avatar_parts/avatar_eyes.h",
        "main/ui/julia_avatar.c", "main/ui/julia_avatar.h",
        "main/ui/julia_display_theme.c", "main/ui/julia_display_theme.h",
        "main/voice/voice_service.c", "main/voice/wake_detector.c",
    }
    git = ["git", "diff", "--name-only", "--diff-filter=ACMR", "--", "main/"]
    files = subprocess.check_output(git, text=True, encoding="utf-8", errors="replace").splitlines()
    files = [f for f in files if f.endswith((".c", ".h"))]
    problems = []
    strict, pre = [], []
    for f in files:
        if f in PRE_EXISTING:
            pre.append(f)
            continue
        strict.append(f)
    for f in strict:
        head = subprocess.check_output(["git", "show", f"HEAD:{f}"], encoding="utf-8", errors="replace")
        with open(f, encoding="utf-8", errors="replace") as fh:
            work = fh.read()
        if norm(strip_c_comments(head)) != norm(strip_c_comments(work)):
            problems.append(f)
    if problems:
        print("NON-COMMENT DIFFERENCES FOUND (code changed):")
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print(f"OK: {len(strict)} tracked files are comments-only vs HEAD.")
    print(f"Pre-existing modified files (not checked against HEAD): {len(pre)}")
    for p in pre:
        print("  [pre-existing] " + p)


if __name__ == "__main__":
    main()
