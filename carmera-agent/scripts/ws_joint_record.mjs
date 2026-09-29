// Read-only recorder for the ArmPilot joint WebSocket.
//
// WHY: the arm has NO position feedback - the backend's joint_state is the
// firmware's internal TARGET (what the arm was COMMANDED to), which is the only
// ground truth available for "how far is the arm actually being told to go".
// Recording it while the operator moves lets scripts/diagnose_hand_arm_transfer.py
// turn the joint angles back into TCP millimetres.
//
// SAFETY: this is a pure SUBSCRIBER. It sends only `status_request` (a
// re-hello), never joint_command / device_command, so it cannot move the arm and
// cannot disturb a visionflow camera-agent that already holds the TCP port.
// Multiple WS clients are supported by the backend by design.
//
// Node >= 22 has a global WebSocket, so there is nothing to install.
//
//   node scripts/ws_joint_record.mjs [duration_ms] [out.jsonl]
//
// Output: one JSON object per line, each server message with two extra fields:
//   t0  epoch ms at start   t  absolute epoch ms   rel  ms since start
import { appendFileSync, writeFileSync } from "node:fs";

const URL = process.env.ARMPILOT_WS || "ws://127.0.0.1:8090/ws/joint";
const durationMs = Number(process.argv[2] || 60000);
const outPath = process.argv[3] || "joint_stream.jsonl";
const t0 = Date.now();

let count = 0;
let seq = 0;
const ws = new WebSocket(URL);

ws.addEventListener("open", () => {
  console.error(`[ws] connected ${URL} (read-only subscriber)`);
  ws.send(JSON.stringify({ version: 1, type: "status_request" }));
});

ws.addEventListener("message", (ev) => {
  const line = typeof ev.data === "string" ? ev.data : String(ev.data);
  let msg;
  try { msg = JSON.parse(line); } catch { return; }
  if (seq === 0) writeFileSync(outPath, "");
  appendFileSync(outPath, JSON.stringify({ t0, t: Date.now(), rel: Date.now() - t0, ...msg }) + "\n");
  seq++;
  if (msg.type === "hello") {
    console.error(`[ws] hello: device=${msg.device} model=${msg.model?.id}`);
  } else if (msg.type === "device_status") {
    console.error(`[ws] device_status: ${JSON.stringify(msg)}`);
  } else if (msg.type === "joint_state") {
    count++;
    if (count % 20 === 0) console.error(`[ws] ${count} joint_state frames`);
  }
});

ws.addEventListener("error", (e) => console.error("[ws] error:", e.message || e));
ws.addEventListener("close", () => {
  console.error(`[ws] closed: ${count} joint_state frames -> ${outPath}`);
  process.exit(0);
});

setTimeout(() => ws.close(), durationMs);
