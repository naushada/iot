import { Component, OnInit, OnDestroy } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

interface SvcRow {
  key: string; label: string; enable_key: string;
  state: string; enabled: boolean; uptime: string;
  restarting: boolean;
}

@Component({
  selector: 'app-services-list',
  template: `
    <div class="page">
      <h3>Service Management</h3>
      <clr-datagrid>
        <clr-dg-column>Service</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column>Enabled</clr-dg-column>
        <clr-dg-column *ngIf="isAdmin">Actions</clr-dg-column>

        <clr-dg-row *clrDgItems="let s of services">
          <clr-dg-cell><code>{{ s.label }}</code></clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge [label]="s.state||'unknown'"
              [state]="s.state||''"></app-status-badge>
          </clr-dg-cell>
          <clr-dg-cell>
            <input type="checkbox" [checked]="s.enabled"
              [disabled]="!isAdmin || s.key==='ds'"
              (change)="toggleEnable(s)" />
          </clr-dg-cell>
          <clr-dg-cell *ngIf="isAdmin">
            <button class="btn btn-sm"
              [disabled]="s.restarting || s.key==='ds'"
              (click)="restart(s)">
              {{ s.restarting ? '…' : 'Restart' }}
            </button>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ services.length }} services</clr-dg-footer>
      </clr-datagrid>
      <p class="hint">ds-server cannot be disabled or self-restart.  Restart: 2 s disable → enable cycle.</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .btn-sm:disabled { opacity: 0.5; cursor: not-allowed; }
    .hint { font-size: 12px; color: #757575; margin-top: 16px; }
  `]
})
export class ServicesListComponent implements OnInit, OnDestroy {
  services: SvcRow[] = [
    { key: 'ds',            label: 'ds-server',        enable_key: '',            state: '', enabled: true,  uptime: '', restarting: false },
    { key: 'iot-cloudd',    label: 'iot-cloudd',        enable_key: 'services.cloud.iot-cloudd.enable',    state: '', enabled: true,  uptime: '', restarting: false },
    { key: 'iot-httpd',     label: 'iot-httpd',         enable_key: 'services.cloud.iot-httpd.enable',     state: '', enabled: true,  uptime: '', restarting: false },
    { key: 'openvpn-server',label: 'openvpn-server',    enable_key: 'services.cloud.openvpn.server.enable',state: '', enabled: true,  uptime: '', restarting: false },
  ];
  private active = true;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpsvcService,
    private session: SessionService,
    private toast: ToastService
  ) {}

  ngOnInit(): void { this.poll(); }

  private poll(): void {
    if (!this.active) return;

    // Read service state keys for all cloud services
    const keys = ['services.ds.state'];
    for (const s of this.services) {
      if (s.key === 'ds') continue;
      keys.push(s.enable_key, s.enable_key.replace('.enable', '.state'));
    }

    this.http.dbGet(keys).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          // ds-server
          const dsState = d['services.ds.state'];
          if (typeof dsState === 'string') this.services[0].state = dsState;
          // Cloud daemons
          for (const s of this.services) {
            if (s.key === 'ds') continue;
            const stateVal = d[s.enable_key.replace('.enable', '.state')];
            const enVal    = d[s.enable_key];
            if (typeof stateVal === 'string') s.state = stateVal;
            if (typeof enVal === 'boolean') s.enabled = enVal;
          }
        }
        if (this.active) setTimeout(() => this.poll(), 5000);
      },
      error: () => {
        if (this.active) setTimeout(() => this.poll(), 5000);
      }
    });
  }

  toggleEnable(svc: SvcRow): void {
    if (!svc.enable_key) return;
    const newVal = !svc.enabled;
    this.http.dbSet([{ key: svc.enable_key, value: newVal }]).subscribe({
      next: (r) => {
        if (r.ok) {
          svc.enabled = newVal;
          this.toast.success(svc.label + (newVal ? ' enabled' : ' disabled'));
        } else {
          this.toast.error(r.err || 'Toggle failed');
        }
      },
      error: () => this.toast.error('Toggle failed')
    });
  }

  restart(svc: SvcRow): void {
    if (!svc.enable_key) return;
    svc.restarting = true;
    // Disable → 2 s → enable
    this.http.dbSet([{ key: svc.enable_key, value: false }]).subscribe({
      next: () => {
        setTimeout(() => {
          this.http.dbSet([{ key: svc.enable_key, value: true }]).subscribe({
            next: () => {
              svc.restarting = false;
              this.toast.success(svc.label + ' restarted');
            },
            error: () => {
              svc.restarting = false;
              this.toast.error(svc.label + ' restart failed');
            }
          });
        }, 2000);
      },
      error: () => {
        svc.restarting = false;
        this.toast.error(svc.label + ' disable failed');
      }
    });
  }

  ngOnDestroy(): void { this.active = false; }
}
