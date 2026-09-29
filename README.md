# Nixie clock for the BUSY Bar

Cold-cathode tubes on the front panel of a [BUSY Bar](https://busy.app),
running as a native app on the [VeryBUSY](https://code.mccullough.dev/christian/VeryBUSY)
firmware.


![Six tubes rolling over from 12:59:59 to 13:00:00](docs/nixie-busybar.gif)

Native FAP (Flipper application package) for Nixie clock. For now to run this you'd
need a custom firmware that supports clock apps. (See releases). Firmware based on
VeryBusy firmware. I'm not responsible for bricking your device. 

## Controls

- **Setup** Settings allow you to enable or disable displaying seconds.
- While the clock shows, the wheel does the rest:
  - **tap**: the colour, orange, blue, green, red;
  - **turn**: how alive clock is. From calm to some crazy levels;
  - **long press**: change glow profile. I couldn't decide which one looks best,
  so hope you'd struggle with this as well
  - **Back**: the Start screen.

## Install

The app needs one thing the stock VeryBUSY r7 does not have: the time service
exported to native apps (a [three-file change](firmware/0001-export-the-time-service-to-native-apps.patch),
API 7.1 to 7.2). Until it lands in a VeryBUSY release, the
[release](../../releases) carries a firmware build that includes it. So:

1. Flash `VeryBUSY-r7-fix3-f22-update.tgz` from the release: on the bar's Web
   UI, Settings, Update, upload the file. It is VeryBUSY r7 plus the export
   plus a fix for a timer restart loop with the cloud linked
   ([PR #1](https://code.mccullough.dev/christian/VeryBUSY/pulls/1)). Your
   apps and settings stay.
2. Upload `Nixie.fap` on the Apps tab (drag and drop), or over HTTP:

   ```sh
   curl -X POST --data-binary @Nixie.fap \
     "http://<bar>/api/storage/write?path=/ext/apps/misc/Nixie.fap"
   ```

3. Open Apps on the bar and start **Nixie Clock**.


## Build

`scripts/build.sh` clones VeryBUSY at the pinned commit, applies the export,
drops `app/` into `applications/external/nixie` and builds the FAPs. The
result is `dist/Nixie.fap`. The first run downloads the fork's toolchain.

The renderer has no firmware dependency and a host test:

```sh
app/tests/run.sh
```

It also renders frames on the host; `scripts/render-promo.py` composes them
into the device picture above.



## Credits and license

The app shell (`app_shell.h`, Start and Setup) and the build come from
VeryBUSY by Christian McCullough. The BUSY Bar device picture in `docs/` is
Flipper Devices' render, used under CC-BY 4.0; this is an unofficial project,
not affiliated with or endorsed by Flipper Devices or BUSY.

Code: GPL-2.0-or-later
