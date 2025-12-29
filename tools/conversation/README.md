# Claude Code 对话展示工具

用于美化展示 Claude Code 导出的对话记录文件。

## 功能

- 彩色高亮显示用户输入、Claude 回复、工具调用
- 支持压缩模式，折叠大量代码差异
- 自动识别代码块、表格、diff 等格式

## 用法

```bash
# 完整显示
python3 show_conversation.py <对话文件.txt>

# 压缩模式（折叠代码差异，适合快速浏览）
python3 show_conversation.py --compact <对话文件.txt>

# 配合 less 使用（支持颜色）
python3 show_conversation.py <对话文件.txt> | less -R

# 示例
python3 show_conversation.py 2025-12-25-this-session-is-being-continued-from-a-previous-co.txt
```

## 颜色说明

| 颜色 | 含义 |
|------|------|
| 绿色背景 | 用户输入 |
| 青色 | Claude 回复 |
| 黄色 | 文件读写操作 |
| 紫色 | Bash 命令 |
| 青色 | 搜索操作 (Glob/Grep) |
| 绿色 | 代码新增行 (+) |
| 红色 | 代码删除行 (-) |

## 对话记录文件

本目录包含的对话记录：

- `2025-12-25-this-session-is-being-continued-from-a-previous-co.txt` - 会话记录1
- `2025-12-25-this-session-is-being-continued-from-a-previous-co2.txt` - 会话记录2

## 依赖

- Python 3.6+
- 无需额外依赖
