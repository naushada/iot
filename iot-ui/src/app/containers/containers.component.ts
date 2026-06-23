import { Component, OnInit, OnDestroy } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

/// Containers page — drive the on-device single-container runtime shim
/// (iot-containerd) over the container.* data-store keys. Pull an OCI/Docker
/// image, run it with an optional CMD/Entrypoint + resource caps, and stop it.
///
/// Admin-only (the daemon runs containers as root). Live status comes from a
/// lightweight 2s self-poll of the container.* keys while the page is open —
/// this page is only mounted when an operator is actively managing a container,
/// so it does not warrant a slot on the shared /status long-poll.
@Component({
  selector: 'app-containers',
  template: `
    <div class="page">
      <h3>Containers</h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <!-- ── Status ─────────────────────────────────────────────── -->
        <clr-datagrid style="margin-bottom:20px;">
          <clr-dg-column>Property</clr-dg-column>
          <clr-dg-column>Value</clr-dg-column>
          <clr-dg-row>
            <clr-dg-cell>State
              <app-ds-hint *dsDebug key="container.state"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="state || 'idle'" [state]="badgeState"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="imageId">
            <clr-dg-cell>Image
              <app-ds-hint *dsDebug key="container.image.id"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><code class="digest">{{ imageId }}</code></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="imageSize > 0">
            <clr-dg-cell>Size
              <app-ds-hint *dsDebug key="container.image.size"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>{{ sizeLabel }}</clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row>
            <clr-dg-cell>Network
              <app-ds-hint *dsDebug key="container.net.mode"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>{{ netMode === 'bridge' ? 'Bridge (own IP)' : 'Host (shared device IP)' }}</clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="netIp">
            <clr-dg-cell>IP
              <app-ds-hint *dsDebug key="container.net.ip"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><code>{{ netIp }}</code><span class="hint" *ngIf="netGateway"> · gw {{ netGateway }}</span></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="state === 'running' && runPid">
            <clr-dg-cell>PID
              <app-ds-hint *dsDebug key="container.run.pid"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>{{ runPid }}<span class="hint" *ngIf="startedLabel"> · started {{ startedLabel }}</span></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="state === 'pulling'">
            <clr-dg-cell>Pull
              <app-ds-hint *dsDebug key="container.pull.progress"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>
              <ng-container *ngIf="pullProgress > 0; else indet">
                <div class="pbar"><span [style.width.%]="pullProgress"></span></div>
                <span class="pbar-pct">{{ pullProgress }}%</span>
              </ng-container>
              <ng-template #indet>
                <div class="pbar indet"><span></span></div>
                <span class="pbar-pct">pulling…</span>
              </ng-template>
              <span class="hint" *ngIf="pullDetail"> {{ pullDetail }}</span>
            </clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="statusMsg">
            <clr-dg-cell>Status
              <app-ds-hint *dsDebug key="container.status"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>{{ statusMsg }}</clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="error">
            <clr-dg-cell>Error
              <app-ds-hint *dsDebug key="container.error"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><span class="err">{{ error }}</span></clr-dg-cell>
          </clr-dg-row>
        </clr-datagrid>

        <!-- ── Pull an image ──────────────────────────────────────── -->
        <h4>Pull an image</h4>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container style="grid-column: span 3;">
            <label>Image reference</label>
            <input clrInput [(ngModel)]="imageRef" style="width:100%;"
                   placeholder="docker.io/library/nginx:latest" />
            <clr-control-helper *dsDebug><app-ds-hint key="container.image.ref"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="pull()"
                    [disabled]="state === 'pulling' || !imageRef">
              {{ state === 'pulling' ? 'Pulling…' : 'Pull' }}
            </button>
          </div>
          <clr-input-container>
            <label>Registry user <span class="hint">(optional)</span></label>
            <input clrInput [(ngModel)]="regUser" autocomplete="off" />
          </clr-input-container>
          <clr-password-container>
            <label>Registry password <span class="hint">(optional)</span></label>
            <input clrPassword [(ngModel)]="regPass" autocomplete="new-password" />
          </clr-password-container>
          <div></div>
          <div></div>
        </div>
        <p class="hint">Pulls from an OCI/Docker registry for this device's
          architecture (linux/arm64). Credentials are write-only — sent once and
          never read back.</p>

        <!-- ── Run ────────────────────────────────────────────────── -->
        <h4>Run</h4>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container>
            <label>Entrypoint <span class="hint">(override)</span></label>
            <input clrInput [(ngModel)]="entrypoint" placeholder='["/entry.sh"] or blank' />
            <clr-control-helper *dsDebug><app-ds-hint key="container.entrypoint"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>CMD <span class="hint">(override)</span></label>
            <input clrInput [(ngModel)]="cmd" placeholder='["-g","daemon off;"] or blank' />
            <clr-control-helper *dsDebug><app-ds-hint key="container.cmd"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Memory limit</label>
            <input clrInput [(ngModel)]="limitMem" placeholder="256M" />
            <clr-control-helper *dsDebug><app-ds-hint key="container.limit.mem"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>CPUs</label>
            <input clrInput [(ngModel)]="limitCpus" placeholder="0.5" />
            <clr-control-helper *dsDebug><app-ds-hint key="container.limit.cpus"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-select-container>
            <label>Network</label>
            <select clrSelect [(ngModel)]="netMode">
              <option value="host">Host (shared device IP)</option>
              <option value="bridge">Bridge (own IP)</option>
            </select>
            <clr-control-helper *dsDebug><app-ds-hint key="container.net.mode"></app-ds-hint></clr-control-helper>
          </clr-select-container>
          <div></div>
          <div></div>
          <div></div>
          <div class="btn-cell">
            <button class="btn btn-success-outline" (click)="run()"
                    [disabled]="!canRun">
              {{ runVerb }}
            </button>
          </div>
          <div class="btn-cell">
            <button class="btn btn-danger-outline" (click)="stop()"
                    [disabled]="!canStop">Stop</button>
          </div>
          <div></div>
          <div></div>
        </div>
        <p class="hint">Entrypoint/CMD accept a JSON array
          (<code>["/bin/sh","-c","echo hi"]</code>) or a plain space-separated
          string; leave blank to use the image defaults. Memory/CPU are optional
          caps. <strong>Host</strong> networking shares the device's IP;
          <strong>Bridge</strong> gives the container its own IP
          (<code>10.88.0.2</code>) with masqueraded egress.</p>
      </ng-container>

      <ng-template #noAccess><p class="hint">You need Admin access to manage containers.</p></ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 18px 0 10px 0; font-size: 13px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; margin-top: 8px; font-weight: 400; }
    .err { color: #c92100; font-size: 13px; }
    .digest { font-size: 12px; word-break: break-all; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn { white-space: nowrap; }
    .pbar { display: inline-block; width: 140px; height: 8px; background: #e0e6e9;
      border-radius: 4px; overflow: hidden; vertical-align: middle; }
    .pbar > span { display: block; height: 100%; background: #0072a3;
      transition: width 0.3s ease; border-radius: 4px; }
    .pbar.indet > span { width: 35%; animation: pbar-slide 1.1s ease-in-out infinite; }
    .pbar-pct { margin-left: 8px; font-size: 12px; color: #607d8b; vertical-align: middle; }
    @keyframes pbar-slide { 0% { margin-left: -35%; } 100% { margin-left: 100%; } }
  `]
})
export class ContainersComponent implements OnInit, OnDestroy {
  // Live status (polled).
  state = 'idle';
  statusMsg = '';
  error = '';
  pullProgress = 0;
  pullDetail = '';
  imageId = '';
  imageSize = 0;
  runPid = '';
  runStarted = '';
  netIp = '';
  netGateway = '';

  // Form (loaded once; not overwritten by the poll so typing isn't clobbered).
  imageRef = '';
  regUser = '';
  regPass = '';
  entrypoint = '';
  cmd = '';
  limitMem = '';
  limitCpus = '';
  netMode = 'host';

  private timer: ReturnType<typeof setInterval> | null = null;

  private readonly STATUS_KEYS = [
    'container.state', 'container.status', 'container.error',
    'container.pull.progress', 'container.pull.detail',
    'container.image.id', 'container.image.size',
    'container.run.pid', 'container.run.started',
    'container.net.ip', 'container.net.gateway',
  ];
  private readonly CONFIG_KEYS = [
    'container.image.ref', 'container.entrypoint', 'container.cmd',
    'container.limit.mem', 'container.limit.cpus', 'container.net.mode',
  ];

  get isAdmin(): boolean { return this.session.isAdmin; }

  get badgeState(): string {
    switch (this.state) {
      case 'running':  return 'connected';
      case 'error':    return 'exited';
      case 'stopped':
      case 'idle':     return 'idle';
      default:         return 'starting';   // pulling/pulled/mounting/created
    }
  }

  // A pulled image (image.id set) can be run when nothing is in flight.
  get canRun(): boolean {
    return !!this.imageId &&
           this.state !== 'pulling' && this.state !== 'mounting' &&
           this.state !== 'running' && this.state !== 'created';
  }
  get canStop(): boolean {
    return this.state === 'running' || this.state === 'mounting' || this.state === 'created';
  }
  get runVerb(): string {
    if (this.state === 'mounting') return 'Mounting…';
    if (this.state === 'running')  return 'Running';
    return 'Run';
  }

  get sizeLabel(): string {
    let v = this.imageSize, i = 0;
    const u = ['B', 'KB', 'MB', 'GB'];
    while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
    return (i === 0 ? v.toFixed(0) : v.toFixed(v < 10 ? 1 : 0)) + ' ' + u[i];
  }
  get startedLabel(): string {
    const t = Number(this.runStarted);
    return t > 0 ? new Date(t * 1000).toLocaleString() : '';
  }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService) {}

  ngOnInit(): void {
    if (!this.isAdmin) return;
    // Prefill the form once from the saved config keys.
    this.http.dbGet(this.CONFIG_KEYS).subscribe(r => {
      if (r.ok && r.data) {
        const d = r.data;
        this.imageRef   = String(d['container.image.ref'] ?? '');
        this.entrypoint = String(d['container.entrypoint'] ?? '');
        this.cmd        = String(d['container.cmd'] ?? '');
        this.limitMem   = String(d['container.limit.mem'] ?? '');
        this.limitCpus  = String(d['container.limit.cpus'] ?? '');
        this.netMode    = String(d['container.net.mode'] ?? 'host') === 'bridge' ? 'bridge' : 'host';
      }
    });
    this.poll();
    this.timer = setInterval(() => this.poll(), 2000);
  }

  ngOnDestroy(): void {
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
  }

  private poll(): void {
    this.http.dbGet(this.STATUS_KEYS).subscribe(r => {
      if (!r.ok || !r.data) return;
      const d = r.data;
      this.state        = String(d['container.state'] ?? 'idle');
      this.statusMsg    = String(d['container.status'] ?? '');
      this.error        = String(d['container.error'] ?? '');
      this.pullProgress = Math.max(0, Math.min(100, Number(d['container.pull.progress']) || 0));
      this.pullDetail   = String(d['container.pull.detail'] ?? '');
      this.imageId      = String(d['container.image.id'] ?? '');
      this.imageSize    = Number(d['container.image.size']) || 0;
      this.runPid       = String(d['container.run.pid'] ?? '');
      this.runStarted   = String(d['container.run.started'] ?? '');
      this.netIp        = String(d['container.net.ip'] ?? '');
      this.netGateway   = String(d['container.net.gateway'] ?? '');
    });
  }

  pull(): void {
    if (!this.imageRef) { this.toast.error('Image reference required'); return; }
    const pairs: { key: string; value: unknown }[] = [
      { key: 'container.image.ref', value: this.imageRef },
      { key: 'container.registry.user', value: this.regUser },
    ];
    if (this.regPass) pairs.push({ key: 'container.registry.pass', value: this.regPass });
    pairs.push({ key: 'container.pull.request', value: String(Date.now()) });
    this.http.dbSet(pairs).subscribe({
      next: (r) => {
        if (r.ok) { this.toast.success('Pulling ' + this.imageRef + '…'); this.regPass = ''; }
        else this.toast.error(r.err || 'Pull failed');
      },
      error: () => this.toast.error('Pull failed'),
    });
  }

  run(): void {
    if (!this.imageId) { this.toast.error('Pull an image first'); return; }
    this.http.dbSet([
      { key: 'container.entrypoint', value: this.entrypoint },
      { key: 'container.cmd', value: this.cmd },
      { key: 'container.limit.mem', value: this.limitMem },
      { key: 'container.limit.cpus', value: this.limitCpus },
      { key: 'container.net.mode', value: this.netMode },
      { key: 'container.run.request', value: String(Date.now()) },
    ]).subscribe({
      next: (r) => { if (r.ok) this.toast.success('Starting container…'); else this.toast.error(r.err || 'Run failed'); },
      error: () => this.toast.error('Run failed'),
    });
  }

  stop(): void {
    this.http.dbSet([{ key: 'container.stop.request', value: String(Date.now()) }]).subscribe({
      next: (r) => { if (r.ok) this.toast.success('Stopping container…'); else this.toast.error(r.err || 'Stop failed'); },
      error: () => this.toast.error('Stop failed'),
    });
  }
}
