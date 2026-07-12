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
            <input *ngIf="isGated(s)" type="checkbox" [checked]="s.info.enable"
                   [disabled]="!isAdmin" (change)="toggleEnable(s)" />
            <span *ngIf="!isGated(s)" class="na" title="This daemon has no enable gate — use Restart">—</span>
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
      <p class="hint">
        Rows are discovered live — any daemon publishing <code>services.&lt;name&gt;.state</code> appears here.
        Restart: gated daemons flip their enable key (disable → 2 s → enable); the rest get a systemd restart.
        ds-server cannot self-restart. “—” under Enabled means the daemon has no enable gate.
      </p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .svc-name { display: block; font-weight: 600; }
    .svc-key { display: block; font-size: 11px; color: #9e9e9e; }
    .num { text-align: left; font-variant-numeric: tabular-nums; }
    .na { color: #9e9e9e; }
    .btn-sm:disabled { opacity: 0.5; cursor: not-allowed; }
    .hint { font-size: 12px; color: #757575; margin-top: 16px; }
  `]
})
export class ServicesListComponent implements OnInit, OnDestroy {
  /// Rows are built from whatever /status reports — NOT a hardcoded list.
  /// The old fixed array is why half the daemons (cellular, sensors, ddns,
  /// smsctl, httpd) never appeared here at all: each new daemon had to be
  /// remembered in three places (schema, the httpd /status chain, and this
  /// array), and it usually wasn't. Now a daemon shows up the moment it
  /// publishes services.<name>.state.
  services: SvcRow[] = [];

  /// Display names for the daemons we know. Anything else still renders, with
  /// a humanised fallback ("foo_bar" → "Foo Bar") — an unknown-but-present
  /// daemon is better shown than hidden.
  private static readonly NAMES: Record<string, string> = {
    ds:              'Data Store',
    net_router:      'Network Router',
    openvpn_client:  'OpenVPN Client',
    lwm2m_client:    'LwM2M Client',
    lwm2m_server:    'LwM2M Server',
    wifi_client:     'Wi-Fi Client',
    cellular:        'Cellular Modem',
    sensors:         'Sensors (I²C)',
    vehicle:         'Vehicle (OBD-II)',
    mqtt:            'MQTT Mirror',
    container:       'Containers',
    ddns:            'Dynamic DNS',
    smsctl:          'SMS Control',
    httpd:           'Web Server',
  };

  /// Sort order for the known daemons; unknown ones sort last, alphabetically.
  private static readonly ORDER = [
    'ds', 'httpd', 'net_router', 'wifi_client', 'cellular', 'openvpn_client',
    'lwm2m_client', 'lwm2m_server', 'container', 'sensors', 'vehicle', 'mqtt',
    'ddns', 'smsctl',
  ];

  /// Only these daemons implement a ServiceGate — i.e. they actually watch
  /// services.<x>.enable and park when it goes false. For the rest the checkbox
  /// would be a lie (nobody is listening), so it is not offered; Restart still
  /// works for them via systemd.
  private static readonly GATED: Record<string, string> = {
    net_router:     'services.net.router.enable',
    openvpn_client: 'services.openvpn.client.enable',
    lwm2m_client:   'services.lwm2m.client.enable',
    lwm2m_server:   'services.lwm2m.server.enable',
    wifi_client:    'services.wifi.client.enable',
  };

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
    const rows: SvcRow[] = [];
    for (const key of Object.keys(svcs)) {
      const prev = this.services.find(r => r.key === key);
      rows.push({
        key,
        name:  ServicesListComponent.NAMES[key] ?? this.humanize(key),
        label: 'services.' + key.replace(/_/g, '.'),
        info:  { ...svcs[key] },
        // Preserve in-flight UI state across the status refresh.
        restarting: prev?.restarting ?? false,
        msg:        prev?.msg ?? '',
      });
    }
    const order = ServicesListComponent.ORDER;
    rows.sort((a, b) => {
      const ia = order.indexOf(a.key), ib = order.indexOf(b.key);
      if (ia >= 0 && ib >= 0) return ia - ib;
      if (ia >= 0) return -1;
      if (ib >= 0) return 1;
      return a.name.localeCompare(b.name);
    });
    this.services = rows;
  }

  /// "net_router" → "Net Router" — the fallback for a daemon we don't know yet.
  private humanize(key: string): string {
    return key.split('_')
              .map(w => w.charAt(0).toUpperCase() + w.slice(1))
              .join(' ');
  }

  /// True when this daemon actually honours services.<x>.enable.
  isGated(s: SvcRow): boolean {
    return !!ServicesListComponent.GATED[s.key];
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
    return ServicesListComponent.GATED[k] || '';
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
