import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { DataStoreService } from '../../common/datastore.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

interface FwPkg { pkg: string; version: string; arch: string; ipk_url: string; sha256: string; }
interface Ep { endpoint: string; tun_ip: string; proxy_port: number; registered: boolean; installed_version?: string; }
interface UpdStatus { serial: string; state: number; result: number; version: string; pkg?: string; ts: number; }

@Component({
  selector: 'app-software-update',
  template: `
    <div class="page">
      <h3>Software Update</h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <!-- Select package + push to selected devices -->
        <div class="form-grid" style="align-items:end;">
          <clr-select-container>
            <label>Package</label>
            <select clrSelect [(ngModel)]="selectedUrl">
              <option value="">— select firmware —</option>
              <option *ngFor="let p of manifest" [value]="p.ipk_url">{{ p.pkg }} {{ p.version }} ({{ p.arch }})</option>
            </select>
            <clr-control-helper *dsDebug><app-ds-hint key="cloud.firmware.manifest → cloud.update.request"></app-ds-hint></clr-control-helper>
          </clr-select-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="pushUpdate()"
                    [disabled]="pushing || !selectedUrl || !selected.length">
              {{ pushing ? 'Pushing…' : 'Update ' + selected.length + ' device(s)' }}
            </button>
          </div>
          <div></div>
          <div></div>
        </div>
        <p class="hint" *ngIf="!manifest.length">No firmware in the catalogue. Seed the iot-firmware volume and set <code>cloud.firmware.manifest</code>.</p>

        <!-- Target devices (multi-select) -->
        <h4>Target Devices</h4>
        <clr-datagrid [(clrDgSelected)]="selected">
          <clr-dg-column>Endpoint</clr-dg-column>
          <clr-dg-column>State</clr-dg-column>
          <clr-dg-column>Tunnel IP</clr-dg-column>
          <clr-dg-column>Installed</clr-dg-column>

          <clr-dg-row *clrDgItems="let e of endpoints" [clrDgItem]="e">
            <clr-dg-cell><code>{{ e.endpoint }}</code></clr-dg-cell>
            <clr-dg-cell>
              <app-status-badge [label]="e.registered ? 'online' : 'offline'"
                [state]="e.registered ? 'connected' : 'exited'"></app-status-badge>
            </clr-dg-cell>
            <clr-dg-cell><code>{{ e.tun_ip }}</code></clr-dg-cell>
            <clr-dg-cell>{{ e.installed_version || '—' }}</clr-dg-cell>
          </clr-dg-row>

          <clr-dg-footer>{{ endpoints.length }} device{{ endpoints.length===1?'':'s' }}</clr-dg-footer>
        </clr-datagrid>

        <!-- Update status -->
        <h4 style="margin-top:28px;">Update Status</h4>
        <clr-datagrid>
          <clr-dg-column>Serial</clr-dg-column>
          <clr-dg-column>Package</clr-dg-column>
          <clr-dg-column>State</clr-dg-column>
          <clr-dg-column>Progress</clr-dg-column>
          <clr-dg-column>Result</clr-dg-column>
          <clr-dg-column>Version</clr-dg-column>

          <clr-dg-row *clrDgItems="let s of status">
            <clr-dg-cell><code>{{ s.serial }}</code></clr-dg-cell>
            <clr-dg-cell>{{ s.pkg || '—' }}</clr-dg-cell>
            <clr-dg-cell>
              <app-status-badge [label]="stateLabel(s.state)"
                [state]="s.state===0 ? 'connected' : 'idle'"></app-status-badge>
            </clr-dg-cell>
            <clr-dg-cell>
              <div class="pbar"><span [style.width.%]="progressPct(s)"
                [class.err]="s.result>=5"></span></div>
              <span class="pbar-pct">{{ progressPct(s) }}%</span>
            </clr-dg-cell>
            <clr-dg-cell>
              <app-status-badge [label]="resultLabel(s.result)"
                [state]="s.result===1 ? 'connected' : (s.result>=5 ? 'exited' : 'idle')"></app-status-badge>
            </clr-dg-cell>
            <clr-dg-cell>{{ s.version || '—' }}</clr-dg-cell>
          </clr-dg-row>

          <clr-dg-footer>{{ status.length }} job{{ status.length===1?'':'s' }}</clr-dg-footer>
        </clr-datagrid>
      </ng-container>

      <ng-template #noAccess><p class="hint">You need Admin access to push updates.</p></ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 18px 0 10px 0; font-size: 13px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; margin-top: 8px; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn-primary { white-space: nowrap; }
    .pbar { display: inline-block; width: 90px; height: 8px; background: #e0e6e9;
      border-radius: 4px; overflow: hidden; vertical-align: middle; }
    .pbar > span { display: block; height: 100%; background: #0072a3;
      transition: width 0.3s ease; border-radius: 4px; }
    .pbar > span.err { background: #c92100; }
    .pbar-pct { margin-left: 6px; font-size: 12px; color: #607d8b; vertical-align: middle; }
  `]
})
export class SoftwareUpdateComponent implements OnInit, OnDestroy {
  manifest: FwPkg[] = [];
  endpoints: Ep[] = [];
  status: UpdStatus[] = [];
  selected: Ep[] = [];
  selectedUrl = '';
  pushing = false;
  private active = true;
  private sub = new Subscription();

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private ds: DataStoreService,
              private session: SessionService, private toast: ToastService) {}

  ngOnInit(): void {
    if (!this.isAdmin) return;
    this.loadManifest();
    this.pollEndpoints();
    // Per-device OTA push progress live off the single shared /status stream
    // (the long-poll wakes on cloud.update.status) — no per-page 5s self-poll.
    this.sub.add(this.ds.observe('cloud.update.status').subscribe((v) => {
      try { const a = JSON.parse(String(v ?? '[]')); this.status = Array.isArray(a) ? a : []; }
      catch { this.status = []; }
    }));
  }

  private loadManifest(): void {
    this.http.dbGet(['cloud.firmware.manifest']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          try { const a = JSON.parse(String((r.data as Record<string, unknown>)['cloud.firmware.manifest'] || '[]'));
                this.manifest = Array.isArray(a) ? a : []; } catch { this.manifest = []; }
        }
      }
    });
  }

  private pollEndpoints(): void {
    if (!this.active) return;
    this.http.getCloudEndpoints().subscribe({
      next: (eps) => { this.endpoints = eps as Ep[]; if (this.active) setTimeout(() => this.pollEndpoints(), 5000); },
      error: () => { if (this.active) setTimeout(() => this.pollEndpoints(), 5000); }
    });
  }


  stateLabel(s: number): string {
    return ['idle', 'downloading', 'downloaded', 'updating'][s] || 'unknown';
  }
  resultLabel(r: number): string {
    if (r === 0) return '—';
    if (r === 1) return 'success';
    if (r === 5) return 'integrity error';
    if (r === 8) return 'uri error';
    if (r === 9) return 'install error';
    return 'error ' + r;
  }
  // Cloud-side progress is PHASE-based (the device's byte-accurate % shows on the
  // device-ui): the cloud sees the coarse Object-5 state, not the live download.
  // 100% once the device reports success or its installed version matches the
  // target; a failed job freezes the bar (rendered red) at the phase it died.
  progressPct(s: UpdStatus): number {
    if (s.result === 1) return 100;
    const ep = this.endpoints.find(e => e.endpoint === s.serial);
    if (ep?.installed_version && ep.installed_version === s.version) return 100;
    if (s.result >= 5) return s.state >= 2 ? 80 : 30;
    return [0, 30, 65, 90][s.state] ?? 0;
  }

  pushUpdate(): void {
    const pkg = this.manifest.find(p => p.ipk_url === this.selectedUrl);
    if (!pkg) { this.toast.error('Select a package'); return; }
    if (!this.selected.length) { this.toast.error('Select target devices'); return; }
    this.pushing = true;
    const req = {
      serials: this.selected.map(e => e.endpoint),
      pkg: pkg.pkg, version: pkg.version, url: pkg.ipk_url, sha256: pkg.sha256,
    };
    this.http.dbSet([{ key: 'cloud.update.request', value: JSON.stringify(req) }]).subscribe({
      next: (r) => {
        this.pushing = false;
        if (r.ok) { this.toast.success('Update queued for ' + req.serials.length + ' device(s)'); }
        else { this.toast.error(r.err || 'Update failed'); }
      },
      error: () => { this.pushing = false; this.toast.error('Update failed'); }
    });
  }

  ngOnDestroy(): void { this.active = false; this.sub.unsubscribe(); }
}
