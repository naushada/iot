import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { VpnStatus } from '../../../common/app-globals';

@Component({
  selector: 'app-vpn-status',
  template: `
    <div class="page">
      <h3>VPN Connection Status</h3>
      <clr-datagrid *ngIf="!loading">
        <clr-dg-column>Property</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>

        <clr-dg-row *clrDgItems="let row of rows">
          <clr-dg-cell>{{ row.key }}</clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge *ngIf="row.isBadge" [label]="row.value" [state]="v.state||''"></app-status-badge>
            <ng-container *ngIf="row.isDns">
              <span *ngFor="let d of dnsList" class="dns-item">{{ d }}</span>
              <span *ngIf="!dnsList.length">—</span>
            </ng-container>
            <span *ngIf="!row.isBadge && !row.isDns">{{ row.value }}</span>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ rows.length }} properties</clr-dg-footer>
      </clr-datagrid>
      <p *ngIf="loading">Loading…</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .dns-item { display: inline-block; margin-right: 6px; padding: 1px 8px;
                background: #eef2f7; border-radius: 10px; font-size: 12px; }
  `]
})
export class VpnStatusComponent implements OnInit, OnDestroy {
  v: VpnStatus = {};
  loading = true;
  private sub = new Subscription();
  private active = true;
  private pollSub?: Subscription;   // in-flight long-poll, cancelled on destroy

  get rows(): { key: string; value: string; isBadge: boolean; isDns?: boolean }[] {
    return [
      { key: 'State',           value: this.v.state || 'unknown', isBadge: true },
      { key: 'Assigned IP',     value: this.v.ip || '—',          isBadge: false },
      { key: 'Gateway',         value: this.v.gateway || '—',     isBadge: false },
      { key: 'Netmask',         value: this.v.netmask || '—',     isBadge: false },
      { key: 'DNS',             value: this.v.dns || '—',         isBadge: false, isDns: true },
      { key: 'PID',             value: String(this.v.pid || '—'), isBadge: false },
      { key: 'Exit Code',       value: this.v.exit_code != null ? String(this.v.exit_code) : '—', isBadge: false },
      { key: 'Gate Reason',     value: this.v.gate_reason || '—', isBadge: false },
      { key: 'Bound Interface', value: this.v.bound_iface || '—', isBadge: false },
    ];
  }

  /// VPN servers can push several DNS resolvers; the daemon stores them
  /// comma/space-joined in vpn.assigned.dns. Split for per-entry display.
  get dnsList(): string[] {
    return (this.v.dns || '').split(/[\s,]+/).filter(Boolean);
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
      this.pollSub = this.http.getStatusLongPoll(30).subscribe({
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

  ngOnDestroy(): void { this.active = false; this.pollSub?.unsubscribe(); this.sub.unsubscribe(); }
}
