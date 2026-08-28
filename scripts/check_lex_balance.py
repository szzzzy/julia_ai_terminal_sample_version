#!/usr/bin/env python3
# 词法级检查：遍历 main/ 下全部 .c/.h（含未参与构建、未跟踪文件），
# 1) 确认块注释/* */与字符串/字符字面量在文件末尾处于闭合状态（防未闭合注释破坏编译）；
# 2) 检测多行宏 `\` 续行前一行是否带行注释 `//`（会吃掉续行符）。
import os
import re
import glob
import sys

BAD_END = []      # 词法未闭合
BAD_MACRO = []    # 宏续行前有 // 注释


def lex(text):
    """扫描并返回结束状态；只区分 normal/line_cmt(不经字符串)/block/string/char。"""
    i, n = 0, len(text)
    state = "normal"
    has_bs_after_line_cmt = []
    line_start = True
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
                i += 1
                continue
            if c == "'":
                state = "char"
                i += 1
                continue
            i += 1
        elif state == "line_cmt":
            if c == "\n":
                state = "normal"
            i += 1
        elif state == "block_cmt":
            if c == "*" and nxt == "/":
                state = "normal"
                i += 2
                continue
            i += 1
        elif state == "string":
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == '"':
                state = "normal"
            i += 1
        elif state == "char":
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == "'":
                state = "normal"
            i += 1
    return state


def check_macro_continuation(path, text):
    """若某行以反斜杠结尾而行尾前是 // 注释（且不是字符串内），该续行符会被注释吞掉。"""
    lines = text.split("\n")
    for idx, line in enumerate(lines[:-1]):
        if not line.rstrip().endswith("\\"):
            continue
        stripped = line.rstrip()
        # 粗略：去掉字符串后看行尾
        rest = re.sub(r'"(?:[^"\\]|\\.)*"', '""', stripped)
        if rest.rstrip().rstrip(" \\\t").endswith("//"):
            BAD_MACRO.append(f"{path}:{idx+1}")


def main():
    roots = ["main"]
    files = []
    for r in roots:
        for dirpath, _dirnames, filenames in os.walk(r):
            for fn in filenames:
                if fn.endswith((".c", ".h")):
                    files.append(os.path.join(dirpath, fn))
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        st = lex(text)
        if st not in ("normal", "line_cmt"):  # 文件末尾处于 // 注释且无换行是合法 C
            BAD_END.append(f"{path} (end state={st})")
        check_macro_continuation(path, text)
    if BAD_END:
        print("UNCLOSED LEXICAL STATE:")
        for p in BAD_END:
            print("  " + p)
    if BAD_MACRO:
        print("MACRO-CONTINUATION COMMENT ISSUES:")
        for p in BAD_MACRO:
            print("  " + p)
    if not BAD_END and not BAD_MACRO:
        print(f"OK: {len(files)} files lexically balanced, no macro-continuation comment issues.")


if __name__ == "__main__":
    main()
