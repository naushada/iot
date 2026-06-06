import { Component, OnInit, OnDestroy } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

interface SvcRow {
  key: string; label: string; desc: string; enable_key: string;
  state: string; enabled: boolean; restarting: boolean;
  state_key?: string;   // explicit state key for services without .enable counterpart
}

@Component({
  selector: 'app-services-list',
  template: `
    <div class="page">
      <h3>Service Management</h3>

      <div class="info-card">
        <p>Cloud daemons self-report their state to the data store.
        Toggle enable to stop/start a service.  ds-server cannot be
        disabled (the IPC socket would go dead).</p>
      </div>

      <div class="svc-grid" style="margin-top:24px;">
        <div class="svc-card" *ngFor="let s of services">
          <div class="svc-header">
            <code class="svc-name">{{ s.label }}</code>
            <app-status-badge [label]="s.state||'unknown'"
              [state]="s.state||''"></app-status-badge>
          </div>
          <p class="svc-desc">{{ s.desc }}</p>

          <div class="svc-actions">
            <clr-checkbox-wrapper *ngIf="s.key !== 'ds' && s.key !== 'lwm2m.bs' && s.key !== 'lwm2m.dm'">
              <input type="checkbox" clrCheckbox
                [checked]="s.enabled"
                [disabled]="!isAdmin"
                (change)="toggleEnable(s)" />
              <label>Enabled</label>
            </clr-checkbox-wrapper>
            <span *ngIf="s.key === 'ds' || s.key === 'lwm2m.bs' || s.key === 'lwm2m.dm'" class="hint">always on</span>

            <button *ngIf="s.key !== 'ds' && s.key !== 'lwm2m.bs' && s.key !== 'lwm2m.dm' && isAdmin"
              class="btn btn-sm" [disabled]="s.restarting"
              (click)="restart(s)">
              {{ s.restarting ? '…' : 'Restart' }}
            </button>
          </div>
        </div>
      </div>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .info-card {
      background: #f0f5ff; border: 1px solid #b3d4ff; border-radius: 4px;
      padding: 12px 16px; font-size: 13px; color: #333;
    }
    .info-card code { background: #d0e4ff; padding: 1px 4px; border-radius: 2px; font-size: 12px; }
    .svc-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 16px; }
    @media (max-width: 900px) { .svc-grid { grid-template-columns: 1fr; } }
    .svc-card {
      background: #fff; border: 1px solid #e0e0e0; border-radius: 6px;
      padding: 16px;
    }
    .svc-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px; }
    .svc-name { font-size: 14px; font-weight: 600; color: #1a1a2e; background: none; }
    .svc-desc { font-size: 12px; color: #757575; margin: 0 0 12px 0; }
    .svc-actions { display: flex; align-items: center; gap: 12px; }
    .btn-sm:disabled { opacity: 0.5; cursor: not-allowed; }

    body.dark-theme {
      .svc-card { background: #1a1a2e; border-color: #2a2a4a; }
      .svc-name { color: #e0e0e0; }
      .svc-desc { color: #9e9e9e; }
      .info-card { background: rgba(46,125,50,0.08); border-color: rgba(46,125,50,0.25); color: #bdbdbd; }
      .info-card code { background: rgba(46,125,50,0.15); color: #a5d6a7; }
    }
  `]
})
export class ServicesListComponent implements OnInit, OnDestroy {
  services: SvcRow[] = [
    { key: 'ds',             label: 'ds-server',        desc: 'Data store — IPC backbone. Schema-enforced, Lua-backed key/value store.', enable_key: '', state: 'stopped', enabled: true,  restarting: false },
    { key: 'iot.cloudd',     label: 'iot-cloudd',        desc: 'Cloud daemon — LwM2M BS/DM, VPN registry, endpoint provisioning.',        enable_key: 'services.cloud.iot.cloudd.enable',       state: 'stopped', enabled: true,  restarting: false },
    { key: 'iot.httpd',      label: 'iot-httpd',         desc: 'REST API + Cloud UI server. Serves /webui/ + /api/v1/* endpoints.',       enable_key: 'services.cloud.iot.httpd.enable',        state: 'stopped', enabled: true,  restarting: false },
    { key: 'openvpn.server', label: 'openvpn-server',    desc: 'OpenVPN server — manages per-device tunnels on 10.9.0.0/24 subnet.',       enable_key: 'services.cloud.openvpn.server.enable',   state: 'stopped', enabled: true,  restarting: false },
    { key: 'lwm2m.bs',       label: 'lwm2m-bs',          desc: 'LwM2M Bootstrap Server — CoAPs endpoint on port 5684.',                   enable_key: '', state: 'stopped', enabled: true, restarting: false, state_key: 'services.cloud.lwm2m.bs.state' },
    { key: 'lwm2m.dm',       label: 'lwm2m-dm',          desc: 'LwM2M Device Management — CoAPs endpoint on port 5683.',                  enable_key: '', state: 'stopped', enabled: true, restarting: false, state_key: 'services.cloud.lwm2m.dm.state' },
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
    const keys = ['services.ds.state'];
    for (const s of this.services) {
      if (s.key === 'ds') continue;
      if (s.state_key) {
        keys.push(s.state_key);
      } else if (s.enable_key) {
        keys.push(s.enable_key, s.enable_key.replace('.enable', '.state'));
      }
    }
    this.http.dbGet(keys).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          const dsState = d['services.ds.state'];
          if (typeof dsState === 'string') this.services[0].state = dsState;
          for (const s of this.services) {
            if (s.key === 'ds') continue;
            const stateKey = s.state_key || s.enable_key.replace('.enable', '.state');
            const sv = d[stateKey];
            if (typeof sv === 'string') s.state = sv;
            if (s.enable_key) {
              const ev = d[s.enable_key];
              if (typeof ev === 'boolean') s.enabled = ev;
            }
          }
        }
        if (this.active) setTimeout(() => this.poll(), 5000);
      },
      error: () => { if (this.active) setTimeout(() => this.poll(), 5000); }
    });
  }

  toggleEnable(svc: SvcRow): void {
    if (!svc.enable_key) return;
    const newVal = !svc.enabled;
    this.http.dbSet([{ key: svc.enable_key, value: newVal }]).subscribe({
      next: (r) => {
        if (r.ok) { svc.enabled = newVal; this.toast.success(svc.label + (newVal ? ' enabled' : ' disabled')); }
        else this.toast.error(r.err || 'Toggle failed');
      },
      error: () => this.toast.error('Toggle failed')
    });
  }

  restart(svc: SvcRow): void {
    if (!svc.enable_key) return;
    svc.restarting = true;
    this.http.dbSet([{ key: svc.enable_key, value: false }]).subscribe({
      next: () => setTimeout(() => {
        this.http.dbSet([{ key: svc.enable_key, value: true }]).subscribe({
          next: () => { svc.restarting = false; this.toast.success(svc.label + ' restarted'); },
          error: () => { svc.restarting = false; this.toast.error('Restart failed'); }
        });
      }, 2000),
      error: () => { svc.restarting = false; this.toast.error('Disable failed'); }
    });
  }

  ngOnDestroy(): void { this.active = false; }
}
