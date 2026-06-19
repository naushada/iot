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
    -- Default registration lifetime pushed to devices at bootstrap
    -- (Server Object OID 1, RID 1).  Devices may request a different
    -- value; the DM server can accept or negotiate.
    ["cloud.dm.lifetime"] = {
        access  = "Admin",
      type    = "integer",
      default = 86400,
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
    -- Deprovision request: iot-httpd writes the endpoint name here;
    -- iot-cloudd watches this key and calls deprovision().
    ["cloud.deprovision.request"] = {
        access  = "Admin",
      type    = "string",
      default = "",
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
    ["cloud.firmware.manifest"] = {
        access  = "Admin",
        type    = "string",
        default = "[]",
    },
    -- Update request from cloud-ui (carrier — iot-cloudd clears to ""):
    --   { "serials":["100000abcd",...], "pkg":"iot", "version":"0.2.0",
    --     "url":"/firmware/iot_0.2.0_aarch64.ipk", "sha256":"<hex>" }
    ["cloud.update.request"] = {
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
    --     "version":"0.2.0", "ts":<unix> }
    ["cloud.update.status"] = {
        access  = "Admin",
        type    = "string",
        default = "[]",
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
