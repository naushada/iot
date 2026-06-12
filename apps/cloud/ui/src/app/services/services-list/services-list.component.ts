import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { DataStoreService } from '../../../common/datastore.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

interface SvcRow {
  key: string; label: string; desc: string; enable_key: string;
  state: string; enabled: boolean; restarting: boolean;
  state_key?: string;   // explicit state key for services without .enable counterpart
  // L22 — resource telemetry (per-container cgroup + /proc/self/fd).
  cpu_permille?: number; cpu_count?: number; mem_kb?: number; fd_count?: number; threads?: number;
}

@Component({
  selector: 'app-services-list',
  template: `
    <div class="page">
      <h3>Service Management</h3>

      <div class="info-card">
        <p>Cloud daemons self-report their state and resource usage to the
        data store. Toggle enable to stop/start a service.  ds-server cannot
        be disabled (the IPC socket would go dead).</p>
      </div>

      <clr-datagrid style="margin-top:16px;">
        <clr-dg-column>Service</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column [style.text-align]="'left'"
          title="Percent of one CPU core; can exceed 100% across multiple cores">CPU %</clr-dg-column>
        <clr-dg-column [style.text-align]="'left'"
          title="Cores available to the container">#CPU</clr-dg-column>
        <clr-dg-column [style.text-align]="'left'">Memory</clr-dg-column>
        <clr-dg-column [style.text-align]="'left'">FDs</clr-dg-column>
        <clr-dg-column [style.text-align]="'left'">Threads</clr-dg-column>
        <clr-dg-column>Enabled</clr-dg-column>
        <clr-dg-column *ngIf="isAdmin">Actions</clr-dg-column>

        <clr-dg-row *clrDgItems="let s of services">
          <clr-dg-cell><code>{{ s.label }}</code></clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge [label]="s.state||'unknown'"
              [state]="s.state||''"></app-status-badge>
          </clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? fmtCpu(s.cpu_permille) : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? s.cpu_count : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? fmtKb(s.mem_kb) : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? s.fd_count : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? s.threads : '—' }}</clr-dg-cell>
          <clr-dg-cell>
            <clr-checkbox-wrapper *ngIf="gateable(s)">
              <input type="checkbox" clrCheckbox
                [checked]="s.enabled"
                [disabled]="!isAdmin"
                (change)="toggleEnable(s)" />
              <label></label>
            </clr-checkbox-wrapper>
            <span *ngIf="!gateable(s)" class="hint">always on</span>
          </clr-dg-cell>
          <clr-dg-cell *ngIf="isAdmin">
            <button *ngIf="gateable(s)"
              class="btn btn-sm" [disabled]="s.restarting"
              (click)="restart(s)">
              {{ s.restarting ? '…' : 'Restart' }}
            </button>
            <span *ngIf="!gateable(s)" class="hint">—</span>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ services.length }} services</clr-dg-footer>
      </clr-datagrid>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .num { text-align: left; font-variant-numeric: tabular-nums; }
    code { background: none; }
    .info-card {
      background: #f0f5ff; border: 1px solid #b3d4ff; border-radius: 4px;
      padding: 12px 16px; font-size: 13px; color: #333;
    }
    .info-card code { background: #d0e4ff; padding: 1px 4px; border-radius: 2px; font-size: 12px; }
    .btn-sm:disabled { opacity: 0.5; cursor: not-allowed; }

    body.dark-theme .info-card { background: rgba(46,125,50,0.08); border-color: rgba(46,125,50,0.25); color: #bdbdbd; }
    body.dark-theme .info-card code { background: rgba(46,125,50,0.15); color: #a5d6a7; }
  `]
})
export class ServicesListComponent implements OnInit, OnDestroy {
  services: SvcRow[] = [
    { key: 'ds',             label: 'ds-server',        desc: 'Data store — IPC backbone.',                       enable_key: '', state: 'stopped', enabled: true,  restarting: false },
    { key: 'iot.cloudd',     label: 'iot-cloudd',        desc: 'Cloud daemon — LwM2M BS/DM, VPN, provisioning.',  enable_key: 'services.cloud.iot.cloudd.enable',       state: 'stopped', enabled: true,  restarting: false },
    { key: 'iot.httpd',      label: 'iot-httpd',         desc: 'REST API + Cloud UI server.',                     enable_key: 'services.cloud.iot.httpd.enable',        state: 'stopped', enabled: true,  restarting: false },
    { key: 'openvpn.server', label: 'openvpn-server',    desc: 'OpenVPN server — per-device tunnels.',            enable_key: 'services.cloud.openvpn.server.enable',   state: 'stopped', enabled: true,  restarting: false },
    { key: 'lwm2m.bs',       label: 'lwm2m-bs',          desc: 'LwM2M Bootstrap Server (CoAPs 5684).',            enable_key: '', state: 'stopped', enabled: true, restarting: false, state_key: 'services.cloud.lwm2m.bs.state' },
    { key: 'lwm2m.dm',       label: 'lwm2m-dm',          desc: 'LwM2M Device Management (CoAPs 5683).',           enable_key: '', state: 'stopped', enabled: true, restarting: false, state_key: 'services.cloud.lwm2m.dm.state' },
  ];
  private sub?: Subscription;   // shared-cache observer, cancelled on destroy

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpsvcService,
    private ds: DataStoreService,
    private session: SessionService,
    private toast: ToastService
  ) {}

  /// Paint instantly from the prefetched cache, then re-apply on every
  /// services.stats.version bump pushed by the shared /status long-poll.
  /// No per-page long-poll → no worker starvation on revisit.
  ngOnInit(): void {
    this.applyState(this.ds.snapshot());
    this.sub = this.ds.observe('services.stats.version')
      .subscribe(() => this.applyState(this.ds.snapshot()));
  }

  /// Services that can be enabled/disabled + restarted (have an enable key).
  gateable(s: SvcRow): boolean { return !!s.enable_key; }

  /// A live StatsPublisher always reports >=1 core, so cpu_count==0 means
  /// the service isn't separately measured (e.g. openvpn-server, which is
  /// managed by iot-cloudd and has no process of its own) — show "—".
  hasStats(s: SvcRow): boolean { return (s.cpu_count ?? 0) > 0; }

  /// ds key prefix for a service's L22 telemetry keys.
  private statsPrefix(s: SvcRow): string {
    return s.key === 'ds' ? 'services.ds' : 'services.cloud.' + s.key;
  }

  fmtCpu(permille?: number): string {
    if (permille == null) return '—';
    return (permille / 10).toFixed(1) + ' %';
  }

  fmtKb(kb?: number): string {
    if (kb == null) return '—';
    if (kb < 1024) return kb + ' KB';
    return (kb / 1024).toFixed(1) + ' MB';
  }

  private stateKeyOf(s: SvcRow): string {
    return s.state_key || s.enable_key.replace('.enable', '.state');
  }

  /// Map the flat ds-key cache into the service rows (state + enable +
  /// telemetry). Source is the shared DataStore snapshot — no HTTP here.
  private applyState(d: Record<string, unknown>): void {
    const num = (k: string): number | undefined => {
      const v = d[k];
      return typeof v === 'number' ? v : undefined;
    };
    for (const s of this.services) {
      if (s.key === 'ds') {
        const sv = d['services.ds.state'];
        if (typeof sv === 'string') s.state = sv;
      } else {
        const sv = d[this.stateKeyOf(s)];
        if (typeof sv === 'string') s.state = sv;
        if (s.enable_key) {
          const ev = d[s.enable_key];
          if (typeof ev === 'boolean') s.enabled = ev;
        }
      }
      const p = this.statsPrefix(s);
      s.cpu_permille = num(p + '.cpu.permille');
      s.cpu_count    = num(p + '.cpu.count');
      s.mem_kb       = num(p + '.mem.rss.kb');
      s.fd_count     = num(p + '.fd.count');
      s.threads      = num(p + '.threads');
    }
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

  ngOnDestroy(): void { this.sub?.unsubscribe(); }
}
