# Security & privacy

## What this device does with your data

- WiFi credentials are stored **only on the device** (NVS partition) and are
  never transmitted anywhere except to your WiFi router. They are not in the
  repo, not in logs (the portal AP password is printed once on the serial
  console, which requires physical access).
- Print jobs stay on your LAN: PC → ESP → printer. Nothing leaves your network.

## Hardening built in

- **Provisioning portal AP is WPA2**, not open. Password = `cfg` + last 3 MAC
  bytes (device-specific, shown on the serial log at portal start).
- **Print/USBIP ports (9100, 515, 3240) only accept clients from the ESP's own
  subnet** (`APP_RESTRICT_LOCAL_SUBNET=y`, default). Disable only on trusted
  networks.
- Bounded buffers everywhere (HTTP request ≤2 KB, DNS ≤512 B with room for the
  answer, USB/IP payloads capped at 16 KB with stream-safe draining).
- USB/IP: one client at a time; unknown opcodes close the connection.
- No dynamic code execution paths; all parsing is length-checked.

## Remaining risks & recommendations

- The portal HTTP page is plain HTTP (fine on the isolated portal AP; TLS needs
  certificates and is overkill for a LAN provisioning step).
- NVS stores the WiFi password unencrypted unless you enable ESP-IDF flash
  encryption (`CONFIG_SECURE_FLASH_ENC_ENABLE`) — recommended if the device
  may be physically accessible to others.
- Raw printing (9100) is unauthenticated by design (that's how printers work);
  keep the subnet restriction on and firewall the ports on your router if the
  LAN is shared.
- USB/IP gives the client full USB control of the printer while attached —
  only attach from machines you trust.
