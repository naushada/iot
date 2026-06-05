import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

@Component({
  selector: 'app-iface-priority',
  template: `
    <div class="page">
      <h3>Interface Priority</h3>
      <p class="desc">Comma-separated priority list — highest-priority OPER-UP interface becomes the active WAN path.</p>

      <div class="form-grid">
        <clr-input-container>
          <label>Priority List</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="priority" placeholder="eth,wifi,cellular" />
        </clr-input-container>
        <clr-input-container>
          <label>Ethernet Interface</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="ethName" placeholder="eth0" />
        </clr-input-container>
        <clr-input-container>
          <label>WiFi Interface</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="wifiName" placeholder="wlan0" />
        </clr-input-container>
        <clr-input-container>
          <label>Cellular Interface</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="cellName" placeholder="wwan0" />
        </clr-input-container>
        <clr-input-container>
          <label>Poll Interval (s)</label>
          <input clrInput [disabled]="!isAdmin" type="number" [(ngModel)]="pollInterval" />
        </clr-input-container>
        <div>
          <label class="clr-control-label">Active Interface</label>
          <span class="active-iface">{{ activeIface || 'none' }}</span>
        </div>
      </div>

      <div style="margin-top:20px;">
        <button class="btn btn-primary" *ngIf="isAdmin" (click)="save()" [disabled]="saving">
          {{ saving ? 'Saving…' : 'Save' }}
        </button>
        <span *ngIf="msg" style="margin-left:12px;"
              [style.color]="msg==='Saved.'?'#2e7d32':'#c62828'">{{ msg }}</span>
      </div>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 8px 0; }
    .desc { color: #888; font-size: 13px; margin-bottom: 20px; }
    .active-iface { font-size: 16px; font-weight: 600; color: #2e7d32; display: block; margin-top: 2px; }
  `]
})
export class IfacePriorityComponent implements OnInit {
  priority = 'eth,wifi,cellular'; ethName = 'eth0'; wifiName = 'wlan0';
  cellName = 'wwan0'; pollInterval = 5; activeIface = '';
  saving = false; msg = '';

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService, private toast: ToastService) {}

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
      next: (r) => { this.saving = false; if(r.ok) this.toast.success('Interface priority saved'); else this.toast.error(r.err||'Save failed'); },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
