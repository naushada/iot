import { Injectable } from '@angular/core';
import { HttpClient, HttpHeaders, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../environments/environment';
import {
  StatusSnapshot,
  LoginRequest, LoginResponse,
  DbGetRequest, DbGetResponse,
  DbSetRequest, DbSetResponse,
  ServiceRestartRequest, ServiceRestartResponse,
  WifiScanResponse,
  UserListResponse, UserCreateRequest, UserMutateResponse
} from './app-globals';

@Injectable({ providedIn: 'root' })
export class HttpsvcService {

  // Base-relative API root. Empty when served at "/" (direct device access) so
  // calls hit /api/v1/*. When the device UI is reverse-proxied through the cloud
  // at /dev/<ep>/ (the proxy rewrites <base href> to that path), this derives
  // "/dev/<ep>" from the document base so calls go to /dev/<ep>/api/v1/* and the
  // cloud proxy strips the prefix back to /api/v1/* on the device.
  // See apps/docs/tdd-device-ui-path-proxy.md.
  private api = environment.apiUrl ||
    (new URL(document.baseURI).pathname.replace(/\/+$/, ''));

  constructor(private http: HttpClient) {}

  private jsonHeaders(): HttpHeaders {
    return new HttpHeaders({ 'Content-Type': 'application/json' });
  }

  // ── Auth ──────────────────────────────────────────────────────────

  login(body: LoginRequest): Observable<LoginResponse> {
    return this.http.post<LoginResponse>(
      `${this.api}/api/v1/auth/login`, body,
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  logout(): Observable<{ ok: boolean }> {
    return this.http.post<{ ok: boolean }>(
      `${this.api}/api/v1/auth/logout`, {},
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  // ── Status ────────────────────────────────────────────────────────

  getStatus(): Observable<StatusSnapshot> {
    return this.http.get<StatusSnapshot>(
      `${this.api}/api/v1/status`, { withCredentials: true });
  }

  /// Long-poll status: blocks until a state change or timeout (max 60s).
  /// Used for real-time dashboard + VPN status updates.
  getStatusLongPoll(timeoutSec = 30): Observable<StatusSnapshot> {
    const params = new HttpParams().set('timeout', timeoutSec.toString());
    return this.http.get<StatusSnapshot>(
      `${this.api}/api/v1/status`, { params, withCredentials: true });
  }

  // ── Data store ────────────────────────────────────────────────────

  dbGet(keys: string[]): Observable<DbGetResponse> {
    return this.http.post<DbGetResponse>(
      `${this.api}/api/v1/db/get`, { keys },
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  dbSet(pairs: { key: string; value: unknown }[]): Observable<DbSetResponse> {
    return this.http.post<DbSetResponse>(
      `${this.api}/api/v1/db/set`, { pairs },
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  // Drag-and-drop OTA: post a .ipk (or .tar.gz bundle) as the raw request body;
  // iot-httpd stages it into the swupdate spool and triggers the install.
  uploadUpdate(file: File): Observable<{ ok: boolean; err?: string }> {
    return this.http.post<{ ok: boolean; err?: string }>(
      `${this.api}/api/v1/update/upload?name=${encodeURIComponent(file.name)}`,
      file,
      { headers: new HttpHeaders({ 'Content-Type': 'application/octet-stream' }),
        withCredentials: true });
  }

  // Long-poll a single key.  Returns as soon as the value changes or
  // the timeout expires (no change → value reflects current state).
  dbGetLongPoll(key: string, timeoutSec = 30): Observable<{
    changed: boolean; key: string; value: unknown; prev?: unknown;
  }> {
    const params = new HttpParams()
      .set('key', key)
      .set('timeout', timeoutSec.toString());
    return this.http.get<{
      changed: boolean; key: string; value: unknown; prev?: unknown;
    }>(`${this.api}/api/v1/db/get`, { params, withCredentials: true });
  }

  // ── WiFi scan ─────────────────────────────────────────────────────

  triggerScan(): Observable<WifiScanResponse> {
    return this.http.post<WifiScanResponse>(
      `${this.api}/api/v1/wifi/scan`, {},
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  // ── Service restart ───────────────────────────────────────────────

  restartService(service: string): Observable<ServiceRestartResponse> {
    const body: ServiceRestartRequest = { service };
    return this.http.post<ServiceRestartResponse>(
      `${this.api}/api/v1/service/restart`, body,
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  // ── User management ───────────────────────────────────────────────

  listUsers(): Observable<UserListResponse> {
    return this.http.get<UserListResponse>(
      `${this.api}/api/v1/users`, { withCredentials: true });
  }

  createUser(body: UserCreateRequest): Observable<UserMutateResponse> {
    return this.http.post<UserMutateResponse>(
      `${this.api}/api/v1/users`, body,
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  deleteUser(id: string): Observable<UserMutateResponse> {
    return this.http.delete<UserMutateResponse>(
      `${this.api}/api/v1/users?id=${encodeURIComponent(id)}`,
      { withCredentials: true });
  }
}
