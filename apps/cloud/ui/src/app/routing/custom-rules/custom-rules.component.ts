import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { DataStoreService } from '../../../common/datastore.service';

/// One firewall rule as stored in net.custom.rules (JSON array).
/// Mirrors the CustomRule POD parsed by the net-router daemon
/// (modules/net/router/src/nft_rules.cpp): action + proto are required;
/// to_ip/to_port apply only to action="forward".
interface CustomRule {
  action: string;            // "accept" | "drop" | "forward"
  proto: string;             // "tcp" | "udp"
  dport?: number;
  sport?: number;
  to_ip?: string;
  to_port?: number;
}

@Component({
  selector: 'app-custom-rules',
  template: `
    <div class="page">
      <h3>Firewall &amp; Forwarding Rules</h3>

      <table class="table table-compact table-borderless" style="margin-bottom:20px;">
        <tbody>
          <tr><td class="label-col">Router State</td>
            <td><app-status-badge [label]="routeState||'unknown'" [state]="routeState||''"></app-status-badge>
              <span *ngIf="routeState==='bad_custom_rules'" class="bad">malformed rules JSON — daemon kept the previous ruleset</span>
            </td></tr>
        </tbody>
      </table>

      <!-- Firewall (custom) rules -->
      <h4>Firewall Rules <span class="hint">(net.custom.rules)</span></h4>
      <table class="table table-compact">
        <thead>
          <tr><th class="left">Action</th><th class="left">Proto</th><th>Dest Port</th>
              <th>Src Port</th><th class="left">Forward To</th><th></th></tr>
        </thead>
        <tbody>
          <tr *ngIf="!rules.length"><td colspan="6" class="muted">No firewall rules.</td></tr>
          <tr *ngFor="let r of rules; let i = index">
            <td class="left"><span class="pill" [class.drop]="r.action==='drop'">{{ r.action }}</span></td>
            <td class="left">{{ r.proto }}</td>
            <td>{{ r.dport != null ? r.dport : '—' }}</td>
            <td>{{ r.sport != null ? r.sport : '—' }}</td>
            <td class="left">{{ r.action==='forward' ? (r.to_ip + (r.to_port!=null ? ':'+r.to_port : '')) : '—' }}</td>
            <td class="right">
              <button class="btn btn-sm btn-danger-outline" *ngIf="isAdmin" (click)="removeRule(i)" [disabled]="saving">Delete</button>
            </td>
          </tr>
        </tbody>
      </table>

      <!-- Add-rule form -->
      <div *ngIf="isAdmin" class="add-form">
        <h4>Add Firewall Rule</h4>
        <div class="form-grid" style="align-items:end;">
          <clr-select-container>
            <label>Action</label>
            <select clrSelect [(ngModel)]="nAction">
              <option *ngFor="let a of actions" [value]="a">{{ a }}</option>
            </select>
          </clr-select-container>
          <clr-select-container>
            <label>Protocol</label>
            <select clrSelect [(ngModel)]="nProto">
              <option *ngFor="let p of protos" [value]="p">{{ p }}</option>
            </select>
          </clr-select-container>
          <clr-input-container>
            <label>Dest Port</label>
            <input clrInput type="number" [(ngModel)]="nDport" placeholder="optional" />
          </clr-input-container>
          <clr-input-container>
            <label>Src Port</label>
            <input clrInput type="number" [(ngModel)]="nSport" placeholder="optional" />
          </clr-input-container>
          <clr-input-container *ngIf="nAction==='forward'">
            <label>Forward To IP</label>
            <input clrInput [(ngModel)]="nToIp" placeholder="10.9.0.12" />
          </clr-input-container>
          <clr-input-container *ngIf="nAction==='forward'">
            <label>Forward To Port</label>
            <input clrInput type="number" [(ngModel)]="nToPort" placeholder="5684" />
          </clr-input-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="addRule()" [disabled]="saving">
              {{ saving ? 'Saving…' : 'Add Rule' }}
            </button>
          </div>
        </div>
      </div>

      <!-- Forwarding rules (derived, read-only) -->
      <h4 style="margin-top:28px;">Port Forwarding <span class="hint">(net.forward.ports → DNAT target)</span></h4>
      <table class="table table-compact">
        <thead><tr><th class="left">Port</th><th class="left">DNAT To</th></tr></thead>
        <tbody>
          <tr *ngIf="!forwardPorts.length"><td colspan="2" class="muted">No forwarded ports.</td></tr>
          <tr *ngFor="let p of forwardPorts">
            <td class="left">{{ p }}</td>
            <td class="left">{{ targetIp || 'target IP' }}:{{ targetPort }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 18px 0 10px 0; font-size: 13px; font-weight: 600; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .muted { color: #999; }
    .bad { color: #c62828; font-size: 12px; margin-left: 8px; }
    table { width: 100%; }
    th.left, td.left { text-align: left; }
    td.right, th:last-child { text-align: right; }
    .pill { background: #e3f2e8; color: #2e7d32; padding: 1px 8px; border-radius: 10px; font-size: 11px; text-transform: uppercase; }
    .pill.drop { background: #fdecea; color: #c62828; }
    .add-form { margin-top: 16px; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn-primary { white-space: nowrap; }
  `]
})
export class CustomRulesComponent implements OnInit {
  rules: CustomRule[] = [];
  forwardPorts: string[] = [];
  targetIp = '';
  targetPort = 5684;
  routeState = '';

  // Add-rule form state
  nAction = 'accept';
  nProto = 'tcp';
  nDport: number | null = null;
  nSport: number | null = null;
  nToIp = '';
  nToPort: number | null = null;
  actions = ['accept', 'drop', 'forward'];
  protos = ['tcp', 'udp'];
  saving = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService, private ds: DataStoreService) {}

  ngOnInit(): void {
    // Instant paint from the prefetched cache, then refresh from the wire.
    this.applyData(this.ds.snapshot());
    this.http.dbGet(['net.custom.rules', 'net.forward.ports',
      'net.lwm2m.target.ip', 'net.lwm2m.target.port', 'net.state']).subscribe({
      next: (r) => { if (r.ok && r.data) this.applyData(r.data as Record<string, unknown>); }
    });
  }

  private applyData(d: Record<string, unknown>): void {
    if (d['net.custom.rules'] != null) {
      try { const arr = JSON.parse(String(d['net.custom.rules'])); this.rules = Array.isArray(arr) ? arr : []; }
      catch { this.rules = []; }
    }
    if (d['net.lwm2m.target.ip'] != null)   this.targetIp   = String(d['net.lwm2m.target.ip']);
    if (d['net.lwm2m.target.port'] != null) this.targetPort = Number(d['net.lwm2m.target.port']) || 5684;
    if (d['net.state'] != null)             this.routeState = String(d['net.state']);
    if (d['net.forward.ports'] != null) {
      this.forwardPorts = String(d['net.forward.ports']).split(',').map(s => s.trim()).filter(Boolean);
    }
  }

  addRule(): void {
    if (!this.nAction || !this.nProto) { this.toast.error('Action and protocol are required'); return; }
    const rule: CustomRule = { action: this.nAction, proto: this.nProto };
    if (this.nDport != null && String(this.nDport) !== '') rule.dport = Number(this.nDport);
    if (this.nSport != null && String(this.nSport) !== '') rule.sport = Number(this.nSport);
    if (this.nAction === 'forward') {
      if (!this.nToIp) { this.toast.error('forward action needs a target IP'); return; }
      rule.to_ip = this.nToIp;
      if (this.nToPort != null && String(this.nToPort) !== '') rule.to_port = Number(this.nToPort);
    }
    this.persist([...this.rules, rule], 'Rule added');
  }

  removeRule(i: number): void {
    const next = this.rules.slice();
    next.splice(i, 1);
    this.persist(next, 'Rule deleted');
  }

  private persist(next: CustomRule[], okMsg: string): void {
    this.saving = true;
    this.http.dbSet([{ key: 'net.custom.rules', value: JSON.stringify(next) }]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) { this.rules = next; this.toast.success(okMsg); this.resetForm(); }
        else { this.toast.error(r.err || 'Save failed'); }
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }

  private resetForm(): void {
    this.nAction = 'accept'; this.nProto = 'tcp';
    this.nDport = null; this.nSport = null; this.nToIp = ''; this.nToPort = null;
  }
}
