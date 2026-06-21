import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { DataStoreService } from '../../common/datastore.service';
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
            <clr-dg-cell>Installed Version
              <app-ds-hint *dsDebug key="iot.version"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>{{ version || '—' }}</clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="pkg">
            <clr-dg-cell>Package
              <app-ds-hint *dsDebug key="iot.update.package"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><code>{{ pkg }}</code></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row>
            <clr-dg-cell>State
              <app-ds-hint *dsDebug key="iot.update.state"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="stateLabel"
              [state]="state===0 ? 'connected' : 'idle'"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="state===1">
            <clr-dg-cell>Download
              <app-ds-hint *dsDebug key="iot.update.progress"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>
              <ng-container *ngIf="progress > 0; else indeterminate">
                <div class="pbar"><span [style.width.%]="progress"></span></div>
                <span class="pbar-pct">{{ progress }}%</span>
              </ng-container>
              <ng-template #indeterminate>
                <div class="pbar indet"><span></span></div>
                <span class="pbar-pct">downloading…</span>
              </ng-template>
            </clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row>
            <clr-dg-cell>Result
              <app-ds-hint *dsDebug key="iot.update.result"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="resultLabel"
              [state]="result===1 ? 'connected' : (result>=5 ? 'exited' : 'idle')"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row *ngIf="bank && !banks.length">
            <clr-dg-cell>Running Bank <span class="hint">(A/B image)</span>
              <app-ds-hint *dsDebug key="iot.boot.bank"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="bank + (confirmed ? ' · confirmed' : ' · unconfirmed')"
              [state]="confirmed ? 'connected' : 'starting'"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
        </clr-datagrid>

        <!-- A/B banks: both rootfs banks, their installed version, and a
             switch-and-reboot action on the inactive one. -->
        <ng-container *ngIf="banks.length">
          <h4>A/B Banks <span class="hint">(dual-bank image)</span>
            <app-ds-hint *dsDebug key="iot.boot.banks"></app-ds-hint></h4>
          <clr-datagrid style="margin-bottom:8px;">
            <clr-dg-column>Bank</clr-dg-column>
            <clr-dg-column>Installed Version</clr-dg-column>
            <clr-dg-column>Status</clr-dg-column>
            <clr-dg-column>Action</clr-dg-column>
            <clr-dg-row *ngFor="let b of banks">
              <clr-dg-cell>{{ b.bootname || '?' }} <span class="hint">({{ b.slot }})</span></clr-dg-cell>
              <clr-dg-cell>{{ b.version || '—' }}</clr-dg-cell>
              <clr-dg-cell>
                <app-status-badge
                  [label]="b.booted ? ('running' + (confirmed ? ' · confirmed' : ' · unconfirmed')) : (b.state || 'standby')"
                  [state]="b.booted ? (confirmed ? 'connected' : 'starting') : (b.state === 'bad' ? 'exited' : 'idle')">
                </app-status-badge>
              </clr-dg-cell>
              <clr-dg-cell>
                <button *ngIf="!b.booted" class="btn btn-sm btn-outline" (click)="askSwitch(b)"
                        [disabled]="switching">Switch &amp; reboot</button>
                <span *ngIf="b.booted" class="hint">current</span>
              </clr-dg-cell>
            </clr-dg-row>
          </clr-datagrid>
          <p class="hint">Switching marks the selected bank for the next boot and reboots the device.
            If the new bank fails to come up cleanly after {{ bootAttempts }} attempts, the bootloader
            automatically rolls back to the current bank.</p>
        </ng-container>

        <!-- Reboot confirmation (switching banks reboots the device). -->
        <clr-modal [(clrModalOpen)]="showSwitchModal">
          <h3 class="modal-title">Switch bank &amp; reboot?</h3>
          <div class="modal-body">
            <p>Reboot into bank <strong>{{ switchTarget?.bootname }}</strong>
               (version {{ switchTarget?.version || 'unknown' }})?</p>
            <p class="hint">The device reboots immediately. If the selected bank does not come up
               cleanly it rolls back to the current bank automatically — but the device will be
               briefly offline either way.</p>
          </div>
          <div class="modal-footer">
            <button type="button" class="btn btn-outline" (click)="showSwitchModal=false"
                    [disabled]="switching">Cancel</button>
            <button type="button" class="btn btn-danger" (click)="confirmSwitch()"
                    [disabled]="switching">
              <span *ngIf="!switching">Switch &amp; reboot</span>
              <span *ngIf="switching">Rebooting…</span>
            </button>
          </div>
        </clr-modal>

        <!-- Self-update trigger -->
        <h4>Apply a package (.ipk URL)</h4>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container style="grid-column: span 3;">
            <label>Package URL</label>
            <input clrInput [(ngModel)]="url" style="width:100%;"
                   placeholder="https://<cloud-host>/firmware/<package>.ipk?sha256=<hash>" />
            <clr-control-helper *dsDebug><app-ds-hint key="iot.update.request"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="apply()" [disabled]="busy || !url">
              {{ busy ? 'Applying…' : 'Apply' }}
            </button>
          </div>
        </div>
        <p class="hint">Runs <code>opkg install</code> on the device and restarts the affected daemons. The URL may carry <code>?sha256=</code> &amp; <code>?version=</code>.</p>

        <!-- Drag-and-drop local package (chunked upload) -->
        <h4>Or drop a package</h4>
        <div class="dropzone" [class.over]="dragOver" [class.busy]="uploading"
             (dragover)="onDragOver($event)" (dragleave)="onDragLeave($event)"
             (drop)="onDrop($event)" (click)="fileInput.click()">
          <input #fileInput type="file" accept=".ipk,.tar,.tar.gz,.tgz,.raucb" hidden
                 (change)="onPick($event)" />
          <clr-icon shape="upload-cloud" size="28"></clr-icon>
          <span *ngIf="!uploading">Drag &amp; drop a <code>.ipk</code> / <code>.tar.gz</code> / <code>.raucb</code>, or click to browse</span>
          <span *ngIf="uploading">Uploading {{ uploadName }} — {{ uploadPct }}%…</span>
        </div>
        <p class="hint">Uploaded to the device in chunks and installed there:
          <code>.ipk</code>/<code>.tar.gz</code> via <code>opkg</code>,
          <code>.raucb</code> via <code>rauc</code> (A/B image — requires the RAUC-enabled image).</p>
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
    .dropzone { display: flex; align-items: center; gap: 12px; padding: 18px;
      border: 2px dashed #b0bec5; border-radius: 8px; color: #607d8b;
      cursor: pointer; font-size: 13px; transition: all 0.15s; background: #fafcfd; }
    .dropzone:hover, .dropzone.over { border-color: #0072a3; color: #0072a3; background: #f0f8fc; }
    .dropzone.busy { opacity: 0.6; pointer-events: none; }
    .pbar { display: inline-block; width: 140px; height: 8px; background: #e0e6e9;
      border-radius: 4px; overflow: hidden; vertical-align: middle; }
    .pbar > span { display: block; height: 100%; background: #0072a3;
      transition: width 0.3s ease; border-radius: 4px; }
    .pbar.indet > span { width: 35%; animation: pbar-slide 1.1s ease-in-out infinite; }
    .pbar-pct { margin-left: 8px; font-size: 12px; color: #607d8b; vertical-align: middle; }
    @keyframes pbar-slide { 0% { margin-left: -35%; } 100% { margin-left: 100%; } }
  `]
})
export class SoftwareUpdateComponent implements OnInit, OnDestroy {
  version = '';       // iot.version — the running/installed release (footer value)
  state = 0;
  result = 0;
  pkg = '';           // iot.update.package — artifact being applied (name + version)
  progress = 0;       // iot.update.progress — download % (0 when size unknown)
  bank = '';          // iot.boot.bank — running A/B rootfs bank (empty = single-rootfs)
  confirmed = false;  // iot.boot.confirmed — this boot marked good
  // iot.boot.banks — both A/B banks (empty on a single-rootfs image).
  banks: Array<{ bootname: string; slot: string; version: string; state: string; booted: boolean }> = [];
  readonly bootAttempts = 3;          // system.conf boot-attempts (rollback threshold)
  showSwitchModal = false;
  switchTarget: { bootname: string; slot: string; version: string; state: string; booted: boolean } | null = null;
  switching = false;
  url = '';
  busy = false;
  dragOver = false;
  uploading = false;
  uploadName = '';
  uploadPct = 0;
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

  constructor(private http: HttpsvcService, private ds: DataStoreService,
              private session: SessionService, private toast: ToastService) {}

  ngOnInit(): void {
    if (!this.isAdmin) return;
    // Read OTA progress live off the single shared /status stream — no
    // per-page 5s self-poll. The long-poll wakes on iot.update.state changes,
    // so download/install progress renders promptly without missing updates.
    // Installed Version = the running release iot-httpd records at startup
    // (iot.version — the same value the sidebar footer shows). That is what is
    // actually installed/booted now. Do NOT read iot.update.version here: that
    // is only the last OTA *target* and is empty on a device that has never
    // been pushed, so it rendered "—" even though a real version is running.
    this.sub.add(this.http.dbGet(['iot.version']).subscribe(r => {
      if (r.ok && r.data)
        this.version = String((r.data as Record<string, unknown>)['iot.version'] || '');
    }));
    this.sub.add(this.ds.observe('iot.update.state').subscribe(v => {
      this.state = Number(v) || 0;
    }));
    this.sub.add(this.ds.observe('iot.update.result').subscribe(v => {
      this.result = Number(v) || 0;
    }));
    this.sub.add(this.ds.observe('iot.update.package').subscribe(v => {
      this.pkg = String(v ?? '');
    }));
    this.sub.add(this.ds.observe('iot.update.progress').subscribe(v => {
      this.progress = Math.max(0, Math.min(100, Number(v) || 0));
    }));
    // A/B boot status changes only once per boot — read it once. iot.boot.banks
    // is also written at boot (before the UI connects), so read it here too; the
    // observe below only delivers later changes (mark-good / post-switch).
    this.sub.add(this.http.dbGet(['iot.boot.bank', 'iot.boot.confirmed', 'iot.boot.banks']).subscribe(r => {
      if (r.ok && r.data) {
        const d = r.data as Record<string, unknown>;
        this.bank = d['iot.boot.bank'] != null ? String(d['iot.boot.bank']) : '';
        this.confirmed = d['iot.boot.confirmed'] === true;
        this.parseBanks(d['iot.boot.banks']);
      }
    }));
    // Live refresh when iot-ota-confirm marks the booted bank good, or a switch
    // re-publishes.
    this.sub.add(this.ds.observe('iot.boot.banks').subscribe(v => this.parseBanks(v)));
  }

  private parseBanks(v: unknown): void {
    if (v == null || v === '') { this.banks = []; return; }
    try {
      const arr = typeof v === 'string' ? JSON.parse(v) : v;
      this.banks = Array.isArray(arr) ? arr.map(b => ({
        bootname: String((b as Record<string, unknown>)['bootname'] ?? ''),
        slot:     String((b as Record<string, unknown>)['slot'] ?? ''),
        version:  String((b as Record<string, unknown>)['version'] ?? ''),
        state:    String((b as Record<string, unknown>)['state'] ?? ''),
        booted:   (b as Record<string, unknown>)['booted'] === true,
      })) : [];
    } catch { this.banks = []; }
  }

  askSwitch(b: { bootname: string; slot: string; version: string; state: string; booted: boolean }): void {
    this.switchTarget = b;
    this.showSwitchModal = true;
  }

  confirmSwitch(): void {
    if (!this.switchTarget) return;
    this.switching = true;
    // Send the target bank's bootname; iot-bank-switch (device) marks it active
    // and reboots. The key is cleared device-side after handling.
    this.http.dbSet([{ key: 'iot.boot.switch.request', value: this.switchTarget.bootname }]).subscribe({
      next: (r) => {
        this.switching = false;
        this.showSwitchModal = false;
        if (r.ok) this.toast.success('Bank switch requested — the device is rebooting…');
        else this.toast.error(r.err || 'Bank switch failed');
      },
      error: () => { this.switching = false; this.toast.error('Bank switch failed'); }
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

  // ── Drag-and-drop upload ──────────────────────────────────────────
  onDragOver(e: DragEvent): void { e.preventDefault(); this.dragOver = true; }
  onDragLeave(e: DragEvent): void { e.preventDefault(); this.dragOver = false; }
  onDrop(e: DragEvent): void {
    e.preventDefault();
    this.dragOver = false;
    const f = e.dataTransfer?.files?.[0];
    if (f) this.uploadFile(f);
  }
  onPick(e: Event): void {
    const input = e.target as HTMLInputElement;
    const f = input.files?.[0];
    if (f) this.uploadFile(f);
    input.value = '';   // allow re-picking the same file
  }

  // ≤8 MiB chunks (under the server body cap); posted sequentially so an
  // arbitrarily large .raucb bundle uploads without buffering it server-side.
  private static readonly CHUNK = 4 * 1024 * 1024;

  private uploadFile(file: File): void {
    if (this.uploading) return;
    // .tar covers a .tar.gz that the browser/OS auto-decompressed on download
    // (macOS Safari/Archive Utility strips the .gz); the device detects gzip by
    // content, so an uncompressed tar installs the same way.
    if (!/\.(ipk|tar\.gz|tgz|tar|raucb)$/i.test(file.name)) {
      this.toast.error('Pick a .ipk, .tar, .tar.gz, .tgz or .raucb file'); return;
    }
    this.uploading = true; this.uploadName = file.name; this.uploadPct = 0;
    const total = file.size;
    const sendFrom = (offset: number): void => {
      const end = Math.min(offset + SoftwareUpdateComponent.CHUNK, total);
      const final = end >= total;
      this.sub.add(this.http.uploadUpdateChunk(file.name, file.slice(offset, end), offset, final).subscribe({
        next: (r) => {
          if (!r.ok) { this.uploading = false; this.toast.error(r.err || 'Upload failed'); return; }
          this.uploadPct = total ? Math.round((end / total) * 100) : 100;
          if (final) { this.uploading = false; this.toast.success('Uploaded — installing ' + file.name + '…'); }
          else sendFrom(end);
        },
        error: (e) => { this.uploading = false; this.toast.error((e?.error?.err) || 'Upload failed'); }
      }));
    };
    sendFrom(0);
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
