import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { PubSubService } from '../../../common/pubsubsvc.service';
import { ToastService } from '../../../common/toast.service';
import { DataStoreService } from '../../../common/datastore.service';

@Component({
  selector: 'app-port-forward',
  template: `
    <div class="page">
      <!-- DNAT Target view -->
      <ng-container *ngIf="view === 'dnat'">
        <h3>DNAT Target</h3>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container>
            <label>Target IP <span class="hint">(LwM2M client)</span></label>
            <input clrInput [disabled]="!isAdmin" [(ngModel)]="targetIp" placeholder="192.168.1.100" />
          </clr-input-container>
          <clr-input-container>
            <label>Target Port</label>
            <input clrInput type="number" [disabled]="!isAdmin" [(ngModel)]="targetPort" />
          </clr-input-container>
          <div class="btn-cell" *ngIf="isAdmin">
            <button class="btn btn-primary" (click)="saveDnat()" [disabled]="savingDnat">
              {{ savingDnat ? 'Saving…' : 'Save DNAT' }}
            </button>
          </div>
        </div>
      </ng-container>

      <!-- Port Forward view -->
      <ng-container *ngIf="view === 'ports'">
        <h3>Forwarded Ports</h3>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container style="grid-column: span 2;">
            <label>Port List</label>
            <input clrInput [disabled]="!isAdmin" [(ngModel)]="forwardPorts" placeholder="80,443,5684" />
            <clr-control-helper>Comma-separated, DNAT'd to {{ targetIp || 'target IP' }}:{{ targetPort }}</clr-control-helper>
          </clr-input-container>
          <div class="btn-cell" *ngIf="isAdmin">
            <button class="btn btn-primary" (click)="savePorts()" [disabled]="savingPorts">
              {{ savingPorts ? 'Saving…' : 'Save Ports' }}
            </button>
          </div>
        </div>
      </ng-container>

      <table class="table table-compact table-borderless" style="margin-top:24px;">
        <tbody>
          <tr><td class="label-col">State</td><td><app-status-badge [label]="routeState||'unknown'" [state]="routeState||''"></app-status-badge></td></tr>
          <tr><td class="label-col">Rules Applied</td><td>{{ rulesApplied }}</td></tr>
          <tr><td class="label-col">Last Apply</td><td>{{ lastApply || '—' }}</td></tr>
        </tbody>
      </table>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3 { color: #333; margin: 0 0 20px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn-primary { white-space: nowrap; }
  `]
})
export class PortForwardComponent implements OnInit, OnDestroy {
  view = 'ports';  // 'ports' | 'dnat'
  targetIp = ''; targetPort = 5684; forwardPorts = '80,443,5684';
  savingDnat = false; savingPorts = false; msg = '';
  routeState = ''; rulesApplied = 0; lastApply = '';
  private sub = new Subscription();

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService, private pubsub: PubSubService,
              private ds: DataStoreService) {}

  ngOnInit(): void {
    // Instant paint from the prefetched cache, then refresh from the wire.
    this.applyData(this.ds.snapshot());
    this.http.dbGet(['net.lwm2m.target.ip', 'net.lwm2m.target.port', 'net.forward.ports',
      'net.state', 'net.rules.applied.count', 'net.last.apply.unix']).subscribe({
      next: (r) => { if (r.ok && r.data) this.applyData(r.data as Record<string, unknown>); }
    });
  }

  private applyData(d: Record<string, unknown>): void {
    if (d['net.lwm2m.target.ip'] != null)      this.targetIp     = String(d['net.lwm2m.target.ip']);
    if (d['net.lwm2m.target.port'] != null)    this.targetPort   = Number(d['net.lwm2m.target.port']) || 5684;
    if (d['net.forward.ports'] != null)        this.forwardPorts = String(d['net.forward.ports']);
    if (d['net.state'] != null)                this.routeState   = String(d['net.state']);
    if (d['net.rules.applied.count'] != null)  this.rulesApplied = Number(d['net.rules.applied.count']) || 0;
    if (d['net.last.apply.unix'] != null) {
      const lu = Number(d['net.last.apply.unix']);
      this.lastApply = lu ? new Date(lu * 1000).toLocaleString() : '';
    }
  }

  saveDnat(): void {
    this.savingDnat = true; this.msg = '';
    this.http.dbSet([
      { key: 'net.lwm2m.target.ip',   value: this.targetIp },
      { key: 'net.lwm2m.target.port', value: this.targetPort },
    ]).subscribe({
      next: (r) => { this.savingDnat = false; if(r.ok) this.toast.success('DNAT saved'); else this.toast.error(r.err||'DNAT save failed'); },
      error: () => { this.savingDnat = false; this.toast.error('DNAT save failed'); }
    });
  }

  savePorts(): void {
    this.savingPorts = true; this.msg = '';
    this.http.dbSet([
      { key: 'net.forward.ports', value: this.forwardPorts },
    ]).subscribe({
      next: (r) => { this.savingPorts = false; if(r.ok) this.toast.success('Ports saved'); else this.toast.error(r.err||'Port save failed'); },
      error: () => { this.savingPorts = false; this.toast.error('Port save failed'); }
    });
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
