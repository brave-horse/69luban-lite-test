#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Luban-Lite WiFi 配置助手 —— 单文件版
# 作者: fangjie.wang
# =============================================================================
# 功能概述
#   - 在 SDK 根目录运行，辅助向 target/configs/*.defconfig 合并 WiFi 相关 CONFIG
#   - 支持版型识别、Pinmux 检查、更换方案时先恢复 WiFi 项再写入新配方
#
# 文档: https://aicdoc.artinchip.com/topics/sdk/peripheral/wifi-lite.html
#
# 用法:
#   python wifi_autoconfig.py
#   python wifi_autoconfig.py --help
#   python wifi_autoconfig.py --pinmux-verbose
#
# 代码结构（自上而下）:
#   §1  版本、类型别名、导入、SDK 版本探测、旧版补丁提示
#   §2  defconfig 文本合并（不解析 Kconfig）
#   §3  发现：芯片 / 板型 / defconfig / .config / pinmux / SDMC（Kconfig.board）
#   §4  defconfig 内 WiFi 状态检测与检查报告
#   §5  CONFIG 配方（模组、SDMC、内核/lwIP、重置字典）
#   §6  pinmux.c 分析（分章节输出）
#   §7  交互：提示、向导、main
#   §8  程序入口
# =============================================================================

from __future__ import annotations

import datetime
import os
import re
import sys
import traceback
from dataclasses import dataclass
from enum import Enum
from typing import (
    Any,
    Callable,
    Dict,
    Iterable,
    List,
    Optional,
    Sequence,
    Set,
    Tuple,
    Union,
)

# -----------------------------------------------------------------------------
# §1 版本与公共类型
# -----------------------------------------------------------------------------

__script_version__ = "11.0.0"
__firmware_version__ = "1.2.3"

# V11 相对此前单文件助手的主要更新（展示给用户）
SCRIPT_V11_CHANGELOG = """V11 更新说明:
  · AIC8800 增加子型号选择（AIC8800D40L / AIC8800DL 与 AIC8800DW 合并为同一 Kconfig 项 AIC8800DW）
  · AIC8800 增加射频电源转换方式（DCDC / LDO）选择及与模组厂硬件相关的备注
  · 根据 SDK 版本在配置流程结束后提示历史补丁：SDIO 检测延时（≤1.2.3）、AIC8800 省内存补丁（≤1.3.0，含）
"""

# CONFIG 合并时允许的值：True/False（y / is not set）、整数、字符串
ConfigValue = Union[str, int, bool]


# =============================================================================
# §2 defconfig 文本合并
# =============================================================================

_UNSET_RE = re.compile(r"^#\s*(CONFIG_[A-Za-z0-9_]+)\s+is not set\s*$")
_SET_RE = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")


def _norm_key(name: str) -> str:
    name = name.strip()
    if not name.startswith("CONFIG_"):
        name = "CONFIG_" + name
    return name


def _format_line(key: str, value: ConfigValue) -> str:
    k = _norm_key(key)
    if value is False:
        return f"# {k} is not set"
    if value is True or value == "y":
        return f"{k}=y"
    if isinstance(value, int):
        return f"{k}={value}"
    s = str(value)
    if s in ("y", "n", "m"):
        return f"{k}={s}"
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'{k}="{escaped}"'


def _parse_key_from_line(line: str) -> Optional[str]:
    s = line.strip()
    m = _UNSET_RE.match(s)
    if m:
        return m.group(1)
    m = _SET_RE.match(s)
    if m:
        return m.group(1)
    return None


def merge_defconfig(
    lines: List[str],
    set_items: Dict[str, ConfigValue],
    unset_keys: Optional[Iterable[str]] = None,
) -> List[str]:
    """按 CONFIG 键合并/覆盖 defconfig 行；False 表示写 `# CONFIG_x is not set`。"""
    desired: Dict[str, ConfigValue] = {}
    for k, v in set_items.items():
        desired[_norm_key(k)] = False if v is False else v
    if unset_keys:
        for u in unset_keys:
            desired[_norm_key(u)] = False

    result: List[str] = []
    seen: Set[str] = set()

    for line in lines:
        stripped = line.rstrip("\n\r")
        key = _parse_key_from_line(stripped)
        if key is None:
            result.append(stripped)
            continue
        if key in desired:
            if key not in seen:
                result.append(_format_line(key, desired[key]))
                seen.add(key)
        else:
            result.append(stripped)

    for k, v in desired.items():
        if k not in seen:
            result.append(_format_line(k, v))

    return result


def read_text_safe(path: str) -> Optional[str]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def write_text_safe(path: str, content: str) -> bool:
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        return True
    except OSError:
        return False


_SDK_SEMVER_RE = re.compile(r"(?i)^v?\s*(\d+)\.(\d+)\.(\d+)\b")


def _parse_sdk_semver_tuple(text: str) -> Optional[Tuple[int, int, int]]:
    s = text.strip().splitlines()[0].strip() if text else ""
    m = _SDK_SEMVER_RE.match(s)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def read_luban_sdk_version_tuple(aic_root: str) -> Optional[Tuple[int, int, int]]:
    """
    读取 Luban-Lite SDK 语义化版本 (major, minor, patch)，用于旧版补丁条件判断。
    优先级: 环境变量 LUBAN_LITE_SDK_VERSION → luban_lite_sdk_version.txt → VERSION（首行须为 x.y.z）。
    """
    envv = os.environ.get("LUBAN_LITE_SDK_VERSION", "").strip()
    if envv:
        t = _parse_sdk_semver_tuple(envv)
        if t:
            return t
    for fname in ("luban_lite_sdk_version.txt", "VERSION"):
        p = os.path.join(aic_root, fname)
        raw = read_text_safe(p)
        if raw:
            t = _parse_sdk_semver_tuple(raw)
            if t:
                return t
    return None


def _sdk_ver_le(a: Tuple[int, int, int], b: Tuple[int, int, int]) -> bool:
    return a <= b


# =============================================================================
# §3 发现：芯片、板型、defconfig、工程 .config
# =============================================================================

_DEFCONFIG_NAME_RE = re.compile(
    r"^(?P<chip>[^_]+)_(?P<board>.+)_(?P<kernel>[^_]+)_(?P<app>.+)_defconfig$"
)


@dataclass
class DefconfigInfo:
    filename: str
    chip: str
    board: str
    kernel: str
    app: str

    @property
    def board_path(self) -> str:
        return os.path.join("target", self.chip, self.board)


def parse_defconfig_name(filename: str) -> Optional[DefconfigInfo]:
    m = _DEFCONFIG_NAME_RE.match(filename)
    if not m:
        return None
    return DefconfigInfo(
        filename=filename,
        chip=m.group("chip"),
        board=m.group("board"),
        kernel=m.group("kernel"),
        app=m.group("app"),
    )


def list_chips(aic_root: str) -> List[str]:
    target = os.path.join(aic_root, "target")
    chips: List[str] = []
    skip = frozenset({"configs", ".git"})
    try:
        for name in sorted(os.listdir(target)):
            if name in skip or name.startswith("."):
                continue
            p = os.path.join(target, name)
            if os.path.isdir(p):
                chips.append(name)
    except OSError:
        return []
    return chips


_BOARD_DIR_SKIP = frozenset({"common", "configs", ".git"})


def list_boards(aic_root: str, chip: str) -> List[str]:
    path = os.path.join(aic_root, "target", chip)
    boards: List[str] = []
    try:
        for name in sorted(os.listdir(path)):
            if name in _BOARD_DIR_SKIP or name.startswith("."):
                continue
            p = os.path.join(path, name)
            if os.path.isdir(p):
                boards.append(name)
    except OSError:
        return []
    return boards


def list_defconfigs(aic_root: str, include_bootloader: bool = False) -> List[DefconfigInfo]:
    cfg_dir = os.path.join(aic_root, "target", "configs")
    out: List[DefconfigInfo] = []
    try:
        for fn in sorted(os.listdir(cfg_dir)):
            if not fn.endswith("_defconfig"):
                continue
            if not include_bootloader and "bootloader" in fn.lower():
                continue
            info = parse_defconfig_name(fn)
            if info:
                out.append(info)
    except OSError:
        return []
    return out


def list_chips_from_defconfigs(all_defs: Sequence[DefconfigInfo]) -> List[str]:
    """与 `scons list` 一致：芯片名仅来自 target/configs 下已有（非 bootloader）defconfig。"""
    return sorted({d.chip for d in all_defs})


def list_boards_from_defconfigs(all_defs: Sequence[DefconfigInfo], chip: str) -> List[str]:
    """板型名仅来自 defconfig 文件名中的 board 段，避免 target/<chip>/ 下空目录或无 defconfig 的版型误入选。"""
    return sorted({d.board for d in all_defs if d.chip == chip})


def find_pinmux_path(aic_root: str, chip: str, board: str) -> Optional[str]:
    p = os.path.join(aic_root, "target", chip, board, "pinmux.c")
    if os.path.isfile(p):
        return p
    return None


# 与 `target/<chip>/common/Kconfig.board` 中「config AIC_USING_SDMCn」声明一致，用于判断 menuconfig 是否暴露该控制器
_SDMC_KCONFIG_BOARD_RE = re.compile(
    r"^\s*config\s+AIC_USING_SDMC(\d)\s*$",
    re.MULTILINE,
)


def discover_sdmc_indices_from_kconfig_board(
    aic_root: str, chip: str,
) -> Tuple[List[int], Optional[str]]:
    """
    从板级 Kconfig.board 解析本芯片在 menuconfig 中可出现哪些 AIC_USING_SDMCn。

    若文件缺失或无法解析出任何项，则回退为 [0,1,2] 并返回说明性提示字符串（非 None），
    提醒用户以实际 menuconfig 为准。
    """
    rel = os.path.join("target", chip, "common", "Kconfig.board")
    path = os.path.join(aic_root, rel)
    if not os.path.isfile(path):
        return [0, 1, 2], (
            f"未找到 {rel}，无法按芯片裁剪 SDMC 选项；"
            "下列仍列出 SDMC0/1/2，请打开 menuconfig 核对硬件是否真有对应控制器。"
        )
    text = read_text_safe(path)
    if not text:
        return [0, 1, 2], (
            f"无法读取 {rel}；下列仍列出 SDMC0/1/2，请以 menuconfig 为准。"
        )
    idx_set = {int(m.group(1)) for m in _SDMC_KCONFIG_BOARD_RE.finditer(text)}
    if not idx_set:
        return [0, 1, 2], (
            f"在 {rel} 中未解析到任何 `config AIC_USING_SDMC*`；"
            "下列仍列出 SDMC0/1/2，请以 menuconfig 为准。"
        )
    return sorted(idx_set), None


def read_prj_from_dot_config(aic_root: str) -> Optional[Dict[str, str]]:
    path = os.path.join(aic_root, ".config")
    if not os.path.isfile(path):
        return None
    keys: Dict[str, str] = {}
    mapping = (
        ("CONFIG_PRJ_DEFCONFIG_FILENAME=", "defconfig_filename"),
        ("CONFIG_PRJ_CHIP=", "chip"),
        ("CONFIG_PRJ_BOARD=", "board"),
        ("CONFIG_PRJ_KERNEL=", "kernel"),
        ("CONFIG_PRJ_APP=", "app"),
    )
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                raw = line.strip()
                if not raw or raw.startswith("#"):
                    continue
                for prefix, name in mapping:
                    if raw.startswith(prefix):
                        keys[name] = raw.split("=", 1)[1].strip().strip('"')
    except OSError:
        return None
    return keys if keys else None


def resolve_defconfig_info(
    all_defs: List[DefconfigInfo],
    prj: Dict[str, str],
) -> Optional[DefconfigInfo]:
    fn = prj.get("defconfig_filename")
    if fn:
        for d in all_defs:
            if d.filename == fn:
                return d
    chip = prj.get("chip")
    board = prj.get("board")
    kernel = prj.get("kernel")
    app = prj.get("app")
    if chip and board and kernel and app:
        candidate = f"{chip}_{board}_{kernel}_{app}_defconfig"
        for d in all_defs:
            if d.filename == candidate:
                return d
    return None


# =============================================================================
# §4 defconfig 内 WiFi 状态（检测 / 检查报告）
# =============================================================================


def parse_config_kv_lines(text: str) -> Dict[str, str]:
    """解析 defconfig/.config 行为 CONFIG 键 -> 值（含 __NOT_SET__）。"""
    out: Dict[str, str] = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#") and "is not set" not in s:
            continue
        m = _UNSET_RE.match(s)
        if m:
            out[m.group(1)] = "__NOT_SET__"
            continue
        m = _SET_RE.match(s)
        if m:
            out[m.group(1)] = m.group(2).strip()
    return out


def is_wifi_likely_enabled(kv: Dict[str, str]) -> bool:
    if kv.get("CONFIG_AIC_WIRELESS_LAN") == "y":
        return True
    for k in (
        "CONFIG_AIC_WLAN_AIC8800D40L",
        "CONFIG_AIC_WLAN_REALTEK",
        "CONFIG_AIC_WLAN_HUGEIC",
        "CONFIG_WIFI_USING_SDIOWIFI_ATBM",
        "CONFIG_AIC_WLAN_ASR",
    ):
        if kv.get(k) == "y":
            return True
    return False


def collect_wifi_related_keys(kv: Dict[str, str]) -> Dict[str, str]:
    prefixes = (
        "CONFIG_AIC_WIRELESS",
        "CONFIG_AIC_WLAN_",
        "CONFIG_AIC_DEV_REALTEK",
        "CONFIG_AIC_USING_RTL",
        "CONFIG_WIFI_USING_SDIOWIFI",
        "CONFIG_AIC_WLAN_ASR",
        "CONFIG_CHIP_SELECT_AIC8800",
        "CONFIG_CONFIG_AIC8800",
        "CONFIG_AIC_DEV_AIC8800",
        "CONFIG_REALTEK_",
        "CONFIG_TXW901",
        "CONFIG_HUGEIC",
        "CONFIG_RT_USING_WIFI",
        "CONFIG_RT_WLAN_",
        "CONFIG_AIC_SDMC0_",
        "CONFIG_AIC_SDMC1_",
        "CONFIG_AIC_SDMC2_",
        "CONFIG_AIC_USING_SDMC",
        "CONFIG_AIC_SDMC_IRQ",
        "CONFIG_LPKG_USING_NETUTILS",
        "CONFIG_LPKG_NETUTILS_IPERF",
    )
    found: Dict[str, str] = {}
    for k, v in sorted(kv.items()):
        if any(k.startswith(p) for p in prefixes):
            found[k] = v
    return found


def format_wifi_inspection_report(defconfig_text: str) -> List[str]:
    kv = parse_config_kv_lines(defconfig_text)
    lines: List[str] = []
    if not is_wifi_likely_enabled(kv):
        lines.append("未检测到已启用的 Wireless LAN / WLAN 驱动（CONFIG_AIC_WIRELESS_LAN 与各 WLAN 驱动均为关）。")
        return lines
    lines.append("已检测到 WiFi 相关配置（以下摘自当前 defconfig 中的匹配项）：")
    rel = collect_wifi_related_keys(kv)
    if not rel:
        lines.append("  （无匹配前缀的条目，请用 menuconfig 核对。）")
        return lines
    for k, v in rel.items():
        if v == "__NOT_SET__":
            lines.append(f"  # {k} is not set")
        else:
            lines.append(f"  {k}={v}")
    return lines


# =============================================================================
# §5 CONFIG 配方：模组、SDMC、内核/lwIP、更换方案时的重置
# =============================================================================


class WifiModule(str, Enum):
    AIC8800 = "aic8800"
    RTL8189 = "rtl8189"
    RTL8733 = "rtl8733"
    TXW901 = "txw901"


class Aic8800ChipVariant(str, Enum):
    """AIC8800 子型号；DL 与 DW 在 Kconfig 中合并为 AIC8800DW 选项。"""

    D40L = "d40l"
    DW_OR_DL = "dw_or_dl"


class Aic8800PowerConverter(str, Enum):
    DCDC = "dcdc"
    LDO = "ldo"


def _all_wlan_drivers_off() -> Dict[str, ConfigValue]:
    return {
        "CONFIG_AIC_WLAN_REALTEK": False,
        "CONFIG_AIC_WLAN_AIC8800D40L": False,
        "CONFIG_AIC_WLAN_HUGEIC": False,
        "CONFIG_WIFI_USING_SDIOWIFI_ATBM": False,
        "CONFIG_AIC_WLAN_ASR": False,
    }


def sdmc_sdio_for_wifi(sdmc_index: int) -> Dict[str, ConfigValue]:
    if sdmc_index not in (0, 1, 2):
        return {}
    p: Dict[str, ConfigValue] = {"CONFIG_AIC_SDMC_IRQ_MODE": True}
    if sdmc_index == 0:
        p.update(
            {
                "CONFIG_AIC_USING_SDMC0": True,
                "CONFIG_AIC_SDMC0_BUSWIDTH4": True,
                "CONFIG_AIC_SDMC0_BUSWIDTH1": False,
                "CONFIG_AIC_SDMC0_BUSWIDTH8": False,
                "CONFIG_AIC_SDMC0_IS_SDIO": True,
                "CONFIG_AIC_SDMC0_DRV_PHASE": 3,
                "CONFIG_AIC_SDMC0_SMP_PHASE": 0,
                "CONFIG_AIC_SDMC0_CLK_FREQ": 40000000,
            }
        )
    elif sdmc_index == 1:
        p.update(
            {
                "CONFIG_AIC_USING_SDMC1": True,
                "CONFIG_AIC_SDMC1_BUSWIDTH4": True,
                "CONFIG_AIC_SDMC1_BUSWIDTH1": False,
                "CONFIG_AIC_SDMC1_BUSWIDTH8": False,
                "CONFIG_AIC_SDMC1_USING_HOTPLUG": False,
                "CONFIG_AIC_SDMC1_IS_SDIO": True,
                "CONFIG_AIC_SDMC1_DRV_PHASE": 3,
                "CONFIG_AIC_SDMC1_SMP_PHASE": 0,
                "CONFIG_AIC_SDMC1_CLK_FREQ": 40000000,
            }
        )
    else:
        p.update(
            {
                "CONFIG_AIC_USING_SDMC2": True,
                "CONFIG_AIC_SDMC2_BUSWIDTH4": True,
                "CONFIG_AIC_SDMC2_BUSWIDTH1": False,
                "CONFIG_AIC_SDMC2_BUSWIDTH8": False,
                "CONFIG_AIC_SDMC2_IS_SDIO": True,
                "CONFIG_AIC_SDMC2_DRV_PHASE": 3,
                "CONFIG_AIC_SDMC2_SMP_PHASE": 0,
                "CONFIG_AIC_SDMC2_CLK_FREQ": 40000000,
            }
        )
    return p


def kernel_lwip_bundle_aic8800() -> Dict[str, ConfigValue]:
    return {
        "CONFIG_RT_USING_TIMER_SOFT": True,
        "CONFIG_RT_TIMER_THREAD_PRIO": 7,
        "CONFIG_RT_TIMER_THREAD_STACK_SIZE": 8192,
        "CONFIG_RT_USING_SYSTEM_WORKQUEUE": True,
        "CONFIG_RT_SYSTEM_WORKQUEUE_STACKSIZE": 8192,
        "CONFIG_RT_USING_SDIO": True,
        "CONFIG_RT_SDIO_STACK_SIZE": 16384,
        "CONFIG_RT_SDIO_THREAD_PRIORITY": 5,
        "CONFIG_RT_USING_WIFI": True,
        "CONFIG_RT_WLAN_WORKQUEUE_THREAD_SIZE": 8192,
        "CONFIG_RT_WLAN_DEBUG": True,
        "CONFIG_RT_WLAN_CMD_DEBUG": True,
        "CONFIG_RT_USING_SAL": False,
        "CONFIG_RT_USING_NETDEV": True,
        "CONFIG_RT_USING_LWIP": True,
        "CONFIG_RT_LWIP_TCPTHREAD_PRIORITY": 3,
        "CONFIG_RT_LWIP_TCPTHREAD_STACKSIZE": 8192,
        "CONFIG_RT_LWIP_REASSEMBLY_FRAG": True,
        "CONFIG_RT_LWIP_NETIF_LOOPBACK": True,
        "CONFIG_LPKG_USING_LWIP": False,
    }


def kernel_lwip_bundle_realtek() -> Dict[str, ConfigValue]:
    return {
        "CONFIG_RT_USING_TIMER_SOFT": True,
        "CONFIG_RT_TIMER_THREAD_PRIO": 7,
        "CONFIG_RT_TIMER_THREAD_STACK_SIZE": 16384,
        "CONFIG_RT_USING_SYSTEM_WORKQUEUE": True,
        "CONFIG_RT_SYSTEM_WORKQUEUE_STACKSIZE": 8192,
        "CONFIG_RT_USING_SDIO": True,
        "CONFIG_RT_SDIO_STACK_SIZE": 16384,
        "CONFIG_RT_SDIO_THREAD_PRIORITY": 5,
        "CONFIG_RT_USING_WIFI": True,
        "CONFIG_RT_WLAN_PROT_LWIP_PBUF_FORCE": True,
        "CONFIG_RT_WLAN_DEBUG": True,
        "CONFIG_RT_WLAN_CMD_DEBUG": True,
        "CONFIG_RT_USING_SAL": False,
        "CONFIG_RT_USING_NETDEV": True,
        "CONFIG_RT_USING_LWIP": True,
        "CONFIG_RT_LWIP_TCPTHREAD_PRIORITY": 3,
        "CONFIG_RT_LWIP_TCPTHREAD_STACKSIZE": 8192,
        "CONFIG_LPKG_USING_LWIP": False,
    }


def kernel_lwip_bundle_txw901() -> Dict[str, ConfigValue]:
    return {
        "CONFIG_RT_USING_TIMER_SOFT": True,
        "CONFIG_RT_TIMER_THREAD_PRIO": 7,
        "CONFIG_RT_TIMER_THREAD_STACK_SIZE": 8192,
        "CONFIG_RT_USING_SYSTEM_WORKQUEUE": True,
        "CONFIG_RT_SYSTEM_WORKQUEUE_STACKSIZE": 8192,
        "CONFIG_RT_USING_SDIO": True,
        "CONFIG_RT_SDIO_STACK_SIZE": 8192,
        "CONFIG_RT_SDIO_THREAD_PRIORITY": 3,
        "CONFIG_RT_USING_WIFI": True,
        "CONFIG_RT_WLAN_PROT_LWIP_PBUF_FORCE": True,
        "CONFIG_RT_WLAN_WORKQUEUE_THREAD_SIZE": 4096,
        "CONFIG_RT_WLAN_DEBUG": True,
        "CONFIG_RT_WLAN_CMD_DEBUG": True,
        "CONFIG_RT_USING_SAL": False,
        "CONFIG_RT_USING_NETDEV": True,
        "CONFIG_RT_USING_LWIP": True,
        "CONFIG_RT_LWIP_TCPTHREAD_PRIORITY": 4,
        "CONFIG_LPKG_USING_LWIP": False,
    }


def build_wifi_profile(
    module: WifiModule,
    *,
    power_gpio: str = "PD.7",
    aic8800_variant: Optional[Aic8800ChipVariant] = None,
    aic8800_power_converter: Optional[Aic8800PowerConverter] = None,
    enable_bt_aic8800: bool = False,
    aic8800_debug_log: bool = False,
    realtek_debug: bool = True,
    realtek_full_function: bool = False,
    realtek_ip: str = "192.168.3.20",
    realtek_gw: str = "192.168.3.1",
    realtek_mask: str = "255.255.255.0",
) -> Tuple[Dict[str, ConfigValue], List[str]]:
    notes: List[str] = []
    base = _all_wlan_drivers_off()
    base["CONFIG_AIC_WIRELESS_LAN"] = True

    if module == WifiModule.AIC8800:
        variant = aic8800_variant or Aic8800ChipVariant.D40L
        if aic8800_power_converter is None:
            pconv = (
                Aic8800PowerConverter.DCDC
                if variant == Aic8800ChipVariant.D40L
                else Aic8800PowerConverter.LDO
            )
        else:
            pconv = aic8800_power_converter
        is_d40l = variant == Aic8800ChipVariant.D40L
        base.update(
            {
                "CONFIG_AIC_WLAN_AIC8800D40L": True,
                "CONFIG_CHIP_SELECT_AIC8800D40L": is_d40l,
                "CONFIG_CHIP_SELECT_AIC8800DW": not is_d40l,
                "CONFIG_CONFIG_AIC8800_VRF_DCDC_MODE": pconv == Aic8800PowerConverter.DCDC,
                "CONFIG_CONFIG_AIC8800_VRF_LDO_MODE": pconv == Aic8800PowerConverter.LDO,
                "CONFIG_CONFIG_AIC8800_NORMAL_MODE": True,
                "CONFIG_CONFIG_AIC8800_RFTEST_MODE": False,
                "CONFIG_CONFIG_AIC8800_BT_SUPPORT": enable_bt_aic8800,
                "CONFIG_AIC_WIRELESS_PWR_GPIO": power_gpio,
                "CONFIG_AIC_DEV_AIC8800_DEBUG_LOG": aic8800_debug_log,
            }
        )
        notes.append(
            "AIC8800 射频电源（DCDC / LDO）与模组厂硬件相关：请模组厂确认或自行尝试选择（可能影响信号强度等指标）。"
        )
        if enable_bt_aic8800:
            notes.append("已打开 AIC8800 BT：请确认硬件连接并在 menuconfig 中填写 bt reset GPIO（若需要）。")

    elif module in (WifiModule.RTL8189, WifiModule.RTL8733):
        base["CONFIG_AIC_WLAN_REALTEK"] = True
        if module == WifiModule.RTL8189:
            base["CONFIG_AIC_USING_RTL8189_WLAN0"] = True
            base["CONFIG_AIC_USING_RTL8733_WLAN0"] = False
        else:
            base["CONFIG_AIC_USING_RTL8189_WLAN0"] = False
            base["CONFIG_AIC_USING_RTL8733_WLAN0"] = True
        base.update(
            {
                "CONFIG_AIC_WIRELESS_PWR_GPIO": power_gpio,
                "CONFIG_AIC_DEV_REALTEK_WLAN0_IPADDR": realtek_ip,
                "CONFIG_AIC_DEV_REALTEK_WLAN0_GW": realtek_gw,
                "CONFIG_AIC_DEV_REALTEK_WLAN0_NETMASK": realtek_mask,
                "CONFIG_AIC_DEV_REALTEK_DEBUG": realtek_debug,
                "CONFIG_REALTEK_FULL_FNC_MODE": realtek_full_function,
            }
        )
        if realtek_full_function:
            notes.append("已打开 Realtek full function：文档说明用于 P2P/蓝牙等场景。")

    elif module == WifiModule.TXW901:
        base["CONFIG_AIC_WLAN_HUGEIC"] = True
        base["CONFIG_AIC_WIRELESS_PWR_GPIO"] = power_gpio
        base["CONFIG_TXW901_MANUAL_PROV_MODE"] = True
        base["CONFIG_TXW901_BLE_BROADCAST_PROV_MODE"] = False
        base["CONFIG_TXW901_BLE_CONNECT_PROV_MODE"] = False
        notes.append("TXW901：文档示例电源脚为 PD.17，请按实际硬件修改。")

    return base, notes


def netutils_iperf_hint() -> Dict[str, ConfigValue]:
    return {
        "CONFIG_LPKG_USING_NETUTILS": True,
        "CONFIG_LPKG_NETUTILS_IPERF": True,
    }


def test_commands_text() -> str:
    return """wifi -d mode sta wlan0      // 初始化 WiFi，并设置为 STA 模式
wifi join SSID PASSWORD   // 连接 AP SSID是热点，PASSWORD是密码
ping 192.168.1.1            // 假设网关 IP 为 192.168.1.1，ping 一下验证通路
wifi disc                   // 断开 WiFi 连接
wifi -d mode ap wlan1         // 设备 WiFi 为 AP 模式
wifi ap SSID PASSWORD       // 生成 AP热点，SSID是热点，PASSWORD是密码
"""


def _sdmc_clear_sdio_wifi_tuning(sdmc_index: int) -> Dict[str, ConfigValue]:
    if sdmc_index == 0:
        return {
            "CONFIG_AIC_SDMC0_IS_SDIO": False,
            "CONFIG_AIC_SDMC0_DRV_PHASE": 3,
            "CONFIG_AIC_SDMC0_SMP_PHASE": 0,
            "CONFIG_AIC_SDMC0_CLK_FREQ": 100000000,
        }
    if sdmc_index == 1:
        return {
            "CONFIG_AIC_SDMC1_IS_SDIO": False,
            "CONFIG_AIC_SDMC1_DRV_PHASE": 3,
            "CONFIG_AIC_SDMC1_SMP_PHASE": 0,
            "CONFIG_AIC_SDMC1_CLK_FREQ": 100000000,
        }
    if sdmc_index == 2:
        return {
            "CONFIG_AIC_SDMC2_IS_SDIO": False,
            "CONFIG_AIC_SDMC2_DRV_PHASE": 3,
            "CONFIG_AIC_SDMC2_SMP_PHASE": 0,
            "CONFIG_AIC_SDMC2_CLK_FREQ": 100000000,
        }
    return {}


def wifi_reset_merge_dict() -> Dict[str, ConfigValue]:
    """更换 WiFi 方案前：关闭本脚本涉及的无线相关 CONFIG（不关闭通用 LwIP/NETDEV 等）。"""
    d: Dict[str, ConfigValue] = {}
    d.update(_all_wlan_drivers_off())
    d["CONFIG_AIC_WIRELESS_LAN"] = False
    d["CONFIG_RT_USING_WIFI"] = False
    d["CONFIG_RT_WLAN_DEBUG"] = False
    d["CONFIG_RT_WLAN_CMD_DEBUG"] = False
    d["CONFIG_RT_WLAN_PROT_LWIP_PBUF_FORCE"] = False
    d["CONFIG_RT_WLAN_WORKQUEUE_THREAD_SIZE"] = 2048

    d["CONFIG_CHIP_SELECT_AIC8800D40L"] = False
    d["CONFIG_CHIP_SELECT_AIC8800DW"] = False
    d["CONFIG_CONFIG_AIC8800_VRF_DCDC_MODE"] = False
    d["CONFIG_CONFIG_AIC8800_VRF_LDO_MODE"] = False
    d["CONFIG_CONFIG_AIC8800_NORMAL_MODE"] = False
    d["CONFIG_CONFIG_AIC8800_RFTEST_MODE"] = False
    d["CONFIG_CONFIG_AIC8800_BT_SUPPORT"] = False
    d["CONFIG_AIC_DEV_AIC8800_DEBUG_LOG"] = False
    d["CONFIG_AIC_DEV_AIC8800_BT_RST_GPIO"] = False
    d["CONFIG_AIC_DEV_AIC8800_WLAN0_RST_GPIO"] = False

    d["CONFIG_AIC_WIRELESS_PWR_GPIO"] = False

    d["CONFIG_AIC_USING_RTL8189_WLAN0"] = False
    d["CONFIG_AIC_USING_RTL8733_WLAN0"] = False
    d["CONFIG_AIC_DEV_REALTEK_WLAN0_IPADDR"] = False
    d["CONFIG_AIC_DEV_REALTEK_WLAN0_GW"] = False
    d["CONFIG_AIC_DEV_REALTEK_WLAN0_NETMASK"] = False
    d["CONFIG_AIC_DEV_REALTEK_DEBUG"] = False
    d["CONFIG_REALTEK_FULL_FNC_MODE"] = False

    d["CONFIG_TXW901_MANUAL_PROV_MODE"] = False
    d["CONFIG_TXW901_BLE_BROADCAST_PROV_MODE"] = False
    d["CONFIG_TXW901_BLE_CONNECT_PROV_MODE"] = False
    d["CONFIG_HUGEIC_TXW901_DEBUG_LOG"] = False

    d["CONFIG_LPKG_USING_NETUTILS"] = False
    d["CONFIG_LPKG_NETUTILS_IPERF"] = False

    for idx in (0, 1, 2):
        d.update(_sdmc_clear_sdio_wifi_tuning(idx))

    return d


# =============================================================================
# §6 pinmux.c 分析（正则解析表项，按章节返回）
# =============================================================================

_PIN_ROW_RE = re.compile(
    r"\{\s*\d+\s*,\s*PIN_[A-Za-z0-9_]+\s*,\s*\d+\s*,\s*"
    r'(?:"([A-Za-z]{1,4}\.\d+)"|([A-Za-z_][A-Za-z0-9_]*))\s*\}\s*,?\s*'
)

_PM_IFDEF_RE = re.compile(r"^\s*#ifdef\s+(\w+)\s*$")
_PM_IFNDEF_RE = re.compile(r"^\s*#ifndef\s+(\w+)\s*$")
_PM_IF_DEFINED_RE = re.compile(r"^\s*#if\s+defined\s*\(\s*(\w+)\s*\)\s*$")
_PM_ENDIF_RE = re.compile(r"^\s*#endif\s*(/\*.*\*/)?\s*$")

_WIFI_BLOCK_SIMPLE = re.compile(
    r"#ifdef\s+AIC_WIRELESS_LAN\s*(.*?)\s*#endif",
    re.DOTALL | re.MULTILINE,
)


def normalize_gpio_name(s: str) -> str:
    t = s.strip().strip('"').strip("'")
    if "." not in t:
        return t
    a, b = t.split(".", 1)
    return f"{a.upper()}.{b.strip()}"


def _pm_strip_line_comment(code: str) -> str:
    if "//" not in code:
        return code
    return code.split("//", 1)[0].rstrip()


def _pm_is_line_commented_out(line: str) -> bool:
    s = line.lstrip()
    return s.startswith("//") or s.startswith("/*")


@dataclass
class _PinRow:
    line_no: int
    raw: str
    ifdef_stack: Tuple[str, ...]
    pin_literal: Optional[str]
    pin_macro: Optional[str]


def _pm_parse_if_directive(line: str) -> Optional[Tuple[str, str]]:
    m = _PM_IFDEF_RE.match(line)
    if m:
        return "push", m.group(1)
    m = _PM_IFNDEF_RE.match(line)
    if m:
        return "push", "!" + m.group(1)
    m = _PM_IF_DEFINED_RE.match(line)
    if m:
        return "push", m.group(1)
    if _PM_ENDIF_RE.match(line):
        return "pop", ""
    return None


def _extract_pin_rows(text: str) -> List[_PinRow]:
    lines = text.splitlines()
    stack: List[str] = []
    stack_tuple = lambda: tuple(stack)
    out: List[_PinRow] = []

    for i, line in enumerate(lines, 1):
        if _pm_is_line_commented_out(line):
            continue
        code = _pm_strip_line_comment(line)
        direc = _pm_parse_if_directive(code)
        if direc:
            kind, name = direc
            if kind == "push":
                stack.append(name)
            elif kind == "pop":
                if stack:
                    stack.pop()
            continue

        m = _PIN_ROW_RE.search(code)
        if not m:
            continue
        lit, macro = m.group(1), m.group(2)
        out.append(
            _PinRow(
                line_no=i,
                raw=code.strip(),
                ifdef_stack=stack_tuple(),
                pin_literal=lit,
                pin_macro=macro if not lit else None,
            )
        )
    return out


def extract_sdmc_pinmux_table_lines(text: str, sdmc_index: int) -> List[str]:
    """
    从 pinmux.c 正文中提取 `#ifdef AIC_USING_SDMCn` 段内、能被表项正则匹配的各行（与源文件行内容一致，便于对照硬件）。
    """
    if sdmc_index not in (0, 1, 2):
        return []
    tag = f"AIC_USING_SDMC{sdmc_index}"
    rows = _extract_pin_rows(text)
    return [r.raw for r in rows if tag in r.ifdef_stack]


def extract_sdmc_pinmux_line_slots(text: str, sdmc_index: int) -> List[Tuple[int, str]]:
    """返回 SDMC 段内各表项的 (1-based 行号, 文件中该行完整文本)，用于写回 pinmux.c。"""
    if sdmc_index not in (0, 1, 2):
        return []
    tag = f"AIC_USING_SDMC{sdmc_index}"
    rows = _extract_pin_rows(text)
    lines = text.splitlines()
    out: List[Tuple[int, str]] = []
    for r in rows:
        if tag not in r.ifdef_stack:
            continue
        if 1 <= r.line_no <= len(lines):
            out.append((r.line_no, lines[r.line_no - 1]))
    return out


_SDIO_TABLE_ROW_PARSE = re.compile(
    r"^\s*\{\s*(\d+)\s*,\s*(PIN_[A-Za-z0-9_]+)\s*,\s*(\d+)\s*,\s*"
    r'"([A-Za-z]{1,4}\.\d+)"\s*\}\s*,?\s*$'
)


def pinmux_doc_path_hint(chip: str) -> str:
    """文档中心内引脚复用章节路径（与芯片目录名一致，如 d21x）。"""
    return f"产品文档/{chip}/用户手册/引脚复用"


def default_sdmc_pinmux_example_lines(sdmc_index: int) -> List[str]:
    """无 SDMC 表项时给出的参考结构（需按手册与硬件替换 Px.x）。"""
    tag = f"AIC_USING_SDMC{sdmc_index}"
    return [
        f"#ifdef {tag}",
        '    {2, PIN_PULL_UP, 3, "Px.x"},  // CLK — 按手册「引脚复用」与原理图填写',
        '    {2, PIN_PULL_UP, 3, "Px.x"},  // CMD',
        '    {2, PIN_PULL_UP, 3, "Px.x"},  // DAT0',
        "    // … 补充 DAT1~DAT3 等，模式/上下拉以手册为准",
        "#endif",
    ]


def parse_sdmc_pinmux_row_fields(line: str) -> Optional[Tuple[str, str, str, str]]:
    """解析一行标准 pinmux 表项，返回 (复用模式, 上下拉宏, 驱动强度, GPIO 引脚名)。"""
    m = _SDIO_TABLE_ROW_PARSE.match(line.strip())
    if not m:
        return None
    return m.group(1), m.group(2), m.group(3), m.group(4)


def replace_gpio_in_sdmc_pinmux_line(line: str, new_gpio: str) -> str:
    """对去掉首尾空白的表项行，仅替换引脚字符串。"""
    n = normalize_gpio_name(new_gpio)

    def _repl(m: re.Match[str]) -> str:
        return m.group(1) + '"' + n + '"'

    return re.sub(
        r'(\{\s*\d+\s*,\s*PIN_[A-Za-z0-9_]+\s*,\s*\d+\s*,\s*)"[^"]+"',
        _repl,
        line.strip(),
        count=1,
    )


def ensure_pinmux_table_row_line_format(body: str) -> str:
    """
    规范 pinmux 表项行：`}` 后应有 `,`（数组元素常见漏写）。
    若 `}` 后为空则追加 `,`；若为行尾注释 `//` 且中间无 `,` 则插入 `, `。
    """
    s = body.strip()
    if not s.startswith("{") or "PIN_" not in s:
        return s
    rbrace = s.rfind("}")
    if rbrace < 0:
        return s
    tail = s[rbrace + 1 :].strip()
    if not tail:
        return s + ","
    if tail.startswith(","):
        return s
    if tail.startswith("//"):
        return s[: rbrace + 1] + ", " + tail
    return s


def _leading_indent_of_line(line: str) -> str:
    m = re.match(r"^(\s*)", line)
    return m.group(1) if m else ""


def apply_gpio_edit_to_pinmux_file_line(full_line: str, new_gpio: str) -> str:
    """保留行首缩进，仅替换 GPIO 名。"""
    ind = _leading_indent_of_line(full_line)
    core = full_line[len(ind) :].strip().rstrip("\r")
    core = ensure_pinmux_table_row_line_format(replace_gpio_in_sdmc_pinmux_line(core, new_gpio))
    return ind + core


def apply_full_pinmux_row_edit_to_file_line(full_line: str, new_row_text: str) -> str:
    """用户粘贴整行表项时，保留原行缩进，并规范末尾逗号。"""
    ind = _leading_indent_of_line(full_line)
    core = ensure_pinmux_table_row_line_format(new_row_text.strip().rstrip("\r"))
    return ind + core


def write_pinmux_line_replacements(
    path: str,
    original_text: str,
    replacements: List[Tuple[int, str]],
) -> bool:
    """
    按 1-based 行号替换 pinmux.c 内容。行号应对应 original_text 解析结果；同一行号多次出现则以后者为准。
    """
    if not replacements:
        return True
    lines = original_text.splitlines()
    by_line: Dict[int, str] = {}
    for ln, text in replacements:
        by_line[ln] = text
    for ln, new_text in by_line.items():
        if ln < 1 or ln > len(lines):
            return False
        lines[ln - 1] = new_text
    new_body = "\n".join(lines)
    if original_text.endswith("\n") or (original_text and original_text[-1] in "\r\n"):
        new_body += "\n"
    return write_text_safe(path, new_body)


def _literal_index(rows: Sequence[_PinRow]) -> Dict[str, List[Tuple[int, Tuple[str, ...]]]]:
    idx: Dict[str, List[Tuple[int, Tuple[str, ...]]]] = {}
    for r in rows:
        if r.pin_literal:
            k = normalize_gpio_name(r.pin_literal)
            idx.setdefault(k, []).append((r.line_no, r.ifdef_stack))
    return idx


def suggest_wifi_pinmux_snippet() -> str:
    return (
        "#ifdef AIC_WIRELESS_LAN\n"
        "    {1, PIN_PULL_DIS, 3, AIC_WIRELESS_PWR_GPIO},  // WIFI_PWR_ON\n"
        "#endif"
    )


def _ordered_unique_macros_from_non_wifi(
    non_wifi: List[Tuple[int, Tuple[str, ...]]],
) -> List[str]:
    """从非 WiFi 分支的 #ifdef 栈中按出现顺序收集宏名（含 #ifndef 的 ! 前缀）。"""
    order: List[str] = []
    seen: Set[str] = set()
    for _ln, st in non_wifi:
        for m in st:
            if m not in seen:
                seen.add(m)
                order.append(m)
    return order


def _overlap_section_config_hints(
    kv: Dict[str, str], non_wifi: List[Tuple[int, Tuple[str, ...]]]
) -> List[str]:
    """
    将 pinmux 条件宏与 defconfig 中 CONFIG_* 对照，便于判断是否可能与 WiFi 同时编译进 pinmux。
    """
    macros = _ordered_unique_macros_from_non_wifi(non_wifi)
    if not macros:
        return []
    lines: List[str] = [
        "当前合并预览 defconfig 中对应 CONFIG 状态（可与 menuconfig 对照）：",
    ]
    for macro in macros:
        neg = macro.startswith("!")
        base = macro[1:] if neg else macro
        key = f"CONFIG_{base}"
        val = kv.get(key)
        if val is None or val == "__NOT_SET__":
            stxt = "未置 y（未出现或 is not set）"
        elif val == "y":
            stxt = "y（已启用）"
        elif val == "n":
            stxt = "n（显式关闭）"
        else:
            stxt = repr(val)
        branch = "#ifndef" if neg else "#ifdef"
        lines.append(
            f"{macro} → {key}={stxt}（{branch} 段是否参与编译取决于该符号）"
        )
    return lines


def analyze_pinmux_sections(
    pinmux_path: str,
    *,
    expected_pwr_gpio: Optional[str] = None,
    sdmc_index: Optional[int] = None,
    verbose_duplicate_scan: bool = False,
    config_kv: Optional[Dict[str, str]] = None,
    chip: Optional[str] = None,
) -> List[Tuple[str, List[str]]]:
    sections: List[Tuple[str, List[str]]] = []

    if not pinmux_path or not os.path.isfile(pinmux_path):
        return [("状态", ["未找到 pinmux.c，已跳过板级引脚检查。"])]

    try:
        with open(pinmux_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        return [("状态", [f"读取 pinmux 失败: {e}"])]

    rows = _extract_pin_rows(text)
    lit_index = _literal_index(rows)

    if "AIC_WIRELESS_LAN" not in text:
        bullets = [
            "文件中未出现 AIC_WIRELESS_LAN；若使用 WiFi，请在 aic_pinmux_config[] 中增加电源脚配置。",
            "可参考：",
        ]
        for snip_line in suggest_wifi_pinmux_snippet().splitlines():
            bullets.append(f"    {snip_line}")
        sections.append(("WiFi 段", bullets))
        return sections

    mblk = _WIFI_BLOCK_SIMPLE.search(text)
    wifi_rows = [r for r in rows if "AIC_WIRELESS_LAN" in r.ifdef_stack]

    wifi_bullets: List[str] = []
    if not mblk and not wifi_rows:
        sections.append(
            (
                "WiFi 段",
                [
                    "含 AIC_WIRELESS_LAN 字样，但未解析到标准 #ifdef … #endif 或表项，请人工核对语法。",
                ],
            )
        )
        return sections

    if wifi_rows:
        if len(wifi_rows) > 1:
            wifi_bullets.append(
                f"本段解析到 {len(wifi_rows)} 条表项（行 {', '.join(str(r.line_no) for r in wifi_rows)}），请确认是否仅需一条电源脚。"
            )
        wr = wifi_rows[0]
        if wr.pin_macro == "AIC_WIRELESS_PWR_GPIO":
            wifi_bullets.append(
                "WiFi 电源使用宏 AIC_WIRELESS_PWR_GPIO（与 Kconfig → rtconfig 中的字符串一致）。"
            )
            if expected_pwr_gpio:
                exp = normalize_gpio_name(expected_pwr_gpio)
                wifi_bullets.append(f"本向导将写入 CONFIG_AIC_WIRELESS_PWR_GPIO=\"{exp}\"，请与原理图核对。")
        elif wr.pin_literal:
            pl = normalize_gpio_name(wr.pin_literal)
            wifi_bullets.append(f"WiFi 电源使用字面量 \"{pl}\"。")
            if expected_pwr_gpio:
                exp = normalize_gpio_name(expected_pwr_gpio)
                if pl != exp:
                    wifi_bullets.append(
                        f"与向导将写入的 \"{exp}\" 不一致，请统一 pinmux 与 defconfig。"
                    )
                else:
                    wifi_bullets.append("与向导将写入的电源脚字符串一致。")
        else:
            wifi_bullets.append(
                f"第 {wr.line_no} 行使用宏 {wr.pin_macro}，请确认与 WiFi 电源定义一致。"
            )
    else:
        blk = mblk.group(1) if mblk else ""
        if "AIC_WIRELESS_PWR_GPIO" in blk:
            wifi_bullets.append(
                "存在 #ifdef AIC_WIRELESS_LAN 且含 AIC_WIRELESS_PWR_GPIO，但未匹配到表项正则，请检查缩进/格式。"
            )
        else:
            wifi_bullets.append("#ifdef AIC_WIRELESS_LAN 块存在，请补全 AIC_WIRELESS_PWR_GPIO 或字面量。")

    if wifi_bullets:
        sections.append(("WiFi 电源脚", wifi_bullets))

    if expected_pwr_gpio:
        exp = normalize_gpio_name(expected_pwr_gpio)
        occ = lit_index.get(exp, [])
        non_wifi = [o for o in occ if "AIC_WIRELESS_LAN" not in o[1]]
        if non_wifi:
            overlap = []
            for ln, st in non_wifi:
                ctx = " / ".join(st) if st else "顶层"
                overlap.append(f"行 {ln}（#ifdef: {ctx}）")
            bullets: List[str] = [
                f"GPIO \"{exp}\" 在下列非 WiFi 条件段也出现：",
                *overlap,
                "若相关 Kconfig 会与 Wireless LAN 同时打开，可能争用同一管脚。",
            ]
            if config_kv:
                bullets.extend(_overlap_section_config_hints(config_kv, non_wifi))
            sections.append(
                (
                    "引脚重叠（请核对是否会与 WiFi 同时使能）",
                    bullets,
                )
            )

    if verbose_duplicate_scan:
        dup_lines: List[str] = []
        for pin, locs in sorted(lit_index.items()):
            if len(locs) <= 1:
                continue
            stacks = [st for _, st in locs]
            uniq_ctx = {tuple(s) for s in stacks}
            if len(uniq_ctx) <= 1:
                continue
            parts = []
            for ln, st in locs:
                ctx = " / ".join(st) if st else "(顶层)"
                parts.append(f"{pin} @ 行 {ln} [{ctx}]")
            dup_lines.append("； ".join(parts))
        if dup_lines:
            sections.append(("GPIO 字面量多处分支（详列）", dup_lines))
    else:
        dup_count = sum(
            1
            for locs in lit_index.values()
            if len(locs) > 1 and len({tuple(st) for _, st in locs}) > 1
        )
        if dup_count:
            sections.append(
                (
                    "GPIO 字面量统计",
                    [
                        f"约 {dup_count} 个 GPIO 在多个 #ifdef 分支中出现（多面板/显示模式互斥时常见，一般可忽略）。",
                        "逐项列出：设置环境变量 WIFI_AUTOCFG_PINMUX_VERBOSE=1 或使用 --pinmux-verbose。",
                    ],
                )
            )

    if sdmc_index is not None and sdmc_index in (0, 1, 2):
        tag = f"AIC_USING_SDMC{sdmc_index}"
        has_ifdef = f"#ifdef {tag}" in text or f"#if defined({tag})" in text
        sdmc_lines = extract_sdmc_pinmux_table_lines(text, sdmc_index)
        sdmc_hw_notes = (
            "SDIO WiFi 依赖所选 SDMC 在 pinmux.c 中的表项，须与原理图、PCB 及模组焊接一致（硬件设计相关）。",
            "本向导不自动改写 pinmux.c；改板或换 SDMC 时请对照手册自行调整。",
        )
        bullets_sdmc: List[str] = []
        if chip:
            bullets_sdmc.append(
                f"引脚复用说明请查阅文档中心：{pinmux_doc_path_hint(chip)}。"
            )
        bullets_sdmc.append(f"条件宏：`{tag}`（与所选 SDMC{sdmc_index} 对应）。")
        if sdmc_lines:
            bullets_sdmc.append("当前 pinmux 中该段已解析到的表项（节选，与源文件一致）：")
            bullets_sdmc.extend(sdmc_lines)
        elif has_ifdef:
            bullets_sdmc.append(
                f"已存在 `{tag}` 条件块，但未匹配到标准 `{{…}}` 表项行；请打开 pinmux.c 人工核对。"
            )
        else:
            bullets_sdmc.append(
                f"未找到 `{tag}` 条件块或表项；若 WiFi 使用该 SDMC，请在 pinmux.c 中补充。"
            )
            bullets_sdmc.append("可参考缺省结构（引脚名须按手册与硬件填写）：")
            bullets_sdmc.extend(default_sdmc_pinmux_example_lines(sdmc_index))
        bullets_sdmc.extend(sdmc_hw_notes)
        if sdmc_lines and has_ifdef:
            bullets_sdmc.append(
                f"请确认：`{tag}` 段内引脚与模组所接物理 SDMC 口、线序是否与当前硬件一致。"
            )
        sections.append(("SDMC（与 SDIO WiFi）", bullets_sdmc))

    return sections if sections else [("Pinmux", ["分析完成（无额外要点）。"])]


def analyze_pinmux(
    pinmux_path: str,
    *,
    expected_pwr_gpio: Optional[str] = None,
    sdmc_index: Optional[int] = None,
    verbose_duplicate_scan: bool = False,
    config_kv: Optional[Dict[str, str]] = None,
    chip: Optional[str] = None,
) -> List[str]:
    flat: List[str] = []
    for title, bullets in analyze_pinmux_sections(
        pinmux_path,
        expected_pwr_gpio=expected_pwr_gpio,
        sdmc_index=sdmc_index,
        verbose_duplicate_scan=verbose_duplicate_scan,
        config_kv=config_kv,
        chip=chip,
    ):
        flat.append(f"[{title}]")
        for b in bullets:
            flat.append(f"  {b}")
    return flat


# =============================================================================
# §7 交互：安全输出、输入、向导、合并、主流程
# =============================================================================


def _safe_print(msg: str = "", **kwargs: Any) -> None:
    try:
        print(msg, **kwargs)
    except Exception:
        pass


def _log_wifi_autoconfig_error(aic_root: str, exc: BaseException) -> Optional[str]:
    """将异常追加写入 SDK 根目录 wifi_autoconfig_error.log，便于排查闪退或偶发失败。"""
    log_path = os.path.join(aic_root, "wifi_autoconfig_error.log")
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    block = (
        f"\n======== {ts} ========\n"
        + "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    )
    try:
        with open(log_path, "a", encoding="utf-8", newline="\n") as f:
            f.write(block)
        return log_path
    except OSError:
        return None


def _prompt_line(allow_quit: bool = True) -> Optional[str]:
    try:
        s = input().strip()
    except EOFError:
        return None
    except Exception:
        return None
    if allow_quit and s.lower() in ("q", "quit", "exit"):
        return None
    return s


def _pause_until_quit_for_review() -> None:
    """在「仅预览 / 写入」之后暂停界面，只接受 q 退出，便于用户核对上方输出。"""
    _safe_print("")
    _safe_print("--- 暂停（核对输出）---")
    _safe_print("  请确认上方变更摘要与 Pinmux 检查。此处仅暂停界面，输入 [q] 后继续显示后续 WiFi 测试命令。")
    while True:
        _safe_print("  [q] 继续: ", end="")
        try:
            raw = input().strip()
        except EOFError:
            return
        except Exception:
            return
        if raw.lower() in ("q", "quit", "exit"):
            _safe_print("  已确认，继续。")
            return
        _safe_print("  请仅输入 q 继续。")


def _pause_until_quit_only(message: str) -> None:
    """只用于兜底暂停：输出 message 后，仅接受 q/quit/exit 退出。"""
    _safe_print("")
    if message:
        _safe_print(message)
    while True:
        _safe_print("  [q] 退出（结束脚本）: ", end="")
        try:
            raw = input().strip()
        except EOFError:
            return
        except Exception:
            return
        if raw.lower() in ("q", "quit", "exit"):
            return
        _safe_print("  请仅输入 q 退出。")


def _print_legacy_sdk_patch_hints(
    sdk_ver: Optional[Tuple[int, int, int]],
    module: WifiModule,
) -> None:
    """在配置流程末尾提示旧版 SDK 所需的手动补丁（脚本不会自动修改源码）。"""
    _safe_print("")
    _safe_print(
        "*****注意:旧版 SDK 补丁提示（本脚本不会自动添加，需要手动添加）*****"
    )
    if sdk_ver is None:
        _safe_print(
            "1）1.2.3之前(含)版本SDK在/kernel/rt-thread/components/drivers/sdio/mmcsd_core.c，"
            "mmcsd_detect while(1)下面增加一个500mS的延时，具体咨询fae"
        )
        _safe_print(
            "2）1.3.0之前(含)版本SDK，如果选用的是AIC8800的wifi增加一个补丁可以减少wifi模块对内存占用。"
        )
        return

    vstr = f"{sdk_ver[0]}.{sdk_ver[1]}.{sdk_ver[2]}"
    _safe_print(f"已识别 SDK 版本: {vstr}")
    need_p1 = _sdk_ver_le(sdk_ver, (1, 2, 3))
    need_p2 = module == WifiModule.AIC8800 and _sdk_ver_le(sdk_ver, (1, 3, 0))
    if need_p1:
        _safe_print("")
        _safe_print(
            "1）1.2.3之前(含)版本SDK在/kernel/rt-thread/components/drivers/sdio/mmcsd_core.c，"
            "mmcsd_detect while(1)下面增加一个500mS的延时，具体咨询fae"
        )
    if need_p2:
        _safe_print("")
        _safe_print(
            "2）1.3.0之前(含)版本SDK，如果选用的是AIC8800的wifi增加一个补丁可以减少wifi模块对内存占用。"
        )
    if not need_p1 and not need_p2:
        _safe_print("")
        _safe_print("按当前识别版本与模组选择，上述补丁通常无需处理。")


def _prompt_choice(
    title: str,
    options: List[Any],
    label_fn: Callable[[Any], str],
    allow_quit: bool = True,
) -> Optional[Any]:
    _safe_print(title)
    for i, opt in enumerate(options, 1):
        _safe_print(f"  [{i}] {label_fn(opt)}")
    if allow_quit:
        _safe_print("  [q] 退出")
    _safe_print("请输入序号: ", end="")
    try:
        raw = input().strip()
    except EOFError:
        return None
    except Exception:
        return None
    if allow_quit and raw.lower() in ("q", "quit", "exit"):
        return None
    if not raw.isdigit():
        _safe_print("无效输入，已取消。")
        return None
    idx = int(raw)
    if idx < 1 or idx > len(options):
        _safe_print("序号超出范围，已取消。")
        return None
    return options[idx - 1]


def _merge_all(
    module: WifiModule,
    sdmc_index: int,
    power_gpio: str,
    enable_bt: bool,
    aic8800_dbg: bool,
    realtek_dbg: bool,
    realtek_full: bool,
    apply_kernel: bool,
    apply_netutils: bool,
    *,
    aic8800_variant: Optional[Aic8800ChipVariant] = None,
    aic8800_power_converter: Optional[Aic8800PowerConverter] = None,
) -> Dict[str, object]:
    profile, notes = build_wifi_profile(
        module,
        power_gpio=power_gpio,
        aic8800_variant=aic8800_variant,
        aic8800_power_converter=aic8800_power_converter,
        enable_bt_aic8800=enable_bt,
        aic8800_debug_log=aic8800_dbg,
        realtek_debug=realtek_dbg,
        realtek_full_function=realtek_full,
    )
    merged: Dict[str, object] = dict(profile)
    merged.update(sdmc_sdio_for_wifi(sdmc_index))

    if apply_kernel:
        if module == WifiModule.AIC8800:
            merged.update(kernel_lwip_bundle_aic8800())
        elif module in (WifiModule.RTL8189, WifiModule.RTL8733):
            merged.update(kernel_lwip_bundle_realtek())
        elif module == WifiModule.TXW901:
            merged.update(kernel_lwip_bundle_txw901())

    if apply_netutils:
        merged.update(netutils_iperf_hint())

    return {"merged": merged, "notes": notes}


def _select_chip_board_defconfig(
    aic_root: str,
    all_defs: List[DefconfigInfo],
) -> Optional[DefconfigInfo]:
    chips = list_chips_from_defconfigs(all_defs)
    if not chips:
        _safe_print("target/configs 中没有任何可用的 defconfig（已排除 bootloader）。")
        return None
    chip = _prompt_choice("请选择芯片:", chips, lambda c: c)
    if chip is None:
        return None
    boards = list_boards_from_defconfigs(all_defs, chip)
    if not boards:
        _safe_print(f"芯片 {chip} 在 target/configs 中没有可用的 defconfig（内部数据不一致）。")
        return None
    board = _prompt_choice("请选择板型:", boards, lambda b: b)
    if board is None:
        return None
    filtered = [d for d in all_defs if d.chip == chip and d.board == board]
    if not filtered:
        _safe_print("没有与该芯片/板型匹配的 defconfig 文件（内部数据不一致）。请向本工具维护者反馈。")
        return None
    if len(filtered) == 1:
        d = filtered[0]
        _safe_print(f"已匹配 defconfig: {d.filename}")
        return d
    return _prompt_choice(
        "请选择 defconfig（同一版型可能存在多个内核/应用组合）:",
        filtered,
        lambda d: d.filename,
    )


def _run_sdmc_pinmux_interactive(aic_root: str, dinfo: DefconfigInfo, sdmc: int) -> bool:
    """
    SDMC 与 pinmux：列出当前表项、缺省模板、与原理图是否一致、可选逐项修改并写回 pinmux.c。
    返回 True 表示用户输入 q 退出（调用方应结束向导）。
    """
    chip = dinfo.chip
    tag = f"AIC_USING_SDMC{sdmc}"
    _safe_print("")
    _safe_print("【SDMC 与 pinmux】")
    _safe_print(
        f"  引脚复用说明请查阅文档中心：{pinmux_doc_path_hint(chip)} "
        "（亦可在 https://aicdoc.artinchip.com 搜索芯片用户手册）。"
    )
    _safe_print(
        "  SDIO WiFi 的 CLK/CMD/DAT 以 pinmux.c 为准；defconfig 由本向导合并。"
        "若在下列逐项核对中修改了表项，可将修改写回当前板型的 pinmux.c。"
    )
    pm = find_pinmux_path(aic_root, chip, dinfo.board)
    if not pm:
        _safe_print("  未找到板级 pinmux.c，无法列出 SDMC 表项。")
        _safe_print("")
        return False
    ptxt = read_text_safe(pm)
    if not ptxt:
        _safe_print("  无法读取 pinmux.c。")
        _safe_print("")
        return False
    try:
        rel = os.path.relpath(pm, aic_root)
    except ValueError:
        rel = pm
    tbl = extract_sdmc_pinmux_table_lines(ptxt, sdmc)
    slots = extract_sdmc_pinmux_line_slots(ptxt, sdmc)
    has_ifdef = f"#ifdef {tag}" in ptxt or f"#if defined({tag})" in ptxt

    _safe_print(f"  文件 `{rel}`  ·  宏 `{tag}`（SDMC{sdmc}）")
    _safe_print("")
    _safe_print(f"  --- 当前 pinmux 中 `{tag}` 段内表项 ---")
    if tbl:
        for ln in tbl:
            _safe_print(f"      {ln}")
    elif has_ifdef:
        _safe_print(
            f"      （已有 `#ifdef {tag}`，但未解析到标准 `{{…}}` 表项行，请对照手册编辑。）"
        )
    else:
        _safe_print(f"      （未找到 `{tag}` 条件块或表项。）")
        _safe_print("")
        _safe_print("  【缺省模板】请在 `aic_pinmux_config[]` 中增补（引脚按手册与硬件填写）：")
        for s in default_sdmc_pinmux_example_lines(sdmc):
            _safe_print(f"      {s}")
        _safe_print("")
        _safe_print("  编辑完成后请保存并重新运行本工具核对。按回车继续向导（q 退出）: ", end="")
        w = _prompt_line(allow_quit=True)
        if w is None:
            _safe_print("已退出。")
            return True
        _safe_print("")
        return False

    if not tbl or not slots:
        _safe_print("")
        _safe_print("  按回车继续（q 退出）: ", end="")
        w = _prompt_line(allow_quit=True)
        if w is None:
            _safe_print("已退出。")
            return True
        _safe_print("")
        return False

    _safe_print("")
    _safe_print(
        "  上述表项是否与原理图、模组 SDIO 接线一致并继续后续向导？"
        "（默认 Y 一致，直接回车；输入 n 则逐项核对并可将修改写入 pinmux.c）: ",
        end="",
    )
    c = _prompt_line(allow_quit=True)
    if c is None:
        _safe_print("已退出。")
        return True
    if c.strip().lower() in ("n", "no"):
        _safe_print("")
        _safe_print("  逐项说明：回车=保持本行；输入新 GPIO（如 PF.3）将替换引脚名；")
        _safe_print(
            f"  或以 `{{` 开头粘贴整行新表项（保留行首缩进写回；行尾漏写 `,` 时会自动补全）。"
            f"详见 {pinmux_doc_path_hint(chip)}。"
        )
        line_updates: List[Tuple[int, str]] = []
        for i, (line_no, full_line) in enumerate(slots, 1):
            ln = full_line.strip()
            _safe_print("")
            _safe_print(f"  --- 表项 {i}/{len(slots)} ---")
            _safe_print(f"      {ln}")
            parsed = parse_sdmc_pinmux_row_fields(ln)
            if parsed:
                mode, pull, drv, gpio = parsed
                _safe_print(
                    f"      解析：复用模式={mode}，{pull}，驱动强度={drv}，GPIO=\"{gpio}\""
                )
            _safe_print("  修改（回车跳过）: ", end="")
            edit = _prompt_line(allow_quit=True)
            if edit is None:
                _safe_print("已退出。")
                return True
            es = edit.strip()
            if not es:
                continue
            try:
                if es.startswith("{"):
                    new_full = apply_full_pinmux_row_edit_to_file_line(full_line, es)
                else:
                    new_full = apply_gpio_edit_to_pinmux_file_line(full_line, es)
            except Exception:
                _safe_print("      无法解析，本行未记录修改。")
                continue
            line_updates.append((line_no, new_full))
            _safe_print(f"      待写回：{new_full.strip()}")

        pinmux_wrote_ok = False
        if line_updates:
            _safe_print("")
            _safe_print(
                f"  是否将上述 {len(line_updates)} 处修改写入 `{rel}`？（默认 Y；n 放弃写盘）: ",
                end="",
            )
            w2 = _prompt_line(allow_quit=True)
            if w2 is None:
                _safe_print("已退出。")
                return True
            if w2.strip().lower() not in ("n", "no"):
                if write_pinmux_line_replacements(pm, ptxt, line_updates):
                    pinmux_wrote_ok = True
                    _safe_print(f"  已经写入 `{rel}` 文件，按回车继续向导: ", end="")
                    _prompt_line()
                else:
                    _safe_print("  写入 pinmux.c 失败，请检查路径与写权限。")
            else:
                _safe_print("  已放弃写入；可手动编辑 pinmux.c 或重新运行本向导。")

        if not pinmux_wrote_ok:
            _safe_print("")
            _safe_print("  按回车继续向导: ", end="")
            _prompt_line()
    _safe_print("")
    return False


def run_interactive(aic_root: str) -> int:
    _safe_print("")
    _safe_print("=== Luban-Lite WiFi 配置助手（单文件版）===")
    _safe_print(f"脚本版本: {__script_version__}")
    _safe_print(f"适配固件版本:{__firmware_version__}")
    _safe_print("作者: fangjie.wang")
    _safe_print("文档: https://aicdoc.artinchip.com/topics/sdk/peripheral/wifi-lite.html")
    _safe_print("SDK 根目录: " + os.path.abspath(aic_root))
    _safe_print("")
    _safe_print(SCRIPT_V11_CHANGELOG.rstrip("\n"))
    _safe_print("")
    sdk_ver_cache = read_luban_sdk_version_tuple(aic_root)
    if sdk_ver_cache:
        _safe_print(
            f"已读取 SDK 版本号: {sdk_ver_cache[0]}.{sdk_ver_cache[1]}.{sdk_ver_cache[2]}"
            "（来自 LUBAN_LITE_SDK_VERSION 环境变量或 luban_lite_sdk_version.txt / VERSION）"
        )
        _safe_print("")

    all_defs = list_defconfigs(aic_root, include_bootloader=False)
    if not all_defs:
        _safe_print("target/configs 下没有可用的 defconfig，已退出。")
        return 1

    prj = read_prj_from_dot_config(aic_root)
    current = resolve_defconfig_info(all_defs, prj) if prj else None

    dinfo: Optional[DefconfigInfo] = None

    if current:
        _safe_print("--- 当前工程版型（来自根目录 .config）---")
        _safe_print(f"  芯片: {current.chip}")
        _safe_print(f"  板型: {current.board}")
        _safe_print(f"  内核: {current.kernel}")
        _safe_print(f"  应用: {current.app}")
        _safe_print(f"  defconfig: {current.filename}")
        _safe_print("")
        _safe_print(
            "是否切换为其它版型配置？（默认: N 不切换；直接回车。输入 y 切换。y/N，q 退出）: ",
            end="",
        )
        sw = _prompt_line()
        if sw is None:
            _safe_print("已退出。")
            return 0
        if sw.lower() in ("y", "yes", "1"):
            dinfo = _select_chip_board_defconfig(aic_root, all_defs)
        else:
            dinfo = current
    else:
        if prj:
            _safe_print("注意: .config 中的工程变量无法在 target/configs 中匹配到 defconfig（可能已改名或未同步）。")
            if prj.get("defconfig_filename"):
                _safe_print(f"  .config 中声明: {prj['defconfig_filename']}")
            _safe_print("")
            _safe_print("请重新选择芯片、板型及 defconfig。")
        else:
            _safe_print("--- 未识别当前版型 ---")
            _safe_print(
                "根目录无 .config，或缺少 CONFIG_PRJ_*，无法判断当前编译目标。"
                "请依次选择芯片、板型及 defconfig。"
            )
        _safe_print("")
        dinfo = _select_chip_board_defconfig(aic_root, all_defs)

    if dinfo is None:
        _safe_print("已退出或未选择 defconfig。")
        return 0

    defconfig_path = os.path.join(aic_root, "target", "configs", dinfo.filename)
    initial_text = read_text_safe(defconfig_path)
    if initial_text is None:
        _safe_print("无法读取 defconfig 文件，已退出。")
        return 1
    working_lines = initial_text.splitlines()
    wifi_kv = parse_config_kv_lines(initial_text)
    did_wifi_reset = False

    if is_wifi_likely_enabled(wifi_kv):
        _safe_print("")
        _safe_print("--- 检测到当前 defconfig 中已启用 WiFi（Wireless LAN 或 WLAN 驱动）---")
        _safe_print("  [1] 仅检查：列出相关 CONFIG（不写文件）")
        _safe_print(
            "  [2] 更换 WiFi：先恢复本工具识别的 WiFi 项为关闭/默认，再进入配置向导（默认）"
        )
        _safe_print("  [3] 不清理：直接按向导在当前配置基础上合并（可能与旧项叠加，高级）")
        _safe_print("  [q] 退出")
        _safe_print("请选择（默认: 2；直接回车）: ", end="")
        wch = _prompt_line()
        if wch is None:
            _safe_print("已退出。")
            return 0
        wch_stripped = wch.strip()
        if wch_stripped == "":
            wch_stripped = "2"
        if wch_stripped.lower() in ("q", "quit", "exit"):
            _safe_print("已退出。")
            return 0
        if wch_stripped not in ("1", "2", "3"):
            _safe_print("无效输入，已退出。")
            return 0
        if wch_stripped == "1":
            for line in format_wifi_inspection_report(initial_text):
                _safe_print(line)
            _safe_print("")
            _safe_print("是否进入后续 WiFi 配置向导？（默认: N；y 进入且不执行清理）: ", end="")
            cont = _prompt_line()
            if cont is None:
                _safe_print("已退出。")
                return 0
            if cont.lower() not in ("y", "yes", "1"):
                _safe_print("检查结束，未修改任何文件。")
                return 0
            working_lines = initial_text.splitlines()
        elif wch_stripped == "2":
            working_lines = merge_defconfig(working_lines, wifi_reset_merge_dict())  # type: ignore[arg-type]
            did_wifi_reset = True
            _safe_print("")
            _safe_print("已在内存中将 WiFi 相关项恢复为关闭/默认，随后按向导写入新方案。")
        else:
            working_lines = initial_text.splitlines()
    else:
        _safe_print("")
        _safe_print("--- 当前 defconfig 未检测到已启用的 WiFi ---")
        _safe_print("（将直接进入 WiFi 配置向导；若符号与版本不一致，请以 menuconfig 为准。）")

    mod = _prompt_choice(
        "请选择 WiFi 模组（参考默认: [1] aic8800；须输入序号）:",
        [
            WifiModule.AIC8800,
            WifiModule.RTL8189,
            WifiModule.RTL8733,
            WifiModule.TXW901,
        ],
        lambda m: m.value,
    )
    if mod is None:
        _safe_print("已退出。")
        return 0

    aic8800_variant: Optional[Aic8800ChipVariant] = None
    aic8800_power_converter: Optional[Aic8800PowerConverter] = None
    if mod == WifiModule.AIC8800:
        _safe_print("")
        _safe_print("--- AIC8800 子型号（menuconfig: Select AIC8800 MODEL）---")
        _safe_print(
            "  AIC8800DL 与 AIC8800DW 为同一套 Kconfig 选项，请选择「AIC8800DW」对应项。"
        )
        aic8800_variant = _prompt_choice(
            "请选择芯片型号（须输入序号）:",
            [Aic8800ChipVariant.D40L, Aic8800ChipVariant.DW_OR_DL],
            lambda v: (
                "AIC8800D40L（Kconfig 选 AIC8800D40L）"
                if v == Aic8800ChipVariant.D40L
                else "AIC8800DL / AIC8800DW（Kconfig 选 AIC8800DW；二者配置相同）"
            ),
        )
        if aic8800_variant is None:
            _safe_print("已退出。")
            return 0
        default_conv = (
            Aic8800PowerConverter.DCDC
            if aic8800_variant == Aic8800ChipVariant.D40L
            else Aic8800PowerConverter.LDO
        )
        other_conv = (
            Aic8800PowerConverter.LDO
            if default_conv == Aic8800PowerConverter.DCDC
            else Aic8800PowerConverter.DCDC
        )
        _safe_print("")
        _safe_print("--- Select AIC8800 power converter ---")
        _safe_print(
            "  备注: DCDC / LDO 与模组厂的射频电源硬件设计直接相关，请模组厂确认，或者进行尝试选择（可能会影响信号强度等硬件指标）。"
        )
        short_default_mode = (
            "DCDC mode"
            if default_conv == Aic8800PowerConverter.DCDC
            else "LDO mode"
        )

        def _mode_choice_label(conv: Aic8800PowerConverter) -> str:
            if conv == Aic8800PowerConverter.DCDC:
                return "DCDC mode（CONFIG_CONFIG_AIC8800_VRF_DCDC_MODE）"
            return "LDO mode （CONFIG_CONFIG_AIC8800_VRF_LDO_MODE）"

        def_conv_label = _mode_choice_label(default_conv)
        other_conv_label = _mode_choice_label(other_conv)
        aic8800_power_converter = _prompt_choice(
            f"默认电源方案（可直接选 [1]）: {short_default_mode}；",
            [default_conv, other_conv],
            lambda c: def_conv_label if c == default_conv else other_conv_label,
        )
        if aic8800_power_converter is None:
            _safe_print("已退出。")
            return 0

    sdmc_options, sdmc_kcfg_note = discover_sdmc_indices_from_kconfig_board(
        aic_root, dinfo.chip
    )
    if sdmc_kcfg_note:
        _safe_print("")
        _safe_print("【SDMC 与 Kconfig】")
        _safe_print(f"  {sdmc_kcfg_note}")
        _safe_print("")
    else:
        _safe_print("")
        _safe_print("【SDMC 与 Kconfig】")
        _safe_print(
            f"  当前芯片 `{dinfo.chip}` 在 target/{dinfo.chip}/common/Kconfig.board 中声明了: "
            + ", ".join(f"SDMC{i}" for i in sdmc_options)
            + "；下列仅列出以上项（与 menuconfig 中可选控制器一致）。"
        )
        _safe_print("")

    sdmc = _prompt_choice(
        "WiFi 接在哪个 SDMC？（文档示例默认: 最小序号为 SDMC0；须与硬件及下列可选一致）",
        sdmc_options,
        lambda i: f"SDMC{i}",
    )
    if sdmc is None:
        _safe_print("已退出。")
        return 0

    if _run_sdmc_pinmux_interactive(aic_root, dinfo, sdmc):
        return 0

    default_gpio = "PD.17" if mod == WifiModule.TXW901 else "PD.7"
    _safe_print(
        f'请输入 WiFi 电源控制 GPIO 字符串（默认: "{default_gpio}"；直接回车。q 退出）: ',
        end="",
    )
    gpio_line = _prompt_line(allow_quit=True)
    if gpio_line is None:
        _safe_print("已退出。")
        return 0
    power_gpio = gpio_line if gpio_line else default_gpio

    enable_bt = False
    aic8800_dbg = False
    if mod == WifiModule.AIC8800:
        _safe_print("是否启用 AIC8800 蓝牙支持？（默认: N；直接回车。y/N，q 退出）: ", end="")
        bt = _prompt_line()
        if bt is None:
            _safe_print("已退出。")
            return 0
        enable_bt = bt.lower() in ("y", "yes", "1")
        _safe_print("是否打开 AIC8800 驱动调试日志？（默认: N；直接回车。y/N，q 退出）: ", end="")
        ad = _prompt_line()
        if ad is None:
            _safe_print("已退出。")
            return 0
        aic8800_dbg = ad.lower() in ("y", "yes", "1")

    realtek_dbg = True
    realtek_full = False
    if mod in (WifiModule.RTL8189, WifiModule.RTL8733):
        _safe_print("是否打开 Realtek 调试信息？（默认: Y；直接回车。Y/n，q 退出）: ", end="")
        rd = _prompt_line()
        if rd is None:
            _safe_print("已退出。")
            return 0
        realtek_dbg = rd.lower() not in ("n", "no", "0")
        _safe_print(
            "是否打开 Realtek full function（P2P/蓝牙等）？（默认: N；直接回车。y/N，q 退出）: ",
            end="",
        )
        rf = _prompt_line()
        if rf is None:
            _safe_print("已退出。")
            return 0
        realtek_full = rf.lower() in ("y", "yes", "1")

    _safe_print(
        "是否合并文档中的「内核与 lwIP」建议项？（默认: Y；直接回车。Y/n，q 退出）: ",
        end="",
    )
    ak = _prompt_line()
    if ak is None:
        _safe_print("已退出。")
        return 0
    apply_kernel = ak.lower() not in ("n", "no", "0")

    _safe_print(
        "是否启用 netutils + iperf（文档调试工具）？（默认: N；直接回车。y/N，q 退出）: ",
        end="",
    )
    an = _prompt_line()
    if an is None:
        _safe_print("已退出。")
        return 0
    apply_netutils = an.lower() in ("y", "yes", "1")

    result = _merge_all(
        mod,
        sdmc,
        power_gpio,
        enable_bt,
        aic8800_dbg,
        realtek_dbg,
        realtek_full,
        apply_kernel,
        apply_netutils,
        aic8800_variant=aic8800_variant,
        aic8800_power_converter=aic8800_power_converter,
    )
    merged = result["merged"]
    notes: List[str] = result["notes"]  # type: ignore

    pinmux_path = find_pinmux_path(aic_root, dinfo.chip, dinfo.board)
    final_lines = merge_defconfig(working_lines, merged)  # type: ignore[arg-type]
    merged_config_kv = parse_config_kv_lines("\n".join(final_lines))

    _safe_print("")
    if did_wifi_reset:
        _safe_print("（本轮已在合并前执行 WiFi 项恢复，下方为恢复后叠加新向导的结果预览。）")
    _safe_print("--- 变更摘要（本次向导将写入的 CONFIG 项）---")
    for k in sorted(merged.keys(), key=str):
        _safe_print(f"  {k} = {merged[k]!r}")

    _safe_print("")
    _safe_print("--- 说明与 Pinmux 检查 ---")
    if notes:
        _safe_print("【模组与选项】")
        for n in notes:
            _safe_print(f"  · {n}")
        _safe_print("")
    pm_rel = ""
    if pinmux_path:
        try:
            pm_rel = os.path.relpath(pinmux_path, aic_root)
        except ValueError:
            pm_rel = pinmux_path
    _safe_print(f"【Pinmux】{pm_rel or '（未找到 pinmux.c）'}")
    _pinmux_verbose = os.environ.get("WIFI_AUTOCFG_PINMUX_VERBOSE", "").lower() in (
        "1",
        "true",
        "yes",
        "y",
    )
    for sec_title, bullets in analyze_pinmux_sections(
        pinmux_path or "",
        expected_pwr_gpio=power_gpio,
        sdmc_index=sdmc,
        verbose_duplicate_scan=_pinmux_verbose,
        config_kv=merged_config_kv,
        chip=dinfo.chip,
    ):
        _safe_print(f"  [{sec_title}]")
        for b in bullets:
            _safe_print(f"    · {b}")
        _safe_print("")

    _safe_print("")
    # 写入模式：首次可选 [1]/[2]/[q]；选 1 或 2 后不再重复该菜单，仅进入「仅 q」暂停，便于核对输出
    while True:
        _safe_print("--- 写入模式 ---")
        _safe_print("  [1] 仅预览（本次配置不生效）")
        _safe_print("  [2] 写入 defconfig（本次配置生效）")
        _safe_print("  [q] 退出（结束脚本）")
        _safe_print("请选择（默认 2；直接回车为写入 defconfig）: ", end="")
        mode = _prompt_line(allow_quit=True)
        if mode is None:
            _safe_print("已退出。")
            return 0
        ms = mode.strip()
        if ms == "":
            ms = "2"
        if ms not in ("1", "2"):
            _safe_print("请输入 1、2，或 q 退出。")
            _safe_print("")
            continue
        if ms == "2":
            new_text = "\n".join(final_lines) + "\n"
            if write_text_safe(defconfig_path, new_text):
                _safe_print("已写入: " + defconfig_path)
                _safe_print("请之后在工程根目录执行官方流程加载该 defconfig（例如 scons 相关命令）并重新编译。")
            else:
                _safe_print("写入失败，请检查路径与权限。")
                return 1
        else:
            _safe_print("已选择仅预览：未写入 defconfig，本次配置不生效。")
            _safe_print("")
            _print_legacy_sdk_patch_hints(sdk_ver_cache, mod)
            _safe_print("")
            _safe_print("开机后请打开调试串口（串口日志/控制台），按下列 `wifi` 测试命令验证：")
            _safe_print("参考文档: https://aicdoc.artinchip.com/topics/sdk/peripheral/wifi-lite.html")
            _safe_print("")
            _safe_print(test_commands_text())
            _pause_until_quit_only("")
            return 0
        # 写入模式：已写入 defconfig 后，立即给出后续“编译/烧录/串口验证”的指令，让用户无需再等 q 才看到 wifi 测试信息。
        _safe_print("")
        _safe_print("写入模式完成：请重新编译并将固件烧录到板子上。")
        _print_legacy_sdk_patch_hints(sdk_ver_cache, mod)
        _safe_print("")
        _safe_print("开机后请打开调试串口（串口日志/控制台），按下列 `wifi` 测试命令验证：")
        _safe_print("参考文档: https://aicdoc.artinchip.com/topics/sdk/peripheral/wifi-lite.html")
        _safe_print("")
        _safe_print(test_commands_text())
        _safe_print("")
        _pause_until_quit_only("")
        return 0


def main(argv: Optional[List[str]] = None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    aic_root = os.getcwd()
    try:
        if "-h" in argv or "--help" in argv:
            _safe_print("用法: python wifi_autoconfig.py")
            _safe_print("  在 Luban-Lite SDK 根目录运行；合并 WiFi 相关 CONFIG 到 target/configs/*.defconfig")
            _safe_print("  --pinmux-verbose  列出 pinmux 中 GPIO 多处分支的明细（信息量大）")
            _safe_print("  或: set WIFI_AUTOCFG_PINMUX_VERBOSE=1")
            _safe_print("  --version         打印脚本版本与 V11 更新说明")
            _safe_print("  异常时堆栈会追加写入 SDK 根目录 wifi_autoconfig_error.log")
            return 0
        if "--version" in argv:
            _safe_print(f"wifi_autoconfig 脚本版本: {__script_version__}")
            _safe_print(f"文档适配基线（固件/SDK 说明）: {__firmware_version__}")
            _safe_print("")
            _safe_print(SCRIPT_V11_CHANGELOG.rstrip("\n"))
            return 0
        if not os.path.isdir(os.path.join(aic_root, "target", "configs")):
            _safe_print("错误: 当前目录下未找到 target/configs，请到 Luban-Lite 根目录再运行。")
            return 1
        if "--pinmux-verbose" in argv:
            os.environ["WIFI_AUTOCFG_PINMUX_VERBOSE"] = "1"
        return run_interactive(aic_root)
    except Exception as exc:
        log_path = _log_wifi_autoconfig_error(aic_root, exc)
        _safe_print("内部错误（已捕获，不向上抛出）:")
        traceback.print_exc()
        if log_path:
            _safe_print(f"详情已追加写入: {log_path}")
        else:
            _safe_print("（未能写入 wifi_autoconfig_error.log，请检查当前目录写权限。）")
        try:
            sys.stdout.flush()
            sys.stderr.flush()
        except Exception:
            pass
        return 2


# =============================================================================
# §8 程序入口
# =============================================================================

if __name__ == "__main__":
    sys.exit(main())
