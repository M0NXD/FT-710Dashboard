# FT-710 Dashboard — Remote Control for Yaesu FT-710

A native Windows C++20 application for **local and remote control of the Yaesu FT-710**. Connect the radio to a Windows station PC by USB, then use the desktop GUI or control the same radio remotely from a phone or browser through the app's authenticated HTTPS interface. Tune frequency and VFO, change mode and radio settings, use PTT, monitor meters, and access audio/RF scopes without connecting the client device to the radio's USB port. The station PC and app must remain running and connected to the radio. Version **2.0.5** also provides two-way phone audio, with a certificate setup page for Safari and a desktop passcode editor. The GitHub release contains the compiled application and documentation only; it does not bundle third-party runtime DLLs. The application dynamically uses Hamlib 4.7.2 and the Yaesu FT-710 driver (model 1049).

## Remote control at a glance

The dashboard serves its authenticated web interface on port **8888**. Open **Web: 8888** in the desktop app to get the station address and access code, then complete the certificate setup if using Safari. Browser controls share the desktop app's live radio connection; the FT-710 stays attached to the station PC by USB. See [Web dashboard and phone audio](#web-dashboard-and-phone-audio) for setup instructions.

### Runtime dependencies

The release ZIP contains `FT710Dashboard.exe`, this README, and `check-runtime.ps1`. **The executable is not self-contained and will not start until its Hamlib runtime dependencies are installed.** It requires `libhamlib-4.dll`, `libusb-1.0.dll` and a compatible MinGW `libwinpthread-1.dll`; none of these files are included in the GitHub release. The direct downloads below match the Hamlib and WinLibs compiler versions used for **dashboard 2.0.5**.

#### Direct downloads and quick setup

| Required component | Direct download | Files to use from the extracted package |
| --- | --- | --- |
| Hamlib **4.7.2, Windows x64** | [Download Hamlib x64 ZIP](https://github.com/Hamlib/Hamlib/releases/download/4.7.2/hamlib-w64-4.7.2.zip) | `hamlib-w64-4.7.2\bin\libhamlib-4.dll` and `hamlib-w64-4.7.2\bin\libusb-1.0.dll` |
| MinGW runtime — WinLibs **GCC 15.2.0, x64, POSIX, UCRT, release 4** | [Download matching WinLibs/MinGW x64 ZIP](https://github.com/brechtsanders/winlibs_mingw/releases/download/15.2.0posix-13.0.0-ucrt-r4/winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-13.0.0-r4.zip) | `mingw64\bin\libwinpthread-1.dll` |

1. Extract the dashboard release ZIP into its own folder.
2. Download and extract the two upstream ZIPs above. Copy the **two Hamlib DLLs** and **the WinLibs `libwinpthread-1.dll`** into the dashboard folder, beside `FT710Dashboard.exe`. You do not need to install a compiler to run the dashboard.
3. Use the threading DLL from the linked WinLibs package. An older `libwinpthread-1.dll` supplied by another application or Hamlib package may lack `clock_gettime64` and prevent the dashboard from starting.
4. Run `powershell -ExecutionPolicy Bypass -File .\check-runtime.ps1` from the dashboard folder, then open `FT710Dashboard.exe`.

The minimum application folder contains:

```text
FT710Dashboard.exe
libhamlib-4.dll
libusb-1.0.dll
libwinpthread-1.dll
README.md
check-runtime.ps1
```

Alternative Hamlib installation: [download the Hamlib x64 installer](https://github.com/Hamlib/Hamlib/releases/download/4.7.2/hamlib-w64-4.7.2.exe) and make its installed `bin` directory available on `PATH`; still use the compatible WinLibs threading DLL beside the dashboard. The [Hamlib release page](https://github.com/Hamlib/Hamlib/releases/tag/4.7.2) and [WinLibs release page](https://github.com/brechtsanders/winlibs_mingw/releases/tag/15.2.0posix-13.0.0-ucrt-r4) provide package details and upstream checksums.

The FTDI `FTD2XX.dll` and `LibFT4222.dll` files are **not needed to start the dashboard or use CAT controls**. They are optional and needed only for RF scope capture. Obtain them from the two separate FTDI packages below and place them beside `FT710Dashboard.exe`. Enable **SCU-LAN10** on the radio for RF scope capture. PortAudio is statically linked, so it has no separate runtime DLL requirement. For local development builds, CMake stages dependencies into the ignored `dist/` directory; those staged files are not uploaded to GitHub.

#### FTDI DLLs for RF capture

**The FT4222H library download does not include the native `FTD2XX.dll`. Download both packages separately:**

| Required dashboard file | Official download | Windows package to choose |
| --- | --- | --- |
| `FTD2XX.dll` | [FTDI D2XX drivers](https://ftdichip.com/drivers/d2xx-drivers/) | Windows **Desktop**, **x64 (64-bit)** driver package or Windows driver installer |
| `LibFT4222.dll` | [FTDI FT4222H software examples and libraries](https://ftdichip.com/software-examples/ft4222h-software-examples/) | **LibFT4222 Windows Library (v1.4.8) and Examples** |

1. Install the FTDI Windows desktop driver on the station PC. On 64-bit Windows, copy the installed `%WINDIR%\System32\FTD2XX.dll` beside `FT710Dashboard.exe`. If the downloaded driver ZIP supplies its x64 DLL as `amd64\ftd2xx64.dll`, name the copy **`FTD2XX.dll`**. The Windows USB driver must still be installed for capture to work.
2. Extract the LibFT4222 Windows package. Copy **`imports\LibFT4222\dll\amd64\LibFT4222-64.dll`** beside `FT710Dashboard.exe` and rename the copy to **`LibFT4222.dll`**.
3. Enable **SCU-LAN10 = ON** on the FT-710, then start RF capture. For a source build, put the same two named DLLs in `runtime/ftdi/` so CMake can stage them into `dist/`.

Use **amd64/x64** files for this dashboard. `i386` and `SysWOW64` DLLs are 32-bit, ARM64 files target a different architecture, and `FTD2XX_NET.dll` is a .NET wrapper rather than the required native DLL. Neither FTDI DLL is bundled in this repository or its release ZIP.

## Features

- Live VFO A/B frequencies, modes and passband widths; VFO selection, swap and copy. Touch/mouse drag tuning, mouse-wheel tuning and larger step buttons, plus exact frequency entry in MHz.
- Mode selection applies immediately using the mode's normal filter; a separate filter editor sets the passband in Hz. Tuning editors follow radio readbacks and preserve unsubmitted typed values.
- Signal strength, RF output in watts, SWR, ALC, compression in dB, supply voltage and drain current. TX meters are marked **TX only** while receiving rather than showing an old measurement.
- AF/RF gain, RF power setting **in watts**, microphone gain, squelch and IF shift; preamp cycling (IPO/AMP1/AMP2), DNR Off–15 and NB Off–10 buttons, plus auto notch, APF, monitor, processor, VOX, dial lock and tuner switches.
- Gain/level editors start empty and unavailable until a successful radio read. A separate **Radio:** readback remains visible while you type a new setting.
- **Power on / Power off** through `rig_set_powerstat`. The serial connection remains open after power off. Hamlib performs the Yaesu wake-up sequence; this can take several seconds.
- A direct FT-710 connection in standby shows **Powered off**, including when the radio is silent and CAT queries time out. The frequency timeout appears in Activity rather than a banner. The CAT connection stays open, **Power on** remains available, and polling continues so readings return when the radio responds. A timeout does not count as a confirmed power-state reply to other Hamlib clients.
- A searchable **All settings** table generated from Hamlib capabilities: levels, functions, parameters, extension settings, RIT/XIT, split, tuning step, memory selection, tones, repeater controls and read-only Hamlib connection configuration. Available rows reflect the installed backend, including its read errors.
- Setting editors with numeric ranges, switch controls and named enum choices; write followed by readback when the driver supports it.
- Single-click/tap **PTT on / PTT off** on the dashboard and in the header (available on every tab). Both buttons toggle the same transmit request, with a selectable timeout (30–600 seconds; default 180 seconds).
- **Audio routing** with separately enabled receive and microphone paths, named device mappings, level controls and input/output meters. Direct WDM-KS host devices remain discoverable independently of RDP's WASAPI endpoint list; redirected phone devices are labelled separately.
- **Audio scope** with a live received-audio spectrum and scrolling waterfall, selectable 3/6/12/24 kHz spans, display gain/floor, freeze, peak hold and clear. It shares the receive stream for both Windows shared and direct/RDP audio routes.
- **RF spectrum and waterfall** from the FT-710's FT4222H USB-SPI scope interface, in both the native app and embedded web interface. It supports 1 kHz–1 MHz spans, a saved waterfall level, tuning by clicking the display, and a scrolling waterfall with newest data at the top.
- Activity log with UTC timestamps, an advanced direct CAT console and CSV export.
- Background polling, stale-data handling, adjustable CAT baud/stop bits/flow control/poll rate, and saved connection preferences.
- Scrollable layouts on smaller RDP windows, DPI-aware fonts/table columns, and selective repainting. Pending tuning and gain requests wait for a worker acknowledgement before reverting to confirmed values. The settings table repopulates when capability discovery finishes.
- Direct USB serial CAT, connection through an existing `rigctld`, and an offline **Hamlib Dummy** mode.
- An embedded **Hamlib / NET rigctl TCP server**. The dashboard owns the serial port and up to 16 applications can share its radio connection over the network protocol.
- An embedded **HTTPS web interface on port 8888**, listening on all IPv4/IPv6 addresses, with the same radio controls, driver settings, meters, activity/CAT console, CSV export, audio mappings, spectrum and waterfall. Its responsive layout includes a persistent single-tap PTT button for phones.
- **Two-way radio audio in the phone/browser**, using trusted HTTPS and microphone permission. An iPhone/iPad certificate profile and installation guide are available on the HTTP setup page. Microphone output requires the corresponding confirmed PTT owner.

## RF spectrum and waterfall

The RF scope is part of the same desktop and web dashboard as the radio controls, audio tools, and CAT server. It reads the FT-710's built-in RF display over USB; it does not provide wideband I/Q or audio from every frequency in the selected span.

### Required radio and USB setup

Enable **MENU → OPERATION SETTING → GENERAL → SCU-LAN10 → ON** on the FT-710 (CAT reference menu **03-01-26**). This setting is required for the radio to expose its scope interface over USB. After connecting the radio, Windows should show the radio's two virtual COM ports and **FT4222H Interface A**.

The scope uses three interfaces:

| FT-710 interface | Dashboard use |
| --- | --- |
| Enhanced USB **CAT-1 COM** | Hamlib radio control: VFOs, settings, meters, power and PTT |
| Standard USB **CAT-2 COM** | Sends and verifies scope center/span/waterfall settings when you choose **Apply to radio** |
| **FT4222H Interface A** | Receives the radio's scope frames over USB-SPI during capture |

CAT-1 and CAT-2 must be different COM ports. The dashboard owns CAT-1 for as long as the radio connection is open. It opens CAT-2 only when applying scope settings, and opens FT4222H Interface A only during RF capture. Close other software that has opened the interface being used.

### Capture and tune

1. Connect the dashboard to the **Enhanced CAT-1 COM** port using **FT-710 / USB CAT**.
2. Open **RF scope** and enter the **Standard CAT-2 COM** port and baud rate configured on the radio (the dashboard defaults to 4800 baud).
3. Choose a span and waterfall level, then press **Apply to radio**. The app sends the scope settings and checks the CAT-2 replies.
4. Press **Start RF capture**. The display follows the span reported by the radio, including changes made directly on the radio. The initial span is **200 kHz** and the default waterfall reference level is **−5 dB**.
5. Tap or click the spectrum or waterfall to tune the active VFO. The scope contains **850 display bins**. New waterfall rows appear at the top while earlier data scrolls down.

The RF scope level adjusts the radio's waterfall reference level, not RF power or receiver gain. The scope shows the FT-710's displayed RF levels across its selected span; only the tuned receiver demodulates audio. The separate **Audio scope** displays received audio and uses the selected audio capture route.

The x64 FTDI runtime DLLs are optional at build time and required beside the executable only when using RF capture. Get `FTD2XX.dll` from [FTDI's D2XX driver package](https://ftdichip.com/drivers/d2xx-drivers/) and `LibFT4222.dll` from [FTDI's FT4222H library package](https://ftdichip.com/software-examples/ft4222h-software-examples/); see [FTDI DLLs for RF capture](#ftdi-dlls-for-rf-capture) for the file locations and naming instructions. For source builds, place both files in `runtime/ftdi/`. The files are excluded from Git; see [`runtime/ftdi/README.md`](runtime/ftdi/README.md).

## Connect your FT-710

1. Install Yaesu's FT-710 virtual COM port driver and connect the rear USB-B port to the PC.
2. In Windows Device Manager, find **Silicon Labs Dual CP210x — Enhanced COM Port** and note its COM number. Select this port for CAT-1. Note the **Standard COM Port** separately for RF scope CAT-2, and verify **FT4222H Interface A** is present if you plan to use the RF scope.
3. On the radio, check **CAT-1 RATE**, **CAT-1 CAT-3 STOP BIT**, and the RTS/flow-control setting. Match these in the dashboard. Defaults are 38400 baud, 1 stop bit, no handshake; if your radio is configured otherwise, change the dashboard values.
4. Open `FT710Dashboard.exe`, select **FT-710 / USB CAT**, enter the actual port (for example `COM5`), and click **Connect**. The COM3 placeholder is not port detection.
5. Keep the radio's DC supply switched on to use CAT **Power on** from standby. A radio without DC power cannot be woken by software. If Windows removes the COM port during a power transition, reconnect after it returns.

No radio is connected automatically on normal launch. Connection, RF scope, audio, and web settings are saved at `%LOCALAPPDATA%\M0NXD\FT710Dashboard\settings.ini`. If an earlier RF scope build saved settings in `%LOCALAPPDATA%\M0NXD\FT710DashboardRF`, the application imports its scope preferences when needed. Disconnect closes CAT; it does not power off the radio. Connect does not intentionally send a power-on command.

## Web dashboard and phone audio

Keep the Windows dashboard running. The web server starts with the application and listens on **port 8888 on all network addresses**: a dual-stack `[::]:8888` listener covers IPv4 too, with an IPv4 `0.0.0.0:8888` fallback when IPv6 is unavailable. The browser and desktop GUI use the same radio worker and serial connection; Hamlib clients can continue using the separate CAT sharing port.

### Open it

1. Open the new application and press **Web: 8888** beside the CAT sharing controls. The dialog lists this PC's HTTPS addresses, station access code and root certificate fingerprint. **Ctrl+C** copies the dialog. It can open the local certificate setup page.
2. On your phone, first open **`http://<PC-IP>:8888/`** in Safari. For this station's current address, use `http://192.168.101.29:8888/`. This serves certificate setup only; the HTTP dashboard and separate HTTP executable have been removed.
3. Tap **Download the iPhone / iPad certificate profile**, allow the download, then open **Settings → Profile Downloaded → Install**. If needed, find it under **General → VPN & Device Management**. The profile contains only this station's public root certificate. Compare its SHA-256 fingerprint with the desktop dialog before installing it.
4. Open **Settings → General → About → Certificate Trust Settings** and enable full trust for **M0NXD FT710 Local Web Root**. Installing the profile alone does not enable TLS trust; see [Apple's trust instructions](https://support.apple.com/en-gb/102390).
5. Return to Safari and open **`https://<PC-IP>:8888/`**, currently `https://192.168.101.29:8888/`. Enter the access code from the desktop dialog. The phone must be able to reach that PC through your network. If Windows Firewall prompts, allow this application on the network you are using; the application does not add firewall rules automatically.

To change the web passcode, press **Set passcode** beside **Web: 8888** in the desktop application. Enter 8–64 characters, optionally use **Show passcode** or **Generate random**, and press **Save**. The passcode persists across app restarts. Saving a changed passcode signs out existing browser sessions, invalidates pending browser requests and releases browser-owned PTT; reconnect using the new passcode. Saving an unchanged value or cancelling leaves sessions active. Passcodes are edited only from the desktop application.

The passcode is stored as a base64-encoded value under `[Web] PasscodeBase64` in the settings INI so spaces, Unicode and quote marks survive Windows INI parsing. Base64 is encoding, not encryption. Earlier `[Web] AccessCode` values are preserved until you change the passcode. Sessions expire after eight hours; stopping the application invalidates sessions. Session cookies are Secure and HttpOnly; radio controls and audio are available only over HTTPS.

If another server already owns port 8888, the native Activity log and **Web: 8888** button report the error. After releasing that port, press the button to retry. Only one dashboard instance can own the web port.

### HTTPS and Safari

Browser microphone capture requires a [secure context](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getUserMedia). The dashboard uses Windows Schannel TLS 1.2 and a local RSA-2048/SHA-256 certificate authority, with a distinct server key, address SANs and server-authentication usage. Server certificates last one year and are recreated on launch with current network addresses. The stable root is reused so a PC address change followed by an app restart does not require reinstalling that root on the phone. The certificate profile is unsigned; trust is a manual device action.

For Windows or another device, download **`http://<PC-IP>:8888/certificate.cer`** and install the public root certificate using that device's trust settings. The application exports `web-certificate.cer` and `ft710-station.mobileconfig` beside `settings.ini`. It keeps private keys in the current Windows user's cryptographic key store and does not install a trusted root automatically.

If HTTPS hangs while HTTP setup opens, make sure you are running version 2.0.5, have installed the profile **and enabled full trust**, and are using an address listed by **Web: 8888**. Close the Safari tab and open a fresh tab after enabling trust. Check that the PC and phone clocks are correct. TLS negotiation failures appear in the native Activity log. If the PC address changed after launching the app, restart it to refresh certificate SANs.

On iPhone, allow microphone access when **Arm browser microphone** requests it. Safari uses the phone's system audio output; choose the speaker or headset using the phone's audio controls. Receive playback can use scheduled Web Audio buffers when AudioWorklet is unavailable, but microphone capture requires AudioWorklet support.

### Map radio audio to the browser

On the web page, open **Audio routing**:

1. **Radio receive source:** select the FT-710 USB **recording** endpoint. The **Host hardware / direct** entry remains available independently of RDP redirection.
2. **Radio transmit destination:** select the FT-710 USB **playback** endpoint.
3. On the phone, press **Use browser audio mappings**. This selects **Web browser / phone speakers** as the host listening destination and **Web browser / phone microphone** as the microphone source. These virtual endpoints also appear in the native GUI's audio selectors.
4. Press **Listen in this browser** to enable the shared receive route and this browser's playback. Select a browser output where supported, and adjust **Local listening volume**. Browsers without output selection use the phone/OS output device. Multiple browsers can listen to the same receive capture stream, even when its host listening destination is a local speaker or RDP endpoint.
5. Select a **Browser microphone**, press **Arm browser microphone**, and grant microphone permission. This enables the host microphone route and starts capture in this browser. The browser mic meter shows the captured level. Saved browser device selections remain visible if unavailable; the app does not silently replace them with a different microphone.
6. Tap **PTT on** once, speak, then tap **PTT off** once. Microphone packets are accepted only from the browser that owns the confirmed transmit request. Host microphone mappings use desktop PTT; the browser microphone mapping uses web PTT. The selected timeout (default 180 seconds) applies to web PTT too.

The FT-710 must accept modulation from USB. For SSB voice, select **RADIO SETTING → MODE SSB → MOD SOURCE = USB**, and set suitable **USB MOD GAIN / USB OUT LEVEL** as described in the radio setup below. Browser routing does not change those radio menu items. A headset helps avoid speaker audio entering the microphone.

**Stop listening** unsubscribes only this browser; the shared receive route stays available for other listeners and the scope. **Disarm browser microphone** stops capture and disables the shared microphone route. Receive playback pauses during TX. Remapping microphone audio, route failure, radio disconnect/power off, page close, or a lost browser connection requests release of dashboard-owned PTT. A stalled WebSocket is closed after 15 seconds without client activity. PTT release errors remain visible and the existing radio worker retries release.

The web page automatically reconnects after a connection loss, but listening and microphone capture stay off until enabled again. A browser that is suspended or a phone that is locked may interrupt audio or trigger the connection timeout. Keep the page active during operation. Other CAT clients or manually keyed transmissions cannot be taken over by web PTT. Up to eight authenticated browser connections are supported, with one transmit owner at a time.

Radio controls and audio mappings are shared station state. Browser volume, input/output choice and scope presentation are local to that browser. Settings show radio readbacks; a command acknowledgement means it was queued, and subsequent state updates or the activity log report the actual driver result. The settings table includes every available Hamlib descriptor and reports unsupported reads individually, matching the native GUI.

Audio is streamed as **48 kHz mono PCM16** in 20 ms packets over the authenticated secure WebSocket, with resampling and bounded buffers. This is a direct PC-to-browser connection; no WebRTC signalling, STUN/TURN server or separate phone app is required. Audio alone needs about **0.77 Mbit/s per direction**, plus scope/state updates and transport overhead. Delay depends on the network, browser and USB driver. The **Audio scope** shows received audio frequencies and digital levels; the separate **RF scope** displays the radio's USB scope frames.

Real FT-710 audio, Safari playback, browser microphone capture and transmitted modulation still need to be exercised on your chosen devices.

## VFO, tuning and mode

- **Use VFO A / Use VFO B**, or the **Control VFO** dropdown, switches the receive VFO and selects that same VFO for frequency and mode changes. The active VFO card is highlighted and its button says **active** after radio readback. Changes made at the radio or through another CAT client are also reflected in the controls.
- To browse for stations, drag the tuning strip left/right (including with a phone's RDP mouse drag), scroll the mouse wheel over the strip or frequency display, or tap **- Down / + Up**. Choose the step size beside the strip: 10 Hz, 100 Hz, 500 Hz, 1 kHz, 2.5 kHz, 5 kHz, 10 kHz or 100 kHz. The choice is saved. Each 8 logical pixels of dragging is one step. With the strip focused, arrow keys tune one step; Page Up/Down tune ten steps.
- For an exact frequency, enter MHz, such as **14.250000**, and press **Enter** or **Go**. The accepted receive range is 0.030–60.000 MHz. The large VFO displays show the radio's readback. Scrolling and step buttons use a fresh radio frequency read in the command queue, so quick consecutive inputs accumulate correctly.
- Select a **Mode** to apply it immediately to the selected VFO. Choices come from the connected Hamlib backend (for example USB, LSB, CW, AM, FM and data modes). Changing mode requests that mode's normal filter. To set another passband, enter **Filter (Hz)** and press **Set filter** or Enter; **0** requests the normal filter. The VFO card and editor show the actual mode/filter readback.
- Frequency and filter fields follow radio changes until you start typing. Your draft remains until you apply it, start step tuning (frequency only), or select another VFO. CAT errors appear in the status bar and Activity tab.

## Share the radio with Hamlib clients

The connection arrangement is:

```text
FT-710 Enhanced USB COM port
            |
       Dashboard (Hamlib FT-710 driver)
            |
       127.0.0.1:4532
          /       \
       WS/WSJT-X   Logger / other Hamlib clients
```

1. Close other programs that have the Enhanced COM port open and release TCP port 4532 if it is already in use.
2. In the dashboard select **FT-710 / USB CAT**, enter the Enhanced COM port and matching serial settings, then **Connect**.
3. Leave **CAT sharing: On** enabled. The sharing row will show **127.0.0.1:4532** and the number of connected clients. It starts automatically after a direct serial connection opens. The sharing button stops/restarts the server; it does not disconnect the radio.
4. In each other application's radio settings choose **Hamlib NET rigctl** (also called **NET rigctl**, Hamlib model **2**, or rigctld). Use **127.0.0.1:4532** as the network server, and **CAT** for PTT where applicable. Audio devices remain configured in those applications.
5. Keep the dashboard open. Its **Power on / Power off** buttons use the direct FT-710 driver, and clients use the same serialized radio command queue.

If port 4532 is occupied, the sharing row explains the problem. Stop the existing listener or change **TCP port** to **4534** and press **Retry CAT sharing**. Configure all clients with that same port. Sharing listens on this PC only (`127.0.0.1`). It does not expose an unprotected control port to the LAN.

Multiple clients can read and change the shared radio at once. Changes are applied in queue order, so a later frequency/mode change becomes the radio's current setting. There is still one physical frequency/split/transmitter configuration shared by all applications.

Each TCP client has its own protocol/VFO option state. Hamlib clients negotiate explicit VFO addressing with `\chk_vfo` (returns `1`); plain text connections initially use current-VFO commands. `\set_vfo_opt 0` or `1` changes only that connection. Standard and extended rigctld responses are supported, including capability discovery (`\dump_state`), frequency/mode/VFO, split frequency/mode, CAT PTT, power status, levels, functions, parameters and driver extension tokens, RIT/XIT, steps, memories, tones, repeater controls and information queries. Unknown/unavailable commands return a negative Hamlib `RPRT` error. This is a rigctld protocol server, not a virtual COM port or a raw Yaesu CAT socket. Daemon process management, changing the serial connection configuration remotely, channel programming, unsolicited transceive events, and CW/voice-message playback are not implemented by the server.

A client that starts CAT PTT owns that transmit request; a second client's PTT-on is rejected while it is held. PTT-off can release transmission. Disconnecting a client releases CAT PTT only if that client still owns it. Dashboard disconnect/shutdown also releases PTT initiated through this server. Dashboard PTT has its configured timeout; client PTT has no GUI timeout so digital modes can complete their normal transmit cycles.

The serial port and sharing server remain open after a successful **Power off**. Other CAT operations report `RPRT -20` while off; clients can still discover capabilities, read the known power state and request **Power on**. Hamlib's radio wake sequence may take several seconds; clients need a suitable response timeout. Power commands report the actual driver result rather than a simulated success.

Sharing preferences are saved with the connection preferences. Normal launch still requires **Connect** before anything controls the radio. Demo mode can share Hamlib Dummy and is labelled **DEMO radio** in the sharing row. Screenshot capture disables sharing.

## PTT and audio routing (including RDP from a phone)

Both **PTT on** buttons are toggles: single-click or tap either button once to transmit, then **PTT off** on either button to return to receive. You do not need to hold the button or double-click. The header button works on every tab. PTT stays active when you release the mouse/touch or move focus to another app. **Audio routing → Tap PTT timeout** sets the maximum duration for both buttons (default 180 seconds). A CAT client or manually keyed radio cannot be taken over by dashboard PTT. Disconnect, shutdown, session lock and RDP session disconnect release dashboard-owned PTT. Failed release commands retain ownership and retry, so the UI does not claim release after a CAT error.

Select four Windows endpoints on **Audio routing**:

| Mapping | Typical RDP selection |
| --- | --- |
| Radio receive source | FT-710 USB recording device (often **Microphone / USB Audio CODEC**) labelled **Host hardware / direct** |
| Listening destination | **Remote Audio [RDP / phone]** playback device |
| Microphone source | **Remote Audio [RDP / phone]** recording device / redirected phone microphone |
| Radio transmit destination | FT-710 USB playback device (often **Speakers / USB Audio CODEC**) labelled **Host hardware / direct** |

The names depend on your Windows drivers and RDP client. Local headphones, sound cards and host virtual audio devices can be selected too. The two radio selectors exclude redirected Remote Audio. Choose the radio's actual USB audio entries; CAT identifies the radio but does not identify its Windows sound device. Unrelated host USB microphones/speakers are not automatically treated as the FT-710.

### Keeping the radio audio available over RDP

RDP can expose only session/redirected endpoints through WASAPI. Version 1.4 also enumerates host audio drivers directly through **WDM kernel streaming**, then bridges those streams to the phone's WASAPI endpoints. This addresses the radio side without turning off the phone's audio redirection. **Host hardware / direct** entries refer to PC drivers; **RDP / phone** entries refer to redirected client devices. **Windows shared** entries remain usable locally when Windows exposes them.

Direct mappings are saved using the actual driver filter path and pin/mux identity, not a transient list index or guessed device name. Shared/remote mappings retain their Windows endpoint IDs. The selected names are saved too. Missing saved devices stay visible as **Unavailable** and cannot be started; no default or Remote Audio device is substituted for the radio. If you selected a Windows shared radio entry before RDP, select its **Host hardware / direct** entry once and save that mapping. Use **Refresh devices** after attaching the radio's USB audio or starting/reconnecting RDP. Refresh always stops routes first.

WDM-KS requires a compatible PCM audio driver and an available streaming pin. Another application's exclusive access or an open Windows audio stream may occupy that pin; release that audio device in the other app if opening it fails. CAT sharing continues to work independently. A saved name does not make unplugged or unsupported hardware usable. Device enumeration and route errors are recorded in **Activity / CAT**.

Enable **Receive audio** to listen and **Microphone** to arm the microphone route. Routes start **off** on every launch and must be enabled after connecting the physical radio. Microphone input is captured only while its route is enabled; its input meter then shows the selected microphone. Microphone output is silent until the dashboard owns PTT and reports TX; it does not feed another client's digital transmission or a manually keyed radio. Receive playback mutes during TX. Turning microphone routing off or remapping it releases dashboard PTT. Changing a mapping stops that route; refresh stops both. Device loss stops the affected route and logs the Windows error; microphone route failure releases dashboard PTT. Session lock/disconnect and radio disconnect/power off stop both routes. Reconnecting does not automatically re-enable them.

The **Level (%)** fields attenuate routed audio from 0–100%; enter a value and leave the field to apply it. They are separate from radio gain controls. The bars show peak digital audio levels; red indicates near full scale. Shared-only routes use Windows conversion to 48 kHz float stereo. Direct routes negotiate supported PCM rates/channels on each device; a bounded mono buffer and interpolation bridge different rates, feeding the selected output channels. RDP adds its own delay. Version 2.0 additionally offers the browser audio endpoints described above; RDP redirection remains an alternative.

### Phone/RDP setup

1. In the phone's RDP client, enable **microphone redirection** and playback **on the phone**, and grant microphone permission. Reconnect after changing these options. In Microsoft Windows App these controls are in the remote PC's device/audio redirection settings; availability depends on the platform, client and host policies. See [Microsoft's redirection instructions](https://learn.microsoft.com/en-us/windows-app/device-audio-folder-redirection-teams).
2. On Windows, allow microphone access for desktop applications. If a redirected recording/playback device is absent, enable redirection in the RDP client/host policy first. Use **Host hardware / direct** for the radio even when Windows Sound shows only Remote Audio in RDP.
3. On the FT-710, for SSB voice select **RADIO SETTING → MODE SSB → MOD SOURCE = USB**. Set an appropriate **USB MOD GAIN** and **USB OUT LEVEL**. Other modes have their own source/gain menus, documented in [Yaesu's FT-710 CAT/menu reference](https://www.yaesu.com/Files/4CB893D7-1018-01AF-FA97E9E9AD48B50C/FT-710_CAT_OM_ENG_2306-C.pdf). Routing does not rewrite those menus. **Mic gain (%)** reads the ordinary microphone gain, not USB MOD GAIN.
4. Connect CAT, open **Audio routing**, refresh and select the four mappings above. Enable receive, then microphone. Tap **PTT on**, speak, then **PTT off**. Watch RF/ALC during TX and the audio bars. Restore your normal MOD SOURCE when you want to use that source again.

The read-only inventory check confirmed that host WDM-KS devices are discovered in an RDP session on this PC. Actual FT-710/phone audio has not been streamed or transmitted during development. Discovery/compilation do not confirm a specific driver's streaming behavior, redirection policy, audio latency or transmitted modulation.

### Received-audio spectrum and waterfall

Connect the radio, select its USB recording device and a listening destination in **Audio routing**, then enable **Receive audio**. Open **Audio scope**. Its receive button controls the same route; **Audio mappings** takes you back to the device selectors. For silent monitoring, set the receive routing **Level (%)** to 0; the scope still reads the captured samples before playback attenuation. Routes remain off at startup.

The horizontal axis is **audio frequency**, starting at 0 Hz. The selectable spans are 3, 6, 12 and 24 kHz, capped at half the negotiated source sample rate. The display processes a mono mix using a 4096-point Hann-window FFT with DC removal, aiming for up to 20 updates per second. At 48 kHz the bin spacing is about 11.7 Hz. Spectrum amplitudes are in **dBFS per FFT bin**, with a bin-centred full-scale sine calibrated to 0 dBFS. They are digital audio levels; CAT signal/RF meters retain their own units.

**Display gain** and **Display floor** adjust visibility and the waterfall colour scale. The axis labels account for display gain and continue to show underlying dBFS. These settings do not change radio gain or listening volume. The live trace uses power smoothing; the waterfall records each unsmoothed spectrum, with the newest row at the top and up to 300 frames retained. Its caption reports the time between the oldest and newest retained frames; pauses and scheduling delays can extend that interval. **Peak hold** adds an amber trace until cleared or toggled. **Freeze / Resume** pauses/resumes the visual history while audio routing continues. **Clear** clears both traces and history; a frozen display stays empty until resumed.

Analysis pauses during TX or unknown radio TX status. Device loss, radio disconnect and power off stop the receive route. Earlier waterfall history stays visible; frequency axes/history reset when a different capture stream starts or the receive mapping changes. Capture callbacks only copy into a bounded buffer; FFT and drawing run on the GUI thread, so analysis does not block playback. Buffer overflow/discontinuity discards incomplete analysis windows and waits for fresh samples.

This AF scope uses the radio's USB receive audio. Its width/content follow the radio's audio filtering and USB output settings. It does not show the FT-710's wideband RF spectrum or its built-in RF waterfall, and clicking this display does not tune a VFO.

### Connect to an existing rigctld instead

The **Hamlib / rigctld** connection profile is also available when a separate daemon owns the serial port. For example:

```powershell
.\vendor\hamlib-w64-4.7.2\bin\rigctld.exe -m 1049 -r COM5 -s 38400 -T 127.0.0.1 -t 4532
```

Then select **Hamlib / rigctld**, enter `127.0.0.1:4532`, and connect. The server must control an FT-710; this profile uses FT-710 labels, ranges and extension tokens. Dashboard CAT sharing is disabled in this profile. The direct raw CAT console is available only with a direct serial connection. Set daemon serial/flow settings when starting `rigctld`.

## Reading and changing settings

The general dashboard/settings refreshes at the chosen interval **after** each polling batch completes. Serial command latency adds to the interval. Meters have an independent schedule: a 250 ms delay between TX batches, 500 ms on RX. Reading a batch also takes serial time, so these are scheduling delays rather than guaranteed frame rates. Meter reads rotate under client load so one busy client cannot repeatedly postpone ALC/compression/current. Slow settings refresh in bounded rotation; the table shows each value's age. Receive-control and slow scans pause during TX to avoid temporarily switching the VFO while transmitting. Unsupported or mode-dependent commands show their Hamlib error. RF power and meters are radio-wide; other common controls follow **All settings → VFO**. Hamlib may briefly switch VFOs for those other controls while receiving.

### RF power and transmit meters

- **RF power (W)** sets the configured FT-710 output limit in whole watts, **5–100 W**, or **5–25 W in AM**. Click **Set**; the worker writes through Hamlib (`PCnnn;`) and then reads `PC;` back. The activity log records the accepted setting or a readback failure. Firmware/menu power limits can still restrict the value accepted by the radio.
- **RF OUTPUT** reads the separate live power meter (`RM5;`) using Hamlib's FT-710 watt calibration. Setting 75 W does not make the output meter display 75 W: it displays the radio's measured value. The note below it also shows the configured setting.
- **ALC** reads `RM4;`, **SWR** reads `RM6;`, **COMPRESSION** reads `RM3;`, **DRAIN CURRENT** reads `RM7;`, and **SUPPLY** reads `RM8;`. These do not require changing the radio's selected panel meter.
- ALC is displayed as **percent of Hamlib's ALC reference**, not watts or percent RF output. The 4.7.2 driver uses its default ALC calibration (raw 0 → 0, raw 64 → 1); values above that reference can exceed 100%. RF watts, SWR, compression, voltage and current use their respective backend calibration tables. The CAT readings are an indication from the radio, not an independently calibrated external wattmeter.
- RF output/SWR/ALC/compression/current require actual transmission. Both manual and CAT transmission are detected with `TX;`. Readings clear on return to RX or unknown TX status; readings older than two seconds are hidden as stale. Supply voltage can be read during RX too.
- The serial instance's advertised transmit power ranges are corrected to 100 W (25 W AM); Hamlib 4.7.2's original FT-710 descriptor inherited 200 W/75 W entries from another model. Power setting uses its actual 1 W step, not those copied range limits.

Hamlib clients continue to use normalized `RFPOWER` values **0.05–1.0** (e.g. `L VFOA RFPOWER 0.25` after VFO negotiation sets 25 W), with an AM maximum of **0.25**. `RFPOWER_METER_WATTS` returns measured watts, `ALC` returns the driver-relative reading, `SWR` returns the ratio, and `COMP_METER` returns dB. In plain text mode without VFO negotiation the equivalent power command is `L RFPOWER 0.25`. Power and meter commands use the current radio context even if a client supplies VFO B; they do not switch VFOs to read a global meter.

Demo radio readings are produced by Hamlib Dummy and do not describe an FT-710. Ordinary demo mode leaves the audio scope empty. The explicit `--scope-demo` option previews synthetic tones/noise and labels them **DEMO / GENERATED AUDIO**; it opens no audio devices.

Click **Edit selected** or double-click a writable row. Normalized Hamlib levels in the table use ranges such as 0–1; common gain controls convert these to percentages and RF power converts to watts. Mode passband 0 asks Hamlib for the normal filter width. Memory selection recalls a radio memory; the app does not rewrite memory contents. RIT/XIT offsets and their enable switches are separate settings.

**All settings means every control exposed by the installed FT-710 Hamlib driver, not every item in the radio's firmware menu.** The CAT interface and driver determine what can be read and changed. The RF scope uses the separate FT4222H USB-SPI interface; it is not a Hamlib meter or a screen mirror. For remaining documented controls, the Activity / CAT console can send a single Yaesu ASCII command through Hamlib's `rig_send_raw`:

- **Query / read reply** for commands that return a reply, for example `ID;`, `PC;` or `AS1;`.
- **Send / no reply** for commands that only set a value. Consult Yaesu's manual for exact values and side effects.
- Supply the full menu ID/format for `EX` commands. The manual button opens Yaesu's reference. Raw commands can alter menu items or transmit state, and are not validated as typed setting values.

PTT from either dashboard button is released by the app; PTT initiated by a microphone, another client or a raw CAT command is reported but is not owned by the dashboard buttons.

## Build

Requires Windows x64, MinGW-w64 `gcc`/`g++`, CMake 3.24+ and Ninja on PATH. The project uses Win32/common controls/GDI rather than requiring a GUI framework installation. Hamlib is dynamically linked; the build script downloads the official 4.7.2 Windows SDK and verifies its pinned SHA256. It also obtains SHA256-pinned PortAudio 19.7.0 and statically builds the WASAPI/WDM-KS drivers. To stage RF capture dependencies for local use, optionally provide the x64 FTDI runtime files: get `FTD2XX.dll` from [FTDI's D2XX driver package](https://ftdichip.com/drivers/d2xx-drivers/) and `LibFT4222.dll` from [FTDI's FT4222H library package](https://ftdichip.com/software-examples/ft4222h-software-examples/), then place them in `runtime/ftdi/`. Follow [FTDI DLLs for RF capture](#ftdi-dlls-for-rf-capture) for the x64 file locations and required names. The build copies required runtime DLLs beside the executable; third-party files are not tracked in Git. Packaging takes the threading and GCC runtime DLLs from the actual compiler, so newer compilers receive their required `clock_gettime64` implementation.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
.\dist\FT710Dashboard.exe
```

For offline operation:

```powershell
.\dist\FT710Dashboard.exe --demo
```

For a screenshot of the app connected to Hamlib Dummy (exits after capture):

```powershell
.\dist\FT710Dashboard.exe --capture C:\Temp\ft710-demo.png
```

Add `--capture-page audio`, `scope`, `settings` or `activity` to capture that tab. Capture always uses Hamlib Dummy, leaves audio routes off, disables CAT/web sharing and does not save preferences. To preview the new scope with generated samples:

```powershell
.\dist\FT710Dashboard.exe --scope-demo
```

For a generated-audio screenshot, add `--scope-demo --capture-page scope` to the capture command. Synthetic samples are only used when this explicit preview option is supplied; actual radio receive scopes use the selected capture stream.

For interactive GUI inspection using Hamlib Dummy without changing saved preferences or starting CAT sharing:

```powershell
.\dist\FT710Dashboard.exe --preview
```

Interactive `--preview` / `--scope-demo` also start the HTTPS web interface and HTTP certificate setup on 8888, using a temporary access code. The web page is labelled **DEMO**, and `--scope-demo` labels its spectrum **GENERATED AUDIO**. These modes open no physical radio/audio stream and do not save connection/audio preferences. `--capture` disables the web server entirely.

On smaller screens, use the window scrollbars to reach the full layout. Mouse-wheel tuning remains active over the frequency display and tuning strip; elsewhere, the wheel scrolls the window when needed.

For read-only audio diagnostics (no streams, microphone recording, CAT or PTT):

```powershell
.\dist\FT710Dashboard.exe --list-audio C:\Temp\ft710-audio.txt
```

The file includes whether RDP is active, device names, input/output direction, host/remote tags and stable mapping identities.

Manual build after obtaining the SDK:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix dist
```

An optional one-frame diagnostic utility is included under `tools/`. To build it, configure with `-DBUILD_SCOPE_CAPTURE_TOOL=ON`, then build target `FT710ScopeCapture`. Run it only after enabling **SCU-LAN10** on the radio and closing other applications using FT4222H Interface A. For example:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SCOPE_CAPTURE_TOOL=ON
cmake --build build --target FT710ScopeCapture
.\build\FT710ScopeCapture.exe --frames 1 --raw .\build\scope-frame.bin --csv .\build\spectrum.csv
```

This utility captures raw scope frames and decoded display-bin values for diagnosis. It does not connect to CAT-1/CAT-2 or change the radio's display settings.

`HAMLIB_ROOT` can override the SDK directory. Build and runtime Hamlib versions must match because this application reads backend descriptors. Native GUI source is in `src/dashboard.cpp`; connection, discovery, polling and commands are in `src/radio.cpp`. FT-710 FT4222H capture and CAT-2 scope setup are in `src/rf_scope.cpp`. CAT TCP sessions are in `src/cat_server.cpp`; protocol commands execute on the radio worker in `src/rigctl_protocol.cpp`. The HTTPS/WebSocket server is in `src/web_server.cpp`, with Schannel and certificate/profile helpers in `src/web_tls.cpp`. GUI command/state integration is in `src/dashboard_web.cpp` and audio buffers are in `src/browser_audio.cpp`. The HTML/CSS/JavaScript in `web/` is embedded into the executable by the resource compiler; no separate asset directory or Node.js runtime is needed for deployment. Socket threads never access the serial rig handle.

### Offline verification

GUI regression checks (no physical radio connection or audio streams):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI_CHECKS=ON
cmake --build build --target GuiChecks
.\build\GuiChecks.exe
```

These checks cover pending versus confirmed VFO/mode/frequency/gain, command failure recovery, delayed capability discovery, typed drafts, synchronized PTT buttons, the base dashboard pages, small-window scrolling, 100/125/150/200 percent font scales, and command acknowledgements using Hamlib Dummy. They do not exercise live FT-710 RF-scope capture or real radio audio.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_RADIO_CHECKS=ON
cmake --build build
.\build\RadioChecks.exe
```

The checks use the real FT-710 Hamlib driver against a loopback CAT fixture. They verify power writes/readback, RF/ALC/SWR/compression/voltage/current conversions, independent meter refresh, RX clearing, AM limits, correct power-range metadata, and two genuine Hamlib NET rigctl clients sharing power/meter access. They never open a physical COM port or transmit RF. Passing them does not verify the connected radio's firmware or actual meter calibration.

## Dependencies and references

Third-party source archives and binaries are intentionally kept out of GitHub. The build script downloads Hamlib and PortAudio into the ignored `vendor/` folder; FTDI DLLs are obtained separately and kept in ignored `runtime/ftdi/`. Local builds stage required runtime DLLs and upstream license notices in `dist/`; the published GitHub executable and ZIP omit these third-party files. Obtain upstream components from the links below. No application license has been selected yet.

Hamlib 4.7.2 is dynamically linked using its FT-710 backend. See the [official Hamlib 4.7.2 release](https://github.com/Hamlib/Hamlib/releases/tag/4.7.2), [rig API](https://hamlib.sourceforge.net/manuals/4.7/group__rig.html), and [rigctld protocol reference](https://hamlib.sourceforge.net/html/rigctld.1.html). PortAudio 19.7.0 is statically linked; its upstream [API documentation](https://portaudio.com/docs/v19-doxydocs/api_overview.html) describes device and audio interfaces. `src/portaudio_wdmks.c` adapts the upstream WDM-KS driver to expose a stable device identity.

- [Hamlib FT-710 backend](https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/ft710.c)
- [FTDI D2XX Windows desktop driver package — FTD2XX.dll](https://ftdichip.com/drivers/d2xx-drivers/)
- [FTDI FT4222H Windows library package — LibFT4222.dll](https://ftdichip.com/software-examples/ft4222h-software-examples/)
- [Yaesu FT-710 CAT reference](https://www.yaesu.com/Files/4CB893D7-1018-01AF-FA97E9E9AD48B50C/FT-710_CAT_OM_ENG_2306-C.pdf)
- [Yaesu FT-710 downloads and USB driver](https://yaesu.com/product-detail.aspx?CatName=HF+Transceivers%2FAmplifiers&Model=FT-710)

Physical FT-710 operation needs checking with your connected radio. A successful build or Dummy connection does not verify the USB driver, CAT configuration, firmware behavior, transmitted meter calibration, or standby wake-up on your hardware.

## Reliability and phone audio in 2.0.5

PTT-off failures are retried by the radio worker, including releases for disconnected Hamlib clients. A pending release prevents new dashboard or CAT PTT-on requests. Scope CAT-2 writes/readback and device refresh/start/stop run in background queues so the GUI and browser controls remain available.

Browser station settings show **Saving**, **Saved**, or a failure with **Retry save**. Save responses acknowledge individual edit revisions, preserving newer drafts and limiting updates to the fields that browser changed. An INI write failure is reported. The RF span editor is a requested setting; received frames supply the confirmed display span.

Drag/wheel tuning sends accumulated changes at a bounded rate during the gesture. RF waterfall history clears when its center/span or capture generation changes. Stopped/stalled capture is labelled as frozen history, and tuning requires a fresh frame. Waterfall image buffers are reused and updated incrementally. Browsers subscribe to visible scopes; live state uses incremental updates and activity-log append/trim messages.

On devices supporting the Audio Session API, **Keep a stable microphone/playback session** selects play-and-record before listening/capture and keeps that session while listening. This avoids an unnecessary session transition when arming the mic; the phone still determines its speaker/headset route and volume. This is not a guarantee that every Safari/iOS output-volume issue is resolved. The option can be disabled. Interrupted audio has **Resume audio**; a lost/hidden/disconnected mic is disarmed and its owned PTT is released. A page restored with browser Back/Forward reconnects without automatically re-keying. Local mic arming is tracked per browser, so one browser disarming cannot tear down another browser's active mic route.

The saved **Unattended browser audio** option in the native Audio routing page, or its equivalent browser option, allows receive audio to continue when Windows locks or RDP disconnects. PTT is still released on that session change. New browser PTT remains an explicit action. The option defaults to off. The browser **Connection & audio diagnostics** panel reports CAT contact age, RF age, command acknowledgement time, audio-context/session state and drawing time.

## Check dependencies and package a release

Before launching a downloaded executable, run the included helper from its folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\check-runtime.ps1
```

It checks x64 runtime discovery, Hamlib 4.7.2 loading/version and the compiler threading runtime's `clock_gettime64` export. It lists optional FTDI runtime files separately and links to upstream downloads. It does not install dependencies. For a local staged build use `scripts/check-runtime.ps1 -ApplicationDirectory dist`. The default Windows loader must be able to find the DLLs before the executable can start.

Create the application-only release after building:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

The script uses an explicit allowlist, verifies the ZIP contents, and writes a SHA-256 checksum under ignored `artifacts/releases/`. It includes only the executable, README and runtime-check helper; Hamlib, FTDI and other third-party runtime DLLs are excluded. Local `dist/` staging retains separately obtained dependencies for running the app.

Offline regression checks are registered with CTest:

```powershell
cmake -S . -B build -DBUILD_GUI_CHECKS=ON -DBUILD_RADIO_CHECKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Node.js is needed only for the browser regression tests, not for deployment. The checks include PTT release fault injection, all six native pages, delayed preference acknowledgements, continuous tuning, RF-grid transitions, browser mic ownership and native-server/browser state-delta round trips. The Windows GitHub Actions workflow builds without FTDI DLLs, runs the offline checks, and uploads only the validated application release archive. Hardware/phone acceptance checks still need the actual FT-710 and browser.
