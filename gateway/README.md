# AI gateway

The gateway is a small, read-only bridge between the printer network and the
CYD. It reads Moonraker telemetry, creates an automatic model preview from the
active G-code, and can optionally ask an AI provider for a structured
print-quality assessment. It has no endpoint that sends G-code or changes
printer state.

For each new print, `/v1/model-preview` prefers an embedded slicer thumbnail.
When none exists, it renders visible extrusion paths. The result is cached as a
220x145 RGB565 image, so the ESP downloads it once per filename without needing
new firmware.

The same service hosts ESP firmware updates. `/v1/firmware/manifest` reports
the published semantic version, size, and SHA-256 digest, while
`/v1/firmware/image` streams the binary. Firmware files are published
atomically by `tools/publish_ota.sh` from the repository root.

## Install on the CM4

Do this while the printer is idle. The Python environment uses approximately
70 MB with the current dependencies.

```sh
cd ~/cyd-printer-display/gateway
python3 -m venv .venv
.venv/bin/pip install .
cp .env.example .env
chmod 600 .env
```

Edit `.env`. Start with `AI_PROVIDER=mock` to verify the complete path without
using credits. For cloud analysis, set `AI_PROVIDER=openai` and put the API key
in `OPENAI_API_KEY`. The key stays on the CM4 and must never be copied into the
ESP32 firmware or committed to Git.

The defaults expect Moonraker on `127.0.0.1:7125` and the first Crowsnest camera
on `127.0.0.1:8080`. Verify both before enabling cloud analysis:

```sh
curl http://127.0.0.1:7125/server/info
curl -o /tmp/printer-snapshot.jpg 'http://127.0.0.1:8080/?action=snapshot'
```

Run it interactively for the first test:

```sh
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8787
curl http://127.0.0.1:8787/health
curl -o /tmp/model-preview.rgb http://127.0.0.1:8787/v1/model-preview
curl http://127.0.0.1:8787/v1/firmware/manifest
curl -X POST http://127.0.0.1:8787/v1/analyze
```

For automatic startup, check the username and paths in
`cyd-printer-ai.service`, then install it:

```sh
sudo cp cyd-printer-ai.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cyd-printer-ai.service
```

## Security and limits

- Set the same random `GATEWAY_TOKEN` in `.env` and `AI_GATEWAY_TOKEN` in the
  private ESP32 `include/config.h` when the printer LAN is not fully trusted.
- Analysis is rate-limited to once per minute by default.
- Camera responses larger than 5 MB are rejected.
- Active G-code downloads are read-only and limited by `MAX_GCODE_BYTES`
  (100 MB by default).
- Generated previews are cached until the active filename changes.
- OTA firmware is read from `FIRMWARE_DIR`; binaries and temporary publishing
  files are excluded from Git.
- OTA endpoints use the same optional `X-Display-Token` authorization as the
  display and AI endpoints.
- OpenAI requests use structured output, low-detail images, and `store=False`.
- The ESP32 uses the asynchronous `/v1/analyze/start` route and remains
  responsive while analysis runs.

## Future local AI machine

The analyzer is isolated behind a small provider interface. An
OpenAI-compatible Responses endpoint can be selected with `AI_BASE_URL`, so a
future local server can replace the cloud without changing the ESP32 protocol.
