# -*- coding: utf-8 -*-
"""校验记忆压缩：索引已瘦身，且 playbook.md 未丢内容。"""
import io, os, sys

BASE = os.path.expanduser("~/.workbuddy")
idx = io.open(os.path.join(BASE, "MEMORY.md"), encoding="utf-8").read()
pb = io.open(os.path.join(BASE, "playbook.md"), encoding="utf-8").read()

print("index   :", len(idx), "chars /", len(idx.encode("utf-8")), "bytes")
print("playbook:", len(pb), "chars /", len(pb.encode("utf-8")), "bytes")
print("index under 6300 chars:", len(idx) < 6300)
print("index under 6300 bytes:", len(idx.encode("utf-8")) < 6300)
print()

# 原 MEMORY.md 逐条特征串（取自原文精确片段）
orig = [
    "41k 字符的记忆文件", "grep` 新内容**命中**", "io.open(p,'w').write(old+new)",
    'grep "^## "`）与**字符数**', "把原文件头整段替换掉", "7 行新增",
    "静默截断**（不报错）", "`APPDATA`/`ProgramData` 为空所致",
    "PROGRAM BLOCKED", "cygpath -w", "awk '!seen[$0]++'", "LISTENING",
    "TIMEOUT.EXE", ".workbuddy/captures/", "本次工具调用结束",
    "`ExitCode` 为空", "Get-PnpDevice", "七·B 节", "就完全不执行",
    "`where dotnet`", "bypasses", "gcc.exe",
    "XPlatMachineWideSetting", "SDK 9.0.308 / 10.0.400",
    "脚本执行期间无法再打开**自身**", "转移控制权而非返回",
    "call :pause_exit 1", "goto :eof", "跑一次成功路径",
    "cmd //c", "Out-File -FilePath out.txt -Encoding utf8", "脚本自身产生的日志",
    "纯英文**（不含中文注释", "`)` 提前闭合 if 块", "`timeout` 钉 System32",
    "for /l` 内 goto 丢变量", "块内读新值用 `!VAR!`",
    "Press any key to close this window", "pause >nul", 'set "PROJ=%~dp0.."',
    "父脚本传 `--no-pause`", "父脚本必须 =1",
    "1107473010@qq.com", "alias_8Hegn2StpYVGGpeJ2pmTFS4rbb_4", "not_bound",
    "created_at", "10 万字符", "email-weekly-", "present_files",
    "msvc_x86_64", "mediamtx_v1.20.1_windows_amd64", "ffmpeg-master-latest-win64-gpl-shared",
    "Software/openocd", "SDL2-2.30.12", "sdl2-config.cmake", ".tar.gz", "不要硬编码",
    "gcc 15.2", "CMAKE_PREFIX_PATH", "SDL2::SDL2", "CMAKE_EXE_LINKER_FLAGS", "libSDL2.dll.a",
    "OV-Watch", "1m36s", "482 个单元", "main.exe", "2.30.1",
    "GPIO48", "Adafruit_NeoPixel", "0x303a", "PID 0x1001", "pyserial", "Get-WmiObject",
    "tud_mounted", "tud_connected", "VBUS", "CONFIG_XXX", "component_requirements.py BUG",
    "idf.py reconfigure", "sdkconfig",
    "diff -u", "mtime", "显式哨兵", "重复标题", "§10.x → §12.x", "stm32-logging-print-log",
    "不回灌 MCU 仓",
    "git push 由用户自己处理", "commit、branch、diff、log", "token / 凭据 / 密钥",
]
miss = [k for k in orig if k not in pb]
print("总检查", len(orig), "条，缺失", len(miss))
print("缺失:", miss if miss else "none")
print()
print("--- index sections ---")
for line in idx.splitlines():
    if line.startswith("## "):
        print("  ", line)
