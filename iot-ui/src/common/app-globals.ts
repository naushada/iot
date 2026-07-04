// ── TypeScript interfaces mirroring the iot data-store schemas ──────

/// Humanise a duration in seconds → "3d 4h", "2h 13m", "5m 12s", "42s".
export function fmtDuration(sec: number): string {
  sec = Math.max(0, Math.floor(sec));
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

export interface StatusSnapshot {
  ok: boolean;
  lwm2m:    Lwm2mStatus;
  update?:  UpdateStatus;
  vpn:      VpnStatus;
  wifi:     WifiStatus;
  wan:      WanStatus;
  routing:  RoutingStatus;
  services: ServicesStatus;
  /// Host (this device / cloud container) runtime info — e.g. uptime since boot.
  device?:  DeviceStatus;
  /// Flat passthrough of ds keys the SPA caches verbatim (domain bump keys
  /// like log.version / services.stats.version, shared with the cloud build).
  cloud?:   Record<string, unknown>;
  /// mangOH Yellow telemetry — cellular modem, GPS fix, onboard sensors.
  /// Present only when the cellular-client / iot-sensord daemons publish.
  cell?:    CellStatus;
  gps?:     GpsStatus;
  sensor?:  SensorStatus;
}

// OTA software-update progress (LwM2M Object 5). state/result are the
// numeric LwM2M update state/result codes; version is the installed feed tag.
export interface UpdateStatus {
  version?: string;
  state?: number;
  result?: number;
}

export interface Lwm2mStatus {
  server_uri?: string;
  // Bootstrap-delivered DM server URI the client persisted to iot.dm.uri
  // (preferred over the legacy ds-driven server_uri on the DM status tab).
  dm_uri?: string;
  endpoint?: string;
  // Connection lifecycle token published by the client on iot.conn.state:
  // idle / bootstrapping / bootstrapped / dm-connecting / dm-connected /
  // registered / failed. The dashboard maps these to professional labels.
  conn_state?: string;
}

export interface VpnStatus {
  state?: string;
  ip?: string;
  gateway?: string;
  netmask?: string;
  dns?: string;
  pid?: number;
  exit_code?: number;
  gate_reason?: string;
  bound_iface?: string;
  /// Unix epoch (seconds) when the tunnel last reached CONNECTED. 0/absent =
  /// not connected. The UI derives "uptime" = now − connected_unix.
  connected_unix?: number;
}

export interface DeviceStatus {
  /// Seconds since the host booted (from /proc/uptime).
  uptime_sec?: number;
}

export interface WifiStatus {
  state?: string;
  ssid?: string;
  rssi?: number;
  dhcp_state?: string;
  dhcp_ip?: string;
  dhcp_mask?: string;
  dhcp_gateway?: string;
  dhcp_dns?: string;
  dhcp_lease_sec?: number;
  dhcp_domain?: string;
  dhcp_obtained_unix?: number;
}

export interface WanStatus {
  active_iface?: string;
  iface_priority?: string;
  tun_ip?: string;
}

export interface RoutingStatus {
  state?: string;
  rules_applied?: number;
  last_apply_unix?: number;
}

// Cellular modem status (mangOH WP), from cell.* (string-typed in the schema).
export interface CellStatus {
  state?: string;        // absent/init/sim-missing/searching/registered/connecting/connected/failed
  operator?: string;
  tech?: string;         // 2G / 3G / 4G
  reg?: string;          // home / roaming / searching / denied / not-registered / unknown
  signal_dbm?: string;
  signal_bars?: string;  // "0".."5"
  ip?: string;
  iccid?: string;
  // Last received SMS (mobile-terminated), from sms.* — only when sms.enable.
  sms_sender?: string;
  sms_text?: string;
  sms_ts?: string;
  sms_count?: string;
}

// GPS / GNSS fix, from gps.* (decimal-string fields).
export interface GpsStatus {
  fix?: string;          // none / 2d / 3d
  lat?: string;
  lon?: string;
  alt?: string;          // metres
  speed?: string;        // km/h
  course?: string;       // degrees
  sats?: string;
  utc?: string;
}

// mangOH onboard sensors, from iot.sensor.* (decimal strings; accel/gyro "x,y,z").
export interface SensorStatus {
  temp?: string;         // °C
  humidity?: string;     // %RH
  pressure?: string;     // Pa
  lux?: string;          // lux
  accel?: string;        // "x,y,z" raw counts
  gyro?: string;         // "x,y,z" raw counts
}

export interface ServiceInfo {
  enable?: boolean;
  state?: string;
  uptime_sec?: number;
  // L22 — per-container resource telemetry (cgroup + /proc/self/fd).
  cpu_permille?: number;
  cpu_count?: number;
  mem_kb?: number;
  fd_count?: number;
  threads?: number;
  [key: string]: unknown;
}

export interface ServicesStatus {
  ds?: ServiceInfo;
  net_router?: ServiceInfo;
  openvpn_client?: ServiceInfo;
  lwm2m_client?: ServiceInfo;
  lwm2m_server?: ServiceInfo;
  wifi_client?: ServiceInfo;
}

// ── Auth types ──────────────────────────────────────────────────────

export interface LoginRequest {
  id: string;
  password: string;
}

export interface LoginResponse {
  ok: boolean;
  role?: string;
  access?: string;   // "Admin" | "Viewer"
  err?: string;
}

export interface SessionInfo {
  role: string;
  access: string;    // "Admin" | "Viewer"
}

// ── User accounts (managed via /api/v1/users) ───────────────────────

export interface UserAccount {
  id: string;
  access: string;    // "Admin" | "Viewer"
}

export interface UserListResponse {
  ok: boolean;
  users?: UserAccount[];
  err?: string;
}

export interface UserCreateRequest {
  id: string;
  password: string;
  access: string;    // "Admin" | "Viewer"
}

export interface UserMutateResponse {
  ok: boolean;
  id?: string;
  err?: string;
}

// ── Db types ────────────────────────────────────────────────────────

export interface DbGetRequest {
  keys: string[];
}

export interface DbGetResponse {
  ok: boolean;
  data?: Record<string, unknown>;
  err?: string;
}

export interface DbSetPair {
  key: string;
  value: unknown;
}

export interface DbSetRequest {
  pairs: DbSetPair[];
}

export interface DbSetResponse {
  ok: boolean;
  changed?: number;
  err?: string;
}

// ── Service restart ─────────────────────────────────────────────────

export interface ServiceRestartRequest {
  service: string;
}

export interface ServiceRestartResponse {
  ok: boolean;
  service?: string;
  err?: string;
}

// ── WiFi scan ───────────────────────────────────────────────────────

export interface WifiScanResponse {
  ok: boolean;
  scan_request?: number;
  err?: string;
}

// ── VPN config (read/write keys) ────────────────────────────────────

export interface VpnConfig {
  remote_host?: string;
  remote_port?: number;
  remote_proto?: string;
  cert_path?: string;
  key_path?: string;
  ca_path?: string;
  cipher?: string;
  dev?: string;
  mgmt_port?: number;
}

// ── WiFi networks (stored as JSON string in wifi.networks) ──────────

export interface WifiNetwork {
  ssid: string;
  psk: string;
  priority?: number;
  key_mgmt?: string;
}

// ── LwM2M config ────────────────────────────────────────────────────

export interface Lwm2mConfig {
  server_uri?: string;
  endpoint?: string;
  binding?: string;
  lifetime?: number;
  observable?: boolean;
}

// ── Net / routing config ────────────────────────────────────────────

export interface NetConfig {
  lwm2m_target_ip?: string;
  lwm2m_target_port?: number;
  forward_ports?: string;    // comma-joined
  iface_priority?: string;   // comma-joined
  custom_rules?: string;     // JSON array
}
