#!/usr/bin/env python3
"""
Claude Code 对话展示工具
用法:
  python3 show_conversation.py <对话文件.txt>              # 终端显示
  python3 show_conversation.py --html <对话文件.txt>       # 导出 HTML
  python3 show_conversation.py --compact <对话文件.txt>    # 压缩模式
"""

import sys
import re
import signal
import argparse
import html as html_module
from pathlib import Path
from string import Template

# 处理管道中断
try:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
except AttributeError:
    pass  # Windows 没有 SIGPIPE

# ANSI 颜色代码
class Colors:
    RESET = '\033[0m'
    BOLD = '\033[1m'
    DIM = '\033[2m'
    RED = '\033[31m'
    GREEN = '\033[32m'
    YELLOW = '\033[33m'
    BLUE = '\033[34m'
    MAGENTA = '\033[35m'
    CYAN = '\033[36m'
    WHITE = '\033[37m'
    BRIGHT_GREEN = '\033[92m'
    BRIGHT_YELLOW = '\033[93m'
    BRIGHT_BLUE = '\033[94m'
    BRIGHT_MAGENTA = '\033[95m'
    BRIGHT_CYAN = '\033[96m'
    BG_BLUE = '\033[44m'
    BG_GREEN = '\033[42m'

# HTML 模板 - 使用 $filename 和 $content 占位符
HTML_TEMPLATE = '''<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Claude Code 对话记录</title>
    <style>
        :root {
            --bg-dark: #1a1a2e;
            --bg-card: #16213e;
            --text-primary: #eee;
            --text-dim: #888;
            --accent-green: #4ade80;
            --accent-cyan: #22d3ee;
            --accent-yellow: #fbbf24;
            --accent-magenta: #e879f9;
            --accent-red: #f87171;
            --accent-blue: #60a5fa;
        }
        * { box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Roboto', 'Helvetica Neue', Arial, sans-serif;
            background: var(--bg-dark);
            color: var(--text-primary);
            margin: 0;
            padding: 20px;
            line-height: 1.6;
            font-size: 15px;
        }
        code, .code-block, .table-row, .tool-result {
            font-family: 'SF Mono', 'Consolas', 'Monaco', 'Menlo', monospace;
            font-size: 13px;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
        }
        .header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            padding: 20px 30px;
            border-radius: 12px;
            margin-bottom: 20px;
            box-shadow: 0 4px 15px rgba(0,0,0,0.3);
        }
        .header h1 {
            margin: 0;
            font-size: 24px;
        }
        .header .meta {
            color: rgba(255,255,255,0.8);
            font-size: 12px;
            margin-top: 8px;
        }
        .user-block {
            background: linear-gradient(135deg, #059669 0%, #10b981 100%);
            border-radius: 12px;
            padding: 15px 20px;
            margin: 20px 0;
            box-shadow: 0 4px 15px rgba(16, 185, 129, 0.3);
        }
        .user-block .label {
            font-size: 12px;
            font-weight: bold;
            opacity: 0.9;
            margin-bottom: 8px;
        }
        .user-block .content {
            font-size: 16px;
        }
        .claude-block {
            background: var(--bg-card);
            border-left: 4px solid var(--accent-cyan);
            padding: 12px 20px;
            margin: 8px 0;
            border-radius: 0 8px 8px 0;
        }
        .tool-call {
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .tool-call .icon { font-size: 16px; }
        .tool-call.file { color: var(--accent-yellow); }
        .tool-call.bash { color: var(--accent-magenta); }
        .tool-call.search { color: var(--accent-cyan); }
        .tool-call.other { color: var(--accent-cyan); }
        .tool-result {
            color: var(--text-dim);
            padding-left: 28px;
            font-size: 13px;
        }
        .diff-add {
            color: var(--accent-green);
            background: rgba(74, 222, 128, 0.1);
            padding: 2px 8px;
            border-radius: 4px;
            display: inline-block;
        }
        .diff-del {
            color: var(--accent-red);
            background: rgba(248, 113, 113, 0.1);
            padding: 2px 8px;
            border-radius: 4px;
            display: inline-block;
        }
        .code-block {
            background: #0d1117;
            border-radius: 8px;
            padding: 15px;
            margin: 10px 0;
            overflow-x: auto;
            border: 1px solid #30363d;
        }
        .table-row {
            color: var(--accent-cyan);
        }
        .separator {
            color: var(--accent-magenta);
            text-align: center;
            margin: 15px 0;
        }
        .task-complete {
            color: var(--accent-green);
        }
        .text-content {
            padding: 8px 0;
            white-space: pre-wrap;
            word-wrap: break-word;
        }
        .diff-summary {
            color: var(--text-dim);
            font-style: italic;
            padding: 8px 20px;
        }
        .diff-summary .add { color: var(--accent-green); }
        .diff-summary .del { color: var(--accent-red); }
        .collapsed {
            cursor: pointer;
            user-select: none;
        }
        .collapsed:hover {
            opacity: 0.8;
        }
        .code-inline {
            background: #0d1117;
            padding: 2px 6px;
            border-radius: 4px;
            font-family: monospace;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>📜 Claude Code 对话记录</h1>
            <div class="meta">文件: $filename</div>
        </div>
        <div class="content">
$content
        </div>
    </div>
</body>
</html>
'''

def escape_html(text):
    """HTML 转义"""
    return html_module.escape(text)

def classify_line(line):
    """分类行类型"""
    stripped = line.strip()

    if line.startswith('>'):
        return 'user', line[1:].strip()

    if stripped.startswith('●'):
        content = stripped[1:].strip()
        if content.startswith('Read(') or content.startswith('Update(') or content.startswith('Write('):
            return 'tool_file', content
        elif content.startswith('Bash('):
            return 'tool_bash', content
        elif content.startswith('Glob(') or content.startswith('Grep('):
            return 'tool_search', content
        else:
            return 'tool_other', content

    if '⎿' in line:
        return 'tool_result', stripped

    if '═' in line or ('━' in line and len(stripped) > 20):
        return 'separator', stripped

    if 'Claude Code' in line or '▐▛' in line or '▝▜' in line:
        return 'header', stripped

    if 'Task' in line and 'completed' in line:
        return 'task_complete', stripped

    diff_match = re.match(r'^(\s*)(\d+)\s*([+-])\s*(.*)$', line)
    if diff_match:
        sign = diff_match.group(3)
        content = diff_match.group(4)
        return 'diff_add' if sign == '+' else 'diff_del', content

    if stripped.startswith('|') and '|' in stripped[1:]:
        return 'table', stripped

    if stripped.startswith('```'):
        return 'code_fence', stripped

    if not stripped:
        return 'empty', ''

    return 'text', line.rstrip()

def to_html_line(line_type, content):
    """转换单行为 HTML"""
    escaped = escape_html(content)

    if line_type == 'user':
        return f'''        <div class="user-block">
            <div class="label">👤 USER</div>
            <div class="content">{escaped}</div>
        </div>'''

    if line_type == 'tool_file':
        return f'        <div class="claude-block"><div class="tool-call file"><span class="icon">📝</span> {escaped}</div></div>'

    if line_type == 'tool_bash':
        return f'        <div class="claude-block"><div class="tool-call bash"><span class="icon">💻</span> {escaped}</div></div>'

    if line_type == 'tool_search':
        return f'        <div class="claude-block"><div class="tool-call search"><span class="icon">🔍</span> {escaped}</div></div>'

    if line_type == 'tool_other':
        return f'        <div class="claude-block"><div class="tool-call other"><span class="icon">🤖</span> {escaped}</div></div>'

    if line_type == 'tool_result':
        return f'        <div class="tool-result">{escaped}</div>'

    if line_type == 'separator':
        return f'        <div class="separator">{escaped}</div>'

    if line_type == 'header':
        return f'        <div class="separator" style="color: var(--accent-blue); font-weight: bold;">{escaped}</div>'

    if line_type == 'task_complete':
        return f'        <div class="task-complete">✓ {escaped}</div>'

    if line_type == 'diff_add':
        return f'        <div class="diff-add">+ {escaped}</div>'

    if line_type == 'diff_del':
        return f'        <div class="diff-del">- {escaped}</div>'

    if line_type == 'table':
        return f'        <div class="table-row">{escaped}</div>'

    if line_type == 'code_fence':
        return f'        <div class="code-fence" style="color: var(--text-dim);">{escaped}</div>'

    if line_type == 'empty':
        return '        <br>'

    return f'        <div class="text-content">{escaped}</div>'

def convert_to_html(lines, filename, compact=False):
    """转换为 HTML"""
    html_lines = []
    i = 0
    n = len(lines)
    diff_count = 0
    diff_adds = 0
    diff_dels = 0
    in_diff = False

    while i < n:
        line = lines[i]
        line_type, content = classify_line(line)

        # 压缩模式下折叠 diff
        if compact and line_type in ('diff_add', 'diff_del'):
            in_diff = True
            diff_count += 1
            if line_type == 'diff_add':
                diff_adds += 1
            else:
                diff_dels += 1
            i += 1
            continue

        # 输出累积的 diff 统计
        if in_diff and diff_count > 0 and line_type not in ('diff_add', 'diff_del'):
            html_lines.append(f'        <div class="diff-summary">... [{diff_count} 行差异: <span class="add">+{diff_adds}</span>, <span class="del">-{diff_dels}</span>]</div>')
            diff_count = diff_adds = diff_dels = 0
            in_diff = False

        # 跳过压缩模式下的纯行号行
        if compact and re.match(r'^\s*\d+\s*$', line.strip()):
            i += 1
            continue

        html_lines.append(to_html_line(line_type, content))
        i += 1

    # 最后的 diff 统计
    if in_diff and diff_count > 0:
        html_lines.append(f'        <div class="diff-summary">... [{diff_count} 行差异: <span class="add">+{diff_adds}</span>, <span class="del">-{diff_dels}</span>]</div>')

    return Template(HTML_TEMPLATE).substitute(
        filename=escape_html(filename),
        content='\n'.join(html_lines)
    )

def colorize_line(line, compact=False):
    """根据行内容添加颜色（终端版）"""
    stripped = line.strip()

    if line.startswith('>'):
        return f"\n{Colors.BOLD}{Colors.BRIGHT_GREEN}{'━' * 70}{Colors.RESET}\n{Colors.BOLD}{Colors.BG_GREEN} 👤 USER {Colors.RESET} {Colors.BRIGHT_GREEN}{line[1:].strip()}{Colors.RESET}\n{Colors.BOLD}{Colors.BRIGHT_GREEN}{'━' * 70}{Colors.RESET}"

    if stripped.startswith('●'):
        content = stripped[1:].strip()
        if content.startswith('Read(') or content.startswith('Update(') or content.startswith('Write('):
            return f"{Colors.BRIGHT_YELLOW}  📝 {content}{Colors.RESET}"
        elif content.startswith('Bash('):
            return f"{Colors.BRIGHT_MAGENTA}  💻 {content}{Colors.RESET}"
        elif content.startswith('Glob(') or content.startswith('Grep('):
            return f"{Colors.BRIGHT_CYAN}  🔍 {content}{Colors.RESET}"
        else:
            return f"{Colors.BRIGHT_CYAN}  🤖 {content}{Colors.RESET}"

    if '⎿' in line:
        return f"{Colors.DIM}{Colors.YELLOW}     {stripped}{Colors.RESET}"

    if '═' in line or ('━' in line and len(stripped) > 20):
        return f"{Colors.BRIGHT_MAGENTA}{line}{Colors.RESET}"

    if 'Claude Code' in line or '▐▛' in line or '▝▜' in line:
        return f"{Colors.BOLD}{Colors.BRIGHT_BLUE}{line}{Colors.RESET}"

    if 'Task' in line and 'completed' in line:
        return f"{Colors.GREEN}  ✓ {stripped}{Colors.RESET}"

    diff_match = re.match(r'^(\s*)(\d+)\s*([+-])\s*(.*)$', line)
    if diff_match:
        indent, linenum, sign, content = diff_match.groups()
        if sign == '+':
            return f"{Colors.GREEN}{indent}{linenum} {sign} {content}{Colors.RESET}"
        else:
            return f"{Colors.RED}{indent}{linenum} {sign} {content}{Colors.RESET}"

    if stripped.startswith('|') and '|' in stripped[1:]:
        return f"{Colors.CYAN}{line}{Colors.RESET}"

    if stripped.startswith('```'):
        return f"{Colors.DIM}{Colors.YELLOW}{line}{Colors.RESET}"

    return f"{Colors.WHITE}{line}{Colors.RESET}"

def show_terminal(filepath, compact=False):
    """终端显示"""
    print(f"\n{Colors.BOLD}{Colors.BG_BLUE} 📜 Claude Code 对话记录 {Colors.RESET}")
    print(f"{Colors.DIM}文件: {filepath}{Colors.RESET}")
    if compact:
        print(f"{Colors.DIM}模式: 压缩{Colors.RESET}")
    print()

    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"{Colors.RED}错误: 文件不存在 - {filepath}{Colors.RESET}")
        return
    except Exception as e:
        print(f"{Colors.RED}错误: {e}{Colors.RESET}")
        return

    if compact:
        show_compact(lines)
    else:
        show_full(lines)

def show_full(lines):
    """完整显示"""
    for line in lines:
        if not line.strip():
            print()
            continue
        try:
            print(colorize_line(line.rstrip()))
        except BrokenPipeError:
            sys.exit(0)

def show_compact(lines):
    """压缩显示"""
    i = 0
    n = len(lines)
    diff_count = 0
    diff_adds = 0
    diff_dels = 0
    in_diff = False

    while i < n:
        line = lines[i].rstrip()
        stripped = line.strip()

        if line.startswith('>'):
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        if stripped.startswith('●'):
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        if '⎿' in line:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        diff_match = re.match(r'^(\s*)(\d+)\s*([+-])\s*(.*)$', line)
        if diff_match:
            in_diff = True
            diff_count += 1
            sign = diff_match.group(3)
            if sign == '+':
                diff_adds += 1
            else:
                diff_dels += 1
            i += 1
            continue

        if re.match(r'^\s*\d+\s*$', line) or re.match(r'^\s*\d+\s+[^+-]', line):
            if in_diff:
                i += 1
                continue

        if stripped.startswith('|') and '|' in stripped[1:]:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        if '═' in line or '━' in line or 'Claude Code' in line or '▐▛' in line:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        if not stripped:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print()
            i += 1
            continue

        if in_diff:
            i += 1
            continue

        try:
            print(colorize_line(line))
        except BrokenPipeError:
            sys.exit(0)
        i += 1

    if in_diff and diff_count > 0:
        print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")

def export_html(filepath, output_path=None, compact=False):
    """导出为 HTML"""
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"错误: 文件不存在 - {filepath}")
        return None
    except Exception as e:
        print(f"错误: {e}")
        return None

    html_content = convert_to_html(lines, filepath, compact)

    if output_path is None:
        output_path = Path(filepath).stem + '.html'

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(html_content)

    print(f"已导出: {output_path}")
    return output_path

def main():
    parser = argparse.ArgumentParser(description='Claude Code 对话展示工具')
    parser.add_argument('file', help='对话文件路径')
    parser.add_argument('--compact', '-c', action='store_true', help='压缩模式：折叠代码差异')
    parser.add_argument('--html', action='store_true', help='导出为 HTML 文件')
    parser.add_argument('-o', '--output', help='HTML 输出路径 (默认: 同名.html)')
    args = parser.parse_args()

    if args.html:
        export_html(args.file, args.output, args.compact)
    else:
        show_terminal(args.file, args.compact)

if __name__ == '__main__':
    main()
