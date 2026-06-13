import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';
import { DataStoreService } from '../../common/datastore.service';

@Component({
  selector: 'app-http-config',
  template: `
    <div class="page">
      <h3>HTTP Server Configuration</h3>

      <div class="info-card">
        <p>Reuses the same <code>http.*</code> schema as the device.
        All keys except <code>http.workers</code> are hot-reloaded —
        iot-httpd re-binds / rotates certs within ~2s, no restart.</p>
      </div>

      <form [formGroup]="form" (ngSubmit)="save()" *ngIf="!loading">
        <h4 style="margin-top:24px;">Listen</h4>
        <div class="form-grid">
          <clr-input-container>
            <label>IP</label>
            <input clrInput [disabled]="!isAdmin" formControlName="ip"
                   placeholder="0.0.0.0" />
            <clr-control-helper *dsDebug><app-ds-hint key="http.listen.ip"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Port</label>
            <input clrInput [disabled]="!isAdmin" type="number"
                   formControlName="port" min="1" max="65535" />
            <clr-control-helper *dsDebug><app-ds-hint key="http.listen.port"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-select-container>
            <label>Scheme</label>
            <select clrSelect [disabled]="!isAdmin" formControlName="scheme">
              <option value="http">HTTP</option>
              <option value="https">HTTPS</option>
            </select>
            <clr-control-helper *dsDebug><app-ds-hint key="http.listen.scheme"></app-ds-hint></clr-control-helper>
          </clr-select-container>
          <clr-input-container>
            <label>Worker Threads</label>
            <input clrInput [disabled]="!isAdmin" type="number"
                   formControlName="workers" min="0" max="64" />
            <clr-control-helper>0 = inline. Requires restart to change.</clr-control-helper>
            <clr-control-helper *dsDebug><app-ds-hint key="http.workers"></app-ds-hint></clr-control-helper>
          </clr-input-container>
        </div>

        <h4 style="margin-top:32px;">TLS <span class="hint">(required for https)</span></h4>
        <div class="form-grid">
          <clr-input-container>
            <label>Certificate (PEM)</label>
            <input clrInput [disabled]="!isAdmin" formControlName="cert"
                   placeholder="/etc/iot/certs/server.crt" />
            <clr-control-helper *dsDebug><app-ds-hint key="http.tls.cert"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Private Key (PEM)</label>
            <input clrInput [disabled]="!isAdmin" formControlName="key"
                   placeholder="/etc/iot/certs/server.key" />
            <clr-control-helper *dsDebug><app-ds-hint key="http.tls.key"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>CA Bundle (PEM) <span class="hint">(optional — enables mTLS)</span></label>
            <input clrInput [disabled]="!isAdmin" formControlName="ca"
                   placeholder="/etc/iot/certs/ca.crt" />
            <clr-control-helper *dsDebug><app-ds-hint key="http.tls.ca"></app-ds-hint></clr-control-helper>
          </clr-input-container>
        </div>

        <h4 style="margin-top:32px;">Auth</h4>
        <div class="form-grid">
          <clr-checkbox-container>
            <clr-checkbox-wrapper>
              <input type="checkbox" clrCheckbox [disabled]="!isAdmin"
                     [ngModel]="authEnabled"
                     (ngModelChange)="toggleAuth($event)"
                     [ngModelOptions]="{standalone: true}" />
              <label>Enable Authentication</label>
            </clr-checkbox-wrapper>
            <clr-control-helper>When disabled, all /api/v1/* routes are public</clr-control-helper>
          </clr-checkbox-container>
        </div>

        <div style="margin-top:24px;" *ngIf="isAdmin">
          <button type="submit" class="btn btn-primary" [disabled]="saving">
            {{ saving ? 'Saving…' : 'Save' }}
          </button>
        </div>
      </form>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 12px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 0 0 12px 0; font-size: 14px; font-weight: 600;
         border-bottom: 1px solid #e0e0e0; padding-bottom: 8px; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .info-card {
      background: #f0f5ff; border: 1px solid #b3d4ff; border-radius: 4px;
      padding: 12px 16px; font-size: 13px; color: #333;
    }
    .info-card code { background: #d0e4ff; padding: 1px 4px; border-radius: 2px; font-size: 12px; }
  `]
})
export class HttpConfigComponent implements OnInit {
  form: FormGroup;
  loading = true;
  saving = false;
  authEnabled = true;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpsvcService,
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService,
    private ds: DataStoreService
  ) {
    this.form = fb.group({
      ip:      ['0.0.0.0'],
      port:    [8080],
      scheme:  ['http'],
      workers: [0],
      cert:    [''],
      key:     [''],
      ca:      [''],
    });
  }

  ngOnInit(): void {
    // Instant paint from the prefetched cache, then refresh from the wire.
    if (this.ds.has('http.listen.ip')) { this.applyData(this.ds.snapshot()); this.loading = false; }
    this.http.dbGet([
      'http.listen.ip', 'http.listen.port', 'http.listen.scheme',
      'http.workers', 'http.tls.cert', 'http.tls.key', 'http.tls.ca',
      'http.auth.enabled'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) this.applyData(r.data as Record<string, unknown>);
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  private applyData(d: Record<string, unknown>): void {
    this.form.patchValue({
      ip:      d['http.listen.ip']     ?? this.form.value.ip,
      port:    d['http.listen.port']   ?? this.form.value.port,
      scheme:  d['http.listen.scheme'] ?? this.form.value.scheme,
      workers: d['http.workers']       ?? this.form.value.workers,
      cert:    d['http.tls.cert']      ?? this.form.value.cert,
      key:     d['http.tls.key']       ?? this.form.value.key,
      ca:      d['http.tls.ca']        ?? this.form.value.ca,
    });
    const ae = d['http.auth.enabled'];
    if (typeof ae === 'boolean') this.authEnabled = ae;
  }

  toggleAuth(v: boolean): void {
    this.http.dbSet([{ key: 'http.auth.enabled', value: v }]).subscribe({
      next: (r) => {
        if (r.ok) { this.authEnabled = v; this.toast.success('Auth ' + (v ? 'enabled' : 'disabled')); }
        else this.toast.error(r.err || 'Toggle failed');
      },
      error: () => this.toast.error('Toggle failed')
    });
  }

  save(): void {
    this.saving = true;
    const v = this.form.value;
    this.http.dbSet([
      { key: 'http.listen.ip',     value: v.ip },
      { key: 'http.listen.port',   value: v.port },
      { key: 'http.listen.scheme', value: v.scheme },
      { key: 'http.workers',       value: v.workers },
      { key: 'http.tls.cert',      value: v.cert },
      { key: 'http.tls.key',       value: v.key },
      { key: 'http.tls.ca',        value: v.ca },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) this.toast.success('HTTP config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
