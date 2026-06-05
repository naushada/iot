import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';

@Component({
  selector: 'app-iface-priority',
  template: `
    <div class="page">
      <h3>Interface Priority</h3>
      <p class="desc">Comma-separated priority list — highest-priority OPER-UP interface becomes the active WAN path.</p>

      <div class="clr-row" style="margin-bottom:20px;">
        <div class="clr-col-md-6"><label class="fl">Priority List</label>
          <input class="clr-input" [(ngModel)]="priority" placeholder="eth,wifi,cellular" />
        </div>
        <div class="clr-col-md-3"><label class="fl">Ethernet Iface</label>
          <input class="clr-input" [(ngModel)]="ethName" placeholder="eth0" />
        </div>
        <div class="clr-col-md-3"><label class="fl">WiFi Iface</label>
          <input class="clr-input" [(ngModel)]="wifiName" placeholder="wlan0" />
        </div>
      </div>

      <div class="clr-row" style="margin-bottom:20px;">
        <div class="clr-col-md-3"><label class="fl">Cellular Iface</label>
          <input class="clr-input" [(ngModel)]="cellName" placeholder="wwan0" />
        </div>
        <div class="clr-col-md-3"><label class="fl">Poll Interval (s)</label>
          <input type="number" class="clr-input" [(ngModel)]="pollInterval" />
        </div>
        <div class="clr-col-md-6">
          <label class="fl">Active Interface</label>
          <span class="active-iface">{{ activeIface || 'none' }}</span>
        </div>
      </div>

      <button class="btn btn-primary" (click)="save()" [disabled]="saving">
        {{ saving ? 'Saving…' : 'Save' }}
      </button>
      <span *ngIf="msg" style="margin-left:12px;"
            [style.color]="msg==='Saved.'?'#2e7d32':'#c62828'">{{ msg }}</span>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 8px 0; }
    .desc { color: #9e9e9e; font-size: 13px; margin-bottom: 20px; }
    .fl { display: block; font-size: 12px; color: #9e9e9e; margin-bottom: 4px; }
    .clr-input { width: 100%; background: #fff; border: 1px solid #ccc; color: #333; padding: 6px 10px; border-radius: 4px; font-size: 13px; }
    .clr-input:focus { outline: none; border-color: #2e7d32; }
    .btn-primary { background: #2e7d32; border: none; color: #fff; padding: 8px 20px; border-radius: 4px; cursor: pointer; }
    .btn-primary:disabled { opacity: 0.5; }
    .active-iface { font-size: 16px; font-weight: 600; color: #66bb6a; display: block; margin-top: 18px; }
  `]
})
export class IfacePriorityComponent implements OnInit {
  priority = 'eth,wifi,cellular'; ethName = 'eth0'; wifiName = 'wlan0';
  cellName = 'wwan0'; pollInterval = 5; activeIface = '';
  saving = false; msg = '';

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void {
    this.http.dbGet(['net.iface.priority', 'net.iface.eth.name', 'net.iface.wifi.name',
      'net.iface.cellular.name', 'net.poll.interval.sec', 'net.iface.active']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.priority = (d['net.iface.priority'] as string) || 'eth,wifi,cellular';
          this.ethName = (d['net.iface.eth.name'] as string) || 'eth0';
          this.wifiName = (d['net.iface.wifi.name'] as string) || 'wlan0';
          this.cellName = (d['net.iface.cellular.name'] as string) || 'wwan0';
          this.pollInterval = (d['net.poll.interval.sec'] as number) || 5;
          this.activeIface = (d['net.iface.active'] as string) || '';
        }
      }
    });
  }

  save(): void {
    this.saving = true; this.msg = '';
    this.http.dbSet([
      { key: 'net.iface.priority', value: this.priority },
      { key: 'net.iface.eth.name', value: this.ethName },
      { key: 'net.iface.wifi.name', value: this.wifiName },
      { key: 'net.iface.cellular.name', value: this.cellName },
      { key: 'net.poll.interval.sec', value: this.pollInterval },
    ]).subscribe({
      next: (r) => { this.saving = false; this.msg = r.ok ? 'Saved.' : 'Error: ' + r.err; },
      error: () => { this.saving = false; this.msg = 'Save failed.'; }
    });
  }
}
