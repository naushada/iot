import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

/// Dynamic DNS config — keeps a public DNS A record pointed at the device's
/// current public IP, written to ddns.* (read by the iot-ddnsd daemon). Four
/// provider backends; the provider-specific fields show conditionally. Secrets
/// (tokens / secret keys) are write-only: blank on save keeps the stored value.
@Component({
  selector: 'app-ddns-config',
  template: `
    <div class="page">
      <h3>Dynamic DNS</h3>
      <p class="hint">
        Keep a public hostname pointed at this device's changing public IP. The
        daemon detects the IP via an HTTPS echo and updates the record through
        your provider. Stays parked until enabled with a hostname + credentials.
      </p>

      <!-- ── status ─────────────────────────────────────────────── -->
      <div class="status-tile">
        <div><span class="k">State</span>
          <span class="v" [class.err]="status.state === 'error'">{{ status.state || 'unknown' }}</span></div>
        <div><span class="k">Public IP</span><span class="v">{{ status.ip || '—' }}</span></div>
        <div><span class="k">Last update</span><span class="v">{{ lastOk() }}</span></div>
        <div *ngIf="status.error"><span class="k">Last error</span>
          <span class="v err">{{ status.error }}</span></div>
        <button class="btn btn-sm btn-link" (click)="refresh()">Refresh</button>
      </div>

      <form [formGroup]="form" (ngSubmit)="save()">
        <div class="form-grid">
          <clr-select-container>
            <label>Provider</label>
            <select clrSelect formControlName="provider" [disabled]="!isAdmin">
              <option value="dyndns2">dyndns2 (generic: No-IP / Dynu / deSEC …)</option>
              <option value="duckdns">DuckDNS</option>
              <option value="cloudflare">Cloudflare</option>
              <option value="route53">AWS Route53</option>
            </select>
          </clr-select-container>
          <clr-input-container>
            <label>Hostname</label>
            <input clrInput formControlName="hostname" [disabled]="!isAdmin" placeholder="dev-1234.example.com" />
          </clr-input-container>
          <clr-input-container>
            <label>Interval (s)</label>
            <input clrInput type="number" formControlName="interval" [disabled]="!isAdmin" />
          </clr-input-container>
          <clr-select-container>
            <label>IP source</label>
            <select clrSelect formControlName="ipSource" [disabled]="!isAdmin">
              <option value="echo">echo (HTTPS IP lookup)</option>
              <option value="dyndns2-auto">provider auto-detect</option>
            </select>
          </clr-select-container>

          <!-- dyndns2 -->
          <ng-container *ngIf="form.value.provider === 'dyndns2'">
            <clr-input-container>
              <label>Update server</label>
              <input clrInput formControlName="dyndns2Server" [disabled]="!isAdmin" placeholder="members.dyndns.org" />
            </clr-input-container>
            <clr-input-container>
              <label>Username</label>
              <input clrInput formControlName="dyndns2User" [disabled]="!isAdmin" />
            </clr-input-container>
            <clr-input-container>
              <label>Token</label>
              <input clrInput type="password" formControlName="dyndns2Token" [disabled]="!isAdmin"
                     autocomplete="new-password" placeholder="(unchanged)" />
            </clr-input-container>
          </ng-container>

          <!-- duckdns -->
          <ng-container *ngIf="form.value.provider === 'duckdns'">
            <clr-input-container>
              <label>Domains (label)</label>
              <input clrInput formControlName="duckdnsDomains" [disabled]="!isAdmin" placeholder="mydev" />
            </clr-input-container>
            <clr-input-container>
              <label>Token</label>
              <input clrInput type="password" formControlName="duckdnsToken" [disabled]="!isAdmin"
                     autocomplete="new-password" placeholder="(unchanged)" />
            </clr-input-container>
          </ng-container>

          <!-- cloudflare -->
          <ng-container *ngIf="form.value.provider === 'cloudflare'">
            <clr-input-container>
              <label>Zone ID</label>
              <input clrInput formControlName="cfZoneId" [disabled]="!isAdmin" />
            </clr-input-container>
            <clr-input-container>
              <label>API token</label>
              <input clrInput type="password" formControlName="cfToken" [disabled]="!isAdmin"
                     autocomplete="new-password" placeholder="(unchanged)" />
            </clr-input-container>
          </ng-container>

          <!-- route53 -->
          <ng-container *ngIf="form.value.provider === 'route53'">
            <clr-input-container>
              <label>Hosted Zone ID</label>
              <input clrInput formControlName="r53ZoneId" [disabled]="!isAdmin" />
            </clr-input-container>
            <clr-input-container>
              <label>Access Key ID</label>
              <input clrInput formControlName="r53AccessKey" [disabled]="!isAdmin" />
            </clr-input-container>
            <clr-input-container>
              <label>Secret Access Key</label>
              <input clrInput type="password" formControlName="r53SecretKey" [disabled]="!isAdmin"
                     autocomplete="new-password" placeholder="(unchanged)" />
            </clr-input-container>
          </ng-container>
        </div>

        <clr-checkbox-container>
          <clr-checkbox-wrapper>
            <input type="checkbox" clrCheckbox formControlName="enabled" [disabled]="!isAdmin" />
            <label>Enable Dynamic DNS</label>
          </clr-checkbox-wrapper>
        </clr-checkbox-container>

        <div style="margin-top:16px;" *ngIf="isAdmin">
          <button type="submit" class="btn btn-primary" [disabled]="saving">
            {{ saving ? 'Saving…' : 'Save' }}
          </button>
        </div>
      </form>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 12px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 16px 0; }
    .status-tile { background:#f5f5f5; border:1px solid #e0e0e0; border-radius:4px;
                   padding:12px 16px; margin-bottom:20px; max-width:520px; }
    .status-tile > div { display:flex; gap:8px; padding:2px 0; font-size:13px; }
    .status-tile .k { width:110px; color:#888; }
    .status-tile .v { color:#333; }
    .status-tile .v.err { color:#c92100; }
  `]
})
export class DdnsConfigComponent implements OnInit {
  form: FormGroup;
  saving = false;
  status: { state: string; ip: string; okTs: number; error: string } =
    { state: '', ip: '', okTs: 0, error: '' };

  get isAdmin(): boolean { return this.session.isAdmin; }

  // Non-secret keys (read back on load). Secrets are deliberately excluded.
  private readonly READ_KEYS = [
    'ddns.enabled', 'ddns.provider', 'ddns.hostname', 'ddns.interval', 'ddns.ip.source',
    'ddns.dyndns2.server', 'ddns.dyndns2.user', 'ddns.duckdns.domains',
    'ddns.cf.zone.id', 'ddns.r53.zone.id', 'ddns.r53.access.key',
  ];
  private readonly STATUS_KEYS = ['ddns.state', 'ddns.last.ip', 'ddns.last.ok.ts', 'ddns.last.error'];

  constructor(private http: HttpsvcService, fb: FormBuilder,
              private session: SessionService, private toast: ToastService) {
    this.form = fb.group({
      provider: ['dyndns2'], hostname: [''], interval: [300], ipSource: ['echo'], enabled: [false],
      dyndns2Server: ['members.dyndns.org'], dyndns2User: [''], dyndns2Token: [''],
      duckdnsDomains: [''], duckdnsToken: [''],
      cfZoneId: [''], cfToken: [''],
      r53ZoneId: [''], r53AccessKey: [''], r53SecretKey: [''],
    });
  }

  ngOnInit(): void {
    this.http.dbGet(this.READ_KEYS).subscribe(r => {
      if (!r.ok || !r.data) return;
      const d = r.data;
      this.form.patchValue({
        provider:       (d['ddns.provider'] as string) || 'dyndns2',
        hostname:       (d['ddns.hostname'] as string) || '',
        interval:       (d['ddns.interval'] as number) || 300,
        ipSource:       (d['ddns.ip.source'] as string) || 'echo',
        dyndns2Server:  (d['ddns.dyndns2.server'] as string) || 'members.dyndns.org',
        dyndns2User:    (d['ddns.dyndns2.user'] as string) || '',
        duckdnsDomains: (d['ddns.duckdns.domains'] as string) || '',
        cfZoneId:       (d['ddns.cf.zone.id'] as string) || '',
        r53ZoneId:      (d['ddns.r53.zone.id'] as string) || '',
        r53AccessKey:   (d['ddns.r53.access.key'] as string) || '',
        enabled:        d['ddns.enabled'] === true,
      });
    });
    this.refresh();
  }

  refresh(): void {
    this.http.dbGet(this.STATUS_KEYS).subscribe(r => {
      if (!r.ok || !r.data) return;
      const d = r.data;
      this.status = {
        state: (d['ddns.state'] as string) || '',
        ip:    (d['ddns.last.ip'] as string) || '',
        okTs:  (d['ddns.last.ok.ts'] as number) || 0,
        error: (d['ddns.last.error'] as string) || '',
      };
    });
  }

  lastOk(): string {
    if (!this.status.okTs) return '—';
    return new Date(this.status.okTs * 1000).toLocaleString();
  }

  save(): void {
    if (!this.isAdmin) return;
    const v = this.form.value;
    const pairs: { key: string; value: unknown }[] = [
      { key: 'ddns.provider',       value: v.provider || 'dyndns2' },
      { key: 'ddns.hostname',       value: v.hostname || '' },
      { key: 'ddns.interval',       value: Number(v.interval) || 300 },
      { key: 'ddns.ip.source',      value: v.ipSource || 'echo' },
      { key: 'ddns.dyndns2.server', value: v.dyndns2Server || 'members.dyndns.org' },
      { key: 'ddns.dyndns2.user',   value: v.dyndns2User || '' },
      { key: 'ddns.duckdns.domains',value: v.duckdnsDomains || '' },
      { key: 'ddns.cf.zone.id',     value: v.cfZoneId || '' },
      { key: 'ddns.r53.zone.id',    value: v.r53ZoneId || '' },
      { key: 'ddns.r53.access.key', value: v.r53AccessKey || '' },
      { key: 'ddns.enabled',        value: !!v.enabled },
    ];
    // Secrets: only pushed when the operator typed a new value.
    if (v.dyndns2Token) pairs.push({ key: 'ddns.dyndns2.token', value: v.dyndns2Token });
    if (v.duckdnsToken) pairs.push({ key: 'ddns.duckdns.token', value: v.duckdnsToken });
    if (v.cfToken)      pairs.push({ key: 'ddns.cf.token',      value: v.cfToken });
    if (v.r53SecretKey) pairs.push({ key: 'ddns.r53.secret.key', value: v.r53SecretKey });

    this.saving = true;
    this.http.dbSet(pairs).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) { this.toast.success('DDNS settings saved'); this.refresh(); }
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
