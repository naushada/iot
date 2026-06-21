import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription, timer } from 'rxjs';
import { switchMap } from 'rxjs/operators';
import { HttpsvcService } from '../../common/httpsvc.service';

/// Vehicle telemetry (CAN/OBD-II ISO 15765-4) read live from vehicle.*,
/// published by the iot-vehicled daemon. Polled via /api/v1/db/get every 2s —
/// these keys are volatile and not on the /status stream. Property/Value
/// datagrid (Project Rule 4).
@Component({
  selector: 'app-vehicle-status',
  template: `
    <div class="page">
      <h3>Vehicle (OBD-II)</h3>
      <p class="hint">
        Bus link:
        <span [style.color]="link==='up' ? '#2e7d32' : (link==='no-ecu' ? '#b26a00' : '#c62828')">
          {{ linkLabel }}
        </span>
      </p>
      <p class="hint" *ngIf="!hasData">
        No vehicle telemetry yet — the iot-vehicled service publishes this once a
        CAN/OBD-II adapter is attached (can0 up) and the service is enabled.
      </p>
      <clr-datagrid>
        <clr-dg-column>Signal</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>

        <clr-dg-row *clrDgItems="let row of rows">
          <clr-dg-cell>
            {{ row.key }}
            <app-ds-hint *dsDebug [key]="row.dsKey"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>{{ row.value }}</clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ rows.length }} signals</clr-dg-footer>
      </clr-datagrid>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 16px 0; }
  `]
})
export class VehicleStatusComponent implements OnInit, OnDestroy {
  d: Record<string, unknown> = {};
  private sub = new Subscription();
  private readonly KEYS = [
    'vehicle.link', 'vehicle.speed', 'vehicle.rpm', 'vehicle.coolant', 'vehicle.throttle',
    'vehicle.load', 'vehicle.fuel', 'vehicle.iat', 'vehicle.maf', 'vehicle.dtc',
  ];

  get link(): string { return (this.d['vehicle.link'] as string) || 'down'; }
  get linkLabel(): string {
    return this.link === 'up' ? 'Connected'
         : this.link === 'no-ecu' ? 'Bus up, no ECU response'
         : 'Down';
  }
  get hasData(): boolean {
    return ['vehicle.speed', 'vehicle.rpm', 'vehicle.coolant', 'vehicle.throttle',
            'vehicle.load', 'vehicle.fuel', 'vehicle.iat', 'vehicle.maf'].some(k => !!this.d[k]);
  }

  private v(k: string, unit = ''): string {
    const x = this.d[k] as string;
    return x ? (unit ? `${x} ${unit}` : x) : '—';
  }

  get rows(): { key: string; value: string; dsKey: string }[] {
    return [
      { key: 'Speed',           value: this.v('vehicle.speed', 'km/h'), dsKey: 'vehicle.speed' },
      { key: 'Engine RPM',      value: this.v('vehicle.rpm'),           dsKey: 'vehicle.rpm' },
      { key: 'Coolant Temp',    value: this.v('vehicle.coolant', '°C'), dsKey: 'vehicle.coolant' },
      { key: 'Throttle',        value: this.v('vehicle.throttle', '%'), dsKey: 'vehicle.throttle' },
      { key: 'Engine Load',     value: this.v('vehicle.load', '%'),     dsKey: 'vehicle.load' },
      { key: 'Fuel Level',      value: this.v('vehicle.fuel', '%'),     dsKey: 'vehicle.fuel' },
      { key: 'Intake Air Temp', value: this.v('vehicle.iat', '°C'),     dsKey: 'vehicle.iat' },
      { key: 'MAF',             value: this.v('vehicle.maf', 'g/s'),    dsKey: 'vehicle.maf' },
      { key: 'DTCs',            value: this.v('vehicle.dtc'),           dsKey: 'vehicle.dtc' },
    ];
  }

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void {
    this.sub.add(
      timer(0, 2000).pipe(switchMap(() => this.http.dbGet(this.KEYS)))
        .subscribe(r => { if (r.ok && r.data) this.d = r.data; }));
  }
  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
