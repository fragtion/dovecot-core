"use strict";
/* Dovecot Mesh status UI. Talks only to this node's /mesh/v1/ API. */

const API = "../v1/";
const REFRESH_MS = 2000;
const USER_LIMIT = 300;

const $ = (id) => document.getElementById(id);
const state = {
  status: null,
  users: null,
  activity: null,
  // server time minus browser time, so "ago" uses the server's clock
  skewMs: 0,
  lastOk: 0,
  busy: false,
  userQuery: "",
  userProblems: false,
};

function esc(value) {
  return String(value ?? "").replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[c]);
}

function nowSecs() {
  return (Date.now() + state.skewMs) / 1000;
}

function ago(ts) {
  if (!ts) return "never";
  let secs = Math.max(0, Math.round(nowSecs() - ts));
  if (secs < 60) return secs + "s ago";
  if (secs < 3600) return Math.floor(secs / 60) + "m ago";
  if (secs < 86400) return Math.floor(secs / 3600) + "h ago";
  return Math.floor(secs / 86400) + "d ago";
}

function duration(secs) {
  if (secs < 60) return secs + "s";
  if (secs < 3600) return Math.floor(secs / 60) + "m " + (secs % 60) + "s";
  if (secs < 86400) return Math.floor(secs / 3600) + "h " + Math.floor(secs % 3600 / 60) + "m";
  return Math.floor(secs / 86400) + "d " + Math.floor(secs % 86400 / 3600) + "h";
}

function clock(ts) {
  if (!ts) return "–";
  return new Date(ts * 1000).toLocaleString();
}

function pill(cls, text, title) {
  return `<span class="pill ${cls}"${title ? ` title="${esc(title)}"` : ""}>${esc(text)}</span>`;
}

async function api(path, opts = {}) {
  const resp = await fetch(API + path, {
    cache: "no-store",
    credentials: "same-origin",
    ...opts,
    headers: { "X-Requested-With": "mesh-ui", ...(opts.headers || {}) },
  });
  if (!resp.ok) {
    throw new Error(resp.status === 401 || resp.status === 403 ?
      "Not authorized – check mesh_api_key" :
      `${resp.status} ${resp.statusText}`);
  }
  return resp.json();
}

function toast(text) {
  const el = $("toast");
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { el.hidden = true; }, 2500);
}

async function post(path, message) {
  try {
    await api(path, { method: "POST" });
    toast(message);
    refresh();
  } catch (e) {
    toast("Failed: " + e.message);
  }
}

/* rendering */

function renderHeader(s) {
  const uptime = Math.max(0, Math.round(s.time_ms / 1000 - s.started));
  $("node-info").innerHTML =
    `<b>${esc(s.node)}</b> · ${esc(s.topology)} topology · ` +
    `${esc(s.version)} · up ${duration(uptime)}`;
  document.title = `${s.node} – Dovecot Mesh`;
}

function renderTiles(s) {
  const up = s.peers.filter((p) => p.up).length;
  const paused = s.peers.filter((p) => p.paused).length;
  const ok = s.peers.reduce((n, p) => n + p.syncs_ok, 0);
  const failed = s.peers.reduce((n, p) => n + p.syncs_failed, 0);
  const tiles = [
    { label: "Peers up", value: `${up} <small>/ ${s.peers.length}</small>`,
      cls: up < s.peers.length ? "bad" : "" },
    { label: "Users", value: s.users },
    { label: "Pending pushes", value: s.pending, cls: s.pending > 50 ? "warn" : "" },
    { label: "Running", value: `${s.running} <small>/ ${s.max_parallel_syncs}</small>` },
    { label: "Sessions ok", value: ok },
    { label: "Sessions failed", value: failed, cls: failed > 0 ? "warn" : "" },
  ];
  if (paused > 0)
    tiles.push({ label: "Paused peers", value: paused, cls: "warn" });
  $("tiles").innerHTML = tiles.map((t) =>
    `<div class="tile ${t.cls || ""}"><div class="label">${t.label}</div>` +
    `<div class="value">${t.value}</div></div>`).join("");
}

function peerState(p) {
  if (!p.up) return pill("bad", "down", p.last_error);
  if (p.paused) return pill("warn", "paused");
  return pill("ok", "up", p.up_since ? "up since " + clock(p.up_since) : "");
}

function renderNodes(s) {
  const head = `<tr><th>Node</th><th>State</th><th class="num">RTT</th>` +
    `<th class="num">Clock offset</th><th>Last heartbeat</th>` +
    `<th class="num">Pending</th><th class="num">Running</th>` +
    `<th class="num">Sessions ok</th><th class="num">Failed</th>` +
    `<th>Version</th><th>Last error</th><th></th></tr>`;
  const self = `<tr class="self"><td><b>${esc(s.node)}</b>` +
    `<span class="tag">this node</span></td><td>${pill("ok", "up")}</td>` +
    `<td class="num muted">–</td><td class="num muted">–</td>` +
    `<td class="muted">–</td><td class="num">${s.pending}</td>` +
    `<td class="num">${s.running}</td><td class="num muted">–</td>` +
    `<td class="num muted">–</td><td>${esc(s.version)}</td><td></td><td></td></tr>`;
  const rows = s.peers.map((p) => {
    const offset = Math.abs(p.clock_offset_ms) >= 1000 ?
      pill("warn", p.clock_offset_ms + " ms", "Clocks differ, which " +
        "makes flag conflict resolution less accurate") :
      p.clock_offset_ms + " ms";
    const version = p.remote.version && p.remote.version !== s.version ?
      pill("warn", p.remote.version, "Differs from this node") :
      esc(p.remote.version || "–");
    const action = p.paused ?
      `<button data-act="resume" data-peer="${esc(p.name)}">Resume</button>` :
      `<button data-act="pause" data-peer="${esc(p.name)}">Pause</button>`;
    return `<tr><td><b>${esc(p.name)}</b> <span class="muted mono">${esc(p.host)}</span></td>` +
      `<td>${peerState(p)}</td>` +
      `<td class="num">${p.up ? p.rtt_ms + " ms" : "–"}</td>` +
      `<td class="num">${p.up ? offset : "–"}</td>` +
      `<td>${ago(p.last_seen)}</td>` +
      `<td class="num">${p.up ? p.remote.pending : "–"}</td>` +
      `<td class="num">${p.up ? p.remote.running : "–"}</td>` +
      `<td class="num">${p.syncs_ok}</td>` +
      `<td class="num">${p.syncs_failed}</td>` +
      `<td>${version}</td>` +
      `<td class="err" title="${esc(p.last_error)}">${esc(p.last_error)}</td>` +
      `<td class="actions">${action}</td></tr>`;
  });
  $("nodes").innerHTML = head + self + rows.join("");
}

const STATUS_PILL = {
  ok: ["ok", "ok"],
  relayed: ["", "via anchor"],
  pending: ["info", "pending"],
  syncing: ["info syncing", "syncing"],
  error: ["bad", "error"],
  never: ["warn", "never synced"],
};

function userPeerPill(up) {
  const [cls, text] = STATUS_PILL[up.status] || ["", up.status];
  const lines = [
    `last ok: ${up.last_ok ? clock(up.last_ok) : "never"}`,
    `last attempt: ${up.last_attempt ? clock(up.last_attempt) : "never"}`,
    `sessions ok ${up.ok_count}, failed ${up.fail_count}`,
    `state: ${up.has_state ? "generation " + up.state_gen : "none (next is stateless)"}`,
  ];
  if (up.pending_since) lines.push(`pending since ${clock(up.pending_since)}`);
  if (up.last_error) lines.push(`last error: ${up.last_error}`);
  return pill(cls, text, lines.join("\n"));
}

function renderUsers(s, u) {
  if (!u) return;
  const peers = s.peers.map((p) => p.name);
  const head = `<tr><th>User</th><th>Anchor</th>` +
    peers.map((n) => `<th>${esc(n)}</th>`).join("") +
    `<th>Last change</th><th>Verified</th><th></th></tr>`;
  const users = u.users.slice().sort((a, b) => a.user.localeCompare(b.user));
  if (users.length === 0) {
    const msg = state.userQuery || state.userProblems ?
      "No users match." : "No users yet. Users appear after their first change or sync.";
    $("users").innerHTML = head + `<tr><td class="empty" colspan="${peers.length + 5}">${msg}</td></tr>`;
  } else {
    $("users").innerHTML = head + users.map((user) => {
      const byPeer = Object.fromEntries(user.peers.map((p) => [p.peer, p]));
      const anchor = user.anchor === s.node ?
        `${esc(user.anchor)} <span class="tag">here</span>` : esc(user.anchor);
      return `<tr><td><b>${esc(user.user)}</b></td><td>${anchor}</td>` +
        peers.map((n) => `<td>${byPeer[n] ? userPeerPill(byPeer[n]) : ""}</td>`).join("") +
        `<td class="muted">${user.last_change ? ago(user.last_change) : "–"}</td>` +
        `<td>${verifyPill(user)}</td>` +
        `<td class="actions"><button data-act="verify" data-user="${esc(user.user)}">Verify</button>` +
        `<button data-act="sync" data-user="${esc(user.user)}">Sync now</button>` +
        `<button data-act="full" data-user="${esc(user.user)}">Full resync</button></td></tr>`;
    }).join("");
  }
  $("users-foot").textContent = u.matched > u.users.length ?
    `Showing ${u.users.length} of ${u.matched} matching users (${u.total} in total). Use the filter to narrow down.` :
    (state.userQuery || state.userProblems) ?
    `${u.matched} of ${u.total} users match.` : "";
}

const VERIFY_PILL = {
  ok: ["ok", "match"],
  mismatch: ["bad", "differs"],
  error: ["warn", "incomplete"],
};

function verifyPill(user) {
  if (!user.verify_result) return `<span class="muted">–</span>`;
  const [cls, text] = VERIFY_PILL[user.verify_result] || ["", user.verify_result];
  return pill(cls, `${text} · ${ago(user.verify_time)}`, clock(user.verify_time));
}

function renderVerify(r) {
  const nodes = r.nodes.map((n) => n.name);
  const differing = r.mailboxes.filter((m) => !m.match).length;
  const failed = r.nodes.filter((n) => n.error);
  const [cls, text] = VERIFY_PILL[r.result] || ["", r.result];
  let summary = `<div class="summary">${pill(cls, text)} ` +
    `<p>${r.mailboxes.length} mailboxes compared on ${nodes.length - failed.length} of ${nodes.length} nodes` +
    (differing ? `, ${differing} differ.` : ".") + ` Each cell shows the number of mails and the start of a ` +
    `hash of their UIDs, GUIDs and flags.</p>`;
  for (const n of failed)
    summary += `<p class="err">${esc(n.name)}: ${esc(n.error)}</p>`;
  summary += "</div>";
  const head = `<tr><th>Mailbox</th><th>Status</th>` +
    nodes.map((n) => `<th>${esc(n)}</th>`).join("") + `</tr>`;
  const rows = r.mailboxes.map((m) => {
    const first = m.nodes.find((c) => c);
    const cells = m.nodes.map((c, i) => {
      if (r.nodes[i].error) return `<td class="muted">?</td>`;
      if (!c) return `<td class="diff">missing</td>`;
      const same = first && c.messages === first.messages && c.hash === first.hash;
      return `<td class="mono${m.match || same ? "" : " diff"}" title="${esc(c.hash)}">` +
        `${c.messages} · ${esc(c.hash.slice(0, 8))}</td>`;
    }).join("");
    return `<tr><td>${esc(m.name)}</td><td>${m.match ? pill("ok", "match") : pill("bad", "differs")}</td>${cells}</tr>`;
  }).join("");
  return summary + `<div class="scroll"><table>${head}${rows}</table></div>`;
}

async function verify(user) {
  const dialog = $("verify-dialog");
  $("verify-title").textContent = `Verify ${user}`;
  $("verify-body").innerHTML = `<div class="empty">Comparing mailboxes on all nodes…</div>`;
  if (!dialog.open) dialog.showModal();
  try {
    const r = await api(`verify?user=${encodeURIComponent(user)}`, { method: "POST" });
    $("verify-body").innerHTML = renderVerify(r);
    refresh();
  } catch (e) {
    $("verify-body").innerHTML = `<div class="empty err">Failed: ${esc(e.message)}</div>`;
  }
}

$("verify-close").addEventListener("click", () => $("verify-dialog").close());

const RESULT_PILL = {
  ok: ["ok", "ok"],
  changed: ["ok", "ok, changed meanwhile"],
  failed: ["bad", "failed"],
};

function renderActivity(a) {
  if (!a) return;
  const head = `<tr><th>Time</th><th>User</th><th>Peer</th><th>Result</th>` +
    `<th class="num">Duration</th><th>Mode</th><th>Error</th></tr>`;
  if (a.activity.length === 0) {
    $("activity").innerHTML = head + `<tr><td class="empty" colspan="7">No sessions yet.</td></tr>`;
    return;
  }
  $("activity").innerHTML = head + a.activity.map((e) => {
    const [cls, text] = RESULT_PILL[e.result] || ["", e.result];
    const mode = e.full ? "full" : e.stateful ? "incremental" : "stateless";
    return `<tr><td title="${esc(clock(e.time))}">${ago(e.time)}</td>` +
      `<td>${esc(e.user)}</td><td>${esc(e.peer)}</td><td>${pill(cls, text)}</td>` +
      `<td class="num">${e.ms} ms</td><td class="muted">${mode}</td>` +
      `<td class="err" title="${esc(e.error)}">${esc(e.error)}</td></tr>`;
  }).join("");
}

function render() {
  const s = state.status;
  if (!s) return;
  renderHeader(s);
  renderTiles(s);
  renderNodes(s);
  renderUsers(s, state.users);
  renderActivity(state.activity);
}

function usersPath() {
  const params = new URLSearchParams({ limit: USER_LIMIT });
  if (state.userQuery) params.set("q", state.userQuery);
  if (state.userProblems) params.set("problems", "1");
  return "users?" + params;
}

async function refresh() {
  if (state.busy) return;
  state.busy = true;
  try {
    const [status, users, activity] = await Promise.all([
      api("status"), api(usersPath()), api("activity?limit=50"),
    ]);
    state.skewMs = status.time_ms - Date.now();
    state.status = status;
    state.users = users;
    state.activity = activity;
    state.lastOk = Date.now();
    $("banner").hidden = true;
    render();
  } catch (e) {
    const banner = $("banner");
    banner.textContent = "Can't reach the mesh service: " + e.message;
    banner.hidden = false;
  } finally {
    state.busy = false;
    updateLive();
  }
}

function updateLive() {
  const dot = $("live-dot");
  if (!state.lastOk) {
    dot.className = "dot stale";
    $("refreshed").textContent = "not connected";
    return;
  }
  const age = (Date.now() - state.lastOk) / 1000;
  dot.className = "dot " + (age < REFRESH_MS / 1000 * 3 ? "live" : "stale");
  $("refreshed").textContent = age < 2 ? "live" : "updated " + Math.round(age) + "s ago";
}

document.addEventListener("click", (ev) => {
  const btn = ev.target.closest("button[data-act]");
  if (!btn) return;
  const { act, user, peer } = btn.dataset;
  const u = encodeURIComponent(user || "");
  const p = encodeURIComponent(peer || "");
  if (act === "verify") verify(user);
  else if (act === "sync") post(`sync?user=${u}`, `Sync of ${user} queued`);
  else if (act === "full") post(`sync?user=${u}&full=1`, `Full resync of ${user} queued`);
  else if (act === "pause") post(`pause?peer=${p}`, `${peer} paused`);
  else if (act === "resume") post(`resume?peer=${p}`, `${peer} resumed`);
});

let queryTimer;
$("user-q").addEventListener("input", (ev) => {
  clearTimeout(queryTimer);
  queryTimer = setTimeout(() => {
    state.userQuery = ev.target.value.trim();
    refresh();
  }, 250);
});
$("user-problems").addEventListener("change", (ev) => {
  state.userProblems = ev.target.checked;
  refresh();
});

refresh();
setInterval(refresh, REFRESH_MS);
setInterval(updateLive, 1000);
