'use strict';

// iDrive 8 RTSP viewer. The native RTSP/RTP client feeds H.264 access units
// into Intel Media SDK and writes the renderer's MGVF shared-memory format.
const fs = require('fs');
const path = require('path');
const net = require('net');
const mgu = require('./kit/mgu');
const { keyboardSheet } = require('./kit/keyboard');
const { cardSheet } = require('./kit/sheet');

const VIDEO_ID = 1;
const VIDEO_PATH = process.env.MGU_RTSP_VIDEO || '/dev/shm/mgu-video-rtsp';
const CONTROL_SOCKET = process.env.MGU_RTSP_SOCKET || '/run/rtsp-viewer/ctl.sock';
const SAVED_URL = path.join(process.cwd(), 'rtsp-camera-url.json');
const BG = '#080b10';
const VIEW_W = 1560;
const RAIL_X = 1590;
const RAIL_W = 450;

process.on('uncaughtException', (e) => { try { console.error('rtsp-viewer:', e.stack || e); } catch (_) {} });

mgu.run({
  async start(ctx) {
    const { ui, log } = ctx;
    let url = '';
    let cameras = [];
    let selectedCamera = 0;
    try {
      const saved = JSON.parse(fs.readFileSync(SAVED_URL, 'utf8'));
      if (Array.isArray(saved.cameras)) cameras = saved.cameras.filter((c) => c && typeof c.url === 'string' && /^rtsp:\/\//i.test(c.url));
      if (!cameras.length && saved.url) {
        // Migrate the URL saved by versions before named cameras were added.
        cameras = [{ name: 'Cam1', url: saved.url }];
        selectedCamera = 0;
        fs.writeFileSync(SAVED_URL, JSON.stringify({ url: saved.url, cameras, selected: 0 }), { mode: 0o600 });
        try { fs.chmodSync(SAVED_URL, 0o600); } catch (_) {}
      } else if (cameras.length) {
        selectedCamera = Math.max(0, Math.min(cameras.length - 1, Number(saved.selected) || 0));
      }
      if (cameras[selectedCamera]) url = cameras[selectedCamera].url;
    } catch (_) {}
    let state = 'Enter an RTSP camera URL';
    let connection = null;
    let videoRegistered = false;
    let pendingUrl = null;
    let stopRequested = false;
    let transportError = '';

    function releaseVideo() {
      videoRegistered = false;
      const gpu = ctx.display && ctx.display.gpu;
      if (gpu && gpu.dropVideo) gpu.dropVideo(VIDEO_ID);
      try { fs.unlinkSync(VIDEO_PATH); } catch (_) {}
    }

    function connectCamera(input) {
      releaseVideo();
      state = 'Connecting to camera…';
      let events = '';
      let receiverMessage = '';
      transportError = '';
      const socket = net.createConnection({ path: CONTROL_SOCKET });
      socket.__rtspStopSent = false;
      connection = socket;
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
        transportError = e.code === 'ENOENT' || e.code === 'ECONNREFUSED' ? 'RTSP Receiver service is not available; install or start the tool' : 'Receiver connection error: ' + e.message;
        socket.destroy();
      });
      socket.on('close', () => {
        if (connection !== socket) return;
        connection = null;
        releaseVideo();
        const nextUrl = pendingUrl;
        pendingUrl = null;
        if (nextUrl) { stopRequested = false; connectCamera(nextUrl); return; }
        state = stopRequested ? 'Stopped' : transportError || (receiverMessage === 'Connected; waiting for H.264 frames'
          ? 'Camera stream ended' : receiverMessage || 'Receiver closed before reporting an RTSP result');
        stopRequested = false;
      });
      socket.on('connect', () => {
        if (connection !== socket) return;
        socket.write(input + '\n');
        if (socket.__rtspStopSent) socket.write('STOP\n');
      });
    }

    function stopCamera(nextUrl = null) {
      if (connection) {
        pendingUrl = nextUrl;
        stopRequested = true;
        state = nextUrl ? 'Restarting camera…' : 'Stopping…';
        // Ask the receiver to send RTSP TEARDOWN and dispose of its decoder
        // before it closes the control socket; reconnect only after that ack.
        if (!connection.__rtspStopSent) {
          connection.__rtspStopSent = true;
          if (!connection.connecting) connection.write('STOP\n');
        }
        return;
      }
      releaseVideo();
      if (nextUrl) { connectCamera(nextUrl); return; }
      state = 'Stopped';
    }

    function startCamera(input) {
      if (!/^rtsp:\/\//i.test(input)) { state = 'Use an rtsp:// camera URL'; return; }
      url = input;
      if (connection) stopCamera(input);
      else connectCamera(input);
    }

    function saveCameras() {
      try {
        fs.writeFileSync(SAVED_URL, JSON.stringify({ url, cameras, selected: selectedCamera }), { mode: 0o600 });
        fs.chmodSync(SAVED_URL, 0o600);
      } catch (e) { log.warn('camera list save: ' + e); }
    }

    function chooseCamera(index) {
      if (!cameras[index]) return 'close';
      selectedCamera = index;
      url = cameras[index].url;
      saveCameras();
      startCamera(url);
      return 'close';
    }

    function cameraPicker() {
      return {
        title: 'Choose Camera', focus: 0, page: 0, controller: false,
        draw(r, t) {
          r.clear(t.bg);
          const W = r.w || 2080, bottom = 512 + (r.extra || 0), targets = [];
          r.text('Choose Camera', 44, 90, 34, t.text, { font: 'bold' });
          const perPage = Math.max(1, Math.floor((bottom - 160) / 76));
          const pages = Math.max(1, Math.ceil(cameras.length / perPage));
          this.page = Math.max(0, Math.min(this.page, pages - 1));
          const begin = this.page * perPage;
          cameras.slice(begin, begin + perPage).forEach((cam, row) => {
            const i = begin + row, y = 122 + row * 76;
            const active = this.controller && this.focus === row;
            r.roundRect(44, y, W - 88, 62, 14, active ? (t.sel || t.accent) : '#18232d');
            r.text(String(cam.name || ('Cam' + (i + 1))), 72, y + 39, 27, active ? '#ffffff' : t.text, { font: 'bold' });
            if (i === selectedCamera) r.text('SELECTED', W - 190, y + 38, 17, active ? '#ffffff' : (t.sel || t.accent), { align: 'right', font: 'bold' });
            targets.push({ x: 44, y, w: W - 88, h: 62, act: () => chooseCamera(i) });
          });
          const navY = bottom - 62;
          if (pages > 1) {
            r.text((this.page + 1) + ' / ' + pages, W / 2, navY + 32, 20, t.dim, { align: 'center' });
            r.roundRect(44, navY, 170, 48, 24, '#263744'); r.text('Previous', 129, navY + 31, 19, '#fff', { align: 'center' });
            r.roundRect(W - 214, navY, 170, 48, 24, '#263744'); r.text('Next', W - 129, navY + 31, 19, '#fff', { align: 'center' });
            targets.push({ x: 44, y: navY, w: 170, h: 48, act: () => { this.page = Math.max(0, this.page - 1); this.focus = 0; return null; } });
            targets.push({ x: W - 214, y: navY, w: 170, h: 48, act: () => { this.page = Math.min(pages - 1, this.page + 1); this.focus = 0; return null; } });
          }
          return targets;
        },
        move(d) { this.controller = true; const perPage = Math.max(1, Math.floor((512 - 160) / 76)); const n = Math.min(perPage, cameras.length - this.page * perPage); this.focus = Math.max(0, Math.min(n - 1, this.focus + d)); },
        press() { this.controller = true; return chooseCamera(this.page * Math.max(1, Math.floor((512 - 160) / 76)) + this.focus); },
        back() { return 'close'; },
        top() { return false; },
      };
    }

    function editCamera() {
      if (!cameras[selectedCamera]) { state = 'Add a camera before editing'; return; }
      cameraForm(selectedCamera);
    }

    function deleteCamera() {
      const camera = cameras[selectedCamera];
      if (!camera) { state = 'Add a camera before deleting'; return; }
      const removedIndex = selectedCamera;
      ctx.sheet(cardSheet({
        title: 'Delete camera',
        heading: 'Delete ' + camera.name + '?',
        subtitle: 'Remove this camera from the saved list?',
        actions: [
          { label: 'Cancel', run: () => 'close' },
          { label: 'OK', primary: true, run: () => {
            const wasActive = !!connection && url === camera.url;
            cameras.splice(removedIndex, 1);
            if (selectedCamera >= cameras.length) selectedCamera = Math.max(0, cameras.length - 1);
            url = cameras[selectedCamera] ? cameras[selectedCamera].url : '';
            saveCameras();
            if (wasActive) stopCamera();
            if (!cameras.length) state = 'Add a camera to connect';
            return 'close';
          } },
        ],
      }));
    }

    function addCamera() { cameraForm(-1); }

    function cameraForm(editIndex) {
      const editing = editIndex >= 0 && !!cameras[editIndex];
      const original = editing ? cameras[editIndex] : null;
      let name = '';
      ctx.sheet(keyboardSheet({ title: editing ? 'Edit camera name' : 'Camera name', hint: 'For example, Driveway', text: original ? original.name : '', action: 'Next',
        onDone: (v) => { name = v; setTimeout(() => ctx.sheet(keyboardSheet({ title: editing ? 'Edit camera URL' : 'Camera URL', hint: 'rtsp://camera/stream', text: original ? original.url : 'rtsp://',
          action: 'Save', onDone: (cameraUrl) => {
            if (!/^rtsp:\/\//i.test(cameraUrl)) { state = 'Use an rtsp:// camera URL'; return; }
            const cleanName = name.trim() || (editing ? original.name : ('Cam' + (cameras.length + 1)));
            const entry = { name: cleanName, url: cameraUrl };
            if (editing) cameras[editIndex] = entry;
            else { cameras.push(entry); selectedCamera = cameras.length - 1; }
            selectedCamera = editing ? editIndex : selectedCamera;
            const urlChanged = !editing || original.url !== cameraUrl;
            url = cameraUrl;
            saveCameras();
            if (urlChanged) startCamera(cameraUrl);
          } })), 40); }
      }));
    }

    ui.onVisible((name, visible) => { if (name === 'VIEW' && !visible && connection) stopCamera(); });

    function render() {
      const probe = ctx.canvas(2080, 520);
      const height = 520 + probe.extra;
      const cv = ctx.canvas(2080, height).clear(BG);
      cv.extra; // Mark the frame as reaching into the fill layout's extra screen height.
      const gpu = ctx.display && ctx.display.gpu;
      if (connection && gpu && gpu.rich && gpu.video) {
        try { if (fs.statSync(VIDEO_PATH).isFile()) { gpu.video(VIDEO_ID, VIDEO_PATH); videoRegistered = true; } } catch (_) {}
      }
      const live = !!(videoRegistered && gpu && gpu.rich);
      cv.fillRect(VIEW_W, 0, 2080 - VIEW_W, height, '#111923');
      if (live) cv.video(VIDEO_ID, 0, 0, VIEW_W, height, 0);
      else {
        const mid = height / 2;
        cv.roundRect(90, mid - 190, VIEW_W - 180, 380, 28, '#111923');
        cv.text('RTSP CAMERA', 140, mid - 94, 28, '#64d8ff', { literal: true });
        cv.text(state, 140, mid - 8, 42, '#ffffff', { literal: true });
        cv.text(url ? 'Selected: ' + (cameras[selectedCamera] && cameras[selectedCamera].name || '') : 'Add a camera, then choose it from the list',
          140, mid + 64, 25, '#aab7c6', { literal: true });
        cv.text('Direct H.264 decode · RTSP over TCP · Intel Media SDK', 140, mid + 118, 23, '#8191a3', { literal: true });
      }
      // Dedicated right rail; the video stays fitted within the larger left region.
      const bx = RAIL_X, bw = RAIL_W;
      cv.roundRect(bx + 16, 24, bw - 32, 68, 16, '#087fa6');
      cv.circle(bx + 54, 58, 16, '#075e7d');
      cv.line(bx + 54, 49, bx + 54, 67, 3, '#d8f6ff');
      cv.line(bx + 45, 58, bx + 63, 58, 3, '#d8f6ff');
      cv.text('Add Camera', bx + bw / 2, 68, 26, '#ffffff', { align: 'center', font: 'bold', literal: true });
      cv.target(bx + 16, 24, bw - 32, 68, addCamera, 'add-camera', 18);
      cv.text('SAVED CAMERAS', bx + 20, 128, 17, '#90a3b2', { font: 'bold', literal: true });
      const selectorX = bx + 16, selectorY = 142, selectorW = bw - 32 - 110, iconX = selectorX + selectorW + 10;
      cv.roundRect(selectorX, selectorY, selectorW, 76, 16, '#182632');
      const selectedName = cameras[selectedCamera] ? cameras[selectedCamera].name : 'Choose camera';
      cv.text(selectedName.length > 17 ? selectedName.slice(0, 15) + '…' : selectedName, selectorX + 24, 189, 24, '#edf5f9', { font: 'bold', literal: true });
      cv.text('▾', selectorX + selectorW - 22, 189, 26, '#b7c8d3', { align: 'center', literal: true });
      cv.target(selectorX, selectorY, selectorW, 76, () => ctx.sheet(cameraPicker()), 'camera-picker', 18);
      // Icon buttons follow the supplied notepad and trash artwork, recoloured white for contrast.
      const iconSize = 45, iconY = selectorY + (76 - iconSize) / 2, trashX = iconX + iconSize + 10;
      cv.roundRect(iconX, iconY, iconSize, iconSize, 12, '#263744');
      cv.strokeRoundRect(iconX + 7, iconY + 6, 25, 30, 2.5, 2.5, '#ffffff');
      cv.line(iconX + 17, iconY + 31, iconX + 36, iconY + 12, 5, '#ffffff');
      cv.line(iconX + 15, iconY + 35, iconX + 19, iconY + 31, 3, '#ffffff');
      cv.target(iconX, iconY, iconSize, iconSize, editCamera, 'edit-camera', 18);
      cv.roundRect(trashX, iconY, iconSize, iconSize, 12, '#263744');
      cv.strokeRoundRect(trashX + 10, iconY + 14, 24, 22, 2.5, 2.5, '#ffffff');
      cv.roundRect(trashX + 7, iconY + 10, 31, 4, 2, '#ffffff');
      cv.line(trashX + 16, iconY + 7, trashX + 29, iconY + 7, 2.5, '#ffffff');
      cv.line(trashX + 17, iconY + 19, trashX + 17, iconY + 33, 2.5, '#ffffff');
      cv.line(trashX + 22, iconY + 19, trashX + 22, iconY + 33, 2.5, '#ffffff');
      cv.line(trashX + 27, iconY + 19, trashX + 27, iconY + 33, 2.5, '#ffffff');
      cv.target(trashX, iconY, iconSize, iconSize, deleteCamera, 'delete-camera', 18);
      cv.roundRect(bx + 16, 300, bw - 32, 62, 16, '#292027');
      cv.roundRect(bx + 40, 323, 14, 14, 2, '#ff8b8b');
      cv.text('Stop stream', bx + bw / 2, 340, 24, '#ffdede', { align: 'center', font: 'bold', literal: true });
      cv.target(bx + 16, 300, bw - 32, 62, () => stopCamera(), 'stop-camera', 18);
      cv.text('STATUS', bx + 20, 392, 17, '#90a3b2', { font: 'bold', literal: true });
      cv.text(state, bx + 20, 426, 20, '#e4ebf0', { literal: true });
      return ui.image('VIEW_GFX', cv);
    }
    ctx.loop('VIEW', 10, render);
    log.info('RTSP viewer ready; control socket=' + CONTROL_SOCKET);
  }
});
