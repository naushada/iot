import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription, interval } from 'rxjs';
import { switchMap } from 'rxjs/operators';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { StatusSnapshot, ServiceInfo } from '../../../common/app-globals';

interface SvcRow { key: string; label: string; info: ServiceInfo; restarting: boolean; msg: string; }

@Component({
  selector: 'app-services-list',
  template: `
    <div class="page">
      <h3>Service Management</h3>
      <table class="table table-compact">
        <thead><tr><th>Service</th><th>State</th><th>Enabled</th><th></th><th>Actions</th></tr></thead>
        <tbody>
          <tr *ngFor="let s of services">
            <td><code>{{ s.label }}</code></td>
            <td><app-status-badge [label]="s.info.state||'unknown'" [state]="s.info.state||''"></app-status-badge></td>
            <td>
              <input type="checkbox" [checked]="s.info.enable" (change)="toggleEnable(s)" />
            </td>
            <td>
              <span *ngIf="s.info.uptime_sec != null" style="font-size:12px;color:#757575;">uptime {{ s.info.uptime_sec }}s</span>
            </td>
            <td>
              <button class="btn btn-sm" [disabled]="s.restarting || s.key==='ds'"
                      (click)="restart(s)">
                {{ s.restarting ? '…' : 'Restart' }}
              </button>
              <span *ngIf="s.msg" style="margin-left:8px;font-size:11px;"
                    [style.color]="s.msg.startsWith('Restarted')?'#66bb6a':'#c62828'">{{ s.msg }}</span>
            </td>
          </tr>
        </tbody>
      </table>
      <p class="hint">Restart: disable → 2 s wait → enable. ds-server cannot self-restart (the API socket would close).</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    
    .btn-sm:disabled { opacity: 0.5; cursor: not-allowed; }
    .hint { font-size: 12px; color: #757575; margin-top: 16px; }
  `]
})
export class ServicesListComponent implements OnInit, OnDestroy {
  services: SvcRow[] = [
    { key: 'ds', label: 'services.ds', info: {}, restarting: false, msg: '' },
    { key: 'net_router',      label: 'services.net.router',     info: {}, restarting: false, msg: '' },
    { key: 'openvpn_client',  label: 'services.openvpn.client', info: {}, restarting: false, msg: '' },
    { key: 'lwm2m_client',    label: 'services.lwm2m.client',   info: {}, restarting: false, msg: '' },
    { key: 'lwm2m_server',    label: 'services.lwm2m.server',   info: {}, restarting: false, msg: '' },
    { key: 'wifi_client',     label: 'services.wifi.client',    info: {}, restarting: false, msg: '' },
  ];
  private sub = new Subscription();

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void {
    this.load();
    this.sub.add(interval(8000).pipe(switchMap(() => this.http.getStatus())).subscribe({
      next: (s) => this.applyStatus(s)
    }));
  }

  load(): void {
    this.http.getStatus().subscribe({ next: (s) => this.applyStatus(s) });
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
        if (r.ok) setTimeout(() => { svc.msg = ''; this.load(); }, 3000);
      },
      error: () => { svc.restarting = false; svc.msg = 'Request failed'; }
    });
  }

  private enableKey(k: string): string {
    const m: Record<string, string> = {
      net_router: 'services.net.router.enable',
      openvpn_client: 'services.openvpn.client.enable',
      lwm2m_client: 'services.lwm2m.client.enable',
      lwm2m_server: 'services.lwm2m.server.enable',
      wifi_client: 'services.wifi.client.enable',
    };
    return m[k] || '';
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
