import { api } from '@/api/client'

export type PlayerState =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'disconnected'
  | 'reconnecting'

export type Transport = 'webrtc' | null

export interface PlayerStatus {
  state: PlayerState
  transport: Transport
  /** Short human-readable reason, shown in the UI so a failure is diagnosable
   *  without opening devtools on a phone. */
  detail: string
  /** True when the browser refused to autoplay and needs a tap. */
  needsTap: boolean
}

// WebRTC is the only transport now (HLS fallback was removed from the web
// client). It is the fragile one on phones: it needs UDP/ICE to reach the
// server and some mobile browsers refuse it on an insecure (http) origin. If
// it has not produced a picture within this window, we retry WebRTC rather
// than switching transports.
const WEBRTC_TIMEOUT_MS = 7000

export class StreamPlayer {
  private video: HTMLVideoElement
  private cameraId: string
  private pc: RTCPeerConnection | null = null
  private closed = false
  private retry = 0
  private timer: number | null = null
  private watchdog: number | null = null
  private transport: Transport = null
  private needsTap = false
  private detail = ''
  /** Generation token. Every connect/switch attempt captures the current value;
   *  after each async step it checks `myToken === this.token`. A newer attempt
   *  (a fresh click) bumps the token, which invalidates all in-flight work from
   *  the previous attempt so its stale callbacks can never touch the element or
   *  fire a switch mid-flight. This is what makes rapid re-clicks safe. */
  private token = 0
  /** True while a user-initiated switch is in progress. Debounces re-entry so a
   *  burst of clicks only triggers one switch (the latest wins). */
  private switching = false
  onStatus: ((s: PlayerStatus) => void) | null = null

  constructor(video: HTMLVideoElement, cameraId: string) {
    this.video = video
    this.cameraId = cameraId
  }

  start(): void {
    this.closed = false
    this.token++
    this.connectWebRTC(this.token)
  }

  /** Re-select / reconnect WebRTC (UI toggle). Frequent clicks used to cause an
   *  occasional reset: overlapping connect attempts left two RTCPeerConnections
   *  racing, and a stale watchdog could flip the element away. The fix is a two
   *  phase flow:
   *    1. revert to the OFF state (tear everything down, transport=null),
   *    2. bump the generation token so any in-flight work is invalidated, then
   *       connect only when the internal state is consistent with this attempt.
   *  A re-entrant click while switching is ignored; the in-flight switch already
   *  reflects the desired (WebRTC) mode. */
  useWebRTC(): void {
    if (this.switching) return
    this.switching = true
    try {
      // 1. revert current state to closed/off.
      this.cancelAll()
      this.transport = null
      // 2. bump token -> any pending async from a prior attempt is now stale and
      //    will abort at its next check, so internal data is consistent with this
      //    (WebRTC) mode before we update.
      this.token++
      const myToken = this.token
      this.retry = 0
      this.detail = 'resetting to WebRTC'
      this.emit({ state: 'connecting' })
      // 3. execute the update (connect) once state is consistent.
      void this.connectWebRTC(myToken).finally(() => {
        if (this.token === myToken) this.switching = false
      })
    } catch {
      this.switching = false
    }
  }

  /** Called on a user tap when autoplay was blocked. */
  resume(): void {
    this.video.muted = true
    void this.video.play().then(
      () => {
        this.needsTap = false
        this.emit()
      },
      () => {}
    )
  }

  stop(): void {
    this.closed = true
    this.token++ // invalidate any in-flight attempt
    this.clearTimers()
    this.teardownPeer()
    this.video.srcObject = null
  }

  // ---------------------------------------------------------------- internals

  private emit(extra?: Partial<PlayerStatus>): void {
    this.onStatus?.({
      state: this.currentState(),
      transport: this.transport,
      detail: this.detail,
      needsTap: this.needsTap,
      ...extra
    })
  }

  private currentState(): PlayerState {
    if (this.retry > 0) return 'reconnecting'
    return 'connecting'
  }

  private clearTimers(): void {
    if (this.timer !== null) {
      window.clearTimeout(this.timer)
      this.timer = null
    }
    if (this.watchdog !== null) {
      window.clearTimeout(this.watchdog)
      this.watchdog = null
    }
  }

  /** Tear down the active peer + timers, reverting to the off state. Does not
   *  flip `closed`, so a follow-up connect can proceed. */
  private cancelAll(): void {
    this.clearTimers()
    this.teardownPeer()
  }

  // ------------------------------------------------------------------- webrtc

  private async connectWebRTC(myToken: number): Promise<void> {
    if (this.closed || myToken !== this.token) return
    this.transport = 'webrtc'
    // Start from a clean <video>: drop any stale srcObject / src so the WebRTC
    // stream we assign on ontrack is the only thing driving it.
    if (this.video.src) {
      this.video.removeAttribute('src')
      this.video.load()
    }
    this.video.srcObject = null
    this.detail = this.retry === 0 ? 'negotiating WebRTC' : 'retrying WebRTC #' + this.retry
    this.emit({ state: this.retry === 0 ? 'connecting' : 'reconnecting' })

    // Give up on this attempt if no frame arrives in time, rather than spinning
    // forever behind a black rectangle - then retry (no transport switch).
    this.watchdog = window.setTimeout(() => {
      this.watchdog = null
      if (this.closed || myToken !== this.token || this.transport !== 'webrtc') return
      this.detail = 'WebRTC timed out - no media in ' + WEBRTC_TIMEOUT_MS / 1000 + 's, retrying'
      this.teardownPeer()
      this.retry++
      this.scheduleReconnect(myToken)
    }, WEBRTC_TIMEOUT_MS)

    try {
      const pc = new RTCPeerConnection({ iceServers: [] })
      this.pc = pc
      pc.addTransceiver('video', { direction: 'recvonly' })
      pc.addTransceiver('audio', { direction: 'recvonly' })

      pc.ontrack = (ev: RTCTrackEvent) => {
        if (myToken !== this.token || this.closed) return // stale attempt
        if (ev.track.kind === 'video') {
          this.video.srcObject = ev.streams[0]
          this.play()
        }
      }

      pc.onconnectionstatechange = () => {
        if (myToken !== this.token || this.closed) return // stale attempt
        const st = pc.connectionState
        if (st === 'connected') {
          this.retry = 0
          this.clearWatchdog()
          this.detail = 'WebRTC connected'
          this.emit({ state: 'connected' })
        } else if (st === 'failed') {
          this.detail = 'WebRTC ICE failed (UDP to server unreachable?)'
          this.onWebRTCFailure(myToken)
        } else if (st === 'disconnected') {
          this.detail = 'WebRTC disconnected'
          this.onWebRTCFailure(myToken)
        }
      }

      pc.oniceconnectionstatechange = () => {
        if (myToken !== this.token || this.closed) return // stale attempt
        if (pc.iceConnectionState === 'failed') {
          this.detail = 'ICE failed - no candidate pair (UDP blocked)'
          this.onWebRTCFailure(myToken)
        }
      }

      const offer = await pc.createOffer()
      if (myToken !== this.token || this.closed) return
      await pc.setLocalDescription(offer)
      if (myToken !== this.token || this.closed) return
      const { sdp } = await api.webrtcOffer(this.cameraId, offer.sdp || '')
      if (myToken !== this.token || this.closed) return
      await pc.setRemoteDescription({ type: 'answer', sdp })
    } catch (err) {
      if (myToken !== this.token || this.closed) return
      this.detail = 'WebRTC error: ' + describe(err)
      this.onWebRTCFailure(myToken)
    }
  }

  private onWebRTCFailure(myToken: number): void {
    if (this.closed || myToken !== this.token) return
    this.teardownPeer()
    this.retry++
    // No HLS fallback: keep retrying WebRTC (bounded delay) until it connects
    // or the player is stopped.
    this.scheduleReconnect(myToken)
  }

  private scheduleReconnect(myToken: number): void {
    if (this.closed || this.timer !== null) return
    if (myToken !== this.token) return
    const delay = Math.min(1000 * this.retry, 4000)
    this.emit({ state: 'reconnecting' })
    this.timer = window.setTimeout(() => {
      this.timer = null
      if (this.closed || myToken !== this.token || this.transport !== 'webrtc') return
      this.connectWebRTC(myToken)
    }, delay)
  }

  private clearWatchdog(): void {
    if (this.watchdog !== null) {
      window.clearTimeout(this.watchdog)
      this.watchdog = null
    }
  }

  private teardownPeer(): void {
    if (this.pc) {
      try {
        this.pc.ontrack = null
        this.pc.onconnectionstatechange = null
        this.pc.oniceconnectionstatechange = null
        this.pc.close()
      } catch {
        /* ignore */
      }
      this.pc = null
    }
    // Release the WebRTC MediaStream from the <video> element so a later
    // re-attach starts from a clean element.
    if (this.video.srcObject) {
      this.video.srcObject = null
    }
  }

  // -------------------------------------------------------------------- media

  private play(): void {
    // Mobile browsers can refuse play() even on a muted inline video (low power
    // mode, data saver). Surface it as "tap to play" instead of a black box.
    const p = this.video.play()
    if (p && typeof p.catch === 'function') {
      p.then(
        () => {
          this.needsTap = false
        },
        (err: unknown) => {
          this.needsTap = true
          this.detail = 'autoplay blocked - tap to play (' + describe(err) + ')'
          this.emit()
        }
      )
    }
  }
}

function describe(err: unknown): string {
  if (err instanceof Error) return err.message
  return String(err)
}
