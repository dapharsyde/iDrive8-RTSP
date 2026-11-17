'use strict';

// iDrive 8 RTSP viewer. The native RTSP/RTP client feeds H.264 access units
// into Intel Media SDK and writes the renderer's MGVF shared-memory format.
const fs = require('fs');
const path = require('path');
const net = require('net');
const mgu = require('./kit/mgu');
const { keyboardSheet } = require('./kit/keyboard');

const VIDEO_ID = 1;
const VIDEO_PATH = process.env.MGU_RTSP_VIDEO || '/dev/shm/mgu-video-rtsp';
const CONTROL_SOCKET = process.env.MGU_RTSP_SOCKET || '/run/rtsp-viewer/ctl.sock';
const SAVED_URL = path.join(process.cwd(), 'rtsp-camera-url.json');
const BG = '#080b10';

process.on('uncaughtException', (e) => { try { console.error('rtsp-viewer:', e.stack || e); } catch (_) {} });

mgu.run({
  async start(ctx) {
    const { ui, log } = ctx;
    let url = '';
    try { url = JSON.parse(fs.readFileSync(SAVED_URL, 'utf8')).url || ''; } catch (_) {}
    let state = 'Enter an RTSP camera URL';
    let connection = null;
    let videoRegistered = false;

    function closeVideo() {
      if (connection) { try { connection.destroy(); } catch (_) {} connection = null; }
      videoRegistered = false;
      const gpu = ctx.display && ctx.display.gpu;
      if (gpu && gpu.dropVideo) gpu.dropVideo(VIDEO_ID);
      try { fs.unlinkSync(VIDEO_PATH); } catch (_) {}
    }

    function startCamera(input) {
      if (!/^rtsp:\/\//i.test(input)) { state = 'Use an rtsp:// camera URL'; return; }
      closeVideo(); url = input;
      try { fs.writeFileSync(SAVED_URL, JSON.stringify({ url }), { mode: 0o600 }); fs.chmodSync(SAVED_URL, 0o600); } catch (_) {}
      state = 'Connecting to camera…';
      let events = '';
      let receiverMessage = '';
      const socket = net.createConnection({ path: CONTROL_SOCKET });
      connection = socket;
      socket.on('connect', () => { if (connection === socket) socket.write(input + '\n'); });
      socket.on('data', (b) => {
        if (connection !== socket) return;
        events += b.toString();
        let nl;
        while ((nl = events.indexOf('\n')) >= 0) {
          const line = events.slice(0, nl); events = events.slice(nl + 1);
          try { const ev = JSON.parse(line); if (ev.msg) { receiverMessage = ev.msg; state = ev.msg; } } catch (_) {}
        }
      });
      socket.on('error', (e) => {
        if (connection !== socket) return;
        state = e.code === 'ENOENT' || e.code === 'ECONNREFUSED' ? 'RTSP Receiver service is not available; install or start the tool' : 'Receiver connection error: ' + e.message;
        closeVideo();
      });
      socket.on('close', () => {
        if (connection === socket) {
          if (receiverMessage === 'Connected; waiting for H.264 frames') state = 'Camera stream ended';
          else if (!receiverMessage) state = 'Receiver closed before reporting an RTSP result';
          else state = receiverMessage;
          closeVideo();
        }
      });
    }

    ui.onClick('BTN_CONNECT', () => {
      ctx.sheet(keyboardSheet({ title: 'RTSP camera URL', hint: 'rtsp://camera/stream', text: url,
        action: 'Connect', onDone: (v) => startCamera(v) }));
    });
    ui.onClick('BTN_STOP', () => { closeVideo(); state = 'Stopped'; });
    ui.onVisible((name, visible) => { if (name === 'VIEW' && !visible && connection) { closeVideo(); state = 'Stopped'; } });

    function render() {
      const cv = ctx.canvas(2080, 520).clear(BG);
      const gpu = ctx.display && ctx.display.gpu;
      if (connection && gpu && gpu.rich && gpu.video) {
        try { if (fs.statSync(VIDEO_PATH).isFile()) { gpu.video(VIDEO_ID, VIDEO_PATH); videoRegistered = true; } } catch (_) {}
      }
      const live = !!(videoRegistered && gpu && gpu.rich);
      if (live) cv.video(VIDEO_ID, 0, 0, 2080, 520, 0);
      else {
        cv.roundRect(100, 58, 1880, 400, 28, '#111923');
        cv.text('RTSP CAMERA', 160, 142, 28, '#64d8ff', { literal: true });
        cv.text(state, 160, 240, 42, '#ffffff', { literal: true });
        cv.text(url ? 'Saved camera: ' + url.replace(/:\/\/[^/@]+@/, '://••••@') : 'Choose Camera URL to connect',
          160, 316, 25, '#aab7c6', { literal: true });
        cv.text('Direct H.264 decode · RTSP over TCP · Intel Media SDK', 160, 370, 23, '#8191a3', { literal: true });
      }
      // Keep controls over the picture; iDrive 8 has no HMI button bar on the host page.
      cv.roundRect(30, 24, 290, 58, 18, '#101923dd');
      cv.text('Camera URL', 175, 62, 25, '#ffffff', { align: 'center', literal: true });
      cv.target(30, 20, 290, 66, () => ctx.sheet(keyboardSheet({ title: 'RTSP camera URL', hint: 'rtsp://camera/stream', text: url,
        action: 'Connect', onDone: (v) => startCamera(v) })), 'camera-url', 18);
      cv.roundRect(1760, 24, 290, 58, 18, '#101923dd');
      cv.text('Stop', 1905, 62, 25, '#ffffff', { align: 'center', literal: true });
      cv.target(1760, 20, 290, 66, () => { closeVideo(); state = 'Stopped'; }, 'stop-camera', 18);
      return ui.image('VIEW_GFX', cv);
    }
    ctx.loop('VIEW', 10, render);
    log.info('RTSP viewer ready; control socket=' + CONTROL_SOCKET);
  }
});
