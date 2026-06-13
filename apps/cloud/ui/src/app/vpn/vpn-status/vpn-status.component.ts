import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../../common/datastore.service';
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
          <clr-dg-cell>
            {{ row.key }}
            <app-ds-hint *dsDebug [key]="row.dsKey"></app-ds-hint>
          </clr-dg-cell>
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

  get rows(): { key: string; value: string; isBadge: boolean; isDns?: boolean; dsKey: string }[] {
    return [
      { key: 'State',           value: this.v.state || 'unknown', isBadge: true,  dsKey: 'vpn.state' },
      { key: 'Assigned IP',     value: this.v.ip || '—',          isBadge: false, dsKey: 'vpn.assigned.ip' },
      { key: 'Gateway',         value: this.v.gateway || '—',     isBadge: false, dsKey: 'vpn.assigned.gateway' },
      { key: 'Netmask',         value: this.v.netmask || '—',     isBadge: false, dsKey: 'vpn.assigned.netmask' },
      { key: 'DNS',             value: this.v.dns || '—',         isBadge: false, isDns: true, dsKey: 'vpn.assigned.dns' },
      { key: 'PID',             value: String(this.v.pid || '—'), isBadge: false, dsKey: 'vpn.pid' },
      { key: 'Exit Code',       value: this.v.exit_code != null ? String(this.v.exit_code) : '—', isBadge: false, dsKey: 'vpn.exit_code' },
      { key: 'Gate Reason',     value: this.v.gate_reason || '—', isBadge: false, dsKey: 'vpn.gate.reason' },
      { key: 'Bound Interface', value: this.v.bound_iface || '—', isBadge: false, dsKey: 'vpn.bound.iface' },
    ];
  }

  /// VPN servers can push several DNS resolvers; the daemon stores them
  /// comma/space-joined in vpn.assigned.dns. Split for per-entry display.
  get dnsList(): string[] {
    return (this.v.dns || '').split(/[\s,]+/).filter(Boolean);
  }

  constructor(private ds: DataStoreService) {}

  /// Read vpn.* live off the single shared /status stream — no per-page
  /// long-poll. The BehaviorSubject replays the latest snapshot, so this
  /// paints instantly on revisit.
  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((s) => {
      this.v = s.vpn || {};
      this.loading = false;
    }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
