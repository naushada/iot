-- cell.* / gps.* schema for the cellular-client modem daemon (mangOH Yellow
-- CF3 / WP module over an AT control channel + GNSS).
--
-- Read keys (operator → daemon):
--   cell.apn                - APN to activate the data context (default "";
--                             operator/SIM-specific, set via ds-cli / cloud-ui)
--   cell.modem.tty          - AT control device (default "/dev/ttyUSB2")
--   cell.gps.tty            - NMEA device. The daemon auto-detects the modem
--                             vendor (AT+GMI/CGMM) and starts GNSS accordingly:
--                             Sierra WP via AT!GPSFIX (needs this NMEA port set),
--                             Quectel via AT+QGPS (empty → poll AT+QGPSLOC).
--   cell.poll.interval.sec  - status poll cadence (+CSQ/+COPS/+CREG) (default 30)
--   cell.gps.enable         - enable GNSS reads (default true)
--   sms.enable              - enable MT SMS receive (AT+CMGF/CNMI/CMGR) (default
--                             false — operator opts in once the SIM/carrier is
--                             confirmed to deliver MT SMS on this NB-IoT plan)
--   sms.forward.cloud       - forward received SMS to cloud-iot (default false)
--
-- Write keys (daemon → operator / device-ui / lwm2m client). All string-typed
-- so the daemon publishes the cache batch verbatim; the device-ui and the
-- client's Object-6 mirror parse as needed. Numeric values are decimal strings.
--   cell.state       - "absent"/"init"/"sim-missing"/"searching"/"registered"/
--                      "connecting"/"connected"/"failed"
--   cell.operator    - registered operator name
--   cell.tech        - "2G"/"3G"/"4G"
--   cell.reg         - "home"/"roaming"/"searching"/"denied"/"not-registered"/
--                      "unknown"
--   cell.signal.dbm  - RSSI in dBm
--   cell.signal.bars - 0..5 signal bars (UI)
--   cell.ip          - IP assigned to the data context. On a WP7702 this is the
--                      address of the MODULE's internal rmnet_data0, NOT of any
--                      host interface -- informational only. Never assign it to
--                      wwan0: see apps/docs/hw-bringup-wp7702-cellular-wan.md.
--   cell.dns         - carrier DNS resolvers for the data context, comma-joined
--                      (AT+CGCONTRDP=1), e.g. "117.96.122.74,59.144.127.117".
--                      Mirrors the vpn.assigned.dns convention. IPv4 only.
--   cell.iccid       - SIM ICCID
--   cell.version     - bump-on-change counter for the device-ui long-poll
--   gps.fix          - "none"/"2d"/"3d"
--   gps.lat/lon      - decimal degrees (signed)
--   gps.alt          - altitude (m)
--   gps.speed        - ground speed (km/h)
--   gps.course       - course over ground (deg)
--   gps.sats         - satellites used
--   gps.utc          - raw UTC field from the fix
--   gps.version      - bump-on-change counter
--
-- The lwm2m client mirrors gps.* into LwM2M Object 6 (Location); net-router
-- already routes the cellular slot (net.iface.cellular.name default "eth1" --
-- the WP7702's ECM link, since the host cannot open a data call on wwan0).
--
-- This daemon does NOT bring up the data path. It provisions the PDP context
-- (AT+CGDCONT) and reports status; the bearer itself is owned by the modem
-- stack. See apps/docs/hw-bringup-wp7702-cellular-wan.md.

local function viewer_str()  return { access = "Viewer", type = "string",  default = "" } end
local function admin_str(d)  return { access = "Admin",  type = "string",  default = d } end

return {
  namespace = "cell",
  keys = {
    -- read (operator-set)
    ["cell.apn"]               = admin_str(""),
    -- Which PDP context is OURS. The modem carries several — a WP7702 ships the
    -- eSIM's own profile on a cid of its choosing — so this says which one the
    -- daemon provisions (AT+CGDCONT=<cid>) and reads back (AT+CGCONTRDP=<cid>,
    -- the source of cell.ip/cell.dns). It does NOT force the network's default
    -- bearer; see cell.apn.profiles for what the modem actually holds.
    ["cell.apn.cid"]           = { access = "Admin", type = "integer", default = 1, min = 1, max = 16 },
    ["cell.modem.tty"]         = admin_str("/dev/ttyUSB2"),
    ["cell.gps.tty"]           = admin_str(""),
    ["cell.poll.interval.sec"] = { access = "Admin", type = "integer", default = 30, min = 5, max = 3600 },
    ["cell.gps.enable"]        = { access = "Admin", type = "boolean", default = true },
    ["cell.rat"]               = { access = "Admin", type = "string", default = "" },  -- ""=leave; auto|gsm|umts|lte|gsm+lte|... (Sierra AT!SELRAT)
    ["sms.enable"]             = { access = "Admin", type = "boolean", default = false },
    ["sms.forward.cloud"]      = { access = "Admin", type = "boolean", default = false },
    -- DirectIP data-call supervisor (opt-in). With the WP7702 in the DirectIP USB
    -- composition (ECM dropped via AT!USBCOMP=1,1,0000010D), the host owns the data
    -- call on wwan0; the daemon runs raw_ip + `qmicli --wds-start-network` + address
    -- /route, and re-establishes on each modem re-enumeration. OFF by default: an
    -- ECM-composition board keeps using eth1 and must NOT bring up wwan0. See
    -- apps/docs/hw-bringup-wp7702-cellular-wan.md §4.4.
    ["cell.data.enable"]       = { access = "Admin", type = "boolean", default = false },
    ["cell.qmi.dev"]           = admin_str("/dev/cdc-wdm0"),   -- QMI control node
    ["cell.wan.iface"]         = admin_str("wwan0"),           -- raw-ip WAN netdev

    -- write (daemon-published status)
    ["cell.state"]       = viewer_str(),
    ["cell.operator"]    = viewer_str(),
    ["cell.tech"]        = viewer_str(),
    ["cell.reg"]         = viewer_str(),
    ["cell.reg.cs"]      = viewer_str(),   -- +CREG (2G/3G CS — MT-SMS rides here on 2G)
    ["cell.reg.ps"]      = viewer_str(),   -- +CGREG (2G/3G PS)
    ["cell.reg.eps"]     = viewer_str(),   -- +CEREG (LTE EPS)
    ["cell.signal.dbm"]  = viewer_str(),
    ["cell.signal.bars"] = viewer_str(),
    ["cell.ip"]          = viewer_str(),
    ["cell.dns"]         = viewer_str(),   -- carrier resolvers, comma-joined (AT+CGCONTRDP=1)
    ["cell.iccid"]       = viewer_str(),
    ["cell.imei"]        = viewer_str(),   -- modem IMEI (ATI)
    ["cell.msisdn"]      = viewer_str(),   -- SIM number (AT+CNUM; often blank)
    ["cell.model"]       = viewer_str(),   -- modem model (ATI), e.g. WP7702
    ["cell.fw"]          = viewer_str(),   -- modem firmware revision (ATI)
    ["cell.capability"]  = viewer_str(),   -- RAT capability, e.g. "LTE-M / NB-IoT / GSM"
    ["cell.apn.current"] = viewer_str(),   -- APN of cell.apn.cid, read back (AT+CGDCONT?)
    -- Every provisioned PDP context as a JSON array (device-ui table):
    -- [{"cid":1,"type":"IP","apn":"airtelgprs.com"},{"cid":2,...}]
    ["cell.apn.profiles"] = viewer_str(),
    ["cell.rat.current"] = viewer_str(),   -- RAT the modem reports (AT!SELRAT?)
    ["cell.reg.reason"]  = viewer_str(),   -- network reject cause (AT+CEER), if any
    ["cell.version"]     = viewer_str(),

    -- DirectIP data-call status (only meaningful when cell.data.enable=true).
    -- cell.data.ip is the REAL wwan0 host address (unlike cell.ip, which on a
    -- WP7702 is the module's internal rmnet_data0 address).
    ["cell.data.state"]   = viewer_str(),  -- waiting-reg/attaching/starting/up/error
    ["cell.data.ip"]      = viewer_str(),  -- host IPv4 on wwan0 (from QMI)
    ["cell.data.gateway"] = viewer_str(),  -- QMI-reported gateway
    ["cell.data.dns"]     = viewer_str(),  -- QMI-reported resolvers, comma-joined

    -- Module restart request (device-ui button): bump this monotonic token
    -- and the daemon cycles the radio (AT+CFUN=0/1) + re-applies APN/SMS/RAT
    -- setup. Progress is visible in the existing cell.state lifecycle
    -- (init -> searching -> registered -> connected).
    ["cell.reset.request"] = { access = "Admin", type = "string", default = "" },

    -- MO SMS send envelope: set sms.send.to + sms.send.text, then bump
    -- sms.send.request (monotonic token) to send. The daemon publishes progress
    -- in sms.send.status ("sending"/"sent <ref>"/"failed: <reason>").
    ["sms.send.to"]      = { access = "Admin", type = "string", default = "" },
    ["sms.send.text"]    = { access = "Admin", type = "string", default = "" },
    ["sms.send.request"] = { access = "Admin", type = "string", default = "" },
    -- device-ui "Clear" on the Received SMS table: bump this monotonic token and
    -- the daemon wipes its in-memory history and republishes an empty sms.inbox
    -- + sms.count=0. A UI-only ds wipe would be undone by the next message,
    -- because the daemon owns the history.
    ["sms.clear.request"] = { access = "Admin", type = "string", default = "" },
    ["sms.send.status"]  = viewer_str(),

    -- GPS / GNSS (daemon-published)
    ["gps.fix"]     = viewer_str(),
    ["gps.lat"]     = viewer_str(),
    ["gps.lon"]     = viewer_str(),
    ["gps.alt"]     = viewer_str(),
    ["gps.speed"]   = viewer_str(),
    ["gps.course"]  = viewer_str(),
    ["gps.sats"]    = viewer_str(),
    ["gps.utc"]     = viewer_str(),
    ["gps.version"] = viewer_str(),

    -- SMS (daemon-published; latest received message + a running count)
    ["sms.last.sender"] = viewer_str(),
    ["sms.last.text"]   = viewer_str(),
    ["sms.last.ts"]     = viewer_str(),
    ["sms.count"]       = viewer_str(),
    -- Received-SMS history as a JSON array (newest first), each element
    -- {ts,from,text}; bounded to the last 20. Rendered as the device-ui
    -- WAN -> Cellular "Received SMS" table. Seeded from ds on daemon restart.
    ["sms.inbox"]       = viewer_str(),
    -- SIM message-store usage "used/total" (AT+CPMS?) — a full store
    -- silently blocks MT-SMS delivery.
    ["sms.storage"]     = viewer_str(),
    ["sms.version"]     = viewer_str(),
  },
}
