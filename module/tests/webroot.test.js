const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const WEBROOT = path.resolve(__dirname, "..", "src", "webroot");

function loadScript(name) {
  const timers = new Map();
  let nextTimer = 1;
  const elements = new Map();
  const document = {
    addEventListener() {},
    documentElement: { lang: "en" },
    getElementById(id) {
      if (!elements.has(id)) {
        elements.set(id, {
          className: "",
          disabled: false,
          hidden: false,
          textContent: "",
          classList: { add() {}, toggle() {} },
          setAttribute() {},
        });
      }
      return elements.get(id);
    },
    querySelectorAll() {
      return [];
    },
  };
  const window = {};
  const context = {
    Promise,
    Uint8Array,
    TextDecoder,
    atob,
    clearTimeout(id) {
      timers.delete(id);
    },
    console,
    document,
    localStorage: {
      getItem() {
        return null;
      },
      setItem() {},
    },
    navigator: { language: "en" },
    setTimeout(callback, delay) {
      const id = nextTimer++;
      timers.set(id, { callback, delay });
      return id;
    },
    window,
  };
  vm.createContext(context);
  const source = fs
    .readFileSync(path.join(WEBROOT, name), "utf8")
    .replaceAll("@WORK_DIRECTORY@", "/data/system/neozygisk")
    .replaceAll("@MODULE_ID@", "zygisksu");
  vm.runInContext(source, context);
  return { context, timers, window };
}

test("authoritative status is independent of module display text", () => {
  const { context } = loadScript("script.js");
  const propText = [
    "id=zygisksu",
    "name=NeoZygisk",
    "version=v2.4",
    "author=JingMatrix",
    "description=Zygote injection with ptrace.",
    "\tmonitor: stopped",
    "\tzygote64: not injected",
    "\tdaemon64: running",
    "\t\tRoot: KernelSU",
    "\t\tModules (1):",
    "\t\t\tzygote_next: cleaned",
    "\t\t\tmonitor: tracing",
    "\t\t\tversion=9.9",
    "\tzygote_next: cleanup failed",
  ].join("\n");
  const statusText = [
    "\tmonitor: stopped",
    "\tzygote64: not injected",
    "\tdaemon64: running",
    "\tzygote_next: cleanup failed",
  ].join("\n");

  const sections = context.parseSectionOutput(
    `---PROP---\n${Buffer.from(propText).toString("base64")}\n` +
      `---STATUS---\n${Buffer.from(statusText).toString("base64")}\n` +
      "---ROOT---\n---ROM---\n---DEVICE---\n---MODULES---\n",
  );
  const snapshot = context.buildSnapshot(sections);

  assert.equal(snapshot.status.monitor, "stopped");
  assert.equal(snapshot.status.zygote64, "not injected");
  assert.equal(snapshot.status.daemon64, "running");
  assert.equal(snapshot.status.zygote_next, "cleanup failed");
  assert.equal(snapshot.prop.version, "v2.4");
  assert.equal(context.overallTone(snapshot.status), "down");
});

test("a zygote that needs restart is a warning, not unknown", () => {
  const { context } = loadScript("script.js");
  const status = context.parseStatusProp(
    "\tmonitor: \t😋 tracing\n" +
      "\tzygote64:\t⚠️ needs restart\n" +
      "\tdaemon64:\t😋 running\n",
  );
  assert.equal(status.zygote64, "needs restart");
  assert.equal(context.overallTone(status), "warn");
});

test("a pending reboot is surfaced as a warning", () => {
  const { context } = loadScript("script.js");
  const status = context.parseStatusProp(
    "\tmonitor: \t😋 tracing\n" +
      "\tzygote64:\t😋 injected\n" +
      "\tdaemon64:\t😋 running\n" +
      "\tpending_reboot:\t⚠️ reboot required\n",
  );
  assert.equal(status.pending_reboot, "reboot required");
  assert.equal(context.overallTone(status), "warn");
});

test("status command uses the configured work directory and module id", () => {
  const { context } = loadScript("script.js");
  const command = context.unifiedStatusCommand();
  assert.match(command, /\/data\/adb\/modules\/zygisksu\/bin\/zygisk-ptrace64/);
  assert.match(command, /\/data\/system\/neozygisk\/status/);
});

test("callback bridge is invoked exactly once", async () => {
  const { context, timers, window } = loadScript("kernelsu.js");
  let calls = 0;
  let callbackName = "";
  const bridge = {
    exec(command, options, callback) {
      calls += 1;
      callbackName = callback;
      return undefined;
    },
  };

  const resultPromise = context.callExec(bridge, "true", {});
  assert.equal(calls, 1);
  assert.equal(typeof window[callbackName], "function");

  window[callbackName](0, "ok", "");
  const result = await resultPromise;

  assert.equal(calls, 1);
  assert.equal(result.code, 0);
  assert.equal(result.stdout, "ok");
  assert.equal(timers.size, 0);
});

test("bridge timeout rejects without running the command again", async () => {
  const { context, timers } = loadScript("kernelsu.js");
  let calls = 0;
  const bridge = {
    exec() {
      calls += 1;
      return undefined;
    },
  };

  const resultPromise = context.callExec(bridge, "true", {});
  const rejection = assert.rejects(resultPromise, /Bridge exec timed out/);
  assert.equal(calls, 1);
  assert.equal(timers.size, 1);

  const [{ callback }] = timers.values();
  callback();
  await rejection;
  assert.equal(calls, 1);
});

test("promise bridge result remains supported", async () => {
  const { context } = loadScript("kernelsu.js");
  let calls = 0;
  const bridge = {
    exec() {
      calls += 1;
      return Promise.resolve({ errno: 0, stdout: "promise result", stderr: "" });
    },
  };

  const result = await context.callExec(bridge, "true", {});
  assert.equal(calls, 1);
  assert.equal(result.stdout, "promise result");
});
