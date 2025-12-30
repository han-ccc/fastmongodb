#!/usr/bin/env python3
"""
Claude Code 对话展示工具
用法: python3 show_conversation.py [--compact] <对话文件.txt>
"""

import sys
import re
import signal
import argparse

# 处理管道中断
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

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

def colorize_line(line, compact=False):
    """根据行内容添加颜色"""
    stripped = line.strip()

    # 用户输入 (> 开头)
    if line.startswith('>'):
        return f"\n{Colors.BOLD}{Colors.BRIGHT_GREEN}{'━' * 70}{Colors.RESET}\n{Colors.BOLD}{Colors.BG_GREEN} 👤 USER {Colors.RESET} {Colors.BRIGHT_GREEN}{line[1:].strip()}{Colors.RESET}\n{Colors.BOLD}{Colors.BRIGHT_GREEN}{'━' * 70}{Colors.RESET}"

    # Claude 回复 (● 开头)
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

    # 工具调用结果 (⎿ 开头)
    if '⎿' in line:
        return f"{Colors.DIM}{Colors.YELLOW}     {stripped}{Colors.RESET}"

    # 标题行
    if '═' in line or ('━' in line and len(stripped) > 20):
        return f"{Colors.BRIGHT_MAGENTA}{line}{Colors.RESET}"

    # Claude Code 头部
    if 'Claude Code' in line or '▐▛' in line or '▝▜' in line:
        return f"{Colors.BOLD}{Colors.BRIGHT_BLUE}{line}{Colors.RESET}"

    # Task 完成
    if 'Task' in line and 'completed' in line:
        return f"{Colors.GREEN}  ✓ {stripped}{Colors.RESET}"

    # 代码差异
    diff_match = re.match(r'^(\s*)(\d+)\s*([+-])\s*(.*)$', line)
    if diff_match:
        indent, linenum, sign, content = diff_match.groups()
        if sign == '+':
            return f"{Colors.GREEN}{indent}{linenum} {sign} {content}{Colors.RESET}"
        else:
            return f"{Colors.RED}{indent}{linenum} {sign} {content}{Colors.RESET}"

    # 表格行
    if stripped.startswith('|') and '|' in stripped[1:]:
        return f"{Colors.CYAN}{line}{Colors.RESET}"

    # 代码块标记
    if stripped.startswith('```'):
        return f"{Colors.DIM}{Colors.YELLOW}{line}{Colors.RESET}"

    return f"{Colors.WHITE}{line}{Colors.RESET}"

def show_conversation(filepath, compact=False):
    """展示对话"""
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
    """压缩显示 - 折叠代码差异和工具输出"""
    i = 0
    n = len(lines)
    diff_count = 0
    diff_adds = 0
    diff_dels = 0
    in_diff = False
    skipped_lines = 0

    while i < n:
        line = lines[i].rstrip()
        stripped = line.strip()

        # 用户输入 - 始终显示
        if line.startswith('>'):
            # 先输出累积的 diff 统计
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        # Claude 回复 (●) - 始终显示
        if stripped.startswith('●'):
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        # 工具结果 (⎿) - 显示
        if '⎿' in line:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        # 代码差异行 - 折叠统计
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

        # 纯数字行号行 (如 "     91") - 跳过
        if re.match(r'^\s*\d+\s*$', line) or re.match(r'^\s*\d+\s+[^+-]', line):
            if in_diff:
                i += 1
                continue

        # 表格行 - 始终显示
        if stripped.startswith('|') and '|' in stripped[1:]:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        # 标题/分隔线 - 显示
        if '═' in line or '━' in line or 'Claude Code' in line or '▐▛' in line:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print(colorize_line(line))
            i += 1
            continue

        # 空行
        if not stripped:
            if in_diff and diff_count > 0:
                print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")
                diff_count = diff_adds = diff_dels = 0
                in_diff = False
            print()
            i += 1
            continue

        # 其他内容 - 如果在 diff 中则跳过，否则显示
        if in_diff:
            # 可能是 diff 上下文行，跳过
            i += 1
            continue

        # 普通内容行
        try:
            print(colorize_line(line))
        except BrokenPipeError:
            sys.exit(0)
        i += 1

    # 最后的 diff 统计
    if in_diff and diff_count > 0:
        print(f"{Colors.DIM}     ... [{diff_count} 行差异: {Colors.GREEN}+{diff_adds}{Colors.RESET}{Colors.DIM}, {Colors.RED}-{diff_dels}{Colors.RESET}{Colors.DIM}]{Colors.RESET}")

def main():
    parser = argparse.ArgumentParser(description='Claude Code 对话展示工具')
    parser.add_argument('file', help='对话文件路径')
    parser.add_argument('--compact', '-c', action='store_true', help='压缩模式：折叠代码差异')
    args = parser.parse_args()

    show_conversation(args.file, compact=args.compact)

if __name__ == '__main__':
    main()
