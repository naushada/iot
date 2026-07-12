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
          <clr-select-container>
            <label>Radio Access Tech</label>
            <select clrSelect [disabled]="!isAdmin" formControlName="rat">
              <option value="">Leave unchanged</option>
              <option value="auto">Automatic</option>
              <option value="gsm">GSM (2G) only</option>
              <option value="umts">UMTS (3G) only</option>
              <option value="lte">LTE only</option>
              <option value="gsm+lte">GSM + LTE</option>
              <option value="gsm+umts+lte">GSM + UMTS + LTE</option>
            </select>
            <clr-control-helper>Applied via AT!SELRAT (Sierra). Pick GSM if only 2G is in range.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="cell.rat"></app-ds-hint></clr-control-helper>
          </clr-select-container>
          <clr-checkbox-container>
            <label>GNSS</label>
            <clr-checkbox-wrapper>
              <input type="checkbox" clrCheckbox [disabled]="!isAdmin"
                     formControlName="gpsEnable" />
              <label>Enable GPS</label>
            </clr-checkbox-wrapper>
            <clr-control-helper *dsDebug><app-ds-hint key="cell.gps.enable"></app-ds-hint></clr-control-helper>
          </clr-checkbox-container>
          <clr-checkbox-container>
            <label>SMS</label>
            <clr-checkbox-wrapper>
              <input type="checkbox" clrCheckbox [disabled]="!isAdmin"
                     formControlName="smsEnable" />
              <label>Receive SMS</label>
            </clr-checkbox-wrapper>
            <clr-control-helper *dsDebug><app-ds-hint key="sms.enable"></app-ds-hint></clr-control-helper>
          </clr-checkbox-container>
          <!-- pad to fill the 4-column row (Project Rule 5) -->
          <div></div>
        </div>

        <h4>SMS Device Control</h4>
        <div class="gate" *ngIf="!modemReady">
          <clr-icon shape="disconnect"></clr-icon>
          <span>
            SMS control needs a modem that is enumerated <em>and</em> registered on the
            network — the device receives its commands as SMS. Current modem state:
            <strong>{{ cellState || 'absent' }}</strong>. These fields unlock once it
            reaches <code>registered</code> / <code>connected</code>.
          </span>
        </div>
        <p class="hint">
          Lets an operator control this device by texting it — the last channel
          left when the device has no IP path. Commands are authenticated with a
          device login: <code>IOT LOGIN &lt;user&gt; &lt;password&gt;</code>, then
          <code>IOT STATUS</code> / <code>IOT WIFI "&lt;ssid&gt;" "&lt;psk&gt;"</code> /
          <code>IOT APN &lt;apn&gt;</code> / <code>IOT RADIO RESTART</code> /
          <code>IOT REBOOT</code> / <code>IOT FACTORY-RESET</code>.
          <strong>The password crosses the carrier in plaintext</strong> — prefer a
          dedicated Admin account, and set an allowlist so unknown senders are
          ignored without a reply.
        </p>
        <div class="form-grid">
          <clr-checkbox-container>
            <label>SMS Control</label>
            <clr-checkbox-wrapper>
              <input type="checkbox" clrCheckbox formControlName="smsctlEnabled" />
              <label>Accept IOT commands</label>
            </clr-checkbox-wrapper>
            <clr-control-helper *dsDebug><app-ds-hint key="smsctl.enabled"></app-ds-hint></clr-control-helper>
          </clr-checkbox-container>
          <clr-input-container>
            <label>Allowed Numbers</label>
            <input clrInput formControlName="smsctlAllowed"
                   [placeholder]="modemReady ? 'e.g. +919096383701,+4915112345678' : ''" />
            <clr-control-helper>Comma-separated. Empty = any sender may attempt login.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="smsctl.allowed.numbers"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Session TTL (s)</label>
            <input clrInput type="number" min="60" max="86400"
                   formControlName="smsctlTtl" />
            <clr-control-helper>How long a login stays valid.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="smsctl.session.ttl.sec"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Lockout After</label>
            <input clrInput type="number" min="1" max="20"
                   formControlName="smsctlFailures" />
            <clr-control-helper>Failed logins per number before lockout.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="smsctl.lockout.failures"></app-ds-hint></clr-control-helper>
          </clr-input-container>
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
    h4 { font-size: 14px; font-weight: 600; color: #333; margin: 28px 0 6px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 20px 0; }
    .gate { display: flex; align-items: flex-start; gap: 6px; margin: 0 0 12px 0;
            padding: 8px 12px; border-radius: 4px; font-size: 13px; line-height: 1.5;
            color: #8a6d3b; background: #fcf8e3; border: 1px solid #faebcc; }
    /* The message is one flex item; without the wrapping span each inline
       tag (em/strong/code) would become its own item and break the sentence. */
    .gate clr-icon { flex: none; margin-top: 2px; }
    code { background: #eef2f7; padding: 0 4px; border-radius: 3px; }
  `]
})
export class CellularConfigComponent implements OnInit, OnDestroy {
  form: FormGroup;
  loading = true;
  saving = false;
  /// Live modem lifecycle (cell.state), off the shared /status stream.
  cellState = '';
  private sub = new Subscription();
  private readonly KEYS = [
    'cell.apn', 'cell.modem.tty', 'cell.gps.tty',
    'cell.poll.interval.sec', 'cell.gps.enable', 'cell.rat', 'sms.enable',
    'smsctl.enabled', 'smsctl.allowed.numbers', 'smsctl.session.ttl.sec',
    'smsctl.lockout.failures',
  ];

  get isAdmin(): boolean { return this.session.isAdmin; }

  /// SMS control is only meaningful when the modem is enumerated AND on the
  /// network: the device's only inbound channel for these commands is MT-SMS,
  /// which needs registration. Offering the fields on a device with no modem
  /// (or one that never registered) invites an operator to "enable" a feature
  /// that silently cannot work.
  get modemReady(): boolean {
    return this.cellState === 'registered' || this.cellState === 'connected';
  }

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
      rat:       [''],
      smsEnable: [false],
      smsctlEnabled:  [false],
      smsctlAllowed:  [''],
      smsctlTtl:      [600],
      smsctlFailures: [5],
    });
  }

  /// Controls the modem gate gets to lock. Reactive forms IGNORE a [disabled]
  /// binding on a formControlName element (FormControlName eats it as an @Input
  /// and only logs a warning), so the gate has to be driven through the control
  /// API or the fields stay editable behind the "locked" banner.
  private readonly SMSCTL_CONTROLS = [
    'smsctlEnabled', 'smsctlAllowed', 'smsctlTtl', 'smsctlFailures',
  ];

  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((s) => {
      this.cellState = s.cell?.state || '';
      this.syncSmsCtlGate();
    }));
    this.applyData(this.ds.snapshot());
    this.syncSmsCtlGate();
    this.loading = false;
    for (const k of this.KEYS)
      this.sub.add(this.ds.observe(k).subscribe(() => {
        if (!this.form.dirty) this.applyData(this.ds.snapshot());
      }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  /// Lock the SMS-control fields unless the operator is an Admin AND the modem
  /// is registered — enabling a channel that physically cannot deliver is a
  /// silent no-op. emitEvent:false so toggling the gate never marks the form
  /// dirty, which would stop live ds updates from re-applying.
  private syncSmsCtlGate(): void {
    const unlocked = this.isAdmin && this.modemReady;
    for (const name of this.SMSCTL_CONTROLS) {
      const c = this.form.get(name);
      if (!c) continue;
      if (unlocked && c.disabled) c.enable({ emitEvent: false });
      else if (!unlocked && c.enabled) c.disable({ emitEvent: false });
    }
  }

  private applyData(d: Record<string, unknown>): void {
    // getRawValue(), not value: a disabled control is omitted from .value, so
    // the ?? fallbacks would read undefined once the gate locks the fields.
    const cur = this.form.getRawValue();
    this.form.patchValue({
      apn:       d['cell.apn']               ?? cur.apn,
      modemTty:  d['cell.modem.tty']         ?? cur.modemTty,
      gpsTty:    d['cell.gps.tty']           ?? cur.gpsTty,
      pollSec:   d['cell.poll.interval.sec'] ?? cur.pollSec,
      gpsEnable: d['cell.gps.enable']        ?? cur.gpsEnable,
      rat:       d['cell.rat']               ?? cur.rat,
      smsEnable: d['sms.enable']             ?? cur.smsEnable,
      smsctlEnabled:  d['smsctl.enabled']           ?? cur.smsctlEnabled,
      smsctlAllowed:  d['smsctl.allowed.numbers']   ?? cur.smsctlAllowed,
      smsctlTtl:      d['smsctl.session.ttl.sec']   ?? cur.smsctlTtl,
      smsctlFailures: d['smsctl.lockout.failures']  ?? cur.smsctlFailures,
    });
  }

  save(): void {
    if (!this.isAdmin) return;
    this.saving = true;
    // Raw: the SMS-control fields are disabled while the modem is down, and a
    // disabled control is dropped from .value — saving would blank them out.
    const v = this.form.getRawValue();
    this.ds.write([
      { key: 'cell.apn',               value: v.apn ?? '' },
      { key: 'cell.modem.tty',         value: v.modemTty ?? '' },
      { key: 'cell.gps.tty',           value: v.gpsTty ?? '' },
      { key: 'cell.poll.interval.sec', value: Number(v.pollSec) || 30 },
      { key: 'cell.gps.enable',        value: !!v.gpsEnable },
      { key: 'cell.rat',               value: v.rat ?? '' },
      { key: 'sms.enable',             value: !!v.smsEnable },
      { key: 'smsctl.enabled',           value: !!v.smsctlEnabled },
      { key: 'smsctl.allowed.numbers',   value: v.smsctlAllowed ?? '' },
      { key: 'smsctl.session.ttl.sec',   value: Number(v.smsctlTtl) || 600 },
      { key: 'smsctl.lockout.failures',  value: Number(v.smsctlFailures) || 5 },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) { this.toast.success('Saved — SMS control applies immediately; cell.* needs a cellular-client restart (or use Restart Module)'); this.form.markAsPristine(); }
        else this.toast.error('Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); },
    });
  }
}
