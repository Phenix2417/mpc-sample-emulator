# MPC Remote

A full-screen control surface for Akai's MPC Sample program running in the Pixel's Linux Terminal
(via `mpc-pixel-kit`). The MPC itself keeps running in the Linux Terminal; this app shows its screen
and sends your pads, buttons and knobs to it. Sound plays from the Linux Terminal as before.

The app contains no Akai software. It connects to the kit's helper at `127.0.0.1:8765`.

## Use it

1. Once, in the Linux Terminal: `~/mpc-pixel-kit/autostart.sh on` (the MPC then starts whenever the Terminal opens).
2. Open **MPC Remote**. If the MPC isn't running, the app opens the Terminal, which starts it; switch back to the
   app and the MPC appears when it is up (about 20 s).
3. If the Terminal asks whether to allow port 8765, allow it.

Without autostart, start it yourself: `cd ~/mpc-pixel-kit && ./4-run.sh`.

Quick test without installing anything: open Chrome on the phone at `http://127.0.0.1:8765`.
If that shows the MPC, the app will work too.

## Get the APK

Every push to `main` is built on Expo's servers (`.github/workflows/build-eas.yml`). It needs the repository
secret `EXPO_TOKEN` (Settings > Secrets and variables > Actions; make the token on expo.dev). On the phone, open
the repository on github.com > **Releases** > the newest "MPC Remote (Expo build N)", download **MPC-Remote.apk**
and install it (allow installing from your browser when Android asks).
To build again without a change: **Actions** > **Build APK on Expo (EAS)** > **Run workflow**.
Backup without Expo: **Actions** > **Build APK (backup, no Expo)** > **Run workflow**.

## Open in Expo Go (no APK)

The repository also runs as an Expo development server on Railway, so **Expo Go** can open the app from a link.

1. On railway.com: **New Project > Deploy from GitHub repo > mpc-sample-emulator**. It builds with the `Dockerfile`.
2. In the service: **Settings > Networking > Generate Domain**, port **8081** if it asks.
3. On the phone, in Expo Go: **Enter URL manually** > `exps://<that domain>` (for example `exps://mpc-remote.up.railway.app`).
   The deploy log prints the exact address.

Expo Go has to match the app's Expo SDK (58). The first load takes a little while (the server builds the app).
In Expo Go, opening the Linux Terminal automatically may not work (that needs the installed app); start the MPC yourself.

## Layout

The kit serves the layout, so the app always matches the kit you run (no app update needed when the kit
changes): the MPC screen on top (its top boxes are B1/B2/B3), K1–K3 and the encoder, the button groups,
SHIFT / PAD BANK / fader / ERASE / NOTE REPEAT beside the 16 pads, and the transport row. Pads and buttons
light up as the MPC sets them. The top of the screen is kept clear of the camera; status and navigation
bars are hidden (swipe from an edge to show them for a moment).

## Sound

The MPC still makes its sound in the Linux Terminal. If the sound stops while MPC Remote is in front,
Android has paused the Terminal in the background: use split screen (Terminal small, MPC Remote large),
or switch back to the Terminal.
