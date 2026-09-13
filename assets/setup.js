/* setup.js — the shared setup page controller. The host serves /api/config
 * (title wording + required files), receives uploads at /api/upload, and
 * answers /api/start with either ok or an error message. Validation wording
 * shown here comes from the server's config; this script never hardcodes a
 * game's names, sizes, or digests. */
(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const statusEl = $("status");
  const bar = $("bar");
  const barFill = bar.firstElementChild;
  const startBtn = $("start");
  const againBtn = $("again");
  const drop = $("drop");
  const picker = $("picker");
  const list = $("files");

  let config = null;
  let chosen = new Map(); // spec key -> {file, name}
  let phase = "picking"; // picking | uploading | ready

  function say(message, kind) {
    statusEl.textContent = message;
    statusEl.className = kind || "";
  }

  function showProgress(fraction) {
    bar.style.display = "block";
    barFill.style.width = `${Math.round(100 * Math.min(1, Math.max(0, fraction)))}%`;
  }

  function hideProgress() {
    bar.style.display = "none";
    barFill.style.width = "0%";
  }

  function renderList() {
    list.textContent = "";
    for (const spec of config.files) {
      const item = document.createElement("li");
      item.className = "step";
      const mark = document.createElement("span");
      mark.className = "mark";
      const label = document.createElement("span");
      label.className = "label";
      label.textContent = spec.label;
      item.append(mark, label);
      const entry = chosen.get(spec.key);
      if (entry) {
        item.classList.add("done");
        mark.textContent = "✓";
      }
      list.append(item);
    }
  }

  function markActive() {
    [...list.children].forEach((item, index) => {
      const spec = config.files[index];
      item.classList.toggle("active", !chosen.has(spec.key) && phase === "picking");
      item.classList.toggle("done", chosen.has(spec.key));
    });
  }

  async function fetchConfig() {
    const response = await fetch("api/config");
    if (!response.ok) {
      say("The setup service is unavailable.", "err");
      throw new Error("config unavailable");
    }
    return response.json();
  }

  function matchSpec(file) {
    for (const spec of config.files) {
      if (file.name === spec.name && !chosen.has(spec.key)) return spec;
    }
    if (config.accepts_archive && /\.zip$/i.test(file.name) && !chosen.has("archive")) {
      return { key: "archive", label: file.name, single_archive: true };
    }
    return null;
  }

  function acceptFiles(files) {
    if (phase !== "picking") return;
    const picked = [...files];
    if (picked.length === 1) {
      const spec = matchSpec(picked[0]);
      if (spec && spec.single_archive) {
        chosen.set("archive", { file: picked[0], name: picked[0].name });
        renderList();
        markActive();
        startBtn.disabled = false;
        say("Archive selected.");
        return;
      }
    }
    let unknown = false;
    for (const file of picked) {
      const spec = matchSpec(file);
      if (!spec) { unknown = true; continue; }
      chosen.set(spec.key, { file, name: file.name });
    }
    renderList();
    markActive();
    if (unknown) {
      say("Some files were not recognised. Each file must be named exactly as shown.", "err");
    } else {
      say("");
    }
    startBtn.disabled = false;
  }

  async function uploadOne(file, spec, singleArchive) {
    const response = await fetch("api/upload", {
      method: "POST",
      headers: { "X-Setup-Name": spec.name, "X-Setup-Key": spec.key },
      body: file,
    });
    if (!response.ok) {
      let message = `The upload of ${file.name} failed`;
      try {
        const report = await response.json();
        if (report && report.error) message = report.error;
      } catch { /* keep the generic wording */ }
      throw new Error(message);
    }
    return response.json();
  }

  async function start() {
    if (phase !== "picking") return;
    phase = "uploading";
    picker.disabled = true;
    startBtn.disabled = true;
    markActive();
    try {
      const archive = chosen.get("archive");
      if (archive) {
        say("Uploading the archive…");
        showProgress(0.2);
        await uploadOne(archive.file, { name: archive.name, key: "archive" }, true);
        showProgress(1);
      } else {
        for (let index = 0; index < config.files.length; ++index) {
          const spec = config.files[index];
          const entry = chosen.get(spec.key);
          if (!entry) throw new Error("Some required files are still missing.");
          say(`Uploading ${spec.name} (${index + 1} of ${config.files.length})…`);
          showProgress(index / config.files.length);
          await uploadOne(entry.file, spec, false);
        }
        showProgress(1);
      }
      say("Validating…");
      const response = await fetch("api/start", { method: "POST" });
      const report = await response.json().catch(() => null);
      if (!response.ok || !report || report.ok !== true) {
        throw new Error((report && report.error) || "The files were not accepted.");
      }
      say(report.message || "Setup complete.", "ok");
      phase = "ready";
      startBtn.style.display = "none";
      drop.style.display = "none";
      again.style.display = "block";
      again.disabled = false;
      markActive();
    } catch (error) {
      say(error instanceof Error ? error.message : "Setup failed.", "err");
      phase = "picking";
      picker.disabled = false;
      chosen = new Map();
      renderList();
      markActive();
      startBtn.disabled = false;
    } finally {
      hideProgress();
    }
  }

  function reset() {
    phase = "picking";
    chosen = new Map();
    picker.value = "";
    picker.disabled = false;
    startBtn.style.display = "block";
    startBtn.disabled = true;
    drop.style.display = "block";
    again.style.display = "none";
    renderList();
    markActive();
    say("");
  }

  startBtn.addEventListener("click", start);
  again.addEventListener("click", reset);
  picker.addEventListener("change", () => { acceptFiles(picker.files); picker.value = ""; });
  for (const event of ["dragenter", "dragover"]) {
    drop.addEventListener(event, (e) => { e.preventDefault(); drop.classList.add("drag"); });
  }
  for (const event of ["dragleave", "drop"]) {
    drop.addEventListener(event, (e) => { e.preventDefault(); drop.classList.remove("drag"); });
  }
  drop.addEventListener("drop", (e) => acceptFiles(e.dataTransfer.files));

  fetchConfig().then((loaded) => {
    config = loaded;
    document.title = config.title;
    $("pick-hint").textContent = config.hint;
    $("start").textContent = config.start_label || "Start";
    for (const step of config.steps || []) {
      const item = document.createElement("li");
      item.className = "step";
      const mark = document.createElement("span");
      mark.className = "mark";
      const label = document.createElement("span");
      label.className = "label";
      label.textContent = step.label;
      item.append(mark, label);
      list.append(item);
    }
    markActive();
    say("");
  });
})();
