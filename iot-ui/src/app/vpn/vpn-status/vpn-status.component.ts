import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription, interval } from 'rxjs';
import { switchMap } from 'rxjs/operators';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { VpnStatus } from '../../../common/app-globals';

@Component({
  selector: 'app-vpn-status',
  template: `
    <div class="page">
      <h3>VPN Connection Status</h3>
      <div class="status-grid" *ngIf="!loading">
        <div class="status-item">
          <span class="label">State</span>
          <app-status-badge [label]="v.state || 'unknown'" [state]="v.state || ''"></app-status-badge>
        </div>
        <div class="status-item">
          <span class="label">Assigned IP</span>
          <span class="value">{{ v.ip || '—' }}</span>
        </div>
        <div class="status-item">
          <span class="label">Gateway</span>
          <span class="value">{{ v.gateway || '—' }}</span>
        </div>
        <div class="status-item">
          <span class="label">Netmask</span>
          <span class="value">{{ v.netmask || '—' }}</span>
        </div>
        <div class="status-item">
          <span class="label">DNS</span>
          <span class="value">{{ v.dns || '—' }}</span>
        </div>
        <div class="status-item">
          <span class="label">PID</span>
          <span class="value">{{ v.pid || '—' }}</span>
        </div>
        <div class="status-item">
          <span class="label">Exit Code</span>
          <span class="value">{{ v.exit_code != null ? v.exit_code : '—' }}</span>
        </div>
        <div class="status-item">
          <span class="label">Gate Reason</span>
          <span class="value">{{ v.gate_reason || '—' }}</span>
        </div>
        <div class="status-item">
          <span class="label">Bound Interface</span>
          <span class="value">{{ v.bound_iface || '—' }}</span>
        </div>
      </div>
      <p *ngIf="loading">Loading…</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #e0e0e0; margin: 0 0 20px 0; }
    .status-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px,1fr)); gap: 16px; }
    .status-item { background: rgba(255,255,255,0.04); border-radius: 6px; padding: 14px; }
    .label { display: block; font-size: 11px; color: #9e9e9e; margin-bottom: 6px; text-transform: uppercase; letter-spacing: 0.5px; }
    .value { font-size: 15px; color: #e0e0e0; font-weight: 500; }
  `]
})
export class VpnStatusComponent implements OnInit, OnDestroy {
  v: VpnStatus = {};
  loading = true;
  private sub = new Subscription();

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void {
    this.sub.add(
      interval(5000).pipe(
        switchMap(() => this.http.getStatus())
      ).subscribe({
        next: (s) => { this.v = s.vpn || {}; this.loading = false; },
        error: () => { this.loading = false; }
      })
    );
    // Immediate first load
    this.http.getStatus().subscribe({
      next: (s) => { this.v = s.vpn || {}; this.loading = false; },
      error: () => { this.loading = false; }
    });
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
