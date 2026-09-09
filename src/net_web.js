// WebRTC transport shim, linked with --js-library.
//
// Trystero is used for SIGNALLING ONLY - it finds the peer over public Nostr
// relays and completes the WebRTC handshake. Game data does NOT go through its
// actions: those are reliable and ordered, which would add head-of-line blocking
// exactly where rollback wants the opposite. Instead we attach our own channel
// to the RTCPeerConnection that getPeers() hands back (documented public API)
// and run it unordered with zero retransmits. Losing a packet is free here -
// every packet carries the last INPUT_HIST frames of input - whereas waiting for
// a retransmit would stall inputs that have already been superseded.
//
// The channel is NEGOTIATED with a fixed id: both peers create it themselves on
// onPeerJoin, so there is no renegotiation and no onnegotiationneeded race with
// Trystero's own channel.
mergeInto(LibraryManager.library, {
  $NETJS: {
    q: [],          // incoming packets, Array<Uint8Array>
    ch: null,       // our unreliable RTCDataChannel
    room: null,
    state: 0,       // 0 idle, 1 connecting, 2 open, 3 failed
    seat: -1,
    err: '',
    started: false,
  },

  ob_net_open__deps: ['$NETJS', '$UTF8ToString'],
  ob_net_open: function (roomPtr) {
    if (NETJS.started) return;
    NETJS.started = true;
    NETJS.state = 1;
    NETJS.err = '';

    var code = UTF8ToString(roomPtr) || 'openball';

    // Resolve against the PAGE, not against this script: the site is a GitHub
    // PROJECT page served under /SportheadOpenBall/, so an absolute "/..." path
    // would 404.
    var url = new URL('trystero-nostr.min.js', document.baseURI).href;

    import(url).then(function (T) {
      // relayConfig.redundancy, not a top-level key: connecting to several
      // relays means one dead public relay is invisible rather than fatal.
      var room = T.joinRoom(
        { appId: 'sporthead-openball-v1', relayConfig: { redundancy: 4 } },
        'ob-' + code,
        {
          // Distinguishes "peers exchanged SDP but WebRTC could not connect"
          // from "no peer found". Without TURN that first case is the symmetric
          // and carrier-grade NAT failure, and it is worth naming precisely
          // rather than leaving someone staring at a spinner.
          onJoinError: function (d) {
            NETJS.state = 3;
            NETJS.err = (d && d.peerId)
              ? 'found opponent but could not connect (likely strict NAT)'
              : 'could not join room: ' + ((d && d.error) || 'unknown');
          }
        }
      );
      NETJS.room = room;

      // ASSIGNED, not called. In Trystero 0.25 these are properties, and calling
      // them as methods throws "onPeerJoin is not a function".
      room.onPeerJoin = function (peerId) {
        if (NETJS.ch) return;                       // v1 is strictly 1v1

        // Seats from a lexicographic id comparison. Both sides run the same
        // comparison, so there is no host/join role and two hosts are impossible.
        NETJS.seat = (T.selfId < peerId) ? 0 : 1;

        var pc = room.getPeers()[peerId];
        if (!pc) { NETJS.state = 3; NETJS.err = 'peer connection unavailable'; return; }

        var dc = pc.createDataChannel('ob', {
          negotiated: true, id: 42, ordered: false, maxRetransmits: 0
        });
        dc.binaryType = 'arraybuffer';
        dc.onopen = function () { NETJS.ch = dc; NETJS.state = 2; };
        dc.onclose = function () {
          NETJS.state = 3;
          if (!NETJS.err) NETJS.err = 'peer disconnected';
        };
        dc.onerror = function () { NETJS.state = 3; NETJS.err = 'data channel error'; };
        dc.onmessage = function (e) {
          // Bounded: if C stops draining we drop rather than grow without limit.
          if (NETJS.q.length < 256) NETJS.q.push(new Uint8Array(e.data));
        };
      };

      room.onPeerLeave = function () {
        NETJS.state = 3;
        NETJS.err = 'opponent left';
        NETJS.ch = null;
      };
    }).catch(function (e) {
      NETJS.state = 3;
      NETJS.err = 'could not load signalling bundle: ' + (e && e.message ? e.message : e);
    });
  },

  ob_net_send__deps: ['$NETJS'],
  ob_net_send: function (ptr, len) {
    if (NETJS.state !== 2 || !NETJS.ch) return;
    // COPY, never subarray(): send() may retain the buffer past this call, and
    // any heap growth would detach and replace the ArrayBuffer underneath a
    // retained view. (Growth is disabled here, but a view into the heap handed
    // to a Web API is a trap worth not setting.)
    var copy = HEAPU8.slice(ptr, ptr + len);
    try { NETJS.ch.send(copy); } catch (e) { /* buffer full: drop, it self-heals */ }
  },

  ob_net_poll__deps: ['$NETJS'],
  ob_net_poll: function (ptr, cap) {
    if (NETJS.q.length === 0) return -1;
    var m = NETJS.q.shift();
    if (m.length > cap) return -1;
    HEAPU8.set(m, ptr);      // read HEAPU8 FRESH every call: it is REBOUND on grow
    return m.length;
  },

  ob_net_state__deps: ['$NETJS'], ob_net_state: function () { return NETJS.state; },
  ob_net_seat__deps:  ['$NETJS'], ob_net_seat:  function () { return NETJS.seat;  },

  ob_net_error__deps: ['$NETJS', '$stringToUTF8'],
  ob_net_error: function (ptr, cap) { stringToUTF8(NETJS.err || '', ptr, cap); },

  ob_net_close__deps: ['$NETJS'],
  ob_net_close: function () {
    try { if (NETJS.ch) NETJS.ch.close(); } catch (e) {}
    try { if (NETJS.room) NETJS.room.leave(); } catch (e) {}
    NETJS.ch = null; NETJS.room = null; NETJS.state = 0; NETJS.seat = -1;
    NETJS.started = false;
  },

  ob_net_hidden: function () {
    try { return document.hidden ? 1 : 0; } catch (e) { return 0; }
  },

  // Pull model for the invite link: C asks for ?room=CODE at startup, so nothing
  // needs ccall/EXPORTED_FUNCTIONS in the other direction.
  ob_net_url_room__deps: ['$stringToUTF8'],
  ob_net_url_room: function (ptr, cap) {
    var r = '';
    try {
      r = (new URLSearchParams(location.search).get('room') || '').toUpperCase();
      r = r.replace(/[^A-Z0-9]/g, '').slice(0, 8);
    } catch (e) { r = ''; }
    stringToUTF8(r, ptr, cap);
  },
});
