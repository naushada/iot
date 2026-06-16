import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
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
      <div class="header-row">
        <h3>Firewall &amp; Forwarding Rules</h3>
        <app-status-badge [label]="'router ' + (routeState||'unknown')" [state]="routeState||''"></app-status-badge>
      </div>
      <p *ngIf="routeState==='bad_custom_rules'" class="bad">
        Malformed rules JSON — the daemon kept the previous ruleset.
      </p>

      <!-- Add rule (admin) -->
      <div class="card" *ngIf="isAdmin" style="margin-bottom:20px;">
        <div class="card-header">Add Firewall Rule</div>
        <div class="card-block">
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
            <!-- pad the forward row to a full 4 columns -->
            <div *ngIf="nAction==='forward'"></div>
            <div *ngIf="nAction==='forward'"></div>
          </div>
          <button class="btn btn-primary" (click)="addRule()" [disabled]="saving">
            {{ saving ? 'Saving…' : 'Add Rule' }}
          </button>
        </div>
      </div>

      <!-- Firewall (custom) rules -->
      <h4>Firewall Rules <span class="hint">(net.custom.rules)</span></h4>
      <clr-datagrid>
        <clr-dg-column>Action</clr-dg-column>
        <clr-dg-column>Protocol</clr-dg-column>
        <clr-dg-column>Dest Port</clr-dg-column>
        <clr-dg-column>Src Port</clr-dg-column>
        <clr-dg-column>Forward To</clr-dg-column>
        <clr-dg-column *ngIf="isAdmin">Actions</clr-dg-column>

        <clr-dg-row *clrDgItems="let r of rules">
          <clr-dg-cell>
            <app-status-badge [label]="r.action"
              [state]="r.action==='drop' ? 'exited' : 'connected'"></app-status-badge>
          </clr-dg-cell>
          <clr-dg-cell>{{ r.proto }}</clr-dg-cell>
          <clr-dg-cell>{{ r.dport != null ? r.dport : '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ r.sport != null ? r.sport : '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ r.action==='forward' ? (r.to_ip + (r.to_port!=null ? ':'+r.to_port : '')) : '—' }}</clr-dg-cell>
          <clr-dg-cell *ngIf="isAdmin">
            <button class="btn btn-sm btn-danger" (click)="removeRule(r)" [disabled]="saving">Delete</button>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ rules.length }} rule{{ rules.length===1?'':'s' }}</clr-dg-footer>
      </clr-datagrid>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    .header-row { display: flex; align-items: center; gap: 12px; margin: 0 0 16px 0; }
    h3 { color: #333; margin: 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 18px 0 10px 0; font-size: 13px; font-weight: 600; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .bad { color: #c62828; font-size: 12px; margin: 0 0 12px 0; }
  `]
})
export class CustomRulesComponent implements OnInit, OnDestroy {
  rules: CustomRule[] = [];
  routeState = '';
  private sub = new Subscription();

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

  constructor(private session: SessionService,
              private toast: ToastService, private ds: DataStoreService) {}

  ngOnInit(): void {
    // Paint instantly from the shared prefetched cache, then stay live off the
    // appglobal store. net.custom.rules is edited only here (persisted on each
    // add/delete) and isn't republished by /status, so it fires once when the
    // prefetch lands; net.state is the live router-state badge.
    this.applyData(this.ds.snapshot());
    this.sub.add(this.ds.observe('net.custom.rules').subscribe(() => this.applyData(this.ds.snapshot())));
    this.sub.add(this.ds.observe('net.state').subscribe(v => { if (v != null) this.routeState = String(v); }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyData(d: Record<string, unknown>): void {
    if (d['net.custom.rules'] != null) {
      try { const arr = JSON.parse(String(d['net.custom.rules'])); this.rules = Array.isArray(arr) ? arr : []; }
      catch { this.rules = []; }
    }
    if (d['net.state'] != null)             this.routeState = String(d['net.state']);
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

  removeRule(rule: CustomRule): void {
    const next = this.rules.filter(r => r !== rule);
    this.persist(next, 'Rule deleted');
  }

  private persist(next: CustomRule[], okMsg: string): void {
    this.saving = true;
    this.ds.write([{ key: 'net.custom.rules', value: JSON.stringify(next) }]).subscribe({
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
