# GatedPulse Android app

*A [Stan's Legacy](https://stanslegacy.com) bench instrument: the Stanley
Meyer Archive.*

A small Android app that finds the GatedPulse generator on your Wi-Fi and opens
its control panel. It isn't on the Play Store. You install the APK directly.

**[Download GatedPulse.apk](https://bitbucket.org/cbake6807/dualtripleseq/raw/master/android/GatedPulse.apk)**:
the generator's own web page links here too, at the bottom.

Needs Android 7.0 or newer. The phone has to be on the **same Wi-Fi network as
the generator**.

---

## Installing it

### 1. Download the APK

Open the link above on the phone, in Chrome or any browser. Chrome may warn
that *"this type of file can harm your device"*. Tap **Download anyway**.

### 2. Allow your browser to install apps

When you open the download, Android asks for permission the first time:

> *For your security, your phone is not allowed to install unknown apps from
> this source.*

Tap **Settings**, turn on **Allow from this source**, then go back.

(If you missed the prompt: **Settings → Apps → Special app access → Install
unknown apps → Chrome → Allow**. The menu names vary a little by phone maker.)

### 3. Install

Tap **Install**. Google Play Protect may show *"Unsafe app blocked"* or
*"App from unverified developer"*. Tap **More details → Install anyway**.
This is expected for any app that doesn't come from the Play Store.

If **Install anyway** isn't offered, your phone is enforcing Google's new
developer-verification rule. Use the next section.

### 3b. If your phone blocks unverified apps

Since 30 Sept 2026, certified Android phones in **Brazil, Indonesia, Singapore
and Thailand** block apps from unverified developers. Google is rolling this out
everywhere through 2027. To install anyway, switch on the one-time "advanced
flow":

1. **Turn on developer options.** Go to **Settings → About phone** and tap
   **Build number** seven times. On Samsung it's **Settings → About phone →
   Software information → Build number**. Enter your PIN when asked.
2. Go to **Settings → System → Developer options**. Find the option for
   installing apps from unverified developers. Turn it on and follow the
   prompts: you confirm nobody is pressuring you, restart the phone, and
   confirm with your fingerprint or PIN.
3. **Wait 24 hours.** This is a deliberate cooling-off period. Afterwards,
   repeat step 3 above and **Install anyway** will be offered.

Once the option is on, you can turn developer options back off.

**No waiting, if you have a computer:** installing over USB with `adb` skips
the verification check entirely:

1. Turn on developer options (step 1 above), then turn on **USB debugging**.
2. On the computer, install
   [Android platform-tools](https://developer.android.com/tools/releases/platform-tools).
3. Plug in the phone and accept the *"Allow USB debugging?"* prompt.
4. Run `adb install GatedPulse.apk`.

### 4. First launch

- **"Allow GatedPulse to find, connect to and determine the relative position
  of nearby devices?"**: tap **Allow**. On Android 17 and newer this is the
  local-network permission, and without it the app can't reach the generator.
  If you denied it by mistake, tap **Open app permissions** on the connect
  screen and turn on **Nearby devices**.
- The app searches the network and connects to the first generator it finds.
  It remembers the generator and reconnects next time.
- If nothing is found, type the generator's IP address (e.g. `192.168.1.9`).
  The serial console prints it at boot, and the `P` command shows it.

**⇄** (top right) switches generators and **↻** reloads the panel; long-press
either to see what it does. Tap Stan to bring up the opening screen, and tap
again to dismiss it. Addresses you've used are listed under **Recent**: tap
one to switch, long-press to forget it. The screen stays on while connected.

**Connection status.** The dot under the title is green while the generator
answers. If it stops answering for about 6 s (a reboot, a firmware update, a
Wi-Fi drop), the dot turns amber, an amber banner says so, and the phone buzzes
twice. The app keeps retrying the same generator, and follows it if it comes
back on a new IP address. It never jumps to another board on the network. When
it's back, the banner clears with one buzz. If the generator came back with
new firmware, the panel reloads itself.

### Updating

Download and install the new APK over the old one. Your settings are kept.
If Android says *"App not installed as package conflicts with an existing
package"*, the new APK was signed with a different key. Uninstall the old app
first.

---

## Building it (maintainers)

There's no Gradle and no Android Studio. `build.sh` drives the SDK tools
directly. The whole app is one Java file using only framework APIs, so the APK
is tens of kilobytes.

**What it does:** it finds the board by mDNS (`_http._tcp`, name starting
`gatedpulse`), checks that `GET /state` returns JSON, and shows the board's
own web UI (`webui.h`) in a WebView. The controls are *not* rebuilt natively,
so the firmware page stays the only place a control is defined. The WebView's
user agent ends in `GatedPulseApp/1`, which the page checks to hide its own
download link.

### One-time toolchain setup (macOS, no sudo)

```sh
# JDK 21 (user-level; /usr/libexec/java_home finds it)
mkdir -p ~/Library/Java/JavaVirtualMachines
curl -L "https://api.adoptium.net/v3/binary/latest/21/ga/mac/x64/jdk/hotspot/normal/eclipse" \
  | tar -xz -C ~/Library/Java/JavaVirtualMachines        # use aarch64 on Apple silicon

# Android command-line tools -> ~/Library/Android/sdk
#   https://developer.android.com/studio#command-line-tools-only
mkdir -p ~/Library/Android/sdk/cmdline-tools
unzip commandlinetools-mac-*_latest.zip -d ~/Library/Android/sdk/cmdline-tools
mv ~/Library/Android/sdk/cmdline-tools/cmdline-tools ~/Library/Android/sdk/cmdline-tools/latest

export JAVA_HOME=$(/usr/libexec/java_home -v 21+)
SDKM=~/Library/Android/sdk/cmdline-tools/latest/bin/sdkmanager
$SDKM --licenses                       # read and accept the SDK licences
$SDKM "platforms;android-37.1" "build-tools;37.0.0" "platform-tools"
```

### Build / release

```sh
make apk                    # -> android/GatedPulse.apk
make apk-install            # ...and adb install -r to a USB-connected phone
```

To release, bump `VERSION_CODE` and `VERSION_NAME` in `build.sh`, build, then
commit and push `android/GatedPulse.apk`. The web UI's link always points at
`master`.

**Signing key:** the first build creates `~/.android/gatedpulse-release.jks`,
outside the repo on purpose. **Back it up.** Phones only accept an update if
it's signed with the same key. Build from a different machine or key and every
user has to uninstall and reinstall.

| | |
|---|---|
| minSdk | 24 (Android 7.0) |
| targetSdk | 37 (Android 17): local-network permission requested at runtime |
| compileSdk | android-37.1, build-tools 37.0.0, JDK 21 |
| Edge-to-edge | handled: system-bar, cutout and IME insets are applied as padding |
| Back | predictive back (`OnBackInvokedCallback`) on 33+, key event below |

**Branding** follows stanslegacy.com: its dark palette (vellum `#14181e`,
sheet `#1c222a`, cobalt `#7fa8f2`, amber `#d7a343`), IBM Plex type where
installed, and Stan's chalk line-art. `res/drawable-nodpi/stan_*.png` are
white-on-transparent crops of the site's share image
(`stanslegacy.com/images/og/stanslegacy.jpg`); the web UI embeds the site's
own small brand mark. The page loads no external fonts or images, so it
still renders with no internet.
