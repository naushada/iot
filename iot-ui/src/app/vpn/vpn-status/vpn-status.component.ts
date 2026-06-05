import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { VpnStatus } from '../../../common/app-globals';

@Component({
  selector: 'app-vpn-status',
  template: `
    <div class="page">
      <h3>VPN Connection Status</h3>
      <table class="table table-compact" *ngIf="!loading">
        <tbody>
          <tr><td class="label-col">State</td><td><app-status-badge [label]="v.state||'unknown'" [state]="v.state||''"></app-status-badge></td></tr>
          <tr><td class="label-col">Assigned IP</td><td>{{ v.ip || '—' }}</td></tr>
          <tr><td class="label-col">Gateway</td><td>{{ v.gateway || '—' }}</td></tr>
          <tr><td class="label-col">Netmask</td><td>{{ v.netmask || '—' }}</td></tr>
          <tr><td class="label-col">DNS</td><td>{{ v.dns || '—' }}</td></tr>
          <tr><td class="label-col">PID</td><td>{{ v.pid || '—' }}</td></tr>
          <tr><td class="label-col">Exit Code</td><td>{{ v.exit_code != null ? v.exit_code : '—' }}</td></tr>
          <tr><td class="label-col">Gate Reason</td><td>{{ v.gate_reason || '—' }}</td></tr>
          <tr><td class="label-col">Bound Interface</td><td>{{ v.bound_iface || '—' }}</td></tr>
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
