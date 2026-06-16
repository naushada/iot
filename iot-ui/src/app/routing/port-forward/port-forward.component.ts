import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
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
            <clr-control-helper *dsDebug><app-ds-hint key="net.lwm2m.target.ip"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Target Port</label>
            <input clrInput type="number" [disabled]="!isAdmin" [(ngModel)]="targetPort" />
            <clr-control-helper *dsDebug><app-ds-hint key="net.lwm2m.target.port"></app-ds-hint></clr-control-helper>
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
            <clr-control-helper *dsDebug><app-ds-hint key="net.forward.ports"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <div class="btn-cell" *ngIf="isAdmin">
            <button class="btn btn-primary" (click)="savePorts()" [disabled]="savingPorts">
              {{ savingPorts ? 'Saving…' : 'Save Ports' }}
            </button>
          </div>
        </div>
      </ng-container>

      <clr-datagrid style="margin-top:24px;">
        <clr-dg-column>Property</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>
        <clr-dg-row>
          <clr-dg-cell>State</clr-dg-cell>
          <clr-dg-cell><app-status-badge [label]="routeState||'unknown'" [state]="routeState||''"></app-status-badge></clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Rules Applied</clr-dg-cell>
          <clr-dg-cell>{{ rulesApplied }}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Last Apply</clr-dg-cell>
          <clr-dg-cell>{{ lastApply || '—' }}</clr-dg-cell>
        </clr-dg-row>
      </clr-datagrid>
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
  savingDnat = false; savingPorts = false;
  routeState = ''; rulesApplied = 0; lastApply = '';
  private sub = new Subscription();

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private session: SessionService,
              private toast: ToastService, private pubsub: PubSubService,
              private ds: DataStoreService) {}

  ngOnInit(): void {
    // Paint instantly from the shared prefetched cache, then stay live off the
    // appglobal store. The DNAT/ports keys are edited only here and aren't
    // republished by /status (they fire once when the prefetch lands); the
    // state/rules/last-apply rows are live telemetry off the shared long-poll.
    this.applyData(this.ds.snapshot());
    for (const k of ['net.lwm2m.target.ip', 'net.lwm2m.target.port', 'net.forward.ports'])
      this.sub.add(this.ds.observe(k).subscribe(() => this.applyEditable(this.ds.snapshot())));
    for (const k of ['net.state', 'net.rules.applied.count', 'net.last.apply.unix'])
      this.sub.add(this.ds.observe(k).subscribe(() => this.applyLive(this.ds.snapshot())));
  }

  private applyData(d: Record<string, unknown>): void {
    this.applyEditable(d);
    this.applyLive(d);
  }

  private applyEditable(d: Record<string, unknown>): void {
    if (d['net.lwm2m.target.ip'] != null)      this.targetIp     = String(d['net.lwm2m.target.ip']);
    if (d['net.lwm2m.target.port'] != null)    this.targetPort   = Number(d['net.lwm2m.target.port']) || 5684;
    if (d['net.forward.ports'] != null)        this.forwardPorts = String(d['net.forward.ports']);
  }

  private applyLive(d: Record<string, unknown>): void {
    if (d['net.state'] != null)                this.routeState   = String(d['net.state']);
    if (d['net.rules.applied.count'] != null)  this.rulesApplied = Number(d['net.rules.applied.count']) || 0;
    if (d['net.last.apply.unix'] != null) {
      const lu = Number(d['net.last.apply.unix']);
      this.lastApply = lu ? new Date(lu * 1000).toLocaleString() : '';
    }
  }

  saveDnat(): void {
    this.savingDnat = true;
    this.ds.write([
      { key: 'net.lwm2m.target.ip',   value: this.targetIp },
      { key: 'net.lwm2m.target.port', value: this.targetPort },
    ]).subscribe({
      next: (r) => { this.savingDnat = false; if(r.ok) this.toast.success('DNAT saved'); else this.toast.error(r.err||'DNAT save failed'); },
      error: () => { this.savingDnat = false; this.toast.error('DNAT save failed'); }
    });
  }

  savePorts(): void {
    this.savingPorts = true;
    this.ds.write([
      { key: 'net.forward.ports', value: this.forwardPorts },
    ]).subscribe({
      next: (r) => { this.savingPorts = false; if(r.ok) this.toast.success('Ports saved'); else this.toast.error(r.err||'Port save failed'); },
      error: () => { this.savingPorts = false; this.toast.error('Port save failed'); }
    });
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
