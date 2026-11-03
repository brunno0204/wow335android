# WoW 3.3.5a for Android ARM64

Experimental Android port of the World of Warcraft **3.3.5a client (build 12340)**, based on [SatyPardus/wowdecomp](https://github.com/SatyPardus/wowdecomp). Current source snapshot: **v0.83**.

**Work in progress.** This is not a complete or officially supported Android release. Features are partial, bugs remain, and a successful build does not guarantee compatibility with a particular device or server.

The repository contains source code, not the game data. You must provide your own 3.3.5a client files. MPQ archives, models, music and other game assets are not distributed here.

## Requirements

- Android 8.0 or later (API 26), **64-bit ARM Android (`arm64-v8a`)**, and OpenGL ES 2.0.
- Enough storage for your client files and the application.
- A complete **WoW 3.3.5a** `Data` directory, including the MPQ archives and locale files.
- An account on a server compatible with this client and the features implemented by this port.

This app does not connect to current WoW Retail servers. Servers requiring Warden are not supported. Notes for a controlled local test server are available in [android/local-server/LEIA-ME.md](android/local-server/LEIA-ME.md) (Portuguese).

## Get the APK

If an APK is available under [Releases](https://github.com/brunno0204/wow335android/releases), download the version you want to test. You can also build it through [GitHub Actions](https://github.com/brunno0204/wow335android/actions):

1. Open **Actions → Android APK → Run workflow**.
2. Wait for the run to finish successfully.
3. Open the run and download the **wow-android-arm64-apk** artifact.
4. Extract the downloaded archive and copy the APK to your phone.

Downloading Actions artifacts may require signing in to GitHub. If a build fails, check its logs; the presence of a workflow does not guarantee a successful APK build.

## Install or update

Open the APK on Android and complete the installation. If requested, allow the browser or file manager you are using to install this application.

**Updating an existing installation requires a compatible version and the same signing key.** APKs built by different people or separate Actions runs may have different signatures. The signing key used for previously shared development APKs is not included in this repository. Uninstalling the app removes its local settings, so record your data paths first.

## Set up your game files

Copy the complete client data to your phone. Example:

```text
/storage/emulated/0/Download/WoW/
└── Data/
    ├── common.MPQ
    ├── common-2.MPQ
    ├── ...other client MPQs...
    └── enUS/
        ├── ...locale files...
        └── realmlist.wtf
```

`enUS` is only an example: keep the locale directory from your installation. A single MPQ is not sufficient. Data from another expansion cannot replace the 3.3.5a files.

1. Open **WoW 3.3.5a → Configuração** (Settings).
2. Set the **Data** directory, for example `/storage/emulated/0/Download/WoW/Data`.
3. Tap **Permitir arquivos** (Allow files) and grant the requested Android file access.
4. Return to the app and wait for the files to load.

Select the directory, not an individual `.MPQ` file. You can add or move the data after installing the APK; rebuilding or embedding the game data in the APK is unnecessary. Some app labels remain in Portuguese.

## Configure your server

The authentication address and port are read from **`realmlist.wtf`**. Use the values supplied by your server administrator:

```text
set realmlist your-server.example:3724
```

Replace the example address. If no port is specified, authentication defaults to `3724`. The world server address is received in the realm list.

Automatic discovery checks locale directories under `Data`, `Data` itself and the client directory. If multiple configurations are ambiguous, enter an explicit `realmlist.wtf` path in the optional Settings field.

Log in using your server account, select a realm and character, then try entering the world. A server working with the PC client does not guarantee that all of its custom requirements are supported by this port.

## Touch controls

| Action | Control |
|---|---|
| Move | Drag on the free left half of the screen; a floating joystick appears where you touch |
| Rotate the camera | Drag on a free area of the screen |
| Zoom | Pinch with two fingers |
| Jump | Tap **PULAR** |
| Select a unit | Tap it in the world |
| Use the UI | Tap its buttons and other elements |
| Type in chat | Tap the chat input to open the Android keyboard |

The UI can consume touch gestures, so dragging a window is different from dragging the world. Full physical gamepad support is not guaranteed in this version.

## Current status

The code includes MPQ loading, login and character screens, terrain and world objects, touch movement, 3.3.5a networking and partial integration of the original UI. Equipment, NPCs, chat, quests and other systems have implementations, but coverage varies. Many Lua API functions still return placeholder or incomplete data.

Portraits, animations, audio, effects, performance, vendors, mail, auctions and talents may have limitations. A visible window does not mean that all of its operations work.

The previously delivered v0.83 APK passed ARM64 compilation, package integrity, signature and alignment checks. That build step did **not** execute the APK on a device or server. Historical implementation notes and third-party test reports are in [android/README.md](android/README.md) (Portuguese).

## Troubleshooting and bug reports

| Problem | Check |
|---|---|
| Data folder or MPQs not found | Full directory path, complete client files and file access permission |
| Server not found | `realmlist.wtf`, port, locale selection and connectivity |
| `AUTH_REJECT` or disconnection | Exact error, client version and server requirements; there is no single confirmed cause |
| Missing or broken interface | Original UI setting and UI log |
| App closes | Reopen it and copy the crash report, if available |
| Update will not install | APK signature and installed version |

When opening an issue, include the APK version, device model, Android version, data locale, reproduction steps and full error message. Never post passwords or tokens.

The **Exportar arquivos de teste** button copies game files into ZIP archives. Review the contents and do not publish these game assets in issues or in this repository.

## Build locally

Install:

- JDK 17.
- Android SDK Platform 35 and Build Tools 35.0.0.
- Android NDK 27.2.12479018 (r27c).
- CMake 3.22.1, as configured by Gradle.

Open the `android` directory in Android Studio, or run from the repository root:

```sh
git clone https://github.com/brunno0204/wow335android.git
cd wow335android/android
chmod +x gradlew
./gradlew --no-daemon assembleDebug
```

On Windows, use `gradlew.bat assembleDebug`. Expected output: `android/app/build/outputs/apk/debug/app-debug.apk` relative to the repository root. Building requires network access to the tool and dependency repositories. The Actions workflow installs the required SDK components as well.

## Source layout and credits

| Directory | Contents |
|---|---|
| `android/` | Android application, controls and build configuration |
| `Tools/AndroidPort/` | Networking, graphics, data loading and UI adaptation |
| `Engine/`, `WoW/` | Reconstructed client code and modules |
| `ThirdParty/` | Dependencies and their notices/licenses |

Based on [SatyPardus/wowdecomp](https://github.com/SatyPardus/wowdecomp). The [upstream README](README.upstream.md) is preserved; its playability claims describe the upstream project, not the state of this Android port. Preserve upstream attribution and dependency notices. This project is not affiliated with Blizzard Entertainment. World of Warcraft and its assets belong to their respective owners.
