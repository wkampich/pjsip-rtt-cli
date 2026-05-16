# PJSIP Real-Time Text (RTT) CLI Test Client

A terminal-based SIP client designed specifically for evaluating, testing, and debugging **RFC 4103 Real-Time Text (RTT)** and **RFC 2198 RTP Payload for Redundant Audio Data (RED)** streams. This utility pairs the **PJSIP (`pjsua`)** framework with an interactive, multi-window **Ncurses** user interface to provide character-by-character text transmission alongside real-time SIP core logging and headless audio file injection.

> **Critical Requirement: Custom `txt_stream.c` Patch**
>
> Out-of-the-box PJSIP (`pjproject`) has limitations regarding RFC 4103 / RFC 2198 text streaming and redundancy handling. To make this test client function correctly, **you must use the customized `txt_stream.c` file included in this repository**. 
>
> * **If using Docker:** The `Dockerfile` handles this automatically by injecting the file during the build process.
> * **If compiling natively:** You must explicitly overwrite PJSIP's original media file with this version before configuring and compiling (see Step 3 in Native Compilation).

---

## Features

* **Real-Time Character Streaming**: Supports redundant RTT (RFC 4103 / RFC 2198) transmission over SIP/RTP text media channels.
* **Split-Screen Terminal UI**: Uses thread-safe Ncurses windows to divide application responsibilities:
  * **Chat History**: Displays committed lines from both conversational parties.
  * **Remote View**: Visualizes remote typing input in real time ("Remote is typing...").
  * **Input Buffer**: Captures your current keystrokes before they are committed to history.
* **Headless Audio Bridging**: Integrates PJSIP's null sound device to loop 16-bit PCM `.wav` files directly into the active call without requiring a physical soundcard.
* **Dynamic Window Resizing**: Catches `KEY_RESIZE` events, dynamically recalculating terminal panel constraints without interrupting active streams.

---

## UI Layout & Architecture

The client dynamically splits your active terminal workspace into a managed panel grid:

```text
+-----------------------------------------------------------+
|                      CHAT HISTORY                         |
|                                                           |
+-----------------------------------------------------------+
|                   SYSTEM / PJSIP LOGS                     |
|                                                           |
+-----------------------------------------------------------+
| [ Remote is typing... ]                                   |
| -> Live uncommitted character buffer from the remote peer |
+-----------------------------------------------------------+
| [ You (Press ESC to quit) ]                               |
| -> Local keystroke buffer space                           |
+-----------------------------------------------------------+

```

### Thread-Safe Synchronization

Because PJSIP callback events (such as receiving downstream text or capturing system logs) execute on separate worker threads from the Ncurses main input loop, all UI redraw operations are strictly protected via an internal POSIX mutex wrapper (`ui_mutex`) to eliminate interface corruption and race conditions.

---

## Building and Deployment

### Method 1: Docker (Recommended)

The repository includes a multi-stage configuration that cleanly separates the heavy compilation toolchains from the final execution image, yielding an optimized runtime container footprint (~20MB).

1. **Build the Docker Image**:
```bash
docker build -t localhost/pjsip-rtt-cli .

```


2. **Run the Interactive Client**:
Because the application relies on an interactive Ncurses interface, you **must** pass the `-it` flags to attach a pseudo-TTY. Using host networking is highly recommended for SIP testing to bypass complex NAT traversal configurations:
```bash
docker run -it --rm --name rtt-sandbox --net=host -v ${PWD}:/home/ localhost/pjsip-rtt-cli

```


*Note: `-v ${PWD}:/home/` maps the working directory, which may contain your WAV file.*

### Method 2: Native Manual Compilation (Alpine Linux Base)

To compile the application directly on an Alpine host, execute the following workflow:

1. **Install toolchain and library dependencies**:
```bash
apk add --no-cache gcc g++ make linux-headers pkgconf ncurses-dev openssl-dev util-linux-dev curl tar opus-dev speex-dev speexdsp-dev libsrtp-dev

```


2. **Download and unpack PJProject 2.17**:
```bash
curl -L [https://github.com/pjsip/pjproject/archive/refs/tags/2.17.tar.gz](https://github.com/pjsip/pjproject/archive/refs/tags/2.17.tar.gz) | tar xz --strip-components=1

```


3. **Apply the repository patch**:
Copy the modified text stream implementation over the stock PJSIP file:
```bash
cp txt_stream.c pjmedia/src/pjmedia/txt_stream.c

```


4. **Configure and build the PJSIP core library** with hardware sound and video tracks disabled:
```bash
./configure CFLAGS="-O2" --disable-sound --disable-video --disable-alsa --disable-oss
make dep && make && make install

```


5. **Compile the CLI binary**:
```bash
gcc -o pjsip_cli pjsua_rtt_cli.c $(pkg-config --cflags --libs --static libpjproject) -lncurses -lssl -lcrypto -luuid -lm -lpthread -lopus -lspeex -lspeexdsp -lsrtp2

```



---

## Command Line Reference

| Parameter | Example | Description | Default |
| --- | --- | --- | --- |
| `--port` | `5061` | Binds the local SIP signaling transport to a specific TCP port. | `5060` |
| `--call` | `sip:agent@10.0.0.5:5060` | Runs the client as a UAC and immediately dials the target destination over TCP. | *Listen Mode* |
| `--public-ip` | `192.168.1.50` | Explicitly overrides network interface binding and SDP media connection tracking. | `127.0.0.1` |
| `--user` | `test_client` | Modifies the local display identity string for SIP headers. | *Blank* |
| `--playback` | `/home/audio.wav` | Path to a **16-bit Mono PCM WAV file** to automatically loop into the call bridge. | *None* |
| `--r` | *Flag* | Forces the engine to commit staging buffers using only Carriage Return (`\r`). | `\r\n` |
| `--n` | *Flag* | Forces the engine to commit staging buffers using only New Line (`\n`). | `\r\n` |

---

## Practical Verification Examples

### 1. Act as a User Agent Server (UAS / Listener Mode)

To host a headless testing endpoint on port 5060 that automatically answers incoming text/audio calls and pipes active logging directly to the UI panel:

```bash
./pjsip_cli --port 5060 --public-ip 127.0.0.1 --user bob

```

### 2. Act as a User Agent Client (UAC / Dialer Mode) with Audio Injection

To initiate an outbound call to a testing target by streaming a pre-recorded audio (`alert.wav`):

```bash
./pjsip_cli --port 5070 --public-ip 127.0.0.1 --user alice --call sip:bob@127.0.0.1 --playback /home/alert.wav

```

---

## Runtime Interface Controls

* **`Alphanumeric Keys / Symbols`**: Immediately transmitted downstream across the RTT transport stack character-by-character.
* **`Backspace / Delete`**: Transmits control deletion characters (`\b`) while rolling back local visualization spaces safely.
* **`Enter / Return`**: Flushes the local staging buffer to the main chat window tracker and transmits structural line terminators.
* **`ESC`**: Shuts down the active Ncurses window wrapper, cleanly destroys active PJSIP session contexts, frees media routing slots, and exits.
