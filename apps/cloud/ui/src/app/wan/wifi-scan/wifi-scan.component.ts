import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { WifiStatus } from '../../../common/app-globals';

interface ScanEntry { ssid: string; bssid: string; signal: number; flags: string; }

@Component({
  selector: 'app-wifi-scan',
  template: `
    <div class="page">
      <div style="display:flex;align-items:center;gap:12px;margin-bottom:20px;">
        <h3 style="margin:0;">WiFi Scan Results</h3>
        <button class="btn btn-primary btn-sm" (click)="scan()" [disabled]="scanning">
          {{ scanning ? 'Scanning…' : 'Scan Now' }}
        </button>
        <span *ngIf="scanMsg" [style.color]="scanMsg.startsWith('Scan')?'#66bb6a':'#c62828'">{{ scanMsg }}</span>
        <span style="margin-left:auto;font-size:12px;color:#757575;">
          {{ status?.ssid ? 'Connected to ' + status.ssid : 'Disconnected' }}
          <span *ngIf="status?.rssi != null"> &middot; {{ status.rssi }} dBm</span>
        </span>
      </div>

      <clr-datagrid *ngIf="results.length > 0">
        <clr-dg-column>SSID</clr-dg-column>
        <clr-dg-column>BSSID</clr-dg-column>
        <clr-dg-column>Signal</clr-dg-column>
        <clr-dg-column>Flags</clr-dg-column>

        <clr-dg-row *clrDgItems="let r of results">
          <clr-dg-cell><strong>{{ r.ssid || '(hidden)' }}</strong></clr-dg-cell>
          <clr-dg-cell><code>{{ r.bssid }}</code></clr-dg-cell>
          <clr-dg-cell>
            <span class="signal-bars">
              <span class="bar" *ngFor="let b of [1,2,3,4]" [class.active]="r.signal > (-80+b*5)" [style.height.px]="4+b*3"></span>
            </span>
            {{ r.signal }} dBm
          </clr-dg-cell>
          <clr-dg-cell><span class="flags">{{ r.flags }}</span></clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ results.length }} networks found</clr-dg-footer>
      </clr-datagrid>
      <p *ngIf="results.length===0 && !loading" style="color:#757575;">No scan results yet — click "Scan Now" to trigger a scan.</p>
      <p *ngIf="loading">Loading…</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3 { font-size: 16px; font-weight: 600; color: #333; }
    .signal-bars { display: inline-flex; align-items: flex-end; gap: 1px; height: 14px; margin-right: 6px; vertical-align: middle; }
    .bar { width: 3px; background: #555; border-radius: 1px; }
    .bar.active { background: #2e7d32; }
    .flags { font-size: 11px; color: #9e9e9e; }
  `]
})
export class WifiScanComponent implements OnInit, OnDestroy {
  results: ScanEntry[] = [];
  status: WifiStatus = {};
  loading = true; scanning = false; scanMsg = '';
  private sub = new Subscription();

  constructor(private http: HttpsvcService) {}

  private active = true;
  private pollSub?: Subscription;   // in-flight long-poll, cancelled on destroy

  ngOnInit(): void {
    this.startLongPoll();
  }

  private startLongPoll(): void {
    const poll = (): void => {
      if (!this.active) return;
      this.pollSub = this.http.getStatusLongPoll(30).subscribe({
        next: (s) => {
          this.status = s.wifi || {};
          this.tryParseScan();
          this.loading = false;
          if (this.active) poll();
        },
        error: () => {
          this.loading = false;
          if (this.active) setTimeout(() => poll(), 5000);
        }
      });
    };
    poll();
  }

  loadStatus(): void {
    this.http.getStatus().subscribe({
      next: (s) => { this.status = s.wifi || {}; this.tryParseScan(); this.loading = false; },
      error: () => { this.loading = false; }
    });
    // Also fetch scan results directly
    this.http.dbGet(['wifi.scan.results']).subscribe({
      next: (r) => { if (r.ok && r.data) this.tryParse(r.data['wifi.scan.results'] as string); }
    });
  }

  tryParseScan(): void {
    this.http.dbGet(['wifi.scan.results']).subscribe({
      next: (r) => { if (r.ok && r.data) this.tryParse(r.data['wifi.scan.results'] as string); }
    });
  }

  tryParse(raw: string): void {
    if (!raw) return;
    try { this.results = JSON.parse(raw); } catch { this.results = []; }
  }

  scan(): void {
    this.scanning = true; this.scanMsg = '';
    this.http.triggerScan().subscribe({
      next: (r) => {
        this.scanning = false;
        this.scanMsg = r.ok ? 'Scan triggered (request #'+r.scan_request+')' : 'Error: '+r.err;
        if (r.ok) setTimeout(() => this.tryParseScan(), 3000);
      },
      error: () => { this.scanning = false; this.scanMsg = 'Scan request failed'; }
    });
  }

  ngOnDestroy(): void { this.active = false; this.pollSub?.unsubscribe(); this.sub.unsubscribe(); }
}
