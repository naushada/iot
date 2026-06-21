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

  /// Remove the current track polyline (+ its endpoint dots).
  clearTrack(): void {
    if (this.trackLayer && this.map) this.map.removeLayer(this.trackLayer);
    this.trackLayer = undefined;
    this.trackCount = 0;
  }

  ngOnDestroy(): void {
    this.sub.unsubscribe();
    this.map?.remove();
  }
}
