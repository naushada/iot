import { Component, OnInit, OnDestroy } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

/// Containers page — drive the on-device multi-container runtime shim
/// (iot-containerd). Each named container is a row in the grid (state, IP,
/// limits, per-row Run/Stop/Remove); the Add form pulls a new image into a new
/// named container. Live state comes from the container.instances JSON array
/// (one object per container) plus the in-flight pull progress, on a 2s self-poll
/// while the page is open.
///
/// Control plane is the single container.cmd.* envelope: set name + action
/// (pull|run|stop|remove) [+ pull params], then bump container.cmd.request. The
/// daemon serialises one command at a time. Admin-only (containers run as root).

interface ContainerRow {
  name: string; image: string; imageId: string; size: string;
  state: string; ip: string; gateway: string; net: string;
  mem: string; cpus: string; pid: number; exitCode: number | null;
  started: number; error: string;
}

@Component({
  selector: 'app-containers',
  template: `
    <div class="page">
      <h3>Containers</h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <!-- ── Container grid ─────────────────────────────────────────── -->
        <clr-datagrid>
          <clr-dg-column [style.width.px]="140">Name</clr-dg-column>
          <clr-dg-column>Image</clr-dg-column>
          <clr-dg-column [style.width.px]="120">State
            <app-ds-hint *dsDebug key="container.instances"></app-ds-hint></clr-dg-column>
          <clr-dg-column [style.width.px]="120">IP</clr-dg-column>
          <clr-dg-column [style.width.px]="80">Net</clr-dg-column>
          <clr-dg-column [style.width.px]="80">Mem</clr-dg-column>
          <clr-dg-column [style.width.px]="70">CPU</clr-dg-column>
          <clr-dg-column [style.width.px]="200">Actions</clr-dg-column>

          <clr-dg-row *clrDgItems="let r of rows" [clrDgItem]="r">
            <clr-dg-cell><strong>{{ r.name }}</strong></clr-dg-cell>
            <clr-dg-cell><span class="img" [title]="r.image">{{ r.image || '—' }}</span></clr-dg-cell>
            <clr-dg-cell>
              <app-status-badge [label]="r.state" [state]="badge(r.state)"></app-status-badge>
              <div class="err" *ngIf="r.error" [title]="r.error">{{ r.error }}</div>
            </clr-dg-cell>
            <clr-dg-cell>
              <code *ngIf="r.ip">{{ r.ip }}</code>
              <span class="hint" *ngIf="!r.ip">—</span>
            </clr-dg-cell>
            <clr-dg-cell>{{ r.net || 'host' }}</clr-dg-cell>
            <clr-dg-cell>{{ r.mem || '—' }}</clr-dg-cell>
            <clr-dg-cell>{{ r.cpus || '—' }}</clr-dg-cell>
            <clr-dg-cell>
              <button class="btn btn-sm btn-success-outline" (click)="run(r)"
                      [disabled]="!canRun(r)">Run</button>
              <button class="btn btn-sm btn-danger-outline" (click)="stop(r)"
                      [disabled]="!canStop(r)">Stop</button>
              <button class="btn btn-sm btn-outline" (click)="remove(r)"
                      [disabled]="busy(r)">Remove</button>
            </clr-dg-cell>
          </clr-dg-row>

          <clr-dg-placeholder>No containers yet — pull an image below to create one.</clr-dg-placeholder>

          <clr-dg-footer>
            {{ rows.length }} container{{ rows.length === 1 ? '' : 's' }}
            <span class="hint" *ngIf="pulling"> · pulling {{ pullProgress }}%
              <span *ngIf="pullDetail">— {{ pullDetail }}</span></span>
          </clr-dg-footer>
        </clr-datagrid>

        <!-- ── Add (pull a new named container) ───────────────────────── -->
        <h4>Add a container</h4>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container>
            <label>Name</label>
            <input clrInput [(ngModel)]="newName" placeholder="web" autocomplete="off" />
            <clr-control-helper *dsDebug><app-ds-hint key="container.cmd.name"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container style="grid-column: span 2;">
            <label>Image reference</label>
            <input clrInput [(ngModel)]="imageRef" style="width:100%;"
                   placeholder="docker.io/library/nginx:latest" />
            <clr-control-helper *dsDebug><app-ds-hint key="container.cmd.image"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="pull()"
                    [disabled]="pulling || !newName || !imageRef">
              {{ pulling ? 'Pulling…' : 'Pull' }}
            </button>
          </div>

          <clr-select-container>
            <label>Network</label>
            <select clrSelect [(ngModel)]="netMode">
              <option value="host">Host (shared device IP)</option>
              <option value="bridge">Bridge (own IP)</option>
            </select>
            <clr-control-helper *dsDebug><app-ds-hint key="container.cmd.net"></app-ds-hint></clr-control-helper>
          </clr-select-container>
          <clr-input-container>
            <label>Memory limit</label>
            <input clrInput [(ngModel)]="limitMem" placeholder="256M" />
          </clr-input-container>
          <clr-input-container>
            <label>CPUs</label>
            <input clrInput [(ngModel)]="limitCpus" placeholder="0.5" />
          </clr-input-container>
          <div></div>

          <clr-input-container>
            <label>Entrypoint <span class="hint">(override)</span></label>
            <input clrInput [(ngModel)]="entrypoint" placeholder='["/entry.sh"] or blank' />
          </clr-input-container>
          <clr-input-container>
            <label>CMD <span class="hint">(override)</span></label>
            <input clrInput [(ngModel)]="cmd" placeholder='["-g","daemon off;"] or blank' />
          </clr-input-container>
          <clr-input-container>
            <label>Registry user <span class="hint">(optional)</span></label>
            <input clrInput [(ngModel)]="regUser" autocomplete="off" />
          </clr-input-container>
          <clr-password-container>
            <label>Registry password <span class="hint">(optional)</span></label>
            <input clrPassword [(ngModel)]="regPass" autocomplete="new-password" />
          </clr-password-container>
        </div>
        <p class="hint">Pulls from an OCI/Docker registry for this device's
          architecture (linux/arm64) into a new container named above, then
          <strong>Run</strong> it from its row. <strong>Host</strong> networking
          shares the device IP; <strong>Bridge</strong> gives each container its
          own IP (<code>10.88.0.2</code>, <code>.3</code>, …) with masqueraded
          egress. Credentials are write-only — sent once, never read back.</p>
      </ng-container>

      <ng-template #noAccess><p class="hint">You need Admin access to manage containers.</p></ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 18px 0 10px 0; font-size: 13px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; margin-top: 8px; font-weight: 400; }
    .err { color: #c92100; font-size: 11px; margin-top: 2px; max-width: 280px;
      overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .img { font-size: 12px; word-break: break-all; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn { white-space: nowrap; }
    .btn-sm { margin: 0 2px; }
  `]
})
export class ContainersComponent implements OnInit, OnDestroy {
  rows: ContainerRow[] = [];
  pullProgress = 0;
  pullDetail = '';

  // Add form (not overwritten by the poll so typing isn't clobbered).
  newName = '';
  imageRef = '';
  regUser = '';
  regPass = '';
  entrypoint = '';
  cmd = '';
  limitMem = '';
  limitCpus = '';
  netMode = 'host';

  private timer: ReturnType<typeof setInterval> | null = null;
  private readonly KEYS = ['container.instances', 'container.pull.progress', 'container.pull.detail'];

  get isAdmin(): boolean { return this.session.isAdmin; }

  get pulling(): boolean { return this.pullProgress > 0 && this.pullProgress < 100; }

  badge(state: string): string {
    switch (state) {
      case 'running':  return 'connected';
      case 'error':    return 'exited';
      case 'stopped':
      case 'idle':     return 'idle';
      default:         return 'starting';   // pulling/pulled/mounting/created/stopping
    }
  }

  // A pulled container (imageId set) can run when it isn't already in flight.
  canRun(r: ContainerRow): boolean {
    return !!r.imageId && ['pulled', 'stopped', 'error', 'idle'].includes(r.state);
  }
  canStop(r: ContainerRow): boolean {
    return ['running', 'mounting', 'created'].includes(r.state);
  }
  busy(r: ContainerRow): boolean {
    return ['pulling', 'mounting', 'created'].includes(r.state);
  }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService) {}

  ngOnInit(): void {
    if (!this.isAdmin) return;
    this.poll();
    this.timer = setInterval(() => this.poll(), 2000);
  }

  ngOnDestroy(): void {
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
  }

  private poll(): void {
    this.http.dbGet(this.KEYS).subscribe(r => {
      if (!r.ok || !r.data) return;
      const d = r.data;
      try {
        const arr = JSON.parse(String(d['container.instances'] ?? '[]'));
        if (Array.isArray(arr)) {
          this.rows = arr.map((x: Record<string, unknown>) => ({
            name: String(x['name'] ?? ''),
            image: String(x['image'] ?? ''),
            imageId: String(x['imageId'] ?? ''),
            size: String(x['size'] ?? ''),
            state: String(x['state'] ?? 'idle'),
            ip: String(x['ip'] ?? ''),
            gateway: String(x['gateway'] ?? ''),
            net: String(x['net'] ?? 'host'),
            mem: String(x['mem'] ?? ''),
            cpus: String(x['cpus'] ?? ''),
            pid: Number(x['pid']) || 0,
            exitCode: x['exitCode'] === null || x['exitCode'] === undefined ? null : Number(x['exitCode']),
            started: Number(x['started']) || 0,
            error: String(x['error'] ?? ''),
          }));
        }
      } catch { /* keep the previous rows on a transient parse error */ }
      this.pullProgress = Math.max(0, Math.min(100, Number(d['container.pull.progress']) || 0));
      this.pullDetail   = String(d['container.pull.detail'] ?? '');
    });
  }

  // ── commands (the container.cmd.* envelope) ──────────────────────────────

  pull(): void {
    const name = this.newName.trim();
    if (!name) { this.toast.error('Container name required'); return; }
    if (!this.imageRef.trim()) { this.toast.error('Image reference required'); return; }
    const pairs: { key: string; value: unknown }[] = [
      { key: 'container.cmd.name',       value: name },
      { key: 'container.cmd.action',     value: 'pull' },
      { key: 'container.cmd.image',      value: this.imageRef.trim() },
      { key: 'container.cmd.net',        value: this.netMode },
      { key: 'container.cmd.mem',        value: this.limitMem },
      { key: 'container.cmd.cpus',       value: this.limitCpus },
      { key: 'container.cmd.entrypoint', value: this.entrypoint },
      { key: 'container.cmd.cmd',        value: this.cmd },
      { key: 'container.registry.user',  value: this.regUser },
    ];
    if (this.regPass) pairs.push({ key: 'container.registry.pass', value: this.regPass });
    pairs.push({ key: 'container.cmd.request', value: String(Date.now()) });
    this.http.dbSet(pairs).subscribe({
      next: (r) => {
        if (r.ok) { this.toast.success('Pulling ' + this.imageRef + ' as ' + name + '…'); this.regPass = ''; }
        else this.toast.error(r.err || 'Pull failed');
      },
      error: () => this.toast.error('Pull failed'),
    });
  }

  run(r: ContainerRow): void   { this.sendCmd(r.name, 'run',    'Starting ' + r.name + '…'); }
  stop(r: ContainerRow): void  { this.sendCmd(r.name, 'stop',   'Stopping ' + r.name + '…'); }
  remove(r: ContainerRow): void {
    this.sendCmd(r.name, 'remove',
      this.canStop(r) ? 'Stopping ' + r.name + ' — Remove again once stopped' : 'Removing ' + r.name + '…');
  }

  private sendCmd(name: string, action: string, okMsg: string): void {
    this.http.dbSet([
      { key: 'container.cmd.name',    value: name },
      { key: 'container.cmd.action',  value: action },
      { key: 'container.cmd.request', value: String(Date.now()) },
    ]).subscribe({
      next: (r) => { if (r.ok) this.toast.success(okMsg); else this.toast.error(r.err || action + ' failed'); },
      error: () => this.toast.error(action + ' failed'),
    });
  }
}
