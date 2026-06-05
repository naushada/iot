import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';

@Component({
  selector: 'app-port-forward',
  template: `
    <div class="page">
      <h3>Port Forwarding &amp; DNAT</h3>

      <div class="form-grid" style="align-items:end;">
        <clr-input-container>
          <label>DNAT Target IP <span class="hint">(LwM2M client)</span></label>
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

      <div class="form-grid" style="margin-top:24px; align-items:end;">
        <clr-input-container style="grid-column: span 2;">
          <label>Forwarded Ports</label>
          <input clrInput [disabled]="!isAdmin" [(ngModel)]="forwardPorts" placeholder="80,443,5684" />
          <clr-control-helper>Comma-separated, DNAT'd to the target IP above</clr-control-helper>
        </clr-input-container>
        <div class="btn-cell" *ngIf="isAdmin">
          <button class="btn btn-primary" (click)="savePorts()" [disabled]="savingPorts">
            {{ savingPorts ? 'Saving…' : 'Save Ports' }}
          </button>
        </div>
      </div>

      <table class="table table-compact table-borderless" style="margin-top:24px;">
        <tbody>
          <tr><td class="label-col">State</td><td><app-status-badge [label]="routeState||'unknown'" [state]="routeState||''"></app-status-badge></td></tr>
          <tr><td class="label-col">Rules Applied</td><td>{{ rulesApplied }}</td></tr>
          <tr><td class="label-col">Last Apply</td><td>{{ lastApply || '—' }}</td></tr>
        </tbody>
      </table>

      <span *ngIf="msg" style="display:block;margin-top:12px;"
            [style.color]="msg.startsWith('Saved')||msg.startsWith('DNAT')?'#2e7d32':'#c62828'">{{ msg }}</span>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3 { color: #333; margin: 0 0 20px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn-primary { white-space: nowrap; }
  `]
})
export class PortForwardComponent implements OnInit {
  targetIp = ''; targetPort = 5684; forwardPorts = '80,443,5684';
  savingDnat = false; savingPorts = false; msg = '';
  routeState = ''; rulesApplied = 0; lastApply = '';

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService) {}

  ngOnInit(): void {
    this.http.dbGet(['net.lwm2m.target.ip', 'net.lwm2m.target.port', 'net.forward.ports',
      'net.state', 'net.rules.applied.count', 'net.last.apply.unix']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.targetIp   = (d['net.lwm2m.target.ip'] as string) || '';
          this.targetPort = (d['net.lwm2m.target.port'] as number) || 5684;
          this.forwardPorts    = (d['net.forward.ports'] as string) || '80,443,5684';
          this.routeState      = (d['net.state'] as string) || '';
          this.rulesApplied    = (d['net.rules.applied.count'] as number) || 0;
          const lu = d['net.last.apply.unix'] as number;
          this.lastApply = lu ? new Date((lu as number)*1000).toLocaleString() : '';
        }
      }
    });
  }

  saveDnat(): void {
    this.savingDnat = true; this.msg = '';
    this.http.dbSet([
      { key: 'net.lwm2m.target.ip',   value: this.targetIp },
      { key: 'net.lwm2m.target.port', value: this.targetPort },
    ]).subscribe({
      next: (r) => { this.savingDnat = false; this.msg = r.ok ? 'DNAT saved.' : 'Error: '+r.err; },
      error: () => { this.savingDnat = false; this.msg = 'DNAT save failed.'; }
    });
  }

  savePorts(): void {
    this.savingPorts = true; this.msg = '';
    this.http.dbSet([
      { key: 'net.forward.ports', value: this.forwardPorts },
    ]).subscribe({
      next: (r) => { this.savingPorts = false; this.msg = r.ok ? 'Saved ports.' : 'Error: '+r.err; },
      error: () => { this.savingPorts = false; this.msg = 'Port save failed.'; }
    });
  }
}
