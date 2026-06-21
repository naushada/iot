import { Component, AfterViewInit, OnDestroy } from '@angular/core';
import { Subscription, timer } from 'rxjs';
import { switchMap } from 'rxjs/operators';
import * as L from 'leaflet';
import { HttpsvcService } from '../../common/httpsvc.service';

interface Telem {
  endpoint: string;
  lat?: string; lon?: string;
  speed?: string; rpm?: string; coolant?: string; link?: string;
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
      <div id="fleet-map" class="map"></div>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 12px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 12px 0; }
    .map { height: 70vh; width: 100%; border: 1px solid #ccc; border-radius: 4px; }
  `]
})
export class MapComponent implements AfterViewInit, OnDestroy {
  count = 0;
  private map?: L.Map;
  private markers: Record<string, L.CircleMarker> = {};
  private sub = new Subscription();

  // Self-hosted tileserver (PR-10a; default :8081). Operator-configurable per
  // deployment / seeded style. Leaflet still shows markers on a blank grid if
  // tiles 404, so the map is useful even before tiles are seeded.
  private readonly tileUrl =
    `${location.protocol}//${location.hostname}:8081/styles/basic/{z}/{x}/{y}.png`;

  constructor(private http: HttpsvcService) {}

  ngAfterViewInit(): void {
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
      const popup =
        `<b>${t.endpoint}</b><br>` +
        `speed ${t.speed ?? '—'} km/h<br>rpm ${t.rpm ?? '—'}<br>` +
        `coolant ${t.coolant ?? '—'} °C<br>link ${t.link ?? '—'}`;
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
  }

  ngOnDestroy(): void {
    this.sub.unsubscribe();
    this.map?.remove();
  }
}
