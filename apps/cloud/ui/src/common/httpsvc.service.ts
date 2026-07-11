import { Injectable } from '@angular/core';
import { HttpClient, HttpHeaders, HttpParams } from '@angular/common/http';
import { Observable, of } from 'rxjs';
import { map, catchError } from 'rxjs/operators';
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

  private api = environment.apiUrl;

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

  // ── Cloud API (L21) ──────────────────────────────────────────────

  getCloudEndpoints(): Observable<Array<{endpoint:string;tun_ip:string;proxy_port:number;registered:boolean;installed_version?:string}>> {
    return this.http.get<{ok:boolean;endpoints:Array<{endpoint:string;tun_ip:string;proxy_port:number;registered:boolean;installed_version?:string}>}>(
      `${this.api}/api/v1/cloud/endpoints`, { withCredentials: true })
      .pipe(map(r => r.endpoints || []), catchError(() => of([])));
  }

  provisionEndpoint(ep: string): Observable<any> {
    return this.http.post(`${this.api}/api/v1/cloud/endpoints`, { endpoint: ep },
      { headers: this.jsonHeaders(), withCredentials: true });
  }

  deprovisionEndpoint(ep: string): Observable<{ok:boolean;err?:string}> {
    return this.http.delete<{ok:boolean;err?:string}>(
      `${this.api}/api/v1/cloud/endpoints?ep=${encodeURIComponent(ep)}`,
      { withCredentials: true });
  }

  /// Historical vehicle track for one endpoint (§3b read-back). iot-httpd
  /// serves the recent window the iot-telemetry-ingest sidecar mongoexports;
  /// `track` is oldest-first [{ts,lat,lon,speed,...}]. Empty until the
  /// telemetry profile is up and the sidecar has exported once.
  getVehicleHistory(ep: string): Observable<Array<Record<string, string|number>>> {
    return this.http.get<{ok:boolean;track:Array<Record<string,string|number>>}>(
      `${this.api}/api/v1/cloud/telemetry/history?ep=${encodeURIComponent(ep)}`,
      { withCredentials: true })
      .pipe(map(r => r.track || []), catchError(() => of([])));
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

  // ── Firmware feed upload (Software Update) ────────────────────────
  // Chunked browse/drag-drop into the cloud firmware feed: offset=0 truncates,
  // final=1 finalises (server computes sha256 + upserts cloud.firmware.manifest).
  // pkg/version/arch are pre-filled from the filename and operator-confirmable.
  uploadFirmwareChunk(name: string, version: string, arch: string, pkg: string,
                      chunk: Blob, offset: number, final: boolean):
                      Observable<{ ok: boolean; err?: string; sha256?: string }> {
    return this.http.post<{ ok: boolean; err?: string; sha256?: string }>(
      `${this.api}/api/v1/firmware/upload?name=${encodeURIComponent(name)}` +
        `&version=${encodeURIComponent(version)}&arch=${encodeURIComponent(arch)}` +
        `&pkg=${encodeURIComponent(pkg)}&offset=${offset}&final=${final ? 1 : 0}`,
      chunk,
      { headers: new HttpHeaders({ 'Content-Type': 'application/octet-stream' }),
        withCredentials: true });
  }

  // Fetch firmware from an EXTERNAL http(s) URL into the cloud feed. The
  // download runs server-side in a background thread and returns 202
  // immediately; progress is reported on cloud.firmware.fetch.status (observe
  // it). On completion the artifact is in the feed + cloud.firmware.manifest,
  // ready to push like an uploaded package.
  fetchFirmware(body: { url: string; name: string; version: string; arch: string;
                        pkg: string; sha256?: string }):
                Observable<{ ok: boolean; err?: string }> {
    return this.http.post<{ ok: boolean; err?: string }>(
      `${this.api}/api/v1/firmware/fetch`, body,
      { headers: this.jsonHeaders(), withCredentials: true });
  }
}
