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

# HTML 模板 - 使用 $filename, $content, $toc 占位符
HTML_TEMPLATE = '''<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Claude Code 对话记录</title>
    <style>
        :root {
            --bg-main: #ffffff;
            --bg-sidebar: #f8f9fa;
            --bg-card: #f1f3f4;
            --bg-code: #f6f8fa;
            --text-primary: #1f2937;
            --text-secondary: #4b5563;
            --text-dim: #6b7280;
            --border-color: #e5e7eb;
            --accent-green: #059669;
            --accent-green-bg: #d1fae5;
            --accent-cyan: #0891b2;
            --accent-cyan-bg: #cffafe;
            --accent-yellow: #d97706;
            --accent-magenta: #9333ea;
            --accent-red: #dc2626;
            --accent-blue: #2563eb;
            --sidebar-width: 300px;
        }
        * { box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Roboto', 'Helvetica Neue', Arial, sans-serif;
            background: var(--bg-main);
            color: var(--text-primary);
            margin: 0;
            padding: 0;
            line-height: 1.8;
            font-size: 16px;
        }
        code, .code-block, .table-row, .tool-result {
            font-family: 'SF Mono', 'Consolas', 'Monaco', 'Menlo', monospace;
            font-size: 14px;
        }
        .layout {
            display: flex;
            min-height: 100vh;
        }
        .sidebar {
            width: var(--sidebar-width);
            background: var(--bg-sidebar);
            border-right: 1px solid var(--border-color);
            position: fixed;
            top: 0;
            left: 0;
            height: 100vh;
            overflow-y: auto;
            padding: 20px 15px;
        }
        .sidebar h2 {
            font-size: 14px;
            color: var(--text-secondary);
            margin: 0 0 15px 0;
            padding-bottom: 10px;
            border-bottom: 1px solid var(--border-color);
        }
        .toc-item {
            display: block;
            padding: 10px 12px;
            margin: 4px 0;
            border-radius: 8px;
            text-decoration: none;
            color: var(--text-primary);
            font-size: 13px;
            transition: background 0.2s;
            border-left: 3px solid transparent;
        }
        .toc-item:hover {
            background: rgba(0,0,0,0.04);
        }
        .toc-item.user {
            border-left-color: var(--accent-green);
            background: var(--accent-green-bg);
            font-weight: 500;
        }
        .toc-item.user:hover {
            background: #a7f3d0;
        }
        .toc-item.claude {
            border-left-color: var(--accent-cyan);
            color: var(--text-dim);
            font-size: 12px;
            padding-left: 20px;
            background: transparent;
        }
        .toc-item.claude:hover {
            background: var(--accent-cyan-bg);
            color: var(--text-primary);
        }
        .toc-label {
            font-size: 10px;
            font-weight: 600;
            color: var(--accent-green);
            margin-bottom: 3px;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        .toc-text {
            display: -webkit-box;
            -webkit-line-clamp: 2;
            -webkit-box-orient: vertical;
            overflow: hidden;
        }
        .main-content {
            margin-left: var(--sidebar-width);
            flex: 1;
            padding: 30px 40px;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
        }
        .header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 25px 30px;
            border-radius: 12px;
            margin-bottom: 30px;
            box-shadow: 0 4px 15px rgba(102, 126, 234, 0.3);
        }
        .header h1 {
            margin: 0;
            font-size: 22px;
            font-weight: 600;
        }
        .header .meta {
            color: rgba(255,255,255,0.85);
            font-size: 13px;
            margin-top: 8px;
        }
        .user-block {
            background: var(--accent-green-bg);
            border: 1px solid #a7f3d0;
            border-radius: 12px;
            padding: 16px 20px;
            margin: 25px 0 15px 0;
            scroll-margin-top: 20px;
        }
        .user-block .label {
            font-size: 11px;
            font-weight: 600;
            color: var(--accent-green);
            margin-bottom: 8px;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        .user-block .content {
            font-size: 17px;
            color: #065f46;
            font-weight: 500;
        }
        .claude-block {
            background: var(--bg-card);
            border-left: 3px solid var(--accent-cyan);
            padding: 12px 18px;
            margin: 8px 0;
            border-radius: 0 8px 8px 0;
        }
        .tool-call {
            display: flex;
            align-items: center;
            gap: 8px;
            font-size: 14px;
        }
        .tool-call .icon { font-size: 16px; }
        .tool-call.file { color: var(--accent-yellow); }
        .tool-call.bash { color: var(--accent-magenta); }
        .tool-call.search { color: var(--accent-cyan); }
        .tool-call.other { color: var(--accent-cyan); }
        .tool-result {
            color: var(--text-dim);
            padding-left: 24px;
            font-size: 12px;
            border-left: 2px solid var(--border-color);
            margin-left: 8px;
        }
        .diff-add {
            color: #166534;
            background: #dcfce7;
            padding: 2px 8px;
            border-radius: 4px;
            display: inline-block;
            font-family: monospace;
            font-size: 13px;
        }
        .diff-del {
            color: #991b1b;
            background: #fee2e2;
            padding: 2px 8px;
            border-radius: 4px;
            display: inline-block;
            font-family: monospace;
            font-size: 13px;
        }
        .code-block {
            background: var(--bg-code);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 15px;
            margin: 10px 0;
            overflow-x: auto;
        }
        .table-row {
            color: var(--text-secondary);
            background: var(--bg-code);
            padding: 4px 8px;
            border-radius: 4px;
            margin: 2px 0;
            display: block;
        }
        .separator {
            color: var(--text-dim);
            text-align: center;
            margin: 20px 0;
            font-size: 13px;
        }
        .task-complete {
            color: var(--accent-green);
            font-weight: 500;
        }
        .text-content {
            padding: 6px 0;
            white-space: pre-wrap;
            word-wrap: break-word;
            color: var(--text-secondary);
            line-height: 1.8;
        }
        .diff-summary {
            color: var(--text-dim);
            font-style: italic;
            padding: 8px 16px;
            background: var(--bg-code);
            border-radius: 6px;
            margin: 8px 0;
            font-size: 13px;
        }
        .diff-summary .add { color: #166534; font-weight: 500; }
        .diff-summary .del { color: #991b1b; font-weight: 500; }
        .section-anchor {
            scroll-margin-top: 20px;
        }
        @media (max-width: 1024px) {
            .sidebar {
                display: none;
            }
            .main-content {
                margin-left: 0;
                padding: 20px;
            }
        }
    </style>
</head>
<body>
    <div class="layout">
        <nav class="sidebar">
            <h2>📑 目录</h2>
$toc
        </nav>
        <div class="main-content">
            <div class="container">
                <div class="header">
                    <h1>📜 Claude Code 对话记录</h1>
                    <div class="meta">文件: $filename</div>
                </div>
                <div class="content">
$content
                </div>
            </div>
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

def summarize_text(text, max_len=50):
    """截取文本摘要"""
    text = text.strip()
    if len(text) > max_len:
        return text[:max_len] + '...'
    return text

def to_html_line(line_type, content, section_id=None):
    """转换单行为 HTML"""
    escaped = escape_html(content)

    if line_type == 'user':
        id_attr = f' id="section-{section_id}"' if section_id else ''
        return f'''        <div class="user-block"{id_attr}>
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
    toc_items = []
    i = 0
    n = len(lines)
    diff_count = 0
    diff_adds = 0
    diff_dels = 0
    in_diff = False
    section_id = 0
    current_user_question = None
    claude_response_buffer = []

    def flush_claude_response():
        """将 Claude 回复总结加入目录"""
        nonlocal claude_response_buffer
        if claude_response_buffer and current_user_question is not None:
            # 提取有意义的回复内容（跳过工具调用）
            meaningful = []
            for resp_type, resp_content in claude_response_buffer:
                if resp_type == 'text' and resp_content.strip():
                    meaningful.append(resp_content.strip())
            if meaningful:
                summary = summarize_text(' '.join(meaningful)[:100], 60)
                if summary:
                    toc_items.append(('claude', current_user_question, summary))
        claude_response_buffer = []

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

        # 用户问题 - 新的 section
        if line_type == 'user':
            flush_claude_response()
            section_id += 1
            current_user_question = section_id
            summary = summarize_text(content, 40)
            toc_items.append(('user', section_id, summary))
            html_lines.append(to_html_line(line_type, content, section_id))
        else:
            # 记录 Claude 回复用于总结
            if line_type == 'text':
                claude_response_buffer.append((line_type, content))
            html_lines.append(to_html_line(line_type, content))

        i += 1

    # 最后的 diff 统计
    if in_diff and diff_count > 0:
        html_lines.append(f'        <div class="diff-summary">... [{diff_count} 行差异: <span class="add">+{diff_adds}</span>, <span class="del">-{diff_dels}</span>]</div>')

    flush_claude_response()

    # 生成目录 HTML
    toc_html_lines = []
    for item in toc_items:
        if item[0] == 'user':
            _, sid, summary = item
            toc_html_lines.append(f'            <a href="#section-{sid}" class="toc-item user"><div class="toc-label">👤 问题</div><div class="toc-text">{escape_html(summary)}</div></a>')
        else:
            _, sid, summary = item
            toc_html_lines.append(f'            <a href="#section-{sid}" class="toc-item claude"><div class="toc-text">↳ {escape_html(summary)}</div></a>')

    return Template(HTML_TEMPLATE).substitute(
        filename=escape_html(filename),
        content='\n'.join(html_lines),
        toc='\n'.join(toc_html_lines)
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
