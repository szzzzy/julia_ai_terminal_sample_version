# COM8 interaction / WSS diagnosis

Read COM8 at 115200 with DTR/RTS false; no reset, flash or control commands sent.
Raw log: `build-host-state-recovery/com8-interaction-diagnosis.log`.
Times below are the device's logged milliseconds since boot, not wall time.

## Observed

- At 542 s, WSS audio payload write (656 bytes) made zero progress for 3657 ms,
  returned transient WANT_WRITE/EAGAIN, and ended as `tx_stall`. 188 MIC frames
  were discarded. State was S3, not S4. MQTT remained online (`online_links=0x01`).
- At 619257 ms, server wake entered S4. Wake reply played successfully. A dialog
  segment then reached 750 frames / limit=1, entered S2.2, played a response and
  returned to S1. This proves at least one successful wake/dialog cycle during capture.
- At 681507 ms, after prolonged write stalls, the owner accepted an
  `audio_overflow` session-end request. 256 MIC ring frames were discarded.
  Final reason was reported as `application_error`, so use the earlier explicit
  overflow request when attributing this incident. RSSI was -33 dBm.
- At 734567 ms, another server wake entered S4. At 736067 ms local dialog
  capture started; at 736577 ms the incoming wake playback was rejected:
  `SPKS rejected in state=S4_INTERACTION/NONE: no playback role`.
  Subsequent downlink PCM was dropped because playback had not started.
  The local LC_START callback clears `s_wake_reply_expected`, consistent with
  this ordering. Whether this early local onset was speech or noise is not known.
- At 741097 ms, a 656-byte WSS payload write timed out after 3547 ms. The session
  ended `tx_stall`, discarded 184 MIC frames, and at 741207 ms transitioned from
  S4 to S7.1. This is approximately 6.64 seconds after entry into S4. RSSI was
  -35 dBm and MQTT stayed online. This directly reproduces the user's perceived
  "wake then disconnected" symptom without Wi-Fi disconnecting.
- S3 repeatedly generated 400-frame wake candidate segments ending by limit;
  dialog segments also reached their 750-frame cap. Background estimates were
  approximately -72 to -74 dBFS. These are observations, not proof of noise identity.

## Interpretation and next investigation

The immediate disconnection triggers are WSS write starvation / microphone-ring
overflow, not an intentional Wi-Fi shutdown on S4 entry. The same write stalls
also occur in S3. The source of transport backpressure remains unproven: correlate
server receive/event-loop timing and TCP behavior with the firmware sender before
changing timeouts or buffer sizes. A larger queue alone only postpones failure.

The rejected wake reply is a distinct ordering issue involving early local onset.
Preserve intended user interruption behavior when investigating it. Recording-tail
behavior and frequent candidate uploads add sustained traffic, but do not by
themselves establish why the transport blocks for seconds.

No firmware changes were made during this observation. Current workspace FFT
changes belong to the ongoing separate work and were not reverted or overwritten.

## Server-log correlation supplied by user

The following findings refer to the captured earlier firmware session, not the
subsequently flashed image. COM8 monitoring has ended and the port was released.

### 1. Capture lifecycle mismatch leads to explicit protocol closure

- Server wall time 23:49:36.930: session `52e0bf37a4b3384b2165b5810668133f`
  accepts S4 revision 28 and sends its ACK.
- 23:49:37.757: the older wake segment 70 completes after this new state snapshot.
- 23:49:37.979: PCM2 segment 71, index 0 arrives with expected segment/index both
  None, followed by a server-sent 1002 `capture protocol error` close.
- 23:53:20.657: the same first-frame/no-active-segment mismatch repeats for
  segment 96 during S4 wake response, again followed by server-sent 1002.

This establishes a capture segment lifecycle/dispatch mismatch as one concrete
disconnection path. It does not establish whether the start was omitted on the
wire, rejected, or cleared after acceptance. The repository firmware queues all
capture start/audio/end/abort records in one FIFO, but `device_state` is sent
directly before the FIFO drain in `voice_service_poll`. The repository cloud
patch validates capture_start mode against the latest state snapshot. Therefore
a newer state can overtake older queued capture records, and validation against
current state is a specific race to reproduce. Also inspect whether retiring an
engine clears a receiver still owned by the same WSS connection. The deployed
server differs from the old patch (its log says `discard_segment; WSS retained`
yet an adapter still closes), so inspect deployed code before choosing a fix.

### 2. Rejected wake response is correlated on both ends

- Server 23:51:48.645 receives S4 ready, then sends SPKS at 23:51:50.498.
- Device boot 736067 ms starts local dialog segment 85; its callback clears
  `s_wake_reply_expected` before SPKS at 736577 ms. Role selection then fails.
- Server 23:51:50.550 receives `ERROR playback_state`.

This explains one missing audible wake response independently of Wi-Fi and the
later disconnection. Determine whether the early onset was intended user speech
or noise; preserve intended barge-in semantics when fixing pending wake playback.

### 3. Transport backpressure is also present

The server's 23:53:20 exception shows its legacy websockets transfer task waiting
on `_put_message_waiter`, the bounded incoming-message queue. This is a concrete
receive-backpressure indication at cancellation, compatible with the firmware's
WANT_WRITE/EAGAIN stalls and microphone-ring overflow. It is not proof that every
earlier stall had the same cause or which application callback delayed recv.
Inspect WSS message handling/locks, engine retirement, inference and send waits;
do not remove sequence checks or merely enlarge buffers as a substitute.

### 4. Noise verification currently fails open

Repeated server lines explicitly report `Remote speech detector failed
(NotImplementedError); fail-open policy; firmware owns capture`. Thus the hoped-for
remote speech verification cannot be assumed operational in these sessions.
Repeated 8 s wake / 15 s dialog limit segments explain delayed endpointing and
ongoing traffic. They do not alone prove a transport or protocol implementation
failure. Likewise `vs.api API WebSocket close deadline` is not automatically a
device WSS close: the same device session continues across many such messages.

Priority: capture receiver lifecycle/order and pending wake playback, then isolate
the application receive bottleneck; tune noise gates after these correctness
failures are understood. S4 ACKs were received in the cited failure cases.
