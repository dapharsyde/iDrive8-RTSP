# iDrive 8 RTSP Camera Viewer

Live H.264 RTSP camera viewer for BMW iDrive 8, with **no FFmpeg dependency**. It uses the display technique found in
RideTube and reuses RideTube's Intel Media SDK decoder:

1. The native receiver connects to RTSP over TCP, reads the H.264 RTP stream and rebuilds Annex-B access units.
2. The decoder feeds those access units to the MGU's Intel Media SDK hardware decoder and fits them inside a
   1280×320 frame, preserving the camera's aspect ratio with black side bars when needed.
3. NV12 frames are written as I420 to `/dev/shm/mgu-video-rtsp` in MGU Video Frame (MGVF) format.
4. The app registers the shared frame file with `mgu-render` and draws renderer video layer 1.

## UI preview

This image is AI-rendered, not captured from an actual head unit, but it closely matches the app's UI.

![AI-rendered RTSP Camera UI preview](docs/images/rtsp-camera-ui-ai-render.jpg)

The display method was found in RideTube's `apps/jap_ridetube/main.js` (`gpu.video`, `gpu.dropVideo`, canvas `video`,
operation 19). The decoder and MGVF writer were adapted from `native/ytdec`.

## Requirements

- BMW iDrive 8 MGU, with MGU Store/deploy, `mgu-render` and `mgu-uplink` installed.
- An active uplink source configured in MGU Store settings (Wi-Fi, USB network/LTE, or phone tethering).
- Install both the RTSP Camera Receiver tool and RTSP Camera app.
- For private cameras, install Tailscale tool 1.104.9 from [my IterioDev/car-apps fork](https://github.com/dapharsyde/car-apps). Its userspace subnet-route support and loopback SOCKS5 proxy let the receiver reach cameras advertised by a tailnet subnet router.
- A reachable RTSP camera with an H.264 video track. The camera and MGU must be on routable networks.
- The receiver service runs with the system `video` group, opens `/dev/dri/renderD128`, and uses the MGU Intel Media
  SDK (`libmfxhw64`, VA-API). The app controls it through `/run/rtsp-viewer/ctl.sock`.

The RTSP app is marked for MGU Uplink internet access. Public camera sockets use the connecting app's allowed
socket UID. For private IPv4 camera addresses, the receiver connects to the Tailscale tool's loopback-only SOCKS5
proxy (`127.0.0.2`, configured port) so the Tailscale userspace network stack carries the camera traffic; it does
not route those packets through the head unit's normal IP routes. The Tailscale daemon itself uses MGU Uplink for
internet access. Receiver 0.1.21 requires Tailscale tool 1.104.9 from [my fork](https://github.com/dapharsyde/car-apps),
whose service enables the proxy on port 1055 and accepts advertised subnet routes in its userspace network stack. The camera's subnet must be advertised by a
reachable Tailscale subnet router and allowed by the tailnet. Hostnames use DNS servers from the uplink lease when
available. In MGU Store, configure an available network under
**Settings → Preferred connection** and **Settings → Wi-Fi** (or connect a supported USB/LTE/phone network);
installing `mgu-uplink` alone does not create an internet connection.

For additional instructions on advertising and approving the camera subnet route, see the [SmartRTSP Tailscale RTSP setup guide](https://www.smartrtsp.com/guides/tailscale-rtsp). Its iOS/Mac client-specific steps do not apply to the MGU; use the forked Tailscale tool described above on the head unit.

The current receiver supports RTSP over TCP, H.264 RTP packetization modes 0/1 (single NAL, STAP-A and FU-A), Basic
URL credentials (`rtsp://user:password@camera:554/path`), and RTSP Digest authentication (MD5 / MD5-sess with
`qop=auth`). It does not support RTSPS/TLS, Digest SHA-256, UDP RTP, or non-H.264 camera streams. Configure the
camera for H.264. Percent-encode reserved characters in the username/password. Basic credentials are not encrypted
by plain RTSP, so use Basic only on a trusted network.

The RTSP path does not determine the video codec by itself. Some Reolink models return H.265 from the main stream;
try `/h264Preview_01_sub` or set that camera stream's encoding to H.264. High-resolution main streams can also produce
frames that are too large for the head unit to decode reliably. If video freezes or fails to appear, try changing
`/h264Preview_01_main` to `/h264Preview_01_sub` to use the lower-resolution substream. The receiver reports the codec
advertised by the camera's SDP when it cannot find an H.264 track.

## Build the native receiver

The receiver uses RideTube's pinned Ubuntu 18.04 amd64 Docker build, producing an x86_64 binary compatible with the
MGU's glibc 2.27 runtime. From `native/rtsp-viewer/`, with Docker installed:

```sh
make target
```

This builds `native/rtsp-viewer/build/target/rtsp-viewer` and checks its imports against the MGU ABI. The tool
component manifest is `native/rtsp-viewer/component.json`; package/install it through the MGU deploy/Store flow. It
installs and enables `rtsp-viewer.service`, which launches the receiver at `/var/opt/rtsp-viewer/current/rtsp-viewer`
with the system `video` group needed for the Intel decoder device. The RTSP Camera app itself runs as an app and
connects to the receiver's local control socket.

## Build the app

With Python 3, from the project root:

```sh
python3 kit/tools/build.py rtsp_viewer --hmi ID8
```

The source package is `dist/src/rtsp_viewer8@0.1.13.tgz`. Install/update the receiver tool and app, then open **RTSP Camera**.
Use **Add Camera** to save a camera name and `rtsp://` URL. Select a saved camera from the dropdown to connect.
Use the pencil icon beside the camera selector to edit its name or URL, or the trash icon beside it to delete the selected
camera after confirming. Use **Stop stream** to close the receiver. Camera names and URLs are saved
under the app's data directory with mode 0600.
When editing a name or URL, tap in the keyboard's text field to move the cursor for inline changes.
On upgrade, a URL saved by older versions is retained as the first choice, named **Cam1**.

## Limits and validation

- This viewer displays video only; it does not play audio or record footage.
- H.264 is decoded by the MGU Intel Media SDK; the camera's bitrate and profile still need to fit the head unit's
  decoder capacity.
- RTSPS/TLS needs additional protocol support.
- The source app package and MGU-compatible native tool were built here. The complete path should be validated on a
  physical iDrive 8 head unit; this environment does not include the MGU decoder libraries/hardware.
- Avoid operating or watching video while driving; follow local laws and BMW's restrictions.

## License

The app and receiver are GPL-3.0-or-later. The included MGU App Kit runtime retains its MIT license in `kit/LICENSE`.
Intel Media SDK ABI headers retain their MIT notices. See `THIRD_PARTY_LICENSES` for notices carried over from the
decoder implementation.

## Install from USB

The ready-to-copy package is `dist/rtsp-camera-usb.zip`. Extract its **contents** to the root of a USB stick; preserve
the `mgu-repo` folder. It contains both RTSP packages, the Tailscale update source overlay, and the official MGU Store
release packages. MGU Store documents this layout for USB installs and Store first-install without internet at the car.

- If MGU Store is already installed, plug the stick into the car, open **MGU Store → USB**, install **RTSP Camera
  Receiver** first and then **RTSP Camera**, and restart the head unit when prompted. The catalog declares the
  renderer and receiver dependencies.
- To install MGU Store from this stick, use Node.js 18+ and the head unit's root SSH key on the computer. From the
  USB root in PowerShell, run:

  ```powershell
  node .\mgu-repo\mgu-store\deploy\mgu-install.js first-install --from .\mgu-repo\mgu-store --identity $HOME\.ssh\mgu_root_key
  ```

  Replace the key path with your MGU root key. Connect the computer to the head unit over its supported SSH route,
  keep the car parked on stable power, and accept the restart prompt. After setup, use **MGU Store → USB** to install
  the RTSP packages.

The USB catalog is at `dist/usb-stick/mgu-repo/local/catalog.json`. MGU Store's `usb-repo` command generates the
package hashes and sizes; dependency declarations are added to the local catalog because its package-file import
does not infer them from tool manifests. See the [MGU Store USB instructions](https://github.com/robertnisipeanu/mgu-store#usb-sticks)
and [catalog schema](https://github.com/robertnisipeanu/mgu-store/blob/main/docs/PUBLISHING.md#2-catalog).




## Publish through MGU Store

This project includes a schema 2 catalog for Store publishing. See [PUBLISHING.md](PUBLISHING.md) for the HTTPS catalog URL, dependencies and USB source details.

## Credits and Thanks

- Routing over Tailscale provided by IterioDev's [car-apps](https://github.com/IterioDev/car-apps).
- Display method from waliworld's [iDrive8-RideTube](https://github.com/waliworld/iDrive8-RideTube).
- [OpenAI ChatGPT / Codex](https://openai.com) – Assisted with code generation, debugging, and documentation.

## Support

If you find this project useful, consider supporting its development:

- **Monero (XMR):** `88UWPj3s3RvHw5CSg9xYvnNxSTDUVAxxEi4BV7PMxwEYfpemYNWg4CuJWSXtUstgFeAvKYifLYSPcYFqwTQYKNt71xoR8si`











