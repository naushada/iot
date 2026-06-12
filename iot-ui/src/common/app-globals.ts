// ── TypeScript interfaces mirroring the iot data-store schemas ──────

export interface StatusSnapshot {
  ok: boolean;
  lwm2m:    Lwm2mStatus;
  vpn:      VpnStatus;
  wifi:     WifiStatus;
  wan:      WanStatus;
  routing:  RoutingStatus;
  services: ServicesStatus;
}

export interface Lwm2mStatus {
  server_uri?: string;
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
}

export interface WifiStatus {
  state?: string;
  ssid?: string;
  rssi?: number;
  dhcp_state?: string;
  dhcp_ip?: string;
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
