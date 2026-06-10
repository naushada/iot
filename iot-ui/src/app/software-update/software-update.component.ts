import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

@Component({
  selector: 'app-software-update',
  template: `
    <div class="page">
      <h3>Software Update</h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <!-- Current state -->
        <clr-datagrid style="margin-bottom:20px;">
          <clr-dg-column>Property</clr-dg-column>
          <clr-dg-column>Value</clr-dg-column>
          <clr-dg-row>
            <clr-dg-cell>Installed Version</clr-dg-cell>
            <clr-dg-cell>{{ version || '—' }}</clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row>
            <clr-dg-cell>State</clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="stateLabel"
              [state]="state===0 ? 'connected' : 'idle'"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row>
            <clr-dg-cell>Result</clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="resultLabel"
              [state]="result===1 ? 'connected' : (result>=5 ? 'exited' : 'idle')"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
        </clr-datagrid>

        <!-- Self-update trigger -->
        <h4>Apply a package (.ipk URL)</h4>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container style="grid-column: span 3;">
            <label>Package URL</label>
            <input clrInput [(ngModel)]="url" style="width:100%;"
                   placeholder="http://10.9.0.1:8080/firmware/iot_0.2.0_aarch64.ipk?sha256=..." />
          </clr-input-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="apply()" [disabled]="busy || !url">
              {{ busy ? 'Applying…' : 'Apply' }}
            </button>
          </div>
        </div>
        <p class="hint">Runs <code>opkg install</code> on the device and restarts the affected daemons. The URL may carry <code>?sha256=</code> &amp; <code>?version=</code>.</p>
      </ng-container>

      <ng-template #noAccess><p class="hint">You need Admin access to update software.</p></ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 18px 0 10px 0; font-size: 13px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; margin-top: 8px; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn-primary { white-space: nowrap; }
  `]
})
export class SoftwareUpdateComponent implements OnInit, OnDestroy {
  version = '';
  state = 0;
  result = 0;
  url = '';
  busy = false;
  private active = true;
  private sub = new Subscription();

  get isAdmin(): boolean { return this.session.isAdmin; }
  get stateLabel(): string {
    return ['idle', 'downloading', 'downloaded', 'updating'][this.state] || 'unknown';
  }
  get resultLabel(): string {
    if (this.result === 0) return '—';
    if (this.result === 1) return 'success';
    if (this.result === 5) return 'integrity error';
    if (this.result === 8) return 'uri error';
    if (this.result === 9) return 'install error';
    return 'error ' + this.result;
  }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService) {}

  ngOnInit(): void { if (this.isAdmin) this.poll(); }

  private poll(): void {
    if (!this.active) return;
    this.http.dbGet(['iot.update.version', 'iot.update.state', 'iot.update.result']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          if (d['iot.update.version'] != null) this.version = String(d['iot.update.version']);
          if (d['iot.update.state'] != null)   this.state = Number(d['iot.update.state']) || 0;
          if (d['iot.update.result'] != null)  this.result = Number(d['iot.update.result']) || 0;
        }
        if (this.active) setTimeout(() => this.poll(), 5000);
      },
      error: () => { if (this.active) setTimeout(() => this.poll(), 5000); }
    });
  }

  apply(): void {
    if (!this.url) { this.toast.error('Package URL required'); return; }
    this.busy = true;
    this.http.dbSet([{ key: 'iot.update.request', value: this.url }]).subscribe({
      next: (r) => {
        this.busy = false;
        if (r.ok) { this.toast.success('Update started'); }
        else { this.toast.error(r.err || 'Update failed'); }
      },
      error: () => { this.busy = false; this.toast.error('Update failed'); }
    });
  }

  ngOnDestroy(): void { this.active = false; this.sub.unsubscribe(); }
}
