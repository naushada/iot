import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../../common/datastore.service';
import { SensorStatus } from '../../../common/app-globals';

/// mangOH Yellow onboard sensors (BME680 env, BMI160 IMU, light), read from
/// iot.sensor.* off the shared /status stream (published by iot-sensord).
/// Property/Value datagrid (Project Rule 4).
@Component({
  selector: 'app-sensors-status',
  template: `
    <div class="page">
      <h3>Sensors</h3>
      <p class="hint" *ngIf="!hasData">
        No sensor telemetry yet — the iot-sensord service publishes this once the
        mangOH Yellow board is attached and the service is enabled.
      </p>
      <clr-datagrid>
        <clr-dg-column>Sensor</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>

        <clr-dg-row *clrDgItems="let row of rows">
          <clr-dg-cell>
            {{ row.key }}
            <app-ds-hint *dsDebug [key]="row.dsKey"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>{{ row.value }}</clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ rows.length }} sensors</clr-dg-footer>
      </clr-datagrid>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 16px 0; }
  `]
})
export class SensorsStatusComponent implements OnInit, OnDestroy {
  s: SensorStatus = {};
  private sub = new Subscription();

  get hasData(): boolean {
    return !!(this.s.temp || this.s.humidity || this.s.pressure || this.s.lux ||
              this.s.accel || this.s.gyro);
  }

  /// Pressure is published in Pa; show kPa alongside for readability.
  private pressureText(): string {
    if (!this.s.pressure) return '—';
    const pa = parseFloat(this.s.pressure);
    if (isNaN(pa)) return this.s.pressure;
    return `${this.s.pressure} Pa (${(pa / 1000).toFixed(2)} kPa)`;
  }

  private axes(v?: string, unit = ''): string {
    if (!v) return '—';
    const [x, y, z] = v.split(',');
    return `X ${x ?? '—'}  Y ${y ?? '—'}  Z ${z ?? '—'}${unit ? ' ' + unit : ''}`;
  }

  get rows(): { key: string; value: string; dsKey: string }[] {
    return [
      { key: 'Temperature',   value: this.s.temp ? `${this.s.temp} °C` : '—',         dsKey: 'iot.sensor.temp' },
      { key: 'Humidity',      value: this.s.humidity ? `${this.s.humidity} %RH` : '—', dsKey: 'iot.sensor.humidity' },
      { key: 'Pressure',      value: this.pressureText(),                              dsKey: 'iot.sensor.pressure' },
      { key: 'Illuminance',   value: this.s.lux ? `${this.s.lux} lux` : '—',           dsKey: 'iot.sensor.lux' },
      { key: 'Accelerometer', value: this.axes(this.s.accel),                          dsKey: 'iot.sensor.accel' },
      { key: 'Gyroscope',     value: this.axes(this.s.gyro),                           dsKey: 'iot.sensor.gyro' },
    ];
  }

  constructor(private ds: DataStoreService) {}

  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((st) => { this.s = st.sensor || {}; }));
  }
  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
