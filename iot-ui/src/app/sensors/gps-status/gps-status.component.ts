import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../../common/datastore.service';
import { GpsStatus } from '../../../common/app-globals';

/// GPS / GNSS location, read from gps.* off the shared /status stream
/// (published by the cellular-client daemon; mirrored to LwM2M Object 6).
/// Property/Value datagrid (Project Rule 4) + an OpenStreetMap link.
@Component({
  selector: 'app-gps-status',
  template: `
    <div class="page">
      <h3>Location (GPS)</h3>
      <p class="hint" *ngIf="!hasFix">
        No GPS fix yet — needs the WP module's GNSS enabled (cell.gps.enable) and
        sky visibility. Fix: <strong>{{ g.fix || 'none' }}</strong>.
      </p>
      <clr-datagrid>
        <clr-dg-column>Property</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>

        <clr-dg-row *clrDgItems="let row of rows">
          <clr-dg-cell>
            {{ row.key }}
            <app-ds-hint *dsDebug [key]="row.dsKey"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>
            <span *ngIf="row.isFix" class="fix" [ngClass]="fixClass">{{ row.value }}</span>
            <a *ngIf="row.isMap && hasFix" [href]="mapUrl" target="_blank" rel="noopener">
              {{ g.lat }}, {{ g.lon }} ↗
            </a>
            <span *ngIf="row.isMap && !hasFix">—</span>
            <span *ngIf="!row.isFix && !row.isMap">{{ row.value }}</span>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ rows.length }} properties</clr-dg-footer>
      </clr-datagrid>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 16px 0; }
    .fix { padding: 1px 8px; border-radius: 10px; font-size: 12px; background: #eef2f7; color: #555; }
    .fix.fix3 { background: #e6f4ea; color: #1e7e34; }
    .fix.fix2 { background: #fff4e5; color: #a05a00; }
    a { color: #1a73e8; text-decoration: none; }
    a:hover { text-decoration: underline; }
  `]
})
export class GpsStatusComponent implements OnInit, OnDestroy {
  g: GpsStatus = {};
  private sub = new Subscription();

  get hasFix(): boolean { return this.g.fix === '2d' || this.g.fix === '3d'; }
  get fixClass(): string { return this.g.fix === '3d' ? 'fix3' : this.g.fix === '2d' ? 'fix2' : ''; }
  get mapUrl(): string {
    return `https://www.openstreetmap.org/?mlat=${this.g.lat}&mlon=${this.g.lon}#map=15/${this.g.lat}/${this.g.lon}`;
  }

  get rows(): { key: string; value: string; isFix?: boolean; isMap?: boolean; dsKey: string }[] {
    return [
      { key: 'Fix',        value: this.g.fix || 'none', isFix: true, dsKey: 'gps.fix' },
      { key: 'Position',   value: '', isMap: true, dsKey: 'gps.lat' },
      { key: 'Altitude',   value: this.g.alt ? `${this.g.alt} m` : '—',       dsKey: 'gps.alt' },
      { key: 'Speed',      value: this.g.speed ? `${this.g.speed} km/h` : '—', dsKey: 'gps.speed' },
      { key: 'Course',     value: this.g.course ? `${this.g.course}°` : '—',   dsKey: 'gps.course' },
      { key: 'Satellites', value: this.g.sats || '—',                          dsKey: 'gps.sats' },
      { key: 'UTC',        value: this.g.utc || '—',                           dsKey: 'gps.utc' },
    ];
  }

  constructor(private ds: DataStoreService) {}

  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((s) => { this.g = s.gps || {}; }));
  }
  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
