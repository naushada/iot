-- cloud.* schema (L21/D1).
--
-- Multi-tenant cloud server state.  Endpoints are stored as a
-- single JSON array (like wifi.networks) so the schema can validate
-- the key regardless of how many devices are provisioned.
--
-- cloud.endpoints = [
--   {
--     "endpoint":      "urn:dev:gateway-42",
--     "state":         "online",
--     "tun_ip":        "10.9.0.12",
--     "proxy_port":    5001,
--     "registered":    true,
--     "last_seen_unix": 1718123456
--   }
-- ]
--
-- Install at /etc/iot/ds-schemas/cloud.lua (ds-server auto-loads).

return {
  namespace = "cloud",
  keys = {
    -- JSON array of provisioned endpoints (read by cloud UI,
    -- written by cloud server daemon).
    ["cloud.endpoints"] = {
      type    = "string",
      default = "[]",
    },

    -- Multi-tenant registry (apps/docs/tdd-multi-tenant-cloud.md, P1). JSON
    -- array of tenants, each: { id, name, vpn.subnet, proxy.port.start/end,
    -- dm.uri, status }. A deployment with no tenants behaves as today: every
    -- untagged endpoint/credential/user belongs to the implicit "default"
    -- tenant. Operators add tenants here (later via the platform-operator
    -- console) to host multiple organizations on one cloud-iot.
    ["cloud.tenants"] = {
      type    = "string",
      default = "[]",
    },

    -- LwM2M registration status. Written by lwm2m-dm (SOLE writer) from its
    -- ClientRegistry whenever a device registers / updates / deregisters or
    -- a lifetime lapses; read by iot-cloudd to merge online/offline +
    -- last_seen into cloud.endpoints (which it owns alongside tun_ip /
    -- proxy_port). Separate key avoids a two-writer clobber. JSON array:
    -- [{ endpoint, registered, last_seen_unix }].
    ["cloud.lwm2m.registrations"] = {
      type    = "string",
      default = "[]",
    },

    -- Live vehicle telemetry per endpoint (CAN/OBD-II), JSON array. lwm2m-dm
    -- observes the device's Vehicle object (/33000/0/*) + Location Object 6 and
    -- writes the latest values here (VOLATILE — latest-wins, the cloud-ui map
    -- reads it live). NOT persisted; the 60-day history lives in cloud Mongo.
    -- See apps/docs/tdd-vehicle-telemetry.md §3b.
    --   [{ "endpoint":"100000abcd", "ts":1718000000,
    --      "lat":"12.97","lon":"77.59","speed":"62","rpm":"2150",
    --      "coolant":"89","throttle":"18","load":"34","fuel":"71",
    --      "iat":"31","maf":"5.2","dtc":"[]","link":"up" }]
    ["cloud.vehicle.telemetry"] = {
      type    = "string",
      default = "[]",
    },

    -- Telemetry ingest inbox (§3b) — lwm2m-dm appends each LwM2M-Send batch
    -- (SenML decoded → JSON array of points) here; iot-httpd (IOT_HTTPD_MONGO)
    -- drains it into the cloud `telemetry` Mongo collection and clears it.
    -- VOLATILE handoff, sole writer = lwm2m-dm. Empty until the transport lands.
    ["cloud.telemetry.inbox"] = {
      type    = "string",
      default = "[]",
    },

    -- Bootstrap Server (L21/D3).  iot-cloudd reads these keys to
    -- configure the CoAP /bs endpoint and to build the Security +
    -- Server object TLV payloads that are pushed to devices during
    -- bootstrap.
    ["cloud.bs.endpoint"] = {
        access  = "Admin",
      type    = "string",
      default = "urn:dev:gateway-",
    },
    ["cloud.bs.uri"] = {
        access  = "Admin",
      type    = "string",
      default = "coaps://0.0.0.0:5684",
    },
    -- Security mode for bootstrapping devices.  "PSK" (default) uses a
    -- pre-shared key; "None" skips DTLS handshake (dev only).
    ["cloud.bs.security.mode"] = {
        access  = "Admin",
      type    = "string",
      default = "PSK",
    },
    -- PSK identity written into the device Security Object at bootstrap.
    ["cloud.bs.psk.id"] = {
        access  = "Admin",
      type    = "string",
      default = "iot-client",
    },
    -- PSK secret (opaque — hex or raw).  Written into the device
    -- Security Object RID 5 at bootstrap.
    ["cloud.bs.psk.key"] = {
        access  = "Admin",
      type    = "opaque",
      default = "",
    },
    -- Device Management server URI.  Written into the device Server
    -- Object (OID 1, RID 0) at bootstrap so the device knows where to
    -- register after bootstrapping.
    ["cloud.dm.uri"] = {
        access  = "Admin",
      type    = "string",
      default = "coaps://0.0.0.0:5683",
    },
    -- Default registration lifetime (seconds) pushed to devices at bootstrap
    -- (Server Object OID 1, RID 1). Devices may request a different value.
    --
    -- This DOUBLES AS THE NAT KEEPALIVE. The device sends a registration Update
    -- at (lifetime - 30s margin), and that Update is what keeps the gateway's
    -- UDP conntrack mapping for the DIRECT LwM2M/DTLS flow (:5683) alive so the
    -- cloud can reach the device (OTA push, server reads). Aligned to netfilter
    -- conntrack defaults:
    --   UDP unreplied ......... 30 s   (nf_conntrack_udp_timeout)
    --   UDP assured/stream ... 120 s   (nf_conntrack_udp_timeout_stream)
    --   TCP established .... 432000 s   (the VPN's own plane — openvpn's 10s
    --                                    ping keeps that alive, not this key)
    -- default 90 → Update every 60 s ≈ half the 120 s assured timeout (classic
    -- keepalive = timeout/2). Lower to 60 (→30 s Update) for aggressive CGNAT
    -- (30-60 s UDP). Raise toward 86400 ONLY if LwM2M is routed over the VPN
    -- tunnel (openvpn's ping then handles NAT — see apps/cloud/CLAUDE.md
    -- "Registration lifetime & NAT keepalive").
    ["cloud.dm.lifetime"] = {
        access  = "Admin",
        type    = "integer",
        default = 90,
        min     = 0,
        max     = 2592000,
    },
    -- Default binding mode pushed to devices at bootstrap
    -- (Server Object OID 1, RID 7).  "U" = UDP (standard).
    ["cloud.dm.binding"] = {
        access  = "Admin",
      type    = "string",
      default = "U",
    },
    -- DM-level PSK for post-bootstrap device communication.
    -- Separate from cloud.bs.psk.* (used only during bootstrapping).
    ["cloud.dm.psk.id"] = {
        access  = "Admin",
      type    = "string",
      default = "iot-dm-client",
    },
    ["cloud.dm.psk.key"] = {
        access  = "Admin",
      type    = "opaque",
      default = "",
    },
    -- LwM2M version reported by devices / expected by the DM server.
    ["cloud.dm.lwm2m.version"] = {
        access  = "Admin",
      type    = "string",
      default = "1.1",
    },

    -- Provision request: iot-httpd writes the endpoint name here;
    -- iot-cloudd watches this key and calls BootstrapProvisioner.
    ["cloud.provision.request"] = {
        access  = "Admin",
      type    = "string",
      default = "",
    },
    -- Target tenant for the next provision (multi-tenant, Option B). The device
    -- is tenant-agnostic — it bootstraps with its bare serial — so the operator
    -- console sets this alongside cloud.provision.request to tag the new cred
    -- row + registry row. Empty ⇒ default tenant (single-tenant legacy).
    -- iot-cloudd clears it after each provision.
    ["cloud.provision.tenant"] = {
        access  = "Admin",
      type    = "string",
      default = "",
    },
    -- Deprovision request: iot-httpd writes the endpoint name here;
    -- iot-cloudd watches this key and calls deprovision().
    ["cloud.deprovision.request"] = {
        access  = "Admin",
      type    = "string",
      default = "",
    },
    -- Operator audit log (multi-tenant P5c). A capped, newest-first JSON array
    -- of {ts,actor,tenant,action,target} records. iot-httpd appends an entry in
    -- the db/set handler for tenant + device lifecycle actions (sole writer);
    -- the cloud-ui Audit page reads it. Admin-only.
    ["cloud.audit.log"] = {
        access  = "Admin",
      type    = "string",
      default = "[]",
    },

    -- ── OTA software update (LwM2M Firmware Update Object 5) ──────────
    -- Operator-curated catalogue of artifacts in the cloud firmware feed
    -- (served by iot-httpd at /firmware/...). An entry's ipk_url may point at
    -- a single .ipk OR a .tar.gz BUNDLE of every iot-*.ipk (built by
    -- iot-bundle.bb) — the device's iot-ota-stage extracts a bundle and
    -- opkg-installs all packages in one shot. JSON:
    --   [ { "pkg":"iot", "version":"0.2.0", "arch":"aarch64",
    --       "ipk_url":"/firmware/iot_0.2.0_aarch64.ipk", "sha256":"<hex>" },
    --     { "pkg":"iot-bundle", "version":"1.1.0", "arch":"raspberrypi3-64",
    --       "ipk_url":"/firmware/iot-bundle-1.1.0-raspberrypi3-64.tar.gz",
    --       "sha256":"<hex>" } ]
    -- PREFER a RELATIVE ipk_url ("/firmware/..."): iot-cloudd resolves it
    -- against the cloud's PUBLIC address (cloud.firmware.base.url, else the
    -- host of cloud.dm.uri), so the device downloads DIRECT over WAN and OTA
    -- never depends on the VPN tunnel. An absolute "http(s)://..." ipk_url is
    -- still honoured verbatim (CDN, or a deliberate tunnel IP).
    ["cloud.firmware.manifest"] = {
        access  = "Admin",
        type    = "string",
        default = "[]",
    },
    -- Public base URL the device downloads firmware from when a manifest
    -- ipk_url is relative ("/firmware/..."). Empty → iot-cloudd derives
    -- http://<host-of-cloud.dm.uri> (the same public address the device
    -- already reaches for DTLS — NOT the VPN tunnel IP). Set this to force
    -- HTTPS or a CDN, e.g. "https://ota.example.com".
    ["cloud.firmware.base.url"] = {
        access  = "Admin",
        type    = "string",
        default = "",
    },
    -- Update request from cloud-ui (carrier — iot-cloudd clears to ""):
    --   { "serials":["100000abcd",...], "pkg":"iot", "version":"0.2.0",
    --     "url":"/firmware/iot_0.2.0_aarch64.ipk", "sha256":"<hex>" }
    ["cloud.update.request"] = {
        access  = "Admin",
        type    = "string",
        default = "",
    },
    -- Cancel a stuck OTA campaign: cloud-ui writes the endpoint serial here;
    -- iot-cloudd drops that endpoint's cloud.update.pending job (stops the
    -- lwm2m-dm Object-5 re-push) and marks its cloud.update.status row cancelled
    -- (result 11), then clears this key.
    ["cloud.update.abort"] = {
        access  = "Admin",
        type    = "string",
        default = "",
    },
    -- Validated per-endpoint update jobs: written by iot-cloudd, consumed
    -- by the lwm2m-dm push tick. cloud-svc-only (mirrors the credentials
    -- two-writer-avoidance pattern). JSON array of
    --   { "endpoint":"100000abcd", "url":"...", "sha256":"...", "version":"..." }
    ["cloud.update.pending"] = {
        access    = "Admin",
        type      = "string",
        default   = "[]",
        write_acl = {"gid:cloud-svc"},
        read_acl  = {"gid:cloud-svc"},
    },
    -- Per-endpoint update status (Object-5 readback), written by lwm2m-dm,
    -- read by cloud-ui. JSON array of
    --   { "serial":"...", "state":<0..3>, "result":<0..9>,
    --     "version":"0.2.0", "ts":<cid> }
    ["cloud.update.status"] = {
        access  = "Admin",
        type    = "string",
        default = "[]",
    },
    -- Monotonic OTA campaign counter. iot-cloudd bumps it on every push and
    -- stamps each cloud.update.pending job with the new value as "cid";
    -- lwm2m-dm pushes Object-5 at-most-once per (endpoint, cid), so a re-push
    -- (fresh cid) re-sends even the SAME version. Persisted so the id is unique
    -- across restarts and same-second double-pushes.
    ["cloud.update.seq"] = {
        access  = "Admin",
        type    = "integer",
        default = 0,
    },

    -- ── PSK provisioning (serial-derived endpoint + per-endpoint PSK) ──
    -- See apps/docs/tdd-psk-provisioning.md.
    --
    -- Per-endpoint credential array. The BS/DM servers load this live
    -- (watch + add_credential) instead of the shared env-var PSK. The
    -- engineer pastes (serial, BS PSK) from device-ui; the backend forms
    -- the identity rpi<serial>@cloud.local and mints the DM PSK. JSON:
    --   [ { "serial":"<raw>", "identity":"rpi<raw>@cloud.local",
    --       "bs.psk.key":"<hex>", "dm.psk.id":"rpi<raw>@cloud.local",
    --       "dm.psk.key":"<hex>" }, ... ]
    -- Write-only / no ds-cli read (mirrors the device rule): only the
    -- cloud-svc server processes may read; reveal via cloud.dev.mode.
    ["cloud.endpoint.credentials"] = {
        access    = "Admin",
        type      = "string",
        default   = "[]",
        write_acl = {"gid:cloud-svc"},
        read_acl  = {"gid:cloud-svc"},
    },
    -- Zero-touch bootstrap (HKDF tier) — apps/docs/tdd-bs-hkdf-zerotouch.md.
    -- The HKDF master that lets the BS server re-derive any device's BS PSK
    -- from its serial, so no per-device row has to be pre-created here. Stored
    -- AES-256-GCM-wrapped (KMS envelope): base64(nonce || ct || tag) under a
    -- KEK delivered to iot-lwm2m-server out-of-band (systemd LoadCredential /
    -- env IOT_BS_MASTER_KEK) — never via the data-store. The server unwraps
    -- once at startup and holds the master in memory only. Empty default ⇒
    -- HKDF tier disabled (commissioned per-device tier still works). The build
    -- bakes a wrapped master here from a gitignored bs_master.lua (cloud image
    -- only). Write-only to ds-cli, like the per-device PSK keys.
    ["cloud.bs.master.key"] = {
        access    = "Admin",
        type      = "opaque",
        default   = "",
        write_acl = {"gid:cloud-svc"},
        read_acl  = {"gid:cloud-svc"},
    },
    -- Provision carrier: the engineer pastes the device-generated BS PSK
    -- here, then sets cloud.provision.request = serial (the trigger).
    -- iot-cloudd reads this, mints the DM PSK, upserts the credential
    -- array, then clears this back to "". Write-only (cloud-svc) — the
    -- cloud-httpd write succeeds under cloud.dev.mode (ACL bypass).
    ["cloud.provision.bs.psk"] = {
        access    = "Admin",
        type      = "string",
        default   = "",
        write_acl = {"gid:cloud-svc"},
        read_acl  = {"gid:cloud-svc"},
    },
    -- Commissioning flag: while true the ds-server bypasses the PSK ACLs
    -- so an operator can inspect/edit credentials on the cloud.
    ["cloud.dev.mode"] = {
        access    = "Admin",
        type      = "boolean",
        default   = true,   -- simple dev commissioning out of the box; set false to lock down
        write_acl = {"gid:cloud-svc"},
    },

    -- Device web UI port reached over the VPN. iot-cloudd installs a
    -- per-device nftables DNAT (cloud:<proxy_port> → <tun_ip>:ui_port over
    -- tun0) so an operator can reach a device's local UI by hitting the
    -- cloud on the device's assigned proxy port. Global (same value for all
    -- devices); read live by iot-cloudd. DNAT target = the port the device's
    -- iot-httpd binds: http-port=8080, published host:8081 -> container:8080,
    -- and over the VPN the cloud DNATs straight to the container's 8080 — so
    -- the default is 8080 (override in ds if a device serves on another port).
    ["cloud.proxy.device.ui.port"] = {
      type    = "integer",
      default = 8080,
      min     = 1,
      max     = 65535,
    },

    -- VPN subnet for tunnel IP allocation.
    ["cloud.vpn.subnet"] = {
      type    = "string",
      default = "10.9.0.0/24",
    },

    -- Multi-tenant VPN pool (apps/docs/tdd-multi-tenant-cloud.md, P4): iot-cloudd
    -- carves a per-tenant /24 from this pool for every cloud.tenants entry that
    -- lacks a vpn.subnet. Kept clear of the legacy default-tenant 10.9.0.0/24.
    ["cloud.vpn.tenant.pool"] = {
      type    = "string",
      default = "10.9.16.0/20",
    },

    -- Multi-tenant VPN enable knob (P3c). EMPTY by default → no
    -- client-config-dir line in the OpenVPN config, no CCD files written →
    -- single-tenant deployments are byte-identical. To turn on per-tenant static
    -- IPs the operator sets this to a directory (e.g. /etc/iot/vpn/ccd) AND
    -- widens cloud.vpn.subnet to a /16 covering the default /24 + the tenant pool
    -- (10.9.0.0/16); iot-cloudd then pins each tenant device into its tenant /24
    -- via a CCD file and allocates its tunnel IP from that /24.
    ["cloud.vpn.ccd.dir"] = {
        access  = "Admin",
      type    = "string",
      default = "",
    },

    -- JSON array of device serials with a live VPN tunnel right now, written
    -- by iot-cloudd from the openvpn management interface. lwm2m-dm reads it to
    -- stop pushing the cert once the device's tunnel is up.
    ["cloud.vpn.connected"] = {
      type    = "string",
      default = "[]",
    },

    -- OpenVPN server listen socket (iot-cloudd spawns openvpn(8) here).
    ["cloud.vpn.listen.port"] = {
      type    = "integer",
      default = 1194,
      min     = 1,
      max     = 65535,
    },
    ["cloud.vpn.proto"] = {
      type    = "string",
      default = "tcp-server",   -- "tcp-server" | "udp"
    },
    ["cloud.vpn.cipher"] = {
      type    = "string",
      default = "AES-256-GCM",
    },
    ["cloud.vpn.dev"] = {
      type    = "string",
      default = "tun",          -- "tun" | "tap"
    },
    ["cloud.vpn.mgmt.port"] = {
      type    = "integer",
      default = 7506,
      min     = 1,
      max     = 65535,
    },
    ["cloud.vpn.verb"] = {
      type    = "integer",
      default = 3,
      min     = 0,
      max     = 11,
    },

    -- DNS resolver pushed to VPN clients (`push "dhcp-option DNS …"`), so the
    -- device surfaces vpn.assigned.dns. Empty disables the push.
    ["cloud.vpn.dns"] = {
      type    = "string",
      default = "1.1.1.1",
    },

    -- Runtime VPN PKI persisted in ds (the source of truth; the iot-vpn volume
    -- is disposable). iot-cloudd restores these into files on boot and persists
    -- back after generating, so a wiped/recreated volume keeps the SAME CA and
    -- already-pushed device certs stay valid. Private keys are gid:cloud-svc
    -- only, like the per-device keys in cloud.endpoint.credentials.
    ["cloud.vpn.ca.key.pem"] = {
        access = "Admin", type = "string", default = "",
        write_acl = {"gid:cloud-svc"}, read_acl = {"gid:cloud-svc"},
    },
    ["cloud.vpn.ca.crt.pem"] = {
        access = "Admin", type = "string", default = "",
        write_acl = {"gid:cloud-svc"},
    },
    ["cloud.vpn.server.key.pem"] = {
        access = "Admin", type = "string", default = "",
        write_acl = {"gid:cloud-svc"}, read_acl = {"gid:cloud-svc"},
    },
    ["cloud.vpn.server.crt.pem"] = {
        access = "Admin", type = "string", default = "",
        write_acl = {"gid:cloud-svc"},
    },

    -- Runtime VPN CRL + CA database, persisted in ds like the PKI above so
    -- revocation (device transfer) survives an iot-vpn volume loss. crl.pem is
    -- what openvpn enforces (crl-verify); index.txt/crlnumber let iot-cloudd
    -- revoke a cert minted before a volume loss. See apps/docs/tdd-device-transfer.md.
    ["cloud.vpn.crl.pem"] = {
        access = "Admin", type = "string", default = "",
        write_acl = {"gid:cloud-svc"},
    },
    ["cloud.vpn.ca.index"] = {
        access = "Admin", type = "string", default = "",
        write_acl = {"gid:cloud-svc"},
    },
    ["cloud.vpn.ca.crlnumber"] = {
        access = "Admin", type = "string", default = "",
        write_acl = {"gid:cloud-svc"},
    },

    -- Transfer-out (release a device from this cloud to a new owner): the
    -- cloud-ui writes the endpoint name here; iot-cloudd watches it, revokes the
    -- device's VPN cert (CRL), deprovisions, and audits. One-shot like
    -- cloud.provision.request. See apps/docs/tdd-device-transfer.md §G.
    ["cloud.transfer.release.request"] = {
        access  = "Admin",
        type    = "string",
        default = "",
    },
    -- Append-only audit of releases/claims (JSON array).
    ["cloud.transfer.audit"] = {
        access = "Admin", type = "string", default = "[]",
        write_acl = {"gid:cloud-svc"},
    },

    -- Next available proxy port (bump-counter).
    ["cloud.vpn.port.next"] = {
      type    = "integer",
      default = 10000,
      min     = 1024,
      max     = 65535,
    },

    -- Per-device proxy-port allocation range for device-UI-over-VPN DNAT.
    -- ds-driven so it isn't hardcoded; iot-cloudd reads these to size the
    -- VpnRegistry pool. The docker-compose published port range must cover
    -- this (Docker publishes ports before ds is consulted, so it can't follow
    -- ds automatically).
    -- Default kept ABOVE the CoAP ports (5683 DM / 5684 BS) and a SMALL
    -- window: every published proxy port spawns a docker-proxy, so a wide
    -- range exhausts the host. Widen for more devices, or use host networking.
    ["cloud.vpn.proxy.port.start"] = {
      type    = "integer",
      default = 10000,
      min     = 1024,
      max     = 65535,
    },
    ["cloud.vpn.proxy.port.end"] = {
      type    = "integer",
      default = 10050,
      min     = 1024,
      max     = 65535,
    },

    -- VPN PKI paths.  CA + server certs are generated at image build
    -- time (per-build CA, xpmile pattern).  ca.key is NOT in the image
    -- — mount at runtime via secret volume.
    ["cloud.vpn.ca.crt"] = {
      type    = "string",
      default = "/etc/iot/vpn/ca/ca.crt",
    },
    ["cloud.vpn.ca.key"] = {
      type    = "string",
      default = "/run/secrets/iot-ca-key/ca.key",
    },
    ["cloud.vpn.server.crt"] = {
      type    = "string",
      default = "/etc/iot/vpn/server.crt",
    },
    ["cloud.vpn.server.key"] = {
      type    = "string",
      default = "/etc/iot/vpn/server.key",
    },
  },
}
