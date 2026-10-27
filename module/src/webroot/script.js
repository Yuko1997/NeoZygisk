const PROP_PATH = "/data/adb/neozygisk/module.prop";
const STATUS_PATH = "/data/adb/neozygisk/status";
const MODULE_MARKER = "@@NZMOD@@ ";
const LANG_STORAGE_KEY = "neozygisk.lang";

const GOOD_STATUSES = new Set(["tracing", "injected", "running", "clean"]);
const BAD_STATUSES = new Set(["stopped", "exited", "crashed", "not_injected"]);
const STATUS_KEYS = new Set([
  "monitor",
  "zygote",
  "zygote32",
  "zygote64",
  "daemon",
  "daemon32",
  "daemon64",
  "zygote_next",
]);

const I18N = {
  en: {
    language: "Language",
    reload: "Reload",
    info: "Device & environment",
    modules: "Active modules",
    settings: "Settings",
    monitor: "Zygote monitor",
    zygote: "Zygote",
    daemon: "Daemon",
    zygote_next: "Zygote next",
    root: "Root",
    rom: "System",
    kernel: "Kernel",
    sdk: "Android SDK",
    abi: "ABI",
    running: "Running",
    tracing: "Tracing",
    injected: "Injected",
    not_injected: "Not injected",
    stopped: "Stopped",
    exited: "Exited",
    crashed: "Crashed",
    clean: "Clean",
    unclean: "Needs attention",
    unknown: "Unknown",
    status_ok: "All systems working",
    status_ok_hint: "Zygote injection and module loading are active",
    status_warn: "Partially working",
    status_warn_hint: "One component needs attention",
    status_down: "Not working",
    status_down_hint: "Zygisk is not active. Try rebooting the device",
    status_unknown: "Status unknown",
    status_unknown_hint: "The current status could not be read",
    modules_none: "No active Zygisk modules",
    by_author: "by",
    reloaded: "Reloaded",
    load_failed: "Failed to read module status",
    bridge_missing: "No supported WebUI bridge detected",
    denylist: "DenyList / App Profile",
    denylist_hint: "NeoZygisk reads the root manager's per-app unmount policy. Set App Profile or Configure DenyList on the apps that must not see root or module mounts.",
  },
  "zh-Hant": {
    language: "語言",
    reload: "重新整理",
    info: "裝置與環境",
    modules: "已啟用模組",
    settings: "設定",
    monitor: "Zygote 監控",
    zygote: "Zygote",
    daemon: "守護程序",
    zygote_next: "Zygote next",
    root: "Root 方案",
    rom: "系統",
    kernel: "核心",
    sdk: "Android SDK",
    abi: "ABI",
    running: "執行中",
    tracing: "追蹤中",
    injected: "已注入",
    not_injected: "未注入",
    stopped: "已停止",
    exited: "已退出",
    crashed: "已崩潰",
    clean: "已清理",
    unclean: "需要檢查",
    unknown: "未知",
    status_ok: "運作正常",
    status_ok_hint: "Zygote 注入與模組載入均在執行",
    status_warn: "部分異常",
    status_warn_hint: "有元件需要檢查",
    status_down: "未運作",
    status_down_hint: "Zygisk 未啟用，請嘗試重新開機",
    status_unknown: "狀態未知",
    status_unknown_hint: "無法讀取目前狀態",
    modules_none: "沒有已啟用的 Zygisk 模組",
    by_author: "作者",
    reloaded: "已重新整理",
    load_failed: "讀取模組狀態失敗",
    bridge_missing: "找不到支援的 WebUI bridge",
    denylist: "DenyList / App Profile",
    denylist_hint: "NeoZygisk 讀取 Root 管理器的按應用卸載策略。請在 App Profile 或 DenyList 設定不應看到 Root/模組掛載的應用。",
  },
  "zh-Hans": {
    language: "语言",
    reload: "刷新",
    info: "设备与环境",
    modules: "已启用模块",
    settings: "设置",
    monitor: "Zygote 监控",
    zygote: "Zygote",
    daemon: "守护进程",
    zygote_next: "Zygote next",
    root: "Root 方案",
    rom: "系统",
    kernel: "内核",
    sdk: "Android SDK",
    abi: "ABI",
    running: "运行中",
    tracing: "追踪中",
    injected: "已注入",
    not_injected: "未注入",
    stopped: "已停止",
    exited: "已退出",
    crashed: "已崩溃",
    clean: "已清理",
    unclean: "需要检查",
    unknown: "未知",
    status_ok: "运行正常",
    status_ok_hint: "Zygote 注入与模块加载均在运行",
    status_warn: "部分异常",
    status_warn_hint: "有组件需要检查",
    status_down: "未运行",
    status_down_hint: "Zygisk 未启用，请尝试重启设备",
    status_unknown: "状态未知",
    status_unknown_hint: "无法读取当前状态",
    modules_none: "没有已启用的 Zygisk 模块",
    by_author: "作者",
    reloaded: "已刷新",
    load_failed: "读取模块状态失败",
    bridge_missing: "找不到支持的 WebUI bridge",
    denylist: "DenyList / App Profile",
    denylist_hint: "NeoZygisk 读取 Root 管理器的按应用卸载策略。请在 App Profile 或 DenyList 中配置不应看到 Root/模块挂载的应用。",
  },
};

let currentLang = pickInitialLanguage();
let toastTimer = null;
let refreshing = false;
let snapshot = null;

const $ = (id) => document.getElementById(id);

document.addEventListener("DOMContentLoaded", () => {
  $("lang-select").addEventListener("click", (event) => {
    const button = event.target.closest("button[data-lang]");
    if (button && button.dataset.lang !== currentLang) {
      currentLang = button.dataset.lang;
      try {
        localStorage.setItem(LANG_STORAGE_KEY, currentLang);
      } catch (_) {
        // The choice still lasts for this WebView session.
      }
      applyTranslations();
    }
  });

  $("btn-reload").addEventListener("click", () => refresh(true));

  applyTranslations();
  refresh(false);
});

function pickInitialLanguage() {
  try {
    const stored = localStorage.getItem(LANG_STORAGE_KEY);
    if (stored && I18N[stored]) return stored;
  } catch (_) {
    // Fall through to the browser language.
  }
  const language = navigator.language || "en";
  if (language.startsWith("zh")) {
    return /hans|cn|sg/i.test(language) ? "zh-Hans" : "zh-Hant";
  }
  return "en";
}

function t(key, vars) {
  const local = I18N[currentLang] || I18N.en;
  let text = local[key] !== undefined ? local[key] : (I18N.en[key] !== undefined ? I18N.en[key] : key);
  if (vars) {
    Object.keys(vars).forEach((name) => {
      text = text.replace(`{${name}}`, vars[name]);
    });
  }
  return text;
}

function applyTranslations() {
  document.documentElement.lang = currentLang;
  document.querySelectorAll("[data-i18n]").forEach((element) => {
    element.textContent = t(element.getAttribute("data-i18n"));
  });
  document.querySelectorAll("[data-i18n-aria]").forEach((element) => {
    element.setAttribute("aria-label", t(element.getAttribute("data-i18n-aria")));
  });
  document.querySelectorAll("#lang-select button").forEach((button) => {
    button.setAttribute("aria-pressed", String(button.dataset.lang === currentLang));
  });
  renderOverview();
}

function parseProp(text) {
  const data = {};
  text.split(/\r?\n/).forEach((line) => {
    const match = line.trim().match(/^([^=]+)=(.*)$/);
    if (!match) return;
    const key = match[1].trim();
    if (!Object.prototype.hasOwnProperty.call(data, key)) {
      data[key] = match[2].trim();
    }
  });
  return data;
}

function parseStatusProp(text) {
  const status = {};
  text.split(/\r?\n/).forEach((line) => {
    const match = line.match(/^\s*([A-Za-z0-9_]+):\s*(.*)$/);
    if (!match) return;
    const key = match[1].toLowerCase();
    if (!STATUS_KEYS.has(key) || Object.prototype.hasOwnProperty.call(status, key)) return;
    const value = match[2].replace(/[^\x20-\x7E]/g, " ").trim().toLowerCase();
    if (value) status[key] = value;
  });
  return status;
}

function escapeHtml(text) {
  return String(text)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function getExec() {
  const exec = window.NeoZygiskWebUi && window.NeoZygiskWebUi.exec;
  return typeof exec === "function" ? exec : null;
}

function decodeBase64Utf8(value) {
  if (!value) return "";
  try {
    const binary = atob(value.replace(/\s+/g, ""));
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return new TextDecoder().decode(bytes);
  } catch (_) {
    return "";
  }
}

function parseSectionOutput(stdout) {
  const sections = {};
  let current = null;
  const buffer = [];
  for (const line of String(stdout || "").split("\n")) {
    const trimmed = line.trim();
    if (trimmed.startsWith("---") && trimmed.endsWith("---") && trimmed.length > 6) {
      if (current) sections[current] = buffer.join("");
      buffer.length = 0;
      current = trimmed.slice(3, -3);
    } else if (current && trimmed.length > 0) {
      buffer.push(trimmed);
    }
  }
  if (current) sections[current] = buffer.join("");
  return {
    prop: decodeBase64Utf8(sections.PROP || ""),
    status: decodeBase64Utf8(sections.STATUS || ""),
    root: decodeBase64Utf8(sections.ROOT || ""),
    rom: decodeBase64Utf8(sections.ROM || ""),
    device: decodeBase64Utf8(sections.DEVICE || ""),
    modules: decodeBase64Utf8(sections.MODULES || ""),
  };
}

function unifiedStatusCommand() {
  return [
    "dump_all() {",
    '  echo "---PROP---"',
    `  base64 < ${PROP_PATH} 2>/dev/null`,
    '  echo "---STATUS---"',
    `  base64 < ${STATUS_PATH} 2>/dev/null`,
    '  echo "---ROOT---"',
    "  {",
    '    r="Unknown"',
    '    pidof apd >/dev/null 2>&1 && r="APatch"',
    '    [ -d /data/adb/ksu ] && r="KernelSU"',
    '    pidof magiskd >/dev/null 2>&1 && r="Magisk"',
    '    echo "root=$r"',
    '    if [ "$r" = "APatch" ] && command -v apd >/dev/null 2>&1; then',
    '      v=$(apd -V 2>/dev/null | head -n 1); [ -n "$v" ] && echo "version_text=$v"',
    '    elif [ "$r" = "KernelSU" ] && command -v ksud >/dev/null 2>&1; then',
    '      v=$(ksud -V 2>/dev/null | head -n 1); [ -n "$v" ] && echo "version_text=$v"',
    '    elif [ "$r" = "Magisk" ] && command -v magisk >/dev/null 2>&1; then',
    '      v=$(magisk -v 2>/dev/null | head -n 1); [ -n "$v" ] && echo "version_text=$v"',
    "    fi",
    "  } | base64",
    '  echo "---ROM---"',
    "  {",
    "    for p in ro.build.version.oplusrom.display ro.build.version.oplusrom ro.mi.os.version.name ro.miui.ui.version.name ro.build.version.oneui; do",
    '      v=$(getprop "$p"); [ -n "$v" ] && echo "$p=$v"',
    "    done",
    "  } | base64",
    '  echo "---DEVICE---"',
    "  {",
    '    echo "kernel=$(uname -r)"',
    '    echo "sdk=$(getprop ro.build.version.sdk)"',
    '    echo "abi=$(getprop ro.product.cpu.abi)"',
    "  } | base64",
    '  echo "---MODULES---"',
    "  {",
    '    for d in /data/adb/modules/*; do',
    '      [ -d "$d" ] || continue',
    '      id=${d##*/}',
    '      [ "$id" = "zygisksu" ] && continue',
    '      [ -f "$d/disable" ] && continue',
    '      [ -f "$d/remove" ] && continue',
    "      has=0",
    '      for f in "$d"/zygisk/*.so; do [ -f "$f" ] && has=1 && break; done',
    '      [ "$has" -eq 1 ] || continue',
    '      echo "@@NZMOD@@ $id"',
    '      cat "$d/module.prop" 2>/dev/null',
    "      echo",
    "    done",
    "  } | base64",
    "}",
    "dump_all",
  ].join("\n");
}

function normalizeStatus(raw) {
  const value = String(raw || "").toLowerCase();
  if (!value) return "unknown";
  if (value.includes("not injected")) return "not_injected";
  if (value.includes("cleanup failed") || value.includes("fallback") || value.includes("restarting")) {
    return "unclean";
  }
  if (value === "clean" || value === "cleaned" || value.includes("clean mount view")) return "clean";
  if (value.includes("tracing")) return "tracing";
  if (value.includes("injected")) return "injected";
  if (value.includes("running")) return "running";
  if (value.includes("stopped")) return "stopped";
  if (value.includes("exited")) return "exited";
  if (value.includes("crashed")) return "crashed";
  return "unknown";
}

function renderStatus(id, status) {
  const element = $(id);
  const normalized = normalizeStatus(status);
  element.textContent = t(normalized);
  element.className = "value";
  if (GOOD_STATUSES.has(normalized)) element.classList.add("value-good");
  else if (normalized === "unclean") element.classList.add("value-warn");
  else if (BAD_STATUSES.has(normalized)) element.classList.add("value-danger");
}

function overallTone(status) {
  const monitor = normalizeStatus(status.monitor);
  const zygote = normalizeStatus(status.zygote64 || status.zygote32 || status.zygote);
  const daemon = normalizeStatus(status.daemon64 || status.daemon32 || status.daemon);
  const next = status.zygote_next ? normalizeStatus(status.zygote_next) : null;

  if (BAD_STATUSES.has(monitor) || BAD_STATUSES.has(zygote)) return "down";
  if (BAD_STATUSES.has(daemon) || next === "unclean") return "warn";
  if ([monitor, zygote, daemon].every((value) => GOOD_STATUSES.has(value)) &&
      (!next || next === "clean")) {
    return "ok";
  }
  return "unknown";
}

function renderHero(tone) {
  const classes = { ok: "tone-ok", warn: "tone-warn", down: "tone-down", unknown: "tone-neutral" };
  $("hero").className = `hero ${classes[tone]}`;
  $("hero-title").textContent = t(`status_${tone}`);
  $("hero-sub").textContent = t(`status_${tone}_hint`);
}

function normalizeRootName(name) {
  const value = String(name || "").trim();
  const names = { apatch: "APatch", kernelsu: "KernelSU", ksu: "KernelSU", magisk: "Magisk" };
  return names[value.toLowerCase()] || value;
}

function formatRootInfo(root) {
  const name = normalizeRootName(root.root);
  if (!name || name.toLowerCase() === "unknown") return "-";
  return root.version_text ? `${name} ${root.version_text}` : name;
}

function formatRomInfo(rom) {
  const oplus = rom["ro.build.version.oplusrom.display"] || rom["ro.build.version.oplusrom"];
  if (oplus) return /[a-z]{2,}/i.test(oplus) ? oplus : `ColorOS / OxygenOS ${oplus.replace(/^V/i, "")}`;
  const hyperos = rom["ro.mi.os.version.name"];
  if (hyperos) return `HyperOS ${hyperos.replace(/^OS/i, "")}`;
  const miui = rom["ro.miui.ui.version.name"];
  if (miui) return `MIUI ${miui.replace(/^V/i, "")}`;
  const oneui = Number(rom["ro.build.version.oneui"]);
  if (oneui > 0) return `One UI ${Math.floor(oneui / 10000)}.${Math.floor(oneui / 100) % 100}`;
  return "";
}

function parseModuleScan(rawText) {
  const modules = [];
  let current = null;
  rawText.split(/\r?\n/).forEach((line) => {
    if (line.startsWith(MODULE_MARKER)) {
      current = { id: line.slice(MODULE_MARKER.length).trim(), lines: [] };
      modules.push(current);
    } else if (current) {
      current.lines.push(line);
    }
  });
  return modules.filter((module) => module.id).map((module) => {
    const props = parseProp(module.lines.join("\n"));
    return {
      id: module.id,
      name: props.name || module.id,
      version: props.version || "",
      author: props.author || "",
      description: props.description || "",
    };
  });
}

function buildSnapshot(sections) {
  return {
    prop: parseProp(sections.prop),
    status: parseStatusProp(sections.status),
    root: parseProp(sections.root),
    rom: parseProp(sections.rom),
    device: parseProp(sections.device),
    modules: parseModuleScan(sections.modules),
  };
}

function renderModules(modules) {
  const list = $("module-list");
  $("v-modules-count").textContent = String(modules.length);
  if (!modules.length) {
    list.innerHTML = `<p class="empty">${escapeHtml(t("modules_none"))}</p>`;
    return;
  }
  list.innerHTML = modules.map((module) => `
    <article class="module">
      <div class="module-head">
        <h3 class="module-name">${escapeHtml(module.name)}</h3>
        ${module.version ? `<span class="module-version">${escapeHtml(module.version)}</span>` : ""}
      </div>
      ${module.description ? `<p class="module-desc">${escapeHtml(module.description)}</p>` : ""}
      <div class="module-meta">
        <span class="badge badge-zygisk">Zygisk</span>
        ${module.author ? `<span>${escapeHtml(t("by_author"))} ${escapeHtml(module.author)}</span>` : ""}
      </div>
    </article>
  `).join("");
}

function renderOverview() {
  if (!snapshot) {
    renderFailure();
    return;
  }

  const { prop, status, root, rom, device, modules } = snapshot;
  const rootText = formatRootInfo(root);
  const romText = formatRomInfo(rom);

  $("v-root").textContent = rootText;
  $("v-rom").textContent = romText || "-";
  $("t-rom").hidden = !romText;
  $("v-kernel").textContent = device.kernel || "-";
  $("v-sdk").textContent = device.sdk || "-";
  $("v-abi").textContent = device.abi || "-";
  $("v-subtitle").textContent = [prop.version, rootText !== "-" ? rootText : ""]
    .filter(Boolean).join(" · ") || "-";

  renderStatus("v-monitor", status.monitor);
  renderStatus("v-zygote", status.zygote64 || status.zygote32 || status.zygote);
  renderStatus("v-daemon", status.daemon64 || status.daemon32 || status.daemon);
  const hasNext = Boolean(status.zygote_next);
  $("row-zygote-next").hidden = !hasNext;
  if (hasNext) renderStatus("v-zygote-next", status.zygote_next);
  renderHero(overallTone(status));

  renderModules(modules);
}

function renderFailure() {
  renderStatus("v-monitor", "unknown");
  renderStatus("v-zygote", "unknown");
  renderStatus("v-daemon", "unknown");
  $("row-zygote-next").hidden = true;
  renderHero("unknown");
  $("v-subtitle").textContent = "-";
  $("v-root").textContent = "-";
  $("v-rom").textContent = "-";
  $("t-rom").hidden = true;
  $("v-kernel").textContent = "-";
  $("v-sdk").textContent = "-";
  $("v-abi").textContent = "-";
  renderModules([]);
}

function showToast(key, isError = false) {
  const toast = $("toast");
  toast.textContent = t(key);
  toast.className = `toast${isError ? " error" : ""}`;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    toast.className = "toast hidden";
  }, isError ? 2600 : 1400);
}

function setLoading(loading) {
  refreshing = loading;
  $("btn-reload").disabled = loading;
  $("btn-reload").classList.toggle("loading", loading);
}

async function refresh(showSuccessToast) {
  if (refreshing) return;
  const exec = getExec();
  if (!exec) {
    snapshot = null;
    renderOverview();
    showToast("bridge_missing", true);
    return;
  }

  setLoading(true);
  try {
    const result = await exec(unifiedStatusCommand());
    if (result.code !== 0 && !result.stdout) {
      snapshot = null;
      renderOverview();
      showToast("load_failed", true);
      return;
    }

    const sections = parseSectionOutput(result.stdout);
    snapshot = buildSnapshot(sections);
    renderOverview();
    if (showSuccessToast) showToast("reloaded");
  } finally {
    setLoading(false);
  }
}
