import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { DataStoreService } from '../../../common/datastore.service';
import { SessionService } from '../../../common/session.service';
import { StatusSnapshot, ServiceInfo } from '../../../common/app-globals';

interface SvcRow { key: string; name: string; label: string; info: ServiceInfo; restarting: boolean; msg: string; }

@Component({
  selector: 'app-services-list',
  template: `
    <div class="page">
      <h3>Service Management</h3>
      <clr-datagrid>
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
        <clr-dg-column>Uptime</clr-dg-column>
        <clr-dg-column *ngIf="isAdmin">Actions</clr-dg-column>

        <clr-dg-row *clrDgItems="let s of services">
          <clr-dg-cell>
            <span class="svc-name">{{ s.name }}</span>
            <code class="svc-key" [title]="s.label + '.state'">{{ s.label }}</code>
          </clr-dg-cell>
          <clr-dg-cell><app-status-badge [label]="s.info.state||'unknown'" [state]="s.info.state||''"></app-status-badge></clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? fmtCpu(s.info.cpu_permille) : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? s.info.cpu_count : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? fmtKb(s.info.mem_kb) : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? s.info.fd_count : '—' }}</clr-dg-cell>
          <clr-dg-cell class="num">{{ hasStats(s) ? s.info.threads : '—' }}</clr-dg-cell>
          <clr-dg-cell>
            <input type="checkbox" [checked]="s.info.enable" [disabled]="!isAdmin" (change)="toggleEnable(s)" />
          </clr-dg-cell>
          <clr-dg-cell>{{ s.info.uptime_sec != null ? s.info.uptime_sec + 's' : '—' }}</clr-dg-cell>
          <clr-dg-cell *ngIf="isAdmin">
            <button class="btn btn-sm" [disabled]="s.restarting || s.key==='ds'" (click)="restart(s)">
              {{ s.restarting ? '…' : 'Restart' }}
            </button>
            <span *ngIf="s.msg" style="margin-left:8px;font-size:11px;"
                  [style.color]="s.msg.startsWith('Restarted')?'#66bb6a':'#c62828'">{{ s.msg }}</span>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ services.length }} services</clr-dg-footer>
      </clr-datagrid>
      <p class="hint">Restart: disable → 2 s wait → enable. ds-server cannot self-restart.</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .svc-name { display: block; font-weight: 600; }
    .svc-key { display: block; font-size: 11px; color: #9e9e9e; }
    .num { text-align: left; font-variant-numeric: tabular-nums; }
    .btn-sm:disabled { opacity: 0.5; cursor: not-allowed; }
    .hint { font-size: 12px; color: #757575; margin-top: 16px; }
  `]
})
export class ServicesListComponent implements OnInit, OnDestroy {
  services: SvcRow[] = [
    { key: 'ds',             name: 'Data Store',     label: 'services.ds',             info: {}, restarting: false, msg: '' },
    { key: 'net_router',     name: 'Network Router', label: 'services.net.router',     info: {}, restarting: false, msg: '' },
    { key: 'openvpn_client', name: 'OpenVPN Client', label: 'services.openvpn.client', info: {}, restarting: false, msg: '' },
    { key: 'lwm2m_client',   name: 'LwM2M Client',   label: 'services.lwm2m.client',   info: {}, restarting: false, msg: '' },
    { key: 'wifi_client',    name: 'Wi-Fi Client',   label: 'services.wifi.client',    info: {}, restarting: false, msg: '' },
    { key: 'vehicle',        name: 'Vehicle (OBD-II)', label: 'services.vehicle',     info: {}, restarting: false, msg: '' },
    { key: 'mqtt',           name: 'MQTT Mirror',    label: 'services.mqtt',           info: {}, restarting: false, msg: '' },
    { key: 'container',      name: 'Containers',     label: 'services.container',      info: {}, restarting: false, msg: '' },
  ];
  private sub = new Subscription();

    get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private ds: DataStoreService,
              private session: SessionService) {}

  /// cpu_count==0 ⇒ no live StatsPublisher for this service (not separately
  /// measured) → show "—" rather than misleading zeros.
  hasStats(s: SvcRow): boolean { return ((s.info.cpu_count as number) ?? 0) > 0; }

  fmtCpu(permille?: number): string {
    if (permille == null) return '—';
    return (permille / 10).toFixed(1) + ' %';
  }

  fmtKb(kb?: number): string {
    if (kb == null) return '—';
    if (kb < 1024) return kb + ' KB';
    return (kb / 1024).toFixed(1) + ' MB';
  }

  /// Read service state/telemetry live off the single shared /status stream
  /// — no per-page long-poll. Replayed snapshot → instant paint on revisit.
  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((s) => this.applyStatus(s)));
  }

  applyStatus(s: StatusSnapshot): void {
    const svcs = s.services as Record<string, ServiceInfo> | undefined;
    if (!svcs) return;
    for (const row of this.services) {
      if (svcs[row.key]) row.info = { ...svcs[row.key] };
    }
  }

  toggleEnable(svc: SvcRow): void {
    const newVal = !svc.info.enable;
    const enableKey = this.enableKey(svc.key);
    if (!enableKey) return;
    this.http.dbSet([{ key: enableKey, value: newVal }]).subscribe({
      next: () => { svc.info.enable = newVal; }
    });
  }

  restart(svc: SvcRow): void {
    svc.restarting = true; svc.msg = '';
    const svcName = svc.label.replace('services.', '');
    this.http.restartService(svcName).subscribe({
      next: (r) => {
        svc.restarting = false;
        svc.msg = r.ok ? 'Restarted' : 'Error: ' + (r.err || 'unknown');
        if (r.ok) setTimeout(() => { svc.msg = ''; }, 3000);
      },
      error: () => { svc.restarting = false; svc.msg = 'Request failed'; }
    });
  }

  private enableKey(k: string): string {
    const m: Record<string, string> = {
      net_router: 'services.net.router.enable',
      openvpn_client: 'services.openvpn.client.enable',
      lwm2m_client: 'services.lwm2m.client.enable',
      wifi_client: 'services.wifi.client.enable',
      vehicle: 'services.vehicle.enable',
      mqtt: 'services.mqtt.enable',
    };
    return m[k] || '';
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
