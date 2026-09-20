"""Inventory owned C code; counts are structural signals, not defect verdicts."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LEX = re.compile(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
DECL = re.compile(r"^(?:static\s+)?(?:[A-Za-z_]\w*[ \t*]+)+([A-Za-z_]\w*)\([^;{}]*\)\s*\{", re.M)


def main():
    files, functions = [], []
    paths = sorted(list((ROOT / "main").rglob("*.c")) +
                   list((ROOT / "components/julia_board_audio").rglob("*.c")))
    for path in paths:
        if "legacy" in path.parts or "generated" in path.parts:
            continue
        original = path.read_text(encoding="utf-8-sig")
        text = LEX.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), original)
        rel = path.relative_to(ROOT).as_posix()
        files.append(dict(path=rel, lines=len(original.splitlines()),
                          code_lines=sum(bool(line.strip()) for line in text.splitlines()),
                          conditional_directives=len(re.findall(r"^\s*#\s*(?:if|ifdef|ifndef|elif|else)\b", text, re.M))))
        for match in DECL.finditer(text):
            start = match.end()-1
            depth = maximum = 0
            for end in range(start, len(text)):
                if text[end] == "{":
                    depth += 1
                    maximum = max(maximum, depth)
                elif text[end] == "}":
                    depth -= 1
                    if not depth:
                        break
            body = text[start:end+1]
            functions.append(dict(path=rel, name=match.group(1),
                line=text.count("\n", 0, match.start())+1,
                lines=text.count("\n", match.start(), end)+1,
                code_lines=sum(bool(line.strip()) for line in text[match.start():end+1].splitlines()),
                if_count=len(re.findall(r"\bif\s*\(", body)),
                max_brace_depth=maximum))
    report = dict(files=files, functions=functions)
    (Path(__file__).parent / "structure.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Owned C sources: {len(files)}; functions: {len(functions)}")
    print("Largest files:")
    for f in sorted(files, key=lambda f:f["code_lines"], reverse=True)[:10]:
        print(f)
    print("Largest functions:")
    for f in sorted(functions, key=lambda f:f["code_lines"], reverse=True)[:15]:
        print(f)
    print("Deepest blocks (function body counts as depth 1):")
    for f in sorted(functions, key=lambda f:f["max_brace_depth"], reverse=True)[:8]:
        print(f)


if __name__ == "__main__":
    main()
