import { Injectable } from '@angular/core';
import { BehaviorSubject, Observable } from 'rxjs';
import { HttpsvcService } from './httpsvc.service';
import { StatusSnapshot } from './app-globals';

/// Shared, prefetched view of the data store.
///
/// On login the SPA fetches every config key it might need in ONE
/// `POST /api/v1/db/get` (prefetchAll) and caches the result, so each
/// page can paint instantly from the cache instead of waiting its own
/// round-trip.  Liveness is kept by the aggregated `/api/v1/status`
/// long-poll (a single socket carrying the volatile telemetry) — we
/// deliberately do NOT open one long-poll socket per cached key.  Config
/// keys change only via this UI and are re-seeded on the next prefetch.
///
/// Secrets (PSK / TLS private keys) and large/volatile values
/// (`wifi.scan.results`) are intentionally excluded from the cache.
@Injectable({ providedIn: 'root' })
export class DataStoreService {

  private readonly ALL_KEYS: string[] = [
    // Bootstrap + Device Management config (PSK keys excluded — secret)
    'cloud.bs.endpoint', 'cloud.bs.uri', 'cloud.bs.security.mode', 'cloud.bs.psk.id',
    'cloud.dm.uri', 'cloud.dm.lifetime', 'cloud.dm.binding', 'cloud.dm.psk.id',
    'cloud.dm.lwm2m.version', 'cloud.dev.mode',
    // VPN server config (private keys excluded — secret)
    'cloud.vpn.subnet', 'cloud.vpn.proto', 'cloud.vpn.listen.port', 'cloud.vpn.mgmt.port',
    'cloud.vpn.cipher', 'cloud.vpn.dev', 'cloud.vpn.verb', 'cloud.vpn.port.next',
    'cloud.vpn.ca.crt', 'cloud.vpn.server.crt',
    // HTTP server config (TLS key path is a path, not the key material)
    'http.listen.ip', 'http.listen.port', 'http.listen.scheme', 'http.workers',
    'http.tls.cert', 'http.tls.key', 'http.tls.ca', 'http.auth.enabled',
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
    // Log levels
    'log.level', 'log.level.cloudd', 'log.level.httpd',
    'log.level.lwm2m.bs', 'log.level.lwm2m.dm', 'log.level.vpn', 'log.level.dtls',
  ];

  private cache = new Map<string, unknown>();
  private subjects = new Map<string, BehaviorSubject<unknown>>();
  private statusActive = false;

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

  /// Keep volatile telemetry fresh via the aggregated /status long-poll.
  startWatch(): void {
    if (this.statusActive) return;
    this.statusActive = true;
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

  private ingestStatus(s: StatusSnapshot): void {
    if (!s) return;
    if (s.routing) {
      if (s.routing.state != null) this.set('net.state', s.routing.state);
      if (s.routing.rules_applied != null) this.set('net.rules.applied.count', s.routing.rules_applied);
      if (s.routing.last_apply_unix != null) this.set('net.last.apply.unix', s.routing.last_apply_unix);
    }
    if (s.wan && s.wan.active_iface != null) this.set('net.iface.active', s.wan.active_iface);
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
