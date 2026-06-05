import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { VpnStatus } from '../../../common/app-globals';

@Component({
  selector: 'app-vpn-status',
  template: `
    <div class="page">
      <h3>VPN Connection Status</h3>
      <table class="table" *ngIf="!loading">
        <colgroup>
          <col style="width:25%;">
          <col style="width:75%;">
        </colgroup>
        <thead>
          <tr><th>Property</th><th>Value</th></tr>
        </thead>
        <tbody>
          <tr *ngFor="let row of rows">
            <td>{{ row.key }}</td>
            <td>
              <app-status-badge *ngIf="row.isBadge" [label]="row.value" [state]="v.state||''"></app-status-badge>
              <span *ngIf="!row.isBadge">{{ row.value }}</span>
            </td>
          </tr>
        </tbody>
      </table>
      <p *ngIf="loading">Loading…</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
  `]
})
export class VpnStatusComponent implements OnInit, OnDestroy {
  v: VpnStatus = {};
  loading = true;
  private sub = new Subscription();
  private active = true;

  get rows(): { key: string; value: string; isBadge: boolean }[] {
    return [
      { key: 'State',           value: this.v.state || 'unknown', isBadge: true },
      { key: 'Assigned IP',     value: this.v.ip || '—',          isBadge: false },
      { key: 'Gateway',         value: this.v.gateway || '—',     isBadge: false },
      { key: 'Netmask',         value: this.v.netmask || '—',     isBadge: false },
      { key: 'DNS',             value: this.v.dns || '—',         isBadge: false },
      { key: 'PID',             value: String(this.v.pid || '—'), isBadge: false },
      { key: 'Exit Code',       value: this.v.exit_code != null ? String(this.v.exit_code) : '—', isBadge: false },
      { key: 'Gate Reason',     value: this.v.gate_reason || '—', isBadge: false },
      { key: 'Bound Interface', value: this.v.bound_iface || '—', isBadge: false },
    ];
  }

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void {
    this.startLongPoll();
  }

  /// Long-poll loop: blocks until vpn.state changes or 30s timeout,
  /// then re-subscribes immediately.  Falls back to 5s poll on error.
  private startLongPoll(): void {
    const poll = (): void => {
      if (!this.active) return;
      this.http.getStatusLongPoll(30).subscribe({
        next: (s) => {
          this.v = s.vpn || {};
          this.loading = false;
          if (this.active) poll();
        },
        error: () => {
          this.loading = false;
          if (this.active) {
            // Fallback: immediate status + retry long-poll after 5s
            this.http.getStatus().subscribe({
              next: (s) => { this.v = s.vpn || {}; }
            });
            setTimeout(() => poll(), 5000);
          }
        }
      });
    };
    poll();
  }

  ngOnDestroy(): void { this.active = false; this.sub.unsubscribe(); }
}
