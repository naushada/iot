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
    'net.custom.rules', 'net.iface.priority', 'net.iface.eth.name',
    'net.iface.wifi.name', 'net.iface.cellular.name', 'net.poll.interval.sec',
    'net.state', 'net.rules.applied.count', 'net.last.apply.unix',
    'net.iface.active',
    // Log levels
    'log.level', 'log.level.cloudd', 'log.level.httpd',
    'log.level.lwm2m.bs', 'log.level.lwm2m.dm', 'log.level.vpn', 'log.level.dtls',
    // Cloud Service rows (state/enable + L22 telemetry). Seeded here for an
    // instant first paint; kept fresh by the shared /status long-poll, which
    // now wakes on services.stats.version. The Services page reads these from
    // the cache instead of opening its own long-poll (worker starvation fix).
    'services.ds.state',
    'services.cloud.iot.cloudd.enable', 'services.cloud.iot.cloudd.state',
    'services.cloud.iot.httpd.enable', 'services.cloud.iot.httpd.state',
    'services.cloud.openvpn.server.enable', 'services.cloud.openvpn.server.state',
    'services.cloud.lwm2m.bs.state', 'services.cloud.lwm2m.dm.state',
    'services.ds.cpu.permille', 'services.ds.cpu.count', 'services.ds.mem.rss.kb',
    'services.ds.fd.count', 'services.ds.threads',
    'services.cloud.iot.cloudd.cpu.permille', 'services.cloud.iot.cloudd.cpu.count',
    'services.cloud.iot.cloudd.mem.rss.kb', 'services.cloud.iot.cloudd.fd.count',
    'services.cloud.iot.cloudd.threads',
    'services.cloud.iot.httpd.cpu.permille', 'services.cloud.iot.httpd.cpu.count',
    'services.cloud.iot.httpd.mem.rss.kb', 'services.cloud.iot.httpd.fd.count',
    'services.cloud.iot.httpd.threads',
    'services.cloud.openvpn.server.cpu.permille', 'services.cloud.openvpn.server.cpu.count',
    'services.cloud.openvpn.server.mem.rss.kb', 'services.cloud.openvpn.server.fd.count',
    'services.cloud.openvpn.server.threads',
    'services.cloud.lwm2m.bs.cpu.permille', 'services.cloud.lwm2m.bs.cpu.count',
    'services.cloud.lwm2m.bs.mem.rss.kb', 'services.cloud.lwm2m.bs.fd.count',
    'services.cloud.lwm2m.bs.threads',
    'services.cloud.lwm2m.dm.cpu.permille', 'services.cloud.lwm2m.dm.cpu.count',
    'services.cloud.lwm2m.dm.mem.rss.kb', 'services.cloud.lwm2m.dm.fd.count',
    'services.cloud.lwm2m.dm.threads',
    // Cloud OTA push status (per-device), rendered live on the Software page.
    'cloud.update.status',
  ];

  private cache = new Map<string, unknown>();
  private subjects = new Map<string, BehaviorSubject<unknown>>();
  private statusActive = false;
  /// Latest full /status snapshot, replayed to late subscribers. Pages that
  /// need live status (vpn-status, wifi-scan) read this instead of opening
  /// their own /status long-poll — one shared socket for the whole SPA.
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
    if (s.wan && s.wan.active_iface != null) this.set('net.iface.active', s.wan.active_iface);
    // Cloud Service rows + domain bump keys come through verbatim — copy
    // each into the cache under its ds key so the Services/Logs pages (and
    // anyone observing services.stats.version / log.version) stay live off
    // this single long-poll.
    if (s.cloud) {
      for (const k of Object.keys(s.cloud)) this.set(k, s.cloud[k]);
    }
    // Publish the full snapshot for status-driven pages (vpn-status,
    // wifi-scan) that consume the structured shape directly.
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
