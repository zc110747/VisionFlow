// Read-only probe of the ArmPilot joint WebSocket (ws://127.0.0.1:8090/ws/joint).
//
// It SUBSCRIBES ONLY. It never sends a joint_command / device_command, so it
// cannot move the arm and cannot disturb the visionflow agent that already
// holds the TCP control port.
//
//   node ws_probe.mjs hello              -> print the hello + first state, exit
//   node ws_probe.mjs record <ms> <out>  -> record every joint_state to <out> (JSONL)
//
// Node >= 22 has a global WebSocket, so there is no dependency to install.
const URL = "ws://127.0.0.1:8090/ws/joint";
const mode = process.argv[2] || "hello";
const ms = Number(process.argv[3] || 30000);
const outPath = process.argv[4] || "ws_record.jsonl";

import { appendFileSync, writeFileSync } from "node:fs";

const ws = new WebSocket(URL);
let count = 0;
const t0 = Date.now();

ws.addEventListener("open", () => {
  console.error(`[ws] connected ${URL} (read-only subscriber)`);
  // Ask the server to (re)send hello + device status + state snapshot.
  ws.send(JSON.stringify({ version: 1, type: "status_request" }));
});

ws.addEventListener("message", (ev) => {
  const line = typeof ev.data === "string" ? ev.data : String(ev.data);
  let msg;
  try { msg = JSON.parse(line); } catch { return; }

  if (mode === "hello") {
    if (msg.type === "hello") {
      console.log("HELLO:", JSON.stringify(msg, null, 1));
    } else if (msg.type === "device_status") {
      console.log("DEVICE_STATUS:", JSON.stringify(msg));
    } else if (msg.type === "joint_state" && count++ < 2) {
      console.log("JOINT_STATE:", JSON.stringify(msg));
    }
    if (count >= 2 && msg.type !== "hello") { /* keep going until the timer */ }
    return;
  }

  if (count === 0) writeFileSync(outPath, "");
  // Absolute epoch ms (aligns with the server's received_at) plus ms since start.
  appendFileSync(outPath, JSON.stringify({ t0, t: Date.now(), rel: Date.now() - t0, ...msg }) + "\n");
  count++;
  if (count % 50 === 0) console.error(`[ws] ${count} frames`);
});

ws.addEventListener("error", (e) => {
  console.error("[ws] error:", e.message || e);
});

ws.addEventListener("close", () => {
  console.error(`[ws] closed after ${count} frames`);
  process.exit(0);
});

setTimeout(() => {
  ws.close();
  if (mode === "hello") {
    console.log(`frames_seen=${count}`);
    process.exit(0);
  }
  console.error(`[ws] done: ${count} frames -> ${outPath}`);
  process.exit(0);
}, mode === "hello" ? 3000 : ms);
