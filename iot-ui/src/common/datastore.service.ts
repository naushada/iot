import { Injectable } from '@angular/core';
import { BehaviorSubject, Observable } from 'rxjs';
import { filter, tap } from 'rxjs/operators';
import { HttpsvcService } from './httpsvc.service';
import { StatusSnapshot, DbSetResponse } from './app-globals';

/// Shared, prefetched view of the data store.
///
/// On login the SPA fetches every config key it might need in ONE
/// `POST /api/v1/db/get` (prefetchAll) and caches the result, so each
/// page can paint instantly from the cache instead of waiting its own
/// round-trip.  Liveness is kept by the aggregated `/api/v1/status`
/// long-poll (a single socket carrying the volatile vpn/wifi/services/
/// routing/wan telemetry) — we deliberately do NOT open one long-poll
/// socket per cached key.  Config keys change only via this UI and are
/// re-seeded on the next prefetch.
///
/// Secrets (PSK / TLS private keys) and large/volatile values
/// (`wifi.scan.results`) are intentionally excluded from the cache.
@Injectable({ providedIn: 'root' })
export class DataStoreService {

  private readonly ALL_KEYS: string[] = [
    // VPN client config
    'vpn.remote.host', 'vpn.remote.port', 'vpn.remote.proto',
    'vpn.cert.path', 'vpn.key.path', 'vpn.ca.path',
    'vpn.cipher', 'vpn.dev', 'vpn.mgmt.port',
    // WiFi config
    'wifi.iface', 'wifi.wpa.path', 'wifi.ctrl.dir',
    'wifi.scan.interval.sec', 'wifi.scan.max.results',
    'wifi.dhcp.client', 'wifi.networks',
    // Net / routing
    'net.lwm2m.target.ip', 'net.lwm2m.target.port', 'net.forward.ports',
    'net.custom.rules', 'net.iface.priority', 'net.iface.eth.name',
    'net.iface.wifi.name', 'net.iface.cellular.name', 'net.poll.interval.sec',
    'net.state', 'net.rules.applied.count', 'net.last.apply.unix',
    'net.iface.active',
    // LwM2M client
    'iot.serial', 'iot.dev.mode', 'iot.bs.uri', 'iot.server.uri', 'iot.dm.uri',
    'iot.binding', 'iot.lifetime',
    // OTA software update (read-only progress; rendered on the Software page)
    'iot.update.version', 'iot.update.state', 'iot.update.result',
    // Log levels
    'log.level', 'log.level.httpd', 'log.level.lwm2m',
    'log.level.vpn', 'log.level.dtls',
    // HTTP server — remote shell master switch (gates the Terminal page)
    'http.shell.enabled',
  ];

  private cache = new Map<string, unknown>();
  private subjects = new Map<string, BehaviorSubject<unknown>>();
  private statusActive = false;
  /// Latest full /status snapshot, replayed to late subscribers. Pages that
  /// need live status (dashboard, vpn-status, wifi-scan, services-list) read
  /// this instead of opening their own /status long-poll — one shared socket
  /// for the whole SPA.
  private statusSubject = new BehaviorSubject<StatusSnapshot | null>(null);

  constructor(private http: HttpsvcService) {}

  /// One batch read of every cached key — seeds the cache.
  prefetchAll(): void {
    this.http.dbGet(this.ALL_KEYS).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          for (const k of Object.keys(r.data)) this.set(k, r.data[k]);
        }
      },
      error: () => { /* pages fall back to their own dbGet */ }
    });
  }

  /// Write keys through the shared store. Persists to ds-server via the REST
  /// API and, on success, mirrors the new values into the local cache so
  /// snapshot()/observe() stay consistent on revisit WITHOUT re-running
  /// prefetchAll — config keys are written only here and are NOT carried by
  /// the /status long-poll, so without this a page would show pre-save values
  /// the next time it mounts. Config pages call this instead of
  /// HttpsvcService.dbSet for any key they also read back. (Secrets that are
  /// deliberately excluded from the cache keep using dbSet directly.)
  write(pairs: { key: string; value: unknown }[]): Observable<DbSetResponse> {
    return this.http.dbSet(pairs).pipe(
      tap(r => { if (r && r.ok) for (const p of pairs) this.set(p.key, p.value); })
    );
  }

  /// Keep volatile telemetry fresh via the aggregated /status long-poll.
  startWatch(): void {
    if (this.statusActive) return;
    this.statusActive = true;
    // Immediate seed so status-driven pages paint without waiting for the
    // first long-poll wake.
    this.http.getStatus().subscribe({ next: (s) => this.ingestStatus(s) });
    const poll = (): void => {
      if (!this.statusActive) return;
      this.http.getStatusLongPoll(30).subscribe({
        next: (s) => { this.ingestStatus(s); if (this.statusActive) poll(); },
        error: () => { if (this.statusActive) setTimeout(() => poll(), 5000); }
      });
    };
    poll();
  }

  stop(): void { this.statusActive = false; }

  /// Live full-status stream off the single shared long-poll. Replays the
  /// most recent snapshot to new subscribers (instant paint on revisit).
  observeStatus(): Observable<StatusSnapshot> {
    return this.statusSubject.pipe(
      filter((s): s is StatusSnapshot => s != null));
  }

  private ingestStatus(s: StatusSnapshot): void {
    if (!s) return;
    if (s.routing) {
      if (s.routing.state != null) this.set('net.state', s.routing.state);
      if (s.routing.rules_applied != null) this.set('net.rules.applied.count', s.routing.rules_applied);
      if (s.routing.last_apply_unix != null) this.set('net.last.apply.unix', s.routing.last_apply_unix);
    }
    if (s.lwm2m) {
      // Republish read-only LwM2M status to per-key subjects so the DM tab's
      // observe('iot.dm.uri') updates live (e.g. when registration completes).
      if (s.lwm2m.dm_uri != null)     this.set('iot.dm.uri', s.lwm2m.dm_uri);
      if (s.lwm2m.server_uri != null) this.set('iot.server.uri', s.lwm2m.server_uri);
    }
    if (s.update) {
      // OTA progress → per-key subjects so the Software page renders live off
      // this stream (the long-poll wakes on iot.update.state changes).
      if (s.update.version != null) this.set('iot.update.version', s.update.version);
      if (s.update.state != null)   this.set('iot.update.state', s.update.state);
      if (s.update.result != null)  this.set('iot.update.result', s.update.result);
    }
    if (s.wan && s.wan.active_iface != null) this.set('net.iface.active', s.wan.active_iface);
    // Flat passthrough keys (log.version / services.stats.version bump keys)
    // copied verbatim so log-viewer can observe('log.version') off this poll.
    if (s.cloud) {
      for (const k of Object.keys(s.cloud)) this.set(k, s.cloud[k]);
    }
    // Publish the full snapshot for status-driven pages.
    this.statusSubject.next(s);
  }

  // ── Accessors ─────────────────────────────────────────────────────
  has(key: string): boolean { return this.cache.has(key); }
  get(key: string): unknown { return this.cache.get(key); }
  getString(key: string, fallback = ''): string {
    const v = this.cache.get(key);
    return (v === undefined || v === null) ? fallback : String(v);
  }
  getNumber(key: string, fallback = 0): number {
    const v = this.cache.get(key);
    const n = Number(v);
    return Number.isFinite(n) ? n : fallback;
  }
  snapshot(): Record<string, unknown> {
    const o: Record<string, unknown> = {};
    for (const [k, v] of this.cache) o[k] = v;
    return o;
  }
  observe(key: string): Observable<unknown> { return this.subject(key).asObservable(); }

  private subject(key: string): BehaviorSubject<unknown> {
    let s = this.subjects.get(key);
    if (!s) { s = new BehaviorSubject<unknown>(this.cache.get(key)); this.subjects.set(key, s); }
    return s;
  }
  private set(key: string, value: unknown): void {
    this.cache.set(key, value);
    const s = this.subjects.get(key);
    if (s) s.next(value);
  }
}
