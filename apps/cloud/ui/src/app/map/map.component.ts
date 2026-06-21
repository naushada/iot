import { Component, AfterViewInit, OnDestroy, Input } from '@angular/core';
import { Subscription, timer } from 'rxjs';
import { switchMap } from 'rxjs/operators';
import * as L from 'leaflet';
import { HttpsvcService } from '../../common/httpsvc.service';

interface Telem {
  endpoint: string;
  lat?: string; lon?: string;
  speed?: string; rpm?: string; coolant?: string; link?: string;
  throttle?: string; load?: string; fuel?: string; iat?: string; maf?: string; dtc?: string;
}

/// A pre-computed sparkline: an SVG path (viewBox 0 0 100 30) plus the
/// metric's min/max/last over the track window.
interface Spark { d: string; min: number; max: number; last: number; }

/// Fleet map — plots each endpoint's latest position from cloud.vehicle.telemetry
/// (live, polled). Client-side Leaflet (no plotting daemon); tiles come from the
/// self-hosted tileserver (compose service). circleMarkers avoid the Leaflet
/// default-marker-icon webpack pitfall. See §3d of the TDD.
@Component({
  selector: 'app-map',
  template: `
    <div class="page">
      <h3>Fleet Map</h3>
      <p class="hint" *ngIf="!count">
        No located vehicles yet — markers appear once devices report GPS (Object 6)
        and the telemetry pipeline populates <code>cloud.vehicle.telemetry</code>.
      </p>
      <div class="track-bar">
        <span class="lbl">History track:</span>
        <select #tsel (change)="trackEp = tsel.value" [value]="trackEp">
          <option value="">— select endpoint —</option>
          <option *ngFor="let ep of endpointList" [value]="ep">{{ ep }}</option>
        </select>
        <button class="btn btn-sm" (click)="loadTrack()" [disabled]="!trackEp || loadingTrack">
          {{ loadingTrack ? '…' : 'Show Track' }}
        </button>
        <button class="btn btn-sm btn-outline" *ngIf="trackLayer" (click)="clearTrack()">Clear</button>
        <span class="tinfo" *ngIf="trackLayer">{{ trackCount }} points (last 24 h)</span>
        <span class="tinfo" *ngIf="trackRequested && !trackLayer && !loadingTrack">no history for this endpoint yet</span>
      </div>
      <div id="fleet-map" class="map"></div>

      <div class="charts" *ngIf="trackLayer && trackCount > 1">
        <h4>{{ trackEp }} — last 24 h ({{ trackCount }} points)</h4>
        <div class="chart-grid">
          <div class="chart" *ngFor="let c of charts">
            <ng-container *ngIf="sparks[c.key] as s; else nochart">
              <div class="chd">
                <span class="cl">{{ c.label }}</span>
                <span class="cv">{{ s.last }}{{ c.unit }}</span>
              </div>
              <svg viewBox="0 0 100 30" preserveAspectRatio="none" class="spark">
                <path [attr.d]="s.d" fill="none" stroke="#0095d3" stroke-width="1.2"
                      vector-effect="non-scaling-stroke"></path>
              </svg>
              <div class="cmm"><span>min {{ s.min }}</span><span>max {{ s.max }}</span></div>
            </ng-container>
            <ng-template #nochart>
              <div class="chd"><span class="cl">{{ c.label }}</span><span class="cv">—</span></div>
              <div class="nodata">no data</div>
            </ng-template>
          </div>
        </div>
      </div>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 12px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 12px 0; }
    .track-bar { display: flex; align-items: center; gap: 8px; margin: 0 0 10px 0; font-size: 13px; }
    .track-bar .lbl { color: #555; font-weight: 600; }
    .track-bar select { padding: 3px 6px; }
    .track-bar .tinfo { color: #888; }
    .map { height: 70vh; width: 100%; border: 1px solid #ccc; border-radius: 4px; }
    .charts { margin-top: 16px; }
    .charts h4 { font-size: 13px; font-weight: 600; color: #555; margin: 0 0 10px 0; }
    .chart-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 12px; }
    .chart { border: 1px solid #e0e0e0; border-radius: 4px; padding: 8px 10px; }
    .chart .chd { display: flex; justify-content: space-between; font-size: 12px; }
    .chart .cl { color: #555; font-weight: 600; }
    .chart .cv { color: #0072a3; font-variant-numeric: tabular-nums; }
    .chart .spark { width: 100%; height: 36px; display: block; margin: 4px 0; }
    .chart .cmm { display: flex; justify-content: space-between; font-size: 10px; color: #999; font-variant-numeric: tabular-nums; }
    .chart .nodata { font-size: 11px; color: #bbb; padding: 12px 0; text-align: center; }
  `]
})
export class MapComponent implements AfterViewInit, OnDestroy {
  /// Optional endpoint to center on (set when arriving via an Endpoints link).
  @Input() focus = '';
  count = 0;
  /// Historical-track read-back (§3b). trackEp is the selected endpoint; the
  /// polyline is fetched on demand from /api/v1/cloud/telemetry/history.
  trackEp = '';
  trackCount = 0;
  loadingTrack = false;
  trackRequested = false;
  trackLayer?: L.FeatureGroup;
  /// Raw track points (incl. ts + every signal) backing the SVG charts below.
  private track: Array<Record<string, string | number>> = [];
  /// Pre-computed sparkline per metric (built once per load, not per CD cycle).
  sparks: Record<string, Spark | null> = {};
  /// Metrics charted under the map when a track is loaded (dep-free inline SVG).
  readonly charts: Array<{ key: string; label: string; unit: string }> = [
    { key: 'speed',    label: 'Speed',    unit: ' km/h' },
    { key: 'rpm',      label: 'RPM',      unit: '' },
    { key: 'coolant',  label: 'Coolant',  unit: ' °C' },
    { key: 'throttle', label: 'Throttle', unit: ' %' },
    { key: 'load',     label: 'Load',     unit: ' %' },
    { key: 'fuel',     label: 'Fuel',     unit: ' %' },
  ];
  private map?: L.Map;
  private markers: Record<string, L.CircleMarker> = {};
  private centeredFor = '';
  private sub = new Subscription();

  /// Endpoints currently on the map (track-picker options).
  get endpointList(): string[] { return Object.keys(this.markers).sort(); }

  // Self-hosted tileserver (PR-10a; default :8081). Operator-configurable per
  // deployment / seeded style. Leaflet still shows markers on a blank grid if
  // tiles 404, so the map is useful even before tiles are seeded.
  private readonly tileUrl =
    `${location.protocol}//${location.hostname}:8081/styles/basic/{z}/{x}/{y}.png`;

  constructor(private http: HttpsvcService) {}

  ngAfterViewInit(): void {
    this.trackEp = this.focus || '';   // pre-select when arriving via a link
    this.map = L.map('fleet-map', { center: [20, 0], zoom: 2 });
    L.tileLayer(this.tileUrl, { maxZoom: 19, attribution: 'Self-hosted tiles' }).addTo(this.map);
    this.sub.add(
      timer(0, 5000).pipe(switchMap(() => this.http.dbGet(['cloud.vehicle.telemetry'])))
        .subscribe(r => this.apply(r)));
  }

  private apply(r: { ok: boolean; data?: Record<string, unknown> }): void {
    const map = this.map;
    if (!r || !r.ok || !r.data || !map) return;
    let arr: Telem[] = [];
    try { arr = JSON.parse((r.data['cloud.vehicle.telemetry'] as string) || '[]'); }
    catch { arr = []; }

    let n = 0;
    for (const t of arr) {
      const lat = parseFloat(t.lat || '');
      const lon = parseFloat(t.lon || '');
      if (isNaN(lat) || isNaN(lon)) continue;
      n++;
      const dtc = (t.dtc && t.dtc !== '[]') ? t.dtc : 'none';
      const popup =
        `<b>${t.endpoint}</b><br>` +
        `speed ${t.speed ?? '—'} km/h &nbsp; rpm ${t.rpm ?? '—'}<br>` +
        `coolant ${t.coolant ?? '—'} °C &nbsp; throttle ${t.throttle ?? '—'} %<br>` +
        `load ${t.load ?? '—'} % &nbsp; fuel ${t.fuel ?? '—'} %<br>` +
        `iat ${t.iat ?? '—'} °C &nbsp; maf ${t.maf ?? '—'} g/s<br>` +
        `link ${t.link ?? '—'} &nbsp; DTCs ${dtc}`;
      const existing = this.markers[t.endpoint];
      if (existing) {
        existing.setLatLng([lat, lon]);
        existing.bindPopup(popup);
      } else {
        this.markers[t.endpoint] =
          L.circleMarker([lat, lon],
            { radius: 7, color: '#0072a3', fillColor: '#0095d3', fillOpacity: 0.8 })
            .bindPopup(popup)
            .addTo(map);
      }
    }
    this.count = n;

    // Arrived via an Endpoints "show on map" link → center + open that marker
    // once it exists (do it once per focus value).
    if (this.focus && this.focus !== this.centeredFor) {
      const fm = this.markers[this.focus];
      if (fm) {
        map.setView(fm.getLatLng(), 14);
        fm.openPopup();
        this.centeredFor = this.focus;
      }
    }
  }

  /// Fetch the selected endpoint's recent track and draw it as a polyline,
  /// fitting the map to its bounds. Start (green) / end (red) dots mark the
  /// window's extremes. Replaces any previous track.
  loadTrack(): void {
    if (!this.trackEp || !this.map) return;
    this.loadingTrack = true;
    this.trackRequested = true;
    this.http.getVehicleHistory(this.trackEp).subscribe(track => {
      this.loadingTrack = false;
      this.clearTrack();
      this.track = track;
      this.buildSparks();         // charts from the full signal set, GPS or not
      const map = this.map;
      if (!map) return;
      const pts: L.LatLngExpression[] = [];
      for (const p of track) {
        const lat = parseFloat(String(p['lat'] ?? ''));
        const lon = parseFloat(String(p['lon'] ?? ''));
        if (isNaN(lat) || isNaN(lon)) continue;
        pts.push([lat, lon]);
      }
      this.trackCount = pts.length;
      if (!pts.length) return;
      const line = L.polyline(pts, { color: '#d35400', weight: 3, opacity: 0.85 });
      const start = L.circleMarker(pts[0],
        { radius: 6, color: '#2e7d32', fillColor: '#43a047', fillOpacity: 0.9 }).bindPopup('Track start');
      const end = L.circleMarker(pts[pts.length - 1],
        { radius: 6, color: '#c62828', fillColor: '#e53935', fillOpacity: 0.9 }).bindPopup('Track end');
      // Group polyline + endpoint dots so Clear removes them together.
      const group = L.featureGroup([line, start, end]).addTo(map);
      this.trackLayer = group;
      map.fitBounds(group.getBounds(), { padding: [30, 30] });
    });
  }

  /// Remove the current track polyline (+ its endpoint dots) and its charts.
  clearTrack(): void {
    if (this.trackLayer && this.map) this.map.removeLayer(this.trackLayer);
    this.trackLayer = undefined;
    this.trackCount = 0;
    this.track = [];
    this.sparks = {};
  }

  /// Build one sparkline path per charted metric from the loaded track. Each
  /// metric is normalised to its own min/max over the window (viewBox
  /// 0 0 100 30, y inverted for SVG). NaN/empty samples are skipped; x is the
  /// sample's ordinal position so gaps don't distort the shape. Pre-computed
  /// here (once per load) rather than in a template getter (every CD cycle).
  private buildSparks(): void {
    const out: Record<string, Spark | null> = {};
    const n = this.track.length;
    for (const c of this.charts) {
      const xs: number[] = [];
      const ys: number[] = [];
      for (let i = 0; i < n; i++) {
        const v = parseFloat(String(this.track[i][c.key] ?? ''));
        if (isNaN(v)) continue;
        xs.push(n > 1 ? (i / (n - 1)) * 100 : 0);
        ys.push(v);
      }
      if (ys.length < 2) { out[c.key] = null; continue; }
      const min = Math.min(...ys);
      const max = Math.max(...ys);
      const range = (max - min) || 1;
      const d = xs
        .map((x, i) => `${i ? 'L' : 'M'}${x.toFixed(1)},${(28 - ((ys[i] - min) / range) * 26).toFixed(1)}`)
        .join(' ');
      // Round display values: integers for whole-ish metrics, 1dp otherwise.
      const r = (v: number) => (Math.abs(v) >= 10 || Number.isInteger(v) ? Math.round(v) : Math.round(v * 10) / 10);
      out[c.key] = { d, min: r(min), max: r(max), last: r(ys[ys.length - 1]) };
    }
    this.sparks = out;
  }

  ngOnDestroy(): void {
    this.sub.unsubscribe();
    this.map?.remove();
  }
}
