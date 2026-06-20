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
--   cell.ip          - IP assigned to the data context (wwan0)
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
-- already routes the cellular slot (net.iface.cellular.name default "wwan0").

local function viewer_str()  return { access = "Viewer", type = "string",  default = "" } end
local function admin_str(d)  return { access = "Admin",  type = "string",  default = d } end

return {
  namespace = "cell",
  keys = {
    -- read (operator-set)
    ["cell.apn"]               = admin_str(""),
    ["cell.modem.tty"]         = admin_str("/dev/ttyUSB2"),
    ["cell.gps.tty"]           = admin_str(""),
    ["cell.poll.interval.sec"] = { access = "Admin", type = "integer", default = 30, min = 5, max = 3600 },
    ["cell.gps.enable"]        = { access = "Admin", type = "boolean", default = true },

    -- write (daemon-published status)
    ["cell.state"]       = viewer_str(),
    ["cell.operator"]    = viewer_str(),
    ["cell.tech"]        = viewer_str(),
    ["cell.reg"]         = viewer_str(),
    ["cell.signal.dbm"]  = viewer_str(),
    ["cell.signal.bars"] = viewer_str(),
    ["cell.ip"]          = viewer_str(),
    ["cell.iccid"]       = viewer_str(),
    ["cell.version"]     = viewer_str(),

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
  },
}
