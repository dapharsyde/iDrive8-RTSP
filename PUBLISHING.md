# Publish with MGU Store

This project uses the Store's schema 2 **Catalog** layout. The repository root `catalog.json` lists the newest app and receiver-tool artifacts in `packages/`, with SHA-256 hashes and the shared PNG icon. The icon is 140 × 140 and under the Store's 128 KiB limit.

After pushing the repository to GitHub, add this HTTPS catalog URL as a source in MGU Store:

`https://raw.githubusercontent.com/dapharsyde/iDrive8-RTSP/main/catalog.json`

The catalog uses relative artifact and icon URLs, so GitHub serves them from the same branch. For a USB install, use `dist/rtsp-camera-usb.zip`; its local catalog includes the same Store listing icon and the original-source Tailscale overlay.

The app requires receiver 0.1.9, Tailscale 1.104.9, MGU Render, and MGU Uplink. For private cameras, add the original `IterioDev/car-apps` source in MGU Store so its Tailscale requirement can be resolved. Tailscale remains owned and updated by its original source.

The app id remains `rtsp_viewer8` so this catalog can update the existing installation. App ids are global in MGU Store; check for an id collision before publishing or adding this source.

No BMW `oap-sdk` is included in the source repository or app package. The head unit supplies its donor SDK when it builds the app.

## GitHub Releases layout

The checked-in `catalog.json` uses paths relative to itself for the HTTPS catalog URL and USB folder layout. If publishing through MGU Store's **Releases** channel instead, create a GitHub Release named `catalog` and attach the two `.tgz` files and `rtsp-camera.png`. In that release's `catalog.json`, change each artifact `url` to its absolute release download URL, such as `https://github.com/dapharsyde/iDrive8-RTSP/releases/download/catalog/rtsp_viewer8@0.1.2.tgz`, and set `icon` to `rtsp-camera.png`. Keep the artifact and icon hashes unchanged. Upload this release-specific catalog as the release asset `catalog.json`.

For a private GitHub repository, use the GitHub Releases source with the Store's authenticated GitHub connection; the raw HTTPS catalog URL above is intended for a public repository.
