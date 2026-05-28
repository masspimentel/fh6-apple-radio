// FH6 Universal Radio dashboard. Vanilla JS, no build step. `state` holds
// the latest /api/state; `cfg` holds the latest /api/config. Render functions
// are idempotent and only touch nodes whose displayed value changed.

const $  = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

const api = {
  async get(path)        { return (await fetch(path)).json(); },
  async send(path, body, method = "POST") {
    const r = await fetch(path, {
      method,
      headers: body ? { "content-type": "application/json" } : {},
      body:    body ? JSON.stringify(body) : undefined,
    });
    if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
    return r.json().catch(() => ({}));
  },
};

let state = null;
let cfg   = null;

let appleMusic = null;
let appleMusicConfigured = false;
let appleMusicSyncTimer = null;

const fmt = ms => {
  if (!ms || ms < 0) return "0:00";
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
};

const toast = (msg, isErr = false) => {
  const el = document.createElement("div");
  el.className = "toast" + (isErr ? " err" : "");
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 2400);
};

// Only write when the displayed value changes, to avoid cursor jumps in inputs.
const setText = (el, v) => { if (el && el.textContent !== String(v)) el.textContent = v; };

function renderStatus() {
  const ok = state?.game?.attached;
  const sub = $("#status");
  sub.className = "subtitle " + (ok ? "ok" : "err");
  sub.textContent = ok ? "connected" : "bridge offline";
}

function renderNowPlaying() {
  const t = state?.track || {};
  const a = state?.sources?.active;
  setText($("#np-title"),  t.title  || "Nothing playing");
  setText($("#np-artist"), t.artist ? `${t.artist}${t.album ? " · " + t.album : ""}` : "");
  setText($("#np-pos"), fmt(t.position_ms));
  setText($("#np-dur"), fmt(t.duration_ms));
  const pct = (t.duration_ms && t.position_ms)
    ? Math.min(100, (t.position_ms / t.duration_ms) * 100)
    : 0;
  $("#np-fill").style.width = pct + "%";

  const src = state?.sources?.available?.find(s => s.name === a);
  const playing = src?.playback_state === "playing";
  $("#t-play").textContent = playing ? "⏸" : "▶";
}

function sourceDetailLine(s) {
  if (s.name === "local_files" && s.details?.track_count != null) {
    const n = s.details.track_count;
    return `${n} track${n === 1 ? "" : "s"} indexed`;
  }
  return null;
}

function renderSources() {
  const wrap = $("#sources");
  const available = state?.sources?.available || [];
  const active = state?.sources?.active;
  const sig = available.map(s =>
    `${s.name}:${s.playback_state}:${s.auth_state}:${s.details?.track_count ?? ""}:${s.name===active}`
  ).join("|");
  if (wrap.dataset.sig === sig) return;
  wrap.dataset.sig = sig;

  wrap.innerHTML = "";
  for (const s of available) {
    const tile = document.createElement("button");
    tile.className = "source" + (s.name === active ? " active" : "");
    tile.type = "button";
    const stateCls = s.auth_state === "needs_auth" ? "warn"
                   : s.auth_state === "error"       ? "err" : "";
    const detail = sourceDetailLine(s);
    const showNote = (s.auth_state === "needs_auth" || s.auth_state === "error") && s.auth_instructions;
    tile.innerHTML = `
      <div class="name">${s.display_name}</div>
      <div class="state ${stateCls}">${s.playback_state}${s.auth_state !== "none_required" ? " - " + s.auth_state.replace("_", " ") : ""}${detail ? " - " + detail : ""}</div>
      ${showNote ? `<div class="auth-note">${s.auth_instructions}</div>` : ""}
    `;
    tile.addEventListener("click", async () => {
      try { await api.send("/api/source/switch", { source: s.name }); }
      catch (e) { toast(e.message, true); }
    });
    wrap.appendChild(tile);
  }

  // Cast box only makes sense while YT is registered.
  $("#yt-cast-card").hidden = !available.some(s => s.name === "youtube_music");
}

let volDirty = false;
function renderOutput() {
  const gain = state?.audio?.output_gain ?? 0;
  if (!volDirty) {
    const slider = $("#vol");
    if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
    $("#vol-out").value = Math.round(gain * 100) + "%";
  }
}

const EQ_BAND_LABELS = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

const SCHEMA = [
  ["general", "General", [
    ["port",            "Port",            "number", 1, 65535],
    ["ring_buffer_mb",  "Ring buffer (MB)","number", 1, 64],
    ["default_source",  "Default source",  "text"],
    ["fallback_source", "Fallback source", "text"],
  ]],
  ["local_files", "Local files", [
    ["enabled",     "Enabled",        "checkbox"],
    ["music_dir",   "Music directory","text"],
    ["recursive",   "Scan subfolders","checkbox"],
    ["shuffle",     "Shuffle",        "checkbox"],
  ]],
  ["youtube_music", "YouTube Music", [
    ["enabled",          "Enabled",                "checkbox"],
    ["cookies_path",     "cookies.txt (optional)", "text"],
    ["yt_dlp_path",      "yt-dlp path (optional)", "text"],
    ["ffmpeg_path",      "ffmpeg path (optional)", "text"],
    ["default_playlist", "Default playlist URL",   "text"],
    ["shuffle",          "Shuffle",                "checkbox"],
  ]],
  ["apple_music", "Apple Music", [
    ["enabled", "Enabled", "checkbox"],
    ["developer_token", "MusicKit developer token", "password"],
    ["storefront", "Storefront", "text"],
    ["playback_mode", "Playback mode", "text"],
  ]],
  ["audio", "Audio", [
    ["output_gain", "Output gain", "number", 0, 1, 0.01],
  ]],
  ["playback", "Playback", [
    ["race_start_playback",  "Race start",                "select",   ["next", "restart", "ignore"]],
    ["quick_station_skip",   "Quick station skip",        "checkbox"],
    ["volume_normalization", "Normalize loudness",        "checkbox"],
    ["equalizer_enabled",    "Equalizer",                 "checkbox"],
    ["equalizer_bands",      "Equalizer bands",           "bands"],
  ]],
];

function field(section, [key, label, type, a, b, c]) {
  const id  = `f-${section}-${key}`;
  const cur = cfg?.[section]?.[key];
  if (type === "checkbox") {
    return `<div class="field checkbox">
      <input type="checkbox" id="${id}" data-section="${section}" data-key="${key}" ${cur ? "checked" : ""}>
      <label for="${id}">${label}</label>
    </div>`;
  }
  if (type === "select") {
    const opts = (a || []).map(v =>
      `<option value="${v}" ${cur === v ? "selected" : ""}>${v}</option>`).join("");
    return `<div class="field">
      <label for="${id}">${label}</label>
      <select id="${id}" data-section="${section}" data-key="${key}">${opts}</select>
    </div>`;
  }
  if (type === "bands") {
    const vals = Array.isArray(cur) ? cur : [0, 0, 0, 0, 0];
    const rows = EQ_BAND_LABELS.map((lbl, i) => `
      <div class="band">
        <span class="band-label">${lbl}</span>
        <input type="range" min="-6" max="6" step="0.5" value="${vals[i] ?? 0}"
               data-section="${section}" data-key="${key}" data-index="${i}">
        <output>${(vals[i] ?? 0).toFixed(1)} dB</output>
      </div>`).join("");
    return `<div class="field bands"><label>${label}</label>${rows}</div>`;
  }
  const inputType = type === "password" ? "password" : type;
  const attrs = type === "number" ? ` min="${a ?? ''}" max="${b ?? ''}" step="${c ?? 1}"` : "";

  return `
    <label class="field">
      <span>${label}</span>
      <input id="${id}" data-section="${section}" data-key="${key}" type="${inputType}" value="${cur ?? ""}"${attrs}>
    </label>
  `;
}

function renderSettings() {
  const form = $("#settings-form");
  form.innerHTML = SCHEMA.map(([sec, title, fields]) =>
    `<fieldset><legend>${title}</legend>${fields.map(f => field(sec, f)).join("")}</fieldset>`
  ).join("");
  // Live "X.X dB" readout next to each EQ slider.
  $$(".field.bands input[type='range']", form).forEach(r => {
    const out = r.nextElementSibling;
    r.addEventListener("input", () => { out.textContent = `${parseFloat(r.value).toFixed(1)} dB`; });
  });
}

function collectSettings() {
  const patch = {};
  $$("#settings-form [data-section]").forEach(el => {
    const sec = el.dataset.section;
    const key = el.dataset.key;
    (patch[sec] ??= {});
    if (el.dataset.index !== undefined) {
      const arr = (patch[sec][key] ??= []);
      arr[parseInt(el.dataset.index, 10)] = parseFloat(el.value);
      return;
    }
    if (el.type === "checkbox")     patch[sec][key] = el.checked;
    else if (el.type === "number" ||
             el.type === "range")   patch[sec][key] = parseFloat(el.value);
    else                            patch[sec][key] = el.value;
  });
  return patch;
}

function openDrawer() {
  $("#drawer").classList.add("open");
  $("#scrim").hidden = false;
  $("#drawer").setAttribute("aria-hidden", "false");
}
function closeDrawer() {
  $("#drawer").classList.remove("open");
  $("#scrim").hidden = true;
  $("#drawer").setAttribute("aria-hidden", "true");
}

async function transport(action) {
  const src = state?.sources?.active;
  if (!src) return;
  // Centre button is a smart play/pause toggle.
  if (action === "play") {
    const s = state.sources.available.find(x => x.name === src);
    if (s?.playback_state === "playing") action = "pause";
  }
  try { await api.send(`/api/source/${src}/${action}`); }
  catch (e) { toast(e.message, true); }
}

async function ensureAppleMusicConfig() {
  if (!cfg) {
    cfg = await api.get("/api/config");
  }

  const am = cfg?.apple_music;

  if (!am?.enabled) {
    throw new Error("Apple Music is disabled in settings.");
  }

  if (!am?.developer_token) {
    throw new Error("Apple Music developer token is missing.");
  }

  if (!window.MusicKit) {
    throw new Error("MusicKit JS did not load.");
  }

  if (!appleMusicConfigured) {
    await MusicKit.configure({
      developerToken: am.developer_token,
      app: {
        name: "FH6 Apple Radio",
        build: "1.0.0"
      }
    });

    appleMusic = MusicKit.getInstance();
    appleMusicConfigured = true;
  }

  return appleMusic;
}

async function authorizeAppleMusic() {
  const music = await ensureAppleMusicConfig();
  await music.authorize();

  await api.send("/api/source/apple_music/now-playing", {
    title: "",
    artist: "",
    album: "",
    playback_state: "stopped",
    authenticated: true
  });

  toast("Apple Music authorized");
  startAppleMusicSync();
}

async function unauthorizeAppleMusic() {
  const music = await ensureAppleMusicConfig();
  await music.unauthorize();

  await api.send("/api/source/apple_music/now-playing", {
    title: "",
    artist: "",
    album: "",
    playback_state: "stopped",
    authenticated: false
  });

  toast("Apple Music signed out");
}

function appleArtworkUrl(artwork, size = 300) {
  if (!artwork?.url) return "";
  return artwork.url
    .replace("{w}", String(size))
    .replace("{h}", String(size));
}

function appleSongToTrack(song) {
  const attrs = song?.attributes || {};

  return {
    title: attrs.name || "",
    artist: attrs.artistName || "",
    album: attrs.albumName || "",
    artwork_url: appleArtworkUrl(attrs.artwork),
    duration_ms: attrs.durationInMillis || 0,
    position_ms: 0,
    playback_state: "playing",
    authenticated: true
  };
}

async function syncAppleNowPlaying() {
  if (!appleMusic) return;

  const item = appleMusic.nowPlayingItem;
  const attrs = item?.attributes || {};

  const payload = {
    title: attrs.name || "",
    artist: attrs.artistName || "",
    album: attrs.albumName || "",
    artwork_url: appleArtworkUrl(attrs.artwork),
    duration_ms: attrs.durationInMillis || 0,
    position_ms: Math.floor((appleMusic.currentPlaybackTime || 0) * 1000),
    playback_state: appleMusic.isPlaying ? "playing" : "paused",
    authenticated: true
  };

  await api.send("/api/source/apple_music/now-playing", payload);
}

function startAppleMusicSync() {
  if (appleMusicSyncTimer) return;

  appleMusicSyncTimer = setInterval(() => {
    syncAppleNowPlaying().catch(() => {});
  }, 1000);
}

async function searchAppleMusic(query) {
  const music = await ensureAppleMusicConfig();

  const results = await music.api.search(query, {
    types: "songs",
    limit: 10
  });

  return results?.songs?.data || [];
}

function renderAppleMusicResults(songs) {
  const wrap = $("#am-results");
  if (!wrap) return;

  wrap.innerHTML = "";

  if (!songs.length) {
    wrap.innerHTML = `<p class="muted">No results found.</p>`;
    return;
  }

  for (const song of songs) {
    const attrs = song.attributes || {};
    const row = document.createElement("button");

    row.type = "button";
    row.className = "source";
    row.innerHTML = `
      <strong>${attrs.name || "Unknown title"}</strong>
      <small>${attrs.artistName || ""}${attrs.albumName ? " - " + attrs.albumName : ""}</small>
    `;

    row.addEventListener("click", async () => {
      try {
        const music = await ensureAppleMusicConfig();

        await api.send("/api/source/switch", { source: "apple_music" });

        await music.setQueue({
          song: song.id
        });

        await music.play();

        await api.send("/api/source/apple_music/now-playing", appleSongToTrack(song));

        startAppleMusicSync();
        toast("Playing Apple Music");
      } catch (e) {
        toast(e.message, true);
      }
    });

    wrap.appendChild(row);
  }
}

function renderAppleMusicCard() {
  const card = $("#apple-card");
  if (!card) return;

  const available = state?.sources?.available || [];
  card.hidden = !available.some(s => s.name === "apple_music");
}

function wire() {
  $("#t-play").onclick = () => transport("play");
  $("#t-next").onclick = () => transport("next");
  $("#t-prev").onclick = () => transport("previous");

  const vol = $("#vol");
  vol.addEventListener("input", () => {
    volDirty = true;
    $("#vol-out").value = Math.round(parseFloat(vol.value) * 100) + "%";
  });
  vol.addEventListener("change", async () => {
    try { await api.send("/api/options", { output_gain: parseFloat(vol.value) }); }
    catch (e) { toast(e.message, true); }
    setTimeout(() => { volDirty = false; }, 400);
  });

  $("#yt-cast").addEventListener("submit", async e => {
    e.preventDefault();
    const url = $("#yt-url").value.trim();
    if (!url) return;
    try {
      await api.send("/api/source/youtube_music/cast", { url });
      $("#yt-url").value = "";
      toast("Casting...");
    } catch (err) { toast(err.message, true); }
  });

  $("#yt-shuffle").addEventListener("click", async () => {
    const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
    if (!yt) return;
    const shuffle = !yt.details?.shuffle;
    try {
      await api.send("/api/source/youtube_music/shuffle", { shuffle });
      toast(shuffle ? "Shuffle on" : "Shuffle off");
    } catch (err) { toast(err.message, true); }
  });

  const amAuth = $("#am-auth");
  if (amAuth) {
    amAuth.addEventListener("click", async () => {
      try {
        await authorizeAppleMusic();
      } catch (e) {
        toast(e.message, true);
      }
    });
  }

  const amUnauth = $("#am-unauth");
  if (amUnauth) {
    amUnauth.addEventListener("click", async () => {
      try {
        await unauthorizeAppleMusic();
      } catch (e) {
        toast(e.message, true);
      }
    });
  }

  const amSearch = $("#am-search");
  if (amSearch) {
    amSearch.addEventListener("submit", async e => {
      e.preventDefault();

      const query = $("#am-query").value.trim();
      if (!query) return;

      try {
        const songs = await searchAppleMusic(query);
        renderAppleMusicResults(songs);
      } catch (err) {
        toast(err.message, true);
      }
    });
  }

  $("#open-settings").onclick  = async () => { cfg = await api.get("/api/config"); renderSettings(); openDrawer(); };
  $("#close-settings").onclick = closeDrawer;
  $("#scrim").onclick          = closeDrawer;
  $("#save-config").onclick    = async () => {
    try {
      cfg = await api.send("/api/config", collectSettings(), "PUT");
      toast("Saved");
      closeDrawer();
    } catch (e) { toast(e.message, true); }
  };
  $("#reload-config").onclick  = async () => {
    cfg = await api.send("/api/config/reload");
    renderSettings();
    toast("Reloaded from disk");
  };
}

// SSE if available, polling fallback otherwise.
function connect() {
  let es;
  try {
    es = new EventSource("/api/events");
    es.onmessage = e => { state = JSON.parse(e.data); render(); };
    es.onerror   = () => { es.close(); setTimeout(poll, 1000); };
  } catch { poll(); }
}
async function poll() {
  try { state = await api.get("/api/state"); render(); }
  catch { /* keep last state */ }
  setTimeout(poll, 1000);
}

function render() {
  renderStatus();
  renderNowPlaying();
  renderSources();
  renderOutput();
  renderAppleMusicCard();

  const yt = state?.sources?.available?.find(s => s.name === "youtube_music");
  const shuffleBtn = $("#yt-shuffle");
  if (shuffleBtn) {
    shuffleBtn.classList.toggle("active", !!yt?.details?.shuffle);
  }
}

async function initAppleMusic() {
  const cfg = state.config?.apple_music;

  if (!cfg?.enabled || !cfg?.developer_token) {
    throw new Error("Apple Music is not configured.");
  }

  await MusicKit.configure({
    developerToken: cfg.developer_token,
    app: {
      name: "FH6 Apple Radio",
      build: "1.0.0"
    }
  });

  const music = MusicKit.getInstance();
  await music.authorize();

  return music;
}

wire();
connect();
