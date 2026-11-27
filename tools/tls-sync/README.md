# Automated HTTPS Certificate Sync (Router → AP)

Uses a **least-privilege forced-command SSH key** so your upstream ACME host (e.g. OpenWrt router) can push renewed wildcard certificates to the HG6371F AP without granting shell access and without unnecessary NAND flash writes.

## 1. Install receiver on AP

```sh
install -m 755 recv-tls-cert.sh /opt/upt/apps/ap-panel/bin/recv-tls-cert.sh
```

## 2. Generate a dedicated SSH key on the router

On OpenWrt (Dropbear):
```sh
dropbearkey -t ed25519 -f /etc/dropbear/ap_cert_push_ed25519
dropbearkey -y -f /etc/dropbear/ap_cert_push_ed25519 | grep '^ssh-ed25519 '
```

Or on standard Linux (OpenSSH):
```sh
ssh-keygen -t ed25519 -N '' -f /etc/ssh/ap_cert_push_ed25519
cat /etc/ssh/ap_cert_push_ed25519.pub
```

## 3. Lock the key in AP `authorized_keys`

Append this single line to `/etc/ssh/authorized_keys_root` (and `/opt/upt/apps/ap-panel/root/.ssh/authorized_keys`):

```text
command="/opt/upt/apps/ap-panel/bin/recv-tls-cert.sh",no-port-forwarding,no-X11-forwarding,no-agent-forwarding,no-pty ssh-ed25519 AAAA...your-key...
```

## 4. Schedule daily push on the router

Pin the AP's host key in `/root/.ssh/known_hosts`, test once with `--force`, then add a cron entry (e.g., 10 minutes after your ACME renewal):

```cron
10 0 * * * CERT=/path/to/fullchain.crt KEY=/path/to/privkey.key AP_HOST=192.168.1.2 /usr/sbin/push-ap-tls.sh 2>&1 | logger -t ap-tls-push
```

`ap-panel-srv` watches `/opt/upt/apps/ap-panel/etc/tls.{crt,key}` by `mtime` and hot-reloads renewed certificates with zero downtime.
