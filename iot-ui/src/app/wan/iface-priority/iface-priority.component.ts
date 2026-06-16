import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { DataStoreService } from '../../../common/datastore.service';

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
          <clr-control-helper *dsDebug><app-ds-hint key="net.iface.priority"></app-ds-hint></clr-control-helper>
        </clr-input-container>
        <clr-input-container>
          <label>Ethernet Interface</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="ethName" placeholder="eth0" />
          <clr-control-helper *dsDebug><app-ds-hint key="net.iface.eth.name"></app-ds-hint></clr-control-helper>
        </clr-input-container>
        <clr-input-container>
          <label>WiFi Interface</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="wifiName" placeholder="wlan0" />
          <clr-control-helper *dsDebug><app-ds-hint key="net.iface.wifi.name"></app-ds-hint></clr-control-helper>
        </clr-input-container>
        <clr-input-container>
          <label>Cellular Interface</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="cellName" placeholder="wwan0" />
          <clr-control-helper *dsDebug><app-ds-hint key="net.iface.cellular.name"></app-ds-hint></clr-control-helper>
        </clr-input-container>
        <clr-input-container>
          <label>Poll Interval (s)</label>
          <input clrInput [disabled]="!isAdmin" type="number" [(ngModel)]="pollInterval" />
          <clr-control-helper *dsDebug><app-ds-hint key="net.poll.interval.sec"></app-ds-hint></clr-control-helper>
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
export class IfacePriorityComponent implements OnInit, OnDestroy {
  priority = 'eth,wifi,cellular'; ethName = 'eth0'; wifiName = 'wlan0';
  cellName = 'wwan0'; pollInterval = 5; activeIface = '';
  saving = false; msg = '';
  private sub = new Subscription();
  private readonly CFG_KEYS = [
    'net.iface.priority', 'net.iface.eth.name', 'net.iface.wifi.name',
    'net.iface.cellular.name', 'net.poll.interval.sec',
  ];

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private session: SessionService,
              private toast: ToastService, private ds: DataStoreService) {}

  ngOnInit(): void {
    // Paint instantly from the shared prefetched cache, then stay live off the
    // appglobal store. The editable config keys change only here and aren't
    // republished by /status (they fire once when the prefetch lands);
    // net.iface.active is live telemetry off the shared long-poll.
    this.applyData(this.ds.snapshot());
    for (const k of this.CFG_KEYS)
      this.sub.add(this.ds.observe(k).subscribe(() => this.applyData(this.ds.snapshot())));
    this.sub.add(this.ds.observe('net.iface.active')
      .subscribe(v => { if (v != null) this.activeIface = String(v); }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyData(d: Record<string, unknown>): void {
    if (d['net.iface.priority'] != null)      this.priority     = String(d['net.iface.priority']);
    if (d['net.iface.eth.name'] != null)      this.ethName      = String(d['net.iface.eth.name']);
    if (d['net.iface.wifi.name'] != null)     this.wifiName     = String(d['net.iface.wifi.name']);
    if (d['net.iface.cellular.name'] != null) this.cellName     = String(d['net.iface.cellular.name']);
    if (d['net.poll.interval.sec'] != null)   this.pollInterval = Number(d['net.poll.interval.sec']) || 5;
    if (d['net.iface.active'] != null)        this.activeIface  = String(d['net.iface.active']);
  }

  save(): void {
    this.saving = true; this.msg = '';
    this.ds.write([
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
