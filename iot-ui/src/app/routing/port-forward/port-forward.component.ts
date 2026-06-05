import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';

@Component({
  selector: 'app-port-forward',
  template: `
    <div class="page">
      <h3>Port Forwarding &amp; DNAT</h3>

      <div class="clr-row" style="margin-bottom:24px;">
        <div class="clr-col-md-6">
          <label class="fl">DNAT Target IP <span class="hint">(LwM2M client)</span></label>
          <input class="clr-input" [(ngModel)]="targetIp" placeholder="192.168.1.100" />
        </div>
        <div class="clr-col-md-3">
          <label class="fl">Target Port</label>
          <input type="number" class="clr-input" [(ngModel)]="targetPort" />
        </div>
        <div class="clr-col-md-3">
          <label class="fl">&nbsp;</label>
          <button class="btn btn-primary" style="width:100%;" (click)="saveDnat()" [disabled]="savingDnat">
            {{ savingDnat ? 'Saving…' : 'Save DNAT' }}
          </button>
        </div>
      </div>

      <h4>Forwarded Ports</h4>
      <div class="clr-row" style="margin-bottom:12px;">
        <div class="clr-col-md-8">
          <input class="clr-input" [(ngModel)]="forwardPorts"
                 placeholder="80,443,5684" />
          <span class="hint">Comma-separated port numbers. These are DNAT'd to the target IP above.</span>
        </div>
        <div class="clr-col-md-4">
          <button class="btn btn-primary" style="width:100%;" (click)="savePorts()" [disabled]="savingPorts">
            {{ savingPorts ? 'Saving…' : 'Save Ports' }}
          </button>
        </div>
      </div>

      <div style="margin-top:24px; padding:16px; background:rgba(255,255,255,0.03); border-radius:6px;">
        <span class="fl" style="margin-bottom:8px;">Routing Status</span>
        <div style="display:flex;gap:24px;">
          <div><span class="lbl">State</span> <app-status-badge [label]="routeState||'unknown'" [state]="routeState||''"></app-status-badge></div>
          <div><span class="lbl">Rules Applied</span> <span class="val">{{ rulesApplied }}</span></div>
          <div><span class="lbl">Last Apply</span> <span class="val">{{ lastApply || '—' }}</span></div>
        </div>
      </div>

      <span *ngIf="msg" style="display:block;margin-top:12px;"
            [style.color]="msg.startsWith('Saved')||msg.startsWith('DNAT')?'#2e7d32':'#c62828'">{{ msg }}</span>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3,h4 { color: #333; margin: 0 0 16px 0; } h4 { font-size: 14px; margin-top: 24px; }
    .fl { display: block; font-size: 12px; color: #9e9e9e; margin-bottom: 4px; }
    .hint { color: #757575; font-weight: normal; font-size: 11px; }

    
    
    .lbl { font-size: 10px; color: #757575; text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 2px; }
    .val { font-size: 14px; color: #333; font-weight: 500; }
  `]
})
export class PortForwardComponent implements OnInit {
  targetIp = ''; targetPort = 5684; forwardPorts = '80,443,5684';
  savingDnat = false; savingPorts = false; msg = '';
  routeState = ''; rulesApplied = 0; lastApply = '';

  constructor(private http: HttpsvcService) {}

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
