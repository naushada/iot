import { Component, OnInit, OnDestroy } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../../common/datastore.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

/// Cellular modem (mangOH WP) configuration — APN + serial ports + poll cadence.
/// The cellular-client daemon reads these cell.* keys at startup. Partial
/// auto-detect: the daemon defaults modem.tty to /dev/ttyUSB2 and can read GPS
/// over the NMEA tty, but the APN (and which ttyUSB is AT vs NMEA) is operator-
/// supplied — hence this form. Clarity .form-grid (Project Rule 5, 4-col).
@Component({
  selector: 'app-cellular-config',
  template: `
    <div class="page">
      <h3>Cellular Configuration</h3>
      <p class="hint">
        The cellular-client service reads these at startup (restart it to apply).
        Find the AT port with <code>AT</code>→<code>OK</code>; leave GPS TTY empty
        only on Quectel modems (Sierra WP uses the NMEA port).
      </p>

      <form [formGroup]="form" (ngSubmit)="save()" *ngIf="!loading">
        <div class="form-grid">
          <clr-input-container>
            <label>APN</label>
            <input clrInput [disabled]="!isAdmin" formControlName="apn"
                   placeholder="internet" />
            <clr-control-helper>Operator/SIM APN for the data context.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="cell.apn"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Modem AT TTY</label>
            <input clrInput [disabled]="!isAdmin" formControlName="modemTty"
                   placeholder="/dev/ttyUSB2" />
            <clr-control-helper>The AT control port.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="cell.modem.tty"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>GPS NMEA TTY</label>
            <input clrInput [disabled]="!isAdmin" formControlName="gpsTty"
                   placeholder="/dev/ttyUSB1" />
            <clr-control-helper>Empty = GPS over AT (Quectel only).</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="cell.gps.tty"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Poll Interval (s)</label>
            <input clrInput [disabled]="!isAdmin" type="number" min="5" max="3600"
                   formControlName="pollSec" />
            <clr-control-helper>Status poll cadence.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="cell.poll.interval.sec"></app-ds-hint></clr-control-helper>
          </clr-input-container>
        </div>

        <div class="form-grid">
          <clr-checkbox-container>
            <label>GNSS</label>
            <clr-checkbox-wrapper>
              <input type="checkbox" clrCheckbox [disabled]="!isAdmin"
                     formControlName="gpsEnable" />
              <label>Enable GPS</label>
            </clr-checkbox-wrapper>
            <clr-control-helper *dsDebug><app-ds-hint key="cell.gps.enable"></app-ds-hint></clr-control-helper>
          </clr-checkbox-container>
          <!-- pad to fill the 4-column row (Project Rule 5) -->
          <div></div><div></div><div></div>
        </div>

        <div style="margin-top:24px;">
          <button type="submit" class="btn btn-primary" [disabled]="saving || !isAdmin">
            {{ saving ? 'Saving…' : 'Save' }}
          </button>
        </div>
      </form>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 12px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 20px 0; }
    code { background: #eef2f7; padding: 0 4px; border-radius: 3px; }
  `]
})
export class CellularConfigComponent implements OnInit, OnDestroy {
  form: FormGroup;
  loading = true;
  saving = false;
  private sub = new Subscription();
  private readonly KEYS = [
    'cell.apn', 'cell.modem.tty', 'cell.gps.tty',
    'cell.poll.interval.sec', 'cell.gps.enable',
  ];

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(fb: FormBuilder,
              private session: SessionService,
              private toast: ToastService,
              private ds: DataStoreService) {
    this.form = fb.group({
      apn:       [''],
      modemTty:  ['/dev/ttyUSB2'],
      gpsTty:    [''],
      pollSec:   [30],
      gpsEnable: [true],
    });
  }

  ngOnInit(): void {
    this.applyData(this.ds.snapshot());
    this.loading = false;
    for (const k of this.KEYS)
      this.sub.add(this.ds.observe(k).subscribe(() => {
        if (!this.form.dirty) this.applyData(this.ds.snapshot());
      }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyData(d: Record<string, unknown>): void {
    this.form.patchValue({
      apn:       d['cell.apn']               ?? this.form.value.apn,
      modemTty:  d['cell.modem.tty']         ?? this.form.value.modemTty,
      gpsTty:    d['cell.gps.tty']           ?? this.form.value.gpsTty,
      pollSec:   d['cell.poll.interval.sec'] ?? this.form.value.pollSec,
      gpsEnable: d['cell.gps.enable']        ?? this.form.value.gpsEnable,
    });
  }

  save(): void {
    if (!this.isAdmin) return;
    this.saving = true;
    const v = this.form.value;
    this.ds.write([
      { key: 'cell.apn',               value: v.apn ?? '' },
      { key: 'cell.modem.tty',         value: v.modemTty ?? '' },
      { key: 'cell.gps.tty',           value: v.gpsTty ?? '' },
      { key: 'cell.poll.interval.sec', value: Number(v.pollSec) || 30 },
      { key: 'cell.gps.enable',        value: !!v.gpsEnable },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) { this.toast.success('Cellular config saved — restart cellular-client to apply'); this.form.markAsPristine(); }
        else this.toast.error('Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); },
    });
  }
}
